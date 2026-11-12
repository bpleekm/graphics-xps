import ipaddress
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import check_privacy as privacy


class PrivacyTests(unittest.TestCase):
    def categories(self, text, path="sample.txt", identities=()):
        return {item.category for item in privacy.scan_text(path, text, identities)}

    def test_private_networks_and_documentation(self):
        address = str(ipaddress.IPv4Address(0x0A172D43))
        self.assertIn("local-network-address", self.categories(address))
        ipv6 = str(ipaddress.IPv6Address((0xFD << 120) + 7))
        self.assertIn("local-network-address", self.categories(ipv6))
        self.assertFalse(self.categories("ssh user@gpu-host.example; git@github.com:owner/project.git"))

    def test_home_accounts_and_email(self):
        home = "/".join(("", "home", "sampleuser", "project"))
        self.assertIn("home-path", self.categories(home))
        self.assertIn("local-account-name", self.categories("account sampleuser", identities=("sampleuser",)))
        email = "someone@" + "workstation" + ".lan"
        self.assertIn("personal-or-local-email", self.categories(email))
        self.assertFalse(self.categories("owner@users.noreply.github.com"))

    def test_credentials_without_echoing_values(self):
        token = "gh" + "p_" + "A" * 36
        findings = privacy.scan_text("sample.txt", token)
        self.assertIn("access-token", {item.category for item in findings})
        self.assertNotIn(token, repr(findings))
        pem = "-----BEGIN " + "PRIVATE KEY-----"
        self.assertIn("private-key", self.categories(pem))
        url = "https://" + "user:sample-password@" + "host.example/"
        self.assertIn("credential-url", self.categories(url))
        assignment = "pass" + "word=" + "'sample-secret-value'"
        self.assertIn("credential-literal", self.categories(assignment))

    def test_sensitive_filename_is_not_echoed(self):
        path = "docs/" + "sampleuser" + "/report.txt"
        findings = privacy.scan_bytes(path, b"generic data", identities=("sampleuser",))
        self.assertTrue(any(item.category == "filename-local-account-name" for item in findings))
        self.assertNotIn("sampleuser", repr(findings))

    def test_report_metadata_policy(self):
        report = {"schema_version": 1, "results": [], "metadata_policy": "minimal",
                  "resources": {"peak_process_rss_bytes": 1},
                  "device": {"backend": "opencl", "capabilities": {}}}
        self.assertFalse(self.categories(json.dumps(report), "sample.json"))
        self.assertTrue(any(item.category == "runtime-report-is-local-only" for item in
                            privacy.scan_bytes("docs/report.json", json.dumps(report).encode(), publication=True)))
        report["host"] = {"name": "sample-machine"}
        self.assertIn("report-environment-metadata", self.categories(json.dumps(report), "sample.json"))
        del report["host"]
        report["started_utc"] = "example-calendar-value"
        self.assertIn("report-identity-or-calendar-time", self.categories(json.dumps(report), "sample.json"))

    def test_documentation_omits_deployment_even_with_placeholders(self):
        text = "ssh user@gpu-host.example 'run'"
        self.assertIn("deployment-topology-description", self.categories(text, "README.md"))
        device_id = "1234" + ":" + "abcd"
        self.assertIn("physical-device-id", self.categories(device_id, "README.md"))

    def test_history_catches_deleted_content(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(["git", "-C", str(root), *args], stderr=subprocess.DEVNULL)
            git("init", "-q")
            address = str(ipaddress.IPv4Address(0xC0A8014D))
            (root / "notes.md").write_text("target=" + address)
            git("add", "notes.md")
            import os
            environment = dict(os.environ, GIT_AUTHOR_NAME="Example", GIT_COMMITTER_NAME="Example",
                               GIT_AUTHOR_EMAIL="example@users.noreply.github.com",
                               GIT_COMMITTER_EMAIL="example@users.noreply.github.com")
            subprocess.check_call(["git", "-C", str(root), "commit", "-qm", "initial"], env=environment)
            (root / "notes.md").write_text("target=user@gpu-host.example")
            git("add", "notes.md")
            subprocess.check_call(["git", "-C", str(root), "commit", "-qm", "generic example"], env=environment)
            self.assertFalse(privacy.scan_worktree(root))
            self.assertTrue(any(item.category == "local-network-address" for item in privacy.scan_history(root)))


if __name__ == "__main__":
    unittest.main()
