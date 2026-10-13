#!/usr/bin/env python3
# Copyright (C) 2026 Rui Nelson
# SPDX-License-Identifier: GPL-2.0-or-later
"""Black-box regressions for copies, transport errors, cancellation and PJL."""
import sys
import http.client
import os
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path

APP = str(Path(sys.argv[1]).resolve())
ROOT = Path(__file__).resolve().parents[1]
REPLY = (b'@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY="READY"\r\nONLINE=TRUE\r\n\x0c'
         b'@PJL INFO PAGECOUNT\r\nPAGECOUNT=100\r\n\x0c'
         b'@PJL INFO DRUMLIFE\r\nDRUMLIFE=11400\r\n\x0c')

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def attrs(data):
    out = {}
    pos = 8
    name = ''
    while pos < len(data):
        tag = data[pos]
        pos += 1
        if tag == 3:
            break
        if tag < 16:
            name = ''
            continue
        size = struct.unpack_from('>H', data, pos)[0]
        pos += 2
        if size:
            name = data[pos:pos+size].decode(errors='replace')
        pos += size
        size = struct.unpack_from('>H', data, pos)[0]
        pos += 2
        raw = data[pos:pos+size]
        pos += size
        val = int.from_bytes(raw, 'big', signed=True) if tag in (0x21, 0x23) else raw.decode(errors='replace')
        out.setdefault(name, []).append(val)
    return out

def field(tag, name, value):
    n = name.encode()
    v = struct.pack('>i', value) if isinstance(value, int) else value.encode()
    return bytes([tag]) + struct.pack('>H',len(n)) + n + struct.pack('>H',len(v)) + v

class Audit:
    def __init__(self, mode):
        self.mode = mode
        self.work = Path(tempfile.mkdtemp(prefix='sister-audit-'+mode+'-'))
        self.ipp = port()
        self.sink = port()
        self.captured = []
        self.first_print = threading.Event()
        self.stop = threading.Event()
        threading.Thread(target=self.listen, daemon=True).start()
        env = os.environ.copy()
        env['XDG_CONFIG_HOME'] = str(self.work)
        self.stderr = (self.work/'stderr.log').open('w')
        self.proc = subprocess.Popen([APP,'server','-o',f'server-port={self.ipp}',
            '-o',f'spool-directory={self.work}/spool','-o',f'log-file={self.work}/server.log',
            '-o','log-level=debug','-o','listen-hostname=127.0.0.1'], env=env,
            stdout=self.stderr,stderr=self.stderr)
        deadline = time.monotonic()+10
        while time.monotonic()<deadline:
            try:
                with socket.create_connection(('127.0.0.1',self.ipp),.1):
                    break
            except OSError:
                time.sleep(.05)
        else:
            self.close()
            raise RuntimeError('PAPPL server did not listen')
        try:
            self.cli('add','-d','audit','-m','sister-hl2030','-v',f'socket://127.0.0.1:{self.sink}')
        except Exception:
            self.close()
            raise

    def cli(self,*args):
        p = subprocess.run([APP,args[0],'-u',f'ipp://127.0.0.1:{self.ipp}/',*args[1:]],
                           capture_output=True,text=True,timeout=20)
        if p.returncode:
            raise RuntimeError(p.stdout+p.stderr)
        return p.stdout

    def listen(self):
        with socket.socket() as s:
            s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            s.bind(('127.0.0.1',self.sink))
            s.listen(10)
            s.settimeout(.2)
            while not self.stop.is_set():
                try:
                    c,_ = s.accept()
                except socket.timeout:
                    continue
                threading.Thread(target=self.receive,args=(c,),daemon=True).start()

    def receive(self,c):
        buf = bytearray()
        answered = False
        c.settimeout(15)
        try:
            while True:
                data = c.recv(65536)
                if not data:
                    break
                buf.extend(data)
                if b'\x1b*b1030m' in buf:
                    self.first_print.set()
                    if self.mode=='fail':
                        c.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
                        break
                if b'INFO DRUMLIFE' in buf and not answered:
                    answered = True
                    if self.mode=='split':
                        first = REPLY.split(b'DRUMLIFE=')[0]+b'DRUMLIFE=11'
                        c.sendall(first)
                        time.sleep(.2)
                        c.sendall(b'400\r\n\x0c')
                    else:
                        c.sendall(REPLY)
        except OSError:
            pass
        finally:
            c.close()
            if b'\x1b*b1030m' in buf:
                self.captured.append(bytes(buf))
                (self.work/f'capture-{len(self.captured)}.prn').write_bytes(buf)

    def request(self,op,extra=b'',job_id=None):
        data = b'\x01\x01'+struct.pack('>HI',op,1)+b'\x01'
        data += field(0x47,'attributes-charset','utf-8')+field(0x48,'attributes-natural-language','en')
        data += field(0x45,'printer-uri',f'ipp://127.0.0.1:{self.ipp}/ipp/print')
        if job_id is not None:
            data += field(0x21,'job-id',job_id)
        data += field(0x42,'requesting-user-name',os.environ.get('USER','test'))
        data += extra+b'\x03'
        conn = http.client.HTTPConnection('127.0.0.1',self.ipp,timeout=10)
        conn.request('POST','/ipp/print',body=data,headers={'Content-Type':'application/ipp'})
        response = conn.getresponse()
        raw = response.read()
        conn.close()
        return attrs(raw)

    def wait_job(self,job_id):
        deadline = time.monotonic()+40
        while time.monotonic()<deadline:
            result = self.request(9,job_id=job_id)
            if result.get('job-state',[0])[0] in (7,8,9):
                time.sleep(.2)
                return result
            time.sleep(.1)
        raise RuntimeError('job did not finish')

    def submit(self,**kwargs):
        options = ['-d','audit','-n',str(kwargs.get('copies',1))]
        if kwargs.get('small',True):
            options += ['-o','media=iso_a6_105x148mm','-o','print-quality=3']
        else:
            options += ['-o','print-scaling=fit','-o','print-quality=4']
        output = self.cli('submit',*options,str(kwargs.get('file', ROOT/'test_fixtures/flag.png')))
        if output.strip().isdigit():
            return int(output.strip())
        match = re.search(r'(?:Job|job)\s+(\d+)',output)
        if not match:
            raise RuntimeError('unexpected submit output: '+output)
        return int(match[1])

    def close(self):
        self.stop.set()
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.stderr.close()

