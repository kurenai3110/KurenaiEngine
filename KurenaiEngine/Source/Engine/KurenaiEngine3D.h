#pragma once

#include <Windows.h>

#include <map>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <string>
#include <thread>
#include <vector>

#include "Diagnostics/CullStatsReadback.h"
#include "GI/DDGIGrid.h"
#include "Settings/EngineSettings.h"
#include "DroneShow.h"
#include "EngineDefaults.h"
#include "KurenaiEngineBase.h"
#include "KurenaiTypes.h"

#include "Assets/MeshLightScene.h"
#include "Assets/RaytracingScene.h"
#include "Assets/Scene.h"
#include "Assets/TextureStreaming.h"
#include "Core/Camera.h"
#include "Core/CPUProfiler.h"
#include "Diagnostics/RenderCapabilities.h"
#include "Diagnostics/RenderStats.h"
#include "Diagnostics/ScheduledRecreation.h"
#include "Rendering/DroneShowResources.h"
#include "Rendering/GIResources.h"
#include "Rendering/IBLResources.h"
#include "Rendering/SkyResources.h"
#include "Rendering/SceneGPUResources.h"
#include "Rendering/RenderTargets.h"
#include "Rendering/ShadowConstants.h"
#include "Rendering/MeshletLODFrameConstants.h"
#include "Rendering/GeometryDrawTypes.h"
#include "Rendering/CubeFaceMath.h"
#include "Passes/DDGIConstants.h"
#include "Passes/EnvironmentConstants.h"
#include "Passes/GeometryConstants.h"
#include "Passes/MegaLightsConstants.h"
#include "Passes/ReflectionProbeConstants.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Core
{
    // IssueTextureDumps / RegisterPasses / WritePassManifestIfDue の引数にだけ使う。
    // RenderGraph.hはRHIのヘッダ群を芋づるで引き込むため、このヘッダをインクルードする側
    // (UIパネル・Sample3D)へ広げないよう前方宣言で済ませる
    class RenderGraph;
}

namespace Kurenai::UI
{
    class UIManager;
}

namespace Kurenai::Passes
{
    // Render()から切り出したパス群(段階6)。実体はPasses/*.hにあり、
    // そちらはこのヘッダをインクルードするため、ここでは前方宣言で止める
    class DDGIPasses;
    class EnvironmentPasses;
    class GeometryPasses;
    class LightingPasses;
    class MegaLightsPasses;
    class PostProcessPasses;
    class ReflectionPasses;
    class ReflectionProbePasses;
    class ShadowPasses;
    class PresentPass;
}

namespace Kurenai::ShaderInterop
{
    // AdvanceFramePrevViewState()の引数にだけ使う。実体はShaderInterop/FrameConstants.hにあり、
    // このヘッダを公開APIから重くしないため前方宣言で止める
    struct FrameConstants;
}

namespace Kurenai::Rendering
{
    // Render()から切り出した関数(ResolveFrameCullStats / RegisterPasses)の引数にだけ使う。
    // 実体はRendering/RenderBlackboard.h・RenderFrameContext.hにあり、
    // そちらはRHIのヘッダ群を引き込むため前方宣言で止める
    struct RenderBlackboard;
    struct RenderFrameContext;
}

namespace Kurenai::Passes
{
    // BuildFrameContext の出力引数にだけ使う。実体は Passes/LightingConstants.h
    struct LightingConstants;
}

namespace Kurenai
{
    // BuildFrameContext の引数にだけ使う。どちらも参照で受けるだけなので、
    // 公開ヘッダの重さを増やさないよう前方宣言で止める
    // (実体は Rendering/GPULight.h と Rendering/SunLighting.h)
    struct GPULight;
    struct SunLighting;
}

namespace Kurenai
{
    // インスタンシングで1体ぶんの変換を渡すレコード。
    // Shaders/3D/ObjectConstants.hlsli の struct ModelInstanceRecord と
    // **バイト単位で一致させること**(144バイト。ずれると全インスタンスが見当違いの場所へ飛ぶ)
    struct alignas(16) GPUModelInstance
    {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4X4 NormalMatrix;
        float TangentSignFlip;
        float Padding[3];
    };
    static_assert(sizeof(GPUModelInstance) == 144, "GPUModelInstanceはHLSL側と同じ144バイトであること");

    // 3Dサンプルプログラム向けの公開API。Deferred Shading(G-Buffer)によるPBRレンダリング、
    // シャドウマッピング、SSAO/SSIL(間接光)、SSR(反射)、ImGuiによる各種設定パネル、
    // 複数シーンの切り替えまでを内包した完結型のレンダラー。
    // 構築してRun()を呼ぶだけでウィンドウが開き、終了するまでブロックする
    class KURENAI_3D_API KurenaiEngine3D : public KurenaiEngineBase
    {
    public:
        // renderWidth/renderHeight: G-Buffer以降の内部解像度(ウィンドウサイズとは独立。
        //   実行時に「システム」パネルからも変更できる)。
        // initialSceneIndex: 起動時に読み込むシーンの番号(Assets/Scenes/*.ksceneをファイル名の
        //   昇順に並べたときの位置)。範囲外なら0にフォールバックする。
        //   グラフィックスAPIを切り替える際、呼び出し側が同じシーンで作り直すために使う
        explicit KurenaiEngine3D(
            GraphicsAPI api = GraphicsAPI::DX11, uint32_t renderWidth = Defaults::RenderWidth,
            uint32_t renderHeight = Defaults::RenderHeight, size_t initialSceneIndex = 0);
        ~KurenaiEngine3D();

        void Run();

        // --- グラフィックスAPIの実行時切り替え ---
        // Run()の中でUI(「システム」パネル)からRequestGraphicsAPIChange()が呼ばれると、
        // Run()はウィンドウを閉じずにループを抜けて戻る。呼び出し側は下の2つを見て、
        // このオブジェクトを破棄してから新しいAPIで作り直すこと(Samples/Sample3D/Source/Main.cpp)。
        //
        // デバイスだけを差し替えるのではなくオブジェクトごと作り直すのは、破棄の順序
        // (派生クラスの全リソース → スワップチェーン → デバイス → ウィンドウ)を
        // C++のメンバ破棄順にそのまま任せられるため。手書きの解放関数にすると、
        // メンバを追加したときに解放漏れが静かに発生する
        bool HasPendingGraphicsAPIChange() const;
        GraphicsAPI GetPendingGraphicsAPI() const;
        // 作り直しへ引き継ぐ状態。上記以外の設定(AO・シャドウ・IBL・ポストプロセス等)は
        // 新しいインスタンスで既定値に戻る
        uint32_t GetRenderWidth() const { return m_RenderWidth; }
        uint32_t GetRenderHeight() const { return m_RenderHeight; }
        size_t GetCurrentSceneIndex() const { return m_CurrentSceneIndex; }

        // デバッグ表示を番号で選ぶ(番号の並びはUIの「デバッグ表示」コンボと同じ)。
        //
        // 【何のためにあるのか】アトラスやバッファの生値を確かめる検証を、GUIのクリック操作
        // なしで起動オプションから行えるようにするため。DDGIのイラディアンスアトラスが
        // 一様な白の環境で基準値と一致するか、といった検証は「見て判断する」ものではなく
        // 画素値を測るものなので、毎回コンボを人手で操作する形にすると再現性が落ちる。
        //
        // 範囲外の番号は無視してログを残す(呼び出し側で範囲を知らなくてよいようにする)
        void SetDebugViewIndex(int index);

        // DDGIのレイの取得をラスタライズへ強制する(既定はDXRが使えるならDXR)。
        //
        // 【何のためにあるのか】ラスタ経路とレイトレース経路のA/B比較を、GUIのコンボを
        // 人手で操作せずに同じ起動手順で行えるようにするため。シーンを切り替えると
        // 露出(EV100)が引き継がれてしまうので、比較は必ず起動直後から同じ手順で行う
        void ForceDDGIRayModeRaster();

        // プローブ分類のしきい値を上書きする(0以下なら分類そのものを無効にする)。
        // しきい値の効き方をA/Bで測るための起動オプション用。根拠はm_Settings.DDGI.BackfaceThresholdを参照
        void SetDDGIBackfaceThreshold(float threshold);

        // DDGIのクリップマップLODの段数と追従の有無を、読み込んだ`.kscene`の指定より優先して上書きする。
        // 段数を振って効果を測るための起動オプション用。アトラスを確保し直すので、
        // **フレームの記録が始まる前(Run()より前)にだけ呼ぶこと**
        void OverrideDDGILOD(uint32_t lodCount, bool followCamera);

        // MegaLightsの手法と、1灯あたりに撃つ影レイの本数を起動時に上書きする。
        // mode は MegaLightsMode の値(0=なし, 1=参照実装)。
        // mode / shadowRayCount / sampleCount のいずれも、負の値を渡すとその項目は既定のままにする
        // (一部だけの指定ができるようにするため)。sampleCount は確率的サンプリングが
        // 1ピクセルあたりに候補プールから引く数(RISのM)。
        //
        // 【何のためにあるのか】SetDebugViewIndex / ForceDDGIRayModeRaster と同じ理由。
        // MegaLightsの検証は「見て判断する」ものではなく画素値を測るもので、
        // 影レイ0本(恒等テスト)と従来のライトループの一致を数値で確かめる、といった比較を
        // 毎回コンボの人手操作でやると再現性が落ちる。**シーンを切り替えると露出(EV100)が
        // 引き継がれるため、A/Bは必ず起動直後から同じ手順で行うこと。**
        //
        // 範囲外の値は無視してログを残す(呼び出し側が範囲を知らなくてよいようにする)。
        // レイトレーシング非対応の環境では手法を変えてもパスが走らない(ShouldRunMegaLights)
        void OverrideMegaLights(int mode, int shadowRayCount, int sampleCount);

        // エミッシブ光源(自発光メッシュを光源として扱う)の切り替え。
        //
        // 自発光面はG-Bufferへ書いて加算されるだけで周囲を照らしていない。有効にすると、
        // 読み込み時に自発光メッシュから起こした光源のかたまりをGPULight(LightType 3)として
        // 従来のライトループにもMegaLightsにも流す。**有効にすると絵が明るくなる。**
        //
        // enabled は 0=無効 / 正=有効 / **負なら既定のまま**(OverrideMegaLightsと同じ約束)。
        // しきい値だけを差し替えたいときに、状態まで巻き添えで倒さないための三値にしてある。
        // cutoffIrradiance は打ち切り照度τ(0以下で既定のまま)。Rangeをこれから解く。
        // maxCount は採用するプロキシ数の上限(0以下で既定のまま)。
        // **上限に当たったら切り捨てではなく併合を疑うこと** ―― 面積の大きい順に上位を
        // 残す形はEmeraldSquareの実測で発光の半分以上を捨てる。
        // doubleCountGI は 0=DDGIから自発光を抜く(既定) / 正=抜かずに二重に数える /
        // **負なら既定のまま**。抑止されるのはDDGIだけで、反射プローブ・RT反射・
        // G-Bufferの自発光には掛からない(鏡面が光源を直接見ているのは二重計上ではない)
        void SetEmissiveLights(int enabled, float cutoffIrradiance, int maxCount, int doubleCountGI);

        // 段階2: 発光面を三角形のまま面積分するか(0=無効 / 正=有効 / 負=既定のまま)。
        //
        // 【MegaLights 経路でのみ効く】DX11・非DXR・MegaLights無効のときは何も起きず、
        // 段階1のプロキシがそのまま光る。エミッシブ光源そのものが無効なら三角形も出ない。
        // **いまは参照実装(全三角形総当たり)しか無いので実シーンでは回らない。**
        void SetMeshLights(int enabled);

        // シーン全体の自発光の強度倍率(ImGuiの「自発光の強度」と同じ値)。0以下で既定のまま。
        //
        // 【検証に要る】glTFのemissiveFactorは[0,1]に収まるため、面積の小さい器具は
        // 物理的に暗すぎて1階調に届かない(実測: Bistroの電球は8bitの1階調の0.36倍)。
        // 単位の正しさを絵で確かめるには、この倍率を振れる必要がある
        void SetEmissiveIntensity(float intensity);


        // MegaLightsの出力を線形空間で何フレーム足し込むかを設定する(0で蓄積しない)。
        // 指定した枚数に達したら足すのを止めるので、表示が静止し
        // 「ちょうどNサンプルの平均」を決定的に撮れる。
        //
        // 【何のためにあるのか】確率的サンプリングの正しさは「平均が真値に一致するか」で決まるが、
        // スクリーンショットはトーンマップ後の8bitで、トーンマップは凹関数のため
        // **偏りがゼロでもノイズがあるだけで平均が低く出る**。N枚のスクリーンショットを
        // 平均しても検証にならない。デバッグ表示「MegaLights - 蓄積平均」と対で使う
        void SetMegaLightsAccumFrames(int frames);

        // 蓄積し終えた平均を、指定パスへ生データ(float4 × 画素数)で書き出す。
        // 形式: 'K','M','L','A' / uint32 幅 / uint32 高さ / uint32 足したフレーム数 / uint32 予約 /
        //       そのあとに float4 が 幅×高さ 個(index = y * 幅 + x)。
        //
        // 【何のためにあるのか】確率的サンプリングの検証は「平均が真値へ 1/√N で寄るか」を測る。
        // 画面キャプチャは8bit・トーンマップ後で、丸めだけでRMSEに0.29階調の下限が生まれ、
        // その下限に隠れて比が読めない。**物差しの分解能が足りないまま原因を断定しないため**、
        // 線形のまま倍精度で取り出せる経路を用意する
        void SetMegaLightsDumpPath(const wchar_t* path);

        // 空間再利用の有無と、借りる近傍の数・半径を起動時に上書きする。
        // いずれも負の値を渡すとその項目は既定のままにする。
        //
        // 【何のためにあるのか】空間再利用は「入れたら誤差が減るはず」の段で、
        // 有無を切り替えて同じ手順で撮り比べられないと効果を測れない。
        // UIのつまみで切り替えると再現性が落ちる(SetDebugViewIndexと同じ理由)
        void SetMegaLightsSpatial(int enabled, int neighborCount, int radius, int useMIS);
        // 初期サンプルの可視レイ(遮蔽されたサンプルをリザーバごと殺す)の有無。負の値は既定のまま
        void SetMegaLightsInitialVisibility(int enabled);
        // 【計測専用】GPUの区間計測をウォームアップ後に指定枚数ぶん集計し、
        // パス名ごとの平均[ms]をCSVへ書き出して終了する。
        //
        // 【Perfログでは段階7の測定ができない】あちらは0.05ms未満のパスを落とし、
        // しかも1フレームの代表値しか出さない。ライト数が少ないとMegaLightsのパスが
        // 消えてしまい、「ライト数に対して横ばいか」を測れない
        // 【計測専用】自動露出の有効/無効を起動時に決める。
        //
        // UI(PostProcessPanel)は m_Settings.PostProcess.AutoExposureEnabled を直接触るが、起動オプションから
        // 同じ状態を作れないと「画面で見ていた設定」と「計測で走らせた設定」を揃えられない。
        // 揃っていない条件どうしの比較は、差が手法の差なのか設定の差なのか分けられない
        void SetAutoExposureEnabled(bool enabled);

        // 【計測専用】Hi-Zオクルージョンカリングの有効/無効を起動時に決める。
        //
        // カリングは保守的でなければならない ―― 有効/無効で絵が1画素も変わらないことが
        // 正しさの定義そのものになる。その突き合わせをUIのチェックボックスでやると、
        // 撮影のたびに同じ操作を再現できず、押せていないのを「差分ゼロ＝合格」と
        // 読み違える(SetDebugViewIndexと同じ理由)。**A/Bは起動直後から同じ手順で行うこと**
        void SetOcclusionCullingEnabled(bool enabled);

        // 【計測専用】メッシュレット描画の有効/無効を起動時に決める。
        //
        // 無効にすると従来の頂点シェーダー + DrawIndexed の経路へ落ちる。この経路は
        // メッシュレット単位のカリング(視錐台・法線コーン・Hi-Z)を一切行わないので、
        // **「メッシュレット経路が何か落としていないか」を見るときの基準になる。**
        // 出力するPSInputの中身は両経路で同じにしてあり、絵は一致するのが正しい
        // (GBufferMeshlet.hlsl 冒頭のコメント)。一致しなければ増幅シェーダーの判定が
        // 保守的でない。UIのチェックボックスからしか切り替えられないと、
        // この基準を同じ起動手順で撮れない
        void SetMeshletRenderingEnabled(bool enabled);

        // 【計測専用】TAAの有効/無効を起動時に決める。
        //
        // TAAは時間方向に蓄積するため、フレームレートの揺れがそのまま画素差になる。
        // 画素単位の一致を測る比較では切っておかないと、再現性の下限が取れない
        void SetTAAEnabled(bool enabled);

        void SetAOTechnique(int technique);
        void SetSoftwareRasterEnabled(bool enabled);
        void SetDDGIHalfResolutionEnabled(bool enabled);
        void SetProbeUpdateMode(int mode);
        void SetUpscaleEnabled(bool enabled);
        void SetFixedTimeStep(float seconds);

        void SetPerfDump(const wchar_t* path, int frames);
        void SetPassManifest(const wchar_t* path, int frames);

        // 【検証専用】中間レンダーターゲットの中身を、線形の生値のままファイルへ書き出す。
        // nameは GetDumpableTextureNames() が返す名前(m_を外したメンバ名)。
        //
        // 【何のためにあるのか】「コンパイルは通るが絵が違う」を切り分ける唯一の数値経路。
        // 画面から採れるのは8bit・トーンマップ後で、G-Bufferの法線も深度も間接光も、
        // 表示のために加工された姿しか見られない。**加工前の値を数えられないと、
        // 「壊れている」と「そう見えるだけ」を区別できない。**
        // SetMegaLightsDumpPathが MegaLights の蓄積バッファ専用に用意した経路を、
        // 任意のレンダーターゲットへ一般化したもの。
        //
        // 複数回呼べば1回の起動で複数枚を同じフレームから落とす(GUIの起動は共有資源なので、
        // 1回の起動で必要な数値が全部取れる形にすること)。
        // 未知の名前・存在しないテクスチャ・非対応フォーマットはログを出して無視する
        void AddTextureDump(const wchar_t* name, const wchar_t* path, int mipLevel, int arraySlice, int frames, int stride);

        // 何フレーム目のものを書き出すか。負なら既定(Passes::kMegaLightsAccumWarmup)。
        // **整定を待たずに撮ると、内部解像度が既定値のままの絵を掴む**(実際に起きた)
        void SetTextureDumpFrame(int frame);

        // 書き出しが全部終わったらウィンドウを閉じる。無人での検証用
        void SetExitAfterDump(bool enabled);

        // 【検証専用】指定フレームでGPUリソースの作り直し経路を踏ませる予約を積む。
        // 複数回呼べば1回の起動で複数の経路を順に踏む(GUIの起動は共有資源なので、
        // 1回の起動で必要な経路が全部通る形にすること)。
        // 種別と各フィールドの意味は Diagnostics/ScheduledRecreation.h を見ること
        void AddScheduledRecreation(const ScheduledRecreation& request);

        // TAAの有無を起動時に上書きするのは SetTAAEnabled(上で宣言済み)。
        // ダンプの比較では、まずこれを切って再現性の下限をゼロにする ――
        // TAAのジッタは投影行列を毎フレームずらすため、同じ条件で2回撮っても
        // ダンプがビット一致しない

        // -dumptex が受け付けるテクスチャ名の一覧(表示・ログ用)。
        // ClaudeのようなUIを見られない利用者にとって、これが唯一の発見手段になる
        std::vector<std::string> GetDumpableTextureNames() const;

        // デノイザの有無、a-trousの段数、時間累積の上限。負/0は既定のまま
        void SetMegaLightsDenoise(int enabled, int atrousPasses, int maxFrames);
        // 輝度のエッジ停止の強さ(負なら既定のまま)
        void SetMegaLightsDenoiseSigmaLuminance(float sigma);
        // ファイアフライの近傍クランプの強さ(0で無効。負なら既定のまま)
        void SetMegaLightsDenoiseFireflyClamp(float k);
        // 空間再利用の反復回数(負なら既定のまま)
        void SetMegaLightsSpatialIterations(int iterations);
        // 時間再利用の有無と、履歴のMの上限。負/0は既定のまま
        void SetMegaLightsTemporal(int enabled, int mClamp);
        // クアッド共有(手法3)の設定。いずれも負の値ならその項目は既定のまま。
        //   share    … 2x2の仲間が撃ったレイの結果を借りるか。**0が陽性対照**で、
        //               手法2から再利用を外した構成と画素単位で一致するはず
        //   stratify … クアッドの4画素へ候補スロットを分けて引かせるか
        //   blockedCache … 遮蔽が確定した灯のキャッシュを使うか(陽性対照では0にする)
        void SetMegaLightsQuadShare(int share, int stratify, int blockedCache);
        // クアッド共有(手法3)の1画素あたりの標本数。1〜kMegaLightsMaxSamplesPerPixel。
        // 影レイの本数がそのままこの数になるので、コストはほぼ比例して増える
        void SetMegaLightsQuadSamples(int samples);
        // 候補プールが1タイルあたりに抽出する灯の数(K)。
        // kMegaLightsTilePoolMinCapacity 〜 kMegaLightsTilePoolCapacity
        void SetMegaLightsTilePoolCapacity(int capacity);
        // 候補プールのタイル格子を画素単位でずらすモード。
        // 0=無効(従来とビット同一)、1=Halton(2,3)、2=有効だが検証用にオフセット0固定。
        // 範囲外はログを出して無視し、負の値では既定値の状態をログへ残す
        void SetMegaLightsTileJitter(int mode);

