# PDF/UA-1の機械検査を共通の規格検証へ加える

作業中のdev22設計。元のSPEC・ACCEPTANCEとPDF/A-1b／2u条件を維持する。意味情報評価の追加範囲として、現在のPDFを読み取り専用でPDF/UA-1の機械検査へ渡す。タグの作成・修復・一般のアクセシビリティ完成を含めない。

「文書ツール→検証・比較→PDF規格を検証」でPDF/A-1b・PDF/A-2u・PDF/UA-1（機械検査）を選ぶ。同じJava・veraPDF設定と非同期の専用プロセスを使い、保存済み原本はロード時SHAに一致する元バイト、未保存変更は候補の別SHAを検査する。原本・未保存変更・署名・Undoを変更しない。暗号化を無断解除せず、取消・時間切れ・エンジン不在・XML不正を完了と表示しない。

プロファイルのCLI ID・画面名・報告名・機械検査に限るかを一つの定義へ集める。既存PDF/A処理を別実装へ複製せず、中立な規格検証モジュールへ名前と責務を整理する。PDF/Aの適合／不適合の意味と、既存JSONのcompliantは維持する。PDF/UAの成功は「機械検査に適合」、失敗は「機械検査で不適合」。成功時も読み順・代替説明・見出し／表・実際の読み上げ等の人による確認を案内し、全体の適合を未判定と表示する。

PDF/UAのJSONはmachine_compliant、validation_scope=machine-verifiable、full_conformance=not-determinedを持ち、全体適合と取り違えるcompliantキーを置かない。結果コピーにも範囲と未判定を含める。編集・プロファイルや実行先の変更では古い結果を失効させ、既存結果ファイルを上書きしない。

実装前に上流veraPDF corpusの固定コミット・既存pass／fail4文書172,273バイトを取得し、Git blob・SHAと分類を凍結した。既存固定Java21・veraPDF1.30.3による技術試験で4分類が一致した。新しい大容量の依存は不要。固定PDFとcriteria.jsonは変更せず、署名入り候補・不正／別プロファイルXML・取消・不足エンジン・実UI・結果保存・検査後の変更も製品で試験する。既存PDF/A3試験と全回帰を維持する。技術検証の成功を製品UI試験の実行済みと表示しない。

出典・利用条件はfixtures/pdfua-validation/README.mdへ記録する。自作コードはMIT、corpusは上流指定のCC BY 4.0。外部Java／veraPDFは既存の固定版・通知を再利用し、通常アプリZIPへ新たに同梱しない。以前のAppContainer FAILと配布条件未確認を保持する。

一次資料：[veraPDFの検査範囲](https://docs.verapdf.org/validation/)はPDF/UAで機械検証可能な項目だけを検査すると明記する。[CLI](https://docs.verapdf.org/cli/validation/)と[Help](https://docs.verapdf.org/cli/help/)のua1指定を固定ローカル版の実出力で確認した。[PDF AssociationのMatterhorn Protocol](https://pdfa.org/resource/the-matterhorn-protocol/)は機械と人の判断を区別する。ネイティブGUI・クリーンWindows・スクリーンリーダー・人の意味評価は実行できた項目だけを報告し、未実行を明示する。

Windows実ビルドと実Qt UI／エンジンのPDF/UA3試験、既存PDF/A3試験が成功。機械検査の適合と不適合の実画面、コピー／結果保存、取消・プロフィール切替時の失効、原本・署名・Undo保持を確認した。独立Pythonの直接実行でPDF/UA7出力（固定4文書・適合／不適合UI2・署名入り未保存候補1）とPDF/A5出力が一致した。署名を追加した候補はAnnotタグ不足の機械不適合を報告した。元の適合結果を変更後の候補へ流用せず、タグを自動作成しないことを確認した。一般出荷・人の判断・全体適合の成功ではない。配布候補全回帰と実ZIP追試は別に実行する。
