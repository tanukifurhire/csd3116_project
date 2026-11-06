#!/usr/bin/env python3
"""portability_report.py LOG [DIR...] -- pull MSVC diagnostics for the given
source dirs (default: game client_folder) out of an MSBuild log, de-duplicated,
as 'file:line: error|warning CODE: message' lines."""
import re, sys
log = sys.argv[1]
dirs = sys.argv[2:] or ["game", "client_folder"]
pat = re.compile(r'([A-Za-z]:\\[^():]*?\\(?:%s)\\[^():]+)\((\d+)(?:,\d+)?\): (error|warning) ([A-Z]+\d+): ([^\[]*)' % "|".join(dirs))
seen = set()
for line in open(log, encoding="utf-8", errors="replace"):
    m = pat.search(line)
    if not m: continue
    path, ln, kind, code, msg = m.groups()
    rel = path.replace("\\", "/")
    for d in dirs:
        i = rel.find("/" + d + "/")
        if i >= 0: rel = rel[i + 1:]; break
    key = (rel, ln, code, msg.strip())
    if key in seen: continue
    seen.add(key)
    print(f"{rel}:{ln}: {kind} {code}: {msg.strip()}")
