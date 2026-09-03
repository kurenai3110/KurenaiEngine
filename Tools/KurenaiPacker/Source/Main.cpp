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
// 以降の出力が丸ごと欠落する(パイプ経由で実行するとヘルプメッセージが1行目で切れる、
// といった形で出る)。SetConsoleOutputCP(CP_UTF8)は
// 実コンソールへ出す場合の表示のためだけに設定し、パスやエラーメッセージ(std::wstring)は
// 必ずCore::WideToUtf8で変換してからstd::coutへ渡す

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "Assets/SceneLoader.h"
#include "Core/Logger.h"
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
            "  KurenaiPacker.exe <入力モデル> --inspect [--scale S]             中身を印字するだけ(書き出さない)\n"
            "\n"
            "オプション:\n"
            "  -o, --output <path>   出力先のパス(必須)。モデルモードではこの親ディレクトリが\n"
            "                        .kgeomと.ktexのミラー先ルートになる\n"
            "      --scene <path>    <入力モデル>の代わりに.ksceneを検証・配置するモードにする\n"
            "      --inspect         assimpが読んだ直後のシーン構造(単位系・上方向軸・ノード数・\n"
            "                        バウンズ・マテリアルのテクスチャスロット・埋め込みテクスチャ)を\n"
            "                        印字して終わる。パッケージは書き出さないので-oは不要。\n"
            "                        外部から持ち込んだモデルの--scaleを決めるとき、テクスチャが\n"
            "                        どのスロットへ入ったかを確かめるときに使う\n"
            "      --log-suffix <S>  ログファイル名をKurenaiEngine<S>.logにする。パッカーを同時に\n"
            "                        複数走らせるとき、ログの奪い合いを避けるために使う\n"
            "      --timing          解析と書き出しのフェーズ別内訳を追加で印字する(モデルモードのみ)。\n"
            "                        どこで時間が溶けているかを推測せずに決めるためのもの\n"
            "      --force           既存の.ktexがあっても再圧縮して上書きする(モデルモードのみ)\n"
            "      --jobs <N>        テクスチャ処理のワーカースレッド数(既定: 論理コア数、上限8。モデルモードのみ)\n"
            "      --origin <X,Y,Z>  頂点位置・バウンズからこの座標を引く(--scaleを掛ける前)。\n"
            "                        原点から遠く離れた絶対座標で作られたモデル(地理座標系など)を\n"
            "                        原点付近へ寄せる。複数のモデルを並べる場合は全部に同じ値を\n"
            "                        指定すること(タイルごとに変えると相対位置が壊れる)\n"
            "      --scale <S>       頂点位置・バウンズに乗算する係数(既定1.0、モデルモードのみ)。\n"
            "                        OBJ等ファイル自体に単位情報を持たない形式で、センチメートル単位の\n"
            "                        アセットをメートル単位として読み込みたい場合は0.01を指定する\n"
            "      --bake-occlusion  遮蔽マップ(ベイク済みAO)を生成する(モデルモードのみ)。\n"
            "                        xatlasで重なりの無いライトマップUVを作り、GPUのレイキャストで\n"
            "                        メッシュごとに遮蔽率を焼く。ソースモデルがocclusionTextureを\n"
            "                        持っていても、焼けたメッシュはこちらを優先する\n"
            "      --occlusion-resolution <N>  遮蔽マップの一辺(既定512)\n"
            "      --occlusion-rays <N>        テクセルあたりのレイ本数(既定128)。多いほど滑らかで遅い\n"
            "      --bent-rays <N>             bent normal用のレイ本数(既定256)。ベクトル和は収束が\n"
            "                                  遅いためAO側より多めにとる\n"
            "      --unwrap-split-threshold <N>  UV展開時にメッシュを内部分割する三角形数の閾値\n"
            "                                  (既定50000、0で分割しない)。巨大な単一メッシュの\n"
            "                                  UV展開が極端に遅くなるのを防ぐ\n"
            "      --unwrap-chunk-triangles <N>  分割後の1チャンクあたりの目標三角形数(既定100000)\n"
            "      --translucent <名前>=<V>    指定した名前のマテリアルへ透過率(0〜1)を与える。\n"
            "                                  葉や花弁のように薄いものが、裏から当たった光を透かして\n"
            "                                  表側が明るく見える量。複数指定できる\n"
            "                                  (例: --translucent Blossom=0.55)\n"
            "      --alpha-cutout <名前>=<V>   指定した名前のマテリアルをアルファカットアウトにする。\n"
            "                                  BaseColorのアルファがV未満の画素を捨てる(0〜1)。\n"
            "                                  FBX/OBJにはglTFのalphaModeに相当する情報が無いため、\n"
            "                                  葉や草のように「アルファで抜く」前提のマテリアルは\n"
            "                                  これを指定しないと不透明な板として描かれる。複数指定できる\n"
            "      --specular-as-orm           aiTextureType_SPECULARのテクスチャをmetallicRoughnessと\n"
            "                                  遮蔽マップとして読む。SpecularColorスロットへ\n"
            "                                  ORM(R=遮蔽/G=ラフネス/B=メタリック)を格納する規約の\n"
            "                                  FBX向け。SpecularColorが本来の鏡面反射色である\n"
            "                                  アセットに指定すると全面が金属になるので注意する\n"
            "      --metallic <V>              全マテリアルのメタリック値を上書きする(0〜1)\n"
            "      --roughness <V>             全マテリアルのラフネス値を上書きする(0〜1)\n"
            "      --emissive <名前>=<R,G,B>   指定した名前のマテリアルへ自発光の係数を与える。\n"
            "                                  Keを持たないアセットは照明器具のジオメトリが\n"
            "                                  あってもEmissiveFactorが0のままで、自発光\n"
            "                                  テクスチャを持つマテリアルすら光らない。\n"
            "                                  自発光は露出を通らないため1を超える値を取る\n"
            "                                  (例: --emissive Bulb=156,156,156)\n"
            "      --base-color <R,G,B>        全マテリアルのベースカラー係数を上書きする(各0〜1)\n"
            "                                  生のOBJ等、PBR係数を表現できない形式へ検証用の\n"
            "                                  マテリアルを与えるためのもの\n"
            "      --no-meshlets       メッシュレット(メッシュシェーダー用の分割情報)を生成しない。\n"
