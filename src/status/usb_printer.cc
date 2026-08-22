// Copyright (C) 2026 Rui Nelson
// SPDX-License-Identifier: GPL-2.0-or-later
//
// IOKit USB printer-class bulk I/O for the HL-2030 (VID 04f9 / PID 0027).

#include "status/usb_printer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>

namespace sisterhl2030 {
namespace {

using DeviceIntf = IOUSBDeviceInterface650**;
using IfaceIntf = IOUSBInterfaceInterface650**;

std::string cf_string(CFTypeRef ref) {
  if (!ref || CFGetTypeID(ref) != CFStringGetTypeID()) {
    return {};
  }
  auto* s = static_cast<CFStringRef>(ref);
  const char* c = CFStringGetCStringPtr(s, kCFStringEncodingUTF8);
  if (c) {
    return c;
  }
  char buf[256];
  if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) {
    return buf;
  }
  return {};
}

std::string registry_str(io_service_t service, CFStringRef key) {
  CFTypeRef ref =
      IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
  std::string out = cf_string(ref);
  if (ref) {
    CFRelease(ref);
  }
  return out;
}

uint16_t registry_u16(io_service_t service, CFStringRef key) {
  CFTypeRef ref =
      IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
  uint16_t v = 0;
  if (ref) {
    if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
      int n = 0;
      CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberIntType, &n);
      v = static_cast<uint16_t>(n);
    }
    CFRelease(ref);
  }
  return v;
}

struct Opened {
  io_service_t service = 0;
  DeviceIntf dev = nullptr;
  IfaceIntf iface = nullptr;
  UInt8 out_pipe = 0;
  UInt8 in_pipe = 0;

  void close() {
    if (iface) {
      (*iface)->USBInterfaceClose(iface);
      (*iface)->Release(iface);
      iface = nullptr;
    }
    if (dev) {
      (*dev)->USBDeviceClose(dev);
      (*dev)->Release(dev);
      dev = nullptr;
    }
    if (service) {
      IOObjectRelease(service);
      service = 0;
    }
  }
};

bool open_interface(Opened* o, std::string* error) {
  IOUSBFindInterfaceRequest req{};
  req.bInterfaceClass = kUSBPrintingClass;
  req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
  req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
  req.bAlternateSetting = kIOUSBFindInterfaceDontCare;

  io_iterator_t it = 0;
  IOReturn kr = (*o->dev)->CreateInterfaceIterator(o->dev, &req, &it);
  if (kr != kIOReturnSuccess) {
    *error = "CreateInterfaceIterator failed";
    return false;
  }
  io_service_t iface_svc = IOIteratorNext(it);
  IOObjectRelease(it);
  if (!iface_svc) {
    *error = "no USB printer-class interface";
    return false;
  }

  IOCFPlugInInterface** plugin = nullptr;
  SInt32 score = 0;
  kr = IOCreatePlugInInterfaceForService(iface_svc, kIOUSBInterfaceUserClientTypeID,
                                         kIOCFPlugInInterfaceID, &plugin, &score);
  IOObjectRelease(iface_svc);
  if (kr != kIOReturnSuccess || !plugin) {
    *error = "interface plug-in failed";
    return false;
  }
  kr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650),
                                 reinterpret_cast<LPVOID*>(&o->iface));
  (*plugin)->Release(plugin);
  if (kr != kIOReturnSuccess || !o->iface) {
    *error = "QueryInterface (interface) failed";
    o->iface = nullptr;
    return false;
  }

  kr = (*o->iface)->USBInterfaceOpenSeize(o->iface);
  if (kr != kIOReturnSuccess) {
    kr = (*o->iface)->USBInterfaceOpen(o->iface);
  }
  if (kr != kIOReturnSuccess) {
    *error = "USBInterfaceOpen failed (printer busy?)";
    (*o->iface)->Release(o->iface);
    o->iface = nullptr;
    return false;
  }

  UInt8 n_pipes = 0;
  (*o->iface)->GetNumEndpoints(o->iface, &n_pipes);
  for (UInt8 i = 1; i <= n_pipes; ++i) {
    UInt8 direction = 0, number = 0, transfer = 0, interval = 0;
    UInt16 max_packet = 0;
    kr = (*o->iface)->GetPipeProperties(o->iface, i, &direction, &number, &transfer,
                                        &max_packet, &interval);
    if (kr != kIOReturnSuccess || transfer != kUSBBulk) {
      continue;
    }
    if (direction == kUSBOut && o->out_pipe == 0) {
      o->out_pipe = i;
    } else if (direction == kUSBIn && o->in_pipe == 0) {
      o->in_pipe = i;
    }
  }
  if (o->out_pipe == 0 || o->in_pipe == 0) {
    *error = "missing bulk IN/OUT pipes";
    return false;
  }
  return true;
}

