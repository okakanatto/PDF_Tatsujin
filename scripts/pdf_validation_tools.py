"""Invoke a local validation engine in an owned temporary configuration directory."""

import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def invoke(java, jar, source, profile, run):
    with tempfile.TemporaryDirectory(
        prefix="pdf-validation-independent-", dir=run
    ) as temporary:
        environment = os.environ.copy()
        environment.update(
            {key: temporary for key in ("APPDATA", "LOCALAPPDATA", "TMP", "TEMP")}
        )
        return subprocess.run(
            [
                str(java),
                "-Xmx512m",
                "-Djava.awt.headless=true",
                "-Djava.io.tmpdir=" + temporary,
                "-Duser.home=" + temporary,
                "-jar",
                str(jar),
                "--format",
                "xml",
                "--flavour",
                profile,
                "--maxfailuresdisplayed",
                "1",
                str(source),
            ],
            capture_output=True,
            timeout=120,
            creationflags=subprocess.CREATE_NO_WINDOW,
            env=environment,
        )
