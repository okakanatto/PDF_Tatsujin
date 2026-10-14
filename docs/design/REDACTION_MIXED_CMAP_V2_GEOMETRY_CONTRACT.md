# 混在CMap v2：同一入力PDFの独立輪郭・位置契約

2026-10-13 dev42。v1 criteria、全PDF、source/r2/r3の失敗、対照PDF、旧出力とSHAは保持する。v1の失敗を合格へ置き換えない。製品出力はこの正解の生成・修正に使っていない。

保存先は`fixtures/redaction-copy/mixed-cmap-v2`。44 PDF（成功候補19、拒否候補25）とcriteria.jsonの45ファイル、1,675,933 bytes。criteria SHA-256は`796b7ae0ef979583d0180c91c1020dd30405530d044daf39e83339f0b2ae326a`。全44 PDFはv1と全byte/SHAで同一。Encoding/ToUnicode/content、コード分解、CID/GID、Unicode、幅、原点、Tc/TJ/引用符/Tm/有効Tw、字体・輪郭・命令、期待保持文字、拒否理由、位置0.5pt、PDFium/Poppler72/96/300dpi・矩形の既存1px境界外差0・対象外ページ全画素一致は変更しない。

v1のコード32→非空白条件では、PDFiumが2バイト0041の「秘」を1バイト41の空白と混同し、文字抽出から省略した。このとき固定コードindexをPDFiumのchar indexと同一視した生成器は、後続位置も誤って採取した。v1固定矩形x=114.99000549316406..229.51998901367188は、固定元字形の「秘」x=109.72..130.202と「鶴」x=219.478..239.828を横切る。実Qtの拒否は正しい。生成器のgeometry guardが不十分だったことを記録し、glyphを横切る操作を製品側で許容しない。

v2のType0文字geometryは、固定入力の（コード長,値）→CID→GIDと独立ToUnicode対応から確定する。固定W/Tc/TJ/引用符/Tmの原点と、元FontFile2のfontTools BoundsPenによる真の二次Bezier輪郭boundsを組み合わせる。元SFNT SHAと各glyphの座標/flags/metrics/命令を照合する。GIDからUnicodeを導出しない。PDFiumの抽出・char-index boxes、製品のdecoder/serializer・出力はgeometryの正解に使わない。

成功候補19件の削除文字輪郭の和集合を囲む最小矩形に各辺1ptを加える。同じ独立方法を全成功条件へ適用するため、19矩形と19×2の主保持語boxes、3共有条件×3の別ページ保持語boxesの66フィールドを更新した。各変更の旧値・新値をcriteriaのgeometry_v2.changed_fieldsに保存する。真の輪郭boundsと削除内包/保持非交差を各条件のgeometry_oracleに保存する。字幅のある空白のadvanceは維持するが、輪郭のない空白をink boundsへ入れない。既存Helveticaの後続行geometryは、PDF本文・書体・間隔が同一なのでv1値を保持する。

全44件のv1矩形を監査し、元SHA、旧矩形、想定する削除字形の独立輪郭和集合、内包判定をgeometry_v2.rectangle_auditへ記録する。拒否25件はCMap/間隔の非対応でgeometry受入へ到達しない。旧矩形を変更せず、意図された意味上の輪郭計算は診断であり実行合格ではないと明記する。

独立検査器は、元Encoding/ToUnicodeと生文字bytesを別々に解析し、固定原点を再計算したうえでBoundsPenによる輪郭・全19矩形の厳密1pt余白・全保持輪郭との非交差・保持語boxesを再導出する。実装前の記録は`evidence/dev42-independent-mixed-cmap-v2-source-pre-freeze-r1.json`。source inspectionは44件、製品の実行PASSは0件である。

PDFiumの入力rendererは41/0041を字形でも混同する。独立Identity-H対照とPopplerの元入力は72/96dpiで全画素一致したが、PDFiumは主ページ306/454画素、共有別ページ306/454画素の差を出した。この問題はv2の入力PDFも同一なので残る。v2で正しい意味・字形を出せても、元PDFium画像との対象外一致が失敗した場合は元の0画素閾値でFAILを保存する。Scoped output semantics/GID/除去データのPASSを直接描画契約のPASSへ代用しない。

独立検査は署名metadataと署名領域の画素、別ページ全体の画素を分ける。失敗した全PDF/PNGのパス・サイズ・SHAを保護一覧へ出す。一般CMap/M1/M3/M5、MIAP/4096命令列の作用、M3配布評価は合格扱いにしない。
