"""Fill app-created standard forms in an isolated headless Firefox and save them.

Uses a dedicated profile and PDF.js' save API; no desktop focus, OS file dialog,
or clipboard. Validate logical values and newly generated appearance text with
independent pypdf/PDFium. Native Reader, IME and physical printing remain separate.
"""

import argparse
import base64
import hashlib
import json
import tempfile
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject
from selenium import webdriver
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
from selenium.webdriver.support.ui import Select, WebDriverWait


ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source = args.run.resolve() / "designed-external-empty.pdf"
    output = args.output.resolve()
    if not output.is_relative_to(ROOT) or output.exists():
        raise ValueError("Use a fresh owned test directory")
    output.mkdir(parents=True)
    original = sha256(source)
    result = {
        "status": "FAIL",
        "scope": "Isolated headless Firefox; synthetic app-created forms",
        "native_UI": "未実行",
        "native_IME": "未実行",
        "OS_clipboard": "未実行",
        "Reader_GUI": "未実行",
    }
    texts = {
        "designed-1": "山田 太郎 髙橋 𠮷野",
        "designed-2": "新しい日本語の入力\n髙橋 𠮷野 ABC 123",
    }
    try:
        with tempfile.TemporaryDirectory(prefix="form-design-", dir=output) as profile:
            options = Options()
            options.binary_location = str(
                ROOT / "tools/viewer-test/firefox/core/firefox.exe"
            )
            options.add_argument("-headless")
            options.add_argument("-profile")
            options.add_argument(profile)
            options.set_preference("dom.disable_beforeunload", True)
            options.set_preference("datareporting.policy.dataSubmissionEnabled", False)
            options.set_preference("toolkit.telemetry.enabled", False)
            options.page_load_strategy = "none"
            service = Service(
                str(ROOT / "tools/viewer-test/geckodriver/geckodriver.exe"),
                log_output=str(output / "geckodriver.log"),
            )
            with webdriver.Firefox(service=service, options=options) as driver:
                driver.set_window_size(1250, 1150)
                driver.set_script_timeout(90)
                driver.get(source.as_uri())
                wait = WebDriverWait(driver, 45)
                wait.until(
                    lambda d: d.find_elements(
                        "css selector", '.annotationLayer input[name="designed-1"]'
                    )
                )
                for name, value in texts.items():
                    tag = "textarea" if name == "designed-2" else "input"
                    field = driver.find_element(
                        "css selector", f'.annotationLayer {tag}[name="{name}"]'
                    )
                    field.send_keys(value)
                    field.send_keys(Keys.TAB)
                    assert field.get_attribute("value") == value, name
                driver.find_element(
                    "css selector", '.annotationLayer input[name="designed-3"]'
                ).click()
                radios = driver.find_elements(
                    "css selector", '.annotationLayer input[name="designed-4"]'
                )
                assert len(radios) == 2
                radios[0].click()
                combo = Select(
                    driver.find_element(
                        "css selector", '.annotationLayer select[name="designed-5"]'
                    )
                )
                combo.select_by_value("京都")
                multiple = Select(
                    driver.find_element(
                        "css selector", '.annotationLayer select[name="designed-6"]'
                    )
                )
                assert multiple.is_multiple
                multiple.deselect_all()
                multiple.select_by_value("東京")
                multiple.select_by_value("京都")
                driver.save_screenshot(str(output / "filled-firefox.png"))
                encoded = driver.execute_async_script(
                    """const done=arguments[0];
                  PDFViewerApplication.pdfDocument.saveDocument().then(data=>{
                    let chunks=[]; for(let i=0;i<data.length;i+=16384)
                      chunks.push(String.fromCharCode(...data.subarray(i,i+16384)));
                    done({base64:btoa(chunks.join(''))});}, e=>done({error:String(e)}));"""
                )
                assert "error" not in encoded, encoded.get("error")
                saved = output / "firefox-filled.pdf"
                saved.write_bytes(base64.b64decode(encoded["base64"]))
                reader = PdfReader(saved)
                fields = reader.get_fields()
                for name, expected in texts.items():
                    assert fields[name]["/V"] == expected, name
                assert fields["designed-5"]["/V"] == "京都"
                assert list(fields["designed-6"]["/V"]) == ["東京", "京都"]
                for name, expected in (("designed-3", "同意"), ("designed-4", "第一")):
                    actual = str(fields[name]["/V"])[1:]
                    # pypdf decodes the UTF-8 PDF name bytes; preserve the exact
                    # Japanese state, without a second encoding conversion.
                    assert actual == expected, (
                        name,
                        actual,
                    )
                appearances = []
                for annotation in reader.pages[0]["/Annots"]:
                    widget = annotation.get_object()
                    if widget.get("/Subtype") != "/Widget" or not widget.get("/Parent"):
                        continue
                    owner = widget["/Parent"].get_object()
                    name = str(owner.get("/T", ""))
                    if name not in texts:
                        continue
                    ap = widget["/AP"]["/N"].get_object()
                    assert ap.get_data(), name
                    # Interpret the appearance's commands as page commands in a
                    # fresh stream; a Form XObject is not itself a page /Contents.
                    probe = PdfWriter()
                    bbox = list(ap["/BBox"])
                    page = probe.add_blank_page(
                        width=float(bbox[2]) - float(bbox[0]),
                        height=float(bbox[3]) - float(bbox[1]),
                    )
                    content = DecodedStreamObject()
                    content.set_data(ap.get_data())
                    page[NameObject("/Contents")] = probe._add_object(content)
                    page[NameObject("/Resources")] = ap["/Resources"].clone(probe)
                    target = output / (name + "-appearance.pdf")
                    probe.write(target)
                    with pdfium.PdfDocument(target) as pdf:
                        p = pdf[0]
                        t = p.get_textpage()
                        actual = t.get_text_range().replace("\r\n", "\n").rstrip("\n")
                        assert actual == texts[name], (name, actual)
                        bitmap = p.render(scale=2)
                        bitmap.to_pil().save(output / (name + "-appearance.png"))
                        bitmap.close()
                        t.close()
                        p.close()
                    appearances.append(name)
                assert set(appearances) == set(texts)
                with pdfium.PdfDocument(saved) as pdf:
                    pdf.init_forms()
                    p = pdf[0]
                    bitmap = p.render(scale=1.4, draw_annots=True)
                    bitmap.to_pil().save(output / "filled-pdfium.png")
                    bitmap.close()
                    p.close()
                assert sha256(source) == original
                result.update(
                    status="PASS",
                    browser_version=driver.capabilities["browserVersion"],
                    six_kinds=True,
                    Unicode_logical_values_exact=True,
                    newly_generated_appearance_text_exact=True,
                    input_sha256=original,
                    saved_sha256=sha256(saved),
                    saved_bytes=saved.stat().st_size,
                    profile_removed_after_run=True,
                )
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (output / "result.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2), encoding="utf8"
        )
    print(json.dumps(result, ensure_ascii=True))


if __name__ == "__main__":
    main()
