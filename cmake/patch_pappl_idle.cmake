# Shrink PAPPL's idle footprint for a LaunchDaemon that never shows a UI.
#
# Applied to the extracted PAPPL ${PAPPL_VERSION} tree:
#   - English-only catalogs (the other eight languages are ~450 KB of
#     __cstring plus thousands of strdup pairs at startup)
#   - Skip cupsSetServerCredentials when PAPPL_SOPTIONS_NO_TLS is set
#     (otherwise PAPPL mints a self-signed cert on every boot)
#   - After configure: dummy status UI so AppKit is not linked,
#     arm64-only so the static lib is not a fat x86_64+arm64 archive,
#     -Os without -g / -fPIC so the .a is size-optimised, not a debug PIC build
#   - Compile-out raw sockets, USB gadget, and TLS/network/security/log web
#     pages so ld does not pull printer-raw/usb/httpmon or those HTML handlers
#
# Inputs: PAPPL_SRC (required), PAPPL_MAKEDEFS (optional, post-configure)

if(NOT PAPPL_SRC)
  message(FATAL_ERROR "PAPPL_SRC is not set")
endif()

function(sister_replace file old new)
  file(READ "${file}" _c)
  string(FIND "${_c}" "${old}" _pos)
  if(_pos EQUAL -1)
    message(FATAL_ERROR "patch_pappl_idle: pattern not found in ${file}")
  endif()
  string(REPLACE "${old}" "${new}" _c "${_c}")
  file(WRITE "${file}" "${_c}")
endfunction()

if(PAPPL_MAKEDEFS)
  sister_replace("${PAPPL_MAKEDEFS}"
    "system-status-macos.o"
    "system-status-dummy.o")
  sister_replace("${PAPPL_MAKEDEFS}"
    "-framework AppKit "
    "")
  # PAPPL's Darwin defaults bake both Intel and Apple Silicon into OPTIM.
  # The driver is arm64-only; drop the unused slice.
  sister_replace("${PAPPL_MAKEDEFS}"
    "-arch x86_64 "
    "")
  # -g pulls DWARF into the static lib; -fPIC is for dylibs, not this archive.
  sister_replace("${PAPPL_MAKEDEFS}"
    "-g -Os"
    "-Os")
  sister_replace("${PAPPL_MAKEDEFS}"
    "-fPIC  "
    "")
  return()
endif()

