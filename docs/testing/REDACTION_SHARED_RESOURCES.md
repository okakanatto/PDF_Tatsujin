# 共有資源・図形を含む墨消しの検証範囲

0.5.0-dev25、Windows x64／Qt6.9.3／MSVC19.50。通常219＋旧全体にあった画面配置の単独追試1＋Windows保存拒否／PDFプリンタ2＝222名がPASS。旧215名を保持、新規M5R12〜16を追加した。全体は2回の環境未完了と1回の完了を記録する。未実行や失敗を成功へ加算しない。

共有TrueType字体を対象外ページ・Form・注釈外観でも再構築し、元字体／対応表の参照や展開ストリーム複製を最終監査する。対象外の単字を保持する。ページ木の資源を局所化し、未知属性や未評価のフォーム字体への残存は拒否する。範囲内へ全体が入る単色図形・線・Bezier・複合パスは構築座標と描画命令を除去し、範囲外は保持する。黒矩形の下へ元のパスを残さない。

追加固定JSONのSHA-256は次のとおり。旧入力・正解・検索語・閾値は変更していない。

| 固定入力 | 成功／拒否 | criteria SHA-256 |
|---|---|---|
| shared-fonts-v1 | 9／3 | a24478e7395e3bfcc4046eb0266aba324c5858d6c3c9db0e447aba5d9ec0f586 |
| vector-paths-v1 | 6／4 | 027f0ab58bf461ce4f47762055244e6ddbe4e3141cfeffed5e60aa7483fbc869 |
| shared-followup-v2 | 4／2 | d4e635ed3ec17063fece57722f81dc84d9378919ad39835c730e8387dc848c5b |

新19＋既存15＝34独立条件PASS。PDFium／pypdfの対象語不在と保持語、fontToolsの字体輪郭、文字位置0.5pt以内、PDFium／Poppler300dpiの範囲外差分0（従来の1画素境界）を要求する。最大位置差0.285949707pt、回転0／90／180／270・CropBox・UserUnit1.5、対象外ページと注釈外観の画素も検査した。元字体／対応表／対象内容の展開データと対象パス構築命令の除去を検査する。

共有Formの異なる指定で削除語が残る旧出力を再現し、参照と内容hashに基づく局所範囲の一致検査で拒否へ修正した。旧危険出力・exe、パス包絡の失敗、入力生成失敗と環境未完了を保持する。失敗・取消は原本、未保存状態、Undo、保存先を保持する。固定追加条件内の検証であり、M5全体の一般文書安全性や復元不能性の全体受入ではない。

M1独立32検査と従来コピー／スキャン独立検証、77 PE／9資産／106通知の静的監査はPASS。実行依存とOCRモデルを増やしていない。通常ビルドは `scripts/build.ps1 -Target PDFTatsujin`。全体試験はOfficeとPDF/A/UAの固定外部エンジンを明示指定し、ヘッドレスで実行する。設定手順はDEVELOPMENTを参照。独立追加検査は `evaluate-redaction-shared.py`／`evaluate-redaction-paths.py`、旧部分文字は `evaluate-redaction-partial.py`／`evaluate-redaction-composite.py`。

保持字形のヒンティング、CFF／可変字体、独自CMap／多文字Unicode、DR／DA、異なる指定の共有Formを扱う一般化、透明合成・部分図形・複雑clip等は残る。ネイティブ入力・倍率、外部実画面、クリーンWindows、物理印刷、実容量不足、第三者評価、一般配布条件、新exeの通信能力ゼロは未実行／未完了。Poppler文字抽出は未実行。詳細設計は [共有資源](../design/REDACTION_SHARED_RESOURCES.md) と [図形](../design/REDACTION_VECTOR_PATHS.md)、後続検査は対応JSONとローカル報告書に記録する。
