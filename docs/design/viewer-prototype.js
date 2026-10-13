/* Design prototype only. Synthetic HTML, no PDF engine, persistence or network. */
"use strict";
const $ = (id) => document.getElementById(id);
const app = $("app");
const viewport = $("viewport");
const world = $("page-world");
const specimens = [
  { title: "出張旅費規程", subtitle: "旅の準備から、精算まで。", body: `
    <p class="intro">業務に必要な移動を、安心して行うために。<br>この規程は、出張の申請と旅費の精算に関する手続きを定めます。</p>
    <div class="rule"></div><h3>01　適用範囲</h3>
    <p>国内の出張に伴う交通費、宿泊費および日当を対象とします。出張前に目的、訪問先、日程を確認し、所属部門の承認を受けてください。</p>
    <h3>02　申請の流れ</h3><p>日程が決まり次第、申請書に必要事項を記入します。予約内容を変更する場合は、変更の理由と費用を確認してください。</p>
    <div class="callout">交通費の精算には、利用日・経路・金額が分かる明細を添付してください。</div>
    <h3>03　この文書について</h3><p>画面の読みやすさを確かめるために作った合成文書です。実在する組織の規程ではありません。</p>` },
  { title: "交通費の取り扱い", subtitle: "必要な移動を、適切な経路で。", body: `
    <h3>01　公共交通機関</h3><p>鉄道、路線バス、航空機を利用した場合は、実際に支払った交通費を精算します。経路は移動時間と費用を考慮して選んでください。</p>
    <table><thead><tr><th>区分</th><th>確認する内容</th></tr></thead><tbody><tr><td>鉄道・バス</td><td>利用日、出発地、到着地、運賃</td></tr><tr><td>航空機</td><td>旅程、搭乗の記録、支払明細</td></tr><tr><td>タクシー</td><td>利用理由、領収書、経路</td></tr></tbody></table>
    <h3>02　領収書の保管</h3><p>電子発行の明細も利用できます。文字が小さい場合は表示を拡大し、日付と金額を確認してください。</p>
    <div class="callout">交通費と宿泊費をまとめて支払った場合は、それぞれの内訳が分かる資料を添付します。</div>
    <h3>03　変更・取消</h3><p>業務上の都合で変更した場合は、理由と取消手数料を記録してください。支払いが発生しなかった場合も、申請内容を更新します。</p>` },
  { title: "宿泊と精算", subtitle: "必要な情報を、ひとつに。", body: `
    <h3>01　宿泊の予約</h3><p>訪問先への移動時間、安全性および費用を考慮して宿泊先を選びます。複数日にわたる場合は、日程と宿泊日を照合してください。</p>
    <h3>02　精算書の作成</h3><p>帰着後、交通費と宿泊費を精算書へ記入します。申請した旅程と異なる場合は、変更点を分かりやすく記載してください。</p>
    <table><thead><tr><th>提出するもの</th><th>記載内容</th></tr></thead><tbody><tr><td>精算書</td><td>出張日程、訪問先、合計額</td></tr><tr><td>支払明細</td><td>日付、項目、金額</td></tr><tr><td>変更の記録</td><td>変更内容と理由</td></tr></tbody></table>
    <h3>03　確認と承認</h3><p>承認者は申請内容と証憑を確認します。不足があれば申請者へ確認し、記録をそろえてから精算を確定します。</p>
    <div class="callout">本文を選択して、複数行の読みやすさと選択色を確認できます。HTMLの選択であり、PDF文字抽出の検証ではありません。</div>` },
  { title: "Travel expense guide", subtitle: "A short reference for business travel.", body: `
    <h3>Before your trip</h3><p>Confirm your destination, schedule and business purpose. Choose a practical route and keep a record of your reservation.</p>
    <h3>During your trip</h3><p>Keep receipts for transport and accommodation. Make sure that the date, description and amount are readable.</p>
    <div class="callout">If your plans change, record the reason and any additional cost. Keep the original booking details with your expense report.</div>
    <h3>After your trip</h3><p>Submit your expense report with the supporting documents. Check each amount before sending the report for approval.</p>
    <p>This is synthetic content for an interface design preview. It is not a policy issued by a real organization.</p>` },
  { title: "精算のチェックリスト", subtitle: "横長ページでも、勝手に倍率を変えない。", landscape: true, body: `
    <table><thead><tr><th>確認項目</th><th>資料</th><th>確認内容</th></tr></thead><tbody><tr><td>日程</td><td>申請書・旅程表</td><td>出発日と帰着日が一致している</td></tr><tr><td>交通費</td><td>乗車・支払明細</td><td>経路と金額が記載されている</td></tr><tr><td>宿泊費</td><td>宿泊明細</td><td>宿泊日と人数が記載されている</td></tr><tr><td>変更</td><td>変更の記録</td><td>理由と差額が確認できる</td></tr></tbody></table>
    <div class="callout">連続スクロールでこのページに入っても倍率は同じです。「幅」を選ぶと、このページを基準に表示幅を合わせます。</div>` },
  { title: "よくある確認事項", subtitle: "必要な箇所へ、すぐに戻る。", body: `
    <h3>交通費の明細が見つからないとき</h3><p>利用したサービスの発行履歴を確認してください。再発行できない場合は、確認できる資料をそろえて相談します。</p>
    <h3>申請内容と実際の旅程が異なるとき</h3><p>変更のあった箇所と理由を記載します。金額だけでなく、移動先と日程も確認してください。</p>
    <h3>文書の読み方</h3><p>左の目次やページ番号から移動できます。検索で見つけた箇所を読んだ後は、上部の「前の表示に戻る」で直前の表示へ戻れます。</p>
    <div class="callout">このプレビューでは、表示の操作だけを試せます。実PDFの表示品質・OCR・保存は製品アプリの試験で別に確認します。</div>` },
];
const state = { scale: 1, zoomMode: "fit-width", fitPage: 0, current: 0,
  left: true, panel: "pages", inspector: null, savedLeft: true, focused: false,
  hand: false, space: false, drag: null, back: [], forward: [], matches: [],
  match: -1, composing: false, queryTimer: null, searchedQuery: "", anchor: null };

