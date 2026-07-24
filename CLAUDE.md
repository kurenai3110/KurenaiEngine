# プロジェクトルール
- コメントは日本語で書く
- 妄想・架空の情報は禁止。事実ベースのみ
- エラーハンドリングは必ず入れる
- プラットフォームはDX11とDX12
- 実装が完了したらコミットとpushをするかユーザーに聞く
- 機能実装時はREADME.mdも合わせて更新する
- ドキュメントはHTMLで書く
- https://github.com/Graphify-Labs/graphify を使用する

# Claudeによる動作確認時の入力操作方針
- マウス操作: 実カーソルを動かす`SetCursorPos`/`mouse_event`ではなく、対象ウィンドウのHWNDへ`PostMessage`/`SendMessage`でメッセージを直接送る方式を使う(実デスクトップのカーソル・フォーカスに影響を与えないため)
- キーボード操作(WASD移動など): エンジンが`GetAsyncKeyState`でグローバルなキー状態を読むため、`PostMessage`では動かせない。WASD移動の動作確認がどうしても必要な場合のみ実キー入力(`keybd_event`等)を使う
- カメラ位置など値の確認: 上記の制約を踏まえ、可能な限り実キー入力によるシミュレーション操作ではなく、一時的なデバッグコードで値を直接指定して検証する方式を優先する

# Claudeによるスクリーンショットの撮り方
- ウィンドウのスクリーンショットを撮る際、`GetWindowRect`はDWMの不可視リサイズ枠を含んだ座標を返すため、これを使って`CopyFromScreen`すると撮影範囲がズレる(枠の分だけ内容が欠けたり、隣接ウィンドウが写り込んだりする)
- 正しい範囲を取得するには`DwmGetWindowAttribute`に`DWMWA_EXTENDED_FRAME_BOUNDS`(値9)を指定し、実際に見えているウィンドウ境界を取得してから`CopyFromScreen`する
- 撮影前に`ShowWindow`(SW_RESTORE)と`SetForegroundWindow`で対象ウィンドウを最前面に出しておく

# Compact instructions
要約するときは、実行したコマンドとその結果、コードの変更内容を優先して残してください。途中の議論や試行錯誤は省いて構いません。