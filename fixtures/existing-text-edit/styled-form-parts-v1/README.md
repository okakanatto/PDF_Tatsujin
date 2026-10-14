# 共有Formの文字部分固定入力

合成CC0の既存共有Form／画像入力とstyled-parts-v1のOFL-1.1日本語部分字体から作成した。私的実務文書は含まない。生成経路はscripts/prepare-styled-form-parts.py。criteria.jsonは製品編集前に固定した7PDFのSHA、選択呼出し、置換文字、拒否条件、境界1px・300dpi・領域外差分0を保持する。

shared／rotated／inheritedは同じ2階層のFormを同一ページで2回、別ページで1回描画する。BBox超過、循環、透明グループ、明示クリップは拒否用の別入力。既存入力・正解・閾値は変更していない。再生成した7PDFとcriteria.jsonのバイト一致を確認する。READMEは生成物のSHA対象外。
