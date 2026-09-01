#!/bin/bash
# Create the local CUPS queue with Apple's AirPrint PPD so the print dialog
# shows Quality (Draft / Normal / Fine).
# Usage: _privileged-create-queue.sh QUEUE
#
# Tahoe's print dialog ignores cupsPrintQuality on an IPP Everywhere queue
# (lpadmin -m everywhere). AirPrint quality is the three APPrinterPreset
# entries with com.apple.print.preset.quality draft/mid/high. ipp2ppd only
# emits Fine when High uses a different resolution than Normal; ours does
# not (HQ1200 is still a 600 dpi raster), so we add that preset ourselves.
#
# lpadmin -P is how System Settings installs every AirPrint printer. CUPS
# may warn that PPDs are deprecated; the alternative everywhere PPD hides
# Quality and maps Normal to 300 dpi.
export PATH="/usr/sbin:/usr/bin:/bin:/sbin"
set -euo pipefail

QUEUE="${1:-Brother_HL_2030_series}"
URI="ipp://localhost:8631/ipp/print"
IPP2PPD="/System/Library/Printers/Libraries/ipp2ppd"
PPD="/etc/cups/ppd/${QUEUE}.ppd"

if ! nc -z localhost 8631 2>/dev/null; then
  echo "WARNING: the printer application is not listening on 8631." >&2
fi

tmp="$(mktemp /tmp/sister-queue.XXXXXX.ppd)"
generated=""

if [[ -x "$IPP2PPD" ]]; then
  if "$IPP2PPD" "$URI" /dev/null >"$tmp" 2>/dev/null &&
     grep -q '^\*PPD-Adobe:' "$tmp"; then
    generated="ipp2ppd"
  fi
fi

if [[ -z "$generated" ]]; then
  echo "ipp2ppd unavailable; generating an everywhere PPD to patch." >&2
  /usr/sbin/lpadmin -p "$QUEUE" -v "$URI" -m everywhere -E \
    -D "Brother HL-2030 series" -L "SisterHL2030" \
    -o printer-is-shared=false
  if [[ ! -f "$PPD" ]]; then
    echo "Could not create the CUPS queue." >&2
    rm -f "$tmp"
    exit 1
  fi
  cp "$PPD" "$tmp"
  generated="everywhere"
fi

python3 - "$tmp" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path, encoding="latin-1").read()

def sub_line(src, key, line):
    pat = re.compile(r'(?m)^' + re.escape(key) + r'.*\n')
    if pat.search(src):
        return pat.sub(line + "\n", src, count=1)
    snmp = re.compile(r'(?m)^(\*cupsSNMPSupplies:.*\n)')
    if snmp.search(src):
        return snmp.sub(r'\1' + line + "\n", src, count=1)
    return src.replace("*PPD-Adobe: \"4.3\"\n",
                       "*PPD-Adobe: \"4.3\"\n" + line + "\n", 1)

text = re.sub(r'(?m)^\*APSupplies:.*\n', '', text)
text = sub_line(text, "*cupsIPPSupplies:", "*cupsIPPSupplies: True")
if "*cupsSNMPSupplies:" not in text:
    text = sub_line(text, "*cupsIPPSupplies:", "*cupsSNMPSupplies: False")

text = re.sub(r'(?m)^\*DefaultResolution:.*$',
              '*DefaultResolution: 600dpi', text)
text = re.sub(
    r'(?m)^\*OpenUI \*cupsPrintQuality.*$',
    '*OpenUI *cupsPrintQuality/Quality: PickOne',
    text)
text = re.sub(
    r'(?m)^\*cupsPrintQuality Draft:.*$',
    '*cupsPrintQuality Draft/Draft: "<</cupsInteger1 3 /HWResolution[300 300]>>setpagedevice"',
    text)
