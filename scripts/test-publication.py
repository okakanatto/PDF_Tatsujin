"""Regression tests for publication exclusions and exact staged content."""

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "publication", Path(__file__).with_name("check-publication.py")
)
publication = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publication)


class PublicationTests(unittest.TestCase):
    def test_private_paths(self):
        for path in (
            "AGENTS.md",
            "docs/HANDOFF_example.md",
            "evidence/result.json",
            "keys/private.key",
            ".env",
            "docs/development-restart-state.json",
        ):
            with self.subTest(path=path):
                self.assertTrue(publication.inspect(path, b"{}", {}))

    def test_prose_and_nested_operations(self):
        self.assertTrue(
            publication.inspect(
                "README.md", "利用者が許可した容量を記録します。".encode(), {}
            )
        )
        self.assertTrue(
            publication.inspect(
                "docs/testing/result.json",
                b'{"a": [{"owner_thread_id": "example"}]}',
                {},
            )
        )

    def test_secrets_are_not_echoed(self):
        data = ("gh" + "p_" + "a" * 36).encode()
        self.assertEqual(
            publication.inspect("src/example.cpp", data, {}), ["access token"]
        )

    def test_operational_context_in_english_or_quoted_request(self):
        self.assertTrue(
            publication.inspect(
                "docs/testing/result.json",
                b'{"parent_status_observed": "model at capacity"}',
                {},
            )
        )
        self.assertTrue(
            publication.inspect(
                "docs/design/example.md",
                "ユーザーの「内部作業を続けて」という依頼に従います。".encode(),
                {},
            )
        )

    def test_personal_path_in_encoded_json(self):
        data = json.dumps({"path": "C:" + "\\Users\\example\\document.pdf"}).encode()
        self.assertIn(
            "personal machine path",
            publication.inspect("docs/testing/result.json", data, {}),
        )

    def test_non_utf8_public_text_is_rejected(self):
        for extension in (".md", ".txt", ".json", ".py", ".cpp"):
            with self.subTest(extension=extension):
                self.assertIn(
                    "text content must use UTF-8",
                    publication.inspect(
                        "docs/example" + extension,
                        "private text".encode("utf-16"),
                        {},
                    ),
                )

    def test_operations_in_public_prose_formats(self):
        for extension in (".txt", ".html", ".xml", ".yaml", ".yml"):
            with self.subTest(extension=extension):
                self.assertIn(
                    "internal operations in prose",
                    publication.inspect(
                        "docs/example" + extension, b"owner_thread_id", {}
                    ),
                )
                self.assertFalse(
                    publication.inspect(
                        "docs/example" + extension, b"PDF viewer with local OCR.", {}
                    )
                )

    def test_fixture_json_operations_and_unchanged_thresholds(self):
        self.assertIn(
            "internal record field",
            publication.inspect(
                "fixtures/example.json",
                b'{"a": [{"owner_thread_id": "example"}]}',
                {},
            ),
        )
        self.assertFalse(
            publication.inspect(
                "fixtures/example.json",
                b'{"thresholds": {"CER_jpn": 0.02, "CER_eng": 0.01, "position_mm": 2}, "cvt_profile": "storage"}',
                {},
            )
        )

    def test_public_technical_results(self):
        self.assertFalse(
            publication.inspect(
                "docs/testing/result.json",
                b'{"placement_error_pt": 0.12000000000000455, "UIAutomationCore.DLL": "Windows:System32"}',
                {},
            )
        )
        self.assertFalse(
            publication.inspect(
                "README.md",
                "PDFはローカルで処理します。OCRには誤認識が残ります。".encode(),
                {},
            )
        )

    def test_new_and_changed_binary(self):
        data = b"\x89PNG\0synthetic"
        path = "fixtures/image.png"
        self.assertTrue(publication.inspect(path, data, {}))
        approved = {
            path: {
                "sha256": hashlib.sha256(data).hexdigest(),
                "origin": "Synthetic test image",
            }
        }
        self.assertFalse(publication.inspect(path, data, approved))
        self.assertTrue(publication.inspect(path, data + b"changed", approved))

    def test_staged_bytes_and_deleted_file(self):
        original_root = publication.ROOT
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            publication.ROOT = root
            try:
                subprocess.run(["git", "init", "-q"], cwd=root, check=True)
                (root / "README.md").write_text("PDF viewer.\n", encoding="utf8")
                subprocess.run(["git", "add", "README.md"], cwd=root, check=True)
                (root / "README.md").write_text("owner_thread_id\n", encoding="utf8")
                self.assertFalse(publication.check(publication.staged()))
                subprocess.run(["git", "add", "README.md"], cwd=root, check=True)
                self.assertTrue(publication.check(publication.staged()))
                subprocess.run(
                    ["git", "rm", "--cached", "-q", "README.md"], cwd=root, check=True
                )
                self.assertNotIn("README.md", publication.staged())
            finally:
                publication.ROOT = original_root


if __name__ == "__main__":
    unittest.main()