bool open_device(io_service_t service, Opened* o, std::string* error) {
  o->service = service;
  IOObjectRetain(service);

  IOCFPlugInInterface** plugin = nullptr;
  SInt32 score = 0;
  IOReturn kr = IOCreatePlugInInterfaceForService(
      service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
      &score);
  if (kr != kIOReturnSuccess || !plugin) {
    *error = "device plug-in failed";
    return false;
  }
  kr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650),
                                 reinterpret_cast<LPVOID*>(&o->dev));
  (*plugin)->Release(plugin);
  if (kr != kIOReturnSuccess || !o->dev) {
    *error = "QueryInterface (device) failed";
    o->dev = nullptr;
    return false;
  }

  kr = (*o->dev)->USBDeviceOpenSeize(o->dev);
  if (kr != kIOReturnSuccess) {
    kr = (*o->dev)->USBDeviceOpen(o->dev);
  }
  if (kr != kIOReturnSuccess) {
    *error = "USBDeviceOpen failed";
    return false;
  }

  UInt8 cfg_n = 0;
  if ((*o->dev)->GetNumberOfConfigurations(o->dev, &cfg_n) == kIOReturnSuccess &&
      cfg_n > 0) {
    IOUSBConfigurationDescriptorPtr desc = nullptr;
    if ((*o->dev)->GetConfigurationDescriptorPtr(o->dev, 0, &desc) == kIOReturnSuccess &&
        desc) {
      UInt8 cur = 0;
      (*o->dev)->GetConfiguration(o->dev, &cur);
      if (cur != desc->bConfigurationValue) {
        (*o->dev)->SetConfiguration(o->dev, desc->bConfigurationValue);
      }
    }
  }
  return open_interface(o, error);
}

io_service_t find_hl2030(const std::string& want_serial, std::string* error) {
  CFMutableDictionaryRef matching = IOServiceMatching("IOUSBHostDevice");
  if (!matching) {
    matching = IOServiceMatching(kIOUSBDeviceClassName);
  }
  if (!matching) {
    *error = "IOServiceMatching failed";
    return 0;
  }
  io_iterator_t it = 0;
  const kern_return_t kr =
      IOServiceGetMatchingServices(kIOMainPortDefault, matching, &it);
  if (kr != KERN_SUCCESS) {
    *error = "IOServiceGetMatchingServices failed";
    return 0;
  }

  io_service_t found = 0;
  io_service_t svc = 0;
  while ((svc = IOIteratorNext(it))) {
    const uint16_t vid = registry_u16(svc, CFSTR("idVendor"));
    const uint16_t pid = registry_u16(svc, CFSTR("idProduct"));
    if (vid != kBrotherVid || pid != kHl2030Pid) {
      IOObjectRelease(svc);
      continue;
    }
    const std::string serial = registry_str(svc, CFSTR("USB Serial Number"));
    if (!want_serial.empty() && serial != want_serial) {
      IOObjectRelease(svc);
      continue;
    }
    found = svc;
    break;
  }
  IOObjectRelease(it);
  if (!found) {
    *error = want_serial.empty() ? "HL-2030 USB not found"
                                 : "HL-2030 serial " + want_serial + " not found";
  }
  return found;
}

