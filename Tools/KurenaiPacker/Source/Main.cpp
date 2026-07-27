// KurenaiPacker: KurenaiEngine専用モデルパッケージ(.kmodel/.kgeom/.ktex)を生成する
// オフラインのアセットビルドツール。従来ランタイムの初回読み込みで行っていた
// assimp解析・WICデコード・ミップ生成・GPU BC7圧縮をすべて事前に完了させる。
// あわせて、.kscene(シーンファイル)の検証・配置も行う(--sceneモード)。
//
// 使い方:
//   KurenaiPacker.exe <入力モデル> -o <出力.kmodel> [--force] [--jobs N] [--scale S]
//   KurenaiPacker.exe --scene <入力.kscene> -o <出力.kscene>
//
// --sceneモードは.ksceneの書式(セクション/キー/数値範囲)を検証し、参照している
// 各[Model]Pathの.kmodelが実在してヘッダが読めることまで確認したうえで、そのまま
// 出力先へコピーする(バイナリ変換は行わない)。[Model]Pathの基準ディレクトリ
// (Assetsルート)は、出力パスから「Scenesフォルダの1つ上」として推定する
// (ランタイムの<DLLフォルダ>/Assets/Scenes/*.ksceneという配置と対応させるため)。
//
// 終了コード: 成功0 / 引数エラー1 / 入力読み込み失敗2 / 書き出し失敗3
//
// コンソール出力は常にstd::cout/cerrへUTF-8の生バイト列として書く(std::wcout/wcerrは
// 使わない)。wcout/wcerrは出力先が実コンソールでない場合(ファイル/パイプへのリダイレクト、
// CIログ取得など)にワイド文字ストリームのロケール変換で失敗しfailbitが立つことがあり、
// 以降の出力が丸ごと欠落する事故につながる(実際にこのツールの開発中、パイプ経由で
// 実行した際にヘルプメッセージが1行目で切れる形で発生した)。SetConsoleOutputCP(CP_UTF8)は
// 実コンソールへ出す場合の表示のためだけに設定し、パスやエラーメッセージ(std::wstring)は
// 必ずCore::WideToUtf8で変換してからstd::coutへ渡す

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "Assets/SceneLoader.h"
#include "Core/StringUtil.h"
#include "ModelSource.h"
#include "PackageWriter.h"

using Kurenai::Core::Utf8ToWide;
using Kurenai::Core::WideToUtf8;
namespace fs = std::filesystem;

namespace
{
    void PrintUsage()
    {
        std::cout <<
            "KurenaiPacker - KurenaiEngine専用モデルパッケージ生成ツール\n"
            "\n"
            "使い方:\n"
            "  KurenaiPacker.exe <入力モデル> -o <出力.kmodel> [オプション]     モデルをパックする\n"
            "  KurenaiPacker.exe --scene <入力.kscene> -o <出力.kscene>         シーンを検証して配置する\n"
            "\n"
            "オプション:\n"
            "  -o, --output <path>   出力先のパス(必須)。モデルモードではこの親ディレクトリが\n"
            "                        .kgeomと.ktexのミラー先ルートになる\n"
            "      --scene <path>    <入力モデル>の代わりに.ksceneを検証・配置するモードにする\n"
            "      --force           既存の.ktexがあっても再圧縮して上書きする(モデルモードのみ)\n"
            "      --jobs <N>        テクスチャ処理のワーカースレッド数(既定: 論理コア数、上限8。モデルモードのみ)\n"
            "      --scale <S>       頂点位置・バウンズに乗算する係数(既定1.0、モデルモードのみ)。\n"
            "                        OBJ等ファイル自体に単位情報を持たない形式で、センチメートル単位の\n"
            "                        アセットをメートル単位として読み込みたい場合は0.01を指定する\n"
            "  -h, --help            このヘルプを表示する\n";
    }

    void PrintError(const std::string& message)
    {
        std::cerr << "[KurenaiPacker][Error] " << message << "\n";
    }

    struct CommandLineArgs
    {
        std::wstring InputPath;
        std::wstring OutputPath;
        bool Force = false;
        unsigned int JobCount = 0;
        float Scale = 1.0f;
        bool ShowHelp = false;
        bool SceneMode = false;
    };

