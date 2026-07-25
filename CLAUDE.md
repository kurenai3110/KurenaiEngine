# プロジェクトルール
- コメントは日本語で書く
- 妄想・架空の情報は禁止。事実ベースのみ
- エラーハンドリングは必ず入れる。その際は必ずログを出力すること
- プラットフォームはDX11とDX12
- 実装が完了したらコミットとpushをするかユーザーに聞く
- 機能実装時はREADME.mdも合わせて更新する
- ドキュメントはHTMLで書く
- https://github.com/Graphify-Labs/graphify を使用する

# Claudeによる動作確認時の入力操作方針
- 大原則: 実カーソルを動かす`SetCursorPos`/`mouse_event`や、フォーカスを操作する`SetForegroundWindow`(スクリーンショット撮影時の対象ウィンドウへの適用を除く)は使わない。対象ウィンドウのHWNDへ`PostMessage`/`SendMessage`でメッセージを直接送る方式を使う(実デスクトップのカーソル・フォーカス・他ウィンドウに影響を与えないため)
- 対象アプリを起動する前に、環境変数`KURENAI_INPUT_AUTOMATION=1`を設定しておくこと。`Window`クラスは起動時にこの環境変数を読み取り、`1`が設定されている場合のみ、`PostMessage`で注入した`WM_MOUSEMOVE`直後に発生する実カーソル位置基準の`WM_MOUSELEAVE`ノイズを抑制する(`IsMouseOverWindow()`が意図せず`false`に戻ってホバー/クリック判定が成立しなくなる問題への対策。詳細は`docs/KurenaiEngine.html` 5章参照)。この環境変数が未設定の通常起動では抑制ロジックは完全に無効のままなので、実際のユーザー操作には一切影響しない。ImGuiパネルのクリック確認など、マウスのホバー/クリック判定を伴う自動操作を行う場合は必ず設定してから対象exeを起動すること(WASD等キーボードのみの確認では不要)
- キーボード操作(WASD移動・F1でのImGui表示切替など): `KurenaiEngineBase`はメッセージベースの入力API(`IsKeyDown`/`WasKeyPressed`、`WM_KEYDOWN`/`WM_KEYUP`由来)を持ち、`KurenaiEngine3D`のWASD移動・Shift速度切替・F1トグルも内部でこれを使っている。そのため`PostMessage`で`WM_KEYDOWN`→(必要な間隔を空けて)→`WM_KEYUP`を送るだけで実キー入力なしに動かせる。実キー入力(`keybd_event`等)はもう不要
  - 例外: 右クリックドラッグによる視点回転(マウスルック)だけは、無限ドラッグのため実カーソルの取得・固定(`GetCursorPos`/`SetCursorPos`)を意図的にそのまま使っている設計であり、`PostMessage`では動かせない(構造的に実カーソルへ影響を与えないための設計なので、変更しないこと)。視点回転そのものの動作確認が必要な場合は、下記の「値の確認」方針に従う
- マウス操作(ImGuiパネルのボタンクリックなど): `WM_MOUSEMOVE` → `WM_LBUTTONDOWN` → `WM_LBUTTONUP` の3つを**間隔を空けずに連続で`PostMessage`送信**すること。Dear ImGuiのWin32バックエンドは、ウィンドウがフォーカスされている間かつ直近のマウス位置追跡が途切れると実カーソル位置(`GetCursorPos`)を毎フレームポーリングして上書きするフォールバックを持ち、さらに`TrackMouseEvent`による`WM_MOUSELEAVE`検出も実カーソル位置基準のため、送信間隔を空けると(実カーソルがウィンドウ外にある限り)ほぼ即座に上書きされてクリックが成立しない。間隔を空けずに送ることで上書きが起きる前にクリック判定を完了させる(詳細は`docs/KurenaiEngine.html` 5章参照)
- 絶対にしないこと: ウィンドウのフォーカスを意図的に外す・移す目的で`SetForegroundWindow`を呼ばない(対象外の別ウィンドウへ意図せずフォーカスが移り、ユーザーの他の作業を妨げる実例が過去に発生した)。`SetForegroundWindow`はスクリーンショット撮影のために対象ウィンドウ自身へ適用する場合のみ許可される
- カメラ位置など値の確認: 上記の制約を踏まえてもなお実操作のシミュレーションが難しい場合(マウスルック等)は、一時的なデバッグコードで値を直接指定して検証する方式を優先する

