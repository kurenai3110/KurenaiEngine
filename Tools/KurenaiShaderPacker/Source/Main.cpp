// KurenaiShaderPacker: HLSLをビルド時にコンパイルし、KurenaiEngine専用の
// シェーダーパッケージ(.kshader)を生成するオフラインツール。
// KurenaiEngine3D / KurenaiEngine2D のビルドイベントから呼ばれる。
//
// 使い方:
//   KurenaiShaderPacker.exe --input <Shadersフォルダ> --output <出力フォルダ>
//                           [--config Debug|Release] [--force] [--jobs N]
//   KurenaiShaderPacker.exe --dump <file.kshader>
//
// 従来は .hlsl を出力フォルダへコピーするだけで、アプリの起動時にその場でコンパイルしていた。
// 実測(RTX 4070 Ti / Release / Sample3D)でDX11は約17.6秒、DX12は約2.1秒をそこで使っていた。
//
// 終了コード: 成功0 / 引数エラー1 / 入力読み込み失敗2 / コンパイルまたは書き出し失敗3
//
// コンソール出力はstd::cout/cerrへ書く(std::wcout/wcerrは使わない。理由はKurenaiPackerの
// Main.cpp冒頭のコメントと同じ)。ただしこのツールはMSBuildのビルドイベントから呼ばれ、
// 標準出力がパイプへリダイレクトされるため、書く直前にToConsoleEncodingで
// 出力先の文字コードへ直す(PrintOut/PrintErrを必ず通すこと)

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "Assets/ShaderPackage.h"
#include "Core/StringUtil.h"
#include "ShaderCompiler.h"
#include "ShaderEntryScanner.h"
#include "ShaderPackageWriter.h"

using Kurenai::Core::Utf8ToWide;
using Kurenai::Core::WideToUtf8;
using namespace Kurenai::Assets;
using namespace Kurenai::ShaderPacker;
namespace fs = std::filesystem;