def run(mode):
    a = Audit(mode)
    try:
        if mode == 'split':
            result = a.request(11, field(0x44, 'requested-attributes', 'marker-levels'))
            assert result.get('marker-levels') == [100, 5], result
        elif mode == 'copies':
            for copies in (2, 3):
                a.captured.clear()
                job = a.submit(copies=copies)
                result = a.wait_job(job)
                capture = b''.join(a.captured)
                assert result['job-state'] == [9], result
                assert capture.count(b'1030M\x0c') == copies
                assert re.findall(rb'\x1b&l(\d+)X', capture) == [b'1']
            raster = a.work / 'test.pwg'
            subprocess.run([sys.argv[2], str(raster)], check=True)
            a.captured.clear()
            job = a.submit(copies=2, file=raster)
            result = a.wait_job(job)
            capture = b''.join(a.captured)
            assert result['job-state'] == [9], result
            assert capture.count(b'1030M\x0c') == 1
            assert re.findall(rb'\x1b&l(\d+)X', capture) == [b'2']
        else:
            job = a.submit(small=False)
            if mode == 'cancel':
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if 'Raster page' in (a.work / 'server.log').read_text():
                        break
                    time.sleep(.01)
                else:
                    raise AssertionError('page never started')
                assert not a.first_print.is_set(), 'page sent before cancellation test'
                a.cli('cancel', '-d', 'audit', '-j', str(job))
            result = a.wait_job(job)
            capture = b''.join(a.captured)
            if mode == 'cancel':
                assert result['job-state'] == [7], result
                assert not capture, 'cancelled buffered page was sent'
            else:
                assert a.first_print.is_set(), 'failure was not injected'
                assert result['job-state'] == [8], result
                assert result['job-impressions-completed'] == [0], result
        print('ok', mode, flush=True)
    except Exception:
        print('Diagnostic files:', a.work, file=sys.stderr)
        raise
    finally:
        a.close()


def check_sink():
    work = Path(tempfile.mkdtemp(prefix='sister-sink-test-'))
    sink_port = port()
    output = work / 'job.prn'
    sink = subprocess.Popen([sys.executable, str(ROOT / 'Scripts/_socket_sink.py'),
                             str(sink_port), str(output)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                conn = socket.create_connection(('127.0.0.1', sink_port), .2)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.05)
        with conn:
            conn.sendall(b'@PJL INFO STATUS\r\n@PJL INFO DRUMLIFE\r\n')
            conn.settimeout(2)
            assert b'DRUMLIFE=' in conn.recv(4096)
        assert not output.exists(), 'status was written as a print job'
        payload = b'\x1b*b1030m3w\x00\x01\xff1030M\x0c\x1b%-12345X'
        with socket.create_connection(('127.0.0.1', sink_port), 2) as conn:
            conn.sendall(payload)
        stdout, stderr = sink.communicate(timeout=5)
        assert sink.returncode == 0, stderr
        assert output.read_bytes() == payload, stdout
        print('ok preview sink', flush=True)
    finally:
        if sink.poll() is None:
            sink.kill()
            sink.wait()


if __name__ == '__main__':
    for mode in ('copies', 'fail', 'cancel', 'split'):
        run(mode)
    check_sink()