        // 【検証専用】蓄積が始まった瞬間にシーンへ摂動を加える。時間再利用の「追従」を
        // 測るためのもので、静止した絵をいくら撮っても測れない側を測る入口。
        //   0 = 何もしない(既定)
        //   1 = 全ライトを消す。ゴースト(灯を消しても明かりが残る)の追従フレーム数を測る
        //   2 = 実効プリ露出EV100を +2 段跳ばす。プリ露出の補正が効いているかを測る
        // 蓄積ダンプは「総和」を書くので、Nを変えた2本の差を取れば1フレームぶんが取り出せる。
        // これで追従の時間変化を、フレームごとの読み戻し無しで測れる
        void SetMegaLightsPerturb(int mode);

        // カスケードシャドウマップの分割数。カメラ視錐台をこの数だけの深度範囲に分割し、
        // それぞれ専用のシャドウマップ・ライト正射影を持たせる。
        // FrameConstants::CascadeSplitsがXMFLOAT4(4要素)にfar距離を詰めているため、
        // この値を変える場合はKurenaiEngine3D.cppのCascadeSplits周りも合わせて変更が必要。
        // KurenaiEngine3D.cpp側の匿名名前空間(FrameConstants宣言)からも参照するためpublicにしている
        // 出所は Rendering/ShadowConstants.h(移行中の別名)
        static constexpr uint32_t kCascadeCount = Rendering::kCascadeCount;
        static_assert(kCascadeCount == 4, "CascadeSplitsはXMFLOAT4前提のため4カスケード固定");

        // --- オーサリングツール向けの口(Tools/KurenaiShowEditor) ------------------------
        //
        // ドローンショーの編集UIと形状生成は、出荷するエンジンのDLLに持ち込みたくない。
        // かといってエディタが自前でレンダラーを持つと、トーンマップ・ブルーム・露出が
        // 本番と違う経路を通り、そこで作った形は本番で見ると別物になる。
        // そこでエディタはこのエンジンをそのまま使い、下の2つだけを追加で呼ぶ。
        // IPanel/UIWidgetsといった内部の型を公開せずに済むよう、口はこの2つに留める。

        // 全パネルを描いた後に一度だけ呼ばれる追加のImGui描画。nullptrで解除。
        // 【Renderスレッドで呼ばれる】ImGuiの状態を触るのはRenderスレッドだけという
        // 不変条件があるため。コールバックの中からエンジンの状態を触ってよいのは、
        // それがRenderスレッドの持ち物である限りにおいて
        void SetExtraImGuiCallback(std::function<void()> callback);

        // 再生中のショーを差し替える(エディタのプレビュー用。ファイルを書かずに絵へ反映する)。
        // 【SetExtraImGuiCallbackで登録したコールバックの中から呼ぶこと】どちらもRenderスレッドで
        // 走るため、この経路なら同期が要らない。別スレッドから呼ぶとm_DroneShowを
        // 描画中に書き換えることになる
        void ApplyDroneShowData(const Assets::ShowData& data);

        // --- UIパネルから呼ばれる操作 ---

        // シーン切り替えを要求する(ScenePanel = Renderスレッドから呼ばれる)。
        // 実際の読み込みはLoaderスレッドが行うため即座に戻る。
        // 読み込み中に再度要求された場合は新しい要求で上書きされる(最後の要求が勝つ)
        void RequestSceneLoad(size_t sceneIndex);
        // 平面反射の反射解像度の倍率変更を要求する(RenderingPanel = Renderスレッドから呼ばれる)。
        // RequestRenderResolutionと同じ方式(要求を記録するだけにしてRender()の先頭でまとめて反映)
        void RequestPlanarReflectionResolutionScale(float scale);
        // SSAO/SSILの半径・厚みとSSRの距離・厚みを、現在のシーンの対角長から決め直す。
        // これらは固定の既定値を持たないため、UIの「既定値に戻す」ではなくこれを呼ぶ
        // (シーン読み込み時はApplyLoadedSceneから呼ばれる)
        void ResetSceneDependentParams();
        // ProfilerPanel用。m_DeviceはKurenaiEngineBaseのprotectedメンバであり、
        // 派生クラスの外からは直接触れないため、ここで明示的に橋渡しする
        float GetLastFrameGPUWaitTimeMs() const;
        // SystemPanelの表示用。m_Windowも同様の理由で橋渡しする。
        // Windowsのディスプレイ設定で指定されている拡大率(UIの拡大率もこれに追従する)
        float GetMonitorDpiScale() const;
        // 超解像の設定をまとめて要求する(SystemPanel = Renderスレッドから呼ばれる)。
        // 内部でRequestRenderResolution()を呼ぶだけで、レンダーターゲットの作り直しはしない
        void RequestUpscaleSettings(
            bool enabled, UpscaleQualityMode mode, uint32_t outputWidth, uint32_t outputHeight);
        // このフレームでMegaLightsパスを実行するか。上のShouldRunRaytraced*と同じ作法で1か所に集約している。
        // これがfalseのときDirectLighting.hlslは従来のライトループへ戻る ―― 「パスを積むか」と
        // 「ライトループを止めるか」がずれると、ライトが二重に加算されるか、逆に全部消える
        bool ShouldRunMegaLights() const;
        // プリセットを適用する(SystemPanel = Renderスレッドから呼ばれる)
        void ApplyQualityPreset(QualityPreset preset);
        // いま選ばれている段を1つだけ返す(フェード中でも切り替え先だけ)。
        // 半透明・平面反射・ソフトウェアラスタライザ用 ―― これらはクロスディザを実装しておらず、
        // 2段を重ねると同じ画素に両方が描かれてしまうため、フェード中も1段に決め打つ
        const Assets::Model* GetCurrentLOD(size_t instanceIndex) const;
        // 「システム」パネルのグラフィックスAPI切り替えコンボから呼ばれる。
        // 実際の作り直しはRun()から戻った後に呼び出し側が行う(上のHasPendingGraphicsAPIChange参照)
        void RequestGraphicsAPIChange(GraphicsAPI api);

        // --- UIパネル向けのアクセサ ---
        //
        // UIパネル群(Source/Engine/UI/)がImGuiウィジェットへアドレスで渡すもの、および表示に
        // 使うものだけをここで名指しする。**この一覧に無いもの(RHIリソース・PSO・履歴バッファ等)
        // へUIから触らせない**のがこの区画の目的なので、増やすときは本当にUIの表示・操作に
        // 要るものかを確かめること。
        // UIが書き換えるものは非const参照(アドレスを渡す/直接代入するため)、読むだけのものは
        // const(値またはconst参照)で返す。
        AmbientOcclusionSettings& GetAmbientOcclusionSettings() { return m_Settings.AmbientOcclusion; }
        CloudSettings& GetCloudSettings() { return m_Settings.Cloud; }
        DDGISettings& GetDDGISettings() { return m_Settings.DDGI; }
        DebugViewSettings& GetDebugViewSettings() { return m_Settings.DebugView; }
        EmissiveLightSettings& GetEmissiveLightSettings() { return m_Settings.EmissiveLight; }
        FogSettings& GetFogSettings() { return m_Settings.Fog; }
        GeometrySettings& GetGeometrySettings() { return m_Settings.Geometry; }
        IBLSettings& GetIBLSettings() { return m_Settings.IBL; }
        MegaLightsSettings& GetMegaLightsSettings() { return m_Settings.MegaLights; }
        PostProcessSettings& GetPostProcessSettings() { return m_Settings.PostProcess; }
        ReflectionProbeSettings& GetReflectionProbeSettings() { return m_Settings.ReflectionProbe; }
        ReflectionSettings& GetReflectionSettings() { return m_Settings.Reflection; }
        ShadowSettings& GetShadowSettings() { return m_Settings.Shadow; }
        SkySettings& GetSkySettings() { return m_Settings.Sky; }
        StarsSettings& GetStarsSettings() { return m_Settings.Stars; }
        SystemSettings& GetSystemSettings() { return m_Settings.System; }
        WaterSettings& GetWaterSettings() { return m_Settings.Water; }

        std::vector<Assets::Light>& GetLights() { return m_Lights; }
        int& GetSelectedLightIndex() { return m_SelectedLightIndex; }
        bool& GetBufferPrecisionDirty() { return m_BufferPrecisionDirty; }
        bool& GetSkyBakeDirty() { return m_SkyBakeDirty; }
        // 焼き上がりの状態の持ち主は Passes::EnvironmentPasses。ここは委譲するだけ
        bool& GetIBLBaked();
        bool& GetIBLIrradianceBaked();
        bool& GetEmissiveLightsCapLogged() { return m_EmissiveLightsCapLogged; }
        bool& GetEmissiveLightsValuesLogged() { return m_EmissiveLightsValuesLogged; }
        bool& GetDDGIEmissiveSuppressLoggedRaster();
        bool& GetDDGIEmissiveSuppressLoggedTrace();
        std::vector<Assets::ReflectionProbe>& GetReflectionProbes() { return m_GIResources.ReflectionProbes; }
        int& GetSelectedProbeIndex() { return m_SelectedProbeIndex; }
        // 焼き上がりの状態の持ち主は Passes::ReflectionProbePasses。ここは委譲するだけ
        bool& GetProbeBaked();
        bool& GetProbeBakeRequested();
        bool& GetDDGIUpdateSuspended();
        uint32_t& GetDDGIStableCycles();
        Assets::TextureStreamingManager& GetTextureStreaming() { return m_TextureStreaming; }

        // std::atomicは呼び出し側が使っているメモリオーダーの書き方(.load/.store)を
        // そのまま維持できるよう、値ではなくatomicへの参照を返す
        std::atomic<bool>& GetTAAHistoryValid() { return m_TAAHistoryValid; }
        std::atomic<uint32_t>& GetSceneLoadProgressLoaded() { return m_SceneLoadProgressLoaded; }
        std::atomic<uint32_t>& GetSceneLoadProgressTotal() { return m_SceneLoadProgressTotal; }

        uint32_t GetHiZMipLevels() const;
        const std::vector<Assets::EmissiveProxy>& GetEmissiveProxies() const { return m_EmissiveProxies; }
        const RenderStats& GetRenderStats() const { return m_RenderStats; }
        RHI::IRHIGPUProfiler* GetGPUProfiler() const { return m_GPUProfiler.get(); }
        const Core::CPUProfiler& GetCPUProfiler() const { return m_CPUProfiler; }
        uint32_t GetProbeRealtimeProbeIndex() const;
        uint32_t GetProbeRealtimeFace() const;
        const Assets::Scene& GetScene() const { return m_Scene; }
        // 【publicにしてある】G-Bufferパスが水面インスタンスのt7へ張る。
        // 差し替えるのはシーン読み込み(Scene/SceneLoadService.cpp)なので持ち主は変えない
        RHI::IRHITexture* GetWaterNormalMapTexture() const { return m_WaterNormalMapTexture.get(); }
        // 【publicにしてある】カウンタをGPUからコピーするのはジオメトリのパス群で、
        // 読み戻して数値にするのはこちらのRender()。受け皿のリングはこちらが持つ
        RHI::IRHIBuffer* GetMeshletCullStatsReadbackSlot() const { return m_CullStats.GetMeshletWriteSlot(); }
        RHI::IRHIBuffer* GetModelCullReadbackSlot() const { return m_CullStats.GetModelWriteSlot(); }
        const RenderCapabilities& GetRenderCapabilities() const { return m_RenderCapabilities; }
        bool GetHasGIVolume() const { return m_GIResources.HasGIVolume; }
        const Assets::GIVolume& GetGIVolume() const { return m_GIResources.GIVolume; }
        uint32_t GetDDGIProbeCount() const { return m_GIResources.DDGIProbeCount; }
        bool GetDDGIWarmingUp() const;
        ReflectionMode GetSceneDefaultReflectionMode() const { return m_SceneDefaultReflectionMode; }
        bool GetSceneLoadInFlight() const { return m_SceneLoadInFlight; }
        const std::vector<std::wstring>& GetSceneDisplayNames() const { return m_SceneDisplayNames; }
        size_t GetSceneLoadingIndex() const { return m_SceneLoadingIndex; }
        const QualitySettings& GetQualitySettings() const { return m_Settings.Quality; }
        uint32_t GetLightTileCountX() const { return m_RenderTargets.LightTileCountX; }
        uint32_t GetLightTileCountY() const { return m_RenderTargets.LightTileCountY; }
        GraphicsAPI GetGraphicsAPI() const { return m_GraphicsAPI; }

        // m_DeviceはKurenaiEngineBaseのprotectedメンバであり、GetLastFrameGPUWaitTimeMsと
        // 同じ理由でここから明示的に橋渡しする。
        // 生成に失敗していればnullptrになりうるので、参照ではなくポインタで返す
        RHI::IRHIDevice* GetDevice() { return m_Device.get(); }

    private:
        // 機能ごとの設定18個をまとめた入れ物。**Rendering::RenderSettingsSnapshot は
        // これの写し**で、実体を共有させてはいけない(理由は EngineSettings.h)
        Settings::EngineSettings m_Settings;

        // UpdateスレッドからRenderスレッドへ、1フレーム分のカメラ・ImGui表示状態を引き渡すための
        // スナップショット。m_Settings.Sky.TimeOfDay等それ以外の状態はRenderスレッド側のみが読み書きするため
        // ここには含めない(RenderThreadMain参照)
        struct FrameState
        {
            Core::Camera Camera;
            bool ImGuiVisible = true;
        };

        void CreateSceneResources();
        // 中間バッファの精度構成(m_Settings.System.Precision)によって変わるフォーマット。
        // レンダーターゲットの作成(CreateRenderTargets)と、そこへ描くPSOのRenderTargetFormats
        // 宣言の両方がこれを使う。両者がずれるとD3D12では仕様違反(デバッグレイヤーがID 613を出す)
        // になるため、値の出所をこの2関数に一本化している
        RHI::Format GetEmissiveFormat() const;
        RHI::Format GetAOFormat() const;
        // 上記のフォーマットに依存するPSOを作る(G-Buffer・SSAO・SSIL・AOブラー)。
        // 初回はCreateSceneResourcesの末尾から、以降はバッファ精度が切り替わるたびに
        // Render()から呼び直す。GPUがまだ参照しているPSOを壊さないよう、呼び出し側で
        // WaitForGPUIdleを済ませておくこと
        void CreatePrecisionDependentPipelineStates();
        // パス用途ごとのサンプラーセット(m_MaterialSamplers / m_ScreenSpaceSamplers)を作る。
        // セットの中身は作成後に書き換えないことが前提のAPIなので、描画を始める前に一度だけ呼ぶ
        // (理由はRHI/IRHISamplerSet.h)
        void CreateSamplerSets();
        void CreateRenderTargets(uint32_t width, uint32_t height);
        // 平面反射専用のレンダーターゲット2枚(m_RenderTargets.PlanarReflectionColor/Depth)を、
        // 反射解像度(レンダー解像度 × m_Settings.Reflection.PlanarResolutionScale)で作り直す。
        // メインのCreateRenderTargetsとは独立に呼べる(Legacy8bitフォールバックの対象外。
        // このバッファは常にHDR固定フォーマットのため)。呼び出し箇所はCreateRenderTargetsと
        // 同じ2か所(Initialize直後、Render()の解像度変更ハンドリング)
        void CreatePlanarReflectionTargets();
        // このフレームでRT反射パスを実行するか。手法がRaytracedでも、高速化構造が無ければ
        // (非対応環境・シーン読み込み中の空シーン・構築失敗)撃つ相手がいないため実行しない。
        // 「パスを追加する条件」と「後段がその出力を読む条件」がずれると、
        // 実行していないパスの出力(前フレームの残骸)を読むことになるため、判定はこの1か所に置く
        bool ShouldRunRaytracedReflection() const;
        // このフレームでRTシャドウパスを実行するか。ShouldRunRaytracedReflectionと同じ理由で
        // 判定を1か所に集約している(パスを追加する条件とDirectLightingがその出力を読む条件が
        // ずれると、実行していないパスの残骸を影として使ってしまう)
        bool ShouldRunRaytracedShadow() const;
        // このフレームでRTAOパスを実行するか。上2つと同じ理由で判定を1か所に集約している
        bool ShouldRunRaytracedAO() const;
        // DDGIのプローブ取得をレイトレースで行うか。
        // 「パスを走らせるか」と「その出力を読むか」を同じ1つの述語で判定するための関数
        // (ShouldRunRaytraced*と同じ作法)
        bool ShouldRunRaytracedDDGITrace() const;
        // このフレームでタイルライトカリングパスを実行するか。上と同じ作法で1か所に集約している。
        // **ライトグリッドを実際に読む者が居るときだけ積む** ―― 読み手は
        // DirectLighting.hlsl のローカルライトのループと Present.hlsl のライトグリッド表示
        // (Mode 11)の2つしかなく、MegaLightsが走るフレームは前者がLightCount.wで止まっている
        bool ShouldRunLightCulling() const;
        // いま1画素あたり何本の標本(リザーバ)を引くか。**バッファの確保も定数バッファも
        // 必ずこの関数を通すこと** ―― 2か所で別々に計算すると静かに食い違う。
        // 手法3以外は常に1(手法2の再利用が1画素1リザーバを前提にしているため)
        int32_t MegaLightsSamplesPerPixel() const;
        // このメッシュをメッシュシェーダー経路で描くか。上のShouldRun*と同じく、
        // 「どのPSOを束ねるか」と「DispatchMeshとDrawIndexedのどちらを積むか」の判断が
        // ずれると即座に破綻するため、判定を1か所に集約する。
        // isWaterがtrueのメッシュは常にfalse(理由は実装のコメント参照)
        bool ShouldUseMeshletPath(const Assets::Model& model, const Assets::Mesh& mesh, bool isWater) const;
    public:
        // このインスタンスを「1回のDispatchMeshでモデル全体」の経路で描けるか。
        // 描けない場合は従来どおりメッシュ単位のループで描く
        // modelは「このパスが描く段」。モデルLODが入ったのでinstance.Model(最も詳細な段)とは
        // 限らず、シャドウは最も粗い段、G-Buffer/プリパスは選ばれた段を渡す
        //
        // 【publicにしてある】ForEachGeometryDrawと対で使う述語で、Passes/*の各群が
        // コールバックの中から呼ぶ。状態を持たない判定なので公開しても持ち主は変わらない
        bool ShouldUseModelMeshletPath(const Assets::ModelInstance& instance, const Assets::Model& model) const;

        // メッシュ単位カリングの判定を、共通の描画ループとまったく同じカウンタへ数えながら行う。
        //
        // 【publicにしてある】自前ソフトウェアラスタライザは共通ループへ判定を任せられない
        // (三角形が3つ未満のメッシュを先に落とすため、任せると分母がずれる)。
        // それでも統計は共通ループと同じ2つのカウンタへ積む必要がある
        bool IsMeshVisibleCounted(
            const Rendering::FrustumPlanes& frustum, const Assets::ModelInstance& instance,
            const Assets::Model& model, const Assets::Mesh& mesh);

    private:
        // このフレームでライティングパス等が読むべきAO/GIバッファ(ブラー後 / ブラー前の生値)。
        // AO無効時はm_AODisabledTexture、Raytracedを選んでいても実行できないフレームはSSAOのもの
        RHI::IRHITexture* GetActiveAOTexture() const;
        RHI::IRHITexture* GetActiveAORawTexture() const;
        // このフレームでHDRのシーン色として後段(自動露出・ブルーム・トーンマップ)が読むべき
        // テクスチャを返す。反射パスを実行したならその出力、していなければRenderTargets::SceneColor
        RHI::IRHITexture* GetActiveReflectionOutput() const;
        // このフレームで空として使うキューブマップを返す。手続き空が有効で、かつ.ksceneが
        // スカイボックスを明示していないときだけ手続き空を使う(明示しているシーンは
        // そのDDSでなければ意味を成さないため。White Furnace Testが該当する)。
        //
        // 【重要】Render()の冒頭で一度だけ呼んでローカル変数へ保持し、RenderGraphの
        // Reads宣言と実際のバインドの両方で同じポインタを使うこと。
        // 呼び出しごとに評価すると両者が食い違い、依存解決が壊れる
        RHI::IRHITexture* ActiveSkyTexture() const;
        // <DLLフォルダ>/Assets/Scenes/*.ksceneを列挙し、m_SceneFilePaths/m_SceneDisplayNamesを構築する。
        // 個々のファイルの[Scene]Name読み取りに失敗した場合はそのファイルを警告ログとともに
        // スキップする(1ファイルの不備でアプリ全体が起動できなくなるのを避けるため)
        void DiscoverScenes();

