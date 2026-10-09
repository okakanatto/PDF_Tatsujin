"""Authenticate encrypted synthetic PDFs in a separate headless Firefox profile."""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import tempfile

from pypdf import PdfReader
from selenium import webdriver
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.common.print_page_options import PrintOptions
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
from selenium.webdriver.support.ui import WebDriverWait


def check(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--firefox", required=True, type=Path)
    parser.add_argument("--geckodriver", required=True, type=Path)
    parser.add_argument(
        "--owner-copy",
        action="store_true",
        help="Check explicit owner copies without a password",
    )
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    result = {
        "status": "FAIL",
        "mode": "Firefox headless, separate temporary profile",
        "native_UI": "未実行",
        "OS_clipboard": "未実行; DOM selection only",
        "physical_printer": "未実行",
        "beforeunload_UI": "未実行; disabled only in owned synthetic test profiles",
        "documents": [],
    }

    def verify(kind, role=None, expected=None):
        label = kind + "-" + (role or "baseline")
        result["stage"] = label
        with tempfile.TemporaryDirectory(
            prefix="firefox-encryption-", dir=args.output.resolve()
        ) as profile:
            options = Options()
            options.binary_location = str(args.firefox.resolve())
            options.page_load_strategy = "none"
            options.set_capability("unhandledPromptBehavior", "accept")
            options.add_argument("-headless")
            options.add_argument("-profile")
            options.add_argument(profile)
            options.set_preference("browser.shell.checkDefaultBrowser", False)
            options.set_preference("datareporting.policy.dataSubmissionEnabled", False)
            options.set_preference("toolkit.telemetry.enabled", False)
            # Isolate data/authentication checks from fixture-close prompts.
            # https://searchfox.org/firefox-main/source/modules/libpref/init/StaticPrefList.yaml
            options.set_preference("dom.disable_beforeunload", True)
            service = Service(
                str(args.geckodriver.resolve()),
                log_output=str(args.output / (label + "-geckodriver.log")),
            )
            with webdriver.Firefox(service=service, options=options) as driver:
                driver.set_window_size(1280, 1000)
                driver.set_script_timeout(40)
                result["browser_version"] = driver.capabilities["browserVersion"]
                result["build_id"] = driver.capabilities["moz:buildID"]
                wait = WebDriverWait(driver, 30)
                suffix = ("copy" if args.owner_copy else "after") if role else "before"
                prefix = "unprotected" if args.owner_copy else "encryption"
                uri = (args.input / f"{prefix}-{kind}-{suffix}.pdf").resolve().as_uri()
                driver.get(uri)
                if role and not args.owner_copy:
                    wait.until(
                        lambda d: d.find_elements(
                            "css selector", "#passwordDialog[open] #password"
                        )
                    )
                    if kind == "rich" and role == "user":
                        original = driver.find_element("id", "passwordText").text
                        driver.find_element("id", "password").send_keys(
                            "wrong-password"
                        )
                        driver.find_element("id", "passwordSubmit").click()
                        wait.until(
                            lambda d: d.find_element("id", "passwordText").text
                            != original
                        )
                        check(
                            driver.find_element("id", "passwordDialog").is_displayed(),
                            "Incorrect password did not remain blocked",
                        )
                    field = driver.find_element("id", "password")
                    field.send_keys(Keys.CONTROL, "a")
                    field.send_keys(
                        "閲覧-User-2026" if role == "user" else "変更-Owner-2026"
                    )
                    driver.find_element("id", "passwordSubmit").click()
                wait.until(
                    lambda d: d.execute_script(
                        "return location.href === arguments[0] && !!window.PDFViewerApplication?.pdfDocument && decodeURI(PDFViewerApplication.url) === decodeURI(arguments[0]) && PDFViewerApplication.pdfViewer.getPageView(0)?.renderingState === 3;",
                        uri,
                    )
                )
                text = driver.execute_async_script(
                    """const done=arguments[0];
                (async()=>{const doc=PDFViewerApplication.pdfDocument;const pages=[];
                for(let i=1;i<=doc.numPages;++i){const page=await doc.getPage(i);const data=await page.getTextContent();
                pages.push(data.items.map(x=>({str:x.str,transform:x.transform,width:x.width,height:x.height,dir:x.dir,hasEOL:x.hasEOL})));}
                return pages;})().then(done,e=>done({error:String(e)}));"""
                )
                check(isinstance(text, list), "Text content unavailable: " + str(text))
                if not role:
                    return text
                check(
                    text == expected, "PDF.js text, reading order or geometry changed"
                )
                row = {
                    "document": kind,
                    "password": "none" if args.owner_copy else role,
                    "pages": len(text),
                    "text_geometry_exact": True,
                }
                if kind == "form" or (kind == "rich" and args.owner_copy):
                    if args.owner_copy:
                        field = driver.find_element("id", "pageNumber")
                        field.send_keys(Keys.CONTROL, "a")
                        field.send_keys("5", Keys.ENTER)
                    wait.until(
                        lambda d: d.execute_script(
                            "return [...document.querySelectorAll('.annotationLayer input')].some(e=>e.value==='髙橋 香織');"
                        )
                    )
                    row["Japanese_form_DOM_value"] = True
                if kind == "ocr":
                    selections = []
                    for number, term in ((1, "市民公園"), (2, "coastal")):
                        field = driver.find_element("id", "pageNumber")
                        field.send_keys(Keys.CONTROL, "a")
                        field.send_keys(str(number), Keys.ENTER)
                        selector = f'.page[data-page-number="{number}"] .textLayer'
                        wait.until(
                            lambda d: d.find_elements(
                                "css selector", selector + " span"
                            )
                        )
                        selected = driver.execute_script(
                            """const layer=document.querySelector(arguments[0]);const selection=window.getSelection();const range=document.createRange();
                        range.selectNodeContents(layer);selection.removeAllRanges();selection.addRange(range);return selection.toString();""",
                            selector,
                        )
                        check(
                            term.lower() in selected.lower() and len(selected) > 500,
                            "Actual encrypted DOM selection failed",
                        )
                        driver.execute_script(
                            """PDFViewerApplication.eventBus.dispatch('find', {source:window,type:'',query:arguments[0],phraseSearch:true,caseSensitive:false,
                        entireWord:false,highlightAll:true,findPrevious:false,matchDiacritics:false});""",
                            term,
                        )
                        wait.until(
                            lambda d: d.execute_script(
                                "return PDFViewerApplication.findController.state?.query === arguments[0] && [...document.querySelectorAll('.highlight.selected')].map(e=>e.textContent).join('').toLowerCase().includes(arguments[0].toLowerCase());",
                                term,
                            )
                        )
                        selections.append(
                            {
                                "page": number,
                                "term": term,
                                "selected_characters": len(selected),
                                "search": "PASS",
                            }
                        )
                    row["DOM_selection_and_search"] = selections
                driver.save_screenshot(str(args.output / (label + ".png")))
                if kind == "rich" and role in ("user", "copy"):
                    print_options = PrintOptions()
                    print_options.background = True
                    printed = base64.b64decode(driver.print_page(print_options))
                    path = args.output / (
                        "owner-copy-rich-firefox-print.pdf"
                        if args.owner_copy
                        else "encrypted-rich-firefox-print.pdf"
                    )
                    path.write_bytes(printed)
                    check(
                        printed.startswith(b"%PDF") and len(PdfReader(path).pages) > 0,
                        "WebDriver print output invalid",
                    )
                    row["print"] = (
                        "WebDriver Print Page; PDF structure checked, physical print 未実行"
                    )
                result["documents"].append(row)
                return text

    try:
        for kind in ("rich", "ocr") if args.owner_copy else ("rich", "form", "ocr"):
            before = verify(kind)
            for role in ("copy",) if args.owner_copy else ("user", "owner"):
                verify(kind, role, before)
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = str(error)
    finally:
        result["input_sha256"] = {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in args.input.glob(
                "unprotected-*-copy.pdf"
                if args.owner_copy
                else "encryption-*-after.pdf"
            )
        }
        (
            args.output
            / (
                "firefox-owner-copy.json"
                if args.owner_copy
                else "firefox-encryption.json"
            )
        ).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(
        json.dumps(
            {
                "status": result["status"],
                "documents": len(result["documents"]),
                "error": result.get("error"),
            },
            ensure_ascii=False,
        )
    )
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
