#!/usr/bin/env python3
"""Build tools/charset.txt: every character the e-paper fonts must contain.

Sources:
  * Big5 level-1 (the ~5,400 common Traditional Chinese characters)
  * every character used in hkbus.app route names and stop names
    (routeFareList.min.json from https://data.hkbus.app/)
  * ASCII and a handful of punctuation marks

Usage:
  python3 tools/make_charset.py routeFareList.min.json > tools/charset.txt
The JSON argument is optional; without it only Big5 level-1 + ASCII is used.
"""
import json
import sys

chars = set()

# ASCII printable
chars.update(chr(c) for c in range(0x20, 0x7F))

# Punctuation and symbols that appear in names or in our own UI
chars.update("，。、；：？！「」『』（）〔〕【】《》〈〉—…·‧．／＆＋－＝％＃＊＠"
             "→←↑↓▲▼●○◆◇■□★☆✕✓℃°")

# Big5 level 1: lead bytes 0xA4-0xC6, trail 0x40-0x7E and 0xA1-0xFE
for lead in range(0xA4, 0xC7):
    for trail in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
        try:
            ch = bytes([lead, trail]).decode("big5")
        except UnicodeDecodeError:
            continue
        if len(ch) == 1:
            chars.add(ch)

if len(sys.argv) > 1:
    with open(sys.argv[1], encoding="utf-8") as f:
        db = json.load(f)
    for route in db["routeList"].values():
        for key in ("orig", "dest"):
            chars.update(route[key]["zh"])
        chars.update(route["route"])
    for stop in db["stopList"].values():
        chars.update(stop["name"]["zh"])

# Strip control/odd whitespace except the plain space.
chars = {c for c in chars if c == " " or (c.isprintable() and not c.isspace())}
sys.stdout.write("".join(sorted(chars)))
sys.stdout.write("\n")
print(f"{len(chars)} characters", file=sys.stderr)
