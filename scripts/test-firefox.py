"""Exercise Firefox's real PDF.js viewer without using the interactive desktop.

Requires a separate Firefox binary and geckodriver. Never uses the user's profile
or OS clipboard. DOM selection is reported separately from clipboard copying.
"""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import tempfile
import unicodedata

from selenium import webdriver
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.common.print_page_options import PrintOptions
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
from selenium.webdriver.support.ui import WebDriverWait


def normalized(text):
    return re.sub(r"\s+", " ", unicodedata.normalize("NFC", text)).strip()


def distance(a, b):
    previous = list(range(len(b) + 1))
    for i, char in enumerate(a, 1):
        current = [i]
        for j, other in enumerate(b, 1):
            current.append(
                min(current[-1] + 1, previous[j] + 1, previous[j - 1] + (char != other))
            )
        previous = current
    return previous[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Completed app selftest directory")
    parser.add_argument("output", type=Path, help="New evidence directory")
    parser.add_argument("--firefox", required=True, type=Path)
    parser.add_argument("--geckodriver", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=False)
    truth_bytes = (root / "fixtures/ground-truth.json").read_bytes()
    truth = json.loads(truth_bytes)
    result = {
        "mode": "Firefox headless, bundled PDF.js; search through viewer event bus",
        "native_find_bar": "未実行; Firefox integrated native find bar is outside content DOM",
        "ground_truth_sha256": hashlib.sha256(truth_bytes).hexdigest(),
        "OS_clipboard_copy": "未実行; DOM selection only, user's clipboard untouched",
        "physical_printer": "未実行",
        "searches": {},
        "pages": [],
    }
    with tempfile.TemporaryDirectory(
        prefix="firefox-", dir=args.output.resolve()
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
                result["geckodriver_version"] = driver.capabilities[
                    "moz:geckodriverVersion"
                ]
                driver.set_window_size(1280, 1000)
                wait = WebDriverWait(driver, 30)

                def open_pdf(name):
                    driver.get((args.input / name).resolve().as_uri())
                    wait.until(
                        lambda d: d.find_elements("css selector", ".textLayer span")
                    )

                open_pdf("signature.pdf")
                driver.save_screenshot(str(args.output / "signature-firefox.png"))
                print_options = PrintOptions()
                print_options.background = True
                printed = base64.b64decode(driver.print_page(print_options))
                assert printed.startswith(b"%PDF"), "Firefox print did not return a PDF"
                (args.output / "signature-firefox-print.pdf").write_bytes(printed)
                result["print"] = "WebDriver Print Page output; inspect PDF separately"

                open_pdf("D03-ocr.pdf")
                # Select each rendered page's actual text layer in DOM order.
                # This does not invoke clipboard APIs or substitute engine extraction.
                for number, reference in enumerate(truth["pages"], 1):
                    field = driver.find_element("id", "pageNumber")
                    field.send_keys(Keys.CONTROL, "a")
                    field.send_keys(str(number), Keys.ENTER)
                    selector = f'.page[data-page-number="{number}"] .textLayer'
                    wait.until(
                        lambda d: d.find_elements("css selector", selector + " span")
                    )
                    text = driver.execute_script(
                        """const layer = document.querySelector(arguments[0]);
                        const selection = window.getSelection();
                        const range = document.createRange();
                        range.selectNodeContents(layer);
                        selection.removeAllRanges(); selection.addRange(range);
                        return selection.toString();""",
                        selector,
                    )
                    expected = normalized(reference["text"])
                    actual = normalized(text)
                    errors = distance(expected, actual)
                    result["pages"].append(
                        {
                            "page": number,
                            "language": reference["language"],
                            "split": reference["split"],
                            "reference_characters": len(expected),
                            "errors": errors,
                            "CER": errors / len(expected),
                            "selected_text": text,
                        }
                    )
                driver.execute_script("window.getSelection().removeAllRanges()")
                for language, terms in truth["search_terms"].items():
                    rows = []
                    for index, term in enumerate(terms):
                        # Observe the viewer's events to avoid accepting stale counts.
                        driver.execute_script(
                            """window.testFindEvents = [];
                            if (!window.testFindHook) {
                              window.testFindHook = true;
                              PDFViewerApplication.eventBus.on('updatefindmatchescount', e => {
                                window.testFindEvents.push({type:'count', ...e.matchesCount});
                              });
                              PDFViewerApplication.eventBus.on('updatefindcontrolstate', e => {
                                window.testFindEvents.push({type:'state', state:e.state, ...e.matchesCount});
                              });
                            }"""
                        )
                        driver.execute_script(
                            """PDFViewerApplication.eventBus.dispatch('find', {
                            source:window, type:'', query:arguments[0], phraseSearch:true,
                            caseSensitive:false, entireWord:false, highlightAll:true,
                            findPrevious:false, matchDiacritics:false
                            });""",
                            term,
                        )
                        wait.until(
                            lambda d: d.execute_script(
                                """const query = PDFViewerApplication.findController.state?.query;
                                const text = [...document.querySelectorAll('.highlight.selected')]
                                  .map(e => e.textContent).join('');
                                return query === arguments[0] &&
                                  (text.toLowerCase().includes(arguments[0].toLowerCase()) ||
                                   window.testFindEvents.some(e => e.state === 1));""",
                                term,
                            )
                        )
                        match = driver.execute_script(
                            """return {
                            label:document.getElementById('findResultsCount').textContent,
                            events:window.testFindEvents,
                            page:document.getElementById('pageNumber').value,
                            selected_text:[...document.querySelectorAll('.highlight.selected')].map(e=>e.textContent).join(''),
                            highlights:document.querySelectorAll('.highlight.selected').length
                            };"""
                        )
                        match.update(
                            term=term,
                            found=any(e.get("total", 0) > 0 for e in match["events"]),
                        )
                        rows.append(match)
                        if index == 0:
                            driver.save_screenshot(
                                str(args.output / f"search-{language}.png")
                            )
                    result["searches"][language] = rows
                result["evaluation"] = {}
                for language, threshold in [("jpn", 0.02), ("eng", 0.01)]:
                    rows = [
                        p
                        for p in result["pages"]
                        if p["language"] == language and p["split"] == "evaluation"
                    ]
                    cer = sum(p["errors"] for p in rows) / sum(
                        p["reference_characters"] for p in rows
                    )
                    found = sum(p["found"] for p in result["searches"][language])
                    result["evaluation"][language] = {
                        "CER": cer,
                        "found": found,
                        "status": (
                            "PASS" if cer <= threshold and found >= 19 else "FAIL"
                        ),
                    }
        except Exception as error:
            result["error"] = str(error)
            raise
        finally:
            (args.output / "firefox-verification.json").write_text(
                json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
            )
    print(json.dumps(result.get("evaluation", {}), ensure_ascii=False))
    return int(any(r["status"] != "PASS" for r in result["evaluation"].values()))


if __name__ == "__main__":
    raise SystemExit(main())
