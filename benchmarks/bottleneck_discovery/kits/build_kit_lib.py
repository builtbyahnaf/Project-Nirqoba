"""Builds a nirqoba_runtime.lib for one extracted revision tree (diagnostic).

Usage: python build_kit_lib.py <rev> <tree> <out_lib>
"""
import glob
import os
import subprocess
import sys

rev, tree, out_lib = sys.argv[1], sys.argv[2], sys.argv[3]
srcs = [p for p in glob.glob(os.path.join(tree, 'runtime/src/*.cpp'))
        if not p.endswith('socket_windows.cpp')]
objs = os.path.join('build', 'kits', rev)
os.makedirs(objs, exist_ok=True)

def q(p):
    return '"' + p.replace('/', '\\') + '"'

lines = [
    '@echo off',
    'call "C:/Program Files/Microsoft Visual Studio/18/BuildTools/VC/Auxiliary/Build/vcvars64.bat"',
    'if errorlevel 1 exit /b 1',
    'cd /d "D:/Actual Project Files/Project Nirqoba"',
]
for src in srcs:
    obj = os.path.join(objs, os.path.basename(src) + '.obj')
    lines.append('cl /nologo /c /O2 /DNDEBUG /std:c++20 /EHsc /MD /W0 '
                 '/DNRQ_RUNTIME_DEBUG_ENABLED=0 /DNOMINMAX /DWIN32_LEAN_AND_MEAN '
                 '/I ' + q(os.path.join(tree, 'runtime/include')) +
                 ' /I ' + q(os.path.join(tree, 'runtime/src')) +
                 ' /Fo' + q(obj) + ' ' + q(src))
    lines.append('if errorlevel 1 exit /b 1')
obj_list = ' '.join(q(os.path.join(objs, os.path.basename(s) + '.obj')) for s in srcs)
lines.append('lib /nologo /OUT:' + q(out_lib) + ' ' + obj_list)
lines.append('if errorlevel 1 exit /b 1')
bat = os.path.join(objs, 'build_lib.bat')
with open(bat, 'w', encoding='ascii', newline='\r\n') as handle:
    handle.write('\n'.join(lines) + '\n')
result = subprocess.run([bat], capture_output=True, text=True)
print('exit', result.returncode)
if result.returncode != 0:
    print(result.stdout[-3000:])
    print(result.stderr[-3000:])
else:
    print('built', out_lib, os.path.getsize(out_lib), 'bytes')