        // --- シーン読み込みのスレッド分担 -----------------------------------------------------
        //
        // シーンの読み込みは「重いファイルI/O・デコード・GPUリソース作成」と「一瞬で終わる
        // エンジン状態への反映」に分かれる。両方をUpdateスレッドで行いRender()全体と
        // ミューテックスで排他すると、読み込みの間フレームが1枚も進まない(Bistro Exteriorで約1.3秒)。
        //
        // そのため前者を専用のLoaderスレッドへ、後者をRenderスレッドのフレーム境界へ分ける。
        // 読み込み中もフレームが進み続け、排他は受け渡しの一瞬だけで済む
        // (詳細はdocs/Architecture.html 23章)。

        // Renderスレッドが不要になったアセット由来のGPUリソースをまとめてLoaderスレッドへ渡すための箱。
        //
        // 【なぜRenderスレッドで破棄しないのか】アセット由来のリソースのディスクリプタは
        // アセット用のディスクリプタヒープから確保されており、そのヒープはロックを持たない
        // (DX12Device::GetAssetSrvCpuHeap参照)。確保するのがLoaderスレッドなので、
        // 解放も同じスレッドに寄せることでロックなしのまま安全にする
        struct RetiredAssets
        {
            Assets::Scene Scene;
            Assets::RaytracingScene RaytracingScene;
            // メッシュライトの三角形テーブル(段階2)。RaytracingSceneと同じ扱い
            Assets::MeshLightScene MeshLightScene;
            std::unique_ptr<RHI::IRHITexture> SkyboxTexture;
            // 水面法線マップ版。SkyboxTextureとまったく同じ扱い
            std::unique_ptr<RHI::IRHITexture> WaterNormalMapTexture;
        };

        // Loaderスレッドが作り、Renderスレッドが受け取る「差し替えられる状態まで仕上がったシーン」
        struct LoadedScene
        {
            Assets::Scene Scene;
            Assets::RaytracingScene RaytracingScene;
            Assets::MeshLightScene MeshLightScene;
            size_t SceneIndex = 0;
            // シーンの[Scene]Skyboxが読み込み済みのものと異なる場合のみ非nullptr。
            // nullptrなら現在のスカイボックスを維持する
            std::unique_ptr<RHI::IRHITexture> SkyboxTexture;
            std::wstring SkyboxPath;
            // シーンの[Water]NormalMapが読み込み済みのものと異なる場合のみ非nullptr。
            // nullptrなら現在の水面法線マップ(またはフラット法線フォールバック)を維持する
            std::unique_ptr<RHI::IRHITexture> WaterNormalMapTexture;
            std::wstring WaterNormalMapPath;
            // ComputeInitialCameraの結果(Updateスレッドが所有するm_Cameraへ後で反映される)
            Core::Camera Camera;
        };

        // 内部レンダー解像度の変更を要求する(SystemPanel = Renderスレッドから呼ばれる)。
        // レンダーターゲットの作り直しはGPUがそれらを参照していない状態で行う必要があるため、
        // ここでは要求を記録するだけにしてRender()の先頭でまとめて反映する
        void RequestRenderResolution(uint32_t width, uint32_t height);
        // Renderスレッドがフレーム先頭で呼ぶ。保留中の切り替え要求の発注と、
        // 出来上がったシーンの取り込みを行う
        void UpdateSceneStreaming();
        // .ksceneの更新時刻を見て、変わっていれば再読み込みを要求する。
        // UpdateSceneStreamingの先頭から呼ぶ。m_Settings.System.SceneAutoReloadEnabledがfalseなら何もしない
        void UpdateSceneHotReloadWatch();
        // 現在のシーンの.ksceneの最終更新時刻。取得できなければ0を返す
        // (ファイルが一時的に開けない、削除された等。0のときは何もしないのが正しい振る舞い)
        uint64_t GetCurrentSceneFileWriteTime() const;
        // Loaderスレッドの本体。要求を待ち、旧シーンを破棄し、新シーンを読み込んで publish する
        void LoaderThreadMain();
        // Loaderスレッドで実行する読み込み本体。エンジンの状態は一切書き換えない。
        // 失敗した場合はログを出してnullptrを返す
        std::unique_ptr<LoadedScene> LoadSceneOnLoaderThread(size_t sceneIndex);
        // Renderスレッドで実行する反映。出来上がったシーンを現在のシーンと差し替え、
        // シーン由来の設定(太陽・影・AO・SSR・ライト・反射プローブ・ベイクフラグ等)を適用する
        void ApplyLoadedScene(LoadedScene& loaded);
        // 不要になったアセット由来のリソースをLoaderスレッドへ破棄依頼として積む。
        // 【重要】呼ぶ前にIRHIDevice::WaitForGPUIdle()でGPUの参照が終わっていることを保証すること
        void RetireAssets(RetiredAssets&& retired);
        // シーンのAABBから初期カメラ(位置・向き・near/far)を決める。エンジンの状態を読まない
        // 純粋な計算なのでLoaderスレッドから呼べる([Camera]セクションがあればそれを優先する)
        static Core::Camera ComputeInitialCamera(const Assets::Scene& scene);

        // imguiWantsMouseはImGuiがマウス入力を掴んでいるか(Renderスレッドから
        // m_ImGuiWantCaptureMouse経由で受け取る)。パネルの上で右ドラッグを始めても
        // 視点回転が始まらないようにするために使う
        void UpdateMouseLook(bool imguiWantsMouse);
        void UpdateMovement(float deltaTime);
        void UpdateImGuiToggle();
        // ApplyLoadedScene(Renderスレッド)が公開した初期カメラ・ウィンドウタイトルを、
        // まだ適用していなければ適用する。m_Cameraの書き込み手をUpdateスレッド1つに保ち、
        // ウィンドウタイトルの変更もウィンドウを所有するこのスレッドから行うためのハンドオフ
        void UpdateAppliedSceneHandoff();
        void Update(float deltaTime);
        // 1フレーム分のUpdateと、Renderスレッドへのフレーム状態の受け渡しを行う。
        // 通常はRun()のループから、ウィンドウのドラッグ中(Windowsのモーダルループ中で
        // PumpMessagesが戻ってこない間)はWindowのタイマーから呼ばれる
        void TickFrame();
        void RenderThreadMain();
        void Render(const FrameState& frameState);
        // --- Render()から切り出したフレームの先頭(段階6.6のAブロック) ---
        // 【定義はRendering/RenderFrame.cppにある】下のEブロックと同じ作法。
        // どれも「このフレームのGPUコマンドをまだ1つも積んでいない」ことを前提にしており、
        // **Render()の先頭から呼ぶ位置を動かさないこと**
        //
        // フレーム単位の統計を前フレームぶんへ控えてから0に戻す
        void ResetFrameCounters();
        // ウィンドウのリサイズ・予約された作り直し・シーンの切り替え・常駐ミップの差し替えを
        // 確定させる。どれもGPUコマンドを積む前でなければならない
        void ApplyPendingRecreations();
        // ImGuiのフレームを開始し、パネルを描いて、入力を掴んでいるかをUpdateスレッドへ返す
        void BeginImGuiFrame(const FrameState& frameState);
        // バッファ精度・内部レンダー解像度・平面反射解像度・超解像の出力を、
        // 要求が立っていれば作り直す。確保に失敗したら元の解像度へ戻す縮退を持つ
        void RecreateDirtyRenderTargets();
        // そのフレームのキー照度[lx]から実効プリ露出(m_EffectiveExposureEV100)を決める
        void UpdateEffectiveExposure(float keyIlluminanceLux);
        // 【検証専用】蓄積が始まる瞬間に1回だけ摂動を加える(-perturbmode)
        void ApplyMegaLightsPerturbationIfDue();
        // モデルLOD・モデルのストリーミング・インスタンスのバッチ・レイトレーシングの
        // 作り直し・テクスチャの常駐目標を、レンダーグラフを組む前にこの1回だけ進める
        void UpdateSceneForFrame(
            RHI::IRHICommandList* commandList, const DirectX::XMFLOAT3& cameraPosition, const Core::Camera& camera);

        // --- Render()から切り出したフレームの値の組み立て(段階6.6のB+Cブロック) ---
        // ジッター・ライト配列・空のパラメータ・FrameConstants・LightingConstants を
        // 組み立て、frameContext のフィールドを埋める(ビューポート・BakedLightCount・
        // ProbeFaceProjection・ProbeCaptureReads・CascadeViewProj の5つだけは
        // RegisterPasses が登録の合間に埋める)。
        //
        // 【定義だけKurenaiEngine3D.cppに残してある】A/D/EブロックはRendering/RenderFrame.cppへ
        // 移したが、この本体はあちらの無名名前空間のkMaxLights・MakeGPULight・
        // GPUReflectionProbe等に依存しており、それらを外へ出すのは別の関心事になる。
        //
        // 【後半4つを出力引数で受ける理由】frameContext.Lights / Lighting / Constants が
        // これらを指す。この関数のローカルにすると graph.Execute() の時点で解放済みになるが、
        // 解放直後なら中身が残っていて同じ絵が出るため、採取では絶対に捕まらない。
        // 寿命を Render() のスコープに保つため、実体は呼び出し側に置く
        // --- BuildFrameContext から切り出した組み立ての各段(段階7.5) ---
        // 【定義は Rendering/RenderFrameBuild.cpp にある】呼ぶ順が実行順の一部で、
        // 後段が前段の書いた frameContext のフィールドを読む。並べ替えないこと
        void DecideFrameJitterAndCamera(
            const FrameState& frameState, Rendering::RenderFrameContext& frameContext);
        void UpdateMeshletLODFrame(const FrameState& frameState);
        void EvaluateDroneShowFrame();
        void UploadDroneInstances(RHI::IRHICommandList* commandList);
        void BuildGpuLightList(
            std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition,
            size_t& bakedLightCount);
        void AppendEmissiveProxyLights(
            std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition,
            size_t manualLightCount, size_t& bakedLightCount);
        void AppendDroneLights(
            std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition);
        void ClampLightsToCapacity(
            std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition,
            size_t& bakedLightCount);
        void ResolveSkyFrameState(
            const SunLighting& sunLighting, Rendering::RenderFrameContext& frameContext);
        void ResolveFrameDrawDecisions(Rendering::RenderFrameContext& frameContext);
        void FillFrameConstants(
            const FrameState& frameState, RHI::IRHICommandList* commandList,
            const SunLighting& sunLighting, float effectiveExposure, float manualExposureScale,
            float keyReferenceEV100, const DirectX::XMFLOAT3& cameraPosition,
            const float (&cascadeSplits)[kCascadeCount],
            const DirectX::XMMATRIX (&cascadeViewProj)[kCascadeCount],
            Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
            ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants,
            size_t& bakedLightCount);
        void ResolveOcclusionCullingFrameState(
            RHI::IRHICommandList* commandList, const DirectX::XMFLOAT3& cameraPosition,
            Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
            ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants);
        void FillFrameContextSnapshot(
            const SunLighting& sunLighting, float effectiveExposure, float manualExposureScale,
            float keyReferenceEV100, const DirectX::XMFLOAT3& cameraPosition,
            Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
            ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants);

        void BuildFrameContext(
            const FrameState& frameState, RHI::IRHICommandList* commandList,
            const SunLighting& sunLighting, float effectiveExposure, float manualExposureScale,
            float keyReferenceEV100, const DirectX::XMFLOAT3& cameraPosition,
            const float (&cascadeSplits)[kCascadeCount],
            const DirectX::XMMATRIX (&cascadeViewProj)[kCascadeCount],
            Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
            ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants,
            size_t& bakedLightCount);

        // --- Render()から切り出したパスの登録(段階6.6のDブロック) ---
        // 【定義はRendering/RenderFrame.cppにある】13回のRegisterを1つにまとめたもの。
        // **中の呼び出し順は実行順の一部**で、RenderGraphは依存が同点のとき最小登録番号を
        // 選ぶ。1つでも入れ替えると実行順が変わる。
        //
        // 【probeCaptureReadsを引数で受ける理由】frameContext.ProbeCaptureReadsがこれを指し、
        // graph.Execute()の時点でも生きている必要がある。この関数のローカルにすると
        // 解放済みのメモリを指すが、直後なら中身が残っていて同じ絵が出るため採取では捕まらない
        void RegisterPasses(
            Core::RenderGraph& graph, Rendering::RenderFrameContext& frameContext,
            Rendering::RenderBlackboard& blackboard, RHI::IRHICommandList* commandList,
            std::vector<RHI::IRHITexture*>& probeCaptureReads, size_t bakedLightCount,
            RHI::IRHITexture* skyTexture, const DirectX::XMMATRIX (&cascadeViewProj)[kCascadeCount],
            const Core::Camera& camera);

        // --- Render()から切り出したフレームの締め(段階6.6のEブロック) ---
        // 【定義はRendering/RenderFrame.cppにある】KurenaiEngine3Dのメンバ関数のまま、
        // 翻訳単位だけを分けている(Diagnostics/RenderDumpService.cppと同じ作法)。
        // **Render()から呼ぶ順序が実行順の一部**なので、呼ぶ位置を動かさないこと
        //
        // GPUカリングの結果(メッシュレット統計とモデル単位)を読み戻す。
        // graph.Execute()の直後、Presentより前で呼ぶ
        void ResolveFrameCullStats(const Rendering::RenderBlackboard& blackboard, bool meshletCullStatsActive);
        // ImGuiの描画をバックバッファへ重ね、GPU計測を締めてPresentする
        void SubmitAndPresentFrame();
        // 次フレームが「前フレーム」として参照する行列・ジッター・カメラ位置を確定させる
        void AdvanceFramePrevViewState(const ShaderInterop::FrameConstants& constants, const DirectX::XMFLOAT2& jitterUv);
        // 履歴テクスチャのping-pongを反転する(TAA / MegaLightsの時間再利用・デノイザ)。
        // 【ResolveTextureDumps()より後で呼ぶこと】ダンプは今フレームの書き込み先を読む
        void AdvanceFrameHistory();
        // このフレームの計測値を集計し、集計期間(FrameStatsLogIntervalSeconds)ぶん溜まっていれば
        // 1行にまとめてログへ出す。Renderスレッドからフレームごとに呼ぶ
        void LogFrameStatsIfDue(float renderDeltaTime);
        // カメラ視錐台をkCascadeCount個の深度範囲に分割する(near/far境界、View空間での距離)。
        // 対数分割と均等分割を混合した実用的な分割(Practical Split Scheme)を使う
        void ComputeCascadeSplits(const Core::Camera& camera, float (&outSplits)[kCascadeCount]) const;
        // カメラ視錐台のうち[splitNear, splitFar]の範囲(View空間距離)だけを覆う、平行光のライト視点
        // 正射影ビュー・プロジェクション行列を求める。カスケードごとに1回呼ぶ
        DirectX::XMMATRIX ComputeCascadeLightViewProj(
            const DirectX::XMFLOAT3& lightDirection, const Core::Camera& camera, float splitNear, float splitFar) const;

        // 起動時に選択されたグラフィックスAPI(タイトルバー・ImGui表示用に保持)
        GraphicsAPI m_GraphicsAPI;

        // 「システム」パネルから要求された切り替え先のAPI。-1なら要求なし。
        // UI(Renderスレッド)が書き、Run()のループ条件(Updateスレッド)が読むためatomic。
        // 実際の作り直しはRun()から戻った後に呼び出し側が行う(上のHasPendingGraphicsAPIChange参照)
        std::atomic<int> m_RequestedGraphicsAPI{ -1 };

        // 起動時に読み込むシーンの番号。コンストラクタ引数をそのまま保持する
        // (APIを切り替えても同じシーンで再開できるようにするため)
        size_t m_InitialSceneIndex = 0;

        // ImGuiのIniFilenameはポインタを保持するだけでコピーしないため、文字列の寿命をここで維持する。
        // m_ImGuiBackendのデストラクタ(ImGui::DestroyContextで最終保存)より後に破棄されるよう、
        // メンバ破棄順(宣言の逆順)に従いm_ImGuiBackendより前で宣言する
        std::string m_ImGuiIniPath;

        // KurenaiEngineBaseが破棄される(m_Deviceが破棄される)前にImGuiのバックエンドを
        // 終了させる必要があるが、基底クラスのメンバは派生クラスのメンバより後に破棄されるため
        // (C++の破棄順の規則上)、この宣言順のままで安全に成立する
        std::unique_ptr<RHI::IRHIImGuiBackend> m_ImGuiBackend;

        // UIパネル群の所有者。ImGuiコンテキストが生きている間だけ有効であればよいため、
        // m_ImGuiBackendより後に宣言してメンバ破棄順(宣言の逆順)で先に破棄させる。
        // UI::UIManagerは不完全型のままにするため、デストラクタは.cpp側で定義する
        std::unique_ptr<UI::UIManager> m_UIManager;

        // Render()から切り出したパス群(段階6)。
        //
        // 【エンジンへの参照を持たせている】段階6は「登録順を1つも変えない」ことだけを
        // 決め手に進めており、その担保はパスマニフェストの完全一致である。状態の引っ越しと
        // 登録位置の移動を同時にやると、食い違ったときにどちらが原因か分けられない。
        // まず登録コードだけを機械的に移し、リソースの所有権は後から群へ移した。
        // 【所有権の引っ越しは完了している】群はエンジンのprivateを触らず、
        // 必要なものは公開のアクセサ(GetScene / ForEachGeometryDraw など)から取る。
        // 不完全型のままにするため、デストラクタは.cpp側で定義する
        std::unique_ptr<Passes::DDGIPasses> m_DDGIPasses;
        std::unique_ptr<Passes::EnvironmentPasses> m_EnvironmentPasses;
        std::unique_ptr<Passes::GeometryPasses> m_GeometryPasses;
        std::unique_ptr<Passes::LightingPasses> m_LightingPasses;
        std::unique_ptr<Passes::MegaLightsPasses> m_MegaLightsPasses;
        std::unique_ptr<Passes::PostProcessPasses> m_PostProcessPasses;
        std::unique_ptr<Passes::ReflectionPasses> m_ReflectionPasses;
        std::unique_ptr<Passes::ReflectionProbePasses> m_ReflectionProbePasses;
        std::unique_ptr<Passes::ShadowPasses> m_ShadowPasses;
        std::unique_ptr<Passes::PresentPass> m_PresentPass;

        // SetExtraImGuiCallbackで登録された追加のImGui描画(Tools/KurenaiShowEditor)。
        // Renderスレッドだけが読み書きする
        std::function<void()> m_ExtraImGuiCallback;

        // ImGuiが入力を掴んでいるかを、RenderスレッドからUpdateスレッドへ返す逆方向のハンドオフ。
        // FrameState(Update→Render)の逆向きだが、渡す値がboolを2つだけなのでロックを増やす
        // 価値がなく、atomicで足りる。Updateスレッドはこれを見てWASD移動と視点回転の開始を抑止する
        std::atomic<bool> m_ImGuiWantCaptureKeyboard{ false };
        std::atomic<bool> m_ImGuiWantCaptureMouse{ false };

        // GPUタイムスタンプクエリによる各パスの計測(Shadow/GBufferなど)。数フレーム遅れの結果が返る
        std::unique_ptr<RHI::IRHIGPUProfiler> m_GPUProfiler;
        // 各パスのコマンド記録にかかるCPU時間の計測(RHIに依存しないためDX11/DX12を直接比較できる)
        Core::CPUProfiler m_CPUProfiler;

        // G-Bufferの内部解像度。ウィンドウサイズとは独立しており、表示時はアスペクト比を保って拡大縮小する。
        // Render()の各所(Dispatchのスレッド数・定数バッファのScreenParams・TAAジッター・
        // Hi-Zのミップ・レターボックス)から読まれるため、フレームの途中で変えてはならない。
        // 変更はm_RenderResolutionDirty経由でフレーム先頭にまとめて反映する
        uint32_t m_RenderWidth;
        uint32_t m_RenderHeight;
        // 「システム」パネルから要求された新しい内部解像度。Render()の先頭で反映する。
        // m_BufferPrecisionDirtyとまったく同じ扱い(レンダーターゲットの作り直しはGPUがそれらを
        // 参照していない状態で行う必要があるため、UI関数の中では実行しない)
        uint32_t m_PendingRenderWidth = 0;
        uint32_t m_PendingRenderHeight = 0;
        bool m_RenderResolutionDirty = false;
        // 内部解像度から決まるカメラのアスペクト比。
        // m_CameraはUpdateスレッドしか書けない(Render()はFrameStateのスナップショット経由でしか
        // 読まない)ため、解像度を変えるRenderスレッドからはここへ置くだけにし、
        // Updateスレッドが毎フレーム読み取ってm_Camera.SetAspectRatio()を呼ぶ
        std::atomic<float> m_RenderAspect{ 1.0f };

        // 出力解像度用テクスチャの作り直し要求。m_RenderResolutionDirtyとまったく同じ扱いで、
        // Render()の先頭のWaitForGPUIdle()を挟んだ位置で処理する
        bool m_UpscaleTargetsDirty = false;

