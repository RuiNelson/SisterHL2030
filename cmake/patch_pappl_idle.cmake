# Shrink PAPPL's idle footprint for a LaunchDaemon that never shows a UI.
#
# Applied to the extracted PAPPL ${PAPPL_VERSION} tree:
#   - English-only catalogs (the other eight languages are ~450 KB of
#     __cstring plus thousands of strdup pairs at startup)
#   - Skip cupsSetServerCredentials when PAPPL_SOPTIONS_NO_TLS is set
#     (otherwise PAPPL mints a self-signed cert on every boot)
#   - After configure: dummy status UI so AppKit is not linked, and
#     arm64-only so the static lib is not a fat x86_64+arm64 archive
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