specimens.forEach((page, index) => {
  const shell = document.createElement("div");
  shell.className = "page-shell";
  shell.dataset.page = index;
  shell.innerHTML = `<article class="paper ${page.landscape ? "landscape" : ""}" aria-label="${index + 1}ページ"><p class="kicker">PDF TATSUJIN / DESIGN SAMPLE</p><h2>${page.title}</h2><p class="subtitle">${page.subtitle}</p>${page.body}<footer><span>出張旅費規程 · 架空の内容 / 2026.10</span><span>${String(index + 1).padStart(2, "0")}</span></footer></article>`;
  world.append(shell);
  page.shell = shell;
  page.paper = shell.firstElementChild;
  page.original = page.paper.innerHTML;
  page.width = page.landscape ? 1123 : 794;
  page.height = page.landscape ? 794 : 1123;
  const thumb = document.createElement("button");
  thumb.className = `thumbnail ${page.landscape ? "wide" : ""}`;
  thumb.setAttribute("aria-label", `${index + 1}ページ ${page.title}`);
  thumb.innerHTML = `<span class="mini-paper" aria-hidden="true"><b>${page.title}</b>${"<i></i>".repeat(page.landscape ? 5 : 13)}</span><span class="thumbnail-label">${index + 1}</span>`;
  thumb.addEventListener("click", () => jumpPage(index));
  $("thumbnails").append(thumb);
  page.thumb = thumb;
  const bookmark = document.createElement("button");
  bookmark.className = "bookmark";
  bookmark.innerHTML = `${page.title}<span>${index + 1}</span>`;
  bookmark.addEventListener("click", () => jumpPage(index));
  $("bookmarks").append(bookmark);
});

function captureAnchor(rx = .5, ry = .5) {
  const rect = viewport.getBoundingClientRect();
  const px = rect.left + viewport.clientWidth * rx;
  const py = rect.top + viewport.clientHeight * ry;
  let distance = Infinity;
  let chosen = null;
  specimens.forEach((page, index) => {
    const box = page.shell.getBoundingClientRect();
    const delta = Math.max(box.top - py, py - box.bottom, 0);
    if (delta < distance) {
      distance = delta;
      chosen = { page: index, x: (px - box.left) / state.scale,
        y: (py - box.top) / state.scale, rx, ry };
    }
  });
  return chosen;
}

