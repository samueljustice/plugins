#!/usr/bin/env python3
"""Syntax-check one or more .cpp files with the plugin's real compile flags (no linking, no shared build dir).
Usage: python3 tools/syntax-check.py Source/Foo.cpp [Source/Bar.cpp ...]
Needs build/compile_commands.json (cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)."""
import json, shlex, subprocess, sys, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
db = json.load(open(os.path.join(root, 'build', 'compile_commands.json')))
template = next(x for x in db if x['file'].endswith('PluginEditor.cpp'))
args = shlex.split(template['command'])
# drop output / source arguments
clean, skip = [], False
for a in args:
    if skip: skip = False; continue
    if a in ('-o', '-c'): skip = (a == '-o'); continue
    if a.endswith('.cpp') or a.endswith('.mm'): continue
    clean.append(a)
clean = [a for a in clean if not a.startswith('-flto')]
rc = 0
for f in sys.argv[1:]:
    cmd = clean + ['-fsyntax-only', '-I' + os.path.join(root, 'Source'), os.path.abspath(f)]
    r = subprocess.run(cmd, cwd=template['directory'])
    print(('OK   ' if r.returncode == 0 else 'FAIL ') + f)
    rc |= r.returncode
sys.exit(rc)
