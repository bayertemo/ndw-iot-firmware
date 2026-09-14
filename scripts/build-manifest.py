#!/usr/bin/env python3
"""Assemble manifest.json from every builds/**/build.json.

The console fetches this one file to populate its firmware picker, so it is
the contract between this repo and the flash page. Two things it guarantees
that a hand-written manifest would not:

  - Every part named by a build actually exists here. A manifest advertising
    a binary that was never committed fails in a technician's browser, with a
    board already half-open on the bench.
  - Every part carries its real size and SHA-256. The console checks the size
    it received against the manifest before writing anything, which catches a
    truncated download — the failure that would otherwise be discovered as an
    unbootable board.

A build whose binaries are missing is still published, marked unpublished, so
the picker can list it greyed out rather than pretending it does not exist.
That is the state every build is in until firmware is actually produced.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import sys
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILDS = ROOT / "builds"
MANIFEST = ROOT / "manifest.json"

# Bumped only when the shape changes incompatibly. The console refuses a
# schema it does not know rather than guessing at unfamiliar fields.
SCHEMA = 1

REQUIRED = ("kind", "name", "role", "chipMatch", "parts")
ROLES = {"gateway", "sensor"}


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def digest(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def load(build_file: pathlib.Path) -> dict[str, object]:
    rel = build_file.parent.relative_to(ROOT).as_posix()
    try:
        doc = json.loads(build_file.read_text())
    except json.JSONDecodeError as err:
        fail(f"{rel}/build.json is not valid JSON: {err}")

    for field in REQUIRED:
        if field not in doc:
            fail(f"{rel}/build.json is missing '{field}'")

    if doc["role"] not in ROLES:
        fail(f"{rel}: role must be one of {sorted(ROLES)}, got {doc['role']!r}")

    # The kind doubles as the path, so a mismatch would make the console fetch
    # binaries from somewhere the build does not live.
    expected_kind = build_file.parent.relative_to(BUILDS).as_posix()
    if doc["kind"] != expected_kind:
        fail(f"{rel}: kind is {doc['kind']!r} but its path says {expected_kind!r}")

    # Compiled here so a bad pattern fails the build rather than throwing in
    # the browser, where it would break the whole picker rather than one row.
    try:
        re.compile(str(doc["chipMatch"]))
    except re.error as err:
        fail(f"{rel}: chipMatch is not a valid regex: {err}")

    parts: list[dict[str, object]] = []
    published = True
    seen: set[int] = set()

    for part in doc["parts"]:
        if "path" not in part or "address" not in part:
            fail(f"{rel}: every part needs 'path' and 'address'")

        address = int(part["address"])
        if address in seen:
            # Two images at one offset means the second silently overwrites
            # the first, and the board fails to boot for no visible reason.
            fail(f"{rel}: two parts share address 0x{address:x}")
        seen.add(address)

        binary = build_file.parent / str(part["path"])
        entry: dict[str, object] = {
            "path": f"{expected_kind}/{part['path']}",
            "address": address,
        }

        if binary.is_file():
            entry["size"] = binary.stat().st_size
            entry["sha256"] = digest(binary)
        else:
            published = False

        parts.append(entry)

    return {
        "kind": doc["kind"],
        "name": doc["name"],
        "role": doc["role"],
        "description": doc.get("description", ""),
        "chipMatch": doc["chipMatch"],
        "chipLabel": doc.get("chipLabel", ""),
        "revision": doc.get("revision", "0.0.0"),
        "published": published,
        "parts": parts,
    }


def main() -> None:
    if not BUILDS.is_dir():
        fail("no builds/ directory")

    builds = [load(f) for f in sorted(BUILDS.rglob("build.json"))]
    if not builds:
        fail("no builds/**/build.json found")

    kinds = [b["kind"] for b in builds]
    duplicates = {k for k in kinds if kinds.count(k) > 1}
    if duplicates:
        fail(f"duplicate kinds: {sorted(duplicates)}")

    # Two builds of the same role claiming one chip leaves the console unable
    # to choose, so it is caught here rather than presented as a coin flip.
    for role in sorted(ROLES):
        same = [b for b in builds if b["role"] == role]
        for i, a in enumerate(same):
            for b in same[i + 1 :]:
                if a["chipMatch"] == b["chipMatch"]:
                    fail(
                        f"{a['kind']} and {b['kind']} are both '{role}' and "
                        f"match the same chips"
                    )

    MANIFEST.write_text(
        json.dumps(
            {
                "$comment": (
                    "Generated by scripts/build-manifest.py. Edit "
                    "builds/**/build.json instead; changes here are overwritten."
                ),
                "schema": SCHEMA,
                "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "builds": builds,
            },
            indent=2,
        )
        + "\n"
    )

    ready = sum(1 for b in builds if b["published"])
    print(f"manifest.json: {len(builds)} builds, {ready} with binaries")


if __name__ == "__main__":
    main()
