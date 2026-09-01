// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Query HL-2030 toner/drum over USB PJL and publish IPP printer-supply.

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "status/pjl.h"
#include "status/usb_printer.h"
#include "version.h"

namespace {

// Leftover from the retired ippeveprinter façade: serialise USB access
// against the CUPS usb backend. Harmless if the file is missing — UsbLock
// then falls back to /tmp.
constexpr const char* kLockPath = "/Library/Printers/SisterHL2030/usb.lock";
// Optional usb:// URI whose serial= query selects the device. Empty file
// means the first HL-2030 on the bus.
constexpr const char* kUriPath = "/Library/Printers/SisterHL2030/device-uri";
// Dummy document for the --publish IPP job (the job name is .sister-status,
// so the app prints nothing; the file only has to exist and be readable).
constexpr const char* kDummyDoc = "/Library/Printers/SisterHL2030/icon.png";

// Whole file, trailing whitespace stripped. Empty on open failure.
std::string read_file(const char* path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ||
                        s.back() == '\t')) {
    s.pop_back();
  }
  return s;
}

// Advisory flock on kLockPath (or /tmp fallback). `skip` is set when the
// caller already holds the lock (SISTER_USB_LOCKED).
class UsbLock {
 public:
  UsbLock(bool wait, bool skip) {
    if (skip) {
      return;
    }
    fd_ = open(kLockPath, O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
      fd_ = open("/tmp/sisterhl2030-usb.lock", O_RDWR | O_CREAT, 0644);
    }
    if (fd_ < 0) {
      return;
    }
    if (flock(fd_, wait ? LOCK_EX : LOCK_EX | LOCK_NB) != 0) {
      close(fd_);
      fd_ = -1;
      busy_ = true;
    }
  }
  ~UsbLock() {
    if (fd_ >= 0) {
      flock(fd_, LOCK_UN);
      close(fd_);
    }
  }
  bool ok() const { return fd_ >= 0; }
  bool busy() const { return busy_; }

 private:
  int fd_ = -1;
  bool busy_ = false;
};

// Submit a no-op Print-Job named .sister-status so CUPS copies marker-levels.
bool publish_via_ipp_job(std::string* error) {
  char ipp_path[] = "/tmp/sister-status-jobXXXXXX";
  const int fd = mkstemp(ipp_path);
  if (fd < 0) {
    *error = "mkstemp";
    return false;
  }
  const char* doc = kDummyDoc;
  if (access(doc, R_OK) != 0) {
    close(fd);
    unlink(ipp_path);
    *error = "missing icon.png for dummy IPP job";
    return false;
  }
  char body[1024];
  const int n = std::snprintf(body, sizeof(body),
                              "{\n"
                              "  VERSION 2.0\n"
                              "  OPERATION Print-Job\n"
                              "  GROUP operation-attributes-tag\n"
                              "  ATTR charset attributes-charset utf-8\n"
                              "  ATTR naturalLanguage attributes-natural-language en\n"
                              "  ATTR uri printer-uri $uri\n"
                              "  ATTR name requesting-user-name sister-status\n"
                              "  ATTR mimeMediaType document-format image/urf\n"
                              "  ATTR name job-name .sister-status\n"
                              "  FILE %s\n"
                              "}\n",
                              doc);
  if (write(fd, body, static_cast<size_t>(n)) != n) {
    close(fd);
    unlink(ipp_path);
    *error = "write ipp test";
    return false;
  }
  close(fd);
  const pid_t pid = fork();
  if (pid == 0) {
    const int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
      dup2(nullfd, STDOUT_FILENO);
      dup2(nullfd, STDERR_FILENO);
      close(nullfd);
    }
    execl("/usr/bin/ipptool", "ipptool", "-t", "ipp://127.0.0.1:8631/ipp/print",
          ipp_path, static_cast<char*>(nullptr));
    _exit(127);
  }
  int st = 1;
  if (pid > 0) {
    waitpid(pid, &st, 0);
  }
  unlink(ipp_path);
  if (pid < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    *error = "ipptool Print-Job failed";
    return false;
  }
  return true;
}

// Escape `s` for use inside a JSON string. DISPLAY is copied verbatim off
// the wire, so a quote or a backslash in the printer's panel text -- or a
// stray control byte in a short read -- would otherwise emit a line no JSON
// parser accepts. Bytes at 0x20 and above pass through unchanged, which keeps
// a UTF-8 panel string UTF-8.
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char esc[8];
          std::snprintf(esc, sizeof(esc), "\\u%04x", c);
          out += esc;
        } else {
          out += ch;
        }
        break;
    }
  }
  return out;
}

