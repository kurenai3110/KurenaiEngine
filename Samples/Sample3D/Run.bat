@echo off
setlocal enabledelayedexpansion

rem ===========================================================================
rem 【このファイルの保存形式について】改行は CRLF、文字コードは CP932(Shift_JIS)。
rem
rem ・改行を LF のみにしてはいけない。cmd.exe はバッチファイルをバイト位置を
rem   保持しながら逐次読み込み、goto も同じくバイト位置でラベルを探すため、
rem   LF のみだと読み取り位置が1バイトずつずれていき、以降の行が単語の途中で
rem   切れて実行される(実際に `echo` が `cho`、`goto` が `oto` として実行され、
rem   引数解析が最後まで到達しない壊れ方をした)。日本語の有無に関係なく壊れる
rem ・文字コードは CP932。cmd.exe は読み込んだバイト列をコンソールのコード
rem   ページ(日本語版 Windows の既定は 932)として解釈するため、UTF-8 で保存すると
rem   このファイル内の日本語が化ける
rem ・BOM も付けないこと(先頭の @echo off より前に BOM のバイト列が現れ、
rem   「'?' は認識されていません」になる)
rem ・このファイルの中で chcp を実行しないこと。コードページの固定は、日本語を
rem   一切含まない RunDX11.bat / RunDX12.bat 側で行っている
rem ===========================================================================

rem ===========================================================================
rem Sample3D 起動スクリプト(共通実装)
rem
rem   Run.bat [-dx12] [-debug|-release] [その他の引数...]
rem
rem DX11/DX12 は同一の Sample3D.exe を引数で切り替えて起動する。DX12 で起動するには
rem -dx12 が必要で、付け忘れると黙って DX11 で動くため、通常は同じフォルダの
rem RunDX12.bat / RunDX11.bat から呼ぶこと。
rem
rem 構成を明示しない場合は Release を優先し、無ければ Debug へフォールバックする。
rem 認識しない引数はそのまま Sample3D.exe へ渡す。
rem
rem 起動前に「exe があるか」「シーン(.kscene)が配置されているか」だけを確認する。
rem .kscene が参照する .kmodel の欠落まではここでは検出できない(その場合は
rem エンジン側がログにエラーを残して該当シーンを一覧から外す)。
rem ===========================================================================

set "SCRIPT_DIR=%~dp0"
rem %%~fI で ..\.. を解決した絶対パスにする(エラーメッセージに生のまま出さないため)
for %%I in ("%~dp0..\..") do set "REPO_ROOT=%%~fI"
set "LOG_FILE=%~dp0Run.log"
set "GRAPHICS_API="
set "API_NAME=DX11"
set "CONFIG="
set "PASSTHROUGH="
set "EXIT_CODE=0"

pushd "%SCRIPT_DIR%"

>>"%LOG_FILE%" echo.
>>"%LOG_FILE%" echo ----------------------------------------------------------------
call :Log "Sample3D 起動スクリプトを開始しました" Info

rem --- 引数の解析 -----------------------------------------------------------
rem shift しても %* は変化しないため、exe へ渡す引数は自前で積み直す。
rem shift を括弧ブロックの中に書くと解釈が分かりにくくなるため、1引数ごとに
rem ラベルへ飛ばす形にしてある
:ParseArgs
if "%~1"=="" goto :ParseArgsDone
if /i "%~1"=="-dx12"    goto :ArgDX12
if /i "%~1"=="-debug"   goto :ArgDebug
if /i "%~1"=="-release" goto :ArgRelease
if /i "%~1"=="-h"       goto :Usage
if /i "%~1"=="--help"   goto :Usage
if /i "%~1"=="-?"       goto :Usage
set "PASSTHROUGH=!PASSTHROUGH! %~1"
shift
goto :ParseArgs

:ArgDX12
set "GRAPHICS_API=-dx12"
set "API_NAME=DX12"
shift
goto :ParseArgs

:ArgDebug
set "CONFIG=Debug"
shift
goto :ParseArgs

:ArgRelease
set "CONFIG=Release"
shift
goto :ParseArgs

:ParseArgsDone

rem --- 構成(Debug/Release)の決定 -------------------------------------------
if defined CONFIG (
    set "OUT_DIR=%SCRIPT_DIR%Build\Bin\x64\!CONFIG!"
    if not exist "!OUT_DIR!\Sample3D.exe" (
        call :Log "指定された構成 !CONFIG! の Sample3D.exe がありません" Error
        goto :NoExe
    )
) else (
    rem 明示指定が無い場合は Release を優先し、無ければ Debug を使う
    set "CONFIG=Release"
    set "OUT_DIR=%SCRIPT_DIR%Build\Bin\x64\Release"
    if not exist "!OUT_DIR!\Sample3D.exe" (
        set "CONFIG=Debug"
        set "OUT_DIR=%SCRIPT_DIR%Build\Bin\x64\Debug"
    )
    if not exist "!OUT_DIR!\Sample3D.exe" goto :NoExe
)

set "EXE=%OUT_DIR%\Sample3D.exe"

rem どのバイナリが起動したのか後から分かるよう、更新日時まで記録する
rem (Debug をビルドしたつもりで古い Release が起動する事故に気付けるようにするため)
for %%F in ("%EXE%") do set "EXE_TIMESTAMP=%%~tF"
call :Log "バックエンド: %API_NAME% / 構成: %CONFIG%" Info
call :Log "実行ファイル: %EXE% (更新日時: %EXE_TIMESTAMP%)" Info