    // 失敗時はstd::nulloptを返し、呼び出し側でエラーメッセージ表示・終了コード1とする
    std::optional<CommandLineArgs> ParseArgs(int argc, wchar_t** argv)
    {
        CommandLineArgs args;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring arg = argv[i];
            if (arg == L"-h" || arg == L"--help")
            {
                args.ShowHelp = true;
            }
            else if (arg == L"-o" || arg == L"--output")
            {
                if (i + 1 >= argc)
                {
                    PrintError(WideToUtf8(arg) + " には値が必要です");
                    return std::nullopt;
                }
                args.OutputPath = argv[++i];
            }
            else if (arg == L"--scene")
            {
                if (i + 1 >= argc)
                {
                    PrintError("--scene には値が必要です");
                    return std::nullopt;
                }
                args.SceneMode = true;
                args.InputPath = argv[++i];
            }
            else if (arg == L"--force")
            {
                args.Force = true;
            }
            else if (arg == L"--jobs")
            {
                if (i + 1 >= argc)
                {
                    PrintError("--jobs には値が必要です");
                    return std::nullopt;
                }
                try
                {
                    args.JobCount = static_cast<unsigned int>(std::stoul(argv[++i]));
                }
                catch (const std::exception&)
                {
                    PrintError("--jobs の値が不正です: " + WideToUtf8(argv[i]));
                    return std::nullopt;
                }
            }
            else if (arg == L"--scale")
            {
                if (i + 1 >= argc)
                {
                    PrintError("--scale には値が必要です");
                    return std::nullopt;
                }
                try
                {
                    args.Scale = std::stof(argv[++i]);
                }
                catch (const std::exception&)
                {
                    PrintError("--scale の値が不正です: " + WideToUtf8(argv[i]));
                    return std::nullopt;
                }
            }
            else if (!arg.empty() && arg[0] == L'-')
            {
                PrintError("不明なオプションです: " + WideToUtf8(arg));
                return std::nullopt;
            }
            else if (args.InputPath.empty())
            {
                args.InputPath = arg;
            }
            else
            {
                PrintError("入力モデルは1つだけ指定できます");
                return std::nullopt;
            }
        }
        return args;
    }

    std::string FormatMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
    {
        return std::to_string(static_cast<long long>(std::chrono::duration<double, std::milli>(end - start).count()));
    }

    // .tmpへ書いてから完了時のみ本来のパスへリネームする(モデル/テクスチャ書き出しと同じ設計)
    void CopyFileAtomic(const fs::path& source, const fs::path& destination)
    {
        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);

        const fs::path tempPath = fs::path(destination).concat(L".tmp");
        std::ifstream in(source, std::ios::binary);
        if (!in.is_open())
        {
            throw std::runtime_error("コピー元を開けませんでした: " + WideToUtf8(source.wstring()));
        }
        {
            std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                throw std::runtime_error("コピー先を書き込めませんでした: " + WideToUtf8(tempPath.wstring()));
            }
            out << in.rdbuf();
            if (!out)
            {
                throw std::runtime_error("コピー先への書き込みに失敗しました: " + WideToUtf8(tempPath.wstring()));
            }
        }
        if (!MoveFileExW(tempPath.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            throw std::runtime_error("コピー先の確定(rename)に失敗しました: " + WideToUtf8(destination.wstring()));
        }
    }

    // --sceneモード: .ksceneを検証してそのまま出力先へ配置する(バイナリ変換は行わない)。
    // [Model]Pathの基準ディレクトリ(Assetsルート)は、出力パスの「Scenesフォルダの1つ上」と
    // 推定する(ランタイムの<DLLフォルダ>/Assets/Scenes/*.ksceneという配置に対応させるため)
    int RunSceneMode(const CommandLineArgs& args)
    {
        std::error_code ec;
        const fs::path inputAbsolute = fs::absolute(fs::path(args.InputPath), ec);
        if (ec || !fs::exists(inputAbsolute))
        {
            PrintError("入力シーンファイルが見つかりません: " + WideToUtf8(args.InputPath));
            return 2;
        }

        const fs::path outputAbsolute = fs::absolute(fs::path(args.OutputPath), ec);
        const fs::path scenesDirectory = outputAbsolute.parent_path();
        const std::wstring assetRootDirectory = scenesDirectory.parent_path().wstring() + L"/";

        try
        {
            Kurenai::Assets::ValidateScene(inputAbsolute.wstring(), assetRootDirectory);
        }
        catch (const std::exception& e)
        {
            PrintError("シーンファイルの検証に失敗しました: " + std::string(e.what()));
            return 2;
        }

        try
        {
            CopyFileAtomic(inputAbsolute, outputAbsolute);
        }
        catch (const std::exception& e)
        {
            PrintError("シーンファイルの配置に失敗しました: " + std::string(e.what()));
            return 3;
        }

        std::cout << "[KurenaiPacker] シーン検証・配置完了: " << WideToUtf8(args.OutputPath) << "\n";
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    // 実コンソールに表示する場合の表示のためだけに設定する(パイプ/ファイルへの
    // リダイレクト時は無効だが、その場合でもstd::coutはUTF-8の生バイト列を書くだけなので
    // 問題なく動作する)
    SetConsoleOutputCP(CP_UTF8);

    const std::optional<CommandLineArgs> parsedArgs = ParseArgs(argc, argv);
    if (!parsedArgs)
    {
        PrintUsage();
        return 1;
    }
    const CommandLineArgs& args = *parsedArgs;

    if (args.ShowHelp)
    {
        PrintUsage();
        return 0;
    }

    if (args.InputPath.empty() || args.OutputPath.empty())
    {
        PrintError(args.SceneMode
            ? "--scene と -o/--output の両方の指定が必要です"
            : "入力モデルと -o/--output の両方の指定が必要です");
        PrintUsage();
        return 1;
    }

    if (args.SceneMode)
    {
        return RunSceneMode(args);
    }

    const fs::path inputPath = fs::path(args.InputPath);
    std::error_code ec;
    const fs::path inputAbsolute = fs::absolute(inputPath, ec);
    if (ec || !fs::exists(inputAbsolute))
    {
        PrintError("入力モデルが見つかりません: " + WideToUtf8(args.InputPath));
        return 2;
    }
    const std::wstring sourceModelDirectory = inputAbsolute.parent_path().wstring();

    const auto startTime = std::chrono::steady_clock::now();

    KurenaiPacker::SourceModel sourceModel;
    try
    {
        sourceModel = KurenaiPacker::LoadSourceModel(inputAbsolute.wstring(), args.Scale);
    }
    catch (const std::exception& e)
    {
        PrintError("入力モデルの解析に失敗しました: " + std::string(e.what()));
        return 2;
    }

    const auto parseTime = std::chrono::steady_clock::now();

    KurenaiPacker::PackOptions options;
    options.Force = args.Force;
    options.JobCount = args.JobCount;

    KurenaiPacker::PackResult result;
    try
    {
        result = KurenaiPacker::WriteModelPackage(sourceModel, fs::absolute(args.OutputPath, ec).wstring(), sourceModelDirectory, options);
    }
    catch (const std::exception& e)
    {
        PrintError("パッケージの書き出しに失敗しました: " + std::string(e.what()));
        return 3;
    }

    const auto endTime = std::chrono::steady_clock::now();

    std::cout
        << "[KurenaiPacker] パック完了: " << WideToUtf8(args.OutputPath) << "\n"
        << "  メッシュ数: " << result.MeshCount
        << " / 頂点数: " << result.VertexCount
        << " / インデックス数: " << result.IndexCount << "\n"
        << "  テクスチャ要求: " << result.TextureRequested
        << " (新規生成 " << result.TextureGenerated
        << " / 既存スキップ " << result.TextureSkippedExisting
        << " / 失敗(フォールバック) " << result.TextureFailed << ")\n"
        << "  所要時間: 解析 " << FormatMs(startTime, parseTime) << "ms"
        << " / 書き出し " << FormatMs(parseTime, endTime) << "ms"
        << " / 合計 " << FormatMs(startTime, endTime) << "ms\n";

    return 0;
}
