"""Check the synthetic HTML viewer design, not the native PDF application.

Runs a separate headless Firefox profile. No user's browser or clipboard is used.
Expected search counts belong to this design specimen, not the frozen OCR corpus.
"""

import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from selenium import webdriver
from selenium.webdriver.common.action_chains import ActionChains
from selenium.webdriver.common.by import By
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
from selenium.webdriver.support.ui import WebDriverWait


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--firefox", required=True, type=Path)
    parser.add_argument("--geckodriver", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    design = root / "docs/design"
    args.output.mkdir(parents=True, exist_ok=False)
    report = {
        "scope": "Synthetic HTML prototype in headless Firefox; not PDF app acceptance",
        "native_app_viewer_V01_to_V12": "未実行",
        "real_PDF_OCR_save_print": "対象外: design prototype",
        "OS_clipboard_IME_DPI_Narrator": "未実行",
        "Acrobat_comparison": "未実行",
        "source_sha256": {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(design.glob("viewer-prototype.*"))
        },
        "checks": [],
    }
    with tempfile.TemporaryDirectory(
        prefix="design-", dir=args.output.resolve()
    ) as temp:
        options = Options()
        options.binary_location = str(args.firefox.resolve())
        options.add_argument("-headless")
        options.add_argument("-profile")
        options.add_argument(temp)
        options.set_preference("browser.shell.checkDefaultBrowser", False)
        options.set_preference("datareporting.policy.dataSubmissionEnabled", False)
        options.set_preference("toolkit.telemetry.enabled", False)
        service = Service(
            str(args.geckodriver.resolve()),
            log_output=str(args.output / "geckodriver.log"),
        )
        with webdriver.Firefox(service=service, options=options) as driver:
            report["browser_version"] = driver.capabilities["browserVersion"]
            report["browser_build_id"] = driver.capabilities["moz:buildID"]
            report["geckodriver_version"] = driver.capabilities[
                "moz:geckodriverVersion"
            ]
            wait = WebDriverWait(driver, 10)

            def element(name):
                return driver.find_element(By.ID, name)

            def js(script, *values):
                return driver.execute_script(script, *values)

            def check(name, operation):
                try:
                    detail = operation()
                    report["checks"].append(
                        {"name": name, "status": "PASS", "detail": detail}
                    )
                except Exception as error:
                    report["checks"].append(
                        {"name": name, "status": "FAIL", "error": str(error)}
                    )
                print(name, report["checks"][-1]["status"], flush=True)

            def size(width, height):
                driver.set_window_size(width, height)
                # Match CSS viewport size; browser chrome differs in headless mode.
                delta = js("return [outerWidth-innerWidth,outerHeight-innerHeight]")
                driver.set_window_size(width + delta[0], height + delta[1])
                wait.until(
                    lambda _: js("return [innerWidth,innerHeight]") == [width, height]
                )

            def reset(width=1440, height=900):
                size(width, height)
                driver.get((design / "viewer-prototype.html").resolve().as_uri())
                wait.until(
                    lambda _: len(driver.find_elements(By.CSS_SELECTOR, ".paper")) == 6
                )
                wait.until(lambda _: element("actual-zoom").text != "100%")

            def jump(page):
                field = element("page-number")
                field.click()
                field.send_keys(Keys.CONTROL, "a")
                field.send_keys(str(page), Keys.ENTER)
                wait.until(
                    lambda _: element("page-number").get_attribute("value") == str(page)
                )

            def anchor():
                return js(
                    """const v=document.getElementById('viewport'),r=v.getBoundingClientRect();
                    const x=r.left+v.clientWidth/2,y=r.top+v.clientHeight/2;
                    const paper=[...document.querySelectorAll('.paper')].find(p=>{
                      const b=p.getBoundingClientRect();return b.top<=y&&b.bottom>=y;
                    });
                    if(!paper)throw Error('Anchor is in a page gap');
                    const b=paper.getBoundingClientRect(),s=b.width/paper.offsetWidth;
                    return {page:Number(paper.parentElement.dataset.page),x:(x-b.left)/s,y:(y-b.top)/s};"""
                )

            def anchor_error(point):
                return js(
                    """const a=arguments[0],v=document.getElementById('viewport'),r=v.getBoundingClientRect();
                    const p=document.querySelectorAll('.paper')[a.page],b=p.getBoundingClientRect(),s=b.width/p.offsetWidth;
                    return Math.max(Math.abs(b.left+a.x*s-r.left-v.clientWidth/2),
                    Math.abs(b.top+a.y*s-r.top-v.clientHeight/2));""",
                    point,
                )

            def opening():
                reset()
                assert (
                    "実PDF処理なし"
                    in driver.find_element(By.CLASS_NAME, "preview-badge").text
                )
                assert all(
                    not button.is_enabled()
                    for button in driver.find_elements(
                        By.CSS_SELECTOR, ".document-actions button"
                    )
                )
                assert js(
                    "return document.getElementById('viewport').scrollHeight>innerHeight*4"
                )
                driver.save_screenshot(str(args.output / "reading-1440.png"))
                return {"pages": 6, "viewport_css_px": [1440, 900]}

            check("P01: honest preview and continuous document", opening)

            def reading():
                reset()
                before = anchor()
                element("viewport").send_keys(Keys.PAGE_DOWN)
                wait.until(
                    lambda _: js("return document.getElementById('viewport').scrollTop")
                    > 100
                )
                point = anchor()
                assert before != point
                jump(6)
                element("back").click()
                wait.until(lambda _: anchor_error(point) <= 2)
                element("forward").click()
                wait.until(
                    lambda _: element("page-number").get_attribute("value") == "6"
                )
                return "PageDown, page entry, Back and Forward restored the expected reading location"

            check("P02: reading and view history", reading)

            def zooming():
                reset()
                jump(2)
                point = anchor()
                errors = []
                for button in (
                    "zoom-in",
                    "zoom-in",
                    "zoom-out",
                    "close-panel",
                    "signature",
                    "close-inspector",
                ):
                    element(button).click()
                    wait.until(lambda _: anchor_error(point) <= 2)
                    errors.append(
                        {
                            "operation": button,
                            "max_axis_error_css_px": anchor_error(point),
                        }
                    )
                return errors

            check("P03: zoom and panel anchor", zooming)

            def finding():
                reset()
                element("find").click()
                query = element("query")
                query.send_keys("交通費")
                wait.until(lambda _: element("search-count").text == "8件")
                results = driver.find_elements(By.CLASS_NAME, "result")
                results[2].click()
                wait.until(lambda _: element("search-count").text == "3 / 8件")
                assert (
                    driver.find_element(By.CSS_SELECTOR, "mark.current-match").text
                    == "交通費"
                )
                element("next-result").click()
                assert element("search-count").text == "4 / 8件"
                driver.save_screenshot(str(args.output / "search-1440.png"))
                page_fits = js(
                    """const v=document.getElementById('viewport').getBoundingClientRect(),p=document.querySelectorAll('.paper')[1].getBoundingClientRect();
                return p.left>=v.left+16&&p.right<=v.right-16;"""
                )
                assert (
                    page_fits
                ), "Search cut off the left context although the whole page fits"
                query.click()
                query.send_keys(Keys.ESCAPE)
                assert js("return document.activeElement.id") == "viewport"
                assert len(driver.find_elements(By.CSS_SELECTOR, "mark")) == 8
                element("clear-search").click()
                assert not driver.find_elements(By.CSS_SELECTOR, "mark")
                assert not driver.find_elements(By.CLASS_NAME, "result")
                return {
                    "fixed_query": "交通費",
                    "expected_matches": 8,
                    "Esc_keeps_results": True,
                    "clear_removes_all": True,
                }

            check("P04: occurrence search, context and clear", finding)

            def fast_query():
                reset()
                element("find").click()
                query = element("query")
                query.send_keys("交通費")
                wait.until(lambda _: element("search-count").text == "8件")
                query.send_keys(Keys.CONTROL, "a")
                query.send_keys("expense", Keys.ENTER)
                wait.until(lambda _: element("search-count").text == "1 / 3件")
                assert (
                    driver.find_element(
                        By.CSS_SELECTOR, "mark.current-match"
                    ).text.lower()
                    == "expense"
                )
                return {"replacement_query": "expense", "expected_matches": 3}

            check("P05: immediate Enter uses the new query", fast_query)

            def composition():
                reset()
                element("find").click()
                js(
                    """const q=document.getElementById('query');
                q.dispatchEvent(new CompositionEvent('compositionstart',{bubbles:true}));
                q.value='交通費';q.dispatchEvent(new InputEvent('input',{bubbles:true,isComposing:true}));"""
                )
                # Wait through two debounce intervals without native IME or sleeps.
                driver.execute_async_script("setTimeout(arguments[0],350)")
                assert not driver.find_elements(By.CSS_SELECTOR, "mark")
                js(
                    "document.getElementById('query').dispatchEvent(new CompositionEvent('compositionend',{bubbles:true}))"
                )
                wait.until(lambda _: element("search-count").text == "8件")
                return (
                    "Synthetic composition events only; Windows IME remains unexecuted"
                )

            check("P06: composition does not start an incomplete search", composition)

            def narrow():
                reset(1024, 720)
                jump(2)
                point = anchor()
                element("signature").click()
                assert not element("left-panel").is_displayed()
                assert element("inspector").is_displayed()
                wait.until(lambda _: anchor_error(point) <= 2)
                driver.save_screenshot(str(args.output / "settings-1024.png"))
                element("close-inspector").click()
                assert element("left-panel").is_displayed()
                wait.until(lambda _: anchor_error(point) <= 2)
                element("focus-mode").click()
                assert not element("left-panel").is_displayed()
                element("viewport").send_keys(Keys.ESCAPE)
                assert element("left-panel").is_displayed()
                wait.until(lambda _: anchor_error(point) <= 2)
                driver.save_screenshot(str(args.output / "reading-1024.png"))
                return "1024×720 CSS px: single expanded panel, anchor and focus-mode restoration"

            check("P07: narrow layout and focus recovery", narrow)

            def invalid_page():
                reset()
                jump(2)
                point = anchor()
                field = element("page-number")
                field.click()
                field.send_keys(Keys.CONTROL, "a")
                field.send_keys("99", Keys.ENTER)
                assert field.get_attribute("aria-invalid") == "true"
                assert anchor_error(point) <= 2
                field.send_keys(Keys.ESCAPE)
                assert field.get_attribute("value") == "2"
                assert not element("page-error").text
                return "Invalid page leaves view unchanged; Escape restores the physical number"

            check("P08: invalid navigation is recoverable", invalid_page)

            def hand_tool():
                reset()
                jump(2)
                element("hand-tool").click()
                before = js("return document.getElementById('viewport').scrollTop")
                ActionChains(driver).move_to_element(
                    element("viewport")
                ).click_and_hold().move_by_offset(0, -120).release().perform()
                wait.until(
                    lambda _: js("return document.getElementById('viewport').scrollTop")
                    > before + 100
                )
                element("select-tool").click()
                assert "hand" not in element("viewport").get_attribute("class")
                element("find").click()
                element("query").send_keys(" ")
                assert "hand" not in element("viewport").get_attribute("class")
                return "Pointer panning works; typing Space in search does not change tools"

            check("P09: hand pan and text input isolation", hand_tool)

            def mixed_page():
                reset()
                zoom_before = element("actual-zoom").text
                js(
                    """const v=document.getElementById('viewport'),p=document.querySelectorAll('.paper')[4];
                v.scrollTop+=p.getBoundingClientRect().top-v.getBoundingClientRect().top-24;"""
                )
                wait.until(
                    lambda _: element("page-number").get_attribute("value") == "5"
                )
                assert element("actual-zoom").text == zoom_before
                element("fit-width").click()
                wait.until(lambda _: element("actual-zoom").text != zoom_before)
                assert element("page-number").get_attribute("value") == "5"
                return "Scrolling into landscape retains scale; explicit fit uses landscape width"

            check("P10: mixed paper sizes", mixed_page)

            def layout_bounds():
                details = []
                for width, height in ((1024, 720), (1440, 900)):
                    reset(width, height)
                    for mode in ("reading", "search", "ocr"):
                        if mode == "search":
                            element("find").click()
                        if mode == "ocr":
                            element("ocr").click()
                        result = js(
                            """const controls=[...document.querySelectorAll('.toolbar button,.statusbar button,.statusbar input,.statusbar select,.rail button,.panel-heading button')].filter(e=>e.getClientRects().length);
                        return controls.filter(e=>{const r=e.getBoundingClientRect();return r.left<0||r.right>innerWidth||r.top<0||r.bottom>innerHeight;}).map(e=>e.id||e.textContent);"""
                        )
                        assert not result, result
                        overlaps = js(
                            """const groups=['.toolbar','.statusbar'];const out=[];
                        for(const group of groups){const items=[...document.querySelector(group).querySelectorAll('button,input,select')].filter(e=>e.getClientRects().length);
                        items.forEach((a,i)=>items.slice(i+1).forEach(b=>{const x=a.getBoundingClientRect(),y=b.getBoundingClientRect();
                        if(Math.min(x.right,y.right)-Math.max(x.left,y.left)>1&&Math.min(x.bottom,y.bottom)-Math.max(x.top,y.top)>1)out.push([a.id,b.id]);}));}return out;"""
                        )
                        assert not overlaps, overlaps
                        assert js(
                            "return document.documentElement.scrollWidth<=innerWidth"
                        )
                        details.append(
                            {
                                "viewport": [width, height],
                                "mode": mode,
                                "offscreen_controls": result,
                            }
                        )
                return details

            check("P11: major controls remain inside viewport", layout_bounds)

            def local_only():
                reset()
                resources = js(
                    "return [...document.scripts].map(e=>e.src).concat([...document.querySelectorAll('link[rel=stylesheet]')].map(e=>e.href))"
                )
                assert len(resources) == 2
                assert all(url.startswith("file:") for url in resources), resources
                policy = driver.find_element(
                    By.CSS_SELECTOR, 'meta[http-equiv="Content-Security-Policy"]'
                ).get_attribute("content")
                assert "connect-src 'none'" in policy
                return {
                    "declared_assets": [url.rsplit("/", 1)[-1] for url in resources],
                    "CSP_connect_src": "none",
                    "network_packet_capture": "未実行",
                }

            check("P12: local asset references and restrictive CSP", local_only)

    report["passed"] = sum(row["status"] == "PASS" for row in report["checks"])
    report["failed"] = sum(row["status"] == "FAIL" for row in report["checks"])
    (args.output / "results.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Design preview: {report['passed']} PASS / {report['failed']} FAIL")
    raise SystemExit(1 if report["failed"] else 0)


if __name__ == "__main__":
    main()
