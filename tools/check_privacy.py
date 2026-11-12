#!/usr/bin/env python3
"""Check publishable files/history without printing matched private values."""

import argparse
import getpass
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess
from dataclasses import dataclass


ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    category: str


PATTERNS = (
    ("private-key", re.compile(r"-----BEGIN (?:[A-Z0-9 ]*PRIVATE KEY)-----")),
    ("access-token", re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{40,})\b")),
    ("cloud-key", re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b")),
    ("service-token", re.compile(r"\b(?:xox[baprs]-[A-Za-z0-9-]{20,}|sk-(?:proj-)?[A-Za-z0-9_-]{20,})\b")),
    ("credential-url", re.compile(r"\b(?:https?|ssh)://[^/\s:@]+:[^@\s/]+@")),
    ("credential-literal", re.compile(r"\b(?:password|passwd|api[_-]?key|access[_-]?token|token|secret)\b[\"']?\s*[:=]\s*[\"']([^\s\"']{8,})[\"']", re.I)),
    ("home-path", re.compile(r"/(?:home|Users|root)/[^\s\"'<>`]+")),
    ("home-path", re.compile(r"\b[A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/][^\s\\/]+")),
    ("local-domain", re.compile(r"\b[A-Za-z0-9][A-Za-z0-9.-]*\.(?:lan|local|internal)\b", re.I)),
    ("mac-address", re.compile(r"\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b")),
    ("public-key-identity", re.compile(r"\bssh-(?:rsa|ed25519)\s+[A-Za-z0-9+/=]{40,}")),
    ("literal-account", re.compile(r"\brunuser\s+-u\s+(?!user\b|USER\b|root\b)[A-Za-z][A-Za-z0-9_.-]*")),
)
EMAIL = re.compile(r"(?<![\w@])[\w.+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
IPV4 = re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])")
IPV6 = re.compile(r"(?<![\w:])[0-9A-Fa-f]*:[0-9A-Fa-f:]+(?![\w:])")
# Numeric networks keep the policy source free of literal private endpoints.
LOCAL_V4 = tuple(ipaddress.IPv4Network((base, prefix)) for base, prefix in (
    (0x0A000000, 8), (0xAC100000, 12), (0xC0A80000, 16),
    (0x7F000000, 8), (0xA9FE0000, 16), (0x64400000, 10),
))
LOCAL_V6 = (ipaddress.IPv6Network((0xFC << 120, 7)),
            ipaddress.IPv6Network((0xFE80 << 112, 10)))
SAFE_MAIL_DOMAINS = {"example.com", "example.org", "example.net", "users.noreply.github.com"}
SAFE_MAIL = {"git@github.com", "noreply@github.com"}
COMMON_ACCOUNTS = {"root", "runner", "user", "ubuntu", "admin", "nobody"}


def safe_email(value):
    domain = value.rsplit("@", 1)[-1].lower()
    return (value.lower() in SAFE_MAIL or domain in SAFE_MAIL_DOMAINS or
            domain.endswith((".example", ".invalid", ".test")))


def local_identities():
    user = getpass.getuser()
    return () if user.lower() in COMMON_ACCOUNTS else (user,)


def scan_text(path, text, identities=()):
    findings = set()

    def add(category, position=0):
        findings.add(Finding(path, text.count("\n", 0, position) + 1, category))

    for category, pattern in PATTERNS:
        for match in pattern.finditer(text):
            add(category, match.start())
    for match in EMAIL.finditer(text):
        if not safe_email(match.group()):
            add("personal-or-local-email", match.start())
    for match in IPV4.finditer(text):
        try:
            address = ipaddress.IPv4Address(match.group())
        except ValueError:
            continue
        if any(address in network for network in LOCAL_V4):
            add("local-network-address", match.start())
    for match in IPV6.finditer(text):
        try:
            address = ipaddress.IPv6Address(match.group())
        except ValueError:
            continue
        if any(address in network for network in LOCAL_V6):
            add("local-network-address", match.start())
    for identity in identities:
        pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(identity) + r"(?![A-Za-z0-9_])", re.I)
        for match in pattern.finditer(text):
            add("local-account-name", match.start())
    if path.endswith((".c", ".h")) and re.search(r"\bgethostname\s*\(", text):
        add("host-identity-collection")
    if path.endswith((".c", ".h")) and "CL_DEVICE_PCI_BUS_INFO_KHR" in text:
        add("physical-topology-collection")
    if path.endswith(".md"):
        topology = re.compile(r"第二台(?:主机|机器)|两台(?:主机|机器)|远端|本机|\bGPU_HOST\b|\b(?:remote|secondary|primary|second)\s+(?:host|machine|server)\b|^\s*(?:ssh|scp)\s+", re.I | re.M)
        for match in topology.finditer(text):
            add("deployment-topology-description", match.start())
        for match in re.finditer(r"\b[0-9a-fA-F]{4}:[0-9a-fA-F]{4}\b|\b[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]\b", text):
            add("physical-device-id", match.start())
    if path.endswith(".json"):
        try:
            data = json.loads(text)
        except ValueError:
            add("invalid-json")
        else:
            if isinstance(data, dict) and "schema_version" in data and "results" in data:
                if data.get("metadata_policy") != "minimal":
                    add("report-metadata-policy")
                for key in ("started_utc", "finished_utc", "hostname", "username", "cwd", "home"):
                    if key in data:
                        add("report-identity-or-calendar-time")
                if "host" in data or "system" in data:
                    add("report-environment-metadata")
                device = data.get("device") or {}
                if set(device) - {"backend", "int8_dot_extension", "capabilities"}:
                    add("report-device-environment")
                for key in ("uuid", "device_uuid", "driver_uuid", "luid", "serial", "serial_number", "mac_address"):
                    if key in device:
                        add("report-unique-device-identity")
                for capability in device.get("capabilities", {}).values():
                    if set(capability) - {"declared"}:
                        add("report-extra-device-properties")
    return sorted(findings, key=lambda item: (item.path, item.line, item.category))


