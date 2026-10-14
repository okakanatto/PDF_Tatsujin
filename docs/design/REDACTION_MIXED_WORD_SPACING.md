# dev43：生コードに基づく非0 Twの限定間隔補償

2026-10-13。混在CMapの既存44入力・v1/v2・5直接描画FAILを保持する別単位。製品実装前に新入力のbytes、EncodingとToUnicodeの独立対応、宣言幅、SFNTの真のBezier範囲、文字原点、後続原点、各命令の有効Tw/Tcを固定して別担当が検査する。一般CMap・TrueType・M5完成、M3評価、公開判断を代用しない。

横書き埋め込みTrueType、曖昧でない1/2バイト、コード1個→BMP1個を維持する。Twはコード長1かつ値32だけに作用する。Unicode空白で判断しない。各原点間の移動は `(width * size / 1000 + Tc + raw-code-Tw) * Tz/100`、TJ数値の移動は `-number * size / 1000 * Tz/100`。再符号化したIdentity-Hへ保持字を出力し、元の単語間隔を対象の保持字直後のTJ数値へ一度だけ置く。除去連続区間は幅・Tc・コードTwと内側TJを倍精度で合算し、個々の除去幅・コード・Unicodeを出力しない。

修正した共通エンジンはIdentity-Hの2バイトにTwを加えない。再符号化TJの生コード補償を置いて、元の有効Tw・Tc・引用符の行送り・後続文字行列を維持する。出力のTwを0へ変更して成功扱いにしない。標準書体・既存Tw0経路は変更しない。

製品のPDF4QT解析は修正前にはUnicode空白へTwを加える。元文書の閲覧・選択・墨消し前プレビューの座標も誤るため、Type0の文字生成で生コード長/値によるTw作用フラグを各文字へ渡し、同じ描画処理がこの明示値を用いるよう修正する。Type0ではfalseも明示し、標準/単純書体・Type3・TJ数値にはフラグを置かず旧経路を維持する。墨消し解析だけ別の累積座標を作る案は使わず、修正した共通エンジンの座標を使い、独立CMapdecoderの生コードとフラグも照合する。Tc・Tz・TL・Ts・引用符の設定を維持する。空命令はTw/Tc設定と行送りを保持し、後続命令へ正しい状態を渡す。輪郭のないU+0020は原点だけで選択せず、幅・Tc・そのコードTwを含む移動区間全体が除去範囲に入る場合だけ除去する。欠落字形の代替advanceやType3の一般問題をこのフラグ修正で解消したとは扱わず、墨消しでは旧拒否を維持する。

新固定集合は各Tj/TJ/単引用符/二重引用符で正負Tw/Tc、逆符号Tc、保持/除去コード32→非空白、Unicode空白→32以外/2バイト、同CIDのコード32/別コード空白、空Tj/TJ/二重引用符、q/Q状態復元、Td/TD/T*/Tm/Tz/Ts、同じSFNTのページ別名・兄弟Form・注釈N/R/D外観を分ける。共有先には除去文と同じUnicodeの「秘」を正当に残し、局所GID閉包と元SFNT SHAごとの最終参照unionを別に調べる。一次正例は長さ間で同じ数値のコードを使わない。既存衝突5FAILは別の入力エンジン診断として残す。

prefix曖昧性、切断、再定義、UseCMap、未知命令、縦書き、多文字Unicode、字形の一部だけを横切る条件は非反映拒否を維持する。非0Twの新範囲では、1バイトを含まないCMap、異なるコード長に同じ数値があるCMap、2バイトの値32、Tmの線形部分が単位行列でない状態も未評価として拒否する。これは旧Tw0のdecoder受入条件の変更ではない。既存の直接描画衝突と旧非0Tw拒否入力を保持する境界である。有限数の通常のTw/Tcを扱い、不正数値・未評価構造は受け入れない。入力結果から正解を作らず、旧条件・正解・検索語・閾値を緩めない。

新入力v1の36PDFと `criteria.json`（SHA97b90f5ec2d5c3fbe7ee9883afaf52cd5ebb289f2d5e78e23d392fde2425ac0a）は不変で保持する。製品編集前の独立した元SFNTそのままのIdentity-H対照で、0020の正負Twは実PDFium72/96/300dpiで差があり、Popplerは差0だった。製品出力を見て正解を作った結果ではない。別の `criteria-v2.json`（SHA549232640c1b627d504f25d17861b3dd46311a33de951fe9298b1c030a9d340f）でこの同一2PDFを非反映拒否に限定し、高位2バイト8107の空白2正例・非単位Tm2負例、旧2バイトTw拒否を全byte一致で再利用した1負例を加え、41条件（26成功候補／15拒否候補）を固定する。旧v1の成功期待と入力差分は消さない。新v2の実装前に別担当の原本解析と描画対照を確認する。旧2バイト境界を追加する前のv2候補JSON（SHA4e356f407cf67b2b8c8148a2dcb1d7d3a1480e57b6d44aab8c46145737ace05d）もgzip全byte読戻しで保持する。

