#!/usr/bin/env python3
"""autopilot.py — the 'keep fixing it until it boots' loop controller.

Orchestrates existing tools; it owns no build logic of its own:

    titlebuild.py   -> measured manifest + verdict
    launch / log    -> a real boot (caller-supplied commands) or dry-run logs
    autofix.py      -> match failure signatures, apply REAL repairs
    titlebuild.py   -> rebuild -> verdict for the next attempt

Stops on: BOOTABLE, a terminal block (x86 / anti-cheat), no new fix for the
observed failures, or --max-attempts. Every attempt is recorded in
<game>/.tiramisu/autopilot.json with the evidence that drove it.

Real mode needs two shell commands:
  --launch-cmd '...'   starts the title (e.g. tools/deploy.sh launch ...)
  --log-cmd '...'      prints tiramisu.log (e.g. tools/devportal.py log ...)
Dry-run mode uses sequential log fixtures instead:
  --dry-run tests/fixtures/logs    (log.1, log.2, ...)
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def status(game_dir):
    p = run([sys.executable, os.path.join(HERE, 'titlebuild.py'), game_dir])
    out = p.stdout + p.stderr
    m = re.search(r'^verdict:\s*([\w-]+)', out, re.M)
    return (m.group(1).lower() if m else 'unknown'), out


def autofix(game_dir, log_text, attempt, db):
    # Write the observed log so the applied plan is auditable.
    meta = os.path.join(game_dir, '.tiramisu')
    os.makedirs(meta, exist_ok=True)
    log_path = os.path.join(meta, 'boot-%d.log' % attempt)
    with open(log_path, 'w') as f:
        f.write(log_text)
    p = run([sys.executable, os.path.join(HERE, 'autofix.py'), game_dir,
             log_path, '--apply', '--attempt', str(attempt)]
            + (['--db', db] if db else []))
    try:
        result = json.loads(p.stdout[p.stdout.index('{'):p.stdout.rindex('}') + 1])
    except Exception:
        result = {'matched_rules': [], 'raw': p.stdout[-500:]}
    return result


def terminal_block(game_dir):
    """A block no configuration change can repair."""
    mpath = os.path.join(game_dir, '.tiramisu', 'manifest.json')
    try:
        with open(mpath) as f:
            m = json.load(f)
    except Exception:
        return False
    return any(f.get('id') in ('anti-cheat-blocked', 'x86-blocked')
               for f in m.get('fixes_applied', []))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game_dir')
    ap.add_argument('--dry-run', metavar='LOG_DIR',
                    help='use log.N fixtures instead of a real console')
    ap.add_argument('--launch-cmd', default=None)
    ap.add_argument('--log-cmd', default=None)
    ap.add_argument('--max-attempts', type=int, default=5)
    ap.add_argument('--db', default=None)
    a = ap.parse_args(argv[1:])

    game_dir = os.path.abspath(a.game_dir)
    history = []

    for attempt in range(1, a.max_attempts + 1):
        verdict, out = status(game_dir)
        print('[attempt %d] verdict=%s' % (attempt, verdict.upper()))
        if verdict == 'bootable':
            print('SUCCESS: title is fully bound; boot evidence is the '
                  'console ladder, not this static verdict.')
            history.append({'attempt': attempt, 'verdict': verdict,
                            'result': 'success'})
            break

        if terminal_block(game_dir):
            print('STOP: terminal exclusion (x86-only or anti-cheat) — '
                  'no configuration repair exists.')
            history.append({'attempt': attempt, 'verdict': verdict,
                            'result': 'terminal-block'})
            break

        if a.dry_run:
            log_path = os.path.join(a.dry_run, 'log.%d' % attempt)
            if not os.path.exists(log_path):
                print('STOP: no log fixture %s' % log_path)
                history.append({'attempt': attempt, 'verdict': verdict,
                                'result': 'no-log'})
                break
            with open(log_path, errors='replace') as f:
                log_text = f.read()
        else:
            if not a.launch_cmd or not a.log_cmd:
                print('ERROR: real mode needs --launch-cmd and --log-cmd')
                return 2
            lr = run(a.launch_cmd, shell=True)
            print('  launch rc=%d' % lr.returncode)
            lg = run(a.log_cmd, shell=True)
            log_text = lg.stdout + lg.stderr

        result = autofix(game_dir, log_text, attempt, a.db)
        new = result.get('matched_rules', [])
        print('  matched: %s' % (', '.join(new) if new else '(none)'))
        history.append({'attempt': attempt, 'verdict': verdict,
                        'result': 'fix' if new else 'no-fix',
                        'matched_rules': new,
                        'profile': result.get('profile_overrides', {}),
                        'aliases': result.get('aliases', {})})
        if not new:
            print('STOP: no declared repair for the observed failure '
                  '(named work item, not hidden).')
            break
    else:
        history.append({'attempt': a.max_attempts, 'result': 'attempt-limit'})

    meta = os.path.join(game_dir, '.tiramisu')
    os.makedirs(meta, exist_ok=True)
    path = os.path.join(meta, 'autopilot.json')
    with open(path, 'w') as f:
        json.dump({'game': game_dir, 'history': history}, f, indent=2)
    print('wrote %s' % path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