sister_replace("${PAPPL_SRC}/pappl/loc.c"
"#include \"loc-private.h\"
#include \"strings/de_strings.h\"
#include \"strings/en_strings.h\"
#include \"strings/es_strings.h\"
#include \"strings/fr_strings.h\"
#include \"strings/it_strings.h\"
#include \"strings/ja_strings.h\"
#include \"strings/nb-NO_strings.h\"
#include \"strings/pl_strings.h\"
#include \"strings/tr_strings.h\"
"
"#include \"loc-private.h\"
#include \"strings/en_strings.h\"
")

sister_replace("${PAPPL_SRC}/pappl/loc.c"
"  r.language = \"de\";
  r.data     = (const void *)de_strings;

  _papplLocCreate(system, &r);

  r.language = \"en\";
  r.data     = (const void *)en_strings;

  _papplLocCreate(system, &r);

  r.language = \"es\";
  r.data     = (const void *)es_strings;

  _papplLocCreate(system, &r);

  r.language = \"fr\";
  r.data     = (const void *)fr_strings;

  _papplLocCreate(system, &r);

  r.language = \"it\";
  r.data     = (const void *)it_strings;

  _papplLocCreate(system, &r);

  r.language = \"ja\";
  r.data     = (const void *)ja_strings;

  _papplLocCreate(system, &r);

  r.language = \"nb-NO\";
  r.data     = (const void *)nb_NO_strings;

  _papplLocCreate(system, &r);

  r.language = \"pl\";
  r.data     = (const void *)pl_strings;

  _papplLocCreate(system, &r);

  r.language = \"tr\";
  r.data     = (const void *)tr_strings;

  _papplLocCreate(system, &r);
"
"  r.language = \"en\";
  r.data     = (const void *)en_strings;

  _papplLocCreate(system, &r);
")

sister_replace("${PAPPL_SRC}/pappl/loc.c"
"    if (!strncmp(loc_default.name, \"de\", 2))
      r.data = (const void *)de_strings;
    else if (!strncmp(loc_default.name, \"en\", 2))
      r.data = (const void *)en_strings;
    else if (!strncmp(loc_default.name, \"es\", 2))
      r.data = (const void *)es_strings;
    else if (!strncmp(loc_default.name, \"fr\", 2))
      r.data = (const void *)fr_strings;
    else if (!strncmp(loc_default.name, \"it\", 2))
      r.data = (const void *)it_strings;
    else if (!strncmp(loc_default.name, \"ja\", 2))
      r.data = (const void *)ja_strings;
    else if (!strncmp(loc_default.name, \"nb\", 2))
      r.data = (const void *)nb_NO_strings;
    else if (!strncmp(loc_default.name, \"pl\", 2))
      r.data = (const void *)pl_strings;
    else if (!strncmp(loc_default.name, \"tr\", 2))
      r.data = (const void *)tr_strings;
"
"    r.data = (const void *)en_strings;
")

sister_replace("${PAPPL_SRC}/pappl/system-accessors.c"
"  // Set the system TLS credentials...
  cupsSetServerCredentials(NULL, system->hostname, 1);
"
"  // Set the system TLS credentials...
  if (!(system->options & PAPPL_SOPTIONS_NO_TLS))
    cupsSetServerCredentials(NULL, system->hostname, 1);
"
)

sister_replace("${PAPPL_SRC}/pappl/system.c"
"  // Load base localizations...
  _papplLocLoadAll(system);
"
"  // English keys from _PAPPL_LOC are the UI strings; skip parsing the
  // catalog into thousands of strdup pairs at idle.
"
)

# Compile-out features this driver never enables so ld does not pull the
# corresponding .o files (USB gadget, raw sockets, HTTP monitor) or, for
# pages that live in system-webif.o with the home page, so -dead_strip can
# drop TLS/network/security/log HTML.
sister_replace("${PAPPL_SRC}/pappl/system.c"
"  if ((system->options & PAPPL_SOPTIONS_WEB_LOG) && system->log_file && strcmp(system->log_file, \"-\") && strcmp(system->log_file, \"syslog\"))
  {
    papplSystemAddResourceCallback(system, \"/logfile.txt\", \"text/plain\", (pappl_resource_cb_t)_papplSystemWebLogFile, system);
    papplSystemAddResourceCallback(system, \"/logs\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebLogs, system);
    papplSystemAddLink(system, _PAPPL_LOC(\"View Logs\"), \"/logs\", PAPPL_LOPTIONS_LOGGING | PAPPL_LOPTIONS_HTTPS_REQUIRED);
  }
"
"#if 0 /* SisterHL2030: no web-log pages */
  if ((system->options & PAPPL_SOPTIONS_WEB_LOG) && system->log_file && strcmp(system->log_file, \"-\") && strcmp(system->log_file, \"syslog\"))
  {
    papplSystemAddResourceCallback(system, \"/logfile.txt\", \"text/plain\", (pappl_resource_cb_t)_papplSystemWebLogFile, system);
    papplSystemAddResourceCallback(system, \"/logs\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebLogs, system);
    papplSystemAddLink(system, _PAPPL_LOC(\"View Logs\"), \"/logs\", PAPPL_LOPTIONS_LOGGING | PAPPL_LOPTIONS_HTTPS_REQUIRED);
  }
#endif
")

sister_replace("${PAPPL_SRC}/pappl/system.c"
"    if (system->options & PAPPL_SOPTIONS_WEB_NETWORK)
    {
      papplSystemAddResourceCallback(system, \"/network\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebNetwork, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Network\"), \"/network\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
      if (system->wifi_join_cb && system->wifi_list_cb && system->wifi_status_cb)
        papplSystemAddResourceCallback(system, \"/network-wifi\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebWiFi, system);
    }
    if (system->options & PAPPL_SOPTIONS_WEB_SECURITY)
    {
      papplSystemAddResourceCallback(system, \"/security\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebSecurity, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Security\"), \"/security\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
    }
#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
    if (system->options & PAPPL_SOPTIONS_WEB_TLS)
    {
      papplSystemAddResourceCallback(system, \"/tls-install-crt\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebTLSInstall, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Install TLS Certificate\"), \"/tls-install-crt\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
      papplSystemAddResourceCallback(system, \"/tls-new-crt\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebTLSNew, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Create New TLS Certificate\"), \"/tls-new-crt\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
      papplSystemAddResourceCallback(system, \"/tls-new-csr\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebTLSNew, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Create TLS Certificate Request\"), \"/tls-new-csr\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
    }
#endif // HAVE_GNUTLS || HAVE_OPENSSL
"
"#if 0 /* SisterHL2030: no network/security/TLS web pages */
    if (system->options & PAPPL_SOPTIONS_WEB_NETWORK)
    {
      papplSystemAddResourceCallback(system, \"/network\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebNetwork, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Network\"), \"/network\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
      if (system->wifi_join_cb && system->wifi_list_cb && system->wifi_status_cb)
        papplSystemAddResourceCallback(system, \"/network-wifi\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebWiFi, system);
    }
    if (system->options & PAPPL_SOPTIONS_WEB_SECURITY)
    {
      papplSystemAddResourceCallback(system, \"/security\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebSecurity, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Security\"), \"/security\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
    }
#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
    if (system->options & PAPPL_SOPTIONS_WEB_TLS)
    {
      papplSystemAddResourceCallback(system, \"/tls-install-crt\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebTLSInstall, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Install TLS Certificate\"), \"/tls-install-crt\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
      papplSystemAddResourceCallback(system, \"/tls-new-crt\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebTLSNew, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Create New TLS Certificate\"), \"/tls-new-crt\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
      papplSystemAddResourceCallback(system, \"/tls-new-csr\", \"text/html\", (pappl_resource_cb_t)_papplSystemWebTLSNew, system);
      papplSystemAddLink(system, _PAPPL_LOC(\"Create TLS Certificate Request\"), \"/tls-new-csr\", PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
    }
#endif
#endif
")

sister_replace("${PAPPL_SRC}/pappl/system.c"
"    // Start the raw socket listeners as needed...
    if ((system->options & PAPPL_SOPTIONS_RAW_SOCKET) && printer->num_raw_listeners > 0)
    {
      pthread_t	tid;			// Thread ID

      papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG, \"Starting socket listener thread.\");

      if (pthread_create(&tid, &tattr, (void *(*)(void *))_papplPrinterRunRaw, printer))
"
"#if 0 /* SisterHL2030: no raw sockets */
    // Start the raw socket listeners as needed...
    if ((system->options & PAPPL_SOPTIONS_RAW_SOCKET) && printer->num_raw_listeners > 0)
    {
      pthread_t	tid;			// Thread ID

      papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG, \"Starting socket listener thread.\");

      if (pthread_create(&tid, &tattr, (void *(*)(void *))_papplPrinterRunRaw, printer))
")

sister_replace("${PAPPL_SRC}/pappl/system.c"
"	_papplRWUnlock(printer);
      }
    }
  }
  pthread_rwlock_unlock(&system->printers_rwlock);

  // Start the USB gadget as needed...
"
"	_papplRWUnlock(printer);
      }
    }
#endif
  }
  pthread_rwlock_unlock(&system->printers_rwlock);

  // Start the USB gadget as needed...
")

sister_replace("${PAPPL_SRC}/pappl/system.c"
"  // Start the USB gadget as needed...
  if ((system->options & PAPPL_SOPTIONS_USB_PRINTER) && (printer = papplSystemFindPrinter(system, NULL, system->default_printer_id, NULL)) != NULL)
  {
    pthread_t	tid;			// Thread ID

    papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG, \"Starting USB listener thread.\");

    if (pthread_create(&tid, &tattr, (void *(*)(void *))_papplPrinterRunUSB, printer))
"
"#if 0 /* SisterHL2030: no USB gadget */
  // Start the USB gadget as needed...
  if ((system->options & PAPPL_SOPTIONS_USB_PRINTER) && (printer = papplSystemFindPrinter(system, NULL, system->default_printer_id, NULL)) != NULL)
  {
    pthread_t	tid;			// Thread ID

    papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG, \"Starting USB listener thread.\");

    if (pthread_create(&tid, &tattr, (void *(*)(void *))_papplPrinterRunUSB, printer))
")

sister_replace("${PAPPL_SRC}/pappl/system.c"
"      _papplRWUnlock(printer);
    }
  }

  // Loop until we are shutdown or have a hard error...
  papplLog(system, PAPPL_LOGLEVEL_DEBUG, \"Entering run loop.\");
"
"      _papplRWUnlock(printer);
    }
  }
#endif

  // Loop until we are shutdown or have a hard error...
  papplLog(system, PAPPL_LOGLEVEL_DEBUG, \"Entering run loop.\");
")

sister_replace("${PAPPL_SRC}/pappl/printer.c"
"  // Add socket listeners...
  if (system->options & PAPPL_SOPTIONS_RAW_SOCKET)
  {
    if (_papplPrinterAddRawListeners(printer) && system->is_running)
"
"  // Add socket listeners...
#if 0 /* SisterHL2030: no raw sockets */
  if (system->options & PAPPL_SOPTIONS_RAW_SOCKET)
  {
    if (_papplPrinterAddRawListeners(printer) && system->is_running)
")

sister_replace("${PAPPL_SRC}/pappl/printer.c"
"	_papplRWUnlock(printer);
      }
    }
  }

  // Add icons...
  _papplSystemAddPrinterIcons(system, printer);
"
"	_papplRWUnlock(printer);
      }
    }
  }
#endif

  // Add icons...
  _papplSystemAddPrinterIcons(system, printer);
")

sister_replace("${PAPPL_SRC}/pappl/device-network.c"
"  httpAddrFreeList(sock->list);
  free(sock);

  papplDeviceSetData(device, NULL);
"
"  httpAddrFreeList(sock->list);
  free(sock->host);
  free(sock);

  papplDeviceSetData(device, NULL);
")

sister_replace("${PAPPL_SRC}/pappl/device-network.c"
"  if ((sock->snmp_fd = _papplSNMPOpen(httpAddrGetFamily(&(sock->addr->addr)))) < 0)
  {
    papplDeviceError(device, \"Unable to open SNMP socket.\");
    return (false);
  }
"
"  if ((sock->snmp_fd = _papplSNMPOpen(httpAddrGetFamily(&(sock->addr->addr)))) < 0)
  {
    papplDeviceError(device, \"Unable to open SNMP socket.\");
    goto error;
  }
")

