#!/usr/bin/env python3
"""Generate compile_commands.json from a dry run of Makefile.x86_64.

IDEs (CLion / IntelliJ C/C++, clangd) need this to resolve -I paths and
macros, which is what makes go-to-definition work. Run from anywhere:

    tools/gen_compile_commands.py [makefile]   # or: make -f Makefile.x86_64 compdb
"""
import json, os, re, shlex, subprocess, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
makefile = sys.argv[1] if len(sys.argv) > 1 else "Makefile.x86_64"
out = subprocess.run(["make", "-f", makefile, "-nB", "all"], cwd=root,
                     capture_output=True, text=True).stdout

entries = []
for line in out.splitlines():
    if not re.match(r"^\S*gcc\s", line) or " -c " not in line:
        continue
    args = shlex.split(line)
    src = args[args.index("-c") + 1]
    entries.append({"directory": root, "file": src, "arguments": args})

with open(os.path.join(root, "compile_commands.json"), "w") as f:
    json.dump(entries, f, indent=1)
print(f"wrote {len(entries)} entries")
