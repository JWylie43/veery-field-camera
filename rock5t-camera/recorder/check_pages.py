#!/usr/bin/env python3
"""Validate the panel's embedded page scripts.  Run after editing PAGE/FILES_PAGE.

    python3 check_pages.py [path/to/veery_server.py]

The pages are Python strings, so the compiler never sees them. Two classes of
bug have actually shipped from that:

  1. a stray apostrophe ends the string early and kills the WHOLE <script>
     block (symptom: buttons dead, status frozen);
  2. a function defined in the wrong page - the script parses fine but throws
     ReferenceError at runtime, and whatever it was rendering silently stays
     empty. Cost: "where did my files go?" after syncCell landed on PAGE
     instead of FILES_PAGE (2026-09-20).

So this checks BOTH pages for: JS syntax (via node, if present), every on*=
handler in the markup being defined, and every bare call in the script
resolving to something that exists.
"""
import re
import subprocess
import sys

# calls that are provided by the browser or the language, not by our script
KNOWN = {
    "fetch", "alert", "confirm", "setTimeout", "setInterval", "clearTimeout",
    "encodeURIComponent", "decodeURIComponent", "parseInt", "parseFloat",
    "Number", "String", "Boolean", "Array", "Object", "JSON", "Math", "Date",
    "if", "for", "while", "switch", "catch", "return", "typeof", "function",
    "querySelectorAll", "getElementById", "async", "await", "new", "delete",
}


def strip_literals(js):
    """Blank out comments and string/template literals.

    Without this, ordinary prose inside a string - "33 GB used (x)" - looks
    exactly like a call to used(), and every check drowns in false positives.
    """
    js = re.sub(r"/\*.*?\*/", " ", js, flags=re.S)
    js = re.sub(r"//[^\n]*", " ", js)
    js = re.sub(r"'(?:\\.|[^'\\\n])*'", "''", js)
    js = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', js)
    js = re.sub(r"`(?:\\.|[^`\\])*`", "``", js)
    return js

src = open(sys.argv[1] if len(sys.argv) > 1 else "veery_server.py").read()
ns = {}
for pat in (r'^CSS = """.*?"""',
            r'^PAGE = """.*?"""\.replace\("%CSS%", CSS\)',
            r'^FILES_PAGE = """.*?"""\.replace\("%CSS%", CSS\)'):
    m = re.search(pat, src, re.S | re.M)
    if not m:
        sys.exit(f"could not extract a page matching {pat!r}")
    exec(m.group(0), ns)

have_node = subprocess.run(["which", "node"], capture_output=True).returncode == 0
failed = False

for name in ("PAGE", "FILES_PAGE"):
    html = ns[name]
    js = html.split("<script>")[1].split("</script>")[0]
    problems = []

    if have_node:
        path = f"/tmp/{name}.js"
        open(path, "w").write(js)
        r = subprocess.run(["node", "--check", path], capture_output=True, text=True)
        if r.returncode:
            problems.append("JS syntax error:\n" + r.stderr.strip())

    code = strip_literals(js)
    defined = set(re.findall(r"function (\w+)", code))
    defined |= set(re.findall(r"(?:const|let|var)\s+(\w+)\s*=\s*(?:\(|\w+\s*=>)", code))

    handlers = set(re.findall(r'on\w+="(\w+)\(', html))
    for h in sorted(handlers - defined):
        problems.append(f'markup calls {h}() but no such function in this page')

    # bare calls: not preceded by a dot (method) or word character
    calls = set(re.findall(r"(?<![.\w$])([a-zA-Z_]\w*)\s*\(", code))
    for c in sorted(calls - defined - KNOWN):
        problems.append(f"script calls {c}() but it is not defined here")

    if problems:
        failed = True
        print(f"[FAIL] {name}")
        for p in problems:
            print("   ", p)
    else:
        print(f"[ok]   {name}: {len(defined)} functions, {len(handlers)} handlers, all resolve")

sys.exit(1 if failed else 0)
