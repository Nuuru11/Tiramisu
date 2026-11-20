#!/usr/bin/env python3
"""Parse Mono 6.8 msvc/*.targets files and emit the Windows x64 SGen
static-libmono source list.

Evaluates a small subset of MSBuild conditions sufficient for these
files: '$(VAR)'=='val' / '!=' comparisons, And/Or, and Exists().
PropertyGroup assignments are applied in document order (imports
first, depth-first) so ExcludeFromWindows* properties resolve.

Usage: parse_targets.py <msvc-dir> <repo-root> <out-file>
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

MSVC_DIR = os.path.abspath(sys.argv[1])
REPO_ROOT = os.path.abspath(sys.argv[2])
OUT_FILE = sys.argv[3]
GIT_ROOT = os.path.dirname(MSVC_DIR)  # .targets live in <git-root>/msvc/

NS = "{http://schemas.microsoft.com/developer/msbuild/2003}"

# Seeded properties for our configuration: Release|x64, SGen, no LLVM,
# no netcore, no BTLS, static CRT.
PROPS = {
    "Configuration": "Release",
    "Platform": "x64",
    "MONO_ENABLE_LLVM": "false",
    "MONO_ENABLE_NETCORE": "false",
    "MONO_ENABLE_BTLS": "false",
    "MONO_TARGET_GC": "sgen",
    "MONO_USE_STATIC_C_RUNTIME": "true",
    "MONO_USE_STATIC_LIBMONO": "true",
    "MonoSourceLocation": "..",
    "MSBuildThisFileDirectory": MSVC_DIR + os.sep,
    "MSBuildProjectDirectory": MSVC_DIR,
}

VAR_RE = re.compile(r"\$\(([^)]+)\)")


def expand(s):
    def rep(m):
        return PROPS.get(m.group(1), "")
    # Expand repeatedly (properties may reference each other).
    for _ in range(5):
        new = VAR_RE.sub(rep, s)
        if new == s:
            break
        s = new
    return s


def eval_cond(cond):
    """Evaluate an MSBuild Condition string. Returns True/False."""
    if cond is None or cond.strip() == "":
        return True
    c = expand(cond.strip())
    # Exists('path') -> check against the repo root / msvc dir.
    def exists_rep(m):
        p = m.group(1).strip("'\"")
        p = p.replace("\\", os.sep)
        for base in (REPO_ROOT, MSVC_DIR):
            if os.path.exists(os.path.join(base, p)):
                return "true"
        return "false"
    c = re.sub(r"Exists\(([^)]*)\)", exists_rep, c, flags=re.I)
    # Normalize: 'a'=='b', 'a'!='b', And/Or/! (case-insensitive keywords).
    c = re.sub(r"\bAnd\b", " and ", c, flags=re.I)
    c = re.sub(r"\bOr\b", " or ", c, flags=re.I)
    c = re.sub(r"!(?!=)", " not ", c)
    # Single-quoted strings -> double-quoted for Python eval.
    c = re.sub(r"'([^']*)'", r'"\1"', c)
    c = c.replace("==", " == ").replace("!=", " != ")
    # Only allow a safe subset of characters now.
    if not re.fullmatch(r'[\w\s"\.\-\\/\(\)=!<>]+', c):
        # Unknown construct: be conservative and EXCLUDE the item, but
        # record it so the caller can review.
        UNKNOWN_CONDS.append(cond)
        return False
    try:
        return bool(eval(c, {"__builtins__": {}}, {}))
    except Exception:
        UNKNOWN_CONDS.append(cond)
        return False


UNKNOWN_CONDS = []
SOURCES = []  # (lib, repo-relative path)
SEEN_FILES = set()


def process_file(path, lib):
    if path in SEEN_FILES:
        return
    SEEN_FILES.add(path)
    try:
        tree = ET.parse(path)
    except ET.ParseError as e:
        print("PARSE FAIL %s: %s" % (path, e))
        return
    root = tree.getroot()
    base_dir = os.path.dirname(path)
    for child in root:
        tag = child.tag.replace(NS, "")
        if tag == "Import":
            proj = child.get("Project", "")
            if "VCTargetsPath" in proj or "UserRootDir" in proj:
                continue
            p = expand(proj).replace("\\", os.sep)
            if not os.path.isabs(p):
                p = os.path.join(base_dir, p)
            p = os.path.normpath(p)
            if os.path.exists(p):
                process_file(p, lib)
            else:
                print("MISSING IMPORT %s (from %s)" % (p, path))
        elif tag == "PropertyGroup":
            if not eval_cond(child.get("Condition")):
                continue
            for prop in child:
                pname = prop.tag.replace(NS, "")
                pcond = prop.get("Condition")
                if pcond and not eval_cond(pcond):
                    continue
                PROPS[pname] = expand((prop.text or "").strip())
        elif tag == "ItemGroup":
            if not eval_cond(child.get("Condition")):
                continue
            for item in child:
                itag = item.tag.replace(NS, "")
                if itag != "ClCompile":
                    continue
                if not eval_cond(item.get("Condition")):
                    continue
                # <ExcludedFromBuild>$(ExcludeFromWindowsBuild)</ExcludedFromBuild>
                # is how the .targets files drop posix-only / wrong-arch /
                # wrong-GC sources from the Windows build. The properties
                # are defined by the PropertyGroups processed above.
                excluded = False
                for meta in item:
                    if meta.tag.replace(NS, "") == "ExcludedFromBuild":
                        if expand((meta.text or "").strip()).lower() == "true":
                            excluded = True
                if excluded:
                    continue
                inc = item.get("Include", "")
                if not inc:
                    continue
                p = expand(inc).replace("\\", os.sep)
                if not os.path.isabs(p):
                    p = os.path.join(base_dir, p)
                p = os.path.normpath(p)
                # Sources are repo-relative; re-root from the git tree
                # (where the .targets live) into the tarball tree.
                rel = os.path.relpath(p, GIT_ROOT)
                SOURCES.append((lib, rel))


LIBS = [
    ("eglib", "eglib.targets"),
    ("libmini", "libmini.targets"),
    ("libmonoruntime", "libmonoruntime.targets"),
    ("libmonoutils", "libmonoutils.targets"),
    ("libgcmonosgen", "libgcmonosgen.targets"),
    ("monozlib", "monozlib.targets"),
]

for lib, targets in LIBS:
    process_file(os.path.join(MSVC_DIR, targets), lib)

# Deduplicate, preserving order.
seen = set()
unique = []
for lib, rel in SOURCES:
    if rel not in seen:
        seen.add(rel)
        unique.append((lib, rel))

missing = [rel for _, rel in unique
           if not os.path.exists(os.path.join(REPO_ROOT, rel))]
with open(OUT_FILE, "w") as f:
    for lib, rel in unique:
        f.write("%s\t%s\n" % (lib, rel))

from collections import Counter
counts = Counter(lib for lib, _ in unique)
print("total sources: %d" % len(unique))
for lib, _ in LIBS:
    print("  %s: %d" % (lib, counts.get(lib, 0)))
print("missing from extracted tree: %d" % len(missing))
for m in missing[:40]:
    print("  MISSING: %s" % m)
print("unknown conditions: %d" % len(set(UNKNOWN_CONDS)))
for u in sorted(set(UNKNOWN_CONDS))[:20]:
    print("  COND?: %s" % u)