function restoreAnchor(anchor) {
  const rect = viewport.getBoundingClientRect();
  const box = specimens[anchor.page].shell.getBoundingClientRect();
  viewport.scrollLeft += box.left - rect.left + anchor.x * state.scale - viewport.clientWidth * anchor.rx;
  viewport.scrollTop += box.top - rect.top + anchor.y * state.scale - viewport.clientHeight * anchor.ry;
  updatePage();
}

function layout(anchor) {
  const page = specimens[state.fitPage];
  if (state.zoomMode === "fit-width") state.scale = (viewport.clientWidth - 64) / page.width;
  if (state.zoomMode === "fit-page") state.scale = Math.min((viewport.clientWidth - 64) / page.width, (viewport.clientHeight - 64) / page.height);
  state.scale = Math.max(.25, Math.min(4, state.scale));
  specimens.forEach((item) => {
    item.shell.style.width = `${item.width * state.scale}px`;
    item.shell.style.height = `${item.height * state.scale}px`;
    item.paper.style.transform = `scale(${state.scale})`;
  });
  world.style.width = `${Math.max(viewport.clientWidth, ...specimens.map((item) => item.width * state.scale + 64))}px`;
  if (anchor) restoreAnchor(anchor);
  else viewport.scrollLeft = (world.scrollWidth - viewport.clientWidth) / 2;
  updatePage();
  $("actual-zoom").textContent = `${Math.round(state.scale * 100)}%`;
  const option = [...$("zoom").options].find((entry) => entry.value === String(state.scale));
  $("zoom").querySelector('[value="custom"]').textContent = `${Math.round(state.scale * 100)}%`;
  $("zoom").value = state.zoomMode || (option ? option.value : "custom");
  $("fit-width").setAttribute("aria-pressed", state.zoomMode === "fit-width");
  $("fit-page").setAttribute("aria-pressed", state.zoomMode === "fit-page");
}

function updatePage() {
  state.current = captureAnchor().page;
  specimens.forEach((item, index) => item.thumb.setAttribute("aria-current", index === state.current ? "page" : "false"));
  if (document.activeElement !== $("page-number")) $("page-number").value = state.current + 1;
  $("previous-page").disabled = state.current === 0;
  $("next-page").disabled = state.current === specimens.length - 1;
  state.anchor = captureAnchor();
}

function snapshot() { return { anchor: captureAnchor(), scale: state.scale, mode: state.zoomMode, fitPage: state.fitPage }; }
function historyButtons() { $("back").disabled = !state.back.length; $("forward").disabled = !state.forward.length; }
function recordView() { state.back.push(snapshot()); state.forward = []; historyButtons(); }
function restoreView(view) {
  state.scale = view.scale; state.zoomMode = view.mode; state.fitPage = view.fitPage;
  layout(view.anchor); viewport.focus({ preventScroll: true });
}
function historyMove(direction) {
  const source = state[direction];
  if (!source.length) return;
  state[direction === "back" ? "forward" : "back"].push(snapshot());
  restoreView(source.pop()); historyButtons(); say("表示位置を復元しました");
}

function jumpPage(index) {
  if (index < 0 || index >= specimens.length) return;
  recordView(); state.fitPage = index; layout();
  const box = specimens[index].shell.getBoundingClientRect();
  const rect = viewport.getBoundingClientRect();
  viewport.scrollTop += box.top - rect.top - 24;
  viewport.scrollLeft += box.left - rect.left - Math.max(24, (viewport.clientWidth - box.width) / 2);
  viewport.focus({ preventScroll: true }); updatePage();
  say(`${index + 1}ページに移動しました`);
}

function changeZoom(mode, scale, anchor = captureAnchor()) {
  cancelDrag(); state.zoomMode = mode; state.scale = scale || state.scale;
  state.fitPage = state.current; layout(anchor);
}
function say(message) { $("status").textContent = message; }