def git(root, *arguments):
    environment = dict(os.environ, GIT_OPTIONAL_LOCKS="0")
    return subprocess.check_output(["git", "-C", str(root), *arguments], env=environment)


def scan_bytes(path, content, identities=(), publication=False):
    name_findings = scan_text("<filename>", path, identities)
    label = "<redacted-filename>" if name_findings else path
    findings = [Finding(label, 1, "filename-" + item.category) for item in name_findings]
    if b"\0" in content:
        return findings + [Finding(label, 1, "binary-file-requires-review")]
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError:
        return findings + [Finding(label, 1, "non-utf8-file-requires-review")]
    findings.extend(scan_text(label, text, identities))
    if publication and path.endswith(".json"):
        try:
            value = json.loads(text)
        except ValueError:
            pass
        else:
            if isinstance(value, dict) and "schema_version" in value and "results" in value:
                findings.append(Finding(label, 1, "runtime-report-is-local-only"))
    return findings


def scan_worktree(root, identities=()):
    names = git(root, "ls-files", "-c", "-o", "--exclude-standard", "-z").split(b"\0")
    findings = []
    for name in sorted(set(names) - {b""}):
        path = name.decode("utf-8")
        file = root / path
        if file.is_symlink():
            findings.append(Finding(path, 1, "symlink-requires-review"))
            continue
        if not file.is_file():
            continue
        findings.extend(scan_bytes(path, file.read_bytes(), identities, publication=True))
        if file.name.startswith(".env") and file.name != ".env.example":
            findings.append(Finding(path, 1, "environment-file"))
        if file.suffix in (".pem", ".key", ".p12", ".pfx") or file.name in ("id_rsa", "id_ed25519"):
            findings.append(Finding(path, 1, "credential-file"))
    return findings


def scan_history(root, revisions=(), identities=()):
    roots = list(revisions) if revisions else ["--all"]
    commits = git(root, "rev-list", *roots).decode().splitlines()
    findings = []
    scanned = set()
    for commit in commits:
        metadata = git(root, "show", "-s", "--format=%an <%ae>%n%cn <%ce>%n%B", commit).decode()
        findings.extend(scan_text(f"history/{commit[:12]}/metadata", metadata, identities))
        entries = git(root, "ls-tree", "-r", "-z", commit).split(b"\0")
        for entry in entries:
            if not entry:
                continue
            attributes, name = entry.split(b"\t", 1)
            mode, kind, oid = attributes.split()
            path = name.decode("utf-8")
            if kind != b"blob" or mode == b"120000":
                findings.append(Finding(f"history/{commit[:12]}/{path}", 1, "nonregular-entry-requires-review"))
                continue
            key = (oid, path)
            if key in scanned:
                continue
            scanned.add(key)
            findings.extend(scan_bytes(f"history/{commit[:12]}/{path}", git(root, "cat-file", "blob", oid.decode()), identities, publication=True))
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--history", action="store_true", help="Also inspect reachable commit contents and author metadata")
    parser.add_argument("--revision", action="append", default=[], help="History root to inspect instead of all refs")
    parser.add_argument("--files", type=Path, nargs="+", help="Inspect explicit publication candidates, including ignored files")
    args = parser.parse_args()
    root = args.root.resolve()
    identities = local_identities()
    if args.files:
        findings = []
        for path in args.files:
            findings.extend(scan_bytes(path.name, path.read_bytes(), identities))
    else:
        findings = scan_worktree(root, identities)
    if args.history:
        findings.extend(scan_history(root, args.revision, identities))
    for finding in sorted(set(findings), key=lambda item: (item.path, item.line, item.category)):
        print(f"{finding.path}:{finding.line}: {finding.category}")
    if findings:
        print(f"FAIL: {len(set(findings))} privacy findings; matched values are intentionally not printed")
        raise SystemExit(1)
    print("PASS: no policy findings in the inspected publication scope")


if __name__ == "__main__":
    main()
