"""Owned local LibreOffice conversion; executed by its bundled Python runtime."""

import json
from pathlib import Path
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET
import zipfile

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


def preserve_explicit_paragraph_spacing(document, source):
    """Honor explicit OOXML spacing where Writer's import loses the setting.

    Only direct, plain body paragraphs with both settings disabled qualify.
    Match the loaded text before changing a property; tables, fields, nested
    content, styles and unspecified spacing retain the engine's interpretation.
    """
    namespace = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
    with zipfile.ZipFile(source) as package:
        root = ET.fromstring(package.read("word/document.xml"))
    body = root.find(namespace + "body")
    if body is None:
        return 0
    paragraphs = list(body.findall(namespace + "p"))
    loaded = []
    enumeration = document.getText().createEnumeration()
    while enumeration.hasMoreElements():
        element = enumeration.nextElement()
        if not element.supportsService("com.sun.star.text.Paragraph"):
            return 0
        loaded.append(element)
    if len(loaded) != len(paragraphs):
        return 0
    changed = 0
    for paragraph, element in zip(paragraphs, loaded):
        properties = paragraph.find(namespace + "pPr")
        if properties is None:
            continue
        settings = [
            properties.find(namespace + name) for name in ("autoSpaceDE", "autoSpaceDN")
        ]
        if any(
            setting is None
            or setting.get(namespace + "val") not in ("0", "false", "off")
            for setting in settings
        ):
            continue
        parts = []
        plain = True
        for child in paragraph:
            if child.tag == namespace + "pPr":
                continue
            if child.tag != namespace + "r":
                plain = False
                break
            for item in child:
                if item.tag == namespace + "rPr":
                    continue
                if item.tag == namespace + "t":
                    parts.append(item.text or "")
                elif item.tag == namespace + "tab":
                    parts.append("\t")
                elif (
                    item.tag == namespace + "br"
                    and item.get(namespace + "type") == "page"
                ):
                    continue
                else:
                    plain = False
                    break
        if plain and element.getString() == "".join(parts):
            element.setPropertyValue("ParaIsCharacterDistance", False)
            changed += 1
    return changed


def main():
    engine, source, destination, profile, suppress, kind = sys.argv[1:]
    service, export_filter = {
        "docx": ("com.sun.star.text.TextDocument", "writer_pdf_Export"),
        "xlsx": ("com.sun.star.sheet.SpreadsheetDocument", "calc_pdf_Export"),
        "pptx": (
            "com.sun.star.presentation.PresentationDocument",
            "impress_pdf_Export",
        ),
    }[kind]
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
        if document is None or not document.supportsService(service):
            raise RuntimeError("Office document did not load as the expected component")
        explicit_spacing = (
            preserve_explicit_paragraph_spacing(document, source)
            if kind == "docx"
            else 0
        )
        if suppress == "true":
            no_asian_spacing(document.getText())
        document.storeToURL(
            Path(destination).resolve().as_uri(),
            (
                prop("FilterName", export_filter),
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
                    "explicit_paragraph_spacing_preserved": explicit_spacing,
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