# Claudeによるスクリーンショットの撮り方
- ウィンドウのスクリーンショットを撮る際、`GetWindowRect`はDWMの不可視リサイズ枠を含んだ座標を返すため、これを使って`CopyFromScreen`すると撮影範囲がズレる(枠の分だけ内容が欠けたり、隣接ウィンドウが写り込んだりする)
- 正しい範囲を取得するには`DwmGetWindowAttribute`に`DWMWA_EXTENDED_FRAME_BOUNDS`(値9)を指定し、実際に見えているウィンドウ境界(物理ピクセル)を取得する
- キャプチャ自体は`CopyFromScreen`(実画面のピクセルをコピー)ではなく`PrintWindow`に`PW_RENDERFULLCONTENT`(値2)を指定する方式を使うこと。`CopyFromScreen`は対象ウィンドウが実際に画面の最前面に出ていることが前提だが、自動化スクリプト(バックグラウンドのPowerShellプロセス)から呼ぶ`SetForegroundWindow`はWindowsのフォアグラウンドロックにより戻り値が`true`でも実際には最前面化されないことがある(実際に発生した事例: 無関係な別ウィンドウ(IDE)がスクリーンショットに写り込み続けた)。`PrintWindow`はZオーダー/被覆状態に関係なくDWMにウィンドウの内容を直接描画させるため、フォーカスを一切操作せずに撮影できる(DirectX描画のウィンドウでも`PW_RENDERFULLCONTENT`指定でハードウェアアクセラレーション内容が正しく取得できる)
- 上記2点を実装済みのヘルパーが`Tools/WinAutomation.ps1`にある。動作確認のたびに再実装せず、これをドットソース(`. Tools\WinAutomation.ps1`)して使うこと。日本語コメントを含むPowerShellスクリプトは、UTF-8 BOM無しで保存すると`Add-Type`内のC#コードがWindows PowerShell 5.1のデフォルトANSIコードページで読まれて文字化けし、コンパイルエラーになることがある(実際に発生)。このファイルを編集する場合は`Set-Content -Encoding utf8`(Windows PowerShell 5.1ではBOM付きUTF-8になる)で保存し直すこと

# Claudeによるクリック座標の計算(スクリーンショット→クライアント座標変換)
- スクリーンショットで目視したピクセル位置を、そのまま`PostMessage`の`lParam`(クライアント座標)として使ってはいけない。Windowsのディスプレイ拡大率(例: 150%)が100%でない環境では、ウィンドウの内部解像度(`KurenaiEngine3D`は既定1280x720固定)と実際の画面上の物理ピクセルサイズが一致しないため、スクリーンショット上の座標をそのまま使うとクリック位置が大きくズレる(実際に発生し、ボタンを外し続けてクリックが無反応になった事例がある)
- `ClientToScreen`/`GetWindowRect`を自動化スクリプト側(呼び出し元プロセス)のDPI認識状態のままクロスプロセスで使うと、WindowsのDPI仮想化により値が不整合になることがある(実際に発生: クライアント原点がウィンドウ枠の外側にずれるおかしな値になった)
- 正しく変換するには`GetClientRect`(ウィンドウ自身の論理サイズ。内部解像度そのものなのでDPI仮想化されない)と`DwmGetWindowAttribute`の`DWMWA_EXTENDED_FRAME_BOUNDS`(物理ピクセル)の比率からスケール・タイトルバー高さを直接計算する。`Tools/WinAutomation.ps1`の`ScreenshotPixelToClient`関数がこれを実装済みなので、スクリーンショット上でクリックしたい位置を特定したら必ずこの関数を通してから`ClickClient`に渡すこと

# Compact instructions
要約するときは、実行したコマンドとその結果、コードの変更内容を優先して残してください。途中の議論や試行錯誤は省いて構いません。