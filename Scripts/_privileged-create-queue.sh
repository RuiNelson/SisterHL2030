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

tmp="$(mktemp /tmp/sister-queue.XXXXXX)"
mv "$tmp" "$tmp.ppd"
tmp="$tmp.ppd"
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

/usr/bin/ruby - "$tmp" <<'RUBY'
path = ARGV.fetch(0)
text = File.binread(path).force_encoding(Encoding::ISO_8859_1)

def sub_line(src, key, line)
  pattern = /^#{Regexp.escape(key)}.*\n/
  return src.sub(pattern, line + "\n") if src.match?(pattern)
  snmp = /^\*cupsSNMPSupplies:.*\n/
  return src.sub(snmp) { |match| match + line + "\n" } if src.match?(snmp)
  src.sub("*PPD-Adobe: \"4.3\"\n", "*PPD-Adobe: \"4.3\"\n" + line + "\n")
end

text = text.gsub(/^\*APSupplies:.*\n/, '')
text = sub_line(text, '*cupsIPPSupplies:', '*cupsIPPSupplies: True')
unless text.include?('*cupsSNMPSupplies:')
  text = sub_line(text, '*cupsSNMPSupplies:', '*cupsSNMPSupplies: False')
end
text = text.gsub(/^\*DefaultResolution:.*$/, '*DefaultResolution: 600dpi')
text = text.gsub(/^\*OpenUI \*cupsPrintQuality.*$/, '*OpenUI *cupsPrintQuality/Quality: PickOne')
[['Draft', 'Draft', 3, 300], ['Normal', 'Normal', 4, 600], ['High', 'Fine', 5, 600]].each do |key, label, quality, dpi|
  text = text.gsub(/^\*cupsPrintQuality #{key}(?:\/[^:]*)?:.*$/,
    "*cupsPrintQuality #{key}/#{label}: \"<</cupsInteger1 #{quality} /HWResolution[#{dpi} #{dpi}]>>setpagedevice\"")
end
[['InputSlot', ['main', 'Main']], ['MediaType', ['stationery', 'Stationery']]].each do |keyword, choices|
  choice = choices.find { |value| text.match?(/^\*#{keyword} #{value}(?:\/[^:]*)?:/) }
  text = text.gsub(/^\*Default#{keyword}:.*$/, "*Default#{keyword}: #{choice}") if choice
end
# Empty option codes make the print dialog drop the rest of the pane.
['MediaType', 'InputSlot'].each do |keyword|
  text = text.gsub(/^(\*#{keyword} )([^\s\/:]+)(\/[^:]*)?: ""$/) do
    "#{$1}#{$2}#{$3}: \"<</#{keyword}(#{$2})>>setpagedevice\""
  end
end
unless text.include?('GrayDeep_with_Paper_Auto-Detect')
  text = text.rstrip + "\n" + <<'PRESET'
*APPrinterPreset GrayDeep_with_Paper_Auto-Detect/Black and White - Fine: "
	*cupsPrintQuality High
	*ColorModel Gray
	com.apple.print.preset.graphicsType General
	com.apple.print.preset.quality high
	com.apple.print.preset.media-front-coating autodetect
	com.apple.print.preset.output-mode monochrome"
*End
PRESET
end
File.binwrite(path, text)
RUBY

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
