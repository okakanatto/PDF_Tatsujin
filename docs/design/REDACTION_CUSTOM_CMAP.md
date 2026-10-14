# 独自CMapの文字コード再構築

2026-10-12。M5の横書き埋込みTrueTypeの対応を、Identity-Hから固定2バイトの独自CMapへ広げる。元字体・Encoding・ToUnicode・CIDToGIDMapを残さず、保持する字形と文字対応からIdentity-Hを生成する。一般のCFF、可変字体、hint、縦書き、多文字Unicodeは別の残作業。

Encodingのcidchar／cidrangeとToUnicodeのbfchar／bfrange（配列を含む）を境界付きで読む。コード空間・行数・範囲・重複・CIDとUnicode対応を確認し、元の各文字列を2バイト単位で全て消費する。上流の解析結果ともCID・Unicodeを照合する。欠損コード・末尾1バイト・重なる定義・UseCMap・未評価の継承・多文字／サロゲート対応は失敗時非反映で拒否する。別コードが同一CIDを指す場合は同じUnicodeだけを認める。コード空間を無視して未読のデータを捨てることを成功にしない。

Tj／TJと、改行・間隔変更を伴う引用符の文字命令を扱う。再符号化時も改行とTw／Tcの状態を保持する。2バイトCMapで非ゼロTwと空白を併用する条件は、現在の表示エンジンの座標検査が一致しないため拒否する。後で正しい表示を実装して固定追試する。

追加入力と期待結果は実装前にcustom-cmap-v1へ固定する。成功は文字対応・範囲外位置0.5pt以内・PDFiumとPoppler300dpiの範囲外差分0・元資源不在・fontTools輪郭一致を要求する。共有ページ、TJ、引用符、同一CIDの別コードと拒否境界を含める。旧固定入力・正解・閾値は維持し、全体回帰は変更をまとめてから行う。原本、未保存変更、Undo、取消、保存先の非反映も確認する。

参照は [Adobe CMap技術資料](https://www.adobe.com/content/dam/acom/en/devnet/font/pdfs/5099.CMapResources.pdf) と [PDF 1.7仕様](https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/pdfreference1.7old.pdf) のCMap、ToUnicode、文字状態・文字表示命令。コードからCID、コードからUnicodeの関係を区別する。CMapのPostScriptを実行する実装にはしない。
