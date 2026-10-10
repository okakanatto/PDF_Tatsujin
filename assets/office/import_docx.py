"""Owned local LibreOffice conversion; executed by its bundled Python runtime."""

import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

import uno
from com.sun.star.beans import PropertyValue


def prop(name, value):
    result = PropertyValue()
    result.Name, result.Value = name, value
    return result


def no_asian_spacing(text):
    cursor = text.createTextCursor()
    cursor.gotoEnd(True)
    cursor.setPropertyValue("ParaIsCharacterDistance", False)
    contents = text.createEnumeration()
    while contents.hasMoreElements():
        element = contents.nextElement()
        if element.supportsService("com.sun.star.text.TextTable"):
            for name in element.getCellNames():
                no_asian_spacing(element.getCellByName(name).getText())


def main():
    engine, source, destination, profile, suppress = sys.argv[1:]
    channel = "PDFTatsujin_" + uuid.uuid4().hex
    command = [
        engine,
        "-env:UserInstallation=" + Path(profile).resolve().as_uri(),
        "--headless",
        "--nologo",
        "--nodefault",
        "--norestore",
        "--accept=pipe,name=" + channel + ";urp;StarOffice.ServiceManager",
    ]
    process = subprocess.Popen(command, creationflags=subprocess.CREATE_NO_WINDOW)
    document = desktop = None
    try:
        local = uno.getComponentContext()
        resolver = local.ServiceManager.createInstanceWithContext(
            "com.sun.star.bridge.UnoUrlResolver", local
        )
        deadline = time.monotonic() + 45
        while True:
            try:
                context = resolver.resolve(
                    "uno:pipe,name=" + channel + ";urp;StarOffice.ComponentContext"
                )
                break
            except Exception:
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(
                        "Cannot connect to the owned LibreOffice process"
                    )
                time.sleep(0.05)
        desktop = context.ServiceManager.createInstanceWithContext(
            "com.sun.star.frame.Desktop", context
        )
        document = desktop.loadComponentFromURL(
            Path(source).resolve().as_uri(),
            "_blank",
            0,
            (
                prop("Hidden", True),
                prop("ReadOnly", True),
                prop(
                    "MacroExecutionMode",
                    uno.getConstantByName(
                        "com.sun.star.document.MacroExecMode.NEVER_EXECUTE"
                    ),
                ),
                prop(
                    "UpdateDocMode",
                    uno.getConstantByName(
                        "com.sun.star.document.UpdateDocMode.NO_UPDATE"
                    ),
                ),
            ),
        )
        if document is None or not document.supportsService(
            "com.sun.star.text.TextDocument"
        ):
            raise RuntimeError("DOCX did not load as a text document")
        if suppress == "true":
            no_asian_spacing(document.getText())
        document.storeToURL(
            Path(destination).resolve().as_uri(),
            (
                prop("FilterName", "writer_pdf_Export"),
                prop(
                    "FilterData",
                    (
                        prop("UseLosslessCompression", True),
                        prop("ReduceImageResolution", False),
                    ),
                ),
            ),
        )
        print(
            json.dumps(
                {
                    "output": str(destination),
                    "suppress_asian_spacing": suppress == "true",
                }
            )
        )
    finally:
        if document is not None:
            document.close(True)
        if desktop is not None:
            desktop.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
