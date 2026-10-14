# 墨消しコピーの固定合成入力

`foundation` は `scripts/prepare-redaction-probe.py`、`fonts` は `scripts/prepare-redaction-font-probe.py --with-sharing` で、製品試験を実行する前に生成した。元のM1の19入力と証明書の21入力は変更していない。各criteria.jsonにファイルのSHA-256と条件を保持する。

画像、文字、フォーム、架空のメタデータはこのプロジェクトの合成試験用で、非公開実務文書ではない。自作部分はCC0-1.0として利用可能。ReportLabで埋め込んだNoto Sans JPの書体部分はOFL-1.1に従う。書体本体・対応する著作権と条件は既存の `assets/fonts` と `licenses` に保持する。

新しい入力を再生成する場合は別の出力先を使う。既存入力や判定条件を出力へ合わせて変更しない。別オブジェクトへの複製・圧縮異常・上限条件は、上記の字体生成スクリプトに `--with-duplicates --with-audit-errors` を付けて新しいフォルダに作成する。診断候補を機密文書へ使用しない。