        // 品質モードの倍率(1.3 / 1.5 / 1.7 / 2.0)
        static float GetUpscaleRatio(UpscaleQualityMode mode);
        // 出力解像度と品質モードから内部レンダー解像度を求める。
        // 8の倍数へ切り捨てるのは、LightCullのタイル・Hi-Zのミップ連鎖・Bloomのピラミッド・
        // SkyCloud/DDGIResolveの1/2解像度がいずれも2の冪で割っていくため。下限は320x180
        static void ComputeUpscaleRenderResolution(
            uint32_t outputWidth, uint32_t outputHeight, UpscaleQualityMode mode,
            uint32_t& outRenderWidth, uint32_t& outRenderHeight);

        // 出力解像度のテクスチャを作り直す。GPUがそれらを参照していない状態で呼ぶこと
        void CreateUpscaleTargets(uint32_t width, uint32_t height);
        // このフレームで超解像パスを走らせるか(有効かつテクスチャが確保済み)
        bool IsUpscaleActive() const;

        // ImGuiでBufferPrecisionが変更されたことをRender()へ伝えるフラグ。レンダーターゲットの
        // 作り直しはGPUがそれらを参照していない状態で行う必要があるため、UI関数の中では実行せず
        // Render()の先頭(RenderGraphの構築より前)でm_Device->WaitForGPUIdle()を挟んで処理する
        bool m_BufferPrecisionDirty = false;

        // G-Buffer・水面・深度プリパス・メッシュレット経路・Hi-Zのシェーダーと
        // PSOは Passes/GeometryPasses へ移した

        // プリパスを走らせるか・メッシュ単位のフラスタムカリングを行うかは
        // m_Settings.Geometry.DepthPrepassEnabled / MeshCullingEnabledへ移した

        // --- インスタンシング(Stage 7) ------------------------------------------------------
        //
        // 同じ .kmodel を指すインスタンスを1回の DrawIndexed(..., instanceCount) へまとめる。
        // インスタンスごとに違う World/NormalMatrix/TangentSignFlip は定数バッファでは渡せないので、
        // 頂点シェーダー専用SRV(t0)の StructuredBuffer を SV_InstanceID で引く
        // (Shaders/3D/ObjectConstants.hlsli の FetchModelInstance)。
        //
        // 【効くシーンは限られる】PLATEAU・Sponza・Bistro は全モデルがユニークなので
        // バッチが1つも作られない。効くのは同じモデルを並べたシーン(InstancingTest /
        // MultiModelTest)と、今後の繰り返し配置(植生・街灯)。
        //
        // 【メッシュシェーダー経路には効かない】DispatchMesh にインスタンス数の概念が無いため、
        // ShouldUseModelMeshletPath が真になるモデルはバッチに入れない。
        // つまり DX12 でメッシュレット描画が有効なあいだ、この機能が働くのは
        // 水面・メッシュレットを持たないモデル・メッシュレット描画を切ったときに限られる
        struct InstanceBatch
        {
            // このバッチが描く段。同じ段を選んだインスタンスだけをまとめる
            const Assets::Model* Model = nullptr;
            // m_SceneGPUResources.ModelInstanceBuffer の中の先頭位置。頂点シェーダーは
            // ModelInstances[InstanceBase + SV_InstanceID] を読む
            uint32_t InstanceBase = 0;
            uint32_t InstanceCount = 0;
            // ワインディングと水面の別はパイプラインステートで分かれるため、
            // 違うものを1つのドローへまとめてはいけない(まとめると片方が裏面として全部捨てられる)
            bool IsMirrored = false;
            bool IsWater = false;
            // 構成インスタンスのワールドAABBの包絡。パスごとのフラスタム判定に使う
            float WorldBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
            float WorldBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
            // 代表インスタンスのシーン内番号(バッチの先頭)。IsMirrored/IsWaterはバッチ内で
            // 同一なので、定数バッファを作るのに1体を代表として使える
            size_t RepresentativeIndex = 0;
        };

        // バッチの一覧は「どの段を描くパスか」で2組に分かれる。
        // 変換そのものはどちらでも同じだが、**まとめられる相手が違う** ――
        // G-Buffer は各インスタンスがそのフレームに選んだ段、シャドウとプローブは常に
        // 最も粗い段(GetCoarsestLOD)を描くため、同じ組では括れない
        std::vector<InstanceBatch> m_InstanceBatchesCurrentLOD;   // 深度プリパス / G-Buffer / 平面反射
        std::vector<InstanceBatch> m_InstanceBatchesCoarsestLOD;  // シャドウ / 反射プローブ
        // インスタンスがどちらの組でバッチに入ったか。パスの個別ループはここが立っているものを飛ばす
        std::vector<uint8_t> m_InstanceBatchedCurrentLOD;
        std::vector<uint8_t> m_InstanceBatchedCoarsestLOD;
        // アップロード用の作業領域(毎フレーム作り直す。確保のやり直しを避けるため持っておく)
        std::vector<GPUModelInstance> m_ModelInstanceRecords;
        // 1バッチの上限。上限が無いと「街灯を市街全域に5000個」のようなグループが
        // 1つの巨大AABBになり、どのパスからも一度も間引かれなくなる。
        // グループ内を空間セルでソートしてから刻むので、バッチは局所的にまとまる
        static constexpr uint32_t kMaxInstancesPerBatch = 128;
        // バッチを組み直す(レンダーグラフの構築より前に1フレーム1回。UpdateModelLODの後)
        void BuildInstanceBatches(RHI::IRHICommandList* commandList);

        // 出所は Rendering/GeometryDrawTypes.h(移行中の別名)
        // このフレームの描画単位を組み立てる。coarsestLOD が真ならシャドウ/プローブ用の組、
        // 偽なら深度プリパス/G-Buffer/平面反射用の組を使う。
        // シーンの全インスタンスがちょうど1回ずつ現れる(バッチに入ったものはバッチとして)
        void GetInstanceDrawUnits(bool coarsestLOD, std::vector<Rendering::InstanceDrawUnit>& outUnits) const;
        // インスタンシングのバッチを使わないパス(DDGI / 半透明 / ソフトウェアラスタライザ)向けに、
        // シーンの全インスタンスを単体の描画単位として詰める。列挙順はm_Scene.Instancesの並びのまま
        void BuildSingleInstanceDrawUnits(std::vector<Rendering::InstanceDrawUnit>& outUnits) const;

        // --- ジオメトリ描画ループの共通化(Rendering/GeometryDrawLoop.h) --------------------
        //
        // どのパスも「インスタンスの列挙 → 錐台カリング → 段の選択 → メッシュのループ」までは
        // 同じで、違うのはPSOの選び方・定数バッファ・張るテクスチャ・ドローの発行だけ。
        // 前半をForEachGeometryDrawへ寄せ、後半をコールバックとして呼び出し側に残す

        // 出所は Rendering/GeometryDrawTypes.h(移行中の別名)

    public:
        // onModel: モデル単位で描き切ったなら真を返す(メッシュのループへ入らない)
        // onMesh : 偽を返すと列挙そのものを打ち切る
        //
        // 【publicにしてある】Passes/*の各群がこれを呼ぶ。状態(下のm_DrawUnitScratch)は
        // エンジンが持ったままなので、群がスクラッチを持つことにはならない
        template <typename ModelFn, typename MeshFn>
        void ForEachGeometryDraw(const Rendering::GeometryDrawLoopDesc& desc, ModelFn&& onModel, MeshFn&& onMesh);

    private:
        // 上の出力先。パスは順に実行されるので1本を使い回してよい(確保のやり直しを避ける)。
        // **パスのラムダより長生きする必要がある**ため、ローカル変数ではなくここに置く
        mutable std::vector<Rendering::InstanceDrawUnit> m_DrawUnitScratch;
        // 上が1本しかないことを守るための旗。入れ子で列挙すると内側が外側の列挙対象を
        // 書き換えてしまう。検査の中身はRendering/GeometryDrawLoop.hにある
        mutable bool m_DrawUnitScratchInUse = false;
        // 統計。**フラスタムカリングとは別建てにする** ―― 「バッチが0のまま」は
        // 「まとめられる相手がいない」のか「一度も実行されていない」のかを区別できないため、
        // まとめた数と減らせたドロー数の両方を出す
        uint32_t m_InstancedBatchCount = 0;
        uint32_t m_InstancedInstanceCount = 0;
        uint64_t m_FrameStatsInstancedBatchSum = 0;
        uint64_t m_FrameStatsInstancedInstanceSum = 0;

        // 起動時に決まる能力値(メッシュシェーダー・レイトレーシング等)。詳細は
        // Diagnostics/RenderCapabilities.h
        RenderCapabilities m_RenderCapabilities;
        // フレームごとの統計値(ImGuiのプロファイラパネル・性能ログ表示用)。詳細は
        // Diagnostics/RenderStats.h
        RenderStats m_RenderStats;
        // 毎フレーム主カメラから作り直し、全パスの定数バッファへ同じものを配る
        MeshletLODFrameConstants m_MeshletLODFrame;
        // 増幅シェーダーが数え上げる先。uint×3 = [判定, 視錐台+コーンで間引き, オクルージョンで間引き]
        // 出所は Passes/GeometryConstants.h(移行中の別名)
        // カウンタバッファ本体は Passes::GeometryPasses が持つ(数えるのが増幅シェーダーのため)。

        // カウンタをCPUへ持ってくる受け皿(メッシュレット統計とモデルカリングの両方)。
        // リングの段数と添字の扱いは Diagnostics/CullStatsReadback.h にある
        Diagnostics::CullStatsReadback m_CullStats;

        // --- モデル単位のGPUカリング(Stage 5-3) ---
        //
        // コンピュートシェーダー(ModelCull.hlsl)が、描画候補のワールドAABBを
        // 視錐台とHi-Zで判定し、生き残ったものだけの ExecuteIndirect 引数を詰める。
        // 深度プリパスとG-Bufferは、その引数でまとめて描く。
        //
        // 【区画の分け方と、その理由】Passes/GeometryConstants.h が持つ。
        // ここに並ぶ7本は移行中の別名で、あちらの値を引くだけ。
        // 出所は Passes/GeometryConstants.h(移行中の別名)
        static_assert(
            Passes::kModelCullArgsBaseOffset >= sizeof(uint32_t) * Passes::kModelCullRegionCount,
            "区画ごとの発行数が引数配列の領域へはみ出している");

        // GpuModelCullInstance は Passes/GeometryConstants.h へ移した

        // GPUカリングの資源一式は Passes::GeometryPasses が持つ。読み戻しと、
        // そこから作るログ用の値は上の m_CullStats が持つ

        // G-Bufferは複数のパス群が共有するため、特定のパス群ではなく唯一の所有者へ集める。
        Rendering::RenderTargets m_RenderTargets;
        // 間接光(DDGI・反射プローブ)のリソースの持ち主は Rendering/GIResources.h
        Rendering::GIResources m_GIResources;

        // 直接光パスのシェーダーとPSOはPasses/LightingPassesへ移した

        std::unique_ptr<RHI::IRHITexture> m_AODisabledTexture; // AO無効時に使う、遮蔽なし・間接光なしのテクスチャ

        // AO/GI(共通ブラー・SSAO・SSIL)のシェーダー・PSO・定数バッファ・SSAOカーネルは
        // Passes/LightingPassesへ移した



        // RTAOのシェーダー・PSO・定数バッファはPasses/LightingPassesへ移した。
        // 出力2枚はレンダー解像度に追従して作り直すためRenderTargets(RTAORawTexture / RTAOTexture)にある

        // ライティングパスのシェーダー・PSO・定数バッファはPasses/LightingPassesへ移した

        // 半透明フォワードパスのシェーダーとPSO2本はPasses/LightingPassesへ移した

        // --- ドローンショー(発光点の描画) ---------------------------------------------
        // 夜空を編隊飛行する多数のドローンを、1機につきカメラ正対のビルボード1枚として
        // 加算合成で描く。編隊の生成と時間補間はDroneShow.h/.cppが持ち、ここは描画だけを担う。
        //
        // 頂点バッファを持たず、Draw(6 * 機体数, 0)とSV_VertexIDでクアッドを展開する
        // (理由はShaders/3D/DroneShow.hlsl冒頭)。機体データはm_DroneShowResources.Bufferから
        // 頂点シェーダーが直接読む(SetVertexShaderResourceBuffer)。
        //
        // 【PSOは1本でよい】平面反射(鏡映カメラ)でもこれをそのまま使う。メッシュ描画のように
        // ワインディングを反転したPSOを別に持つ必要は無い ―― 理由はPSO生成箇所のコメント
        std::unique_ptr<RHI::IRHIShader> m_DroneShowVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_DroneShowPixelShader;
        // PSO・定数バッファ・機体データは本描画と平面反射の2群が同じものを使うため、
        // 持ち主を Rendering/DroneShowResources.h へ移した
        Rendering::DroneShowResources m_DroneShowResources;
        // 1フレームぶんの機体の状態。毎フレームDroneShow::Evaluateが書き、
        // グラフ構築前に1回だけm_DroneShowResources.BufferへUpdateBufferする
        // (m_SceneGPUResources.LightBufferと同じ理由: 本描画と平面反射の2パスから読まれるため、
        //  パスの中で更新すると先に走る側が未更新の内容を読む)
        std::vector<GPUDrone> m_DroneInstances;
        // 機体を光源として送るときの、間引いた灯。毎フレームDroneShow::BuildLightSamplesが書き、
        // gpuLightsの組み立てで手置きライト・エミッシブプロキシの後ろへ連結する
        std::vector<DroneLightSample> m_DroneLightSamples;
        // 再生器。編隊の点そのものはここが持つ(.kshowから読み込む)
        DroneShow m_DroneShow;

        // ショーの進行時刻[秒]。RenderThreadMainがm_CloudScrollOffsetと同じ場所で進める
        float m_DroneShowTime = 0.0f;

        // --- .ksceneが持つパラメータ ---
        //
        // 【ショーの中身に属する値はここに無い】機体数・保持/変形秒・明るさ・ビルボード半径・
        // 揺れ・再生速度・種はすべて.kshowが持つ(m_DroneShow.Data()から読む)。
        // シーンが決めてよいのは「出すかどうか」と「どこにどの大きさで置くか」だけで、
        // 同じショーを別のシーンへ置けるのはこの分担があるため
        bool m_DroneShowEnabled = Defaults::DroneShowEnabled;
        DirectX::XMFLOAT3 m_DroneShowCenter{
            Defaults::DroneShowCenterX, Defaults::DroneShowCenterY, Defaults::DroneShowCenterZ };
        float m_DroneShowScale = Defaults::DroneShowScale;
        // 遠方の機体が1画素を割ってTAAのジッターでちらつくのを防ぐ、画面上の最小半径(NDC単位)。
        // 【これだけはシーンにもショーにも持たせない】ショーの表現ではなく描画側の下限で、
        // 「1画素を割ったらちらつく」という事実はどのシーン・どのショーでも変わらないため
        float m_DroneShowMinScreenRadius = Defaults::DroneShowMinScreenRadius;
        // 機体を光源としても送るか。シーンが決める(「出すか」の一種)
        bool m_DroneShowCastLight = Defaults::DroneShowCastLight;
        // 灯の明るさの倍率。1.0がスプライトから導いた物理的な値で、演出用にシーンが上げられる
        float m_DroneShowCastLightScale = Defaults::DroneShowCastLightScale;
        // 光源として送る灯の数と、Rangeを逆算する打ち切り照度[lx]。
        // 【これらもシーンにもショーにも持たせない】MinScreenRadiusと同じで、
        // タイルライトカリングの容量という描画側の事情で決まる値だから
        int m_DroneShowLightSampleCount = Defaults::DroneShowLightSampleCount;
        float m_DroneShowLightCutoffLux = Defaults::DroneShowLightCutoffLux;
        // 実際に送った灯の数。ログとUIの表示用
        uint32_t m_DroneShowLightUsedCount = 0;
        // 容量超過の警告と実効値ログを、それぞれ1回だけ出すためのフラグ
        bool m_DroneShowLightTileOverflowLogged = false;
        bool m_DroneShowLightValuesLogged = false;

        // Hi-Zのミップ段数と「1回でも構築されたか」は Passes::GeometryPasses が持つ
        // (構築するのがHi-Zパス自身のため)。デバッグ表示で確認するミップレベルは
        // m_Settings.DebugView.HiZDebugMipLevelへ移した

        // UIの「既定値に戻す」(右クリック)が戻る先。シーン読み込み時に決まった手法を控えておく。
        // 【静的なDefaultReflectionModeを使ってはいけない】.ksceneが指定を持つ場合、
        // 戻る先はエンジンの既定ではなく**そのシーンを読み込んだ直後の状態**である。
        // ここを取り違えると「既定へ戻したらシーンが要求した反射が消える」ことになる
        ReflectionMode m_SceneDefaultReflectionMode = ReflectionSettings::DefaultReflectionMode(false);

        // SSRとRT反射のシェーダー・PSO・定数バッファはPasses/ReflectionPassesへ移した。
        // RT反射の出力テクスチャだけは、レンダー解像度に追従して作り直すものなので
        // 持ち主をRenderTargets(RTReflectionTexture)にしてある

        // MegaLightsのシェーダー・PSO・定数バッファは Passes/MegaLightsPasses へ移した。
        // 生出力と候補プール本体は直接光・Presentも読むため RenderTargets が持つ

        // 出所は Passes/MegaLightsConstants.h(移行中の別名)
        // 履歴・デノイザの作業バッファは RenderTargets、その添字と有効性は
        // Passes/MegaLightsPasses が持つ

        // 前フレームの実効プリ露出EV100。
        // 【補正には使っていない】リザーバのWは露出に対して不変(比なので約分される)と
        // 実測で確かめた ―― TAAのm_TAAPrevEffectiveExposureEV100と違い、掛ける係数は1。
        // 詳細はKurenaiEngine3D.cppの「プリ露出の補正は入れない」。
        // 値は、将来この前提を疑うときに差を見られるよう記録だけ続けている
        float m_MegaLightsPrevEffectiveExposureEV100 = 0.0f;
        // 摂動を適用済みか(蓄積開始の1回だけ効かせる)
        bool m_MegaLightsPerturbApplied = false;

        // いまリザーババッファを確保したときの標本数。**定数バッファへ渡す値と必ず一致させる**。
        // 食い違うと Initial が確保外へ書くか Resolve が別画素の標本を読み、
        // 例外もログも出ないまま絵だけが壊れる
        int32_t m_MegaLightsAllocatedSamplesPerPixel = 1;
        // 標本数が変わったのでリザーババッファを作り直す必要がある。
        // 解像度変更と同じくフレームの先頭(GPUアイドル後)でまとめて処理する
        bool m_MegaLightsReservoirDirty = false;

        // --- 蓄積平均(計測専用) ---
        // MegaLightsの出力を線形空間でフレーム方向へ足し込み、フレーム数で割った平均を表示する。
        //
        // 【なぜ要るのか】確率的サンプリングの正しさは「平均が真値に一致するか」で決まるが、
        // 画面キャプチャで得られるのはトーンマップ後の8bitで、トーンマップは凹関数のため
        // **偏りがゼロでもノイズがあるだけで平均が低く出る**。スクリーンショットをN枚平均しても
        // 検証にならないので、線形空間で足す場所をエンジン側に持つ
        // 蓄積バッファは RenderTargets::MegaLightsAccumBuffer、進行状態は Passes/MegaLightsPasses が持つ
        // 何フレーム待ってから足し始めるか。小さなシーンの読み込みとリサイズが片付く目安
        // 出所は Passes/MegaLightsConstants.h(移行中の別名)

        // --- GPU計測の書き出し(計測専用) ---
        std::wstring m_PerfDumpPath;
        int32_t m_PerfDumpTargetFrames = 0;
        int32_t m_PerfDumpWarmupFrames = 0;
        int32_t m_PerfDumpCollected = 0;
        bool m_PerfDumpDone = false;
        // パス名 -> 合計時間[ms]。同じ名前のパスが1フレームに複数あるぶんも足し込む
        // (a-trousは段の数だけ同名で登録される。**合計が知りたいので足すのが正しい**)
        std::map<std::string, double> m_PerfDumpTotals;
        // このフレームのパス別GPU時間を上の合計へ足し込み、目標枚数に達したらCSVへ書き出す。
        // 【定義はDiagnostics/RenderDumpService.cppにある】LogFrameStatsIfDueと同じ翻訳単位
        void AccumulatePerfDump();

        // --- 作り直し経路の予約(検証専用。AddScheduledRecreation) ---
        // 発火済みのものはFiredを立てて二度と撃たない。フレームが飛んでも取りこぼさないよう、
        // 「>= Frameの最初のフレーム」で撃つ。
        struct ScheduledRecreationSlot
        {
            ScheduledRecreation Request;
            bool Fired = false;
        };
        std::vector<ScheduledRecreationSlot> m_ScheduledRecreations;

