"""Compare a frozen Fit destination after preceding clicks and on a fresh PDF.

Observes focus, text rendering and page events without overriding Firefox APIs.
Uses a dedicated headless profile and never touches the user's browser/clipboard.
The existing navigation acceptance sequence remains separate and unchanged.
"""

import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import time

from selenium import webdriver
from selenium.common.exceptions import TimeoutException
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
from selenium.webdriver.support.ui import WebDriverWait


OBSERVE = """
const app = PDFViewerApplication;
window.tatsuTrace = [];
window.tatsuState = () => ({time:performance.now(),page:app.page,
    scale:app.pdfViewer.currentScaleValue,
    scroll_top:app.pdfViewer.container.scrollTop,
    location_page:app.pdfViewer._location?.pageNumber,
    focused_page:document.activeElement?.closest('.page')?.dataset.pageNumber ?? null});
for (const name of ['pagechanging','scalechanging','updateviewarea',
                    'pagerendered','textlayerrendered']) {
    app.eventBus.on(name, e => window.tatsuTrace.push({name,
        event_page:e.pageNumber, ...window.tatsuState()}));
}
document.addEventListener('focusin', e => window.tatsuTrace.push({name:'focusin',
    target_page:e.target.closest?.('.page')?.dataset.pageNumber ?? null,
    target_class:e.target.className, ...window.tatsuState()}), true);
"""


def run_case(driver, wait, pdf, output, prefix, expected, name, read_each=False):
    driver.get(pdf.resolve().as_uri())
    wait.until(lambda d: d.find_elements("css selector", ".textLayer span"))
    driver.set_script_timeout(30)
    initialized = driver.execute_async_script(
        "const done=arguments[arguments.length-1]; PDFViewerApplication.pdfViewer.pagesPromise.then(()=>done(true),e=>done(String(e)));"
    )
    assert initialized is True, f"PDF page initialization failed: {initialized}"
    driver.execute_script(OBSERVE)
    # Firefox remembers sidebar visibility on reload. Open it only when closed;
    # toggling unconditionally would close the controls on the next fresh PDF.
    if not driver.find_element("id", "viewsManagerSelectorButton").is_displayed():
        driver.find_element("id", "viewsManagerToggleButton").click()
        wait.until(
            lambda d: d.find_element("id", "viewsManagerSelectorButton").is_displayed()
        )
    if not driver.find_element("id", "outlinesView").is_displayed():
        driver.find_element("id", "viewsManagerSelectorButton").click()
        wait.until(lambda d: d.find_element("id", "outlinesViewMenu").is_displayed())
        driver.find_element("id", "outlinesViewMenu").click()
    wait.until(lambda d: d.find_elements("css selector", "#outlinesView a"))

    def click(row):
        matches = [
            a
            for a in driver.find_elements("css selector", "#outlinesView a")
            if a.text == row["title"]
        ]
        assert len(matches) == 1, f"Missing/ambiguous outline: {row['title']}"
        driver.execute_script(
            "arguments[0].scrollIntoView({block:'nearest'})", matches[0]
        )
        driver.execute_script(
            "window.tatsuTrace.push({name:'test-click',title:arguments[0],...window.tatsuState()})",
            row["title"],
        )
        matches[0].click()

    for row in prefix:
        click(row)
        wait.until(
            lambda d: d.execute_script("return PDFViewerApplication.page")
            == row["page"]
        )
        if read_each:
            # Actual target text must exist before the next action. No forced
            # scrolling, focus suppression or replacement of the destination.
            wait.until(
                lambda d: d.execute_script(
                    "return !!PDFViewerApplication.pdfViewer.getPageView(arguments[0]-1).textLayer?.div.querySelector('span')",
                    row["page"],
                )
            )

    click(expected)
    candidate_since = None

    def stable_target(d):
        nonlocal candidate_since
        state = d.execute_script("return window.tatsuState()")
        correct = (
            state["page"] == expected["page"]
            and state["location_page"] == expected["page"]
        )
        now = time.monotonic()
        if not correct:
            candidate_since = None
        elif candidate_since is None:
            candidate_since = now
        return correct and now - candidate_since >= 0.5

    passed = True
    try:
        wait.until(stable_target)
    except TimeoutException:
        passed = False
    result = {
        "case": name,
        "prefix": [row["title"] for row in prefix],
        "target_text_wait_between_actions": read_each,
        "expected_physical_page": expected["page"],
        "actual_state": driver.execute_script("return window.tatsuState()"),
        "status": "PASS" if passed else "FAIL",
        "event_trace": driver.execute_script("return window.tatsuTrace"),
        "time_origin": driver.execute_script("return performance.timeOrigin"),
    }
    driver.save_screenshot(str(output / f"{name}.png"))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--firefox", type=Path, required=True)
    parser.add_argument("--geckodriver", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    manifest_bytes = (root / "fixtures/viewer-navigation-manifest.json").read_bytes()
    outlines = json.loads(manifest_bytes)["expected"]["outlines"]
    fit = next(row for row in outlines if row["title"] == "全体 Fit")
    result = {
        "input_sha256": hashlib.sha256(args.input.read_bytes()).hexdigest(),
        "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "mode": "Firefox headless, real outline DOM clicks; observation only",
        "checks": "Expected physical page and viewport location remain there for at least 500ms, within 30s",
        "OS_clipboard": "未実行; untouched",
        "native_window": "未実行; dedicated headless profile",
        "cases": [],
    }
    try:
        with tempfile.TemporaryDirectory(prefix="firefox-", dir=args.output) as profile:
            options = Options()
            options.binary_location = str(args.firefox.resolve())
            options.add_argument("-headless")
            options.add_argument("-profile")
            options.add_argument(str(Path(profile).resolve()))
            for preference in (
                "browser.shell.checkDefaultBrowser",
                "datareporting.policy.dataSubmissionEnabled",
                "toolkit.telemetry.enabled",
            ):
                options.set_preference(preference, False)
            service = Service(
                str(args.geckodriver.resolve()),
                log_output=str(args.output / "geckodriver.log"),
            )
            with webdriver.Firefox(service=service, options=options) as driver:
                result["browser_version"] = driver.capabilities["browserVersion"]
                result["build_id"] = driver.capabilities["moz:buildID"]
                result["geckodriver_version"] = driver.capabilities[
                    "moz:geckodriverVersion"
                ]
                driver.set_window_size(1280, 1000)
                wait = WebDriverWait(driver, 30, poll_frequency=0.1)
                for name, prefix, read_each in (
                    ("preceding-clicks", outlines[:5], False),
                    ("fresh-fit", [], False),
                    ("target-text-ready", outlines[:5], True),
                ):
                    result["cases"].append(
                        run_case(
                            driver,
                            wait,
                            args.input,
                            args.output,
                            prefix,
                            fit,
                            name,
                            read_each,
                        )
                    )
                # Keep the observed implementation locally; public records need
                # only its hash, not copied Firefox source text.
                observed_source = driver.execute_script(
                    "return PDFViewerApplication.pdfLinkService.goToDestination.toString()"
                )
                source_bytes = observed_source.encode("utf-8")
                (args.output / "observed-goToDestination.js").write_bytes(source_bytes)
                result["observed_goToDestination_sha256"] = hashlib.sha256(
                    source_bytes
                ).hexdigest()
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (args.output / "fit-diagnosis.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
    print(
        json.dumps(
            [
                {k: v for k, v in case.items() if k != "event_trace"}
                for case in result["cases"]
            ],
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
