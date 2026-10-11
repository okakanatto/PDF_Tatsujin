"""Restore a byte-exact old development ZIP from its retained range manifest."""

import argparse, hashlib, json, os, re, subprocess, uuid, zipfile
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        while data := f.read(1024 * 1024):
            h.update(data)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if not re.fullmatch(r"PDFTatsujin-[0-9a-z.-]+-windows-x64\.zip", args.archive):
        raise ValueError("Exact development archive name required")
    plan = json.loads(args.plan.read_text(encoding="utf-8-sig"))
    if Path(plan["root"]).resolve() != root:
        raise ValueError("Recovery plan belongs to a different checkout")
    baseline = Path(plan["baseline"]).resolve()
    bundle = Path(plan["bundle"]).resolve()
    if baseline.parent != root / "dist" or bundle.parent != root / "dist":
        raise ValueError("Recovery outside this checkout")
    if (
        digest(baseline) != plan["baseline_sha256"]
        or digest(bundle) != plan["bundle_sha256"]
    ):
        raise ValueError("Recovery baseline or delta changed")
    record = next(v for v in plan["targets"] if Path(v["path"]).name == args.archive)
    target = root / "dist" / args.archive
    temporary = target.with_name(".restore-" + uuid.uuid4().hex + ".part")
    if not args.verify_only:
        if target.exists():
            raise FileExistsError("Preserve existing ZIP")
        measured = subprocess.run(
            [
                "pwsh",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(root / "scripts/measure-storage.ps1"),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8-sig",
            errors="replace",
            check=True,
        )
        if (
            json.loads(measured.stdout)["Bytes"] + record["bytes"] + 4_000_000
            >= 20_000_000_000
        ):
            raise RuntimeError("Restore would exceed the project cap")
    stream = None
    try:
        if not args.verify_only:
            stream = temporary.open("xb")
        h = hashlib.sha256()
        total = 0
        with baseline.open("rb") as base, zipfile.ZipFile(bundle) as zipped:
            for span in record["spans"]:
                if "baseline_offset" in span:
                    base.seek(span["baseline_offset"])
                    data = base.read(span["bytes"])
                else:
                    data = zipped.read(span["member"])
                if (
                    len(data) != span["bytes"]
                    or hashlib.sha256(data).hexdigest() != span["sha256"]
                ):
                    raise ValueError("Recovery range mismatch")
                h.update(data)
                total += len(data)
                if stream:
                    stream.write(data)
        if total != record["bytes"] or h.hexdigest() != record["sha256"]:
            raise ValueError("Whole original ZIP mismatch")
        if stream:
            stream.flush()
            os.fsync(stream.fileno())
            stream.close()
            stream = None
            with zipfile.ZipFile(temporary) as zipped:
                if zipped.testzip() is not None:
                    raise ValueError("Restored ZIP CRC mismatch")
            # Windows rename fails if the destination already exists.
            temporary.rename(target)
        print(
            json.dumps(
                dict(
                    status="PASS",
                    archive=args.archive,
                    bytes=total,
                    sha256=h.hexdigest(),
                    verified_only=args.verify_only,
                )
            )
        )
    finally:
        if stream:
            stream.close()
        if temporary.exists():
            temporary.unlink()


if __name__ == "__main__":
    main()
