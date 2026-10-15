#!/usr/bin/env python3
import json, os, sys

res_build = sys.argv[1]
screen = sys.argv[2]
json_path = sys.argv[3]

with open(json_path, encoding='utf-8') as f:
    notes = json.load(f)

fallback_locale = notes.get('en', {})

for dirpath, _, filenames in os.walk(res_build):
    if 'welcome.html' not in filenames:
        continue
    locale = os.path.basename(dirpath)
    if locale.endswith('.lproj'):
        locale = locale[:-len('.lproj')]
    path = os.path.join(dirpath, 'welcome.html')
    with open(path, encoding='utf-8') as f:
        text = f.read()
    if '@HALFTONE_NOTE@' not in text:
        continue
    locale_notes = notes.get(locale, fallback_locale)
    note = locale_notes.get(screen, fallback_locale.get(screen, ''))
    text = text.replace('@HALFTONE_NOTE@', note)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
