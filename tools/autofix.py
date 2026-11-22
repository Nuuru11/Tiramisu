#!/usr/bin/env python3
"""autofix.py — failure-signature matcher + fix planner.

Reads a failed boot's tiramisu.log, matches declared signatures from
tools/fix_db.json, and produces an idempotent fix plan. Applying the plan
writes REAL repairs the next launch will use:

  <game>/tiramisu.profile.ini        merged setting overrides
  <game>/.tiramisu/aliases.json      DLL forwarders for missing redists
  <game>/.tiramisu/autofix.json      attempt history (evidence, not a claim)

Rules never hide a gap: they either supply a real alias/setting or record a
named work item. Usage:
  python3 tools/autofix.py <game_dir> <logfile> [--apply] [--attempt N]
Exit: 0 always (an audit/planner; callers decide).
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_db(path=None):
    with open(path or os.path.join(HERE, 'fix_db.json')) as f:
        return json.load(f)


def match_log(log_text, db):
    """Return the rules whose every pattern is present in the log."""
    hits = []
    for rule in db['rules']:
        pats = rule.get('match', [])
        if pats and any(re.search(p, log_text, re.I) for p in pats):
            hits.append(rule)
    return hits


def read_ini(path):
    out = {}
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or '=' not in line:
                continue
            k, v = line.split('=', 1)
            out[k.strip()] = v.strip()
    return out


def write_ini(path, values):
    """Rewrite the ini, preserving unrelated keys and comments' key lines."""
    with open(path, 'w') as f:
        f.write('# generated/merged by tools/autofix.py\n')
        for k in sorted(values):
            f.write('%s=%s\n' % (k, values[k]))


def read_json(path, default):
    if not os.path.exists(path):
        return default
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return default


def plan(game_dir, log_text, db=None, attempt=1):
    db = db or load_db()
    hits = match_log(log_text, db)
    profile = {}
    aliases = {}
    for rule in hits:
        acts = rule.get('actions', {})
        profile.update(acts.get('profile', {}))
        aliases.update(acts.get('aliases', {}))
    return {
        'attempt': attempt,
        'matched_rules': [r['id'] for r in hits],
        'notes': [r['note'] for r in hits],
        'profile_overrides': profile,
        'aliases': aliases,
        'severity': 'warn' if any(r.get('severity') == 'warn' for r in hits)
                    else 'info',
    }


def apply(game_dir, result):
    meta_dir = os.path.join(game_dir, '.tiramisu')
    os.makedirs(meta_dir, exist_ok=True)

    if result['profile_overrides']:
        path = os.path.join(game_dir, 'tiramisu.profile.ini')
        values = read_ini(path)
        values.update({k: str(v) for k, v in result['profile_overrides'].items()})
        write_ini(path, values)

    if result['aliases']:
        path = os.path.join(meta_dir, 'aliases.json')
        cur = read_json(path, {})
        cur.update(result['aliases'])
        with open(path, 'w') as f:
            json.dump(cur, f, indent=2, sort_keys=True)

    hist_path = os.path.join(meta_dir, 'autofix.json')
    hist = read_json(hist_path, {'attempts': []})
    hist['attempts'].append(result)
    with open(hist_path, 'w') as f:
        json.dump(hist, f, indent=2, sort_keys=True)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game_dir')
    ap.add_argument('logfile')
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--attempt', type=int, default=1)
    ap.add_argument('--db', default=None)
    a = ap.parse_args(argv[1:])

    with open(a.logfile, errors='replace') as f:
        log = f.read()
    result = plan(a.game_dir, log, load_db(a.db), a.attempt)
    print(json.dumps(result, indent=2, sort_keys=True))
    if a.apply:
        apply(a.game_dir, result)
        print('applied: %s' % a.game_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