        // --- 中間レンダーターゲットの生値ダンプ(検証専用。AddTextureDump) ---
        // 名前 -> テクスチャ の対応表。CreateRenderTargetsでテクスチャを増やしたら
        // BuildDumpableTextureTableにも足すこと(表の実体はそちらのコメントを参照)
        struct DumpableTexture
        {
            const char* Name = nullptr;
            RHI::IRHITexture* Texture = nullptr;
        };
        // 【毎回作り直す】レンダーターゲットはリサイズやバッファ精度の切り替えで
        // ポインタごと作り直される。キャッシュすると解放済みのテクスチャを指す
        std::vector<DumpableTexture> BuildDumpableTextureTable() const;

        // 上の表の名前を、グラフィックスデバッガ向けの名前としてテクスチャへ焼く。
        //
        // 【-dumptex と同じ表を使うのが要点】RenderDocのキャプチャ上の名前と
        // `-dumptex <名前>` の名前が同じ文字列になるので、2つの経路で同じものを見ていることが
        // 名前だけで分かる。表を1つにしておけば、テクスチャを増やしたときの追従先も1箇所で済む
        void ApplyDebugNames() const;
        // 名前を焼き直す必要があるか。レンダーターゲットを作り直すと立てる。
        // **毎フレーム焼かない** —— 43本のSetNameを60回/秒で呼ぶ意味がない
        bool m_DebugNamesDirty = true;

    public:
        // 旗が立っていれば上を呼んで下ろす。
        //
        // 【publicにしてある】呼ぶのはPasses::PresentPassだけだが、名前を焼く対象は
        // エンジン全体のテクスチャ表(BuildDumpableTextureTable)なので、この機能を
        // Present群へ降ろすことはできない。**旗の判定と下ろしをここへ閉じておく**と、
        // 群がエンジンのメンバ変数を触らずに済む
        void ApplyDebugNamesIfDirty();

    private:

        // 連番ダンプの受け皿1枚ぶん。
        //
        // 【なぜ1枚では足りないのか】コピーを積んでから読めるようになるまで
        // kTextureDumpReadDelayFrames ぶん空ける必要がある。受け皿が1枚しか無いと、
        // 毎フレーム積んだときに**まだ読んでいない中身へ次のコピーを上書きしてしまう**。
        // エラーにはならず、静かに同じ絵が並ぶ or 途中のフレームが消えるという形で出る
        struct TextureDumpSlot
        {
            // 受け皿。m_DeviceはKurenaiEngineBase(基底)のメンバで、派生クラスのメンバは
            // 基底より先に破棄されるため、デバイスより後に解放される心配は無い
            // (MegaLightsの読み戻しも同じ理由で Passes::MegaLightsPasses のメンバに置いてある)
            std::unique_ptr<RHI::IRHITexture> Readback;
            // 【寸法は積むときに控える】あとで引き直すと、その間のリサイズで
            // 受け皿の中身と食い違う値をヘッダへ書いてしまう
            RHI::TextureReadbackDesc Desc{};
            // コピーを積んだフレーム番号。GPUの実行はCPUより数フレーム遅れるので、
            // 積んだ直後に読んではいけない(IRHICommandList::CopyTextureToReadback のコメント)。
            // **ファイルのFrameIndex欄にもこの値を書く** —— 画素の中身が属するのはこのフレーム
            uint32_t CopyFrame = 0;
            // 連番の何枚目か。ファイル名の _%04u になる
            uint32_t SequenceIndex = 0;
            // 読み戻しに失敗し続けたフレーム数。**無人実行が静かに固まるのを防ぐための打ち切り用**
            uint32_t FailedFrames = 0;
            // 積んであり、まだ回収していない
            bool Busy = false;
        };

        struct TextureDumpRequest
        {
            std::string Name; // 表の名前(ファイルのヘッダにも書く)
            std::wstring Path;
            uint32_t MipLevel = 0;
            uint32_t ArraySlice = 0;
            // 何枚撮るか。**既定1のときは受け皿も深さ1**なので、連番を使わない従来の
            // 呼び出しはメモリ使用量も発行のタイミングも1ミリも変わらない
            uint32_t TargetFrames = 1;
            // 何フレームおきに撮るか。1なら連続フレーム。
            // 【間隔を記録できることに意味がある】画面キャプチャの連写は撮影間隔が
            // 撮る側の都合で揺れ、同じ構成の2回で時間統計が3〜4倍動いた(61.7i)。
            // ここでは間隔が指定値として決まり、ファイルのFrameIndexから検算もできる
            uint32_t Stride = 1;
            // 実際に確保した受け皿の枚数。解像度が大きいと上限で削られるので、
            // kTextureDumpRingDepth とは一致しないことがある
            uint32_t RingDepth = 0;
            // 連番に異なる寸法の画像を混在させないため、最初の読み戻し形式を固定する。
            RHI::TextureReadbackDesc FirstDesc{};
            std::vector<TextureDumpSlot> Slots;
            // 積んだ枚数と、実際にファイルへ書けた枚数。
            // 【2つ分けて数える】これまでは「諦めた」も完了として扱われ、1枚も書けなくても
            // -exitafterdump が正常終了していた。書けた数を別に持って報告する
            uint32_t IssuedCount = 0;
            uint32_t WrittenCount = 0;
            // 直近で積んだフレーム(Strideの間引き用)と、最初に積んだフレーム(打ち切りの起点)
            uint32_t LastIssueFrame = 0;
            uint32_t FirstIssueFrame = 0;
            bool AnyIssued = false;
            bool Done = false;
        };
        std::vector<TextureDumpRequest> m_TextureDumps;
        // 何フレーム目で撮るか。負なら Passes::kMegaLightsAccumWarmup を使う
        // (新しい定数を作らないのは、あちらのコメントに書かれた「整定を待つ理由」が
        //  そのまま当てはまり、値が2つに割れると片方だけ直す事故が起きるため)
        int32_t m_TextureDumpFrame = -1;
        std::wstring m_PassManifestPath;
        uint32_t m_PassManifestTargetFrames = 1;
        uint32_t m_PassManifestIssuedFrames = 0;
        bool m_PassManifestIssued = false;
        bool m_ExitAfterDump = false;
        bool m_ExitAfterDumpRequested = false;
        // 読み戻しを何フレーム失敗し続けたら諦めるか。DX11のMap(DO_NOT_WAIT)は
        // GPUが詰まっていると何度も失敗しうるので、1フレームで諦めてはいけない
        static constexpr uint32_t kTextureDumpMaxFailedFrames = 60;
        // コピーを積んでから読むまでに空けるフレーム数(MegaLightsのダンプと同じ値)
        static constexpr uint32_t kTextureDumpReadDelayFrames = 5;
        // 遅延中のコピーを連続発行できる深さ。これ以上は回収より先に増えてメモリだけを使う。
        static constexpr uint32_t kTextureDumpRingDepth = kTextureDumpReadDelayFrames + 1;
        // 高解像度バッファの連番が無制限にメモリを消費しないための上限。
        static constexpr size_t kTextureDumpRingMaxBytes = 512ull * 1024 * 1024;

        // ダンプの発行(コピーを積む)と、読み戻し・ファイル書き出し。Render()から呼ぶ
        void ApplyScheduledRecreations();

    public:
        // 【publicにしてある】積む位置がPresentより前と決まっているためPasses::PresentPassが
        // 呼ぶ。書き出す対象はエンジン全体のテクスチャ表なので、群へは降ろせない
        void IssueTextureDumps(Core::RenderGraph& graph);

    private:
        void ResolveTextureDumps();
        // このフレームのパスマニフェスト(RenderGraphの実行順)を、指定のフレームに達していれば
        // ファイルへ書き出す(検証専用の -passmanifest)。graph.Execute()の直前に呼ぶこと
        void WritePassManifestIfDue(Core::RenderGraph& graph);
        // 1件ぶんをファイルへ書く。書けたらtrue
        bool WriteTextureDumpFile(
            const TextureDumpRequest& request, const TextureDumpSlot& slot, const std::vector<uint8_t>& pixels) const;

        // --- 雲(低解像度の専用パス) ---
        // Lightingパスの直前に置くフルスクリーン三角形+ピクセルシェーダー。積雲と巻雲だけを
        // 内部レンダー解像度の1/2(面積で1/4)で評価し、「透過率 + 事前乗算済みの散乱光」を書く。
        // Lightingパスの背景分岐がこれをバイリニアで引いて
        // SkyColorWithoutClouds(rayDir) * a + rgb を合成する。
        // 分離の根拠と、太陽・星がフル解像度のまま保たれる理由はShaders/3D/SkyCloud.hlsl冒頭を参照
        // シェーダーとPSOはPasses/LightingPassesへ移した。書き先2枚と実寸は
        // レンダー解像度に追従して作り直すためRenderTargets(SkyCloud*)にある

        // DDGIの低解像度解決パスの資源と実寸は Passes/DDGIPasses と Rendering/GIResources.h へ移した

        // --- 大気遠近(height fog / aerial perspective) ---
        // 反射パス(SSR/RT反射)の後、TAAパスの直前に置くフルスクリーン三角形+ピクセルシェーダー。
        // Lightingパスの中へ入れない理由・TAAより前へ置く理由はShaders/3D/AerialPerspective.hlsl
        // 冒頭のコメント参照。無効時(m_Settings.Fog.Enabled=falseまたはm_Settings.Fog.Density<=0)はパス自体を
        // 登録せず、GetActiveReflectionOutput()の結果がそのままTAA(またはTonemap)へ渡る
        // シェーダーとPSOはPasses/PostProcessPassesへ移した。書き先は
        // レンダー解像度に追従するためRenderTargets(AerialPerspectiveTexture)にある

        // TAA(Temporal Anti-Aliasing)パス: SSRの後、露出/ブルーム/トーンマップの前に置く。
        // 毎フレーム投影行列を1ピクセル未満だけずらして(ジッター)サンプル位置を散らし、
        // モーションベクターで前フレームの結果を今フレームの画素へ再投影して蓄積する。
        // 静止していれば十数フレームで収束し、実質的なスーパーサンプリングになる。
        // 詳細な原理と各工夫の理由はArchitecture.htmlのTAAの章を参照
        // シェーダーとPSOと定数バッファはPasses/PostProcessPassesへ移した
        // 履歴バッファ2枚。読みながら同じテクスチャへ書けないため役割を毎フレーム入れ替える。
        // m_TAAHistoryIndexが今フレームの書き込み先で、もう一方が前フレームの結果(=履歴)。
        // このパスの出力がそのまま後段(自動露出/ブルーム/トーンマップ)の入力にもなる
        uint32_t m_TAAHistoryIndex = 0;
        // 履歴の内容が信用できるか。falseの間、TAAは履歴を「サンプルすらせず」今フレームの色を返す。
        // ブレンド率を0にするだけでは不十分で、未初期化fp16のNaNはlerp(NaN, x, 1.0)でもNaNのまま
        // 伝播し、一度混入すると履歴に固着し続ける。
        // 落とすのは (1)履歴バッファ作成直後(初回・バッファ精度変更) (2)シーン切り替え
        // (3)TAAのON/OFFトグル。(2)はUpdateスレッドのLoadSceneから書くためatomicにする
        std::atomic<bool> m_TAAHistoryValid{ false };
        // ジッターのサンプル列を進めるフレーム番号(Halton列の添字に使う)
        uint32_t m_TAAFrameIndex = 0;
        // 前フレームのビュー射影行列(ジッター済み・転置済み=シェーダへ渡す形のまま)。
        // Renderスレッドのみが読み書きするため追加の排他は不要。
        // 履歴テクスチャの有効性(m_TAAHistoryValid)とは意図的に別管理にしている。シーン切り替えや
        // バッファ精度変更では履歴の中身は捨てるが、カメラ行列そのものは前フレームのものが正しく
        // 残っているため、速度バッファまで0に潰す必要がない
        DirectX::XMFLOAT4X4 m_TAAPrevViewProj{};
        // m_TAAPrevViewProj / m_TAAPrevJitterUv に実際の前フレームの値が入っているか。
        // 初回のRender()でのみfalseで、以降はずっとtrue
        bool m_TAAPrevViewProjValid = false;
        // 前フレームのジッター量(UV単位)。速度からジッター差分を取り除くのに使う
        DirectX::XMFLOAT2 m_TAAPrevJitterUv{ 0.0f, 0.0f };
        // 前フレームのカメラ位置(ワールド)。有効性は m_TAAPrevViewProjValid と同じ
        // (同じ場所で同じタイミングに書くため)。
        //
        // 【何に使うか】Hi-Zオクルージョンカリングが判定に使うHi-Zは1フレーム古く、
        // シーンが静的である以上ずれの原因はカメラの移動だけ。移動距離をバウンディング球の
        // 半径へ足せば、そのずれを1次の範囲で保守側へ吸収できる(FrameConstants::OcclusionCullParams.z)
        DirectX::XMFLOAT3 m_PrevCameraPosition{ 0.0f, 0.0f, 0.0f };
        // 前フレームの実効プリ露出EV100。このエンジンはSceneColorへプリ露出を掛け込んでおり、
        // その値が時間順応で毎フレーム変わる(m_EffectiveExposureEV100)。補正しないと
        // 露出が動いている間ずっと履歴が古い明るさを引きずり、明るさの尾を引く
        float m_TAAPrevEffectiveExposureEV100 = 0.0f;

        // Tonemapパス: SceneColor(SSR有効時はRenderTargets::SSRTexture)のHDR値をReinhardトーンマッピング+
        // ガンマ補正でLDRへ変換し、Presentパスへ渡す。SSR等のHDR演算より後、Present直前の
        // 独立したステージとして置くことで、反射や将来のブルーム/露出制御(M7)がトーンマップの
        // 影響を受けないHDR値の上に成立できるようにする
        // シェーダーとPSOと定数バッファはPasses/PostProcessPassesへ移した

        // 超解像パス(Upscale.hlsl): Tonemapが出したLDR画像を、EASUで出力解像度へ再構成し、
        // RCASでシャープ化してからPresentへ渡す。出力2枚と実寸(RenderTargets::UpscaleTexture /
        // UpscaleSharpTexture / UpscaleTargetWidth / Height)はPresentPassも読むため
        // 持ち主をRenderTargetsへ移した。作り直しはCreateRenderTargets()とは別の契機で走る
        // シェーダーとPSO2本と定数バッファはPasses/PostProcessPassesへ移した


        // 自動露出(eye adaptation)パス: SceneColorの輝度ヒストグラムをGPUで作り、
        // 低/高パーセンタイルを除外した加重平均から目標EV100を求めて時間方向に追従させる。
        // 結果はRenderTargets::ExposureTextureへ書かれ、Tonemapパスが読んで露出倍率に変換する。
        //
        // 露出そのものはCPU側でライト強度へ事前乗算されている(プリ露出方式、
        // m_Settings.PostProcess.SceneExposureEV100)。自動露出の結果をライト強度へ戻すとフィードバックループになり、
        // かつGPU→CPUのリードバック(同期待ち)が要るため、プリ露出は固定のままにして
        // 「プリ露出EVと自動露出EVの差」だけをTonemapで掛ける構成にしている
        // (詳細はAutoExposure.hlsl冒頭)
        // シェーダー3本・PSO3本・ヒストグラムバッファ・定数バッファ・順応リセットの要求は
        // Passes/PostProcessPassesへ移した(ビン数の定数はPasses/PostProcessConstants.hへ)。
        // 露出の保存先はRenderTargets(ExposureTexture)にある

        // ブルームパス(Bloom.hlsl): 半解像度から始まるピラミッドを段階的にダウンサンプルし、
        // 3x3テントで戻しながら加算することで広く滑らかな光の裾を作る。
        //
        // ピラミッドをミップチェーン1枚ではなくレベルごとの独立テクスチャで持っているのは、
        // 同一リソースのSRV/UAV同時バインドを避けるため(理由の詳細はBloom.hlsl冒頭)。
        // ピラミッド本体(RenderTargets::BloomDownTextures / BloomUpTextures / BloomLevelSizes)は
        // PresentPassのデバッグ表示も読むため、持ち主をRenderTargetsへ移した
        // シェーダーとPSO2本と定数バッファはPasses/PostProcessPassesへ移した
        // ピラミッドの段数。半解像度を第0段として、これ以上小さくしても見た目が変わらない範囲で選ぶ
        static constexpr uint32_t kBloomLevelCount = 6;


        // 垂直同期・固定FPSモードはm_Settings.Systemへ移した

        // Presentパスのシェーダー・PSO・定数バッファはPasses/PresentPassへ移した

        // デバッグ表示用: Presentパスで最終的に表示するレンダーターゲットの種類(DebugView enum)と
        // その表示パラメータはm_Settings.DebugViewへ移した(Settings/DebugViewSettings.h)。
        // enumとkDebugViewCountも同じヘッダのKurenai名前空間直下にある
        // シャドウパス(平行光のライト視点から深度のみを描画する)。カメラ視錐台をkCascadeCount個の
        // 深度範囲に分割し(Practical Split Scheme)、それぞれ専用の正射影・シャドウマップを持たせる
        // カスケードシャドウマップ(CSM)。近いカスケードほどテクセル密度が高く、遠いカスケードほど
        // 広い範囲を粗くカバーする
        // 出所は Rendering/ShadowConstants.h(移行中の別名)



        // 背景(深度が書き込まれなかったピクセル)に表示する空のキューブマップ。
        // .ksceneの[Scene]Skyboxでシーンごとに差し替えられる(LoadScene参照)
        std::unique_ptr<RHI::IRHITexture> m_SkyboxTexture;
        // 既定のスカイボックス(Assets/Skybox/Sky.dds)の絶対パス。[Scene]Skybox指定が無いシーンへ
        // 切り替えたときはここへ戻す
        std::wstring m_DefaultSkyboxPath;
        // 現在m_SkyboxTextureへ読み込んでいるファイルの絶対パス。シーン切り替えのたびに
        // 読み直さずに済むよう比較に使う
        std::wstring m_CurrentSkyboxPath;

        // 水面法線マップ(水面マテリアル基盤)。m_SkyboxTextureとまったく同じ方針で、
        // .ksceneの[Water]NormalMapで差し替えられる。空文字列のシーン(NormalMap未指定)では
        // 1x1のフラット法線(128,128,255,255、CreateSolidColorTexture)へフォールバックする
        std::unique_ptr<RHI::IRHITexture> m_WaterNormalMapTexture;
        // 現在m_WaterNormalMapTextureへ読み込んでいる絶対パス。空文字列ならフラット法線
        // フォールバックを使用中であることを表す(m_CurrentSkyboxPathと同じ比較用途)
        std::wstring m_CurrentWaterNormalMapPath;

