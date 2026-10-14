"""Inspect the exact Git content and prose intended for publication."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ALLOWED_ROOT_FILES = {
    ".clang-format",
    ".editorconfig",
    ".gitattributes",
    ".gitignore",
    ".gitmodules",
    "CMakeLists.txt",
    "LICENSE",
    "README.md",
    "SPEC.md",
    "TECH.md",
    "ACCEPTANCE.md",
    "CONTRIBUTING.md",
    "ROADMAP.md",
    "dependency-lock.json",
    "office-engine-lock.json",
    "pdfa-engine-lock.json",
    "publication-assets.json",
    "vcpkg.json",
    "vcpkg-configuration.json",
}
ALLOWED_DIRS = {
    "src",
    "tests",
    "scripts",
    "cmake",
    "assets",
    "fixtures",
    "licenses",
    "docs",
    ".github",
    ".githooks",
    "vendor",
}
PRIVATE_PATH = re.compile(
    r"(?:^|/)(?:AGENTS\.md|HANDOFF[^/]*|CAPACITY[^/]*|\.env(?:\..*)?|"
    r"[^/]*\.(?:pfx|p12|key)|development-(?:restart|storage)[^/]*|"
    r"(?:archive_recovery|restore-legacy-zip|storage-breakdown)\.[^/]+)$|"
    r"^(?:evidence|build|dist|tools|\.codex|\.agents|private|internal)/|"
    r"docs/design/(?:ARCHIVE_RECOVERY|DEVELOPMENT_VALIDATION_DEDUP_PLAN)\.md$|"
    r"docs/testing/[^/]*-storage\.json$",
    re.I,
)
PRIVATE_PROSE = re.compile(
    r"チャット|サブエージェント|エージェント|Codex|ChatGPT|"
    r"(?:利用者|ユーザー).{0,15}(?:許可|指示|承認)|"
    r"(?:20|30)\s*GB|20,000,000,000|20_000_000_000|"
    r"pdf-10|owner_thread_id|prior_owner|parallel_agents|watchdog|"
    r"development-restart-state|recovery-index|全員.{0,8}ログ|"
    r"親が|親の|親と|独立担当|実装担当|モデル混雑|停止時刻|"
    r"(?:利用者|ユーザー)の[「『].{0,100}[」』].{0,30}(?:依頼|指示|意向)|"
    r"parent_status_observed|next_owner_action|prior_user_Python_diff|"
    r"authorized push|Parent approval|model at capacity|checkpoint sections staged|"
    r"not_modified_not_staged|prior_user_documents",
    re.I,
)
HOME = re.compile(
    r"[A-Za-z]:[\\/]+Users[\\/]+[^\s\"<>]+|/(?:home|Users)/[^\s\"<>]+", re.I
)
SOURCE_ENVIRONMENT = re.compile(
    r"shell\s+DNS\s+unavailable|source\s+retrieval\s+unavailable\s+via\s+web", re.I
)
SECRETS = {
    "access token": re.compile(
        rb"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{60,}|xox[baprs]-[A-Za-z0-9-]{20,})"
    ),
    "API key": re.compile(
        rb"\b(?:sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{32,}|AIza[A-Za-z0-9_-]{35}|(?:AKIA|ASIA)[A-Z0-9]{16})\b"
    ),
    "private key": re.compile(
        rb"-{5}BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-{5}"
    ),
}
TEXT_EXTENSIONS = {
    ".md",
    ".txt",
    ".json",
    ".py",
    ".ps1",
    ".cpp",
    ".h",
    ".cmake",
    ".yml",
    ".yaml",
    ".xml",
    ".html",
    ".css",
    ".js",
    ".spdx",
    ".chromium",
    ".tar-license",
}


def git(*args, cwd=None):
    return subprocess.check_output(["git", *args], cwd=cwd or ROOT)


def inspect(path, data, approved, prose_only=False):
    errors = []
    p = Path(path)
    if not prose_only:
        if PRIVATE_PATH.search(path):
            errors.append("private file")
        if (
            "/" not in path
            and path not in ALLOWED_ROOT_FILES
            and not re.fullmatch(r"M\d+(?:_[A-Z0-9]+)*_REPORT\.md", path)
        ):
            errors.append("unclassified root file")
        if "/" in path and path.split("/", 1)[0] not in ALLOWED_DIRS:
            errors.append("unclassified directory")
    for label, pattern in SECRETS.items():
        if pattern.search(data):
            errors.append(label)
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        text = None
        if p.suffix in TEXT_EXTENSIONS:
            errors.append("text content must use UTF-8")
    if text is not None:
        if SOURCE_ENVIRONMENT.search(text):
            errors.append("internal environment details")
        if HOME.search(text):
            errors.append("personal machine path")
        if prose_only or p.suffix in {".md", ".txt", ".html", ".xml", ".yaml", ".yml"}:
            if PRIVATE_PROSE.search(text):
                errors.append("internal operations in prose")
        elif p.suffix == ".json":
            try:
                obj = json.loads(text)

                def visit(v):
                    if isinstance(v, dict):
                        for k, x in v.items():
                            if re.search(
                                r"(?:thread_id|parallel_agents|watchdog|storage_budget|project_limit|LimitBytes|(?:^|_)(?:automation|capacity|restart|handoff)(?:_|$)|metadata_recovery|old_recovery_indices|regression_runner_recovery|parent_status_observed|next_owner_action|prior_user_|protected_files)",
                                k,
                                re.I,
                            ):
                                errors.append("internal record field")
                            visit(x)
                    elif isinstance(v, list):
                        for x in v:
                            visit(x)
                    elif isinstance(v, str) and PRIVATE_PROSE.search(v):
                        errors.append("internal operations in record")

                visit(obj)
            except json.JSONDecodeError:
                errors.append("invalid JSON")
    if (
        not prose_only
        and p.suffix not in TEXT_EXTENSIONS
        and p.name not in ALLOWED_ROOT_FILES
    ):
        # New or modified binary content needs a reviewed origin and exact bytes.
        record = approved.get(path)
        if (
            not record
            or record.get("sha256") != hashlib.sha256(data).hexdigest()
            or not record.get("origin")
        ):
            errors.append("binary origin or hash requires review")
    return sorted(set(errors))


def tree(ref):
    result = {}
    for raw in git("ls-tree", "-r", "-z", ref).split(b"\0"):
        if not raw:
            continue
        meta, path = raw.split(b"\t", 1)
        mode, kind, sha = meta.decode().split()
        if kind == "blob":
            result[path.decode()] = sha
    return read_blobs(result)


def read_blobs(entries):
    result = {}
    with subprocess.Popen(
        ["git", "cat-file", "--batch"],
        cwd=ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
    ) as process:
        for path, sha in entries.items():
            process.stdin.write((sha + "\n").encode())
            process.stdin.flush()
            header = process.stdout.readline().split()
            if len(header) != 3 or header[1] != b"blob":
                raise RuntimeError("Could not read publication blob")
            result[path] = process.stdout.read(int(header[2]))
            process.stdout.read(1)
        process.stdin.close()
        if process.wait() != 0:
            raise RuntimeError("Could not complete publication inspection")
    return result


def staged():
    result = {}
    for raw in git("ls-files", "--stage", "-z").split(b"\0"):
        if not raw:
            continue
        meta, path = raw.split(b"\t", 1)
        mode, sha, stage = meta.decode().split()
        if stage != "0":
            raise RuntimeError("Resolve index conflicts first")
        if mode != "160000":
            result[path.decode()] = sha
    return read_blobs(result)


def check(files):
    approved = json.loads(files.get("publication-assets.json", b"{}")).get("assets", {})
    problems = [(path, inspect(path, data, approved)) for path, data in files.items()]
    return [(path, errors) for path, errors in problems if errors]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--staged", action="store_true")
    group.add_argument("--directory", type=Path)
    group.add_argument("--text-file", type=Path)
    group.add_argument("--outgoing", action="store_true")
    group.add_argument("--ref", default="HEAD")
    args = parser.parse_args()
    problems = []
    if args.text_file:
        errors = inspect(
            "publication-text.md", args.text_file.read_bytes(), {}, prose_only=True
        )
        if errors:
            problems.append(("publication text", errors))
    elif args.directory:
        files = {
            p.relative_to(args.directory).as_posix(): p.read_bytes()
            for p in args.directory.rglob("*")
            if p.is_file() and ".git" not in p.parts
        }
        problems = check(files)
    elif args.outgoing:
        seen = set()
        for line in sys.stdin:
            local_ref, local_sha, remote_ref, remote_sha = line.split()
            if local_sha == "0" * 40:
                continue
            revisions = [local_sha]
            if remote_sha != "0" * 40:
                revisions += ["--not", remote_sha]
            for sha in git("rev-list", *revisions).decode().splitlines():
                if sha in seen:
                    continue
                seen.add(sha)
                problems.extend(check(tree(sha)))
                msg = git("show", "-s", "--format=%B", sha)
                errors = inspect("commit.md", msg, {}, prose_only=True)
                if errors:
                    problems.append(("commit message", errors))
    else:
        problems = check(staged() if args.staged else tree(args.ref))
    for path, errors in problems:
        # Report paths and reasons only; never echo a detected secret.
        print(f"BLOCKED {path}: {', '.join(errors)}")
    if problems:
        return 1
    print("Publication checks passed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"Publication checks could not complete: {type(error).__name__}")
        sys.exit(1)
