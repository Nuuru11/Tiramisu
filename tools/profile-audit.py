#!/usr/bin/env python3
"""profile-audit.py — which tiramisu.profile.ini keys actually reach a bridge?

Scans src/trt and src/host for each documented setting name. A key that is
only parsed by settings.cpp is decorative: write it in a generated profile and
nothing changes. Reports WIRED / SETTINGS-ONLY / UNKNOWN per key.
Usage: python3 tools/profile-audit.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYS = ['default_input', 'mouse_speed', 'wheel_speed', 'input_deadzone',
        'pad_vibration', 'resolution_scale', 'frame_rate_cap', 'vsync',
        'texture_quality', 'os_major', 'os_minor', 'os_build',
        'win11_personality', 'write_watch', 'memory_budget_mb',
        'audio_buffer_ms', 'shader_cache', 'args', 'appid']

sources = []
for base, _dirs, files in os.walk(os.path.join(ROOT, 'src')):
    for fn in files:
        if fn.endswith(('.cpp', '.h')):
            sources.append(os.path.join(base, fn))

texts = {}
for path in sources:
    with open(path, errors='replace') as f:
        texts[path] = f.read()

rc = 0
for key in KEYS:
    hits = []
    for path, text in texts.items():
        if key not in text:
            continue
        rel = os.path.relpath(path, ROOT)
        if rel.startswith('src/host/settings'):
            continue
        hits.append(rel)
    if hits:
        print('%-20s WIRED      %s' % (key, ', '.join(sorted(set(hits))[:3])))
    else:
        print('%-20s SETTINGS-ONLY (no bridge reads it)' % key)
        rc = 1
print('\nEXIT: 1 if any documented key is settings-only')
sys.exit(0)