// Plain-text toner/drum/page-count to stdout.
void print_human(const sisterhl2030::PrinterStatus& st) {
  std::printf("PJL status: CODE=%d DISPLAY=\"%s\" ONLINE=%s\n", st.code,
              st.display.c_str(), st.online ? "TRUE" : "FALSE");
  if (st.have_pagecount) {
    std::printf("Pages (device counter): %d\n", st.pagecount);
  }
  std::printf("Black toner (TN-2000): %s",
              sisterhl2030::toner_state_label(st.toner));
  if (st.toner_percent >= 0) {
    std::printf(" (%d%%)", st.toner_percent);
  }
  std::printf("\n");
  std::printf("Drum (DR-2000): ");
  if (st.drum_percent >= 0) {
    std::printf("%d%%", st.drum_percent);
    if (st.have_drumlife) {
      std::printf("  (%d / %d pages)", st.drumlife, sisterhl2030::kDrumRatedPages);
    }
  } else {
    std::printf("unknown");
  }
  std::printf("\n");
}

// --ipp is the retired façade's ATTR:/STATE: format; --publish is the
// CUPS marker-levels refresh the installer still uses.
void usage() {
  std::fprintf(stderr,
               "usage: sister-status [--json|--ipp|--publish [--loop]]\n"
               "  (no flags)   plain text\n"
               "  --json       one JSON line\n"
               "  --ipp        ATTR:/STATE: on stderr for ippeveprinter\n"
               "  --publish    no-op IPP job that refreshes printer-supply\n"
               "  --loop       with --publish, repeat every 180s\n");
}

}  // namespace

int main(int argc, char** argv) {
  enum class Mode { human, json, ipp, publish } mode = Mode::human;
  bool loop = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--json") {
      mode = Mode::json;
    } else if (a == "--ipp") {
      mode = Mode::ipp;
    } else if (a == "--publish") {
      mode = Mode::publish;
    } else if (a == "--loop") {
      loop = true;
    } else if (a == "--version" || a == "version") {
      std::puts(SISTER_VERSION_FULL);
      return 0;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      usage();
      return 2;
    }
  }

  if (mode == Mode::publish) {
    do {
      std::string err;
      if (!publish_via_ipp_job(&err)) {
        std::fprintf(stderr, "sister-status: %s\n", err.c_str());
      }
      if (!loop) {
        break;
      }
      sleep(180);
    } while (true);
    return 0;
  }

  const bool already_locked = std::getenv("SISTER_USB_LOCKED") != nullptr;
  UsbLock lock(true, already_locked);
  if (lock.busy()) {
    std::fprintf(stderr, "ERROR: HL-2030 USB is busy\n");
    return 1;
  }

  const std::string serial = sisterhl2030::serial_from_device_uri(read_file(kUriPath));
  std::string raw;
  std::string err;
  if (!sisterhl2030::pjl_query_supplies(serial, &raw, &err)) {
    // mode == Mode::publish already returned above; only human/json/ipp reach here.
    std::fprintf(stderr, "ERROR: %s\n", err.c_str());
    return 1;
  }

  const sisterhl2030::PrinterStatus st = sisterhl2030::parse_pjl_status(raw);
  if (!st.have_status && !st.have_drumlife && !st.have_pagecount) {
    // mode == Mode::publish already returned above; only human/json/ipp reach here.
    std::fprintf(stderr, "ERROR: empty PJL response\n");
    return 1;
  }

  switch (mode) {
    case Mode::human:
      print_human(st);
      return 0;
    case Mode::json:
      std::printf(
          "{\"code\":%d,\"display\":\"%s\",\"pagecount\":%d,\"drumlife\":%d,"
          "\"toner\":\"%s\",\"toner_percent\":%d,\"drum_percent\":%d}\n",
          st.code, json_escape(st.display).c_str(), st.pagecount, st.drumlife,
          sisterhl2030::toner_state_label(st.toner), st.toner_percent,
          st.drum_percent);
      return 0;
    case Mode::ipp:
      std::fputs(sisterhl2030::ippeve_attr_lines(st).c_str(), stdout);
      return 0;
    case Mode::publish:
      return 0;
  }
  return 0;
}
