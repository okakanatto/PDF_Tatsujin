"""Regression tests for publication exclusions and exact staged content."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "publication", Path(__file__).with_name("check-publication.py")
)
publication = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publication)

TEST_DIRECTORY = Path(__file__).resolve().parents[1] / "build" / "publication-check-tests"


@contextlib.contextmanager
def owned_temporary_directory():
    TEST_DIRECTORY.mkdir(parents=True, exist_ok=True)
    if TEST_DIRECTORY.resolve() != TEST_DIRECTORY.absolute():
        raise RuntimeError("Test parent must not redirect outside the workspace")
    with tempfile.TemporaryDirectory(dir=TEST_DIRECTORY, prefix="case-") as directory:
        root = Path(directory).resolve()
        if not root.is_relative_to(TEST_DIRECTORY.resolve()):
            raise RuntimeError("Test directory escaped its owned parent")
        yield root


class PublicationTests(unittest.TestCase):
    def test_source_environment_notes(self):
        note = ("shell" + " DNS" + " unavailable").encode()
        for suffix in (".py", ".cpp", ".ps1"):
            with self.subTest(suffix=suffix):
                self.assertIn(
                    "internal environment details",
                    publication.inspect("scripts/example" + suffix, note, {}),
                )

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
        with owned_temporary_directory() as root:
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

    def test_upstream_email_credit_remains_legal_content(self):
        self.assertFalse(
            publication.inspect(
                "licenses/UPSTREAM.txt", b"Copyright Example <upstream@gmail.com>\n", {}
            )
        )


class CommitIdentityTests(unittest.TestCase):
    """Real Git objects exercise complete-history checks without publishing."""

    PUBLIC = "12345+example@users.noreply.github.com"

    def setUp(self):
        self.scope = owned_temporary_directory()
        self.root = self.scope.__enter__()
        self.original_root = publication.ROOT
        publication.ROOT = self.root
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        self.tree = self.git("mktree", data=b"").decode().strip()

    def tearDown(self):
        publication.ROOT = self.original_root
        self.scope.__exit__(None, None, None)

    def git(self, *args, data=None):
        return subprocess.check_output(["git", *args], cwd=self.root, input=data)

    def commit(
        self, *, author=None, committer=None, parent=None,
        message="PDF update.", author_name="Example"
    ):
        headers = ["tree " + self.tree]
        if parent:
            headers.append("parent " + parent)
        headers += [
            f"author {author_name} <{author or self.PUBLIC}> 1700000000 +0000",
            f"committer Example <{committer or self.PUBLIC}> 1700000000 +0000",
        ]
        data = ("\n".join(headers) + "\n\n" + message + "\n").encode()
        # Plumbing creates fixtures only; no hooks are installed or bypassed.
        sha = self.git("hash-object", "-t", "commit", "-w", "--stdin", data=data).decode().strip()
        self.git("update-ref", "refs/heads/main", sha)
        return sha

    def tag(self, target, *, tagger=None, name="example", message="PDF release."):
        kind = self.git("cat-file", "-t", target).decode().strip()
        data = (
            f"object {target}\ntype {kind}\ntag {name}\n"
            f"tagger Example <{tagger or self.PUBLIC}> 1700000000 +0000\n\n"
            + message + "\n"
        ).encode()
        sha = self.git("hash-object", "-t", "tag", "-w", "--stdin", data=data).decode().strip()
        self.git("update-ref", "refs/tags/" + name, sha)
        return sha

    def check(self, *args, outgoing=""):
        output = io.StringIO()
        with patch.object(sys, "argv", ["check-publication.py", *args]), patch.object(
            sys, "stdin", io.StringIO(outgoing)
        ), contextlib.redirect_stdout(output):
            status = publication.main()
        return status, output.getvalue()

    def test_noreply_and_official_web_commits_pass(self):
        first = self.commit()
        self.commit(author="noreply@github.com", committer="noreply@github.com", parent=first)
        self.assertEqual(self.check("--ref", "main")[0], 0)

    def test_outgoing_checks_private_author_in_middle_commit(self):
        first = self.commit()
        middle = self.commit(author="fake-author@gmail.com", parent=first)
        tip = self.commit(parent=middle)
        status, output = self.check(
            "--outgoing", outgoing=f"refs/heads/main {tip} refs/heads/main {first}\n"
        )
        self.assertEqual(status, 1)
        self.assertIn("private author email", output)
        self.assertIn(middle[:12], output)
        self.assertNotIn("fake-author@gmail.com", output)

    def test_outgoing_checks_committer_separately(self):
        tip = self.commit(committer="fake-committer@googlemail.com")
        status, output = self.check(
            "--outgoing", outgoing=f"refs/heads/main {tip} refs/heads/main {'0' * 40}\n"
        )
        self.assertEqual(status, 1)
        self.assertIn("private committer email", output)
        self.assertNotIn("fake-committer@googlemail.com", output)

    def test_ref_checks_reachable_metadata_not_only_tip(self):
        first = self.commit(author="fake-author@gmail.com")
        self.commit(parent=first)
        status, output = self.check("--ref", "main")
        self.assertEqual(status, 1)
        self.assertIn(first[:12], output)

    def test_private_coauthor_and_unfolded_trailers_are_rejected(self):
        for trailer in (
            "Co-authored-by: Example <fake-coauthor@gmail.com>",
            "Reviewed-by: Example\n <fake-reviewer@googlemail.com>",
        ):
            with self.subTest(trailer=trailer.split(":", 1)[0]):
                tip = self.commit(message="PDF update.\n\n" + trailer)
                status, output = self.check("--ref", tip)
                self.assertEqual(status, 1)
                self.assertIn("private email in commit trailer", output)
                self.assertNotIn("@gmail.com", output)
                self.assertNotIn("@googlemail.com", output)

    def test_other_private_domain_is_not_silently_allowed(self):
        self.commit(author="fake-author@example.test")
        self.assertEqual(self.check("--ref", "main")[0], 1)

    def test_private_address_in_author_name_is_rejected(self):
        self.commit(author_name="Example fake-name@gmail.com")
        status, output = self.check("--ref", "main")
        self.assertEqual(status, 1)
        self.assertIn("private author email", output)
        self.assertNotIn("fake-name@gmail.com", output)

    def test_noreply_lookalike_domain_is_rejected(self):
        self.commit(author="fake@users.noreply.github.com.example.test")
        self.assertEqual(self.check("--ref", "main")[0], 1)

    def test_noreply_coauthor_passes(self):
        self.commit(message="PDF update.\n\nCo-authored-by: Example <" + self.PUBLIC + ">")
        self.assertEqual(self.check("--ref", "main")[0], 0)

    def test_staged_mode_does_not_invent_commit_metadata(self):
        self.commit(author="fake-author@gmail.com")
        self.assertEqual(self.check("--staged")[0], 0)

    def test_text_file_mode_still_checks_only_the_supplied_text(self):
        path = self.root / "description.txt"
        path.write_text("Local PDF viewer.\n", encoding="utf-8")
        self.assertEqual(self.check("--text-file", str(path))[0], 0)

    def test_private_tagger_is_rejected_for_ref_and_outgoing(self):
        tag = self.tag(self.commit(), tagger="fake-tagger@gmail.com")
        for args, incoming in (
            (("--ref", "refs/tags/example"), ""),
            (("--outgoing",), f"refs/tags/example {tag} refs/tags/example {'0' * 40}\n"),
        ):
            with self.subTest(mode=args[0]):
                status, output = self.check(*args, outgoing=incoming)
                self.assertEqual(status, 1)
                self.assertIn("private tagger email", output)
                self.assertNotIn("fake-tagger@gmail.com", output)

    def test_noreply_annotated_tag_passes(self):
        self.tag(self.commit())
        self.assertEqual(self.check("--ref", "refs/tags/example")[0], 0)

    def test_nested_tag_does_not_hide_private_inner_tagger(self):
        inner = self.tag(self.commit(), name="inner", tagger="fake-inner@googlemail.com")
        outer = self.tag(inner, name="outer")
        for args, incoming in (
            (("--ref", "refs/tags/outer"), ""),
            (("--outgoing",), f"refs/tags/outer {outer} refs/tags/outer {'0' * 40}\n"),
        ):
            with self.subTest(mode=args[0]):
                status, output = self.check(*args, outgoing=incoming)
                self.assertEqual(status, 1)
                self.assertIn(inner[:12], output)
                self.assertNotIn("fake-inner@googlemail.com", output)

    def test_lightweight_tag_to_commit_remains_supported(self):
        sha = self.commit()
        self.git("update-ref", "refs/tags/lightweight", sha)
        self.assertEqual(self.check("--ref", "refs/tags/lightweight")[0], 0)

    def test_annotation_uses_existing_prose_checks(self):
        self.tag(self.commit(), message="owner_thread_id")
        status, output = self.check("--ref", "refs/tags/example")
        self.assertEqual(status, 1)
        self.assertIn("internal operations in prose", output)

    def test_nested_public_tags_pass(self):
        inner = self.tag(self.commit(), name="inner")
        self.tag(inner, name="outer", tagger="noreply@github.com")
        self.assertEqual(self.check("--ref", "refs/tags/outer")[0], 0)

    def test_local_replace_cannot_hide_private_raw_commit(self):
        original = self.commit(author="fake-replaced@gmail.com")
        safe = self.commit()
        self.git("replace", original, safe)
        self.git("update-ref", "refs/heads/main", original)
        # Ordinary Git display now conceals the original identity.
        self.assertEqual(
            self.git("show", "-s", "--format=%ae", original).decode().strip(),
            self.PUBLIC,
        )
        for args, incoming in (
            (("--ref", "main"), ""),
            (("--outgoing",), f"refs/heads/main {original} refs/heads/main {'0' * 40}\n"),
        ):
            with self.subTest(mode=args[0]):
                status, output = self.check(*args, outgoing=incoming)
                self.assertEqual(status, 1)
                self.assertIn("private author email", output)
                self.assertNotIn("fake-replaced@gmail.com", output)


if __name__ == "__main__":
    unittest.main()