bool bulk_write(Opened* o, const uint8_t* data, uint32_t n, std::string* error) {
  const IOReturn kr =
      (*o->iface)->WritePipeTO(o->iface, o->out_pipe, const_cast<uint8_t*>(data), n,
                               2000, 2000);
  if (kr != kIOReturnSuccess) {
    *error = "USB bulk OUT failed";
    return false;
  }
  return true;
}

void drain_in(Opened* o) {
  for (int i = 0; i < 40; ++i) {
    uint8_t buf[256];
    UInt32 n = sizeof(buf);
    const IOReturn kr =
        (*o->iface)->ReadPipeTO(o->iface, o->in_pipe, buf, &n, 20, 20);
    if (kr == kIOReturnSuccess && n > 0) {
      continue;
    }
    if (i < 8) {
      usleep(50000);
      continue;
    }
    break;
  }
}

bool response_complete(const std::string& s) {
  return s.find("CODE=") != std::string::npos &&
         s.find("DRUMLIFE=") != std::string::npos;
}

}  // namespace

bool write_pjl(Opened* o, const std::string& command, std::string* error) {
  std::string payload = "\x1b%-12345X@PJL\r\n@PJL ";
  payload += command;
  payload += "\r\n\x1b%-12345X";
  return bulk_write(o, reinterpret_cast<const uint8_t*>(payload.data()),
                    static_cast<uint32_t>(payload.size()), error);
}

// Append whatever the printer sends for ~wait_ms. HL-2030 PJL readback is
// lagged: the reply to command N often arrives after command N+1 is sent.
void collect_in(Opened* o, std::string* acc, int wait_ms) {
  const int slices = std::max(1, wait_ms / 50);
  for (int i = 0; i < slices; ++i) {
    uint8_t buf[256];
    UInt32 n = sizeof(buf);
    const IOReturn kr =
        (*o->iface)->ReadPipeTO(o->iface, o->in_pipe, buf, &n, 20, 20);
    if (kr == kIOReturnSuccess && n > 0) {
      acc->append(reinterpret_cast<char*>(buf), n);
    }
    usleep(50000);
  }
}

bool pjl_query(const std::string& want_serial, const std::string& commands_crlf,
               std::string* response, std::string* error) {
  response->clear();
  error->clear();
  io_service_t svc = find_hl2030(want_serial, error);
  if (!svc) {
    return false;
  }
  Opened o;
  bool ok = open_device(svc, &o, error);
  IOObjectRelease(svc);
  if (!ok) {
    o.close();
    return false;
  }

  // One INFO (or other) command. A trailing ECHO is added by supplies query.
  if (!write_pjl(&o, commands_crlf, error)) {
    o.close();
    return false;
  }
  collect_in(&o, response, 800);
  o.close();
  if (response->empty()) {
    *error = "no PJL response";
    return false;
  }
  return true;
}

bool pjl_query_supplies(const std::string& want_serial, std::string* response,
                        std::string* error) {
  response->clear();
  error->clear();
  io_service_t svc = find_hl2030(want_serial, error);
  if (!svc) {
    return false;
  }
  Opened o;
  bool ok = open_device(svc, &o, error);
  IOObjectRelease(svc);
  if (!ok) {
    o.close();
    return false;
  }

  drain_in(&o);
  const char* cmds[] = {"INFO STATUS", "INFO PAGECOUNT", "INFO DRUMLIFE",
                        "ECHO SisterHL2030"};
  for (const char* cmd : cmds) {
    if (!write_pjl(&o, cmd, error)) {
      o.close();
      return false;
    }
    collect_in(&o, response, 700);
    if (response_complete(*response)) {
      collect_in(&o, response, 200);
      o.close();
      return true;
    }
  }
  o.close();
  if (response->empty()) {
    *error = "no PJL response";
    return false;
  }
  return true;
}

}  // namespace sisterhl2030