        // IBL(Image Based Lighting): m_SkyboxTextureから拡散イラディアンス・プリフィルタ済み鏡面・
        // BRDF積分LUTの3つをコンピュートシェーダーで畳み込む(split-sum近似、Karis 2013)。
        // スカイボックスは実行時に変化しない静的アセットのため、起動後最初のRender()で一度だけ
        // 焼いてEnvironmentPassesのm_IBLBakedを立て、以降は焼き直さない(詳細はdocs/Architecture.html参照)。
        // 拡散イラディアンス・プリフィルタ済み鏡面はいずれも本物のTextureCube
        // (CreateUAVTextureCube/CreateMippedUAVTextureCube、面ごとに個別のUAVを持つ)で、
        // IBLConvolve.hlslが面ごとに1回ずつディスパッチして書き込む
    public:
        // キューブマップの面数(D3D標準順: +X,-X,+Y,-Y,+Z,-Z)。IBLの2つのキューブマップは
        // いずれもこの順で面ごとにディスパッチする(IBLConvolve.hlsl CubeFaceDirectionと一致させる)
        // 出所は Rendering/CubeFaceMath.h(移行中の別名)
        static constexpr uint32_t kCubeFaceCount = ::Kurenai::kCubeFaceCount;
    private:
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // ボリュメトリック雲の3Dノイズの1辺のテクセル数。
        // Shapeは128^3のRGBA8で8MB、Detailは32^3のRGBA8で128KB。合わせて約8.1MB。
        // Shapeを128にしているのは、雲1つが画面上で数百画素に広がるため塊の形にはこの程度の
        // 解像度が要る一方、これ以上上げるとメモリが4倍(256^3で64MB)に跳ねるため。
        // Detailは縁を削るだけで低周波成分を持たないので32で足りる
        // 大気散乱のLUT(Hillaire 2020)。解像度は論文の推奨値。
        // Transmittanceは高度×視線天頂角、MultiScatteringは高度×太陽天頂角で、
        // どちらも大気パラメータだけで決まるためカメラにも時刻にも依存しない。
        // SkyViewは空そのもの(太陽の子午線からの方位×天頂角)で、太陽が動くと変わる。
        // **Passes::kSkyViewLUTWidth/Heightはシェーダ側(AtmosphereCommon.hlsliの
        // kSkyViewLUTWidthF/kSkyViewLUTHeightF)と一致させること** — UVの半テクセル補正に
        // 解像度が要るため、焼く側・引く側の両方が同じ値を知っている必要がある
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // ウェザーマップ(H3)。ノイズ空間の1周期(256セル=358km)を1枚で覆うので、
        // 4096なら88m/テクセル。**CloudNoiseGenerate.hlsl の kWeatherNoiseSize と同じ値であること**
        // (片方だけ変えるとテクセル中心がずれ、バイリニアが半テクセル分ぼける)。
        // R8G8B8A8で4096^2 = 67MB。解像度の実測はSky.hlsliのウェザーマップの節
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 手続き空(SkyGenerate.hlsl): Perez分布をGPUで評価してキューブマップを生成する。
        // オフラインで焼いたDDS(Sky.dds)と違い、太陽が動くと空の輝度分布の「形」も追従する
        // (circumsolarの明るい領域が太陽と一緒に動く)。詳細はSkyGenerate.hlsl冒頭。
        //
        // .ksceneで[Scene]Skyboxを明示しているシーン(White Furnace TestのUniformWhite.dds)は
        // 従来どおりDDSを使う必要があるため、手続き空は別テクスチャに持ち、
        // ActiveSkyTexture()がフレームごとにどちらを使うか決める
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // 手続き空のキューブマップは持ち主を SkyResources::ProceduralSkyTexture へ移した
        // シェーダー・PSO・定数バッファは Passes/EnvironmentPasses へ移した
        // SkyGenerate用の専用定数バッファ。m_IBLResources.PrefilterConstantBufferと共用しないこと
        // (UpdateBuffer→SetComputeConstantBufferの順序制約があり、共用すると事故りやすい。
        //  詳細はRHI/IRHICommandList.hのSetConstantBufferのコメント)
        // 手続き空を焼き直す必要があるか。太陽が動いたとき等に立てる
        bool m_SkyBakeDirty = true;
        // 最後に焼いたときの太陽の向き。これと現在の向きの角度差が閾値を超えたら焼き直す。
        // 毎フレーム焼くと空生成6回+プリフィルタ36回が常時走って無駄なため
        DirectX::XMFLOAT3 m_LastBakedSunPosition{ 0.0f, 0.0f, 0.0f };
        // 最後に焼いたときの実効プリ露出。空はプリ露出済みの値で焼かれるため、
        // 露出が動いたときも焼き直さないと空だけ古い露出のまま取り残される
        float m_LastBakedExposureEV100 = 0.0f;
        // 最後に焼いたときのタービディティ。m_Settings.Sky.Turbidityが動いたときも、Preethamの
        // xyYモデルの形自体が変わるため焼き直しが要る(exposureMovedと同じ形の判定。Render()参照)
        float m_LastBakedTurbidity = 0.0f;
        // 最後に焼いたときの空の彩度。タービディティと同じ理由で、動いたら焼き直す
        float m_LastBakedSkySaturation = 0.0f;

        // P18: 雲込みの空の照度(SkyIntegrateのCloudSkyLight)とIBLキューブの平均透過率
        // (m_ActiveCloudTransmittance)は、どちらもベイクのタイミングでしか更新されない。
        // ところが焼き直しの判定に雲のパラメータが入っていなかったため、被覆率を動かしても
        // 古い値が残り続けていた。ここへ「ベイク時点の雲のパラメータ」を覚えておき、
        // 変化したら焼き直す(Render()の焼き直し判定を参照)。
        // **風のスクロールとカメラ位置は入れない**——毎フレーム動くので入れると毎フレーム
        // 焼き直しになる。求めているのは半球平均なので、雲の場の平行移動では値がほとんど動かない
        struct CloudBakeSignature
        {
            float CumulusCoverage = -1.0f;   // 無効(m_Settings.Cloud.Enabled=false)なら0
            float CumulusAltitude = 0.0f;
            float CumulusUvScale = 0.0f;
            float CumulusDensity = 0.0f;
            float CumulusForwardG = 0.0f;
            float CumulusThickness = 0.0f;   // ボリューム無効なら0(FrameConstantsと同じ扱い)
            float CloudTypeBias = 0.0f;
            float CirrusCoverage = 0.0f;     // 無効(m_Settings.Cloud.CirrusEnabled=false)なら0
            float CirrusAltitude = 0.0f;
            float CirrusUvScale = 0.0f;
            float CirrusDensity = 0.0f;
            float CirrusAnisotropy = 0.0f;
            float FogSigma0 = 0.0f;          // 霞は雲の見え方(打ち切り)を変えるので入れる
            float FogScaleHeight = 0.0f;
            float FogRefHeight = 0.0f;
            float FogEnabled = 0.0f;

            bool operator==(const CloudBakeSignature&) const = default;
        };
        // 現在の設定からシグネチャを作る。**FrameConstants/SkyIntegrateConstantsへ詰めるのと
        // 同じ有効/無効の潰し方をすること**(m_Settings.Cloud.Enabled=falseなら被覆率0、など)。
        // 揃っていないと「無効にしたのに焼き直しが走らない」取りこぼしが出る
        CloudBakeSignature MakeCloudBakeSignature() const;
        CloudBakeSignature m_LastBakedCloudSignature{};
        bool m_HasBakedCloudSignature = false;
        // 空パラメータ(ティント4本+照度正規化済みの天頂輝度)をGPU側で計算するコンピュートシェーダー
        // (SkyIntegrate.hlsl)。**CPU側に同じ式のミラーを置いてはいけない**(二重実装になる)。
        // 結果はm_SkyResources.ParametersBuffer(SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslが読む)へ書く
        // 空・大気・雲のリソースの持ち主は Rendering/SkyResources.h
        Rendering::SkyResources m_SkyResources;
        // 空パラメータの初期化済み旗は持ち主を Passes/EnvironmentPasses へ移した

        // IBL・BRDF・雲ノイズ・大気の焼き上がりの状態は Passes/EnvironmentPasses へ移した
        // 畳み込み結果とBRDF積分LUTの持ち主は Rendering/IBLResources.h
        Rendering::IBLResources m_IBLResources;
        // BRDF積分LUTのスクラッチとPSO2本は Passes/EnvironmentPasses へ移した


        // 大気散乱の定数バッファとPSO3本は Passes/EnvironmentPasses へ移した
        // 太陽がこの角度以上動いたらSkyView LUTを焼き直す。LUTは天頂方向180度を108テクセルで
        // 持つので1テクセルあたり約1.67度あり、その1/30以下しかずらさない値にしてある。
        //
        // 【意図的に手続き空のm_Settings.Sky.BakeAngleThresholdDegrees(1.0度)より桁で細かくしている】
        // このLUTは背景の空(Sky.hlsliのSkyColor)が画面解像度で毎フレーム引くもので、
        // 間引きの粒度がそのまま背景の時間解像度になる。一方あちらが焼くIBLキューブは
        // 6面+プリフィルタ36回のディスパッチを伴う重いベイクで、間接光にしか効かない。
        // 変更前は「毎フレーム焼く」だったので、それに最も近い挙動を選んでいる。
        //
        // 【時刻を自動で進めるシーンでは削減にならない】Auto Advance既定(1h/s)では太陽は
        // 15度/秒動くため、60fpsでも毎フレームこの閾値を超えて結局毎フレーム焼く。
        // 削減が効くのは太陽が止まっているシーン(Defaults::TimeAutoAdvanceは既定false)で、
        // その場合は起動直後の1回だけになる
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // イラディアンス畳み込みのシェーダーとPSOは Passes/EnvironmentPasses へ移した
        std::unique_ptr<RHI::IRHIShader> m_PrefilterComputeShader;
        // 畳み込みのPSOは持ち主を IBLResources::PrefilterPipelineState へ移した
        // 拡散イラディアンスの球面調和関数(SH L2)経路。CSIrradianceの高速な
        // 代替で、m_Settings.IBL.UseSHIrradianceでA/B比較できるようトグルにしてある。詳細は
        // IBLConvolve.hlsl冒頭のコメントとdocs/Architecture.htmlを参照
        // SHの項数の定数は Passes/EnvironmentConstants.h へ移した
        // CSProjectSHの射影に使う離散化解像度(1面の1辺のテクセル数)。
        // 【SourceSkyboxの実解像度とは無関係】スカイボックスはDDS(シーンごとに任意の解像度)や
        // 手続き空(256)など実行時に変わりうる一方、IRHITextureには解像度を問い合わせる手段が
        // 無いため、射影側は独立した固定解像度を持つ(IBLConvolve.hlslのSHProjectionSizeコメント参照)。
        // 64×64×6=24,576テクセルはCSIrradianceの約9,750万サンプルに対し十分密で、
        // 9個の係数を求めるだけの積分には(理論上は32でも足りる範囲)余裕を持たせた値
        // 出所は Passes/EnvironmentConstants.h(移行中の別名)
        // SHのシェーダー3本・PSO3本・バッファ2本は Passes/EnvironmentPasses へ移した
        // IBLの有効/強度・SH経路・専用イラディアンス・環境光の拡散/鏡面/フォールバック強度は
        // m_Settings.IBLへ移した(Settings/IBLSettings.h)。bent normal/multi-bounce AOの
        // ソース選択はm_Settings.AmbientOcclusionへ移した(Settings/AmbientOcclusionSettings.h)


        // --- エミッシブ光源(自発光メッシュを光源として扱う) ---
        //
        // SceneLoaderがワールド空間へ変換したプロキシ。**m_Lightsとは別に持つ。**
        // 作者が置いたライトと自動生成の光源を同じ配列にすると、ImGuiのライト一覧から
        // 消せてしまい元のメッシュと食い違う。上限超過時に手置きを押し出さないためでもある
        std::vector<Assets::EmissiveProxy> m_EmissiveProxies;
        // インスタンスごとに「このインスタンスからプロキシを起こしたか」。
        // LoadSceneでm_EmissiveProxiesから作る(要素数はm_Scene.Instances.size())。
        //
        // 【DDGIのラスタ経路で要る】あちらはモデルLODの粗い段を描くので、
        // プロキシが持つMeshIndex(段0の番号)では引けない。インスタンス単位で
        // 判定し、メッシュ側はEmissiveClustersの有無で見る
        std::vector<bool> m_EmissiveProxyInstances;
    public:
        // 【publicにしてある】シーン読み込みが構築し、Passes::DDGIPasses が
        // ラスタ経路で「このインスタンスは自発光プロキシか」を引くために読むだけ
        const std::vector<bool>& GetEmissiveProxyInstances() const { return m_EmissiveProxyInstances; }

    private:
        // 段階2: 発光面を三角形のまま面積分するか。MegaLights 経路でのみ効く
        // (有効なフレームは参照実装が型3のプロキシを読み飛ばし、代わりに三角形を積む)
        bool m_MeshLightsEnabled = Defaults::MeshLightsEnabled;
        // RangeのクランプにつかうシーンAABBの対角。LoadSceneで一度だけ求める
        float m_EmissiveLightsMaxRange = 0.0f;
        // 上限で切り捨てたときの「採用した集合」の指紋。切り捨てが起きなければ0。
        //
        // 【プローブの署名に混ぜるためだけにある】採用順はカメラからの照度で決まるので、
        // 上限に当たっているシーンではカメラを動かすだけで焼く光源の集合が変わる。
        // 署名へ入れないと、収束済みのプローブだけ古い集合のまま残る
        uint64_t m_EmissiveLightsSelectionHash = 0;
        bool m_EmissiveLightsCapLogged = false;
        // 送信した灯の実効値を1回だけログへ出したか(「走っていない」と「暗い」の切り分け用)
        bool m_EmissiveLightsValuesLogged = false;

        // 反射プローブ(19章): プローブ位置から6方向をProbeCapture.hlslで2Dレンダーターゲットへ描き、
        // IBLConvolve.hlsl CSCopyCaptureToCubeFaceでスクラッチのキューブマップへ組み上げてから、
        // IBLと同じCSIrradiance/CSPrefilterで畳み込んでプローブごとのキューブマップ配列へ書き込む。
        // 環境ソースを差し替えるだけなので、シェーダー側の評価式(EvaluateIBL)はIBLと完全に共通。
        //
    public:
        // キューブマップ配列の枚数上限。TextureCubeArrayは実行時に伸縮できないため固定容量で確保し、
        // これを超えるプローブが置かれたシーンは先頭からこの数だけを採用する(警告ログを出す)
        static constexpr uint32_t kMaxReflectionProbes = 8;
    private:
        // キャプチャ解像度。プリフィルタ済み鏡面のベース解像度(Passes::kIBLPrefilterBaseSize)と揃えることで、
        // ミップ0が「畳み込み無しのキャプチャそのもの」になりデバッグ表示で生の映り込みを確認できる
        // 出所は Passes/ReflectionProbeConstants.h(移行中の別名)
        std::unique_ptr<RHI::IRHIShader> m_ProbeCaptureVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ProbeCapturePixelShader;
        // キャプチャのPSOは持ち主を GIResources::ProbeCapturePipelineState へ移した。
        // その書き先(ProbeCaptureColor / ProbeCaptureDistance / ProbeCaptureDepth)も同じ理由でGIResourcesにある
        std::unique_ptr<RHI::IRHIShader> m_ProbeCubeCopyComputeShader;
        // キューブへ写すPSOは持ち主を GIResources::ProbeCubeCopyPipelineState へ移した。
        // 写し先のスクラッチキューブマップ(ProbeRadianceCube)も同じくGIResourcesにある
        // 面ごとの定数バッファは持ち主を GIResources::ProbeCaptureConstantBuffer へ移した
        // プローブの一覧は持ち主を GIResources::ReflectionProbes へ移した
        int m_SelectedProbeIndex = -1;
        // 焼き上がりの状態(要求・焼けたか・Realtimeの進行・署名・焼いた時点の露出)は
        // 持ち主を Passes::ReflectionProbePasses へ移した。書き手がその群だけだったため
        // プリフィルタの進行状態・OnDemandの署名は持ち主を Passes::ReflectionProbePasses へ移した。
        // ステップ数の定数(kProbePrefilterStepCount / kProbeRealtimePrefilterStepsPerFrame)も
        // 読み手がその群だけになったため、Passes/ReflectionProbeConstants.h を直接使わせている
        // 焼き上がりに影響する状態(時刻・太陽・シャドウ・IBL強度・全ライト)から署名を作る。
        // 影響範囲(形状・半径・ブレンド距離)はキャプチャ内容を変えないため含めない
        uint64_t ComputeProbeBakeSignature() const;
        // 焼き直しを要求する実効プリ露出の変化量[EV]。1段=明るさ2倍。
        // 手続き空の0.05段よりずっと粗いのは、フルベイクがプローブ数×6面の描画になるため。
        // 1日を通した時刻変化(最大18段)なら十数回のフルベイクに収まる
        static constexpr float kProbeRebakeExposureEV = 1.0f;

        // --- DDGI(Dynamic Diffuse Global Illumination、22章) ---
        //
        // 反射プローブ(上)が「少数を手で置き、主に鏡面を担う」のに対し、DDGIは
        // 「格子状に多数を自動配置し、拡散の間接光だけを担う」。レイの取得には反射プローブと
        // まったく同じキャプチャ経路(ProbeCapture.hlslの6面MRT)を使い、キャプチャ解像度だけ
        // 落とす。得られた放射輝度と距離を、キューブではなくオクタヘドラル投影の2Dアトラスへ
        // 畳み込む(DDGIProbeUpdate.hlsl)。
        //
        // 【20章の単一定義規則との関係】DDGIが差し替えるのはReflectionProbe.hlsliの
        // SampleEnvironmentが返す拡散イラディアンスだけで、鏡面(prefiltered)と
        // SpecularIBLWeightには一切触れない。したがって「SSRはDeferredLightingが足した
        // 鏡面IBLと厳密に同じ量を引く」という不変条件はDDGIを入れても保たれる

        // オクタヘドラル1プローブぶんの1辺のテクセル数(境界を含まない)。
        // 拡散イラディアンスは低周波なのでこの程度で足りる。距離は遮蔽の輪郭を担うので広く取る
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // 各辺に足す境界の幅。オクタヘドラルは正方形の縁が球面上で折り返して繋がるため、
        // その繋がる先のテクセルを外周へ複製しておかないと、バイリニア補間が縁で破綻する
        // (隣のプローブのテクセルを拾ってしまうことの防止も兼ねる)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // アトラス上の1プローブぶんのセルの1辺(境界込み)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // プローブ数の上限。反射プローブと違いアトラスはシーン読み込み時に確保し直すので
        // 技術的な固定容量ではないが、.ksceneの書き間違いで数GBのアトラスを作らないための歯止め
        // シーン全体で確保してよいプローブ数の上限。**容量の限界ではなく、`.kscene`の
        // 打ち間違いでギガバイト単位を確保しないための番人**である。
        // クリップマップLODでプローブ総数が「格子の積 × LOD段数」になったので引き上げた
        // (8192でもイラディアンス8MB + 距離16MB程度で、実際の律速は更新スループット側)
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // クリップマップLODの最大段数。FrameConstantsへ段数ぶんの配列を持つので有界にしておく。
        // SceneLoaderのLODCountの検証範囲と一致させること
        static constexpr uint32_t kDDGIMaxLODCount = 4;
        // キャプチャ解像度(1面あたり)。6面ぶんで 16×16×6 = 1536方向がレイの代わりになる。
        // 反射プローブのkProbeCaptureSize(128)と違い小さくてよいのは、DDGIが必要とするのが
        // 「低周波の拡散イラディアンス」であって鏡面の映り込みではないため
        // 出所は Passes/DDGIConstants.h(移行中の別名)

        // 【m_GIResourcesより後に宣言すること】ボリュームの実体を参照で掴むので、
        // 宣言順が逆になると未初期化のメンバを束ねることになる
        GI::DDGIGrid m_DDGIGrid{ m_GIResources.GIVolume };

        // レイ取得(DXR)の経路とキャプチャ資源一式は Passes/DDGIPasses へ移した

        // これを超えて実効プリ露出が動いたら追従させる(段)。1段=明るさ2倍ぶん
        // 出所は Passes/DDGIConstants.h(移行中の別名)
        // ConvergeThenStopで停止するまでの巡回数。
        //
        // 【なぜヒステリシス由来の巡回数をやめたか】以前は残差0.01を切る巡回数
        // N = ln(0.01)/ln(ヒステリシス) を停止条件にしていた(既定0.97なら152巡)。
        // これは**1巡が何フレームかを見ていない**ため、プローブが多いボリュームでは
        // 実質止まらなかった ―― Sponza(1152プローブ・4個/フレーム)で1巡288フレーム、
        // 152巡 = 43,776フレーム ≒ 53分。実測でも180秒回して止まらず、
        // 品質プリセット「中」がいちばん助けが要るシーンで効かない状態だった。
        //
        // 【上書きで巡回すれば足りる理由】署名が止まっている間、キャプチャは決定的な
        // ラスタライズなので平滑すべき確率的ノイズが無い。ヒステリシスは目標値へ
        // 指数的に近づくだけで、目標値そのものは上書き1巡で入る。複数巡が要るのは
        // 多重バウンスだけで、ProbeCapture.hlslが前巡のアトラスを読む構造上
        // 1巡につき1バウンス積み上がる。反射率aの面ならN巡後の相対残差はおよそa^Nで、
        // a=0.5なら4巡で6%、a=0.7なら24%。4巡は「止まるまでの時間」との折り合いで選んだ値であり、
        // バウンスを完全に積み切る数ではない(積み切りたいならAlwaysを使う)。
        // ヒステリシスは「常時更新」で光の変化に滑らかに追従させる役目に戻した。
        //
        // 【4巡と1巡の差はまだ実測できていない】Sponzaの同一カメラで両者を撮り比べると
        // 3Dビューポートはビット一致だった(Alwaysを200秒回したものとも一致)。
        // ProbeCapture.hlslへデバッグ色を焼いて原因を追ったが、赤チャンネルが垂れ幕への
        // 直接光と混ざり、Always側もヒステリシス0.97のため200秒ではまだ大半がウォームアップ時の
        // 値で、多重バウンスの寄与を分離できる計測になっていない。上の残差の式は理屈であって
        // 裏取り済みの数字ではない
        // 出所は Passes/DDGIConstants.h(移行中の別名)

        // クリップマップLODの格子(プローブ番号 ⇔ ワールド座標)は GI::DDGIGrid が持つ。
        // 設計の意図と、更新CSのアトラス座標式を1文字も変えずに済む理由は GI/DDGIGrid.h にある
    public:
        // 【publicにしてある】Passes::DDGIPasses がプローブの位置と担当座標を引くために呼ぶ。
        // どれも設定と格子から導くだけの計算で、状態を持たないので公開しても持ち主は変わらない
        GI::DDGIGrid& GetDDGIGrid() { return m_DDGIGrid; }

    private:

        // m_GIResources.GIVolumeのProbeCountsに合わせてアトラス2枚を確保し直す。ボリュームが無いシーンでは
        // 1プローブぶんのダミーを確保する(SRVは常にバインドできる必要があるため、
        // 「確保しない」という選択肢は取れない。無効化はDDGIParams0.wで行う)
        void RecreateDDGIAtlases();

