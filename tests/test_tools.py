#!/usr/bin/env python3
# Copyright (C) 2026 Rui Nelson
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check real CLI padding output and the system-Ruby PPD transformation."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAW = sys.argv[1]
with tempfile.TemporaryDirectory(prefix='sister-tools-') as directory:
    work = Path(directory)
    for width in (1, 7, 9, 4661):
        stride = (width + 7) // 8
        blank = bytes(stride)
        padding = blank[:-1] + bytes([(1 << (8 - width % 8)) - 1])
        header = f'P4\n{width} 1\n'.encode()
        a = subprocess.check_output([RAW], input=header + blank)
        b = subprocess.check_output([RAW], input=header + padding)
        assert a == b, f'padding changed the printed bitmap for width {width}'
    print('ok PBM padding')
    if sys.platform == 'darwin':
        script = (ROOT / 'Scripts/_privileged-create-queue.sh').read_text()
        ruby = script.split("<<'RUBY'\n", 1)[1].split('\nRUBY', 1)[0]
        ppd = work / 'test.ppd'
        original = '''*PPD-Adobe: "4.3"
*APSupplies: "obsolete"
*DefaultResolution: 300dpi
*OpenUI *cupsPrintQuality/Qualidade: PickOne
*cupsPrintQuality Draft/Rascunho: "old"
*cupsPrintQuality Normal/Normal: "old"
*cupsPrintQuality High/Fina: "old"
*DefaultInputSlot: manual
*InputSlot main/Bandeja: ""
*InputSlot manual/Manual: ""
*DefaultMediaType: auto
*MediaType stationery/Papel: ""
*ModelName: "Impressora á"
'''
        ppd.write_bytes(original.encode('latin1'))
        subprocess.run(['/usr/bin/ruby', '-', str(ppd)], input=ruby, text=True, check=True)
        result = ppd.read_bytes().decode('latin1')
        for line in ('*cupsIPPSupplies: True', '*cupsSNMPSupplies: False',
                     '*DefaultResolution: 600dpi', '*DefaultInputSlot: main',
                     '*DefaultMediaType: stationery', '*ModelName: "Impressora á"',
                     '*InputSlot main/Bandeja: "<</InputSlot(main)>>setpagedevice"',
                     '*cupsPrintQuality Normal/Normal: "<</cupsInteger1 4 /HWResolution[600 600]>>setpagedevice"'):
            assert line in result, line
        assert '*APSupplies:' not in result
        assert '\n*APPrinterPreset GrayDeep_with_Paper_Auto-Detect/' in result
        subprocess.run(['/usr/bin/ruby', '-', str(ppd)], input=ruby, text=True, check=True)
        assert ppd.read_bytes().decode('latin1') == result
        print('ok system Ruby PPD, Latin-1 and idempotence')
