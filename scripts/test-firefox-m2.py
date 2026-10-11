"""Open M2 PDFs in a separate, headless Firefox PDF.js profile.

Verifies interactive Japanese form values, standard annotation types, drawing
and WebDriver print output. This is not native browser/OS clipboard testing.
"""

import argparse
import base64
import json
from pathlib import Path
import tempfile
import pypdfium2 as pdfium
from selenium import webdriver
from selenium.webdriver.common.print_page_options import PrintOptions
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
from selenium.webdriver.support.ui import WebDriverWait


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--firefox", type=Path, required=True)
    parser.add_argument("--geckodriver", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    result = {
        "mode": "headless Firefox PDF.js, separate profile",
        "native_browser_UI": "未実行",
        "OS_clipboard": "未実行",
        "documents": [],
    }
    with tempfile.TemporaryDirectory(
        prefix="firefox-m2-", dir=args.output.resolve()
    ) as profile:
        options = Options()
        options.binary_location = str(args.firefox.resolve())
        options.add_argument("-headless")
        options.add_argument("-profile")
        options.add_argument(profile)
        options.set_preference("browser.shell.checkDefaultBrowser", False)
        options.set_preference("datareporting.policy.dataSubmissionEnabled", False)
        options.set_preference("toolkit.telemetry.enabled", False)
        service = Service(
            str(args.geckodriver.resolve()),
            log_output=str(args.output / "geckodriver.log"),
        )
        try:
            with webdriver.Firefox(service=service, options=options) as driver:
                result["browser_version"] = driver.capabilities["browserVersion"]
                result["build_id"] = driver.capabilities["moz:buildID"]
                driver.set_window_size(1280, 1000)
                wait = WebDriverWait(driver, 30)
                for name in (
                    "form-input.pdf",
                    "annotations.pdf",
                    "writing.pdf",
                    "m2-combined.pdf",
                ):
                    driver.get((args.input / name).resolve().as_uri())
                    wait.until(
                        lambda browser: browser.find_elements(
                            "css selector", ".page canvas"
                        )
                    )
                    wait.until(
                        lambda browser: browser.execute_script(
                            "return !!window.PDFViewerApplication?.pdfDocument && PDFViewerApplication.pdfViewer.getPageView(0)?.renderingState === 3;"
                        )
                    )
                    data = driver.execute_async_script(
                        """
                        const done = arguments[arguments.length - 1];
                        PDFViewerApplication.pdfDocument.getPage(1).then(page =>
                            page.getAnnotations()).then(items => done(items.map(item => ({
                                type: item.annotationType, name: item.fieldName,
                                value: item.fieldValue, contents: item.contentsObj?.str,
                                rect: item.rect
                            })))).catch(error => done({error:String(error)}));
                    """
                    )
                    if not isinstance(data, list):
                        raise RuntimeError(f"PDF.js annotations failed: {data}")
                    document_result = {"file": name, "annotations": data}
                    result["documents"].append(document_result)
                    if name == "form-input.pdf":
                        fields = {
                            item["name"]: item["value"]
                            for item in data
                            if item.get("name")
                        }
                        if (
                            fields.get("name") != "髙橋 香織"
                            or fields.get("notes") != "東京都\n申請内容の追記"
                        ):
                            raise RuntimeError(
                                "Firefox Japanese interactive field values"
                            )
                    if name == "annotations.pdf":
                        # Standard Text comments also have a /Popup companion.
                        # It is not an extra drawing tool or one of the five marks.
                        if [item["type"] for item in data if item["type"] != 16] != [
                            1,
                            5,
                            4,
                            4,
                            9,
                        ]:
                            raise RuntimeError("Firefox standard annotation types")
                        if data[0]["contents"] != "日本語の確認コメント":
                            raise RuntimeError("Firefox Japanese comment contents")
                    driver.save_screenshot(
                        str(args.output / (Path(name).stem + "-firefox.png"))
                    )
                    print_options = PrintOptions()
                    print_options.background = True
                    printed = base64.b64decode(driver.print_page(print_options))
                    if not printed.startswith(b"%PDF"):
                        raise RuntimeError("Firefox did not print PDF")
                    print_path = args.output / (Path(name).stem + "-firefox-print.pdf")
                    print_path.write_bytes(printed)
                    with pdfium.PdfDocument(print_path) as document:
                        expected_pages = 2 if name == "m2-combined.pdf" else 1
                        if len(document) != expected_pages:
                            raise RuntimeError("Firefox print changed page count")
                        printed_text = "\n".join(
                            page.get_textpage().get_text_range() for page in document
                        )
                        if name == "form-input.pdf" and any(
                            value not in printed_text
                            for value in (
                                "髙橋 香織",
                                "東京都",
                                "申請内容の追記",
                                "Red",
                                "West",
                            )
                        ):
                            raise RuntimeError(
                                "Firefox print lost Japanese/choice values"
                            )
                        if name == "m2-combined.pdf" and any(
                            value not in printed_text
                            for value in ("髙橋 香織", "山田 太郎")
                        ):
                            raise RuntimeError(
                                "Firefox print lost combined form/signature"
                            )
                        document[0].render(scale=1.5).to_pil().save(
                            print_path.with_suffix(".png")
                        )
                        document_result.update(
                            printed_pages=len(document),
                            independent_print_engine="PDFium",
                            printed_characters=len(printed_text),
                        )
                    document_result.update(
                        canvas_rendered=True, WebDriver_print_PDF=True
                    )
            result["status"] = "PASS"
        except Exception as error:
            result["status"] = "FAIL"
            result["error"] = str(error)
            raise
        finally:
            (args.output / "firefox-m2.json").write_text(
                json.dumps(result, ensure_ascii=False, indent=2) + "\n", "utf-8"
            )
    print(
        "PASS: Firefox rendered four M2 PDFs, Japanese field/comment values and standard annotations; WebDriver print outputs created"
    )


if __name__ == "__main__":
    main()
