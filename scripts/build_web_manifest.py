#!/usr/bin/env python3
"""Stage firmware release binaries for the web flasher and write its manifest.

GitHub release assets redirect to release-assets.githubusercontent.com, which
sends no Access-Control-Allow-Origin header, so a browser cannot fetch them
directly. The Pages deploy therefore copies the newest assets into web/firmware/
alongside a manifest the flasher reads.

Run from the repository root with the `gh` CLI authenticated:

    python3 scripts/build_web_manifest.py
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = os.environ.get("GITHUB_REPOSITORY", "StuckAtPrototype/AirCube")
OUT_DIR = Path("web/firmware")
ASSET_PATTERN = re.compile(r"^AirCube_firmware_v[\d.]+\.bin$")
VERSION_PATTERN = re.compile(r"\d+(?:\.\d+)*")
KEEP_RELEASES = 5
FLASH_ADDRESS = "0x0"


def version_of(tag: str, filename: str) -> str:
    """Releases have been tagged Release_v2.0.1, Release_V1.4.3 and Release_1.4.2,
    and one asset shipped as v.1.4.3, so read the digits and drop the decoration.
    """
    match = VERSION_PATTERN.search(tag) or VERSION_PATTERN.search(filename)
    return match.group(0) if match else tag


def gh(*args: str) -> str:
    result = subprocess.run(
        ("gh", *args), capture_output=True, text=True, check=True)
    return result.stdout


def fetch_releases() -> list[dict]:
    payload = json.loads(gh("api", f"repos/{REPO}/releases?per_page=30"))
    releases = []
    for release in payload:
        if release.get("draft"):
            continue
        asset = next(
            (a for a in release.get("assets", []) if ASSET_PATTERN.match(a["name"])),
            None,
        )
        if asset is None:
            continue
        releases.append({
            "tag": release["tag_name"],
            "version": version_of(release["tag_name"], asset["name"]),
            "file": asset["name"],
            "size": asset["size"],
            "prerelease": bool(release.get("prerelease")),
            "publishedAt": release.get("published_at") or "",
            "notesUrl": release.get("html_url", ""),
        })
    releases.sort(key=lambda r: r["publishedAt"], reverse=True)
    return releases[:KEEP_RELEASES]


def download(entry: dict) -> Path:
    gh("release", "download", entry["tag"],
       "--repo", REPO,
       "--pattern", entry["file"],
       "--dir", str(OUT_DIR),
       "--clobber")
    return OUT_DIR / entry["file"]


def write_manifest(entries: list[dict]) -> None:
    latest = next((e["version"] for e in entries if not e["prerelease"]), None)
    if latest is None and entries:
        latest = entries[0]["version"]

    manifest = {
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "latest": latest,
        "releases": entries,
    }
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    try:
        releases = fetch_releases()
    except (subprocess.CalledProcessError, FileNotFoundError, json.JSONDecodeError) as err:
        # A missing catalog is not fatal: the flasher still accepts a local
        # .bin file, and we would rather ship the site than fail the deploy.
        print(f"warning: could not list releases ({err})", file=sys.stderr)
        write_manifest([])
        return 0

    entries = []
    for release in releases:
        path = download(release)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        entries.append({
            "version": release["version"],
            "tag": release["tag"],
            "file": release["file"],
            "size": path.stat().st_size,
            "sha256": digest,
            "address": FLASH_ADDRESS,
            "prerelease": release["prerelease"],
            "publishedAt": release["publishedAt"],
            "notesUrl": release["notesUrl"],
        })
        print(f"staged {release['file']} ({path.stat().st_size} bytes)")

    write_manifest(entries)
    print(f"wrote {OUT_DIR / 'manifest.json'} with {len(entries)} release(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
