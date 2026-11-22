#!/usr/bin/env python3
"""Generate the "Measured" table of docs/title-matrix.md from evidence.

    python3 tools/title-matrix.py            # rewrite the table
    python3 tools/title-matrix.py --check    # exit 1 if the doc is stale

Two inputs, and no third:

  * docs/proof/titles.json - one entry per title the repo can back with
    artifacts. The curated columns (build, audio, input, playable) live here
    because only a human watching the TV can write them down.
  * docs/proof/<name>/proof.json - written by `tools/console.py proof`. When it
    exists it wins over the curated entry for the machine columns (machine,
    console OS build, screenshot, log), so a row cannot drift from the run that
    produced it.

A row whose evidence files are missing is a hard error: the contract at the top
of the matrix ("a row exists only if an artifact exists") is enforced here
rather than trusted to a reviewer.
"""

import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(REPO, "docs", "proof", "titles.json")
DOC = os.path.join(REPO, "docs", "title-matrix.md")
BEGIN = "<!-- title-matrix:measured:begin -->"
END = "<!-- title-matrix:measured:end -->"

HEADER = ("| Title | Build | Machine | Boots | Renders | Audio | Input | "
          "Playable | Evidence |")
RULE = "|---|---|---|---|---|---|---|---|---|"


def load_manifest():
    with open(MANIFEST, encoding="utf-8") as handle:
        return json.load(handle)


def proof_for(name):
    path = os.path.join(REPO, "docs", "proof", name, "proof.json")
    if not os.path.isfile(path):
        return None, None
    with open(path, encoding="utf-8") as handle:
        return json.load(handle), f"docs/proof/{name}/proof.json"


def resolve(row):
    """Merge the curated row with any machine record that exists for it."""
    name = row["title"]
    record, record_path = proof_for(row.get("dir") or name.replace(" ", "-"))
    merged = dict(row)
    evidence = list(row.get("evidence", []))
    notes = []
    if record:
        merged["machine"] = record.get("machine") or merged.get("machine", "?")
        if record.get("screenshot"):
            shot = f"docs/proof/{row.get('dir') or name}/{record['screenshot']}"
            if shot not in evidence:
                evidence.insert(0, shot)
        for log in record.get("logs", []):
            entry = f"docs/proof/{row.get('dir') or name}/{log}"
            if entry not in evidence:
                evidence.append(entry)
        merged["boots"] = "yes"
        detail = []
        if record.get("console"):
            detail.append(f"console `{record['console']}`")
        if record.get("os_version"):
            detail.append(f"OS `{record['os_version']}`")
        if record.get("exe_sha256"):
            detail.append(f"exe `{str(record['exe_sha256'])[:12]}`")
        if record.get("captured_at"):
            detail.append(record["captured_at"])
        if detail:
            notes.append((name, record_path, "; ".join(detail)))
    merged["evidence"] = evidence
    return merged, notes


def render(rows, notes):
    lines = [BEGIN, HEADER, RULE]
    for row in rows:
        cells = [
            row["title"],
            row.get("build", "?"),
            row["machine"],
            row.get("boots", "?"),
            row.get("renders", "?"),
            row.get("audio", "?"),
            row.get("input", "?"),
            row.get("playable", "?"),
            ", ".join(f"`{path}`" for path in row["evidence"]) or "**none**",
        ]
        lines.append("| " + " | ".join(cells) + " |")
    lines.append(END)
    if notes:
        lines.append("")
        lines.append("Machine records behind those rows (`proof.json`):")
        lines.append("")
        for name, path, detail in notes:
            lines.append(f"- {name} — `{path}`: {detail}")
    return "\n".join(lines)


def splice(document, block):
    if BEGIN not in document or END not in document:
        sys.exit(f"ERROR: {DOC} has no {BEGIN} / {END} markers")
    head, _, rest = document.partition(BEGIN)
    _, _, tail = rest.partition(END)
    return head + block + tail


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="report drift instead of writing the file")
    args = parser.parse_args(argv)

    manifest = load_manifest()
    rows, notes, problems = [], [], []

    for row in manifest["titles"]:
        merged, row_notes = resolve(row)
        rows.append(merged)
        notes.extend(row_notes)

    for row in rows:
        if not row["evidence"]:
            problems.append(f"{row['title']}: no evidence artifact")
        for path in row["evidence"]:
            if not os.path.isfile(os.path.join(REPO, path)):
                problems.append(f"{row['title']}: missing {path}")

    listed = {(row.get("dir") or row["title"]) for row in manifest["titles"]}
    proof_root = os.path.join(REPO, "docs", "proof")
    for name in sorted(os.listdir(proof_root)):
        folder = os.path.join(proof_root, name)
        if not os.path.isdir(folder):
            continue
        if os.path.isfile(os.path.join(folder, "proof.json")) and \
                name not in listed:
            problems.append(
                f"docs/proof/{name}/proof.json has no row in "
                f"docs/proof/titles.json (add one, or delete the run)")

    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        return 1

    with open(DOC, encoding="utf-8") as handle:
        document = handle.read()
    updated = splice(document, render(rows, notes))

    if args.check:
        if updated != document:
            print("STALE: docs/title-matrix.md does not match the evidence; "
                  "run tools/title-matrix.py", file=sys.stderr)
            return 1
        print(f"title matrix: {len(rows)} rows, all evidence present")
        return 0

    if updated != document:
        with open(DOC, "w", encoding="utf-8") as handle:
            handle.write(updated)
        print(f"wrote {DOC} ({len(rows)} rows)")
    else:
        print(f"{DOC} already current ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
