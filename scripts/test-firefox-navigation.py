"""Click preserved bookmarks and an internal PDF link in a dedicated Firefox.

Uses a separate headless profile. Never targets the user's browser or clipboard.
Expected physical pages come from the already frozen navigation fixture manifest.
"""

import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from selenium import webdriver
from selenium.common.exceptions import TimeoutException
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
from selenium.webdriver.support.ui import WebDriverWait


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Saved navigation-preserved.pdf")
    parser.add_argument("output", type=Path, help="New evidence directory")
    parser.add_argument("--firefox", required=True, type=Path)
    parser.add_argument("--geckodriver", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    manifest_bytes = (root / "fixtures/viewer-navigation-manifest.json").read_bytes()
    expected = json.loads(manifest_bytes)["expected"]
    result = {
        "input_sha256": hashlib.sha256(args.input.read_bytes()).hexdigest(),
        "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "mode": "Firefox headless, real WebDriver clicks in PDF.js content DOM",
        "OS_clipboard": "未実行; untouched",
        "native_window": "未実行; dedicated headless profile",
        "bookmarks": [],
        "status": "FAIL",
    }
    try:
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
            with webdriver.Firefox(service=service, options=options) as driver:
                result["browser_version"] = driver.capabilities["browserVersion"]
                result["build_id"] = driver.capabilities["moz:buildID"]
                driver.set_window_size(1280, 1000)
                wait = WebDriverWait(driver, 30)
                driver.get(args.input.resolve().as_uri())
                wait.until(
                    lambda d: d.execute_script(
                        "return !!window.PDFViewerApplication?.pdfDocument"
                    )
                )
                # An allocated PDFDocument alone does not mean the page layout is
                # initialized. Test links after actual text rendering and page setup.
                wait.until(lambda d: d.find_elements("css selector", ".textLayer span"))
                driver.set_script_timeout(30)
                initialized = driver.execute_async_script(
                    "const done = arguments[arguments.length-1]; PDFViewerApplication.pdfViewer.pagesPromise.then(()=>done(true), e=>done(String(e))); "
                )
                assert initialized is True, f"PDF page setup failed: {initialized}"
                result["observed_buttons"] = driver.execute_script(
                    "return [...document.querySelectorAll('button')].map(e => ({id:e.id,title:e.title,label:e.getAttribute('aria-label')}))"
                )
                driver.find_element("id", "viewsManagerToggleButton").click()
                driver.find_element("id", "viewsManagerSelectorButton").click()
                driver.find_element("id", "outlinesViewMenu").click()
                result["observed_outline_DOM"] = driver.execute_script(
                    "return [...document.querySelectorAll('[id]')].filter(e=>/outline|view/i.test(e.id)).map(e=>({id:e.id,classes:e.className,text:e.textContent.slice(0,200)}))"
                )
                result["outline_markup"] = driver.find_element(
                    "id", "outlinesView"
                ).get_attribute("innerHTML")
                wait.until(lambda d: d.find_elements("css selector", "#outlinesView a"))
                observed = [
                    a.text
                    for a in driver.find_elements("css selector", "#outlinesView a")
                ]
                result["observed_outline_titles"] = observed
                # Do not execute the diagnostic invalid/compound/external actions.
                for index, row in enumerate(expected["outlines"][:11]):
                    anchors = driver.find_elements("css selector", "#outlinesView a")
                    matches = [a for a in anchors if a.text == row["title"]]
                    assert (
                        len(matches) == 1
                    ), f"Expected one observed outline: {row['title']}"
                    driver.execute_script(
                        "arguments[0].scrollIntoView({block:'nearest'})", matches[0]
                    )
                    matches[0].click()
                    passed = True
                    try:
                        wait.until(
                            lambda d: d.execute_script(
                                "return PDFViewerApplication.page"
                            )
                            == row["page"]
                        )
                    except TimeoutException:
                        passed = False
                        driver.save_screenshot(
                            str(args.output / f"failed-destination-{index}.png")
                        )
                    result["bookmarks"].append(
                        {
                            "title": row["title"],
                            "expected_physical_page": row["page"],
                            "actual_physical_page": driver.execute_script(
                                "return PDFViewerApplication.page"
                            ),
                            "status": "PASS" if passed else "FAIL",
                            "viewer_state": driver.execute_script(
                                "return {page:PDFViewerApplication.page,label:document.getElementById('pageNumber').value,scale:PDFViewerApplication.pdfViewer.currentScaleValue,location:PDFViewerApplication.pdfViewer._location}"
                            ),
                        }
                    )
                driver.save_screenshot(str(args.output / "saved-userunit-bookmark.png"))
                # Setup uses the viewer API; the action under test is a real DOM click.
                driver.execute_script(
                    "PDFViewerApplication.pdfViewer.currentScaleValue = 1;"
                )
                wait.until(
                    lambda d: d.execute_script(
                        "return PDFViewerApplication.pdfViewer.currentScale"
                    )
                    == 1
                )
                driver.execute_script("PDFViewerApplication.page = 1;")
                wait.until(
                    lambda d: d.execute_script("return PDFViewerApplication.page") == 1
                )
                selector = (
                    '.page[data-page-number="1"] .annotationLayer .linkAnnotation a'
                )
                wait.until(lambda d: d.find_elements("css selector", selector))
                links = driver.find_elements("css selector", selector)
                result["observed_links"] = [
                    {
                        "href": a.get_attribute("href"),
                        "internal": a.get_attribute("data-internal-link"),
                    }
                    for a in links
                ]
                internal = [
                    a
                    for a in links
                    if a.get_attribute("href").endswith("#named-target")
                ]
                assert (
                    len(internal) >= 1
                ), "No preserved internal link is available in the viewer"
                wait.until(lambda d: internal[0].is_displayed())
                # This observed named link in the frozen fixture points to page 3.
                internal[0].click()
                passed = True
                try:
                    wait.until(
                        lambda d: d.execute_script("return PDFViewerApplication.page")
                        == 3
                    )
                except TimeoutException:
                    passed = False
                result["internal_link"] = {
                    "expected_physical_page": 3,
                    "actual_physical_page": driver.execute_script(
                        "return PDFViewerApplication.page"
                    ),
                    "status": "PASS" if passed else "FAIL",
                }
                driver.save_screenshot(str(args.output / "saved-internal-link.png"))
                result["status"] = (
                    "PASS"
                    if passed
                    and all(row["status"] == "PASS" for row in result["bookmarks"])
                    else "FAIL"
                )
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (args.output / "firefox-navigation.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    print(
        json.dumps(
            {
                "bookmarks": len(result["bookmarks"]),
                "internal_link": result["internal_link"],
                "status": result["status"],
            },
            ensure_ascii=False,
        )
    )
    return int(result["status"] != "PASS")


if __name__ == "__main__":
    raise SystemExit(main())