namespace
{
    // UTF-8の文字列を、実際の出力先が読める文字コードへ直す。
    //
    // 【SetConsoleOutputCP(CP_UTF8)だけでは足りない】あれが効くのは実コンソールへ出す場合だけ。
    // このツールはMSBuildのビルドイベントから呼ばれ、標準出力はパイプへリダイレクトされる。
    // その場合MSBuildはANSI(日本語環境ではCP932)として読むため、UTF-8のまま書くと
    // ビルドログの日本語がすべて文字化けする。リダイレクトされているときだけ変換する
    std::string ToConsoleEncoding(const std::string& utf8)
    {
        static const bool redirected = []() {
            DWORD mode = 0;
            return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) == 0;
        }();
        if (!redirected || utf8.empty())
        {
            return utf8;
        }

        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wideLength <= 0) { return utf8; }
        std::wstring wide(static_cast<size_t>(wideLength), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), wideLength);

        const int narrowLength =
            WideCharToMultiByte(CP_ACP, 0, wide.c_str(), wideLength, nullptr, 0, nullptr, nullptr);
        if (narrowLength <= 0) { return utf8; }
        std::string narrow(static_cast<size_t>(narrowLength), '\0');
        WideCharToMultiByte(CP_ACP, 0, wide.c_str(), wideLength, narrow.data(), narrowLength, nullptr, nullptr);
        return narrow;
    }

    // 出力先の文字コードへ直して1行書く。std::wcout/wcerrは使わない
    // (リダイレクト時にロケール変換でfailbitが立ち、以降の出力が丸ごと欠落することがある)
    void PrintOut(const std::string& utf8) { std::cout << ToConsoleEncoding(utf8); }
    void PrintErr(const std::string& utf8) { std::cerr << ToConsoleEncoding(utf8); }

    // Dxil65(bindless無し / SM 6.5)を焼かないファイル。
    //
    // 【ここに足すときの基準】「そのバリアントが選ばれる実行環境では、エンジンがそのシェーダーを
    // 一度も生成しない」ことをエンジン側のコードで確かめてから足すこと。
    // 単にコンパイルが通らないから、で足してはいけない ―― それは本物の破壊を握り潰すことになる。
    const char* const kSkipDxil65Files[] = {
        // 自前ソフトウェアラスタライザは64bit整数アトミックを使い、これはSM 6.6の機能。
        // DX12Device::m_SupportsSoftwareRaster が「bindlessと64bit整数アトミックの両方」を
        // 要求しているため、SM 6.5の環境ではこのシェーダーは生成されない
        "SoftwareRaster.hlsl",
        "SoftwareRasterResolve.hlsl",
        // DDGIのプローブ取得CSは、コンピュートシェーダー内でミップを選ぶためにテクスチャを
        // 微分付きでサンプルする。DXILの検証が "Derivatives in CS/MS/AS is SM 6.6+" で弾く。
        // このシェーダーはレイトレーシング経路(DXR Tier 1.1)でのみ生成され、そこは
        // 実際にはSM 6.6のデバイスしか通らない
        "DDGIProbeTrace.hlsl",
        // メッシュレット描画はジオメトリを bindless で引くため、KURENAI_BINDLESS が無いと
        // ファイル全体がコンパイルできない(KURENAI_BINDLESS_BUFFER をガード無しで使っている)。
        // DX12Device::DetectMeshShaderSupport は bindless 判定の後に走り、bindless非対応なら
        // SupportsMeshShader() が false になるため、このファイルは SM 6.5 環境では使われない。
        // 【インクルード展開で VSMain も対象になる】GBufferCommon.hlsli の VSMain が
        // このファイルのエントリとしても検出されるが、エンジンは要求しない。
        // ファイル単位で除外するのが正しい
        "GBufferMeshlet.hlsl",
    };

    bool ShouldSkipDxil65(const std::string& fileName)
    {
        for (const char* name : kSkipDxil65Files)
        {
            if (fileName == name)
            {
                return true;
            }
        }
        return false;
    }

    void PrintUsage()
    {
        PrintOut(
            "KurenaiShaderPacker - HLSLを事前コンパイルして.kshaderを生成するツール\n"
            "\n"
            "使い方:\n"
            "  KurenaiShaderPacker.exe --input <Shadersフォルダ> --output <出力フォルダ> [オプション]\n"
            "  KurenaiShaderPacker.exe --dump <file.kshader>\n"
            "\n"
            "オプション:\n"
            "  --config <Debug|Release>  コンパイルフラグを切り替える(既定: Release)\n"
            "  --force                   更新日時に関わらず全ファイルを焼き直す\n"
            "  --jobs <N>                並列数(既定: 論理コア数、上限16)\n");
    }

    struct Options
    {
        fs::path Input;
        fs::path Output;
        fs::path DumpPath;
        bool DebugBuild = false;
        bool Force = false;
        unsigned Jobs = 0;
    };

    bool ParseArguments(int argc, wchar_t** argv, Options& options, std::string& outError)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring arg = argv[i];
            auto next = [&](std::wstring& out) -> bool {
                if (i + 1 >= argc)
                {
                    outError = WideToUtf8(arg) + " に値が指定されていません";
                    return false;
                }
                out = argv[++i];
                return true;
            };

            std::wstring value;
            if (arg == L"--input")
            {
                if (!next(value)) { return false; }
                options.Input = value;
            }
            else if (arg == L"--output")
            {
                if (!next(value)) { return false; }
                options.Output = value;
            }
            else if (arg == L"--dump")
            {
                if (!next(value)) { return false; }
                options.DumpPath = value;
            }
            else if (arg == L"--config")
            {
                if (!next(value)) { return false; }
                if (value == L"Debug") { options.DebugBuild = true; }
                else if (value == L"Release") { options.DebugBuild = false; }
                else { outError = "--config には Debug か Release を指定してください"; return false; }
            }
            else if (arg == L"--force")
            {
                options.Force = true;
            }
            else if (arg == L"--jobs")
            {
                if (!next(value)) { return false; }
                options.Jobs = static_cast<unsigned>(std::max(1, _wtoi(value.c_str())));
            }
            else
            {
                outError = "不明な引数です: " + WideToUtf8(arg);
                return false;
            }
        }
        return true;
    }

    bool ReadTextFile(const fs::path& path, std::string& outText)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) { return false; }
        const std::streamoff size = in.tellg();
        if (size < 0) { return false; }
        in.seekg(0, std::ios::beg);
        outText.resize(static_cast<size_t>(size));
        if (size > 0 && !in.read(outText.data(), size)) { return false; }
        return true;
    }

    // このexe自身のパス(増分判定に使う。コンパイルフラグを変えたら焼き直したいため)
    fs::path GetExecutablePath()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return fs::path(path);
    }

    // 1ファイル分の処理結果
    struct FileResult
    {
        std::string FileName;
        bool Succeeded = true;
        int EntryCount = 0;
        size_t BytecodeSize = 0;
        std::vector<std::string> Errors;
        std::vector<std::string> Warnings;
    };
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    Options options;
    std::string error;
    if (!ParseArguments(argc, argv, options, error))
    {
        PrintErr("エラー: " + error + "\n\n");
        PrintUsage();
        return 1;
    }

    if (!options.DumpPath.empty())
    {
        if (!DumpShaderPackage(options.DumpPath.wstring(), error))
        {
            PrintErr("エラー: " + error + "\n");
            return 2;
        }
        return 0;
    }

    if (options.Input.empty() || options.Output.empty())
    {
        PrintErr("エラー: --input と --output は必須です\n\n");
        PrintUsage();
        return 1;
    }

    std::error_code ec;
    if (!fs::is_directory(options.Input, ec))
    {
        PrintErr("エラー: 入力フォルダがありません: " + WideToUtf8(options.Input.wstring()) + "\n");
        return 2;
    }

    // 対象は入力フォルダ直下の .hlsl のみ。.hlsli はインクルード専用でエントリを持たない。
    // 【再帰しない理由】ランタイムは Shaders\<名前>.kshader という平坦な配置を前提に
    // パスを組み立てる(KurenaiEngine3D.cpp の shaderDirectory + L"GBuffer.kshader")。
    // サブフォルダを掘ると出力側で名前が衝突しうるため、掘られていたら明示的に落とす
    std::vector<fs::path> shaderFiles;
    std::vector<fs::path> allSources;
    for (const auto& entry : fs::directory_iterator(options.Input, ec))
    {
        if (entry.is_directory())
        {
            for (const auto& sub : fs::recursive_directory_iterator(entry.path(), ec))
            {
                if (sub.is_regular_file() && sub.path().extension() == L".hlsl")
                {
                    PrintErr("エラー: サブフォルダの .hlsl には対応していません: " + WideToUtf8(sub.path().wstring()) + "\n");
                    return 2;
                }
            }
            continue;
        }
        if (!entry.is_regular_file()) { continue; }
        const std::wstring ext = entry.path().extension().wstring();
        if (ext == L".hlsl")
        {
            shaderFiles.push_back(entry.path());
            allSources.push_back(entry.path());
        }
        else if (ext == L".hlsli")
        {
            allSources.push_back(entry.path());
        }
    }
    std::sort(shaderFiles.begin(), shaderFiles.end());

    if (shaderFiles.empty())
    {
        PrintErr("エラー: .hlsl が1つも見つかりません: " + WideToUtf8(options.Input.wstring()) + "\n");
        return 2;
    }

    // 増分判定の基準時刻。
    // 【.hlsli が1本でも更新されていれば全部焼き直す】どの .hlsl がどの .hlsli を
    // インクルードしているかを静的に追うのは、条件付きインクルードや多段インクルードがあると
    // 正確にはできない。取りこぼすと「古いバイトコードのまま起動する」という、
    // 気付きにくく原因も追いにくい壊れ方をするので、安全側に倒す。
    // 自分自身(exe)の更新時刻も含める ―― コンパイルフラグを変えたら焼き直す必要があるため
    fs::file_time_type newestSource = fs::file_time_type::min();
    for (const fs::path& src : allSources)
    {
        const auto time = fs::last_write_time(src, ec);
        if (!ec && time > newestSource) { newestSource = time; }
    }
    {
        const auto exeTime = fs::last_write_time(GetExecutablePath(), ec);
        if (!ec && exeTime > newestSource) { newestSource = exeTime; }
    }

    fs::create_directories(options.Output, ec);

    // dxc の用意。無くても SM 5.0 のバリアントだけは焼けるので、ここでは落とさない
    DxcRuntime dxcRuntime;
    std::string dxcMessage;
    const bool dxcAvailable = dxcRuntime.Initialize(dxcMessage);
    PrintOut("dxc   : " + (dxcAvailable ? dxcMessage : ("利用できません(" + dxcMessage + ")")) + "\n");
    if (dxcAvailable && !dxcRuntime.HasDxil())
    {
        PrintOut("警告  : dxil.dll が dxcompiler.dll と同じフォルダにありません"
                 "(DXILが未署名になり、開発者モードでない環境でパイプラインステートの作成に失敗します)\n");
    }

    uint32_t variantMask = 1u << static_cast<uint32_t>(ShaderVariant::Dxbc50);
    if (dxcAvailable)
    {
        variantMask |= 1u << static_cast<uint32_t>(ShaderVariant::Dxil65);
        if (dxcRuntime.SupportsShaderModel66())
        {
            variantMask |= 1u << static_cast<uint32_t>(ShaderVariant::Dxil66);
        }
        else
        {
            PrintOut("警告  : dxc が SM 6.6 に対応していないため bindless バリアントを焼きません"
                     "(実行時に bindless とメッシュレット経路が無効になります)\n");
        }
    }
    else
    {
        PrintOut("警告  : SM 5.0 のバリアントだけを焼きます(DX12は SM 5.0 へ縮退し、"
                 "レイトレーシングとメッシュシェーダーが無効になります)\n");
    }

    PrintOut("入力  : " + WideToUtf8(options.Input.wstring()) + " (" + std::to_string(shaderFiles.size()) + " ファイル)\n" +
             "出力  : " + WideToUtf8(options.Output.wstring()) + "\n" +
             "構成  : " + (options.DebugBuild ? "Debug" : "Release") + "\n");

    // 焼き直しが要るファイルだけに絞る
    std::vector<fs::path> targets;
    for (const fs::path& file : shaderFiles)
    {
        const fs::path outPath = options.Output / (file.stem().wstring() + L".kshader");
        if (options.Force)
        {
            targets.push_back(file);
            continue;
        }
        const auto outTime = fs::last_write_time(outPath, ec);
        if (ec || outTime < newestSource)
        {
            targets.push_back(file);
        }
    }

    if (targets.empty())
    {
        PrintOut("すべて最新です(" + std::to_string(shaderFiles.size()) + " ファイル)\n");
        return 0;
    }
    PrintOut("焼き直し: " + std::to_string(targets.size()) + " / " + std::to_string(shaderFiles.size()) + " ファイル\n");

    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    // 【並列数に上限を切る】コンパイラ側にも内部並列があり、掛け算になると
    // かえって遅くなるうえメモリを食う。物理的なコア数を大きく超えても意味がない
    const unsigned jobs = std::min<unsigned>(
        options.Jobs != 0 ? options.Jobs : hardware, std::min<unsigned>(16u, static_cast<unsigned>(targets.size())));

    const auto startTime = std::chrono::steady_clock::now();
    std::vector<FileResult> results(targets.size());
    std::atomic<size_t> nextIndex{ 0 };

    auto worker = [&]() {
        ShaderCompiler compiler;
        std::string compilerMessage;
        const bool compilerReady = dxcAvailable && compiler.Initialize(dxcRuntime, compilerMessage);

        for (;;)
        {
            const size_t index = nextIndex.fetch_add(1);
            if (index >= targets.size()) { break; }

            const fs::path& file = targets[index];
            FileResult& result = results[index];
            result.FileName = WideToUtf8(file.filename().wstring());

            std::string source;
            if (!ReadTextFile(file, source))
            {
                result.Succeeded = false;
                result.Errors.push_back("読み込めませんでした");
                continue;
            }

            // BOMチェック: d3dcompiler は BOM を error X3000 (illegal character) で弾く。
            // リポジトリ側は BOM 無し UTF-8 で固定する規約(dxc 側は Utf8IncludeHandler が吸収する)
            if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
                static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF)
            {
                result.Succeeded = false;
                result.Errors.push_back("UTF-8 BOM が付いています(BOM無しUTF-8で保存してください)");
                continue;
            }

            // 【走査は #include を展開してから】エントリポイントは .hlsl 本体にあるとは限らない
            // (GBuffer.hlsl の VSMain は GBufferCommon.hlsli にある)。展開しないと取りこぼし、
            // エンジンが要求するエントリがパッケージに無い状態で起動して落ちる
            std::string expandError;
            const std::string expanded = ExpandIncludes(file.wstring(), expandError);
            if (!expandError.empty())
            {
                result.Warnings.push_back(expandError);
            }

            const std::vector<ScannedEntry> entries = ScanEntryPoints(expanded);
            if (entries.empty())
            {
                result.Warnings.push_back("エントリポイントが見つかりませんでした(パッケージを作りません)");
                continue;
            }

            // SM 5.0 に無い機能を使うファイルは d3dcompiler の対象外。
            // DX11 はそもそもこれらのシェーダーを生成しない
            static const std::regex sm6OnlyRe(
                R"(RayQuery|TraceRay|RaytracingAccelerationStructure|ResourceDescriptorHeap|KURENAI_BINDLESS)");
            const bool sm6Only = std::regex_search(source, sm6OnlyRe);
            const bool skipDxil65 = ShouldSkipDxil65(result.FileName);

            std::vector<BuiltEntry> built;
            for (const ScannedEntry& entry : entries)
            {
                const bool meshPipeline = entry.Stage == ShaderPackageStage::Amplification ||
                                          entry.Stage == ShaderPackageStage::Mesh;

                // Dxbc50 (d3dcompiler / SM 5.0)
                if (!sm6Only && !meshPipeline)
                {
                    CompileResult compiled =
                        ShaderCompiler::CompileDxbc(file.wstring(), entry.Name, entry.Stage, options.DebugBuild);
                    if (!compiled.Succeeded)
                    {
                        result.Succeeded = false;
                        result.Errors.push_back(entry.Name + " (" + StagePrefix(entry.Stage) + "_5_0): " + compiled.Diagnostics);
                    }
                    else
                    {
                        BuiltEntry b;
                        b.Name = entry.Name;
                        b.Profile = std::string(StagePrefix(entry.Stage)) + "_5_0";
                        b.Stage = entry.Stage;
                        b.Variant = ShaderVariant::Dxbc50;
                        b.Bytecode = std::move(compiled.Bytecode);
                        built.push_back(std::move(b));
                    }
                }

                if (!compilerReady) { continue; }

                // Dxil65 (dxc / SM 6.5 / define無し)。
                // 【増幅/メッシュシェーダーはここには入らない】このエンジンのメッシュシェーダーは
                // ジオメトリをbindlessで引くため、KURENAI_BINDLESS が無いとコンパイルが通らない。
                // エンジン側も DetectMeshShaderSupport が bindless 判定の後に走り、
                // bindless非対応なら SupportsMeshShader() が false になる
                // (check-shaders.ps1 が as/ms を bindless 有りでしか検証しないのと同じ理由)
                if (!skipDxil65 && !meshPipeline)
                {
                    CompileResult compiled =
                        compiler.CompileDxil(file.wstring(), entry.Name, entry.Stage, "6_5", false, options.DebugBuild);
                    if (!compiled.Succeeded)
                    {
                        result.Succeeded = false;
                        result.Errors.push_back(entry.Name + " (" + StagePrefix(entry.Stage) + "_6_5): " + compiled.Diagnostics);
                    }
                    else
                    {
                        BuiltEntry b;
                        b.Name = entry.Name;
                        b.Profile = std::string(StagePrefix(entry.Stage)) + "_6_5";
                        b.Stage = entry.Stage;
                        b.Variant = ShaderVariant::Dxil65;
                        b.Bytecode = std::move(compiled.Bytecode);
                        built.push_back(std::move(b));
                    }
                }

                // Dxil66 (dxc / SM 6.6 / KURENAI_BINDLESS=1)
                if (dxcRuntime.SupportsShaderModel66())
                {
                    CompileResult compiled =
                        compiler.CompileDxil(file.wstring(), entry.Name, entry.Stage, "6_6", true, options.DebugBuild);
                    if (!compiled.Succeeded)
                    {
                        result.Succeeded = false;
                        result.Errors.push_back(
                            entry.Name + " (" + StagePrefix(entry.Stage) + "_6_6 +bindless): " + compiled.Diagnostics);
                    }
                    else
                    {
                        BuiltEntry b;
                        b.Name = entry.Name;
                        b.Profile = std::string(StagePrefix(entry.Stage)) + "_6_6";
                        b.Stage = entry.Stage;
                        b.Variant = ShaderVariant::Dxil66;
                        b.Bytecode = std::move(compiled.Bytecode);
                        built.push_back(std::move(b));
                    }
                }
            }

            if (!result.Succeeded) { continue; }
            if (built.empty())
            {
                result.Warnings.push_back("焼けたバリアントがありませんでした(パッケージを作りません)");
                continue;
            }

            for (const BuiltEntry& b : built) { result.BytecodeSize += b.Bytecode.size(); }
            result.EntryCount = static_cast<int>(built.size());

            const fs::path outPath = options.Output / (file.stem().wstring() + L".kshader");
            std::string writeError;
            if (!WriteShaderPackage(outPath.wstring(), built, variantMask, options.DebugBuild, writeError))
            {
                result.Succeeded = false;
                result.Errors.push_back("書き出しに失敗しました: " + writeError);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(jobs);
    for (unsigned i = 0; i < jobs; ++i) { threads.emplace_back(worker); }
    for (std::thread& t : threads) { t.join(); }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();

    int failedFiles = 0;
    int totalEntries = 0;
    size_t totalBytes = 0;
    for (const FileResult& r : results)
    {
        for (const std::string& w : r.Warnings)
        {
            PrintOut("  [警告] " + r.FileName + ": " + w + "\n");
        }
        if (!r.Succeeded)
        {
            ++failedFiles;
            for (const std::string& e : r.Errors)
            {
                PrintErr("  [失敗] " + r.FileName + ": " + e + "\n");
            }
            continue;
        }
        totalEntries += r.EntryCount;
        totalBytes += r.BytecodeSize;
    }

    PrintOut(
        "完了  : " + std::to_string(targets.size() - failedFiles) + " ファイル / " + std::to_string(totalEntries) +
        " エントリ / " + std::to_string(totalBytes / 1024) + "KB / " + std::to_string(elapsed) + "ms (並列 " +
        std::to_string(jobs) + ")\n");

    if (failedFiles > 0)
    {
        PrintErr("エラー: " + std::to_string(failedFiles) + " ファイルのコンパイルに失敗しました\n");
        return 3;
    }
    return 0;
}
