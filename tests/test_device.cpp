// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
#include <pappl/pappl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
bool fail = false;
size_t bytes_written = 0;
bool open_device(pappl_device_t*, const char*, const char*) { return true; }
void close_device(pappl_device_t*) {}
void error(const char*, void*) {}
ssize_t write(pappl_device_t*, const void*, size_t size) {
  if (fail) return -1;
  // Also exercise the short-write retry in the project patch.
  const size_t count = std::min(size, size_t{3});
  bytes_written += count;
  return static_cast<ssize_t>(count);
}
}

int main() {
  papplDeviceAddScheme("sister-test", PAPPL_DEVTYPE_CUSTOM_LOCAL, nullptr,
                       open_device, close_device, nullptr, write, nullptr, nullptr);
  pappl_device_t* device = papplDeviceOpen("sister-test://local", "test", error, nullptr);
  if (!device) return 1;
  const char data[] = "a small buffered write";
  bool ok = papplDeviceWrite(device, data, sizeof(data)) == sizeof(data);
  ok = ok && bytes_written == 0;
  ok = papplDeviceFlush(device) && ok;
  ok = ok && bytes_written == sizeof(data);
  fail = true;
  ok = (papplDeviceWrite(device, data, sizeof(data)) == sizeof(data)) && ok;
  ok = !papplDeviceFlush(device) && ok;
  papplDeviceClose(device);
  if (!ok) {
    std::fputs("FAIL: final flush must report a backend failure\n", stderr);
    return 1;
  }
  std::puts("ok final flush and short writes");
  return 0;
}
