#!/usr/bin/env python3
"""Check publishable source files and optional APKs for accidental private data.

Run from anywhere: python tools/audit-public-release.py --apk path/to/release.apk
Use --forbid-string for an additional private identifier without putting it here.
Output contains rule names and locations, never matched values. This is a focused
release check, not a replacement for reviewing the export and asset licensing.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile


SKIP_DIRS = {".git", ".gradle", ".cxx", "__pycache__", "artifacts", "dist", "build"}
PRIVATE_EXTENSIONS = {
    ".iso", ".gcm", ".ciso", ".rvz", ".gcz", ".wbfs", ".wad", ".nsp", ".xci",
    ".sav", ".gcp", ".raw", ".dtm", ".dmp", ".keystore", ".jks", ".p12", ".pfx",
    ".pyc", ".pyo",
}
PRIVATE_NAMES = {"local.properties", "credentials.json", "google-services.json", "temp.sav"}
FILE_FIXTURES = {
    "Externals/mGBA/mgba/src/third-party/zlib/contrib/puff/zeros.raw",
    "Externals/minizip-ng/minizip-ng/test/test.p12",
}
# These are public upstream examples/test certificates or PEM parser/writer literals.
# Do not expand this list to cover a new finding without inspecting that file.
PEM_FIXTURES = {
    "Externals/curl/curl/docs/examples/usercertinmem.c",
    "Externals/mbedtls/library/certs.c",
    "Externals/mbedtls/library/pkparse.c",
    "Externals/mbedtls/library/pkwrite.c",
    "Externals/minizip-ng/minizip-ng/test/test.pem",
}
TOKEN_FIXTURES = {"Externals/zlib-ng/zlib-ng/test/CVE-2018-25032/fixed.txt"}
TOKEN_PATTERN = re.compile(
    rb"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|"
    rb"sk-(?:proj-)?[A-Za-z0-9_-]{30,}|AKIA[0-9A-Z]{16})"
)
PEM_PATTERN = re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----")
# Delimiter strings alone are normal in a crypto library. In an APK, require a
# following base64 body before treating a PEM delimiter as embedded private data.
PEM_BODY_PATTERN = re.compile(
    rb"-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----[\r\n]+"
    rb"(?:[A-Za-z0-9+/=]{16,}[\r\n]+){2,}"
)
WINDOWS_HOME = re.compile(
    rb"[A-Za-z]:[/\\]Users[/\\](?![<$%])"
    rb"(?!(?:user|username|you|example|public|default)(?:[/\\]|$))[A-Za-z0-9_. -]+[/\\]",
    re.IGNORECASE,
)
UNIX_HOME = re.compile(
    rb"(?:/home/|/Users/)(?![<$%])"
    rb"(?!(?:user|username|you|example|runner|build|builder|test)(?:/|$))[A-Za-z0-9_.-]+/"
)
DEVELOPMENT_PATH = re.compile(rb"[A-Za-z]:[/\\]Development[/\\]Games(?:[/\\]|$)", re.IGNORECASE)
ABSOLUTE_BUILD_PATH = re.compile(
    rb"(?:[A-Za-z]:[/\\][^\x00\r\n]{0,160}[/\\](?:Source|Externals)[/\\]|"
    rb"/(?:home|Users)/[^\x00\r\n]{0,160}/(?:Source|Externals)/)"
)


def source_files(root: Path) -> tuple[list[Path], str]:
    """Prefer precisely the tracked and unignored publication candidates."""
    if not root.is_dir():
        raise FileNotFoundError("Source directory does not exist")
    if (root / ".git").exists():
        try:
            result = subprocess.run(
                ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            names = sorted(set(os.fsdecode(p) for p in result.stdout.split(b"\0") if p))
            return [root / name for name in names if (root / name).is_file()], "git publication candidates"
        except (OSError, subprocess.CalledProcessError) as error:
            raise RuntimeError("Git candidate enumeration failed; refusing an incomplete audit") from error
    result = []
    for directory, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        result.extend(Path(directory) / name for name in sorted(files))
    return result, "export tree (generated output directories excluded)"


def filename_issue(name: str, apk: bool = False) -> bool:
    path = Path(name)
    lower = path.name.lower()
    if name in FILE_FIXTURES and not apk:
        return False
    if path.suffix.lower() in PRIVATE_EXTENSIONS or lower in PRIVATE_NAMES:
        return True
    if lower == ".env" or (lower.startswith(".env.") and lower not in {".env.example", ".env.sample"}):
        return True
    if lower.startswith(("logcat", "bugreport-", "tombstone_")):
        return True
    if not apk and path.suffix.lower() in {".apk", ".aab", ".dex"}:
        return True
    if apk and any(part.lower() in {"statesaves", "screenshots", "dumps", "logs"} for part in path.parts):
        return True
    if apk and name.lower().startswith(("assets/config/", "assets/gc/", "assets/user/config/", "assets/user/gc/")):
        return True
    return False


def content_issues(data: bytes, name: str, needles: list[bytes], apk: bool = False) -> list[tuple[str, int]]:
    issues = []
    if (data.startswith((b"CISO", b"RVZ\x01", b"WBFS")) or
            data[0x1C:0x20] == b"\xc2\x33\x9f\x3d" or
            data[0x18:0x1C] == b"\x5d\x1c\x9e\xa3"):
        issues.append(("disc-image signature", 1))
    rules = [("credential-shaped value", TOKEN_PATTERN)]
    if apk:
        rules += [("private key body", PEM_BODY_PATTERN), ("absolute build path", ABSOLUTE_BUILD_PATH)]
    else:
        rules += [("private key delimiter outside reviewed fixtures", PEM_PATTERN)]
    # Vendored source contains public documentation/test paths. Additional private
    # identifiers still apply to every byte, including those upstream files.
    if apk or not name.startswith("Externals/"):
        rules += [("hardcoded user home", WINDOWS_HOME), ("hardcoded user home", UNIX_HOME)]
    rules += [("local development workspace", DEVELOPMENT_PATH)]
    for label, pattern in rules:
        if not apk and ((pattern is PEM_PATTERN and name in PEM_FIXTURES) or
                        (pattern is TOKEN_PATTERN and name in TOKEN_FIXTURES)):
            continue
        match = pattern.search(data)
        if match:
            issues.append((label, data.count(b"\n", 0, match.start()) + 1))
    lower_data = data.lower()
    for needle in needles:
        position = lower_data.find(needle.lower())
        if position >= 0:
            issues.append(("caller-supplied private identifier", data.count(b"\n", 0, position) + 1))
    return issues


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--apk", type=Path, action="append", default=[])
    parser.add_argument("--forbid-string", action="append", default=[], help="additional private value; never echoed")
    parser.add_argument("--json", action="store_true", help="emit a machine-readable report")
    args = parser.parse_args()
    root = args.root.resolve()
    findings = []
    needles = [value.encode(encoding) for value in args.forbid_string if value
               for encoding in ("utf-8", "utf-16-le")]
    try:
        paths, mode = source_files(root)
        if not paths:
            raise RuntimeError("No publication candidates found")
        for path in paths:
            name = path.relative_to(root).as_posix()
            if filename_issue(name):
                findings.append({"file": name, "rule": "private/generated filename"})
            for rule, line in content_issues(path.read_bytes(), name, needles):
                findings.append({"file": name, "rule": rule, "line": line})
        apks = []
        for number, path in enumerate(args.apk, 1):
            # An index avoids accidentally printing a private APK parent directory.
            label = f"APK {number}"
            with zipfile.ZipFile(path) as archive:
                entries = archive.infolist()
                for entry in entries:
                    if entry.is_dir():
                        continue
                    location = f"{label}:{entry.filename}"
                    if filename_issue(entry.filename, apk=True):
                        findings.append({"file": location, "rule": "private/game-data APK filename"})
                    for rule, _ in content_issues(archive.read(entry), entry.filename, needles, apk=True):
                        findings.append({"file": location, "rule": rule})
                apks.append({"label": label, "entries": len(entries)})
        report = {"passed": not findings, "source_mode": mode, "source_files": len(paths),
                  "apks": apks, "findings": findings}
    except (OSError, RuntimeError, zipfile.BadZipFile) as error:
        # Error text can contain private paths; report only the failure type.
        report = {"passed": False, "error": type(error).__name__,
                  "message": "Audit could not finish; check input paths and read permissions."}
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print("PASS" if report["passed"] else "FAIL")
        print(f"Source files: {report.get('source_files', 0)}; APKs: {len(report.get('apks', []))}")
        for finding in report.get("findings", []):
            line = f":{finding['line']}" if "line" in finding else ""
            print(f"{finding['file']}{line}: {finding['rule']}")
        if "message" in report:
            print(report["message"])
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