        // 1フレームに焼くプローブ数を、DX12の「1フレームあたりの予算」に収まる範囲へ抑える。
        //
        // 【なぜ要るのか】ラスタ経路のプローブキャプチャは1プローブにつきシーンを6回描き直すため、
        // 1フレームの描画回数とObjectConstantsの書き込み回数がどちらも
        // 「プローブ数 × 6面 × 不透明メッシュ数」に比例して増える。DX12はどちらにも上限があり、
        //   - 描画回数(IRHIDevice::GetMaxDrawsPerFrame) … 超えるとSRVテーブルの払い出しが
        //     例外を投げ、ログを残さずプロセスごと落ちる
        //   - 定数の書き込み回数(IRHIBuffer::GetSafeUpdatesPerFrame) … 超えるとGPUが
        //     読み取り中のスロットを上書きして描画が壊れる
        // BistroInteriorLit(不透明59メッシュ)を既定の16プローブ/フレームで焼くと
        // 59×6×16 = 5664 となり、実際に前者を踏んで起動直後に落ちていた。
        //
        // レイトレース経路にはメッシュごとの描画そのものが無いので、この制約は掛からない
    public:
        // 【publicにしてある】上と同じ理由。1フレームに焼けるプローブ数を
        // ObjectConstantsのリング段数から決める判定で、群が登録時に呼ぶ
        uint32_t ClampDDGIProbesPerFrameToConstantRing(uint32_t requested);

    private:
        // ObjectConstantsのリングに要求する「1フレームあたりの書き込み回数」。
        // 根拠はこのバッファを作っている箇所(KurenaiEngine3D.cpp)のコメントを参照
        static constexpr uint32_t kObjectConstantUpdatesPerFrame = 16384;
        // DDGI以外のパスが1メッシュあたり何回描くかの見積り。DDGIへ回す予算から差し引く。
        // 内訳の目安: 深度プリパス1 + G-Buffer1 + シャドウ4 + 半透明・平面反射・水面で数回、
        // これに反射プローブのキャプチャ(こちらも1プローブ6面ぶんメッシュを描き直す)が乗る。
        // 厳密に数えず多めに取っているのは、パス構成が設定とシーンで変わるため
        static constexpr uint32_t kDDGIFrameBudgetReserveDrawsPerMesh = 16;
        // 上のクランプが効いたことを一度だけログへ出すためのフラグ(毎フレーム出さない)
        bool m_DDGIProbesPerFrameClampReported = false;

        // 水面。m_Settings.Sky.TimeOfDayの自動進行とまったく同じ方針
        // (RenderThreadMainが同じ場所・同じ条件分岐の形で進める)で、水面法線マップの
        // スクロール位相を[0,1)で持つ。FrameConstants.TimeParams.xとしてWater.hlslへ渡る
        float m_WaterScrollOffset = 0.0f;
        // --- 平面反射 ---
        // 水面に不透明ジオメトリの鏡像を映す専用フォワードパス。設計判断の詳細は
        // Shaders/3D/PlanarReflection.hlsl冒頭のコメントを参照。反射解像度はレンダー解像度に
        // m_Settings.Reflection.PlanarResolutionScaleを掛けた値で、実際の作成はCreatePlanarReflectionTargetsが行う。
        // レンダーターゲット2枚と実寸は、PresentPassのデバッグ表示も読むため
        // 持ち主をRenderTargets(m_RenderTargets.PlanarReflection*)へ移した。
        // シェーダー・PSO・定数バッファはPasses/ReflectionPassesへ移した
        // 「システム」パネルのm_PendingRenderWidth/Height・m_RenderResolutionDirtyとまったく同じ方式
        // (要求を記録するだけにしてRender()の先頭でまとめて反映する。理由はCreateRenderTargets/
        // RequestRenderResolutionのコメント参照。GPUがまだ参照しているテクスチャを
        // 即座には破棄できないため)
        float m_PendingPlanarReflectionResolutionScale = Defaults::PlanarReflectionResolutionScale;
        bool m_PlanarReflectionResolutionDirty = false;
        // 複数の水面インスタンスが異なる高さで見つかったことを検出した最初のフレームだけ
        // 警告ログを出すためのフラグ(m_LightTileOverflowLoggedと同じ作法)。平面反射は
        // 「水面は単一の水平な平面である」という前提に立っており、複数ある場合は最初のものだけを使う
        bool m_PlanarReflectionMultipleWaterLogged = false;

    public:
        // UIのつまみの上限。**シェーダー側の段数そのものではない。**
        // Sky.hlsli は既定 kCloudMaxRaymarchSteps(384)で走り、cbuffer で0より大きい値を
        // 渡されたときだけそれを使う。その値は kCloudRaymarchStepsHardMax(512)で丸められる。
        // したがってここに要る条件は「512を超えないこと」だけで、一致させる相手はいない
        static constexpr uint32_t kCloudRaymarchStepsMax = 32;
    private:
        // 風によるノイズ空間の移動量。m_WaterScrollOffsetと同じくUIつまみではなく内部状態で、
        // RenderThreadMainがSky.hlsliのkCloudNoisePeriodと同じ周期でstd::fmodしながら進める
        DirectX::XMFLOAT2 m_CloudScrollOffset{ 0.0f, 0.0f };
        // 判断B(被覆率による平均透過率をIBLキューブのベイク時にだけ掛ける)のキャッシュ。
        // bakeSkyThisFrameブロックで確定させ、ベイクとFrameConstantsが同じタイミングの
        // 値を見るようにする(GPU側のm_SkyResources.ParametersBufferと同じ更新タイミング)。
        // 巻雲(m_Settings.Cloud.CirrusCoverage)も加味した2層の積になる
        // (ComputeCloudAverageTransmittance参照)
        float m_ActiveCloudTransmittance = 1.0f;

        // 風によるノイズ空間の移動量(巻雲側)。m_CloudScrollOffsetとまったく同じ形で
        // RenderThreadMainがkCloudNoisePeriodの周期でstd::fmodしながら進める。
        // 凍結トグルはm_Settings.Cloud.TimeFrozenを共有する(片方にしか効かないとA/B比較の対照が
        // 崩れるため。RenderThreadMainのスクロール更新箇所を参照)
        DirectX::XMFLOAT2 m_CirrusScrollOffset{ 0.0f, 0.0f };


        // パスごとにバインドするサンプラーの組。スロットの役割(s0=MaterialSampler、
        // s1=ColorSampler、s2=DataSampler)はShaders/3D/Samplers.hlsliで定義しており、
        // どちらのセットを使うかでs0の実体だけが変わる。
        //
        // マテリアルをタイリングで読むパス(G-Buffer・半透明フォワード・IBL畳み込み)用。
        // s0は異方性16x + Wrap
        std::unique_ptr<RHI::IRHISamplerSet> m_MaterialSamplers;
        // フルスクリーンのスクリーン空間パス(DirectLighting/DeferredLighting/SSAO/SSIL/SSR/
        // AOブラー/トーンマップ/Present)用。これらは画面内の中間バッファしか読まないため、
        // s0にもWrapではなくLinear + Clampを入れる。こうしておくとシェーダ側で役割を選び違えても
        // 画面端でUVが反対側へ回り込む不具合が起きない
        std::unique_ptr<RHI::IRHISamplerSet> m_ScreenSpaceSamplers;
        std::unique_ptr<RHI::IRHIBuffer> m_FrameConstantBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_ObjectConstantBuffer;

        // 有効ライト数を渡すb1の持ち主は Passes/LightingPasses。
        // ライトのリスト本体は m_SceneGPUResources.LightBuffer
        // 容量(kMaxLights)超過を検出した最初のフレームだけ警告ログを出すためのフラグ
        bool m_LightOverflowLogged = false;

        // タイルライトカリング(Shaders/3D/LightCulling.hlsl)。画面を16x16ピクセルのタイルに分け、
        // タイルごとに「そのタイルに届くライト」のインデックスリストをコンピュートシェーダーで作る。
        // 直接光パスはそのリストだけをループするため、ピクセルあたりのコストが
        // シーン全体のライト数ではなくタイル内のライト数になる。
        // これは純粋な最適化であり、有効/無効で最終画像が変わってはならない
        // タイルライトカリングのタイルサイズ(1辺のピクセル数)。
        // LightCulling.hlsl の kTileSize および numthreads と必ず一致させること
        // 出所は Passes/MegaLightsConstants.h(移行中の別名)
        // ライトグリッド1タイルぶんの要素数(先頭1個がライト数、残りがライトインデックス)
        static constexpr uint32_t kLightTileStride = 1 + Passes::kLightTileCapacity;

    public:
        // MegaLightsの候補プールが1タイルあたりに抽出する候補の数(K)。
        // ライトタイルの容量と違い**これは打ち切りではなく抽出数**で、タイルへ何灯届いていても
        // ここで決めた本数だけを重みつきで取り出す。届いた灯が欠落するわけではない
        // (どの灯も w_i / SumW の確率で選ばれる)ため、容量超過のような静かな欠落は起きない。
        // 【実行時に振れる。ここは確保の上限】1タイルの抽出数Kは
        // m_Settings.MegaLights.TilePoolCapacity が持ち、シェーダへは定数バッファで渡している。
        // バッファの確保だけがコンパイル時の上限を要るのでここに残す
        static constexpr uint32_t kMegaLightsTilePoolCapacity = 128;
        // Kの下限。これを下回るとタイルに届く灯を代表できない。
        static constexpr int32_t kMegaLightsTilePoolMinCapacity = 8;
    private:
        // 候補プール1タイルぶんの要素数。先頭6個がヘッダ(SumW / 届いた灯数 / 有効候補数 / 予約 /
        // 手前のViewZ / 奥のViewZ)、
        // 以降は候補1つにつき2個(ライト番号と重み)。MegaLightsTilePool.hlsl 冒頭のレイアウトと一致させること
        static constexpr uint32_t kMegaLightsTilePoolStride = 6 + 2 * kMegaLightsTilePoolCapacity;
    public:
        // 1画素あたりの標本数の上限。リザーババッファはこの倍数まで太る
        //(16バイト x 画素数 x 標本数。2560x1440・4本で236MB)ので、際限なく上げさせない。
        // クアッド層化は4層なので、4を超えると層の割り当てが一巡して効きが鈍る
        static constexpr int32_t kMegaLightsMaxSamplesPerPixel = 4;
    private:

        // ライトグリッド本体とタイル数は、3群(Lighting / MegaLights / Present)が読むため
        // 持ち主をRenderTargets(m_RenderTargets.LightTileBuffer / LightTileCountX / Y)へ移した
        // タイル容量の超過"条件"(シーンのライト数が容量を超えている)を検出した最初のフレームだけ
        // 警告ログを出すためのフラグ(m_LightOverflowLoggedと同じ作法)。
        // 実際に超過したかはGPU側にしか無いため、確認はDebugView::LightTilesのマゼンタで行う
        bool m_LightTileOverflowLogged = false;
        // DebugView::LightTilesのヒートマップの上限はm_Settings.DebugView.LightTileHeatmapMaxへ移した

        // 自前ソフトウェアラスタライザ(46章)の資源とパス本体は Passes::GeometryPasses が持つ。
        // 型と定数(SWRasterConstants / SWRasterMeshInfo / kSWRaster*)は
        // Passes/GeometryConstants.h へ移した。
        // 巨大三角形とみなすbbox画素面積のしきい値と、その既定値・可動範囲(kSWRasterDefault/Min/Max
        // LargeTriangleArea)はm_Settings.Geometry.SoftwareRasterLargeTriangleAreaへ移した

        // --- 品質プリセット(41章) ---------------------------------------------------------
        //
        // QualityPreset(enum)とその既定値はSettings/QualitySettings.hへ移した
        // (m_Settings.Quality.Preset)。ここに残るのはプリセットが実際に触る設定の一式
        // (QualitySnapshot)と、それを読み書きする関数だけ。
        //
        // 【QualitySnapshotという名前にしている理由】Settings/QualitySettings.hの
        // struct QualitySettingsと役目がまったく違う(あちらは「今どのプリセットを
        // 選んでいるか」の静的な設定、こちらは「プリセットが一括で振る個々のつまみの値を
        // 退避・復元するためのスナップショット」)。同じ名前にすると呼び出し側で
        // どちらの型か紛らわしくなるため、クラス内のこちらをQualitySnapshotと呼び分ける

        // 品質プリセットが触る設定の一式(退避・復元用のスナップショット)。
        //
        // 【プリセット「高」はエンジンの静的な既定ではなく「シーン読み込み直後の値」へ戻す】
        // .ksceneはSSR・TAAを自分で指定できる(ApplyLoadedScene参照。実例として
        // MontSaintMichel.ksceneはどちらも明示的に有効化している)。静的なDefaults::へ戻すと
        // 「高にしたらシーンが要求した反射が消える」ことになる。m_SceneDefaultReflectionModeが
        // UIの右クリック(既定値へ戻す)に対して同じ問題を解いており、プリセットもそれに倣う
        struct QualitySnapshot
        {
            ReflectionMode Reflection = ReflectionMode::Off;
            bool PlanarReflectionEnabled = Defaults::PlanarReflectionEnabled;
            float PlanarReflectionResolutionScale = Defaults::PlanarReflectionResolutionScale;
            bool CloudVolumetric = Defaults::CloudVolumetric;
            bool CirrusEnabled = Defaults::CirrusEnabled;
            bool StarsEnabled = Defaults::StarsEnabled;
            bool TAAEnabled = Defaults::TAAEnabled;
            bool BloomEnabled = Defaults::BloomEnabled;
            bool ScreenSpaceShadowEnabled = Defaults::ScreenSpaceShadowEnabled;
            int32_t DDGIProbesPerFrame = Defaults::DDGIProbesPerFrame;
            uint32_t SSAOKernelSize = Defaults::SSAOKernelSize;
            uint32_t CloudRaymarchSteps = Defaults::CloudRaymarchSteps;
            DDGIUpdateMode DDGIUpdate = DDGIUpdateMode::Always;
            bool DDGIHalfResolution = Defaults::DDGIHalfResolution;
        };

        // 現在の各メンバから上記の一式を読み出す
        QualitySnapshot CaptureQualitySettings() const;
        // 一式を各メンバへ書き戻す。平面反射の解像度倍率だけはレンダーターゲットの作り直しを
        // 伴うため直接代入せず、RequestPlanarReflectionResolutionScale()経由で要求する
        void ApplyQualitySettings(const QualitySnapshot& settings);

        // シーンを読み込んだ直後の値。ApplyLoadedSceneが控え、プリセット「高」が戻る先になる
        QualitySnapshot m_SceneDefaultQuality;

        // 現在描画しているシーン。ApplyLoadedScene(Renderスレッド)だけが差し替え、
        // Render()とUIパネル(いずれもRenderスレッド)だけが読む。つまりRenderスレッド専有の状態で、
        // ミューテックスによる保護は不要(LoadSceneがUpdateスレッドから直接書き換える構成だと
        // ミューテックスが要る。26章)。
        //
        // 【読み込み中は空になる】シーン切り替えを開始した時点で旧シーンを手放すため
        // (VRAMの二重常駐を避けるため)、読み込みが終わるまでInstancesが空のまま描画される
        Assets::Scene m_Scene;
        // GPU側のシーンデータの持ち主は Rendering/SceneGPUResources.h。
        // **m_Sceneより後に宣言すること**(理由はそのヘッダの冒頭)
        Rendering::SceneGPUResources m_SceneGPUResources;
        // メッシュライトの三角形テーブル(段階2)。段階1のプロキシと同じ集合から作られる
        Assets::MeshLightScene m_MeshLightScene;
    public:
        // 【publicにしてある】シーン読み込みが構築し、Passes::MegaLightsPasses が
        // 三角形の数とバッファを引くために読むだけ
        const Assets::MeshLightScene& GetMeshLightScene() const { return m_MeshLightScene; }
        bool IsMeshLightsEnabled() const { return m_MeshLightsEnabled; }

    private:
        // テクスチャの常駐ミップ制御。自前のワーカースレッドを持ち、そこがm_Sceneの
        // IRHITexture*を掴む。
        //
        // 【破棄順】m_Sceneより後に宣言し、メンバ破棄順(宣言の逆順)でm_Sceneより先に
        // 破棄されるようにする。加えて、シーンを差し替えるときはUpdateSceneStreamingが
        // 明示的にReset()を呼んでワーカーを止める
        Assets::TextureStreamingManager m_TextureStreaming;
        // m_Sceneと同じくRenderスレッド専有(ScenePanelが選択中のシーンの表示に読む)
        size_t m_CurrentSceneIndex = 0;
        // Updateスレッド専有。UpdateMouseLook/UpdateMovementが書き換え、TickFrameがFrameStateへ
        // スナップショットしてRenderスレッドへ渡す。シーン読み込み時の初期カメラも
        // (Renderスレッドではなく)UpdateAppliedSceneHandoff経由でこのスレッドが適用することで、
        // 書き込み手を1スレッドに保っている
        Core::Camera m_Camera;

        // WASD/E/Qの移動速度[m/s]はm_Settings.System.CameraSpeedへ移した。
        // 【スレッド】書き手はRenderスレッド(ScenePanelのスライダとResetSceneDependentParams)、
        // 読み手はUpdateスレッド(UpdateMovement)。単一のfloatを跨いで読み書きするだけなので
        // 同期は置かない ―― 途中の値が1フレーム見えても「その1フレームだけ移動量が古い速度で
        // 計算される」以上のことは起きない。m_Camera本体はUpdateスレッド専有のまま
        // (この値はそこへ入力されるだけ)。値はシーン対角から決まるためResetSceneDependentParams()
        // が上書きする。Settings側の初期化子は最初のシーンを読むまでの値でしかない

        // --- シーン読み込みのハンドオフ -------------------------------------------------------

        std::thread m_LoaderThread;

        // Renderスレッド専有。ScenePanelが押されたときに積まれ、UpdateSceneStreamingが消費する。
        // -1は「要求なし」。UIもRenderスレッドで動くため、これはatomicである必要がない
        int m_PendingSceneRequest = -1;
        // Renderスレッド専有。Loaderスレッドへ発注してから完成品を受け取るまでtrue。
        // 多重発注を防ぐために見る
        bool m_SceneLoadInFlight = false;
        // Renderスレッド専有。いまLoaderスレッドが読んでいるシーンの番号(m_SceneDisplayNamesの添字)。
        // 進捗表示にシーン名を出すために持つ ―― m_CurrentSceneIndexは読み込みが完了するまで
        // 旧シーンを指したままで、m_PendingSceneRequestは発注した時点で-1へ戻る
        size_t m_SceneLoadingIndex = 0;

        // シーン読み込みの進捗(読み終えたモデル数 / [Model]の総数)。
        //
        // 【なぜ要るか】読み込み中は旧シーンを先に手放すため画面にはUIとスカイボックスしか出ない
        // (UpdateSceneStreamingのコメント参照)。767モデルのシーンでは数十秒かかり、
        // m_SceneLoadInFlightのboolだけでは「進んでいる」と「固まった」を区別できない。
        //
        // 【atomicにする理由】書き手はLoaderスレッド(Assets::LoadSceneのコールバック)、
        // 読み手はRenderスレッド(UIManagerの進捗ウィンドウ)で、フレーム境界の受け渡しに
        // 乗らない唯一の値のため。表示だけに使うのでmemory_order_relaxedで足りる
        std::atomic<uint32_t> m_SceneLoadProgressLoaded{ 0 };
        std::atomic<uint32_t> m_SceneLoadProgressTotal{ 0 };

        // --- .ksceneのホットリロード -----------------------------------------------------
        //
        // 起動し直さずに.ksceneの変更を絵へ出すための仕組み。読み込み自体は上の非同期経路を
        // そのまま使い(「今のシーンをもう一度読む」だけ)、ここが持つのは「いつ発注するか」だけ。
        //
        // 【監視するのは実行ファイルの隣のファイル】エンジンが読むのは<exe>\Assets\Scenes\*.ksceneで、
        // リポジトリのScenes\*.ksceneからはKurenaiPacker --scene → Assets\Packed → xcopy の
        // 2ホップで届く。エンジンは自分が実際に読んだファイル(m_SceneFilePaths)だけを見る

        // 自動監視の有効/無効とリロード時のカメラ保持はm_Settings.Systemへ移した
        // (SceneAutoReloadEnabled / SceneReloadKeepsCamera)
        // 監視中の.ksceneの更新時刻(FILETIMEを64bitへ詰めたもの)。0は「まだ取得していない」
        uint64_t m_WatchedSceneWriteTime = 0;
        // 検証に失敗した更新時刻。同じ内容で警告ログを繰り返さないために覚えておく
        uint64_t m_SceneReloadRejectedWriteTime = 0;
        // 次に更新時刻を見る時刻。毎フレーム見る必要は無いので250msに1回へ間引く。
        // Render()のフレーム時間ではなく自前のsteady_clockで測るのは、この関数が
        // フレーム時間の更新より前に呼ばれる位置にあり、呼び出し順への依存を作らないため
        std::chrono::steady_clock::time_point m_NextSceneWatchTime{};

        // Render → Loader の要求。-1は「要求なし」
        std::mutex m_LoadRequestMutex;
        std::condition_variable m_LoadRequestCV;
        int m_LoadRequestSceneIndex = -1;
        bool m_StopLoaderThread = false;