text = re.sub(
    r'(?m)^\*cupsPrintQuality Normal:.*$',
    '*cupsPrintQuality Normal/Normal: "<</cupsInteger1 4 /HWResolution[600 600]>>setpagedevice"',
    text)
text = re.sub(
    r'(?m)^\*cupsPrintQuality High:.*$',
    '*cupsPrintQuality High/Fine: "<</cupsInteger1 5 /HWResolution[600 600]>>setpagedevice"',
    text)

slot = "main" if re.search(r'(?m)^\*InputSlot main:', text) else (
    "Main" if re.search(r'(?m)^\*InputSlot Main:', text) else None)
if slot:
    text = re.sub(r'(?m)^\*DefaultInputSlot:.*$',
                  '*DefaultInputSlot: ' + slot, text)
media = "stationery" if re.search(r'(?m)^\*MediaType stationery:', text) else (
    "Stationery" if re.search(r'(?m)^\*MediaType Stationery:', text) else None)
if media:
    text = re.sub(r'(?m)^\*DefaultMediaType:.*$',
                  '*DefaultMediaType: ' + media, text)

# Empty option codes make the print dialog drop the rest of the pane.
def fill_empty_option(src, keyword):
    return re.sub(
        r'(?m)^(\*' + keyword + r' )(\S+): ""$',
        lambda m: m.group(1) + m.group(2)
        + ': "<</' + keyword + '(' + m.group(2) + ')>>setpagedevice"',
        src,
    )

text = fill_empty_option(text, "MediaType")
text = fill_empty_option(text, "InputSlot")

if "GrayDeep_with_Paper_Auto-Detect" not in text:
    text = text.rstrip() + """
*APPrinterPreset GrayDeep_with_Paper_Auto-Detect/Black and White - Fine: "
	*cupsPrintQuality High
	*ColorModel Gray
	com.apple.print.preset.graphicsType General
	com.apple.print.preset.quality high
	com.apple.print.preset.media-front-coating autodetect
	com.apple.print.preset.output-mode monochrome"
*End
"""

open(path, "w", encoding="latin-1", newline="\n").write(text)
PY

# Register as an AirPrint PPD, not IPP Everywhere. Tahoe only draws the
# Quality control from APPrinterPreset on an AirPrint queue.
#
# lpadmin -P is what installs $PPD, applying its own defaults as it writes.
# Copying $tmp over the result afterwards would throw those away, so don't:
# $tmp is only ever the input. And a failed lpadmin means there is no queue
# at all -- report that instead of leaving a PPD behind for a queue that
# does not exist.
status=0
/usr/sbin/lpadmin -p "$QUEUE" -v "$URI" -P "$tmp" -E \
  -D "Brother HL-2030 series" -L "SisterHL2030" \
  -o printer-is-shared=false || status=$?
rm -f "$tmp"
if [[ "$status" -ne 0 || ! -f "$PPD" ]]; then
  echo "Could not create the CUPS queue (lpadmin exited $status)." >&2
  exit 1
fi

# lpadmin writes $PPD itself, normally as root:_lp and mode 644, so these are
# usually a no-op. They are kept because cupsd runs as _lp and must be able
# to read the file whatever an earlier install or a hand-edited copy left
# behind, and re-asserting costs nothing.
chmod 644 "$PPD"
chown root:_lp "$PPD" 2>/dev/null || true

/usr/sbin/cupsenable "$QUEUE" 2>/dev/null || true
/usr/sbin/cupsaccept "$QUEUE" 2>/dev/null || true
/usr/sbin/lpadmin -p "$QUEUE" -o printer-is-shared=false 2>/dev/null || true
launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || true

echo "CUPS queue '$QUEUE' created from $generated PPD (AirPrint Quality UI)."
if [[ -f "$PPD" ]]; then
  echo "Quality mapping:"
  grep -E '^\*(DefaultResolution|cupsPrintQuality |APPrinterPreset|cupsIPPSupplies)' "$PPD" || true
fi