rem --- シーン(.kscene)の確認 ------------------------------------------------
rem エンジンは実行ファイルと同じフォルダの Assets\Scenes\*.kscene を列挙する。
rem リポジトリ側の Assets\Packed\Scenes\ とは別物で、ビルド後イベントでコピーされる
dir /b "%OUT_DIR%\Assets\Scenes\*.kscene" >nul 2>&1
if errorlevel 1 (
    dir /b "%REPO_ROOT%\Assets\Packed\Scenes\*.kscene" >nul 2>&1
    if errorlevel 1 ( goto :NoSceneAtAll ) else ( goto :NoSceneInOutDir )
)

rem --- 起動 -----------------------------------------------------------------
rem 出力フォルダをカレントにしてから起動する。Sample3D.exe は初期化に失敗すると
rem カレントディレクトリへ error.log を書くため、エンジンのログと同じ場所に集まる。
rem
rem exe はカレント依存にせずフルパスで起動する。環境変数
rem NoDefaultCurrentDirectoryInExePath が設定された環境では、cmd.exe が実行ファイルを
rem カレントディレクトリから探さなくなり「'Sample3D.exe' は認識されていません」で
rem 起動できないため(実際に発生した)
call :Log "起動します: Sample3D.exe %GRAPHICS_API%%PASSTHROUGH%" Info
pushd "%OUT_DIR%"
"%EXE%" %GRAPHICS_API%%PASSTHROUGH%
set "EXIT_CODE=!ERRORLEVEL!"
popd

if not "%EXIT_CODE%"=="0" (
    call :Log "Sample3D.exe が終了コード %EXIT_CODE% で終了しました" Error
    call :Log "詳細は %OUT_DIR%\KurenaiEngine_%API_NAME%.log と %OUT_DIR%\error.log を確認してください" Error
    echo.
    pause
) else (
    call :Log "正常に終了しました" Info
)
goto :End

rem === エラー処理 ===========================================================

:NoExe
call :Log "Sample3D.exe が見つかりません: %SCRIPT_DIR%Build\Bin\x64\[構成]\Sample3D.exe" Error
echo.
echo 「x64 Native Tools Command Prompt for VS 2022」をリポジトリのルートで開き、
echo 次のコマンドでビルドしてください:
echo.
echo   MSBuild Samples\Sample3D\Sample3D.sln /p:Configuration=Release /p:Platform=x64
echo.
echo 初回は ThirdParty の assimp・DirectXTex のビルドが先に必要です。
echo 詳しくは README.md の「セットアップ」を参照してください。
echo.
set "EXIT_CODE=1"
pause
goto :End

:NoSceneAtAll
call :Log "表示できるシーン(.kscene)がありません" Error
echo.
echo   実行時の参照先  : %OUT_DIR%\Assets\Scenes\
echo   リポジトリ側の元: %REPO_ROOT%\Assets\Packed\Scenes\
echo.
echo どちらにも .kscene が無いため、このまま起動しても
echo 「有効なシーンファイル(.kscene)が見つかりませんでした」で終了します。
echo KurenaiPacker でアセットを用意してください(README.md「5. アセットの準備」参照):
echo.
echo   MSBuild Tools\KurenaiPacker\KurenaiPacker.sln /p:Configuration=Release /p:Platform=x64
echo   Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^^
echo     Assets\Source\Sponza\Sponza.gltf -o Assets\Packed\Sponza\Sponza.kmodel
echo   Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^^
echo     --scene Scenes\Sponza.kscene -o Assets\Packed\Scenes\Sponza.kscene
echo.
set "EXIT_CODE=1"
pause
goto :End

:NoSceneInOutDir
call :Log "出力フォルダに .kscene がコピーされていません: %OUT_DIR%\Assets\Scenes\" Error
echo.
echo リポジトリ側(%REPO_ROOT%\Assets\Packed\Scenes\)には .kscene があります。
echo Sample3D を再ビルドすると、ビルド後イベントで出力フォルダへコピーされます:
echo.
echo   MSBuild Samples\Sample3D\Sample3D.sln /p:Configuration=%CONFIG% /p:Platform=x64
echo.
set "EXIT_CODE=1"
pause
goto :End

:Usage
echo Sample3D 起動スクリプト
echo.
echo   Run.bat [-dx12] [-debug^|-release] [その他の引数...]
echo.
echo   -dx12      DX12 バックエンドで起動する(既定は DX11)
echo   -debug     Debug 構成を使う
echo   -release   Release 構成を使う
echo   上記以外の引数は Sample3D.exe へそのまま渡されます。
echo.
echo 構成を指定しない場合は Release を優先し、無ければ Debug を使います。
echo 通常は RunDX12.bat / RunDX11.bat から起動してください。
set "EXIT_CODE=0"
goto :End

rem === 共通処理 =============================================================

rem コンソールとログファイルの両方へ1行出力する。
rem 書式はエンジンの Logger (KurenaiEngine_DX11.log など)に合わせてある。
rem
rem ファイルへの追記でリダイレクトを先に書いているのは、`echo ...%~1>>"file"` の形だと
rem メッセージが数字で終わったときに末尾の数字が `1>>`(標準出力のリダイレクト指定)として
rem 解釈され、その数字が消えてしまうため(例: 「終了コード 1」→「終了コード 」)
:Log
echo [%~2] %~1
>>"%LOG_FILE%" echo [%DATE% %TIME%][Run][%~2] %~1
goto :eof

:End
popd
endlocal & exit /b %EXIT_CODE%
