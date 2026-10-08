# 閲覧中と文書を閉じた後のメモリ診断

通常の製品に診断用のヒープ操作を追加しません。`PDFTatsujinSoakTest` は同じUIライブラリを使う別の実行ファイルです。`scripts/soak-candidate.py` が候補のDLL・資材を使う専用runnerへコピーし、プロセス自身のRSS・Private・スレッド・ハンドルを記録します。

## 比較する負荷

`TATSU_SOAK_MODE` は既定の `mixed`、文書なしの `empty`、`digital`、`image`、同じWindowへ次の文書を開く `reuse` です。文書を使うモードは固定した100ページのデジタル文書と50ページの画像文書を使用し、全ページの表示完了を待ちます。PDF・Undo・原本の非変更と既存キャッシュ上限を検査します。`empty` はPDFの保持・描画の試験として数えません。

## 任意の診断

| 環境変数 | 動作と限界 |
|---|---|
| `TATSU_HEAP_DIAGNOSTICS` | 閉じた後に標準Win32ヒープだけをロックして使用中・空きの数値を数える。解放前にJSON等を割り当てない。他ライブラリの破棄され得るヒープは走査しない |
| `TATSU_MEMORY_DIAGNOSTICS` | 自プロセスのVirtualQueryで状態・種別ごとの合計を数える。メモリ本文・ファイル名は読まない。MEM_IMAGE／MEM_MAPPEDのcopy-on-writeがPrivateになった分は、この種別集計では再分類されない |
| `TATSU_PIXMAP_DIAGNOSTICS` | 負荷終了後に一度だけQPixmapCacheを解放し、前後の数値を比較する。動作中のキャッシュ容量・解像度を減らす試験ではない |
| `TATSU_TRIM_DIAGNOSTICS` | 負荷終了後に一度だけ、自プロセスのLFHキャッシュをHeapOptimizeResourcesで最適化して前後を比較する。使用中データの解放や漏れの修正として扱わない |

これらを有効にした時間を通常の操作性能と比較しません。通常の長時間試験では診断変数をすべて解除します。並行診断は各プロセスのメモリの切り分けに使えますが、起動時間や閲覧速度の比較には使いません。

ハーネス・診断ソース・ラッパーのハッシュを起動時に固定し、完了後の作業ツリーを過去のハーネスへ結び付けません。全開閉で有効な診断の完了を要求します。実行した負荷・回数・時間・版を報告し、有限の試験を全PDFでの漏れなしの保証とは呼びません。

参照：[Microsoft VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery)、[HeapSetInformation](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapsetinformation)、[Qt QPixmapCache](https://doc.qt.io/qt-6/qpixmapcache.html)。