function applyPanels(anchor) {
  app.dataset.left = String(state.left);
  app.dataset.right = String(Boolean(state.inspector));
  app.classList.toggle("focus", state.focused);
  $("inspector").hidden = !state.inspector;
  ["pages", "bookmarks", "search"].forEach((panel) => {
    $(`${panel}-panel`).hidden = panel !== state.panel;
    document.querySelector(`[data-panel="${panel}"]`).setAttribute("aria-pressed", state.left && panel === state.panel);
  });
  $("panel-title").textContent = { pages: "ページ", bookmarks: "しおり", search: "検索" }[state.panel];
  $("focus-mode").setAttribute("aria-pressed", state.focused);
  $("focus-mode").querySelector("span").textContent = state.focused ? "集中表示を解除" : "集中表示";
  $("signature").setAttribute("aria-expanded", state.inspector === "signature");
  $("ocr").setAttribute("aria-expanded", state.inspector === "ocr");
  layout(anchor);
}
function openPanel(panel, toggle = false) {
  const anchor = captureAnchor();
  state.focused = false;
  if (innerWidth < 1200 && state.inspector) { state.inspector = null; state.left = state.savedLeft; }
  state.left = !(toggle && state.left && panel === state.panel);
  state.panel = panel; applyPanels(anchor);
  if (panel === "search" && state.left) { $("query").focus(); $("query").select(); }
}
function inspector(kind) {
  cancelDrag(); const anchor = captureAnchor(); state.focused = false;
  if (kind && !state.inspector) state.savedLeft = state.left;
  state.inspector = kind;
  if (innerWidth < 1200) state.left = kind ? false : state.savedLeft;
  $("signature-settings").hidden = kind !== "signature";
  $("ocr-settings").hidden = kind !== "ocr";
  $("inspector-title").textContent = kind === "ocr" ? "OCR" : "署名";
  applyPanels(anchor);
  if (kind) $("close-inspector").focus(); else viewport.focus({ preventScroll: true });
}
function toggleFocus() {
  const anchor = captureAnchor(); state.focused = !state.focused;
  applyPanels(anchor); viewport.focus({ preventScroll: true });
}

function search() {
  const anchor = captureAnchor();
  const query = $("query").value;
  state.searchedQuery = query;
  state.matches = []; state.match = -1;
  $("results").replaceChildren();
  specimens.forEach((page, pageIndex) => {
    page.paper.innerHTML = page.original;
    if (!query) return;
    const walker = document.createTreeWalker(page.paper, NodeFilter.SHOW_TEXT);
    const nodes = []; while (walker.nextNode()) nodes.push(walker.currentNode);
    for (const node of nodes) {
      const text = node.textContent;
      const expression = new RegExp(query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "giu");
      const matches = [...text.matchAll(expression)];
      if (!matches.length) continue;
      const fragment = document.createDocumentFragment(); let end = 0;
      for (const match of matches) {
        fragment.append(document.createTextNode(text.slice(end, match.index)));
        const mark = document.createElement("mark"); mark.textContent = match[0];
        mark.dataset.hit = state.matches.length; fragment.append(mark);
        const hit = { mark, page: pageIndex, excerpt: text.slice(Math.max(0, match.index - 13), match.index + query.length + 27) };
        state.matches.push(hit); end = match.index + match[0].length;
      }
      fragment.append(document.createTextNode(text.slice(end))); node.replaceWith(fragment);
    }
  });
  state.matches.forEach((hit, index) => {
    const button = document.createElement("button"); button.className = "result";
    const small = document.createElement("small"); small.textContent = `${hit.page + 1}ページ · ${index + 1}件目`;
    const text = document.createElement("span"); text.textContent = hit.excerpt;
    button.append(small, text); button.addEventListener("click", () => selectMatch(index));
    $("results").append(button); hit.button = button;
  });
  $("search-count").textContent = query ? `${state.matches.length}件` : "語句を入力";
  $("previous-result").disabled = $("next-result").disabled = !state.matches.length;
  restoreAnchor(anchor);
}
function selectMatch(index) {
  if (!state.matches.length) return;
  const wrapped = index < 0 || index >= state.matches.length;
  index = (index + state.matches.length) % state.matches.length;
  recordView(); state.match = index;
  state.matches.forEach((hit, item) => {
    hit.mark.classList.toggle("current-match", item === index);
    hit.button.setAttribute("aria-current", item === index);
  });
  const hit = state.matches[index]; const rect = viewport.getBoundingClientRect();
  const box = hit.mark.getBoundingClientRect();
  viewport.scrollTop += box.top - rect.top - viewport.clientHeight / 3;
  const paperBox = specimens[hit.page].shell.getBoundingClientRect();
  if (paperBox.width <= viewport.clientWidth - 48) {
    viewport.scrollLeft += paperBox.left - rect.left - (viewport.clientWidth - paperBox.width) / 2;
  } else if (box.left < rect.left + 32) {
    viewport.scrollLeft += box.left - rect.left - 32;
  } else if (box.right > rect.left + viewport.clientWidth - 32) {
    viewport.scrollLeft += box.right - rect.left - viewport.clientWidth + 32;
  }
  hit.button.scrollIntoView({ block: "nearest" });
  $("search-count").textContent = `${index + 1} / ${state.matches.length}件`;
  updatePage(); say(wrapped ? "検索結果の端から循環しました" : `${hit.page + 1}ページの一致を表示`);
}
function clearSearch() { clearTimeout(state.queryTimer); $("query").value = ""; search(); $("query").focus(); }