        // Render → Loader の破棄依頼(RetiredAssetsのコメント参照)
        std::mutex m_RetiredAssetsMutex;
        std::vector<RetiredAssets> m_RetiredAssets;

        // Loader → Render の完成品
        std::mutex m_LoadedSceneMutex;
        std::unique_ptr<LoadedScene> m_LoadedScene;

        // Render → Update の初期カメラ・ウィンドウタイトル。
        // 毎フレームのロックを避けるため、まずatomicで有無を判定してから中身を取りにいく
        std::atomic<bool> m_AppliedScenePending{ false };
        std::mutex m_AppliedSceneMutex;
        // カメラを適用するか。ホットリロードで「現在のカメラを保持する」を選んでいるときだけ
        // falseになる。falseでもウィンドウタイトルの更新は行うため、引き渡し自体は毎回発生する
        bool m_AppliedSceneApplyCamera = true;
        Core::Camera m_AppliedSceneCamera;
        std::wstring m_AppliedSceneTitle;

        // Loaderスレッド専有。「今どのスカイボックスを読み込み済みか」の真実。
        // スカイボックスを読むのがこのスレッドだけなので、ここで持つのが最も素直になる
        std::wstring m_LoaderSkyboxPath;
        // 水面法線マップ版。m_LoaderSkyboxPathと同じ扱いで、空文字列なら
        // フラット法線フォールバックを読み込み済みであることを表す
        std::wstring m_LoaderWaterNormalMapPath;

        // DiscoverScenesが起動時に一度だけ列挙する.ksceneの一覧。要素の並びがImGuiのシーン
        // 一覧・LoadSceneのインデックスに対応する(ファイル名の昇順)
        std::vector<std::wstring> m_SceneFilePaths;
        std::vector<std::wstring> m_SceneDisplayNames;

        // ApplyLoadedSceneがm_Scene.Lights(SceneLoaderが各ModelInstanceのModel::Lightsをワールド空間へ
        // 変換し、.kscene自身の[Light]セクションのライトと合成済みのシーン全体のライト一覧)から
        // コピーし、以降ImGui(Lightingパネル)が編集する。アセット由来のデータとユーザー編集を
        // 分離するため(シーンを再読み込みすればアセット既定値に戻る)。
        // m_Sceneと同じくRenderスレッド専有のためロックは不要
        std::vector<Assets::Light> m_Lights;
        int m_SelectedLightIndex = -1;
        // 実際にライト強度へ事前乗算される「実効プリ露出」。m_Settings.PostProcess.SceneExposureEV100(ユーザー設定)に
        // 時刻由来のバイアスを足したもので、Renderスレッドのみが読み書きする。
        //
        // 【なぜ可変にする必要があるか】
        // プリ露出をEV100=15固定のままだと夜がfp16でつぶれる。満月の照度は0.25lxで、
        // 反射率0.2の面の輝度は 0.25*0.2/π = 0.016 cd/m^2。これに ComputeExposure(15)=2.54e-5 を
        // 掛けると 4.0e-7 となり、SceneColor(R16G16B16A16_Float、最小正規化数6.1e-5)の
        // 非正規化域へ落ちて情報が失われる。AutoExposure.hlsl も輝度1e-6未満の画素は
        // ヒストグラムに数えないため、露出計にも乗らず復元できない。
        //
        // M7で導入したプリ露出方式は Tonemap・Bloom・AutoExposure がすべて同じ値を受け取って
        // 割り戻す構造になっているため、**フレーム単位で変えても最終的な絵は変わらない**。
        // その性質をそのまま利用して、バッファの数値レンジだけを健全に保つ
        float m_EffectiveExposureEV100 = 15.0f;
        // 実効プリ露出が初期化済みか(初回フレームは平滑化せず即座に合わせる)。
        // **シーン読み込み時にLoadSceneがfalseへ戻す**。シーンをまたぐと時刻が入れ替わって
        // 実効プリ露出が最大18段跳ぶが、そこを平滑化しても得られるものが無い(プリ露出は
        // Tonemap側で割り戻されるため過渡の絵には現れない)一方で、追従の途中の値で焼かれた
        // 資産(反射プローブ)が残ってしまう。切り替えは平滑化せず即座に合わせるのが正しい
        bool m_EffectiveExposureInitialized = false;

        std::chrono::steady_clock::time_point m_LastFrameTime;

        // Update(メインスレッド)とRender(描画専用スレッド)を並列化するためのハンドオフ機構。
        // キュー深度1(バッファ1面)で、Updateが1フレーム分書き込むたびにRenderが取り込んでから
        // 重いGPU発行に入るため、UpdateスレッドはRenderの実際の描画時間とは並行して次フレームを
        // 計算できる(=Update(N+1)とRender(N)が並列に進む)
        std::thread m_RenderThread;
        std::mutex m_FrameStateMutex;
        std::condition_variable m_FrameStateCV;
        FrameState m_FrameState;
        bool m_FrameStateReady = false;
        bool m_FrameStateTaken = true;
        bool m_StopRenderThread = false;
        // Renderスレッド側のフレーム間隔計測用(時刻自動進行・FPS計測に使う。Renderスレッドのみが読み書きする)
        std::chrono::steady_clock::time_point m_LastRenderFrameTime;
        // 直前のRenderフレームの経過時間[秒]。自動露出の時間方向の順応に使う。
        // RenderThreadMainが書き、Render()が読む。どちらもRenderスレッドなので追加の排他は不要
        // (m_Settings.Sky.TimeOfDayと同じ扱い)
        float m_RenderDeltaTime = 0.0f;
        float m_FixedTimeStep = 0.0f;

        // 性能ログ(LogFrameStatsIfDue)の有効/無効はm_Settings.System.FrameStatsLoggingEnabledへ移した。
        // 集計状態はすべてRenderスレッドのみが読み書きするため追加の排他制御は不要
        std::chrono::steady_clock::time_point m_FrameStatsWindowStart;
        uint32_t m_FrameStatsFrameCount = 0;
        // 集計期間中の合計。平均を出すためにフレーム数で割る
        double m_FrameStatsCPUTimeSumMs = 0.0;
        double m_FrameStatsGPUTimeSumMs = 0.0;
        double m_FrameStatsGPUWaitSumMs = 0.0;
        // 平均だけではスパイクが埋もれるため、集計期間中のフレーム間隔の最悪値も残す
        float m_FrameStatsWorstFrameTimeMs = 0.0f;

        // モデル単位フラスタムカリングの統計(1フレーム分)。フレーム先頭でリセットし、
        // LogFrameStatsIfDueが集計期間の合計として出す。
        //
        // 【何のために出すか】カリングは「効いていない」と「間引きすぎて物が消えた」の
        // どちらも絵からは判別しにくい。判定式が常にtrueを返していても既存シーンの絵は
        // 一致してしまうため、間引いた数が0でないことを数値で確かめられるようにしておく
        uint32_t m_FrustumCullTested = 0;
        uint32_t m_FrustumCullCulled = 0;

        // --- モデルLOD(.ksceneの[Model]LODPath / LODDistance) --------------------------------
        //
        // インスタンスごとの「いま使っている段」と、切り替え中のクロスディザの進み具合。
        // m_Scene.Instancesと同じ添字で並び、ApplyLoadedSceneで作り直す。
        //
        // 【Assets::Sceneではなくエンジン側に持つ理由】これは読み込んだデータではなく
        // カメラ位置から毎フレーム決まる実行時の状態で、Loaderスレッドが作るSceneに
        // 混ぜると「シーンの内容」と「今の見え方」の境界が曖昧になる
        struct InstanceLODState
        {
            uint32_t CurrentLOD = 0;   // 0 = ModelInstance::Model、1以上は LODModels[n-1]
            uint32_t PreviousLOD = 0;  // フェード中の切り替え元
            float FadeT = 1.0f;        // 1.0でフェード完了。0→1へ進み、その間だけ2段を重ねる
        };
        std::vector<InstanceLODState> m_InstanceLODStates;
    public:
        // RenderingPanel(モデルLOD段ごとの内訳表示)向け。InstanceLODStateがこのクラスの
        // 入れ子型のため、UIパネル向けのアクセサ一覧とは別にここで公開する
        const std::vector<InstanceLODState>& GetInstanceLODStates() const { return m_InstanceLODStates; }
    private:
        // 段の切り替えにかける秒数とヒステリシス幅はm_Settings.Geometry.LODFadeDuration /
        // LODHysteresisへ移した
        // 統計。1フレームあたりの段の切り替え回数と、そのフレームでフェード中のインスタンス数。
        // 【0なら一度も切り替わっていない】LODが効いているかはここでしか分からない
        // (フェード中のインスタンス数はm_RenderStats.LODFadingCountへ出す。UIが読む完成値のため)
        uint32_t m_LODSwitchCount = 0;
        uint64_t m_FrameStatsLODSwitchSum = 0;
        // 【瞬間値ではなく積算する】m_RenderStats.LODFadingCountをそのままログへ出していたときは、
        // 集計期間(1秒)の最終フレームの値だけを見ていた。既定のフェードは0.25秒なので
        // 構造的にほぼ必ず取りこぼし、「フェードが一度も実行されていない」のか
        // 「実行されたが見ていないだけ」なのかを区別できなかった(実際に取りこぼした)。
        // 期間中の「フェード中インスタンス×フレーム」を足し込めば、0.25秒のフェードでも
        // 14フレームぶんとして必ず現れる
        uint64_t m_FrameStatsLODFadingSum = 0;
        // カメラ位置から各インスタンスの段を決め、フェードを進める。
        // レンダーグラフの構築より前に1フレーム1回だけ呼ぶこと ―― パスごとに測り直すと
        // 深度プリパスとG-Bufferが違う段を選び、画面に穴が開く
        void UpdateModelLOD(const DirectX::XMFLOAT3& cameraPosition, float deltaSeconds);
        // instanceIndex番目のインスタンスについて、このフレームで描く段を返す。
        // フェード中は2件(切り替え先と元)、そうでなければ1件。DitherFadeも一緒に返す
        struct LODDraw
        {
            const Assets::Model* Model = nullptr;
            float DitherFade = 1.0f;
        };
        // 戻り値の件数。fadingなら2、それ以外は1
        uint32_t GetLODDraws(size_t instanceIndex, LODDraw (&outDraws)[2]) const;
        // シャドウ・反射プローブ・DDGI用。常に最も粗い段を返す(影と間接光はテクスチャを読まない)
        const Assets::Model* GetCoarsestLOD(const Assets::ModelInstance& instance) const;

        // DDGIから自発光を抜くか。**判定を1か所に置くこと** ―― ラスタ経路(ObjectConstantsの
        // 倍率)とレイトレ経路(DDGITraceConstants.Params1.w)で条件がずれると、
        // 環境によって二重計上の有無が変わる。しかも絵は両方それらしく出る
        bool ShouldSuppressEmissiveForGI() const;

        // --- モデルのストリーミング(.ksceneの[Scene]StreamingDistance) ----------------------
        //
        // カメラ位置から「読むべきなのにまだ無いモデル」を選んでLoaderスレッドへ発注し、
        // 出来上がったものを受け取ってインスタンスへ差し込む。
        // レンダーグラフの構築より前に1フレーム1回だけ呼ぶこと。
        //
        // 【まず読み込みだけ】破棄はまだ行わない。絵が出ることを確かめてから、
        // kFrameCountフレーム遅延させる解放キューを通して足す
        void UpdateModelStreaming(const DirectX::XMFLOAT3& cameraPosition);
        // 常駐が変わったことを記録する。実際の作り直しは静かになってから
        void RequestRaytracingRebuild();
        // 出来上がったRaytracingSceneの差し替えと、静かになった後の発注。
        // UpdateModelStreamingの後にフレーム1回だけ呼ぶ
        void UpdateRaytracingRebuild();

        // Render → Loader の読み込み発注。m_LoadRequestMutexで保護し、
        // シーン切り替えと同じ条件変数で起こす(専用スレッドを増やさない)
        struct StreamingRequest
        {
            std::wstring Path;
            uint64_t Generation = 0;
        };
        std::vector<StreamingRequest> m_StreamingRequests;

        // Loader → Render の完成品
        std::mutex m_StreamingLoadedMutex;
        struct StreamingLoaded
        {
            std::wstring Path;
            std::shared_ptr<Assets::Model> Model;
            uint64_t Generation = 0;
        };
        std::vector<StreamingLoaded> m_StreamingLoaded;

        // 発注済みで、まだ受け取っていないパス(同じものを何度も発注しないため)
        std::unordered_set<std::wstring> m_StreamingInFlight;

        // 【シーンに紐づく世代番号】シーンを切り替えると進める。古い世代の完成品は捨てる。
        // これが無いと、切り替え前のシーンのモデルが新しいシーンのインスタンスへ差し込まれる
        uint64_t m_StreamingGeneration = 0;

        // ストリーミングで読むモデルが使う1x1フォールバックの共有プール。
        //
        // 【Assets::Scene::SharedTexturesを使ってはいけない】あちらはシーンが所有しており、
        // シーン切り替えのときRenderスレッドがstd::moveでRetiredAssetsへ移す。
        // Loaderスレッドが読み込み中にそれが起きるとプールのアドレスが変わり、解放済みを指す。
        // こちらはLoaderスレッドだけが作り・使い・捨てるので、その競合が起きない
        std::unique_ptr<Assets::SharedTexturePool> m_StreamingTexturePool;

        // 破棄を寝かせるフレーム数。
        //
        // 【なぜ即座に捨ててはいけないか】CPUはGPUの完了を待たずに次フレームの記録を始めるため
        // (DX12は kFrameCount = 2 フレーム先行する)、いま画面から外れたモデルの頂点バッファを
        // その場で解放すると、GPUがまだ読んでいる最中のリソースを消すことになる。
        // シーン切り替えの経路は WaitForGPUIdle でこれを避けているが(RetiredAssetsのコメント)、
        // ストリーミングの破棄は毎フレーム起こりうるので待つわけにいかない。
        // 代わりにこの数だけ寝かせてから解放する。DX12の先行分2に1フレームの余裕を足してある
        static constexpr uint32_t kStreamingReleaseDelayFrames = 3;

        // 破棄待ち。ここに積まれている間はshared_ptrが実体を生かし続ける。
        // 0になったらLoaderスレッドへ渡す(解放も確保と同じスレッドで行うため)
        struct PendingModelRelease
        {
            std::shared_ptr<Assets::Model> Model;
            uint32_t FramesRemaining = 0;
        };
        std::vector<PendingModelRelease> m_StreamingPendingRelease;

        // Render → Loader の破棄依頼。受け取った側はvectorを空にするだけでよい
        // (shared_ptrの最後の参照が消えてデストラクタが走る)
        std::mutex m_StreamingReleaseMutex;
        std::vector<std::shared_ptr<Assets::Model>> m_StreamingRelease;

        // --- レイトレーシングを常駐の増減へ追随させる ----------------------------------------
        //
        // 常駐が変わるとBLAS/TLASと統合バッファが実態と食い違う。作り直して追随させる。
        // 最後の増減からこの時間だけ静かなら作り直す(走行中は毎フレーム変わりうるため)
        bool m_RaytracingRebuildPending = false;
        std::chrono::steady_clock::time_point m_RaytracingRebuildAfter{};
        static constexpr float kRaytracingRebuildQuietSeconds = 0.5f;
        std::mutex m_RaytracingRebuiltMutex;
        std::unique_ptr<Assets::RaytracingScene> m_RaytracingRebuilt;
        uint64_t m_RaytracingRebuiltGeneration = 0;
        bool m_RaytracingRebuildRequested = false;   // m_LoadRequestMutexで保護
        // 再構築が走っている間はtrue。立っている間はRenderスレッド側の差し込みと破棄を見送る。
        // Loaderスレッドが m_Scene を走査している最中に書き換えると走査中のコンテナが変わるため
        std::atomic<bool> m_RaytracingRebuildInFlight{ false };
        // 差し替えた旧RaytracingSceneの破棄待ち。モデルと同じくフレームを寝かせる。
        //
        // 【Renderスレッドで破棄してはいけない】RaytracingSceneが持つBLAS/TLASと統合バッファの
        // ディスクリプタは、ロックを持たないアセット用ヒープ(DX12Device::GetAssetSrvCpuHeap)
        // から取られている。Loaderスレッドがストリーミングで確保している最中にRenderスレッドが
        // 解放するとフリーリストが壊れる。寝かせたあとはLoaderスレッドへ渡すこと
        struct PendingRaytracingRelease
        {
            std::unique_ptr<Assets::RaytracingScene> Scene;
            uint32_t FramesRemaining = 0;
        };
        std::vector<PendingRaytracingRelease> m_RaytracingPendingRelease;
        std::mutex m_RaytracingReleaseMutex;
        std::vector<std::unique_ptr<Assets::RaytracingScene>> m_RaytracingRelease;
        // 統計。0なら一度も作り直していない
        uint64_t m_RaytracingRebuildCount = 0;
        double m_RaytracingRebuildLastMs = 0.0;

        // 統計。【いずれも累計】瞬間値だと短い出来事を取りこぼす(47.9の失敗と同じ)
        uint64_t m_StreamingLoadedTotal = 0;
        uint64_t m_StreamingEvictedTotal = 0;
        uint32_t m_StreamingResidentCount = 0;
        uint32_t m_StreamingTargetCount = 0;
        // 集計期間中の合計(平均はフレーム数で割って出す)
        uint64_t m_FrameStatsCullTestedSum = 0;
        uint64_t m_FrameStatsCullCulledSum = 0;
        // メッシュレット単位のカリング(増幅シェーダー)の集計。上のCPU側とは別の行に出す ――
        // 粒度(モデル単位 / メッシュレット単位)も判定の種類も違うので、混ぜると読めなくなる。
        // 読み戻せなかったフレームは足さないため、フレーム数も別に数える
        uint64_t m_FrameStatsMeshletTestedSum = 0;
        uint64_t m_FrameStatsMeshletFrustumCulledSum = 0;
        uint64_t m_FrameStatsMeshletOcclusionCulledSum = 0;
        uint32_t m_FrameStatsMeshletSampleCount = 0;

        // パス別のドローコール数の集計(1フレーム分)。フラスタムカリングの統計と同じく
        // フレーム先頭でリセットし、LogFrameStatsIfDueが集計期間の平均として出す。
        // 数える本体は Passes::GeometryPasses(G-Buffer・深度プリパス)と
        // Passes::ShadowPasses が持ち、ここはその和を積むだけ。
        //
        // 【なぜパスごとに分けるのか】フラスタムカリングの統計が全パス合計になっていて、
        // どのパスが何回描いているのかが分からない。ドローコールの削減はこのエンジンで
        // これから何度も測る対象(メッシュレットによる1モデル1ドロー化、GPU駆動描画)で、
        // 「G-Bufferは減ったがシャドウは減っていない」のような片手落ちは
        // パス別に見ないと気づけない。
        //
        // 直前に描き終えたフレームの値はm_RenderStats.DrawCalls*LastFrameへ出す。
        // **UIパネルはこちらを読むこと** ―― パス群のカウンタはフレーム先頭で0に戻るため、
        // Renderの外で描かれるUIからは常に0に見える
        uint64_t m_FrameStatsDrawCallsGBufferSum = 0;
        uint64_t m_FrameStatsDrawCallsShadowSum = 0;
        uint64_t m_FrameStatsDrawCallsDepthPrepassSum = 0;

        // メッシュ単位フラスタムカリングの統計(1フレーム分)。上のモデル単位とまったく同じ扱い。
        //
        // 【絶対に上のカウンタと混ぜない】分母も意味も違う。モデル単位は
        // 「シーンのインスタンス数」が分母で、メッシュ単位は「モデル単位を通過した
        // インスタンスのメッシュ数の合計」が分母になる。合算すると、どちらが効いているのか
        // ―― あるいは片方が一度も実行されていないのか ―― が読めなくなる。
        //
        // 【効くシーンが逆】モデル単位は.kmodelを多数並べるシーン(PLATEAUの671タイル)で効き、
        // 1モデルに数千メッシュを持つアセット(Emerald Square、Bistro)では1つも間引けない。
        // メッシュ単位はその逆で、後者でしか値が動かない
        uint32_t m_MeshCullTested = 0;
        uint32_t m_MeshCullCulled = 0;

        // 完成した最後のフレームの値はm_RenderStats.FrustumCullTestedLastFrame等へ出す。
        // UIパネルはRenderの外で描かれるため、上のカウンタをそのまま読むと
        // リセット直後の0になる(ドローコール数のm_RenderStats.DrawCalls*LastFrameと同じ)
        uint64_t m_FrameStatsMeshCullTestedSum = 0;
        uint64_t m_FrameStatsMeshCullCulledSum = 0;

        bool m_MouseCaptured = false;
        POINT m_MouseCaptureCenter{};

        // F1キーでImGuiの表示/非表示を切り替える(WasKeyPressedがエッジ検出を内蔵しているため、
        // 前フレームの押下状態を保持するメンバは不要)
        bool m_ImGuiVisible = true;
    };
}

#pragma warning(pop)