固定前のTJ入力作成では負Tw/Tcと保持境界直後のTJ125が保持輪郭へ掛かり、幾何assertionで停止した。3PDF・生成器・失敗記録を `mixed-word-spacing-prefreeze-failed-r1` に保持した。最終新PDFでは正負TJを除去文字の内部へ置いてから、製品を実行せず入力の保持輪郭と除去矩形を固定した。重複メタデータの圧縮前と空引用符原点の修正前のJSONも同じ場所にgzipで全byte読戻し付き保持した。元PDF bytes・数式・既存閾値はその修正で変更していない。

位置0.5pt、両エンジン72/96/300dpiで1px境界外差0、対象外ページ全画素一致を維持する。文字抽出、資源ごとの対応/使用数、FontFile2の輪郭/metrics/flags/命令/checksum、局所閉包、元SFNT SHA union、除去データ/旧資源不在を別担当が確認する。実Qt入口で取消・未保存署名・Undo/Redo・別名保存・再編集・原本保持を確認する。

生成器はPythonの `-B` オプションで実行し、レンダラーや製品を起動しません。

PDF4QTは上流submoduleのまま保持する。元SHAを固定したrootのCMake適応でType0のフラグを渡し、共通content processorとpdffont.hを生成する。引用includeの先頭検索だけに依存せず、CoreのPUBLIC使用条件としてMSVCの `/FI` またはGNU/Clangの `-include` で同じ `PDFFONT_H` 定義を全C++ consumerへ先に渡す。Core・Widgets・文書・OCR・アプリ・selftestのNinja規則を全件検査してからビルドする。

初回実装・r1対象試験の後、保持コード20→公のTwが、Qtの通常抽出へ推定空白を入れる条件を観測した。固定文字列「公開」は生Unicodeとして存在し、元と保存後の同じ保持語を実字形のpointer範囲で比較すれば推定抽出「公 開」も不変である。新範囲だけ、生Unicodeの順序・実空白・固定保持語・原点を確認し、推定抽出を別に比較する。検索語や上流の推定空白1.2閾値は変更しない。旧共通補助検査を緩めず、r1のFAIL・出力PDF・実装ソースのsnapshotとSHA記録を保持する。

その独立検査で、旧v2の負Tw/Tc条件7件は、輪郭のない末尾U+0020の移動区間が矩形内に完全に入るのに `removed=false` と記載していたことが分かった。製品の除去規則をその誤記へ合わせず、元PDFの生コード・幅・Tw/Tc・原点・矩形だけで独立に計算した索引 `evidence/dev43-independent-corrected-space-selection-spec-r1.json`（SHA00cff7d7c18a206c91efb55fa6cba7b8a4468350fee5f40e7552fc044e32807a）に従い、別の `criteria-v3.json`（SHAbcf6e229e6f487d6287acb6962791dca8bc34a91857e16d1eb0c8e4ba6cdaa2e）を作る。旧41PDFの全byte・矩形・検索語・閾値・全fieldsを維持し、旧fieldsの差分は指定7空白の除去フラグfalse→trueだけである。v1/v2と失敗はliteral保持する。v3の固定は初回実装/r1失敗後、v3自身の再試験前であり、初回実装前固定とは呼ばない。

負Tw/Tcを含む正例でも、検査済み空白の実移動は+0.289ptであり、実際のゼロ/逆方向移動は未実証である。非0Twの受入を有限かつ正のTz、各字形の `width*size/1000 + Tc + raw-code-Tw > 0` に限定し、零/逆方向移動は選択前に非反映拒否する。Tw0の受入は維持する。v3へ別に追加する2負例は混在の既存正例のCMap・SFNTを維持し、サイズ25pt・幅220・Tw3.25・Tc-8.75/-9.75でコード20空白の移動を厳密に0/-1ptにする。幾何合格とはせず拒否原本と独立source計算だけを固定する。v3は43条件（26成功候補／17拒否候補）、一般の負/零移動や任意のTw/Tcを完成扱いにしない。親はmetadataを含む所有予約を3MBへ限定追加した。

r2はnative2条件がPASSしたが、独立検査でIdentity-H出力の2FAILを保持した。`array-negative.pdf` はPDFium300dpiで保持文字に20画素の差があり、72/96dpiと全Popplerでは差0。元SFNT全体を製品出力のCID/GIDへ差し戻した対照でも差は不変で、書体サブセットを原因としない。公式上流のfloat文字位置合算を元の幅→生コードTw→TcとTJの順でsourceだけから再現すると、保持本文の前は165.57998657226562pt、製品の一括合算は165.5800018310547ptとなる。厳密な実インストール153.0.7999.0のソース同定とは区別する。もう1FAILはコード20→公をIdentity-Hと明示TJへ変えた出力で、元PDFium抽出「公開」が保存後「公 開」（追加空白generated=1）となった。生Unicode保持と描画差0は外部検索/コピー保持を代替しない。

