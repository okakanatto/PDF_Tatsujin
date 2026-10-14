# 回転した縦書きOCRの文字層

dev20では元の回転90・CropBox・UserUnit1.5の凍結条件を実OCRへ接続した。正解33文字、連続した3検索語、2mm位置、可視画素差0と原本SHAを維持する。以下はその設計と、dev21で別の180度・270度条件へ広げた記録である。ルビ・旧字体R01等の未評価を固定入力の成功で置き換えない。

認識はdev19と同じ正立したPDF座標の300dpiを使い、保存するページの回転・CropBox・UserUnitを保持する。回転がないページの既存文字層は維持する。回転90では列ごとに一つの標準TJテキストオブジェクトとし、文字を表示座標で並べ替える外部ビューアでも列内の論理順序を保つ。

列の先頭の尺度を共有する。各文字のW2原点と送りを実認識位置へ合わせ、列内の寸法差が0.5mm相当のPDF座標を超えればジョブを反映しない。不可視の字形をもつ実字体は保持し、原本を画像化して保存する方式へ置き換えない。

Qtが生成したTrueTypeサブセットだけを対象に、PDFのWと字体のhmtxを同じ2emへ揃える。字形のglyf・loca、対応GIDとUnicode、サイドベアリングは変えない。各文字の縦送りへ四分の一emを加え、TJで同じ量を戻して正しい位置に置く。文字間の見かけの空白と外部文字層の大きなまとまりを避けるための、標準PDFの送り・位置調整である。固定原稿と検索語に応じた特例は置かない。

字体テーブルの範囲・重複・長さ・glyph数とmetrics数を検査し、hheaの最大送りと全テーブルのチェックサム、headのchecksumAdjustmentを更新する。未対応の形式や署名された字体は拒否する。これは自作の小さな変換で、PDFiumやPDF.jsのコードは取り込まない。NotoのOFLを維持し、予約名Sourceを使わない。

検査は同じアプリの実Qt操作でOCR・署名保持・検索・選択コピー・Undo／Redo・保存・再読込・再編集を行い、PDFium／pypdf、Poppler、Firefoxで独立に確認する。診断PDFの成功をアプリの受入成功としない。ネイティブGUI・IME・OSクリップボード等の未実行も維持する。

参照した一次資料：[PDFiumのテキスト処理](https://pdfium.googlesource.com/pdfium/+/refs/heads/main/core/fpdftext/cpdf_textpage.cpp)、[PDF.jsの文字抽出](https://github.com/mozilla/pdf.js/blob/master/src/core/evaluator.js)、[文字層](https://github.com/mozilla/pdf.js/blob/master/src/display/text_layer.js)、[OpenType hmtx](https://learn.microsoft.com/en-us/typography/opentype/spec/hmtx)、[headチェックサム](https://learn.microsoft.com/en-us/typography/opentype/spec/head)。仕様を調査し、自作処理を実装する。元のFAILと不採用診断も保持する。

## dev21の180度・270度対応条件

実装前に元の回転90入力のRotateだけを変更した180度・270度の別入力と正解を凍結した。元の入力・正解を更新せず、33文字・3検索語・位置2mm・可視画素差0を維持する。成功したdev20の文字層のRotateだけを変更した診断はPDFium／Firefoxで成功したが、製品の実OCR・UI試験とは区別する。

認識を正立PDF座標で行い、すべての回転ページで同じ列TJを使う。保存時のRotateを保持し、表示順から文字を並べ替えない。元の0度・90度試験に加え、180度・270度の実UIでOCR→検索→ドラッグ選択コピー→Undo／Redo→保存→再読込→署名再編集を試験する。各回転で1ページ完了後の取消による部分非反映・署名／Undo保持と作業フォルダ除去も検証する。保存・再編集2出力をPDFium／pypdf、Poppler、Firefoxで同じ閾値に照らす。実行結果は別記録に残す。実行前の時点では製品試験は未実行である。

180度・270度の旧拒否試験は対応実装に伴い全四象限の非変更準備へ更新し、旧候補の拒否とFAIL記録は保持する。モデル不足・不正なモデル組合せ・実取消の失敗条件は維持する。ルビ・古い字体等の一般精度、ネイティブIME・OSクリップボード・外部GUIやM3全体の未評価をこの追加対応で合格にしない。

2026-10-11、dev21のWindows実ビルドとQt UI11件が成功した。90度・180度・270度それぞれの保存／署名再編集2出力についてPDFium／pypdf、全5ページのPoppler可視画素、Firefox157の検索・連続コピー・2mm位置も成功。試験の選択前に実「全体に合わせる」を使い、文書の全体が見える画面でドラッグコピーする。配布候補全回帰・実ZIP展開・新exeの通信遮断は別の実行結果で追跡し、この記録時点では未実行。正解・位置閾値・入力を実装結果に合わせて変えていない。