function cancelDrag() {
  if (state.drag && viewport.hasPointerCapture(state.drag.pointer)) viewport.releasePointerCapture(state.drag.pointer);
  state.drag = null; viewport.classList.remove("dragging");
}
function tool(hand) {
  cancelDrag(); state.hand = hand;
  viewport.classList.toggle("hand", state.hand || state.space);
  $("hand-tool").setAttribute("aria-pressed", hand);
  $("select-tool").setAttribute("aria-pressed", !hand);
  viewport.focus({ preventScroll: true });
}

$("back").addEventListener("click", () => historyMove("back"));
$("forward").addEventListener("click", () => historyMove("forward"));
document.querySelectorAll("[data-panel]").forEach((button) => button.addEventListener("click", () => openPanel(button.dataset.panel, true)));
$("close-panel").addEventListener("click", () => { const anchor = captureAnchor(); state.left = false; applyPanels(anchor); viewport.focus({ preventScroll: true }); });
$("find").addEventListener("click", () => openPanel("search"));
$("focus-mode").addEventListener("click", toggleFocus);
$("signature").addEventListener("click", () => inspector(state.inspector === "signature" ? null : "signature"));
$("ocr").addEventListener("click", () => inspector(state.inspector === "ocr" ? null : "ocr"));
$("close-inspector").addEventListener("click", () => inspector(null));
$("select-tool").addEventListener("click", () => tool(false));
$("hand-tool").addEventListener("click", () => tool(true));
$("previous-page").addEventListener("click", () => jumpPage(state.current - 1));
$("next-page").addEventListener("click", () => jumpPage(state.current + 1));
$("page-number").addEventListener("keydown", (event) => {
  if (event.key === "Escape") { $("page-number").value = state.current + 1; $("page-error").textContent = ""; $("page-number").removeAttribute("aria-invalid"); viewport.focus(); }
  if (event.key !== "Enter") return;
  const value = $("page-number").value;
  if (!/^[1-6]$/.test(value)) { $("page-error").textContent = "1〜6のページ番号を入力してください"; $("page-number").setAttribute("aria-invalid", "true"); return; }
  $("page-error").textContent = ""; $("page-number").removeAttribute("aria-invalid"); jumpPage(Number(value) - 1);
});
$("zoom").addEventListener("change", (event) => { const value = event.target.value; if (value !== "custom") changeZoom(value.startsWith("fit") ? value : null, Number(value)); });
$("zoom-in").addEventListener("click", () => changeZoom(null, state.scale * 1.2));
$("zoom-out").addEventListener("click", () => changeZoom(null, state.scale / 1.2));
$("fit-width").addEventListener("click", () => changeZoom("fit-width"));
$("fit-page").addEventListener("click", () => changeZoom("fit-page"));
$("query").addEventListener("compositionstart", () => { state.composing = true; clearTimeout(state.queryTimer); });
$("query").addEventListener("compositionend", () => { state.composing = false; clearTimeout(state.queryTimer); state.queryTimer = setTimeout(search, 150); });
$("query").addEventListener("input", () => { clearTimeout(state.queryTimer); if (!state.composing) state.queryTimer = setTimeout(search, 150); });
$("query").addEventListener("keydown", (event) => {
  if (event.isComposing || state.composing) return;
  if (event.key === "Enter") { event.preventDefault(); clearTimeout(state.queryTimer); if ($("query").value !== state.searchedQuery) search(); selectMatch(state.match + (event.shiftKey ? -1 : 1)); }
  if (event.key === "Escape") { event.preventDefault(); event.stopPropagation(); viewport.focus({ preventScroll: true }); }
});
$("clear-search").addEventListener("click", clearSearch);
$("next-result").addEventListener("click", () => selectMatch(state.match + 1));
$("previous-result").addEventListener("click", () => selectMatch(state.match - 1));
viewport.addEventListener("scroll", updatePage, { passive: true });
viewport.addEventListener("wheel", (event) => {
  if (!event.ctrlKey) return;
  event.preventDefault(); const rect = viewport.getBoundingClientRect();
  const anchor = captureAnchor((event.clientX - rect.left) / viewport.clientWidth, (event.clientY - rect.top) / viewport.clientHeight);
  changeZoom(null, state.scale * Math.exp(-event.deltaY * .002), anchor);
}, { passive: false });
viewport.addEventListener("pointerdown", (event) => {
  if (!(state.hand || state.space) || event.button !== 0) return;
  event.preventDefault(); viewport.focus({ preventScroll: true });
  state.drag = { x: event.clientX, y: event.clientY, left: viewport.scrollLeft, top: viewport.scrollTop, pointer: event.pointerId };
  viewport.setPointerCapture(event.pointerId); viewport.classList.add("dragging");
});
viewport.addEventListener("pointermove", (event) => {
  if (!state.drag) return;
  viewport.scrollLeft = state.drag.left - event.clientX + state.drag.x;
  viewport.scrollTop = state.drag.top - event.clientY + state.drag.y;
});
viewport.addEventListener("pointerup", cancelDrag);
viewport.addEventListener("pointercancel", cancelDrag);
window.addEventListener("blur", () => { cancelDrag(); state.space = false; viewport.classList.toggle("hand", state.hand); });
document.addEventListener("keydown", (event) => {
  if (event.isComposing || state.composing) return;
  const editing = /INPUT|SELECT|TEXTAREA/.test(event.target.tagName);
  if (event.ctrlKey && event.key.toLowerCase() === "f") { event.preventDefault(); openPanel("search"); return; }
  if (event.ctrlKey && event.key.toLowerCase() === "l") { event.preventDefault(); $("page-number").focus(); $("page-number").select(); return; }
  if (event.key === "F3") { event.preventDefault(); selectMatch(state.match + (event.shiftKey ? -1 : 1)); return; }
  if (editing) return;
  if (event.altKey && ["ArrowLeft", "ArrowRight"].includes(event.key)) { event.preventDefault(); historyMove(event.key === "ArrowLeft" ? "back" : "forward"); return; }
  if (event.key === "F8") { event.preventDefault(); toggleFocus(); return; }
  if (event.key === "Escape") {
    event.preventDefault();
    if (state.drag) cancelDrag();
    else if (!getSelection().isCollapsed) getSelection().removeAllRanges();
    else if (state.focused) toggleFocus();
    return;
  }
  if (document.activeElement !== viewport) return;
  if (event.key === " ") { event.preventDefault(); state.space = true; viewport.classList.add("hand"); }
  if (event.ctrlKey && ["+", "=", "-", "0", "1"].includes(event.key)) {
    event.preventDefault();
    if (event.key === "0" || event.key === "1") changeZoom(event.key === "0" ? "fit-page" : "fit-width");
    else changeZoom(null, state.scale * (event.key === "-" ? 1 / 1.2 : 1.2));
  }
  if (["PageDown", "PageUp"].includes(event.key)) {
    event.preventDefault(); const direction = event.key === "PageDown" ? 1 : -1;
    if (event.ctrlKey) jumpPage(state.current + direction); else viewport.scrollTop += direction * viewport.clientHeight * .9;
  }
  if (event.ctrlKey && ["Home", "End"].includes(event.key)) { event.preventDefault(); jumpPage(event.key === "Home" ? 0 : specimens.length - 1); }
});
document.addEventListener("keyup", (event) => {
  if (event.key === " ") { state.space = false; cancelDrag(); viewport.classList.toggle("hand", state.hand); }
});
let dimensions = "";
new ResizeObserver(() => {
  const next = `${viewport.clientWidth}:${viewport.clientHeight}`;
  if (next === dimensions) return;
  dimensions = next;
  const anchor = state.anchor;
  if (innerWidth < 1200 && state.inspector && state.left) { state.savedLeft = state.left; state.left = false; applyPanels(anchor); }
  else layout(anchor);
}).observe(viewport);
layout();
viewport.focus({ preventScroll: true });