次の出力方式は、実装・製品試験前にこの設計と別担当のsourceだけの対照を固定してから採用する。v3の43原本・正解・検索語・矩形・閾値を変更せず、旧Identity-H出力のFAILも残す。非0Twを使う適格な局所書体資源に限り、全showの保持字を元の(コード長,値)のbytesで出力する。同じ局所資源のTw0 showも同じ新Encodingで出力する。書体資源が異なるaliasやForm/APは各最終参照の使用集合を別に作る。旧のTw0だけの資源とIdentity-H経路は従来の出力を維持する。

`redaction_content.cpp` は独立decoderの元コードbytesと文字列区分を各字形に結び、最終保持字から使用codekey→subsetCID→GID、codekey→Unicodeの対応を作る。`redaction_font.h/.cpp` の再構築APIへ任意の保持コード対応を追加し、空指定は旧Identity-H出力そのままとする。生コード分岐では新しいEncoding/cidcharとToUnicode/bfcharを使用コードだけで作り、1/2バイトのexact codespaceを両方へ宣言する。複数コード→同CID/同Unicodeのaliasは実使用があるコードだけ残す。未使用コード・除去専用コード・旧CMap bytes・元SFNTの私的表を流用しない。W/CIDToGID/輪郭/metrics/hint/CVTの局所閉包・元SFNT SHA unionは既存規則を維持し、幅やヒントを画素差へ合わせない。ActualText・注釈・不可視文字による抽出の置換は導入しない。

生コードの間隔を再符号化TJへ変換せず、共通エンジンの元コードTwをそのまま使う。除去連続区間は個別の幅・コード・Unicodeを出力しない一つの移動値へ合算する。ただし内部計算では元PDFのfloat合算順（width*size/1000を足す→コード32ならTwを足す→Tcを足す）と、各元文字列間のTJ数値をfloatで先に合算してから引く順を再現する。実画素や測定bboxから補正値を作らない。元の先頭TJは文字objectの外側、内部TJは字形列内、末尾TJは後続状態への移動として区別し、空文字列も元parserの区分に従う。

保持字直前とshow終端で、旧の標準書体にある逆算可能なfloat TJ探索を用い、元float cursorに一致する近傍の移動operandを求める。同時に元のdouble幅/Tw/Tc/TJとの保持原点誤差を実際の出力operandで計算し、既存0.5pt以内を確認する。単なるfloat castや入力別epsilonは使わず、再現不能・不正数・上限超過は候補を保存せず拒否する。保持した元文字列の区分を残し、除去専用内部命令は集約する。新方式の固定26正例では各nonempty showの最初の元字形を保持するため、元の字形列内float原点を維持できる。先頭字形除去を含む任意の新float条件や一般の抽出品質まで実証したとは扱わない。

初回RAW製品編集前に、独立source仕様 `evidence/dev43-independent-before-RAW-source-spec-r1.json`（SHA ddd5b560bde59ede7dba634f744f4d7659973cf00f72fbbbed80dc09be92cb17）と描画対照 `evidence/dev43-independent-raw-source-controls-r1.json`（SHA705d24b2a996351fbb12a1b8b81eea8da46c766e6fd522fda20adb8055ceb9bf）が固定された。元SFNT全体を使う26のメモリ内対照は両エンジン全3dpiで境界外差0、最大数学原点誤差1.0791e-5pt。これはsource方式対照で、RAW製品の除去データ・閉包・実行PASSではない。r2ソース4ファイルとFAIL/report/specのSHAを `evidence/m5-dev43-target-r2-20261013/source-before-RAW/` にliteral保存してから実装を追加した。

RAW候補の未評価境界は非反映拒否で表す。各showの先頭または末尾の実字形が選択された場合は、外側/内側float原点の区分が変わるため拒否する。全除去showもこの新方式では未評価であり、旧Tw0だけの経路をこの条件で変えない。新方式の固定26 show群は先頭・末尾を保持し、この境界へ掛からない。再編集時の既存decoderはcodespace数256までなので、exact一コード区間で出すRAW保持codekeyも256までに限定し、超過は再構築前に拒否する。複数codekey→同CIDのaliasもcodekey数へ計上する。これらはRAW候補の独立静的検査で確認した制約であり、一般の先頭/末尾除去や256超のRAW集合の合格を代用しない。v3の43原本と正解fieldsはこの追加で変更していない。
