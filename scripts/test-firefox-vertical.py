"""Check saved vertical OCR in Firefox's real headless PDF.js viewer."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile

from selenium import webdriver
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
from selenium.webdriver.support.ui import WebDriverWait

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--firefox", required=True, type=Path)
    parser.add_argument("--geckodriver", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    criterion = ROOT / "fixtures/vertical-ocr-criteria.json"
    assert hashlib.sha256(criterion.read_bytes()).hexdigest() == (
        "516522131307b548f3b46e8d8a274b398d732857455806c8df2484ee40f3e604"
    )
    fixed = json.loads(criterion.read_text("utf-8"))
    expected = "".join(fixed["expected_lines_right_to_left"])
    result = dict(
        mode="Firefox headless actual PDF.js text layer and find events",
        cases=[],
        native_GUI_OS_clipboard="未実行",
        position_accuracy="PDF.js DOM highlight bounds against frozen printed-glyph rectangles",
    )
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
                driver.set_window_size(1280, 1000)
                wait = WebDriverWait(driver, 30)
                for name in ("vertical-ui-saved.pdf", "vertical-ui-reedited.pdf"):
                    row = dict(file=name)
                    result["cases"].append(row)
                    driver.get((args.input / name).resolve().as_uri())
                    # This fixed PDF starts with image-only pages. Navigate to
                    # its OCR page before waiting for an actual text layer.
                    wait.until(lambda d: d.find_elements("id", "pageNumber"))
                    field = driver.find_element("id", "pageNumber")
                    field.send_keys(Keys.CONTROL, "a")
                    field.send_keys(str(fixed["page"]), Keys.ENTER)
                    selector = f'.page[data-page-number="{fixed["page"]}"] .textLayer'
                    wait.until(
                        lambda d: d.find_elements("css selector", selector + " span")
                    )
                    copied = driver.execute_script(
                        """const layer=document.querySelector(arguments[0]);
                        const range=document.createRange();range.selectNodeContents(layer);
                        const selection=window.getSelection();selection.removeAllRanges();
                        selection.addRange(range);return selection.toString();""",
                        selector,
                    )
                    row["DOM_selected_text"] = copied
                    row["exact_normalized_column_order"] = (
                        re.sub(r"\s+", "", copied) == expected
                    )
                    row["contiguous_column_copy"] = all(
                        column in copied
                        for column in fixed["expected_lines_right_to_left"]
                    )
                    driver.execute_script("window.getSelection().removeAllRanges()")
                    searches = []
                    row["searches"] = searches
                    for term in (entry["term"] for entry in fixed["search_terms"]):
                        driver.execute_script(
                            """window.verticalFindEvents=[];
                            if(!window.verticalFindHook){window.verticalFindHook=true;
                            PDFViewerApplication.eventBus.on('updatefindmatchescount',
                            e=>window.verticalFindEvents.push({type:'count',...e.matchesCount}));
                            PDFViewerApplication.eventBus.on('updatefindcontrolstate',
                            e=>window.verticalFindEvents.push({type:'state',state:e.state,...e.matchesCount}));}
                            PDFViewerApplication.eventBus.dispatch('find', {
                            source:window,type:'',query:arguments[0],phraseSearch:true,
                            caseSensitive:false,entireWord:false,highlightAll:true,
                            findPrevious:false,matchDiacritics:false});""",
                            term,
                        )
                        wait.until(
                            lambda d: d.execute_script(
                                r"""return PDFViewerApplication.findController.state?.query===arguments[0]
                            && (window.verticalFindEvents.some(e=>e.state===1)
                            || [...document.querySelectorAll('.highlight.selected')]
                            .map(e=>e.textContent).join('').replace(/\s+/g,'').includes(arguments[0]));""",
                                term,
                            )
                        )
                        observed = driver.execute_script(
                            """const page=PDFViewerApplication.pdfViewer.getPageView(arguments[0]-1);
                            const base=document.querySelector(arguments[1]).getBoundingClientRect();
                            const highlights=[...document.querySelectorAll('.highlight.selected')];
                            const rectangles=highlights.map(e=>e.getBoundingClientRect());
                            const scale=page.viewport.scale;
                            return {events:window.verticalFindEvents,
                            selected_text:highlights.map(e=>e.textContent).join(''),
                            highlight_bounds_pt:rectangles.length ? [
                            (Math.min(...rectangles.map(r=>r.left))-base.left)/scale,
                            (Math.min(...rectangles.map(r=>r.top))-base.top)/scale,
                            (Math.max(...rectangles.map(r=>r.right))-Math.min(...rectangles.map(r=>r.left)))/scale,
                            (Math.max(...rectangles.map(r=>r.bottom))-Math.min(...rectangles.map(r=>r.top)))/scale] : null};""",
                            fixed["page"],
                            selector,
                        )
                        bounds = observed["highlight_bounds_pt"]
                        expected_bounds = next(
                            item["qt_bounds_pt"]
                            for item in fixed["search_terms"]
                            if item["term"] == term
                        )
                        error = (
                            max(abs(a - b) for a, b in zip(bounds, expected_bounds))
                            * 25.4
                            / 72
                            if bounds
                            else None
                        )
                        observed.update(
                            term=term,
                            maximum_bounds_error_mm=error,
                            bounds_match=error is not None
                            and error <= fixed["maximum_bounds_error_mm"],
                            found=any(
                                event.get("total", 0) == 1
                                for event in observed["events"]
                            ),
                        )
                        searches.append(observed)
                    driver.save_screenshot(
                        str(args.output / (Path(name).stem + ".png"))
                    )
                    row["status"] = (
                        "PASS"
                        if row["exact_normalized_column_order"]
                        and row["contiguous_column_copy"]
                        and all(
                            search["found"] and search["bounds_match"]
                            for search in searches
                        )
                        else "FAIL"
                    )
        result["status"] = (
            "PASS"
            if all(row["status"] == "PASS" for row in result["cases"])
            else "FAIL"
        )
    except Exception as error:
        result.update(status="FAIL", error=str(error))
    (args.output / "result.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", "utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(result["status"] != "PASS")


if __name__ == "__main__":
    main()