"                                  併せて頂点キャッシュ最適化とインデックスの並べ替えも\n"
"                                  行わないため、頂点/インデックスは入力の並びのまま出る。\n"
"                                  見た目の異常がメッシュレット化由来かの切り分けに使う\n"
"      --meshlet-lods <N>  メッシュレットの離散LODを何段まで作るか(既定4、上限4)。\n"
"                                  1なら原寸のみ。0は--no-meshletsと同じ。\n"
"                                  段ごとに三角形を半分にし、それ以上潰せなくなったら打ち切る\n"
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
        std::optional<std::array<float, 3>> OriginOffset;
        bool ShowHelp = false;
        bool SceneMode = false;
        bool Inspect = false;
        bool Timing = false;
        std::wstring LogSuffix;
        bool BakeOcclusion = false;
        bool EnableMeshlets = true;
        unsigned int MeshletLODCount = 4;
        unsigned int OcclusionResolution = 512;
        unsigned int OcclusionRays = 128;
        unsigned int BentNormalRays = 256;
        // 既定値はOcclusionBakeOptionsと合わせること
        unsigned int UnwrapSplitThreshold = 50000;
        unsigned int UnwrapChunkTriangles = 100000;
        KurenaiPacker::MaterialOverride MaterialOverride;
    };

    // [0,1]のスカラーをパースする。失敗時はfalseを返す
    bool ParseUnitScalar(const std::wstring& option, const std::wstring& value, std::optional<float>& out)
    {
        try
        {
            const float parsed = std::stof(value);
            if (parsed < 0.0f || parsed > 1.0f)
            {
                PrintError(WideToUtf8(option) + " は0〜1の範囲で指定してください: " + WideToUtf8(value));
                return false;
            }
            out = parsed;
            return true;
        }
        catch (const std::exception&)
        {
            PrintError(WideToUtf8(option) + " の値が不正です: " + WideToUtf8(value));
            return false;
        }
    }

    // "R,G,B" 形式をパースする。失敗時はfalseを返す。
    // 上限はオプションによって違う(ベースカラーは0〜1だが、自発光は露出を通らないぶん
    // 1を大きく超える値を取る。ModelSource.h の MaterialOverride::Emissive を参照)
    bool ParseRgbTriplet(
        const std::wstring& option, const std::wstring& value, float maxComponent,
        std::array<float, 3>& out)
    {
        size_t start = 0;
        for (int i = 0; i < 3; ++i)
        {
            const size_t comma = value.find(L',', start);
            if ((i < 2 && comma == std::wstring::npos) || (i == 2 && comma != std::wstring::npos))
            {
                PrintError(WideToUtf8(option) + " は R,G,B の3要素で指定してください: " + WideToUtf8(value));
                return false;
            }
            try
            {
                out[i] = std::stof(value.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
            }
            catch (const std::exception&)
            {
                PrintError(WideToUtf8(option) + " の値が不正です: " + WideToUtf8(value));
                return false;
            }
            if (out[i] < 0.0f || out[i] > maxComponent)
            {
                PrintError(
                    WideToUtf8(option) + " の各成分は0〜" + std::to_string(static_cast<int>(maxComponent))
                    + " の範囲で指定してください: " + WideToUtf8(value));
                return false;
            }
            start = comma + 1;
        }
        return true;
    }

    bool ParseBaseColor(const std::wstring& value, std::optional<std::array<float, 3>>& out)
    {
        std::array<float, 3> color{};
        if (!ParseRgbTriplet(L"--base-color", value, 1.0f, color))
        {
            return false;
        }
        out = color;
        return true;
    }

    // --jobs/--occlusion-*/--bent-rays/--unwrap-* 共通の符号なし整数パース。失敗時はfalseを返す。
    // allowZero: 0を「機能を無効にする」意味で受け付けるオプション用
    // (--bent-raysは0で「bent normalを焼かない」、--unwrap-split-thresholdは0で「分割しない」。
    //  他は0だと解像度0・レイ0本になり無意味なので拒否する)
    bool ParseUnsigned(const std::wstring& option, const std::wstring& value, unsigned int& out, bool allowZero = false)
    {
        try
        {
            const unsigned long parsed = std::stoul(value);
            if (parsed == 0 && !allowZero)
            {
                PrintError(WideToUtf8(option) + " には1以上の値を指定してください");
                return false;
            }
            out = static_cast<unsigned int>(parsed);
            return true;
        }
        catch (const std::exception&)
        {
            PrintError(WideToUtf8(option) + " の値が不正です: " + WideToUtf8(value));
            return false;
        }
    }

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
            else if (arg == L"--inspect")
            {
                args.Inspect = true;
            }
            else if (arg == L"--timing")
            {
                args.Timing = true;
            }
            else if (arg == L"--log-suffix")
            {
                if (i + 1 >= argc)
                {
                    PrintError("--log-suffix には値が必要です");
                    return std::nullopt;
                }
                args.LogSuffix = argv[++i];
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
            else if (arg == L"--no-meshlets")
            {
                args.EnableMeshlets = false;
            }
            else if (arg == L"--meshlet-lods")
            {
                if (i + 1 >= argc)
                {
                    PrintError(WideToUtf8(arg) + " には値が必要です");
                    return std::nullopt;
                }
                // 0(段を作らない=メッシュレット自体を作らない)を許す
                if (!ParseUnsigned(arg, argv[++i], args.MeshletLODCount, true))
                {
                    return std::nullopt;
                }
                if (args.MeshletLODCount > Kurenai::Assets::kMaxMeshletLODCount)
                {
                    PrintError("--meshlet-lods の上限は "
                        + std::to_string(Kurenai::Assets::kMaxMeshletLODCount)
                        + " です(MeshEntryが段ごとの範囲を固定長で持つため)");
                    return std::nullopt;
                }
            }
            else if (arg == L"--bake-occlusion")
            {
                args.BakeOcclusion = true;
            }
            else if (arg == L"--occlusion-resolution" || arg == L"--occlusion-rays" || arg == L"--bent-rays")
            {
                if (i + 1 >= argc)
                {
                    PrintError(WideToUtf8(arg) + " には値が必要です");
                    return std::nullopt;
                }
                unsigned int* target = &args.OcclusionRays;
                if (arg == L"--occlusion-resolution") { target = &args.OcclusionResolution; }
                else if (arg == L"--bent-rays")       { target = &args.BentNormalRays; }
                // --bent-raysだけは0(bent normalを焼かない)を許す
                if (!ParseUnsigned(arg, argv[++i], *target, arg == L"--bent-rays"))
                {
                    return std::nullopt;
                }
            }
            else if (arg == L"--unwrap-split-threshold" || arg == L"--unwrap-chunk-triangles")
            {
                if (i + 1 >= argc)
                {
                    PrintError(WideToUtf8(arg) + " には値が必要です");
                    return std::nullopt;
                }
                const bool isThreshold = (arg == L"--unwrap-split-threshold");
                unsigned int& target = isThreshold ? args.UnwrapSplitThreshold : args.UnwrapChunkTriangles;
                // 閾値だけは0(=分割しない)を許可する。チャンクの目標三角形数に0は意味が無い
                if (!ParseUnsigned(arg, argv[++i], target, isThreshold))
                {
                    return std::nullopt;
                }
            }
            else if (arg == L"--translucent")
            {
                if (i + 1 >= argc)
                {
                    PrintError("--translucent には <マテリアル名>=<値> が必要です");
                    return std::nullopt;
                }
                const std::wstring token = argv[++i];
                const size_t separator = token.rfind(L'=');
                if (separator == std::wstring::npos || separator == 0 || separator + 1 >= token.size())
                {
                    PrintError("--translucent の書式が不正です(<マテリアル名>=<値>): " + WideToUtf8(token));
                    return std::nullopt;
                }
                std::optional<float> value;
                if (!ParseUnitScalar(arg, token.substr(separator + 1), value))
                {
                    return std::nullopt;
                }
                args.MaterialOverride.Translucency[WideToUtf8(token.substr(0, separator))] = value.value_or(0.0f);
            }
            else if (arg == L"--alpha-cutout")
            {
                if (i + 1 >= argc)
                {
                    PrintError("--alpha-cutout には <マテリアル名>=<値> が必要です");
                    return std::nullopt;
                }
                const std::wstring token = argv[++i];
                const size_t separator = token.rfind(L'=');
                if (separator == std::wstring::npos || separator == 0 || separator + 1 >= token.size())
                {
                    PrintError("--alpha-cutout の書式が不正です(<マテリアル名>=<値>): " + WideToUtf8(token));
                    return std::nullopt;
                }
                std::optional<float> value;
                if (!ParseUnitScalar(arg, token.substr(separator + 1), value))
                {
                    return std::nullopt;
                }
                args.MaterialOverride.AlphaCutoff[WideToUtf8(token.substr(0, separator))] = value.value_or(0.5f);
            }
            else if (arg == L"--emissive")
            {
                if (i + 1 >= argc)
                {
                    PrintError("--emissive には <マテリアル名>=<R,G,B> が必要です");
                    return std::nullopt;
                }
                const std::wstring token = argv[++i];
                const size_t separator = token.rfind(L'=');
                if (separator == std::wstring::npos || separator == 0 || separator + 1 >= token.size())
                {
                    PrintError("--emissive の書式が不正です(<マテリアル名>=<R,G,B>): " + WideToUtf8(token));
                    return std::nullopt;
                }
                std::array<float, 3> color{};
                // 上限は「露出済みの輝度」として現実的に取りうる範囲。裸電球で数百になる
                if (!ParseRgbTriplet(L"--emissive", token.substr(separator + 1), 10000.0f, color))
                {
                    return std::nullopt;
                }
                args.MaterialOverride.Emissive[WideToUtf8(token.substr(0, separator))] = color;
            }
            else if (arg == L"--specular-as-orm")
            {
                args.MaterialOverride.SpecularAsOrm = true;
            }
            else if (arg == L"--metallic" || arg == L"--roughness")
            {
                if (i + 1 >= argc)
                {
                    PrintError(WideToUtf8(arg) + " には値が必要です");
                    return std::nullopt;
                }
                std::optional<float>& target = (arg == L"--metallic")
                    ? args.MaterialOverride.MetallicFactor
                    : args.MaterialOverride.RoughnessFactor;
                if (!ParseUnitScalar(arg, argv[++i], target))
                {
                    return std::nullopt;
                }
            }
            else if (arg == L"--base-color")
            {
                if (i + 1 >= argc)
                {
                    PrintError("--base-color には値が必要です");
                    return std::nullopt;
                }
                if (!ParseBaseColor(argv[++i], args.MaterialOverride.BaseColor))
                {
                    return std::nullopt;
                }
            }
            else if (arg == L"--origin")
            {
                if (i + 1 >= argc)
                {
                    PrintError("--origin には X,Y,Z が必要です");
                    return std::nullopt;
                }
                // --base-colorと同じ「3要素をカンマ区切り」の書式だが、あちらと違って
                // 範囲の制約は無い(座標なので負値も巨大な値も正当)
                const std::wstring token = argv[++i];
                std::array<float, 3> origin{};
                size_t start = 0;
                bool ok = true;
                for (int axis = 0; axis < 3 && ok; ++axis)
                {
                    const size_t comma = token.find(L',', start);
                    if ((axis < 2 && comma == std::wstring::npos) || (axis == 2 && comma != std::wstring::npos))
                    {
                        ok = false;
                        break;
                    }
                    try
                    {
                        origin[axis] = std::stof(token.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
                    }
                    catch (const std::exception&)
                    {
                        ok = false;
                        break;
                    }
                    start = comma + 1;
                }
                if (!ok)
                {
                    PrintError("--origin は X,Y,Z の3要素で指定してください: " + WideToUtf8(token));
                    return std::nullopt;
                }
                args.OriginOffset = origin;
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

    // 秒をミリ秒の整数文字列にする(FormatMsと同じ見え方に揃えるため)
    std::string FormatSeconds(double seconds)
    {
        return std::to_string(static_cast<long long>(seconds * 1000.0));
    }

    // 比率を固定小数2桁で出す。sprintf_sの書式文字列へ日本語を入れるとC4819が出るため、
    // 書式はASCIIに限り日本語は連結側へ置く(OcclusionBaker.cppと同じ作法)
    std::string Format2(double value)
    {
        char buffer[64];
        sprintf_s(buffer, "%.2f", value);
        return buffer;
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

    // coutを1挿入ごとにフラッシュする。
    //
    // これを入れないと、cout(ブロックバッファ)とcerr(unitbuf)を同じ出力先へ束ねたとき
    // ―― つまり "KurenaiPacker.exe ... > log 2>&1" という最も自然な取り方をしたとき ――
    // 双方が独立したバッファで同じファイルへ書き込み、出力が途中で失われる
    // (出力が長いモデルでは、UTF-8シーケンスの途中から後半が丸ごと欠落する)。
    // 消えるのは後半なので、遮蔽ベイクの検証結果とWarn()がまとめて落ちる ―― 最悪の失われ方をする。
    // CLIツールなので1行ごとのフラッシュによる速度低下は問題にならない
    std::cout << std::unitbuf;

    const std::optional<CommandLineArgs> parsedArgs = ParseArgs(argc, argv);
    if (!parsedArgs)
    {
        PrintUsage();
        return 1;
    }
    const CommandLineArgs& args = *parsedArgs;

    // 【最初のログ出力より前に呼ぶ】Core::Loggerはexeの隣のKurenaiEngine<接尾辞>.logを
    // truncで開くため、パッカーを同時に複数走らせると同じファイルを奪い合い、
    // 警告(BC7圧縮の失敗など。これらはstderrへは出ずログにしか残らない)が混ざるか消える。
    // 並列に回す側が呼び出しごとに違う接尾辞を渡せるようにする
    if (!args.LogSuffix.empty())
    {
        Kurenai::Core::Logger::SetFileSuffix(WideToUtf8(args.LogSuffix));
    }

    if (args.ShowHelp)
    {
        PrintUsage();
        return 0;
    }

    // --inspectは読んで印字するだけで何も書き出さないため-oを要求しない。
    // それ以外のモードでは従来どおり入力と出力の両方が要る
    if (args.InputPath.empty() || (args.OutputPath.empty() && !args.Inspect))
    {
        PrintError(args.SceneMode
            ? "--scene と -o/--output の両方の指定が必要です"
            : (args.Inspect
                ? "--inspect には入力モデルの指定が必要です"
                : "入力モデルと -o/--output の両方の指定が必要です"));
        PrintUsage();
        return 1;
    }

    if (args.SceneMode && args.Inspect)
    {
        PrintError("--scene と --inspect は同時に指定できません");
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

    // --inspect: assimpが読んだ構造を印字して終わる。以降のベイク・書き出しには進まない
    if (args.Inspect)
    {
        try
        {
            KurenaiPacker::InspectModel(inputAbsolute.wstring(), args.Scale);
        }
        catch (const std::exception& e)
        {
            PrintError("入力モデルの解析に失敗しました: " + std::string(e.what()));
            return 2;
        }
        return 0;
    }

    const auto startTime = std::chrono::steady_clock::now();

    KurenaiPacker::SourceModel sourceModel;
    KurenaiPacker::ParseTimings parseTimings;
    try
    {
        sourceModel = KurenaiPacker::LoadSourceModel(
            inputAbsolute.wstring(), args.Scale, args.MaterialOverride, args.OriginOffset,
            args.Timing ? &parseTimings : nullptr);
    }
    catch (const std::exception& e)
    {
        PrintError("入力モデルの解析に失敗しました: " + std::string(e.what()));
        return 2;
    }

    const auto parseTime = std::chrono::steady_clock::now();

    // 遮蔽マップのベイク。sourceModelのジオメトリをライトマップUV付きへ置き換えるため、
    // 必ずパッケージ書き出しより前に行う
    KurenaiPacker::OcclusionBakeResult bakeResult;
    if (args.BakeOcclusion)
    {
        KurenaiPacker::OcclusionBakeOptions bakeOptions;
        bakeOptions.Resolution = args.OcclusionResolution;
        bakeOptions.RayCount = args.OcclusionRays;
        bakeOptions.BentNormalRayCount = args.BentNormalRays;
        bakeOptions.UnwrapSplitThreshold = args.UnwrapSplitThreshold;
        bakeOptions.UnwrapChunkTriangles = args.UnwrapChunkTriangles;
        try
        {
            bakeResult = KurenaiPacker::BakeOcclusion(sourceModel, bakeOptions);
        }
        catch (const std::exception& e)
        {
            PrintError("遮蔽マップのベイクに失敗しました: " + std::string(e.what()));
            return 2;
        }
    }
    const auto bakeTime = std::chrono::steady_clock::now();

    KurenaiPacker::PackOptions options;
    options.Force = args.Force;
    options.JobCount = args.JobCount;
    options.BakedOcclusion = args.BakeOcclusion ? &bakeResult : nullptr;
    options.EnableMeshlets = args.EnableMeshlets;
    options.MeshletLODCount = args.MeshletLODCount;
    if (sourceModel.EmbeddedTextures)
    {
        options.EmbeddedTextureDirectory = sourceModel.EmbeddedTextures->Directory();
    }

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

    if (args.OriginOffset)
    {
        // .ksceneの先頭コメントへ記録できるよう、引いた値をそのまま出す
        std::cout
            << "[KurenaiPacker] --origin により ("
            << (*args.OriginOffset)[0] << ", " << (*args.OriginOffset)[1] << ", " << (*args.OriginOffset)[2]
            << ") を減算しました\n";
    }

    std::cout
        << "[KurenaiPacker] パック完了: " << WideToUtf8(args.OutputPath) << "\n"
        << "  メッシュ数: " << result.MeshCount
        << " / 頂点数: " << result.VertexCount
        << " / インデックス数: " << result.IndexCount << "\n"
        << "  テクスチャ要求: " << result.TextureRequested
        << " (新規生成 " << result.TextureGenerated
        << " / 既存スキップ " << result.TextureSkippedExisting
        << " / 失敗(フォールバック) " << result.TextureFailed << ")\n";

    if (sourceModel.EmbeddedTextures && sourceModel.EmbeddedTextures->ExtractedCount() > 0)
    {
        // 埋め込みテクスチャは「取り出せた枚数」と「テクスチャ要求の数」の両方を見ないと
        // 落ちているものに気づけない(取り出しに失敗したスロットは-1へフォールバックし、
        // 要求そのものが立たないため)
        std::cout << "  埋め込みテクスチャ: " << sourceModel.EmbeddedTextures->ExtractedCount()
            << "枚を一時ファイルへ取り出しました\n";
    }

    if (args.EnableMeshlets && args.MeshletLODCount > 0)
    {
        std::cout << "  メッシュレット: " << result.MeshletCount << " (LOD0 " << result.MeshletLOD0Count << ")";
        if (result.MeshletLOD0Count > 0)
        {
            // 1メッシュレットあたりの平均三角形数。上限(kMeshletMaxTriangles)に近いほど
            // 分割が詰まっており、極端に少ない場合はモデルの三角形が散らばっている。
            // LOD0だけで割る(簡略化した段は三角形が減っているので混ぜると意味が薄れる)
            std::cout << " (LOD0の1つあたり平均 "
                      << (result.IndexCount / 3 + result.MeshletLOD0Count / 2) / result.MeshletLOD0Count << "三角形)";
        }
        std::cout << "\n";

        if (args.MeshletLODCount > 1 && result.MeshletTrianglesByLOD[0] > 0)
        {
            // 【段ごとに出す】段が進んでも三角形が減っていなければ、簡略化が効いていない。
            // 総数だけを見ていると「段は作れた」で通ってしまう
            std::cout << "  メッシュレットLOD:";
            for (unsigned int lod = 0; lod < Kurenai::Assets::kMaxMeshletLODCount; ++lod)
            {
                if (lod > 0 && result.MeshletTrianglesByLOD[lod] == 0)
                {
                    break;
                }
                std::cout << " [" << lod << "] " << result.MeshletTrianglesByLOD[lod] << "三角形";
            }
            std::cout << "\n";
        }
    }

    if (args.BakeOcclusion)
    {
        std::cout
            << "  遮蔽マップ: ベイク " << bakeResult.BakedMeshCount
            << " / スキップ " << bakeResult.SkippedMeshCount
            << " / 書き出し " << result.OcclusionBaked
            << " (解像度 " << args.OcclusionResolution
            << " / レイ " << args.OcclusionRays << "本)\n"
            << "  bent normal: 書き出し " << result.BentNormalBaked
            << " (レイ " << args.BentNormalRays << "本)\n";
    }

    std::cout
        << "  所要時間: 解析 " << FormatMs(startTime, parseTime) << "ms";
    if (args.BakeOcclusion)
    {
        std::cout << " / 遮蔽ベイク " << FormatMs(parseTime, bakeTime) << "ms";
    }
    std::cout
        << " / 書き出し " << FormatMs(bakeTime, endTime) << "ms"
        << " / 合計 " << FormatMs(startTime, endTime) << "ms\n";

    if (args.Timing)
    {
        // 【0の項目は出さない】テクスチャ0枚のPLATEAU LOD1タイルのように、ほとんどの項目が
        // 0になる入力がある。全部並べると671タイルぶんのログが読めなくなる
        const auto emit = [](const char* label, double seconds)
        {
            if (seconds < 0.0005) { return; }
            std::cout << " / " << label << " " << FormatSeconds(seconds) << "ms";
        };

        std::cout << "  解析の内訳:";
        emit("assimp読み込み", parseTimings.ReadSeconds);
        emit("ノード収集", parseTimings.CollectSeconds);
        emit("接線蓄積", parseTimings.TangentSeconds);
        emit("頂点ループ", parseTimings.VertexSeconds);
        emit("結合", parseTimings.MergeSeconds);
        emit("マテリアル", parseTimings.MaterialSeconds);
        std::cout << "\n";

        const KurenaiPacker::WriteTimings& wt = result.Timings;
        std::cout << "  書き出しの内訳:";
        emit("収集", wt.CollectSeconds);
        emit("スキップ判定", wt.SkipCheckSeconds);
        emit("テクスチャ", wt.TextureSeconds);
        emit("エントリ確定", wt.EntrySeconds);
        emit("遮蔽マップ", wt.OcclusionSeconds);
        emit("bentNormal", wt.BentNormalSeconds);
        emit("メッシュレット構築", wt.MeshletSeconds);
        emit("連結", wt.AppendSeconds);
        emit(".kgeom書き込み", wt.GeometryWriteSeconds);
        emit(".kmodel書き込み", wt.ModelWriteSeconds);
        std::cout << "\n";

        if (wt.WorkerCount > 0)
        {
            // 【和は実時間を超えうる】全ワーカーの累計なので上限は実時間×ワーカー数。
            // 実効並列度がワーカー数に近ければ全員が働いており、1に近ければ1本を残して
            // 全員がBC7のミューテックスで待っている。ここがスレッドを増やす価値を直接決める
            const double workerSum = wt.WorkerLoadSeconds + wt.WorkerDdsSeconds + wt.WorkerWriteSeconds;
            const double effective = wt.TextureSeconds > 0.0 ? workerSum / wt.TextureSeconds : 0.0;
            std::cout
                << "  テクスチャ内訳(全ワーカーの累計): 読み込み+ミップ+BC7 " << FormatSeconds(wt.WorkerLoadSeconds) << "ms"
                << " / DDS化 " << FormatSeconds(wt.WorkerDdsSeconds) << "ms"
                << " / 書き込み " << FormatSeconds(wt.WorkerWriteSeconds) << "ms\n"
                << "    ワーカー " << wt.WorkerCount << "本 / フェーズ実時間 " << FormatSeconds(wt.TextureSeconds) << "ms"
                << " / 実効並列度 " << Format2(effective) << "\n";

            // 【ここが本丸】BC7待ちとBC7圧縮の比が「ワーカーを増やして意味があるか」を決める。
            // 待ちが支配的なら本数を増やしても待ち行列が伸びるだけで、直列点そのものを
            // 見直すか、プロセスを分けてデバイスを分けるしかない
            std::cout
                << "    LoadFromFileの内訳: デコード " << FormatSeconds(wt.TexDecodeSeconds) << "ms"
                << " / ミップ " << FormatSeconds(wt.TexMipSeconds) << "ms"
                << " / BC7待ち " << FormatSeconds(wt.TexBC7WaitSeconds) << "ms"
                << " / BC7圧縮 " << FormatSeconds(wt.TexBC7CompressSeconds) << "ms"
                << " / デバイス生成 " << FormatSeconds(wt.TexDeviceCreateSeconds) << "ms\n";
        }

        // 【プロセスCPU÷実時間を必ず出す】これがワーカー数を超えていたら、内側のライブラリが
        // 既に自前で並列化しているという意味で、外側にプールを足してはいけない
        // (DirectXTexのOpenMPと外側8ワーカーが掛かって224スレッドになり、機械が固まった前例がある)
        const double cpuSeconds = KurenaiPacker::GetProcessCpuSeconds();
        const double wallSeconds = std::chrono::duration<double>(endTime - startTime).count();
        std::cout
            << "  プロセス全体: CPU " << FormatSeconds(cpuSeconds) << "ms"
            << " (" << Format2(wallSeconds > 0.0 ? cpuSeconds / wallSeconds : 0.0) << "コア相当)"
            << " / ピークWS " << KurenaiPacker::GetPeakWorkingSetMB() << "MB\n";
    }

    return 0;
}
