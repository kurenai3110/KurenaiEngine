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

#include "DroneShow.h"
#include "EngineDefaults.h"
#include "KurenaiEngineBase.h"
#include "KurenaiTypes.h"

#include "Assets/RaytracingScene.h"
#include "Assets/Scene.h"
#include "Assets/TextureStreaming.h"
#include "Core/Camera.h"
#include "Core/CPUProfiler.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::UI
{
    class UIManager;
    class ScenePanel;
    class RenderingPanel;
    class PostProcessPanel;
    class DebugViewPanel;
    class LightingPanel;
    class SystemPanel;
    class ProfilerPanel;
    class ReflectionProbePanel;
    class StreamingPanel;
}

namespace Kurenai
{
    // メッシュレットLODの段を選ぶために、フレーム内の全パスへ配る値(Stage 6)。
    //
    // 【主カメラの値である】シャドウと深度プリパスは G-Buffer とまったく同じ増幅シェーダーを
    // 使うが、そちらのViewProjは光源やカスケードのものに差し替わっている。各パスのカメラで
    // 段を選ぶと、影を落とす形と本体の形が違う段になり、影の縁が本体からずれる。
    // 段の選択は主カメラだけで決め、全パスで同じ値を配る
    struct MeshletLODFrameConstants
    {
        DirectX::XMFLOAT3 CameraPos{ 0.0f, 0.0f, 0.0f };
        // 距離1メートルにある長さ1メートルが何ピクセルになるか
        // (= 射影行列の縦方向の拡大率 × レンダーターゲットの高さ / 2)
        float PixelScale = 0.0f;
        // しきい値の倍率。段を落とす投影直径は
        // Quality * sqrt(4 * モデルのLOD0三角形数 / π) [画素]。
        // 0以下なら段の選択を行わない(A/B比較のOFF側)
        float Quality = 0.0f;
        // 0以上ならその段に固定する(対照実験用)。負なら自動
        int32_t Forced = -1;
        // メッシュレットの色分け表示を「塊ごと」ではなく「段ごと」にするか
        bool DebugColorByLOD = false;
    };

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
        // しきい値の効き方をA/Bで測るための起動オプション用。根拠はm_DDGIBackfaceThresholdを参照
        void SetDDGIBackfaceThreshold(float threshold);

        // DDGIのクリップマップLODの段数と追従の有無を、読み込んだ`.kscene`の指定より優先して上書きする。
        // 段数を振って効果を測るための起動オプション用。アトラスを確保し直すので、
        // **フレームの記録が始まる前(Run()より前)にだけ呼ぶこと**
        void OverrideDDGILOD(uint32_t lodCount, bool followCamera);

        // MegaLightsの手法と、1灯あたりに撃つ影レイの本数を起動時に上書きする。
        // mode は KurenaiEngine3D::MegaLightsMode の値(0=なし, 1=参照実装)。
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
        // UI(PostProcessPanel)は m_AutoExposureEnabled を直接触るが、起動オプションから
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

        void SetPerfDump(const wchar_t* path, int frames);

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
        static constexpr uint32_t kCascadeCount = 4;
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

    private:
        // UIパネル群(Source/Engine/UI/)は、m_SSAORadius等のパラメータメンバをImGuiウィジェットへ
        // アドレスで直接渡すためprivateへアクセスする必要がある。
        // パラメータを専用の構造体へ切り出して物理的に移動させる案も検討したが、Render()内の
        // 参照が約200箇所あり、書き換えの過程で1箇所間違えてもコンパイルが通ってしまい静かに
        // 壊れるため採らない。
        // friendにしても「UIから触ってよいのはパラメータ用メンバだけで、RHIリソース
        // (m_GBufferAlbedo等)には触らない」という規約は各パネルの実装側で守ること
        friend class UI::UIManager;
        friend class UI::ScenePanel;
        friend class UI::RenderingPanel;
        friend class UI::PostProcessPanel;
        friend class UI::DebugViewPanel;
        friend class UI::LightingPanel;
        friend class UI::SystemPanel;
        friend class UI::ProfilerPanel;
        friend class UI::ReflectionProbePanel;
        friend class UI::StreamingPanel;

        // UpdateスレッドからRenderスレッドへ、1フレーム分のカメラ・ImGui表示状態を引き渡すための
        // スナップショット。m_TimeOfDay等それ以外の状態はRenderスレッド側のみが読み書きするため
        // ここには含めない(RenderThreadMain参照)
        struct FrameState
        {
            Core::Camera Camera;
            bool ImGuiVisible = true;
        };

        void CreateSceneResources();
        // 中間バッファの精度構成(m_BufferPrecision)によって変わるフォーマット。
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
        // 平面反射専用のレンダーターゲット2枚(m_PlanarReflectionColor/Depth)を、
        // 反射解像度(レンダー解像度 × m_PlanarReflectionResolutionScale)で作り直す。
        // メインのCreateRenderTargetsとは独立に呼べる(Legacy8bitフォールバックの対象外。
        // このバッファは常にHDR固定フォーマットのため)。呼び出し箇所はCreateRenderTargetsと
        // 同じ2か所(Initialize直後、Render()の解像度変更ハンドリング)
        void CreatePlanarReflectionTargets();
        // 自前ソフトウェアラスタライザパス(46章)の本体。クリア2回とディスパッチ3回を積む。
        // 呼ぶのはRender()のレンダーグラフ登録からのみで、
        // m_SoftwareRasterEnabled && m_SoftwareRasterAvailable のときだけ登録される。
        // viewProjはGBufferパスが使ったものとまったく同じ行列(ジッターを含む)を渡すこと ――
        // 別の行列で描くと深度の比較が意味を失う。
        // sunDirectionは光が進む向き(FrameConstants::LightDirectionと同じ規約)
        void ExecuteSoftwareRasterPass(
            RHI::IRHICommandList* cmd,
            const DirectX::XMMATRIX& viewProj,
            const DirectX::XMFLOAT3& sunDirection);
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
        // このフレームでMegaLightsパスを実行するか。上と同じ作法で1か所に集約している。
        // これがfalseのときDirectLighting.hlslは従来のライトループへ戻る ―― 「パスを積むか」と
        // 「ライトループを止めるか」がずれると、ライトが二重に加算されるか、逆に全部消える
        bool ShouldRunMegaLights() const;
        // いま1画素あたり何本の標本(リザーバ)を引くか。**バッファの確保も定数バッファも
        // 必ずこの関数を通すこと** ―― 2か所で別々に計算すると静かに食い違う。
        // 手法3以外は常に1(手法2の再利用が1画素1リザーバを前提にしているため)
        int32_t MegaLightsSamplesPerPixel() const;
        // このメッシュをメッシュシェーダー経路で描くか。上のShouldRun*と同じく、
        // 「どのPSOを束ねるか」と「DispatchMeshとDrawIndexedのどちらを積むか」の判断が
        // ずれると即座に破綻するため、判定を1か所に集約する。
        // isWaterがtrueのメッシュは常にfalse(理由は実装のコメント参照)
        bool ShouldUseMeshletPath(const Assets::Model& model, const Assets::Mesh& mesh, bool isWater) const;
        // このインスタンスを「1回のDispatchMeshでモデル全体」の経路で描けるか。
        // 描けない場合は従来どおりメッシュ単位のループで描く
        // modelは「このパスが描く段」。モデルLODが入ったのでinstance.Model(最も詳細な段)とは
        // 限らず、シャドウは最も粗い段、G-Buffer/プリパスは選ばれた段を渡す
        bool ShouldUseModelMeshletPath(const Assets::ModelInstance& instance, const Assets::Model& model) const;
        // モデル単位のGPUカリングが使うバッファを、候補数に足りる大きさで用意する。
        // シーン切り替えとストリーミングでインスタンス数が変わるため、足りなくなったときだけ作り直す
        void EnsureModelCullCapacity(uint32_t candidateCount);
        // 間接描画で1区画ぶんを発行する。区画が空、またはPSOが無ければ何もせずfalseを返す。
        // currentPipelineStateは呼び出し側のPSOキャッシュで、切り替えたら書き換える
        bool IssueModelCullIndirect(
            RHI::IRHICommandList* cmd, uint32_t region, RHI::IRHIPipelineState* pipelineState,
            RHI::IRHIPipelineState*& currentPipelineState);
        // このフレームでライティングパス等が読むべきAO/GIバッファ(ブラー後 / ブラー前の生値)。
        // AO無効時はm_AODisabledTexture、Raytracedを選んでいても実行できないフレームはSSAOのもの
        RHI::IRHITexture* GetActiveAOTexture() const;
        RHI::IRHITexture* GetActiveAORawTexture() const;
        // このフレームでHDRのシーン色として後段(自動露出・ブルーム・トーンマップ)が読むべき
        // テクスチャを返す。反射パスを実行したならその出力、していなければm_SceneColor
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
            std::unique_ptr<RHI::IRHITexture> SkyboxTexture;
            // 水面法線マップ版。SkyboxTextureとまったく同じ扱い
            std::unique_ptr<RHI::IRHITexture> WaterNormalMapTexture;
        };

        // Loaderスレッドが作り、Renderスレッドが受け取る「差し替えられる状態まで仕上がったシーン」
        struct LoadedScene
        {
            Assets::Scene Scene;
            Assets::RaytracingScene RaytracingScene;
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

        // シーン切り替えを要求する(ScenePanel = Renderスレッドから呼ばれる)。
        // 実際の読み込みはLoaderスレッドが行うため即座に戻る。
        // 読み込み中に再度要求された場合は新しい要求で上書きされる(最後の要求が勝つ)
        void RequestSceneLoad(size_t sceneIndex);
        // 内部レンダー解像度の変更を要求する(SystemPanel = Renderスレッドから呼ばれる)。
        // レンダーターゲットの作り直しはGPUがそれらを参照していない状態で行う必要があるため、
        // ここでは要求を記録するだけにしてRender()の先頭でまとめて反映する
        void RequestRenderResolution(uint32_t width, uint32_t height);
        // 平面反射の反射解像度の倍率変更を要求する(RenderingPanel = Renderスレッドから呼ばれる)。
        // RequestRenderResolutionと同じ方式(要求を記録するだけにしてRender()の先頭でまとめて反映)
        void RequestPlanarReflectionResolutionScale(float scale);
        // Renderスレッドがフレーム先頭で呼ぶ。保留中の切り替え要求の発注と、
        // 出来上がったシーンの取り込みを行う
        void UpdateSceneStreaming();
        // .ksceneの更新時刻を見て、変わっていれば再読み込みを要求する。
        // UpdateSceneStreamingの先頭から呼ぶ。m_SceneAutoReloadEnabledがfalseなら何もしない
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

        // SSAO/SSILの半径・厚みとSSRの距離・厚みを、現在のシーンの対角長から決め直す。
        // これらは固定の既定値を持たないため、UIの「既定値に戻す」ではなくこれを呼ぶ
        // (シーン読み込み時はApplyLoadedSceneから呼ばれる)
        void ResetSceneDependentParams();
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
        // このフレームの計測値を集計し、集計期間(FrameStatsLogIntervalSeconds)ぶん溜まっていれば
        // 1行にまとめてログへ出す。Renderスレッドからフレームごとに呼ぶ
        void LogFrameStatsIfDue(float renderDeltaTime);
        // ProfilerPanel用。m_DeviceはKurenaiEngineBaseのprotectedメンバであり、派生クラスの
        // friendから触れるかどうかはC++の規則の解釈が分かれるため、ここで明示的に橋渡しする
        float GetLastFrameGPUWaitTimeMs() const;
        // SystemPanelの表示用。m_Windowも同様の理由で橋渡しする。
        // Windowsのディスプレイ設定で指定されている拡大率(UIの拡大率もこれに追従する)
        float GetMonitorDpiScale() const;
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
        void RequestGraphicsAPIChange(GraphicsAPI api);

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

        // --- 超解像(FSR1相当のEASU+RCAS。41.23節) ---
        //
        // 【m_RenderWidth/m_RenderHeightの意味は変えていない】ここで足したのは
        // 「出力解像度」という一段外側の概念だけで、上のm_RenderWidth/m_RenderHeightは
        // 従来どおり「G-Buffer以降すべての中間バッファの解像度」のままである。
        // 超解像が有効なとき、出力解像度を品質モードの倍率で割った値を
        // RequestRenderResolution()へ流し込む、という関係になっている。
        // こうしてあるのは、Render()の各所に散らばるm_RenderWidth/m_RenderHeightの参照を
        // 「レンダー解像度」と「出力解像度」へ仕分ける必要をなくすため。
        // 追加のパスはTonemapの後ろに2本足すだけで済んでいる
        enum class UpscaleQualityMode
        {
            UltraQuality, // 1.3倍
            Quality,      // 1.5倍
            Balanced,     // 1.7倍
            Performance,  // 2.0倍
        };
        // 品質モードの既定値。EngineDefaults.hは列挙を知らない(<cstdint>しか取り込まない)ため
        // ここに置くが、「メンバの初期化子とUIの『既定値に戻す』が同じ出所を見る」という
        // EngineDefaults.hの原則自体は守る
        static constexpr UpscaleQualityMode kDefaultUpscaleQualityMode = UpscaleQualityMode::Quality;

        bool m_UpscaleEnabled = Defaults::UpscaleEnabled;
        UpscaleQualityMode m_UpscaleQualityMode = kDefaultUpscaleQualityMode;
        // RCASのシャープネス(0〜1)。UIの見た目の値で、シェーダーへ渡す前に
        // ComputeRcasSharpnessScale()で参照実装のスケールへ変換する
        float m_UpscaleSharpness = Defaults::UpscaleSharpness;
        // 超解像が有効なときの出力解像度。無効なときは内部レンダー解像度そのものになる。
        // ウィンドウサイズには追従しない(追従させるとドラッグ中に何度も
        // レンダーターゲットを作り直すことになる。SystemPanelの「ウィンドウサイズに合わせる」参照)
        uint32_t m_UpscaleOutputWidth = Defaults::RenderWidth;
        uint32_t m_UpscaleOutputHeight = Defaults::RenderHeight;
        // 出力解像度用テクスチャの作り直し要求。m_RenderResolutionDirtyとまったく同じ扱いで、
        // Render()の先頭のWaitForGPUIdle()を挟んだ位置で処理する
        bool m_UpscaleTargetsDirty = false;
        // 実際に確保済みの出力解像度用テクスチャのサイズ。0なら未確保(超解像が無効)
        uint32_t m_UpscaleTargetWidth = 0;
        uint32_t m_UpscaleTargetHeight = 0;

        // 品質モードの倍率(1.3 / 1.5 / 1.7 / 2.0)
        static float GetUpscaleRatio(UpscaleQualityMode mode);
        // 出力解像度と品質モードから内部レンダー解像度を求める。
        // 8の倍数へ切り捨てるのは、LightCullのタイル・Hi-Zのミップ連鎖・Bloomのピラミッド・
        // SkyCloud/DDGIResolveの1/2解像度がいずれも2の冪で割っていくため。下限は320x180
        static void ComputeUpscaleRenderResolution(
            uint32_t outputWidth, uint32_t outputHeight, UpscaleQualityMode mode,
            uint32_t& outRenderWidth, uint32_t& outRenderHeight);
        // UIのシャープネス(0〜1)を、シェーダーへ渡す線形スケールへ変換する。
        // FSR1のsharpnessは「何ストップ弱めるか」で0が最大なので、2^(-2*(1-v)) とする
        static float ComputeRcasSharpnessScale(float sharpness);

        // 超解像の設定をまとめて要求する(SystemPanel = Renderスレッドから呼ばれる)。
        // 内部でRequestRenderResolution()を呼ぶだけで、レンダーターゲットの作り直しはしない
        void RequestUpscaleSettings(
            bool enabled, UpscaleQualityMode mode, uint32_t outputWidth, uint32_t outputHeight);
        // 出力解像度のテクスチャを作り直す。GPUがそれらを参照していない状態で呼ぶこと
        void CreateUpscaleTargets(uint32_t width, uint32_t height);
        // このフレームで超解像パスを走らせるか(有効かつテクスチャが確保済み)
        bool IsUpscaleActive() const;

        // 中間バッファの精度構成。HDRが本来採用したい構成で、Legacy8bitは
        // 「中間バッファはすべてR8G8B8A8_UNorm」にする比較用の経路。
        //
        // 精度改善の効果を主観ではなく実測で比較できるようにするために残している。
        // UNorm8は刻みが絶対値1/255=0.392%で固定なのに対し、half floatは仮数10bitで
        // 相対2^-11=0.049%が一定のため、両者の相対精度比は格納値vに対して8/vになる
        // (v=0.1で80倍、v=0.02で401倍)。暗い間接光ほど差が開く
        // (詳細と各バッファの根拠はdocs/Architecture.html)
        enum class BufferPrecision
        {
            HDR,
            Legacy8bit,
        };
        BufferPrecision m_BufferPrecision = BufferPrecision::HDR;
        // ImGuiでBufferPrecisionが変更されたことをRender()へ伝えるフラグ。レンダーターゲットの
        // 作り直しはGPUがそれらを参照していない状態で行う必要があるため、UI関数の中では実行せず
        // Render()の先頭(RenderGraphの構築より前)でm_Device->WaitForGPUIdle()を挟んで処理する
        bool m_BufferPrecisionDirty = false;

        // ジオメトリパス(G-Buffer書き込み)
        std::unique_ptr<RHI::IRHIShader> m_GBufferVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_GBufferPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferPipelineState;
        // ミラーリング(Worldの行列式が負)されたインスタンス用。表裏判定を入れ替えただけで
        // 他は上と同一(ModelInstance::IsMirrored、docs/Architecture.html 10.2節)
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferPipelineStateMirrored;
        // 水面(ModelInstance::IsWater)専用のピクセルシェーダー・PSO(水面マテリアル基盤)。
        // 頂点シェーダーはm_GBufferVertexShaderをそのまま共有する(Water.hlslもGBufferCommon.hlsli
        // 由来の同じVSMainを使うため)。ミラーリングとの組み合わせ(4値)はGBufferパスの
        // bindPipelineStateラムダが選ぶ
        std::unique_ptr<RHI::IRHIShader> m_GBufferWaterPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferWaterPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferWaterPipelineStateMirrored;

        // --- 深度プリパス(41.22節。Shaders/3D/DepthPrepass.hlsl) ------------------------
        //
        // G-Bufferを描く前に不透明ジオメトリの深度だけを埋め、G-Buffer側の深度比較を
        // GREATER_EQUALにして最前面の断片だけを通す。隠れる画素のピクセルシェーダー
        // (6テクスチャ + 6レンダーターゲット書き込み)がまるごと省ける。
        //
        // 【頂点シェーダーはm_GBufferVertexShaderを共有する】プリパスとG-Bufferで頂点の
        // 変換結果が1ulpでもずれると深度が一致せず、GREATER_EQUALのテストを通らずに
        // その面がまるごと消える。写して2本にすると最適化の差で容易にずれる。
        // 不透明マテリアル用はピクセルシェーダーを持たない(nullptr = 段ごと省く)
        std::unique_ptr<RHI::IRHIShader> m_DepthPrepassCutoutPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassPipelineStateMirrored;
        std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassCutoutPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassCutoutPipelineStateMirrored;
        // メッシュシェーダー版のプリパス(G-Bufferと同じ増幅/メッシュシェーダーを使う)。
        // これが無いと、メッシュレット経路で描くモデルの深度をプリパスで埋められない
        std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassMeshletPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassMeshletPipelineStateMirrored;
        std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassMeshletCutoutPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassMeshletCutoutPipelineStateMirrored;
        // プリパスを走らせるか。オーバードローが小さいシーンでは、増えるジオメトリ1周ぶんが
        // 省けるピクセルシェーダーより高くつくため切れるようにしてある
        bool m_DepthPrepassEnabled = Defaults::DepthPrepassEnabled;
        // メッシュ単位のフラスタムカリングを行うか(対照実験用。EngineDefaults.h参照)。
        // OFFのあいだは判定を1回も呼ばないので、統計は「判定なし」になる
        bool m_MeshCullingEnabled = Defaults::MeshCullingEnabled;

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
            // m_ModelInstanceBuffer の中の先頭位置。頂点シェーダーは
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
        // 上のレコードを載せる StructuredBuffer。**1フレームに1回だけ更新する** ――
        // パスごとに詰め直す案は、DX12 の StructuredReadOnly が
        // MaxUpdatesPerFrame x kFrameCount + 1 段の UPLOAD ヒープを常時確保するため、
        // 反射プローブの6面ぶんを見込むと VRAM が跳ねる(DX12Device::CreateBuffer)
        std::unique_ptr<RHI::IRHIBuffer> m_ModelInstanceBuffer;
        // 1バッチの上限。上限が無いと「街灯を市街全域に5000個」のようなグループが
        // 1つの巨大AABBになり、どのパスからも一度も間引かれなくなる。
        // グループ内を空間セルでソートしてから刻むので、バッチは局所的にまとまる
        static constexpr uint32_t kMaxInstancesPerBatch = 128;
        bool m_InstancingEnabled = Defaults::InstancingEnabled;
        // バッチを組み直す(レンダーグラフの構築より前に1フレーム1回。UpdateModelLODの後)
        void BuildInstanceBatches(RHI::IRHICommandList* commandList);

        // 各パスが1回のドローで描く単位。バッチ(InstanceCount>=2)と、まとめられなかった
        // 1体(InstanceCount==1)を同じ形で扱うためのもの。
        //
        // 【1つのループで両方を回すため】バッチ用の描画コードを別に書くと、
        // 「まとめたときだけ条件を間違える」類のずれが入り込む。判定も定数もドロー発行も
        // 1か所に保つ
        struct InstanceDrawUnit
        {
            // 代表インスタンス。World以外の値(IsMirrored / IsWater / メッシュ単位AABB)を読む。
            // Worldはバッチのときインスタンスバッファ側から引かれるので使われない
            const Assets::ModelInstance* Instance = nullptr;
            // 代表のシーン内番号。単体のときに呼び出し側がGetLODDraws/GetCurrentLODを引くのに使う
            size_t InstanceIndex = 0;
            // バッチのときだけ非nullptr。単体のときは呼び出し側が段を決める
            const Assets::Model* Model = nullptr;
            uint32_t InstanceBase = 0;
            uint32_t InstanceCount = 1;
            // カリングに使うAABB。バッチでは構成インスタンスの包絡。
            // 【参照ではなく値で持つ】IsAABBVisibleがfloat[3]への参照を取るのに合わせる
            float WorldBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
            float WorldBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
            bool IsBatch() const { return InstanceCount > 1; }
        };
        // このフレームの描画単位を組み立てる。coarsestLOD が真ならシャドウ/プローブ用の組、
        // 偽なら深度プリパス/G-Buffer/平面反射用の組を使う。
        // シーンの全インスタンスがちょうど1回ずつ現れる(バッチに入ったものはバッチとして)
        void GetInstanceDrawUnits(bool coarsestLOD, std::vector<InstanceDrawUnit>& outUnits) const;
        // 上の出力先。パスは順に実行されるので1本を使い回してよい(確保のやり直しを避ける)。
        // **パスのラムダより長生きする必要がある**ため、ローカル変数ではなくここに置く
        mutable std::vector<InstanceDrawUnit> m_DrawUnitScratch;
        // 統計。**フラスタムカリングとは別建てにする** ―― 「バッチが0のまま」は
        // 「まとめられる相手がいない」のか「一度も実行されていない」のかを区別できないため、
        // まとめた数と減らせたドロー数の両方を出す
        uint32_t m_InstancedBatchCount = 0;
        uint32_t m_InstancedInstanceCount = 0;
        uint64_t m_FrameStatsInstancedBatchSum = 0;
        uint64_t m_FrameStatsInstancedInstanceSum = 0;

        // --- メッシュシェーダー版のジオメトリパス(Shaders/3D/GBufferMeshlet.hlsl) ---------
        //
        // 増幅シェーダーがメッシュレット単位で錐台・法線コーンのカリングを行い、
        // 生き残った塊だけをメッシュシェーダーがラスタライザへ流す。書き込む先も内容も
        // 上の通常パスとまったく同じG-Bufferで、ピクセルシェーダーも共有している
        // (m_GBufferPixelShader)。そのため切り替えても見た目は一致するのが正しい。
        //
        // 非対応環境(DX11、メッシュシェーダーTier 1未満、bindless非対応)では
        // すべてnullptrのままになり、描画側は自動的に従来経路を使う
        std::unique_ptr<RHI::IRHIShader> m_GBufferAmplificationShader;
        std::unique_ptr<RHI::IRHIShader> m_GBufferMeshShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferMeshletPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferMeshletPipelineStateMirrored;
        // メッシュレットごとに色分けするデバッグ表示。ピクセルシェーダーだけが違う
        std::unique_ptr<RHI::IRHIShader> m_GBufferMeshletDebugPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferMeshletDebugPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferMeshletDebugPipelineStateMirrored;
        // このデバイスがメッシュシェーダーを使えるか(IRHIDevice::SupportsMeshShader()の写し)。
        // m_DeviceはKurenaiEngineBaseのprotectedメンバで、派生クラスのfriendであるUIパネルから
        // 触れるかはC++の規則の解釈が分かれるため、m_RaytracingAvailableと同じくここへ控える
        bool m_MeshShaderAvailable = false;
        // bindless区画の容量と使用数(IRHIDevice::GetBindlessCapacity/GetBindlessUsedCountの写し)。
        // 容量は初期化時に、使用数はフレーム先頭に控える。**満杯でも例外は飛ばず
        // 白1x1で描かれてしまう**ため、UIとフレーム統計ログの両方へ出す
        uint32_t m_BindlessCapacity = 0;
        uint32_t m_BindlessUsedCount = 0;
        // メッシュレット経路を使うか(ImGuiのレンダリングパネルから切り替える)。
        // 対応環境では既定で有効。無効にすると従来の頂点シェーダー描画に戻るため、
        // 見た目の差分を目で比較できる
        bool m_MeshletRenderingEnabled = true;
        // メッシュレットごとの色分け表示。m_MeshletRenderingEnabledが有効なときだけ効く
        bool m_MeshletDebugViewEnabled = false;
        // 増幅シェーダーのHi-Zオクルージョンカリング(Stage 5-2)。メッシュレットのバウンディング球を
        // 前フレームのHi-Zへ投影し、「視界内だが手前の何かに完全に隠れている」塊を落とす。
        //
        // 【メッシュレット経路でしか効かない】判定を書いてあるのは増幅シェーダーなので、
        // メッシュシェーダー非対応の環境(基準機のIntel UHD 620を含む)では一切走らない。
        // これが有効なフレームだけHi-Zパスも構築される(m_HiZTextureのコメント参照)
        bool m_OcclusionCullingEnabled = Defaults::OcclusionCullingEnabled;
        // オクルージョン判定でバウンディング球を膨らませる倍率。
        //
        // 【1.0が基準】判定に使うHi-Zは前フレームのものなので、そのフレームのカメラ移動ぶんは
        // 別項(移動距離をそのまま半径へ足す)で吸収している。この倍率が埋めるのはそれとは別の
        // 誤差 ―― バウンディング球がメッシュレットの実体より緩いこと、およびカメラ回転による
        // 見え方の変化。ポップ(隠れていないものが消える)が出たら上げる
        float m_OcclusionCullRadiusScale = Defaults::OcclusionCullRadiusScale;

        // --- メッシュレットカリングの統計(Stage 5-2) ---
        //
        // 【「間引き0」だけでは何も分からない】判定式が常に通しているのか、本当に全部
        // 見えているのかを区別できない。CPU側のフラスタムカリング(m_FrustumCullTested /
        // m_FrustumCullCulled)が判定数と対で出しているのと同じ理由で、ここでも対で出す。
        // **オクルージョンは視錐台+コーンとは別のカウンタにする** ―― 合算すると
        // 「俯瞰(遮蔽が少ない)と街路(遮蔽が多い)で差が出るか」という確認ができない。
        bool m_MeshletCullStatsEnabled = Defaults::MeshletCullStatsEnabled;

        // --- メッシュレットLOD(離散LOD。Stage 6) ---------------------------------------
        //
        // 段を選ぶのは増幅シェーダーで、ここにあるのはその入力。
        // 【1つのモデル内で段を混ぜない】選択の入力はモデルのバウンディング球とカメラだけで、
        // メッシュレットごとの値を使わない。段が混ざると、簡略化で頂点が動いた側と
        // 動いていない側で辺が一致せず、境目に穴が開く
        bool m_MeshletLODEnabled = Defaults::MeshletLODEnabled;
        float m_MeshletLODQuality = Defaults::MeshletLODQuality;
        int32_t m_MeshletLODForcedLevel = Defaults::MeshletLODForcedLevel;
        // 色分け表示を段ごとにする。上の「メッシュレットを色分け」が有効なときだけ効く
        bool m_MeshletLODDebugColorEnabled = false;
        // 毎フレーム主カメラから作り直し、全パスの定数バッファへ同じものを配る
        MeshletLODFrameConstants m_MeshletLODFrame;
        // 増幅シェーダーが数え上げる先。uint×3 = [判定, 視錐台+コーンで間引き, オクルージョンで間引き]
        static constexpr uint32_t kMeshletCullStatsCount = 3;
        std::unique_ptr<RHI::IRHIBuffer> m_MeshletCullStatsBuffer;
        // カウンタをCPUへ持ってくるための受け皿。
        //
        // 【リングにする理由】コピーを積んだ直後に読んでもGPUはまだ実行していない。
        // DX12はkFrameCount(=2)フレームぶんCPUが先行するので、3本持って「2フレーム前に
        // 書いたもの」を読めばGPUの完了を待たずに済む。待つとフレームが直列化し、
        // 計測のために計測対象を壊すことになる
        static constexpr uint32_t kMeshletCullStatsRingSize = 3;
        std::unique_ptr<RHI::IRHIBuffer> m_MeshletCullStatsReadback[kMeshletCullStatsRingSize];
        // 今フレームが書き込むリングの位置。読むのは (index + 1) % リング長 = 最も古いもの
        uint32_t m_MeshletCullStatsRingIndex = 0;
        // カウンタバッファのUAVのbindless番号(RegisterBindlessUAVが払い出す)。
        // 非対応環境ではkInvalidBindlessIndexのままで、統計は無効になる
        uint32_t m_MeshletCullStatsBindlessIndex = RHI::kInvalidBindlessIndex;
        // 直近に読み戻せた値(Perfログの集計に足し込む前の生値)。デバッグ表示にも使う
        uint32_t m_MeshletCullTested = 0;
        uint32_t m_MeshletCullFrustumCulled = 0;
        uint32_t m_MeshletCullOcclusionCulled = 0;

        // --- モデル単位のGPUカリング(Stage 5-3) ---
        //
        // コンピュートシェーダー(ModelCull.hlsl)が、描画候補のワールドAABBを
        // 視錐台とHi-Zで判定し、生き残ったものだけの ExecuteIndirect 引数を詰める。
        // 深度プリパスとG-Bufferは、その引数でまとめて描く。
        //
        // 【行き先をPSOごとに分ける】1回のExecuteIndirectで切り替えられるのは引数に
        // 含めたルートパラメータだけで、PSOは切り替えられない。ミラーリングの有無と
        // 深度プリパスの不透明/カットアウトはPSOが違うため区画を分け、1区画につき
        // 1回ずつ発行する。
        //
        // 【プリパスとG-Bufferを同じ引数で描く理由】片方だけ間引くと絵が壊れる。
        // プリパスが深度を書いたものをG-Bufferが描かないと、その画素は
        // 「深度はあるのに色が無い」穴になる
        enum ModelCullRegion : uint32_t
        {
            kModelCullRegionGBuffer = 0,
            kModelCullRegionGBufferMirrored,
            kModelCullRegionPrepassOpaque,
            kModelCullRegionPrepassOpaqueMirrored,
            kModelCullRegionPrepassCutout,
            kModelCullRegionPrepassCutoutMirrored,
            kModelCullRegionCount,
        };
        // 引数バッファの先頭に置く「区画ごとの発行数」の領域。ExecuteIndirectの
        // 件数バッファとしてそのまま渡す(1区画あたりuint1つ)。
        //
        // 【256バイトに切り上げる】後ろに続く引数配列の先頭を、定数バッファのGPUアドレスが
        // 8バイト境界に載る位置から始めるため
        static constexpr uint32_t kModelCullArgsBaseOffset = 256;
        static_assert(
            kModelCullArgsBaseOffset >= sizeof(uint32_t) * kModelCullRegionCount,
            "区画ごとの発行数が引数配列の領域へはみ出している");

        // ModelCull.hlsl の struct ModelCullInstance と1対1で対応(48バイト)。
        // **構造化バッファは詰めて並ぶ**ので、float3の直後にuintが来る配置がそのまま一致する
        struct GpuModelCullInstance
        {
            float BoundsMin[3];
            uint32_t GroupCount;
            float BoundsMax[3];
            // 出力先の区画番号(= PSO。ModelCullRegion)
            uint32_t RegionIndex;
            // このドローが使うObjectConstantsのGPU仮想アドレス([0]=下位32bit、[1]=上位32bit)
            uint32_t CbvAddress[2];
            uint32_t Padding[2];
        };
        static_assert(sizeof(GpuModelCullInstance) == 48, "ModelCull.hlslのModelCullInstanceと一致させること");

        // Hi-Zを深度プリパスの深度から作るか。切ると従来どおりG-Bufferの後で作り、
        // 判定は前フレームのHi-Zで行う(意味と効果はDefaults::HiZFromDepthPrepass)
        bool m_HiZFromDepthPrepassEnabled = Defaults::HiZFromDepthPrepass;
        bool m_ModelCullGpuEnabled = Defaults::ModelCullGpuEnabled;
        // カリング結果で実際に描画発行まで行うか。falseなら判定と計数だけ行い、
        // 描くのは従来のCPUループのまま(コストと効果をA/Bで測るためのトグル)
        bool m_ModelCullIndirectEnabled = Defaults::ModelCullIndirectEnabled;
        std::unique_ptr<RHI::IRHIShader> m_ModelCullComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ModelCullPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_ModelCullConstantBuffer;
        // 描画候補(GpuModelCullInstance)の配列。毎フレームCPUから書き直す
        std::unique_ptr<RHI::IRHIBuffer> m_ModelCullInstanceBuffer;
        // [判定, 視錐台で間引き, オクルージョンで間引き, 生き残り] + 区画ごとの発行数。
        //
        // 【前の4つはモデル数】数えるのはG-Bufferぶんの候補だけで、そこは1モデル1件になる
        // (m_ModelCullPrepassCandidateCount のコメント参照)。深度プリパスぶんも数えると
        // 1モデルを2回数えてしまい、CPU側の判定と単位が合わなくなる
        static constexpr uint32_t kModelCullCounterCount = 4 + kModelCullRegionCount;
        std::unique_ptr<RHI::IRHIBuffer> m_ModelCullCounterBuffer;
        // ExecuteIndirectへそのまま渡すバッファ。先頭に区画ごとの発行数が並び、
        // kModelCullArgsBaseOffset から先が区画ごとの引数配列
        std::unique_ptr<RHI::IRHIBuffer> m_ModelCullDrawArgsBuffer;
        // 区画1つぶんのバイト数(ComputeModelCullRegionStride)。描画パスが
        // 自分の区画の先頭オフセットを求めるのに使う
        uint32_t m_ModelCullRegionStride = 0;
        // 区画ごとの候補数。ExecuteIndirectへ渡すmaxCommandCount(GPUが書く発行数の上限)
        uint32_t m_ModelCullRegionCandidates[kModelCullRegionCount]{};
        // GPUへ載せる直前の候補配列。毎フレームの確保を避けるため使い回す
        std::vector<GpuModelCullInstance> m_ModelCullUploadScratch;
        // 受け皿。リングの理由と段数はメッシュレット統計と同じ
        std::unique_ptr<RHI::IRHIBuffer> m_ModelCullReadback[kMeshletCullStatsRingSize];
        uint32_t m_ModelCullRingIndex = 0;
        // m_ModelCullInstanceBuffer / m_ModelCullDrawArgsBuffer が収まる候補数。
        // 1インスタンスがLODのクロスディザで最大2件の候補を出すため、インスタンス数の2倍で確保する
        uint32_t m_ModelCullCapacity = 0;
        // このフレームにCPUが積んだ候補数(プリパスぶん + G-Bufferぶん)
        uint32_t m_ModelCullCandidateCount = 0;
        // そのうち深度プリパスぶんの数。候補配列の前半を占め、G-Bufferぶんが後半に続く。
        //
        // 【この境目が2つの役目を持つ】判定を2回に分けるときの区切りであり、
        // 統計を数え始める位置でもある。**統計はG-Bufferぶんだけで数える** ――
        // 両方数えると1モデルを2回数え、CPU側の数と単位が合わなくなる
        uint32_t m_ModelCullPrepassCandidateCount = 0;
        // 直近に読み戻せた値
        uint32_t m_ModelCullTested = 0;
        uint32_t m_ModelCullFrustumCulled = 0;
        uint32_t m_ModelCullOcclusionCulled = 0;
        uint32_t m_ModelCullSurvived = 0;
        // 区画ごとにGPUが実際に発行したドロー数(読み戻した値)。
        // ここが0のまま絵が出ているなら、間接描画ではなく従来のCPUループが描いている
        uint32_t m_ModelCullRegionIssued[kModelCullRegionCount]{};
        // 上の値がどの経路のものか。ログで「間接描画で描いた」と「数えただけ」を区別する
        bool m_ModelCullIndirectActiveLastFrame = false;
        // Hi-Zを深度プリパスから作った経路だったか。
        //
        // 【これが無いと切り替えを確かめられない】カメラが止まっていると新旧どちらの経路でも
        // 間引き数が一致する(前フレームのHi-Zと今フレームのHi-Zが同じ内容になるため)。
        // 「差が出ない」を合格と読まないために、経路そのものをログへ出す
        bool m_HiZFromDepthPrepassLastFrame = false;
        // 判定を2回に分けたときの、それぞれが受け持った候補数(プリパスぶん / G-Bufferぶん)
        uint32_t m_ModelCullDispatchCounts[2]{};
        // 同じフレームでCPU側が視錐台で間引いた数。GPUの「視錐台で間引き」と突き合わせる。
        //
        // 【GPUの数値は2フレーム遅れなので、CPU側も同じだけ遅らせて比べる】
        // 今フレームのCPU値と2フレーム前のGPU値を比べると、カメラが動いている間は
        // 常に食い違って見える。リードバックと同じリングに積んで、同じフレームのものを比べる
        uint32_t m_ModelCullCpuFrustumCulled = 0;
        uint32_t m_ModelCullCpuFrustumHistory[kMeshletCullStatsRingSize]{};
        uint32_t m_ModelCullCandidateHistory[kMeshletCullStatsRingSize]{};
        // 上のリングから取り出した、GPUの数値と同じフレームのCPU側の値(ログの比較に使う)
        uint32_t m_ModelCullComparedCpuFrustumCulled = 0;
        uint32_t m_ModelCullComparedCandidateCount = 0;

        std::unique_ptr<RHI::IRHITexture> m_GBufferAlbedo;
        std::unique_ptr<RHI::IRHITexture> m_GBufferNormal;
        std::unique_ptr<RHI::IRHITexture> m_GBufferMaterial;
        // 自発光(エミッシブ)。AO/シャドウの影響を受けずライティングパスで常に加算される
        std::unique_ptr<RHI::IRHITexture> m_GBufferEmissive;
        std::unique_ptr<RHI::IRHITexture> m_GBufferDepth;
        // モーションベクター(速度バッファ)。「この画素に映っているものが前フレームでは画面の
        // どこにいたか」をUV単位の2Dベクトルで持ち、TAAが履歴を引く位置の決定に使う。
        // 現在のシーンは全インスタンスが静的(ModelInstance::Worldは読み込み時に確定し以降
        // 変わらない)なので、速度の発生源はカメラの移動・回転だけである。そのためGBuffer.hlslは
        // 同じワールド座標を今フレームと前フレームのビュー射影行列で投影して差を取るだけでよく、
        // インスタンスごとの前フレームのワールド行列(PrevWorld)を持つ必要がない。
        // 動的オブジェクトを入れる際はObjectConstantsへPrevWorldを追加すること
        std::unique_ptr<RHI::IRHITexture> m_GBufferVelocity;
        // bent normal(ワールド空間の正規化しない可視方向の平均)。.rgb = bRaw、.a = 有効フラグ
        std::unique_ptr<RHI::IRHITexture> m_GBufferBentNormal;

        // 直接光パス(G-Buffer+シャドウマップからPBRの直接光(拡散+鏡面反射、シャドウ適用済み)を
        // 計算しHDRで書き出す。DeferredLightingパスとSSIL_VisibilityBitmask.hlslの両方から
        // サンプルされるため、G-Bufferと同じレンダー解像度・R32G32B32A32_Float(HDR)で保持する)
        std::unique_ptr<RHI::IRHIShader> m_DirectLightVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_DirectLightPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_DirectLightPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_DirectLightTexture;

        // AO/GI手法の選択。SSAOは遮蔽率のみ、SSIL(Visibility Bitmask)は遮蔽率に加えて
        // 近傍サーフェスからの間接拡散光(バウンス光)も計算する。Raytracedは同じものを
        // 深度バッファではなく高速化構造への交差判定で求める(画面外の遮蔽物も効く)。
        // いずれも出力フォーマットは共通(rgb=間接拡散光, a=遮蔽率)で、
        // ライティングパスは選択中のテクスチャを1枚読むだけでよい
        enum class AOTechnique
        {
            SSAO,
            SSILVisibilityBitmask,
            Raytraced,
        };
        bool m_AOEnabled = Defaults::AOEnabled;
        AOTechnique m_AOTechnique = AOTechnique::SSAO;
        // マテリアルの遮蔽マップ(glTFのocclusionTexture。22章)を使うか。
        // 上のm_AOEnabled(スクリーンスペースAO/GI)とは完全に別系統で、無効にしても遮蔽マップは
        // 効き続けるためこのトグルを別に持つ。無効時はObjectConstants.OcclusionStrengthへ0を渡し、
        // 各パスのlerp(1, occlusionSample, 0) = 1(遮蔽なし)にする方式なのでシェーダー側の変更は不要。
        // 反射プローブはキャプチャ時の値が焼き込まれるため、切り替えても焼き直すまで反映されない
        bool m_OcclusionMapEnabled = Defaults::OcclusionMapEnabled;
        std::unique_ptr<RHI::IRHITexture> m_AODisabledTexture; // AO無効時に使う、遮蔽なし・間接光なしのテクスチャ

        // AO/GI共通のブラーパス(4x4ボックスブラーでrgba全チャンネルを均す。SSAO/SSIL両方から使い回す)
        std::unique_ptr<RHI::IRHIShader> m_AOVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_AOBlurPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_AOBlurPipelineState;

        // SSAOパス(G-BufferのNormal/Depthから遮蔽率を計算する。G-Bufferと同じレンダー解像度)
        std::unique_ptr<RHI::IRHIShader> m_SSAOPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SSAOPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SSAORawTexture;
        std::unique_ptr<RHI::IRHITexture> m_SSAOTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_SSAOConstantBuffer;
        std::vector<DirectX::XMFLOAT4> m_SSAOKernel;
        float m_SSAORadius = Defaults::SSAORadius;
        float m_SSAOPower = Defaults::SSAOPower;
        // 1画素あたりのカーネルサンプル数。SSAOのコストはほぼこれに比例する
        // (実測でAOパスはジオメトリが画面を占めるシーンで4.8〜11.0msあり、雲を分離した後の
        //  最大の残りだった)。定数バッファの配列はkSSAOKernelSizeMax(16)で固定のまま、
        // 実際に回す段数だけをSSAOConstants.Params.wでシェーダへ渡す。
        //
        // 【減らすときはカーネルを作り直す】GenerateSSAOKernelはi/kernelSizeで各サンプルの
        // 長さを決めているため、16本用のカーネルの先頭N本を使うと原点付近の短いサンプルばかりが
        // 残り、遠距離の遮蔽を拾わなくなる。必ずこの数で生成し直すこと(EnsureSSAOKernel)
        uint32_t m_SSAOKernelSize = Defaults::SSAOKernelSize;

        // SSILパス(Visibility Bitmask): G-BufferのAlbedo/Normal/Depthから遮蔽率と間接拡散光を計算する
        std::unique_ptr<RHI::IRHIShader> m_SSILPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SSILPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SSILRawTexture;
        std::unique_ptr<RHI::IRHITexture> m_SSILTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_SSILConstantBuffer;
        float m_SSILRadius = Defaults::SSILRadius;
        float m_SSILThickness = Defaults::SSILThickness;
        float m_SSILIntensity = Defaults::SSILIntensity;
        float m_SSILPower = Defaults::SSILPower;
        uint32_t m_SSILSliceCount = Defaults::SSILSliceCount;
        uint32_t m_SSILStepCount = Defaults::SSILStepCount;

        // RTAOパス: 法線周りの半球へ余弦重みでレイを撃ち、遮蔽率と1バウンスの間接拡散光を求める
        // コンピュートパス。出力はSSAO/SSILとまったく同じ意味・同じフォーマットなので、
        // 後段のAOBlurパスとライティングパスは無変更で使い回せる(27章)。
        // シェーダーとパイプラインステートはm_RaytracingAvailableがtrueのときだけ作る。
        // 生バッファだけはコンピュートがUAVで書くためCreateUAVTextureで作る(ブラー後は従来どおり
        // ピクセルシェーダーが書くレンダーターゲット)
        std::unique_ptr<RHI::IRHIShader> m_RTAOComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_RTAOPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_RTAORawTexture;
        std::unique_ptr<RHI::IRHITexture> m_RTAOTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_RTAOConstantBuffer;
        int32_t m_RTAOSampleCount = Defaults::RTAOSampleCount;
        float m_RTAOMaxDistance = Defaults::RTAOMaxDistance;
        float m_RTAOPower = Defaults::RTAOPower;
        float m_RTAOIntensity = Defaults::RTAOIntensity;
        bool m_RTAOBounceShadowRayEnabled = Defaults::RTAOBounceShadowRayEnabled;

        // ライティングパス(G-Bufferを読みSceneColorへ出力。G-Bufferと同じレンダー解像度)。
        // SceneColorはHDR(R16G16B16A16_Float)で、トーンマッピングは行わない(Tonemapパス参照)
        std::unique_ptr<RHI::IRHIShader> m_LightingVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_LightingPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_LightingPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SceneColor;

        // 半透明フォワードパス: Deferred(G-Buffer)には書き込まれなかったBLENDマテリアルのメッシュを、
        // Lightingパスの後にSceneColorへ直接フォワードシェーディングしてアルファブレンド合成する
        // (深度テストはGBuffer深度に対して行うが書き込みは行わない)。頂点レイアウトはGBufferパスと
        // 共通(POSITION/NORMAL/TEXCOORD/TANGENT)
        std::unique_ptr<RHI::IRHIShader> m_TransparentVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_TransparentPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_TransparentPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_TransparentPipelineStateMirrored;

        // --- ドローンショー(発光点の描画) ---------------------------------------------
        // 夜空を編隊飛行する多数のドローンを、1機につきカメラ正対のビルボード1枚として
        // 加算合成で描く。編隊の生成と時間補間はDroneShow.h/.cppが持ち、ここは描画だけを担う。
        //
        // 頂点バッファを持たず、Draw(6 * 機体数, 0)とSV_VertexIDでクアッドを展開する
        // (理由はShaders/3D/DroneShow.hlsl冒頭)。機体データはm_DroneBufferから
        // 頂点シェーダーが直接読む(SetVertexShaderResourceBuffer)。
        //
        // 【PSOは1本でよい】平面反射(鏡映カメラ)でもこれをそのまま使う。メッシュ描画のように
        // ワインディングを反転したPSOを別に持つ必要は無い ―― 理由はPSO生成箇所のコメント
        std::unique_ptr<RHI::IRHIShader> m_DroneShowVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_DroneShowPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_DroneShowPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_DroneShowConstantBuffer;
        // 機体データ(StructuredReadOnly)。kMaxDrones分を固定で確保し、実際に描くのは
        // m_DroneInstances.size()機ぶんだけ
        std::unique_ptr<RHI::IRHIBuffer> m_DroneBuffer;
        // 1フレームぶんの機体の状態。毎フレームDroneShow::Evaluateが書き、
        // グラフ構築前に1回だけm_DroneBufferへUpdateBufferする
        // (m_LightBufferと同じ理由: 本描画と平面反射の2パスから読まれるため、
        //  パスの中で更新すると先に走る側が未更新の内容を読む)
        std::vector<GPUDrone> m_DroneInstances;
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

        // Hi-Zミップチェーン: G-Buffer深度から、コンピュートシェーダーで1x1まで縮小するミップチェーンを
        // 構築するパス。各ミップは2x2ブロックの最小値(Reverse-Zのため「最も遠い」深度)を保持する。
        //
        // 消費者は2つ: デバッグ表示(Render Targets - Hi-Z)と、増幅シェーダーの
        // オクルージョンカリング(m_OcclusionCullingEnabled)。**どちらも要らないフレームでは
        // 構築しない** ―― 1280x720で「コピー1回 + ミップ段数-1回のディスパッチ」が走り、
        // Intel UHD 620での実測で1.19〜1.21ms(GPUフレーム時間30msの約4%)を占めるため
        std::unique_ptr<RHI::IRHIShader> m_HiZCopyComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_HiZCopyPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_HiZDownsampleComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_HiZDownsamplePipelineState;
        std::unique_ptr<RHI::IRHITexture> m_HiZTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_HiZConstantBuffer;
        uint32_t m_HiZMipLevels = 1;
        // デバッグ表示(Render Targets - Hi-Z)で確認するミップレベル
        int32_t m_HiZDebugMipLevel = 0;
        // m_HiZTextureの中身が「1回でも構築されたHi-Z」になっているか。
        //
        // 【オクルージョン判定の門番】CreateHiZTextureが作った直後の中身は未定義で、
        // それを深度として判定すると視界内のほぼ全部を「隠れている」と誤判定しうる。
        // 解像度変更・シーン読み込みでfalseへ戻し、Hi-Zパスが1回走ってからtrueにする
        bool m_HiZValid = false;

        // 鏡面反射の手法。どのモードでもLightingパスが適用した鏡面IBLを「差し替える」形で働き、
        // Offならその差し替えを一切行わない(プローブ/グローバルIBLがそのまま残る。20章)
        enum class ReflectionMode
        {
            Off,         // 反射パスを実行しない
            ScreenSpace, // SSR(SSR.hlsl)。画面に映っているものだけが反射に映る
            Raytraced,   // RT反射(RTReflection.hlsl)。画面外も映るが、DX12かつDXR Tier 1.1が要る
        };
        // 「反射を出す」と決まったあとで、環境から**手法だけ**を選ぶ。出すかどうかはここでは決めない。
        // 画面外も反射に映るRTが使えるなら常にそちら
        static constexpr ReflectionMode ReflectionModeForCapability(bool raytracingAvailable)
        {
            return raytracingAvailable ? ReflectionMode::Raytraced : ReflectionMode::ScreenSpace;
        }
        // シーンが何も言っていないときの既定。「反射を出すか」をここで決める。
        //
        // 【この関数に「出すか」と「どの手法か」を兼ねさせてはいけない】兼ねさせると、
        // ApplyLoadedSceneが「シーンが反射を要求している。ではどの手法か」を聞くときにも
        // 同じ関数を使うことになる。Defaults::SSREnabledはfalse(SSRは画面端で反射が途切れる
        // 破綻が目立つため)なので、RTが使えない環境では**.ksceneがScreenSpaceReflection = true
        // と明示していてもReflectionMode::Offが返り、シーンの指定が握り潰される**。
        // DX11でモン・サン=ミシェルの水面に何も映らない、White Furnace TestのSSR回帰テストが
        // 実は動いていない、という形で現れていた(DX12はDXRが使えてRTが選ばれるため露見しなかった)
        static constexpr ReflectionMode DefaultReflectionMode(bool raytracingAvailable)
        {
            return Defaults::SSREnabled ? ReflectionModeForCapability(raytracingAvailable) : ReflectionMode::Off;
        }
        // 現在の手法。RaytracedはSupportsRaytracing()がtrueの環境でしか選べない
        // (UI側で選択不可にする)。ここの初期値はm_RaytracingAvailableが確定する前の値でしかなく、
        // 実際の既定はシーン読み込み時にDefaultReflectionModeで決め直される
        ReflectionMode m_ReflectionMode = DefaultReflectionMode(false);
        // UIの「既定値に戻す」(右クリック)が戻る先。シーン読み込み時に決まった手法を控えておく。
        // 【静的なDefaultReflectionModeを使ってはいけない】.ksceneが指定を持つ場合、
        // 戻る先はエンジンの既定ではなく**そのシーンを読み込んだ直後の状態**である。
        // ここを取り違えると「既定へ戻したらシーンが要求した反射が消える」ことになる
        ReflectionMode m_SceneDefaultReflectionMode = DefaultReflectionMode(false);
        // レイトレーシング反射が使える環境か。デバイスのSupportsRaytracing()を初期化時に控えたもので、
        // UIの選択可否とシェーダー/パイプラインステートを作るかどうかの両方に使う
        // (RTReflection.hlslはRayQueryを含むためSM 6.5でしかコンパイルできず、
        //  非対応環境で作ろうとすると例外になる)
        bool m_RaytracingAvailable = false;
        // DDGIのレイ取得をDXRで行えるか。m_RaytracingAvailableとは別に持つ。
        //
        // 【なぜ別なのか】DDGIProbeTrace.hlslはコンピュートシェーダーの中でテクスチャを
        // 微分付きにサンプルするため、DXILの検証がシェーダーモデル6.6を要求する
        // (Derivatives in CS/MS/AS is SM 6.6+)。RayQuery自体はSM 6.5で足りるので、
        // 「DXR Tier 1.1に対応していて、かつSM 6.5のシェーダーバリアントで動いている」環境が
        // 実在しうる ―― その場合、他のRTパスは作れるのにこれだけ作れない。
        // 作成に失敗したらここをfalseにして、DDGIのレイ取得だけをラスタ経路へ戻す
        bool m_DDGIRaytracedTraceAvailable = false;

        // SSR(Screen Space Reflections)パス: LightingパスのSceneColorを反射先の環境色として
        // 再利用し、G-Buffer(Normal/Material/Depth)からワールド空間でレイマーチングして
        // 鏡面反射を加算する。無効時はこのパスをスキップし、Presentが直接m_SceneColorを参照する
        std::unique_ptr<RHI::IRHIShader> m_SSRVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_SSRPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SSRPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SSRTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_SSRConstantBuffer;
        float m_SSRMaxDistance = Defaults::SSRMaxDistance;
        float m_SSRThickness = Defaults::SSRThickness;
        float m_SSRRoughnessCutoff = Defaults::SSRRoughnessCutoff;

        // RT反射パス: TLASへ鏡面レイを撃ち、ヒット面を陰影計算して反射色を求めるコンピュートパス。
        // 出力はSSRと同じ「SceneColor + 反射の差し替え」なので、後段(Tonemap)から見ると
        // m_SSRTextureと完全に等価な入れ替え可能なバッファになる。
        // シェーダーとパイプラインステートはm_RaytracingAvailableがtrueのときだけ作る
        std::unique_ptr<RHI::IRHIShader> m_RTReflectionComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_RTReflectionPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_RTReflectionTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_RTReflectionConstantBuffer;
        float m_RTReflectionMaxDistance = Defaults::RTReflectionMaxDistance;
        float m_RTReflectionRoughnessCutoff = Defaults::RTReflectionRoughnessCutoff;
        bool m_RTReflectionShadowRayEnabled = Defaults::RTReflectionShadowRayEnabled;

        // RTシャドウパス: TLASへ太陽の見かけの円盤に向けて影レイを撃ち、可視率(0〜1)を
        // 単チャンネルのテクスチャへ書くコンピュートパス。DirectLighting.hlslがt6で読み、
        // CSMのComputeCascadedShadowFactorの戻り値と同じ位置で使う(26章)。
        // シェーダーとパイプラインステートはm_RaytracingAvailableがtrueのときだけ作る
        std::unique_ptr<RHI::IRHIShader> m_RTShadowComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_RTShadowPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_RTShadowTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_RTShadowConstantBuffer;
        int32_t m_RTShadowSampleCount = Defaults::RTShadowSampleCount;
        float m_RTShadowSunAngularRadiusDegrees = Defaults::RTShadowSunAngularRadiusDegrees;

        // --- MegaLights: ポイント/スポットライトの直接光を専用パスで求める経路 ---
        // 求めた寄与をHDRのテクスチャへ書き、DirectLighting.hlslがt7でそれを読んで加算する
        // (有効なあいだ、あちらのライトループは回らない)。太陽はこの経路の対象外で、
        // 従来どおりb0とCSM/RTシャドウが担当する。
        //
        // Referenceは全灯を総当たりして1灯ごとに影レイを撃つ、遅いが真値を返す経路で、
        // すべての測定の物差しにするためにある(MegaLightsReference.hlsl冒頭を参照)。
        //
        // 【StochasticとQuadSharedは同じ問題への別の解き方】どちらも候補プールから
        // 確率的に灯を選ぶが、1画素の推定量を良くする手段が違う:
        //   Stochastic … リザーバを時間・空間で再利用する(ReSTIR DI)。厳密に不偏だが、
        //                 再利用のたびに可視性を確かめるレイと不偏化の分母のための補正レイが要る。
        //                 実測(BistroExteriorNight 107灯/1280x720/RTX 4070 Ti)で
        //                 MegaLights合計4.26ms、うちSpatialだけで2.64ms ――
        //                 **全灯総当たりの参照実装(4.21ms)と同じコスト**になっていた
        //   QuadShared  … 2x2クアッドの4画素が撃った4本の結果を共有して平均する。
        //                 追加のレイは1本も撃たない(UE5 MegaLightsのDownsampleFactor=2に相当)。
        //                 影の縁が最大1画素ぼける偏りを受け入れる代わりにコストを切り下げる
        enum class MegaLightsMode
        {
            Off,        // 従来どおりDirectLighting.hlslのライトループで評価する
            Reference,  // 全灯総当たり+1灯1影レイ。ノイズは無いが遅い(グラウンドトゥルース)
            Stochastic, // 候補プールからRISで1灯選び、時間・空間再利用で磨く。厳密に不偏だがレイが多い
            QuadShared, // 1画素1レイのまま、2x2クアッドで可視性を共有して平均する
        };
        MegaLightsMode m_MegaLightsMode = Defaults::MegaLightsEnabled ? MegaLightsMode::Reference
                                                                     : MegaLightsMode::Off;
        // シェーダーとパイプラインステートはm_RaytracingAvailableがtrueのときだけ作る
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsReferenceComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsReferencePipelineState;
        std::unique_ptr<RHI::IRHITexture> m_MegaLightsTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsConstantBuffer;
        // 1灯あたりに撃つ影レイの本数。**0にすると影を撃たず可視率1で評価する**。
        // その状態の出力は、スクリーンスペースシャドウを切った既存のライトループと
        // 数値的に一致するはずで、BRDF・減衰・スポット円錐・プリ露出をまとめて検証できる
        // (MegaLightsReference.hlslの「恒等テスト」)。punctualは方向が1つに決まるため、
        // 1より大きくしても答えは変わらない(光源に半径が入る段階で意味を持つ)
        int32_t m_MegaLightsShadowRayCount = Defaults::MegaLightsShadowRayCount;

        // MegaLightsの候補プール(MegaLightsTilePool.hlsl)。タイルごとに「届くライト」を走査し、
        // 寄与に比例した確率でK灯を重みつきで抽出する。読み手は Initial(RISの提案分布)と
        // Spatial(不偏化の分母で「その灯が隣のタイルへ届くか」を判定する)。
        // 参照実装はこれを使わず全灯を回すので、参照実装のときは出力が使われない。
        // レイを撃たないパスだがMegaLightsと同時にしか使わないので、生成もRT対応時だけにしてある
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsTilePoolComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsTilePoolPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsTilePoolConstantBuffer;
        // 候補プール本体(BufferUsage::StructuredRW)。解像度に依存するためCreateRenderTargetsで作り直す
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsTilePoolBuffer;

        // 確率的サンプリング本体。2パスに分かれる。
        //   Initial (MegaLightsInitialSample.hlsl) … 候補プールからM個引きRISで1灯へ絞り、
        //                                            結果を**リザーバ**として書く(色は作らない)
        //   Shade   (MegaLightsShade.hlsl)         … そのリザーバへ影レイを1本撃ちHDRを書く
        //
        // 【なぜ分けるのか】時間・空間の再利用は「どの灯を選んだか」を持ち回って現フレームで
        // 評価し直す形でしか書けない。選択とシェードが1パスに混ざっていると再利用の段を
        // 差し込む場所が無い。出力先は参照実装と同じm_MegaLightsTexture
        // (同じ表示経路・同じ後段のままA/Bが撮れるようにするため)
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsInitialComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsInitialPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsShadeComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsShadePipelineState;
        // クアッド共有(手法3)の解決パス。Initial が書いた4画素ぶんのリザーバを読み、
        // **自分の面で評価し直して平均する**。レイを1本も撃たないのでTLASを束縛しない。
        // Shade と分けてあるのは、あちらが RayQuery を持ちSM6.5でしか焼けないのに対し、
        // こちらはレイを撃たず3バリアントすべてで焼けるため
        // (混ぜると使わないTLASを束縛したままレイ経路が残る)
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsResolveComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsResolvePipelineState;
        // 2パスで共有する定数バッファ(中身はフレーム内で不変なのでInitial側で1回更新する)
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsStochasticConstantBuffer;
        // 空間再利用の反復ごとの定数(中身は共有分と同じで、反復番号だけが違う)。
        // 【1本を使い回してはいけない】UpdateBuffer は同じフレームで2回書くと
        // 後の値が両方のパスに見えるため、反復の数だけバッファを分ける
        static constexpr uint32_t kMegaLightsMaxSpatialIterations = 2u;
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsSpatialConstantBuffer[kMegaLightsMaxSpatialIterations];
        // 1画素につき1リザーバ(16バイト)。MegaLightsCommon.hlsli の MegaLightsReservoir と
        // 一致させること。解像度に依存するためCreateRenderTargetsで作り直す。
        // 空間再利用は「読みながら同じバッファへ書けない」(近傍を読むので競合する)ため2本持つ
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsReservoirBuffer;
        // 画素ごとの「遮蔽が確定した灯」のキャッシュ(影の縁の暗いフリンジ対策)
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsBlockedLightBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsReservoirSpatialBuffer;
        // 空間再利用を2回以上回すときの ping-pong の相方。
        // 近傍を読むので入力と同じバッファへは書けず、反復のたびに交互に使う
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsReservoirSpatialBuffer2;
        // 時間再利用の履歴リザーバと履歴の幾何。**ping-pongにするのはWAR回避のため**
        // (RenderGraphは前方走査でRAWの辺しか張らない。詳細は生成箇所のコメント)
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsReservoirHistory[2];
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsHistoryGuide[2];
        // --- デノイザ(段階5) ---
        // 【時空間再利用とは別物】あちらはリザーバ(どの灯を選ぶか)を混ぜ、こちらは出た色を
        // 空間・時間へならす。TAAの手前でノイズを落とすためのもの
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsDenoiseTemporalShader;
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsDenoiseAtrousShader;
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsDenoiseRemodulateShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsDenoiseTemporalPSO;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsDenoiseAtrousPSO;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsDenoiseRemodulatePSO;
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsDenoiseConstantBuffer;
        // 時間累積の履歴(rgb=復調済みの色)とモーメント。どちらもping-pong。
        // **RenderGraphがWARの辺を張らない**ので、読む側と書く側を必ず別にする
        std::unique_ptr<RHI::IRHITexture> m_MegaLightsDenoiseHistory[2];
        std::unique_ptr<RHI::IRHITexture> m_MegaLightsDenoiseMoments[2];
        // à-trous のping-pong用。段ごとに入れ替える
        std::unique_ptr<RHI::IRHITexture> m_MegaLightsDenoisePing[2];
        std::unique_ptr<RHI::IRHITexture> m_MegaLightsDenoiseMomentPing[2];
        // 復調を戻した最終出力。DirectLightingはこれをt7で読む
        std::unique_ptr<RHI::IRHITexture> m_MegaLightsDenoisedTexture;
        uint32_t m_MegaLightsDenoiseHistoryIndex = 0u;
        bool m_MegaLightsDenoiseHistoryValid = false;
        bool m_MegaLightsDenoiseEnabled = Defaults::MegaLightsDenoiseEnabled;
        int32_t m_MegaLightsDenoiseAtrousPasses = Defaults::MegaLightsDenoiseAtrousPasses;
        int32_t m_MegaLightsDenoiseMaxFrames = Defaults::MegaLightsDenoiseMaxFrames;
        // クアッド共有(手法3)での時間累積の上限。**手法ごとに別に持つ。**
        // 1つの変数を共有して手法ごとに黙って読み替えると、UIのつまみが示す値と
        // 実際に効いている値が食い違う(「指定したのに効かない」の型)。
        // 分けておけば、UIもCLIも「いま効いている値」をそのまま触れる。
        // 手法3にリザーバの履歴が無いぶんここを長くしている(根拠は EngineDefaults.h)
        int32_t m_MegaLightsQuadDenoiseMaxFrames = Defaults::MegaLightsQuadDenoiseMaxFrames;
        float m_MegaLightsDenoiseSigmaLuminance = Defaults::MegaLightsDenoiseSigmaLuminance;
        float m_MegaLightsDenoiseFireflyClamp = Defaults::MegaLightsDenoiseFireflyClamp;
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsTemporalComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsTemporalPipelineState;
        uint32_t m_MegaLightsHistoryIndex = 0u;
        // 履歴の中身が今の解像度・今のシーンのものとして使えるか。
        // バッファのクリアが無いRHIなので、無効な間はシェーダへ「履歴を読むな」と伝える
        bool m_MegaLightsHistoryValid = false;
        bool m_MegaLightsTemporalEnabled = Defaults::MegaLightsTemporalEnabled;
        // 履歴のM(何個の候補から絞ったか)の上限。大きいほど収束は速いが、
        // 新しいサンプルが採用されにくくなり、灯を消しても明るさが残る(ゴースト)
        int32_t m_MegaLightsTemporalMClamp = Defaults::MegaLightsTemporalMClamp;
        // 前フレームの実効プリ露出EV100。
        // 【補正には使っていない】リザーバのWは露出に対して不変(比なので約分される)と
        // 実測で確かめた ―― TAAのm_TAAPrevEffectiveExposureEV100と違い、掛ける係数は1。
        // 詳細はKurenaiEngine3D.cppの「プリ露出の補正は入れない」。
        // 値は、将来この前提を疑うときに差を見られるよう記録だけ続けている
        float m_MegaLightsPrevEffectiveExposureEV100 = 0.0f;
        // 【検証専用】蓄積開始時に加える摂動(0=なし / 1=全ライトを消す / 2=露出を+2段跳ばす)。
        // 静止した絵では測れない「追従」を測るための入口。SetMegaLightsPerturbのコメント参照
        int32_t m_MegaLightsPerturbMode = 0;
        // 摂動を適用済みか(蓄積開始の1回だけ効かせる)
        bool m_MegaLightsPerturbApplied = false;

        // 空間再利用(MegaLightsSpatial.hlsl)。近傍が選んだ灯を借りて自分の面で評価し直す。
        // レイは1本も増えない ―― 借りるのは「どの灯か」だけ
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsSpatialComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsSpatialPipelineState;
        bool m_MegaLightsSpatialEnabled = Defaults::MegaLightsSpatialEnabled;
        int32_t m_MegaLightsSpatialNeighborCount = Defaults::MegaLightsSpatialNeighborCount;
        int32_t m_MegaLightsSpatialRadius = Defaults::MegaLightsSpatialRadius;
        int32_t m_MegaLightsSpatialIterations = Defaults::MegaLightsSpatialIterations;
        // 結合を不偏化(Z)にするか。単純なconfidence重みは、近傍が自分と違う候補集合から
        // 引いている可能性を無視するため不偏にならない(実測で総和の相対差 -8.0%)。
        // **切り替えて長時間平均を比べられるようにしてある** ――
        // 差が出なければどちらかが実装されていない
        bool m_MegaLightsSpatialMIS = Defaults::MegaLightsSpatialMIS;
        // 初期サンプルの可視レイでリザーバを殺すか。殺すと影の縁に暗い側の系統誤差が残る
        // (Zが可視率まで判定できないため)。詳細は EngineDefaults.h のコメント
        bool m_MegaLightsInitialVisibility = Defaults::MegaLightsInitialVisibility;
        // 1ピクセルあたりに候補プールから引く数(RISのM)。影レイの本数はこれとは独立で常に1本
        int32_t m_MegaLightsSampleCount = Defaults::MegaLightsSampleCount;

        // --- クアッド共有(手法3) ---
        // 2x2クアッドの仲間が撃った影レイの結果を借りて平均するか。
        // **切れるようにしてあるのは陽性対照のため** ―― 切ると自分の標本だけを使う形になり、
        // 手法2から時間再利用と空間再利用を外した構成と画素単位で一致するはず。
        // 一致しなければ配線のバグで、共有の効果を測る前にそこを潰す
        bool m_MegaLightsQuadShareEnabled = Defaults::MegaLightsQuadShareEnabled;
        // クアッドの4画素へ候補スロットを分けて引かせるか(層化)。
        // プールのスロットは混合分布からの i.i.d. 抽出なので、スロットの選び方を変えても
        // **周辺分布は変わらず割り戻しの式はそのまま厳密**。クアッドで重複した灯を
        // 引く確率が下がるぶん、4標本の多様性が上がる
        bool m_MegaLightsQuadStratify = Defaults::MegaLightsQuadStratify;
        // 遮蔽が確定した灯のキャッシュ(BlockedLights)を手法3でも使うか。
        // 手法3は時間再利用パスを持たないが、キャッシュ自体は Initial が維持している。
        // **陽性対照では切る**(履歴に依存すると手法2との画素単位の一致が崩れる)
        bool m_MegaLightsBlockedCacheEnabled = Defaults::MegaLightsBlockedCacheEnabled;
        // 1画素あたりに引く標本(リザーバ)の数。**手法3だけが1より大きくできる。**
        // 手法2の時間・空間再利用は「1画素1リザーバ」を前提に添字を組み立てているため。
        // 影レイの本数はそのままこの数になる(標本ごとに1本撃つ)
        int32_t m_MegaLightsQuadSamplesPerPixel = Defaults::MegaLightsQuadSamplesPerPixel;
        // 候補プールが1タイルあたりに抽出する灯の数(K)。
        // **1画素あたりの標本数では減らないノイズがここで決まる** ―― プールはタイルに1つで、
        // タイル内の全画素が同じK個から引くので、プールの引き方のばらつきはタイル内で
        // 共通のオフセットとして乗る(根拠は EngineDefaults.h)
        int32_t m_MegaLightsTilePoolCapacity = Defaults::MegaLightsTilePoolCapacity;
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
        std::unique_ptr<RHI::IRHIShader> m_MegaLightsAccumComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MegaLightsAccumPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsAccumConstantBuffer;
        // 1画素につきfloat4。レイトレーシング非対応の環境では、Presentがt6へ張るための
        // 1要素だけのダミーになる(DX12はPSO切替でルート引数が無効化されるため、
        // シェーダが宣言しているリソースは必ず何かをバインドする必要がある)
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsAccumBuffer;
        // これまでに足したフレーム数。表示側はこれで割る
        uint32_t m_MegaLightsAccumFrames = 0;
        // レンダーターゲットを作り直してから何フレーム経ったか。
        //
        // 【整定を待たずに足し始めると測定そのものが壊れる】起動直後は内部解像度が
        // 既定値(1280x720)から実際のウィンドウサイズへ切り替わり、モデルとテクスチャも
        // ストリーミングで入ってくる。その間の絵を混ぜて平均すると、**別のシーンの平均**を
        // 測ることになる。実際に、待たずに書き出したときは1280x720のまま吐き出された
        uint32_t m_MegaLightsAccumWarmupFrames = 0;
        // 何フレーム待ってから足し始めるか。小さなシーンの読み込みとリサイズが片付く目安
        static constexpr uint32_t kMegaLightsAccumWarmup = 180;
        // 何フレーム足したら止めるか。0なら蓄積そのものを行わない。
        // **止めることに意味がある** ―― 止めれば表示が静止し、「ちょうどNサンプルの平均」を
        // 決定的に撮れる(1/√Nで誤差が下がるかを測るのに要る)
        int32_t m_MegaLightsAccumTargetFrames = 0;
        // 蓄積し終えた平均をこのパスへ生データで書き出す(空なら書き出さない)。
        //
        // 【なぜ画面キャプチャでは足りないのか】画面から採れるのは8bitで、しかも
        // トーンマップを通っている。ここで測りたいのは「平均が真値へ 1/√N で寄るか」で、
        // 8bitの丸めだけでRMSEに0.29階調の下限が生まれ、その下限に隠れて比が読めなくなる。
        // 物差しの分解能が足りないまま「1/√Nで落ちていない」と読むと、原因を取り違える
        std::wstring m_MegaLightsDumpPath;
        std::unique_ptr<RHI::IRHIBuffer> m_MegaLightsAccumReadback;
        // コピーを積んだフレーム番号(0なら未発行)。GPUの実行はCPUより数フレーム遅れるので、
        // 積んだ直後に読んではいけない(IRHICommandList::CopyBufferToReadback のコメント)
        uint32_t m_MegaLightsDumpCopyFrame = 0;
        bool m_MegaLightsDumpIssued = false;
        bool m_MegaLightsDumpDone = false;
        // --- GPU計測の書き出し(計測専用) ---
        std::wstring m_PerfDumpPath;
        int32_t m_PerfDumpTargetFrames = 0;
        int32_t m_PerfDumpWarmupFrames = 0;
        int32_t m_PerfDumpCollected = 0;
        bool m_PerfDumpDone = false;
        // パス名 -> 合計時間[ms]。同じ名前のパスが1フレームに複数あるぶんも足し込む
        // (a-trousは段の数だけ同名で登録される。**合計が知りたいので足すのが正しい**)
        std::map<std::string, double> m_PerfDumpTotals;

        // --- 雲(低解像度の専用パス) ---
        // Lightingパスの直前に置くフルスクリーン三角形+ピクセルシェーダー。積雲と巻雲だけを
        // 内部レンダー解像度の1/2(面積で1/4)で評価し、「透過率 + 事前乗算済みの散乱光」を書く。
        // Lightingパスの背景分岐がこれをバイリニアで引いて
        // SkyColorWithoutClouds(rayDir) * a + rgb を合成する。
        // 分離の根拠と、太陽・星がフル解像度のまま保たれる理由はShaders/3D/SkyCloud.hlsl冒頭を参照
        std::unique_ptr<RHI::IRHIShader> m_SkyCloudVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_SkyCloudPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SkyCloudPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SkyCloudTexture;
        // m_SkyCloudTextureの実寸(内部レンダー解像度を割った後の値。奇数解像度の切り捨てと
        // 最低1pxの下限があるため、割り算をその場でやり直さずここへ保存する)。
        // パスのビューポート指定に使う
        uint32_t m_SkyCloudWidth = 0;
        uint32_t m_SkyCloudHeight = 0;

        // --- DDGIの低解像度解決パス ---
        // 雲と同じくLightingパスの直前に置くフルスクリーン三角形+ピクセルシェーダー。
        // DDGIの拡散イラディアンスだけを内部レンダー解像度の1/2(面積で1/4)で評価し、
        // 「rgb=イラディアンス, a=insideWeight」を書く。
        //
        // 【雲と違い近似である】雲は視線方向だけの関数で深度に依存しないため、
        // 低解像度化してバイリニアで引き伸ばしても数学的に等価だった。DDGIは面の位置と
        // 法線の関数なので、ジオメトリの輪郭をまたぐと手前の間接光が奥へ滲む。
        // 合成側(DeferredLighting.hlslのUpsampleDDGI)が深度を見たアップサンプルで
        // 抑えているが、厳密ではない。そのため**既定では無効**にしてある
        std::unique_ptr<RHI::IRHIShader> m_DDGIResolveVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_DDGIResolvePixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_DDGIResolvePipelineState;
        std::unique_ptr<RHI::IRHITexture> m_DDGIResolveTexture;
        // 上のパスが2枚目のレンダーターゲットへ書く低解像度の深度(41.24節)。
        // 合成側のバイラテラルアップサンプルがGatherRed 1回で4テクセルぶんを取るために使う
        std::unique_ptr<RHI::IRHITexture> m_DDGIResolveDepthTexture;
        // m_SkyCloudWidth/Heightと同じ理由でここへ保存する(パスのビューポート指定に使う)
        uint32_t m_DDGIResolveWidth = 0;
        uint32_t m_DDGIResolveHeight = 0;
        // DDGIを低解像度パスから引くか。実測(ProbeTest / 1280x720 / DX11)では
        // Lightingパス23.9msのうちDDGIのサンプリングが10.2msを占めていた
        bool m_DDGIHalfResolution = Defaults::DDGIHalfResolution;

        // --- 大気遠近(height fog / aerial perspective) ---
        // 反射パス(SSR/RT反射)の後、TAAパスの直前に置くフルスクリーン三角形+ピクセルシェーダー。
        // Lightingパスの中へ入れない理由・TAAより前へ置く理由はShaders/3D/AerialPerspective.hlsl
        // 冒頭のコメント参照。無効時(m_FogEnabled=falseまたはm_FogDensity<=0)はパス自体を
        // 登録せず、GetActiveReflectionOutput()の結果がそのままTAA(またはTonemap)へ渡る
        std::unique_ptr<RHI::IRHIShader> m_AerialPerspectiveVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_AerialPerspectivePixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_AerialPerspectivePipelineState;
        std::unique_ptr<RHI::IRHITexture> m_AerialPerspectiveTexture;
        bool m_FogEnabled = Defaults::FogEnabled;
        // 基準高度(m_FogRefHeight)での消散係数[1/m]。AerialPerspective.hlsl/PlanarReflection.hlslの
        // FogParams0.xへ渡る
        float m_FogDensity = Defaults::FogDensity;
        // スケールハイト[m]。大きいほど霞が高くまで及ぶ(HeightFog.hlsli参照)
        float m_FogScaleHeight = Defaults::FogScaleHeight;
        // 基準高度[m](ワールドY)。既定は水面の高さに合わせている
        float m_FogRefHeight = Defaults::FogRefHeight;
        // 不透明度の上限(1.0で遠方が完全に空の色まで行く)
        float m_FogMaxOpacity = Defaults::FogMaxOpacity;
        // 水中項。Water.hlslのPSMainがメッシュ自身のBaseColorFactorの代わりにこの色を
        // 出力Albedoに使う(見下ろした水面がFresnel最小でほぼ真っ黒になる問題への対処。
        // 干潟の水の色はシーン側で調整したいパラメータであり、.kmodelを焼き直さずに変えられるようにするため)
        DirectX::XMFLOAT3 m_WaterBodyColor{
            Defaults::WaterBodyColorR, Defaults::WaterBodyColorG, Defaults::WaterBodyColorB
        };

        // TAA(Temporal Anti-Aliasing)パス: SSRの後、露出/ブルーム/トーンマップの前に置く。
        // 毎フレーム投影行列を1ピクセル未満だけずらして(ジッター)サンプル位置を散らし、
        // モーションベクターで前フレームの結果を今フレームの画素へ再投影して蓄積する。
        // 静止していれば十数フレームで収束し、実質的なスーパーサンプリングになる。
        // 詳細な原理と各工夫の理由はArchitecture.htmlのTAAの章を参照
        std::unique_ptr<RHI::IRHIShader> m_TAAVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_TAAPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_TAAPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_TAAConstantBuffer;
        // 履歴バッファ2枚。読みながら同じテクスチャへ書けないため役割を毎フレーム入れ替える。
        // m_TAAHistoryIndexが今フレームの書き込み先で、もう一方が前フレームの結果(=履歴)。
        // このパスの出力がそのまま後段(自動露出/ブルーム/トーンマップ)の入力にもなる
        std::unique_ptr<RHI::IRHITexture> m_TAAHistory[2];
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
        bool m_TAAEnabled = Defaults::TAAEnabled;
        // 今フレームの色を履歴へ混ぜる割合。小さいほど収束後は滑らかだが、
        // 遮蔽が変わったときの追従が遅くなる
        float m_TAABlendWeight = Defaults::TAABlendWeight;
        // ジッターの振れ幅の倍率(1.0でピクセル内いっぱい)。0にするとジッターが無くなり、
        // 時間方向のスーパーサンプリング効果だけが消える(再投影と蓄積は残る)
        float m_TAAJitterScale = Defaults::TAAJitterScale;
        // 蓄積によるボケを補うシャープネス。TAAの中ではなくTonemapパスで最終出力にのみ掛ける。
        // TAAの入力へ掛けるとアンシャープマスクが「ジッターで変動する高域」を増幅し、
        // ちらつきが実測で約53%増える(Architecture.html 23.7節)
        float m_TAASharpness = Defaults::TAASharpness;
        // 近傍クリップのボックス幅(近傍の標準偏差の何倍まで履歴を許容するか)。
        // 小さいほどゴーストに強いがちらつきが増え、大きいほどその逆になる。
        // これは「動いている画素」に適用される値で、静止した画素ではm_TAAAntiFlickerに応じて広がる
        float m_TAAClipGamma = Defaults::TAAClipGamma;
        // 静止している画素に限ってブレンド率を下げ、近傍クリップのボックスを実質無効まで広げる量。
        // 速度が0の画素では再投影誤差が原理的に起きないためクリップは害にしかならず、
        // 一方でちらつきはブレンド率とクリップの両方から出る。動いている画素の挙動は
        // 一切変えないため、ゴーストの出方はこの機能を切ったときと同じままになる。
        // 0で無効(この機能を入れる前の挙動に戻る)
        float m_TAAAntiFlicker = Defaults::TAAAntiFlicker;
        // 近傍クリップの方式。TAA.hlsl側のclipModeと値を一致させること
        // (TonemapCurveと同じく、列挙の既定値はEngineDefaults.hではなくここへ直接書く)
        enum class TAAClipMode : int32_t
        {
            None = 0,     // クリップしない(切り分け測定用。ゴーストが激しく出るので常用しない)
            Variance = 1, // 近傍の平均±(標準偏差×ClipGamma)のみ
            Clamped = 2,  // 上記と近傍の実在min/maxとの積集合(最も狭く、最もゴーストに強い)
        };
        TAAClipMode m_TAAClipMode = TAAClipMode::Clamped;

        // Tonemapパス: SceneColor(SSR有効時はm_SSRTexture)のHDR値をReinhardトーンマッピング+
        // ガンマ補正でLDRへ変換し、Presentパスへ渡す。SSR等のHDR演算より後、Present直前の
        // 独立したステージとして置くことで、反射や将来のブルーム/露出制御(M7)がトーンマップの
        // 影響を受けないHDR値の上に成立できるようにする
        std::unique_ptr<RHI::IRHIShader> m_TonemapVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_TonemapPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_TonemapPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_TonemapTexture;
        std::unique_ptr<RHI::IRHIBuffer> m_TonemapConstantBuffer;

        // 超解像パス(Upscale.hlsl): Tonemapが出したLDR画像を、EASUで出力解像度へ再構成し、
        // RCASでシャープ化してからPresentへ渡す。2つのテクスチャはどちらも出力解像度で、
        // 内部解像度用のm_TonemapTextureとは作り直す契機が違うためCreateRenderTargets()の外にある。
        // 分けているのはRCASがEASUの結果を読むためで、同一リソースのSRV/UAV同時バインドを避ける
        std::unique_ptr<RHI::IRHIShader> m_UpscaleEASUComputeShader;
        std::unique_ptr<RHI::IRHIShader> m_UpscaleRCASComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_UpscaleEASUPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_UpscaleRCASPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_UpscaleTexture;      // EASUの出力
        std::unique_ptr<RHI::IRHITexture> m_UpscaleSharpTexture; // RCASの出力(Presentが読む)
        std::unique_ptr<RHI::IRHIBuffer> m_UpscaleConstantBuffer;

        // トーンマッピングカーブ。Tonemap.hlsl側のCurveと値を一致させること
        enum class TonemapCurve
        {
            Reinhard, // c/(c+1)。比較用のリファレンスカーブ
            ACES,     // Narkowicz 2015のフィット近似
            AgX,      // Troy Sobotka の AgX(Filament/three.jsの実装形)
        };
        // 既定をAgXにしている理由: ACESは飽和した明るい色の色相がシフトする(赤がオレンジへ寄る)
        // ことが知られており、Bistro内観のように赤い壁が支配的なシーンでその欠点が最も出やすい。
        // AgXはハイライトが色相を保ったまま白へ脱色するため、この用途では素直な絵になる
        TonemapCurve m_TonemapCurve = TonemapCurve::AgX;
        // 黒の締め(ブラックポイント)。0で恒等。詳細はShaders/3D/Tonemap.hlslのコメント参照
        float m_TonemapBlackPoint = Defaults::TonemapBlackPoint;

        // 薄明視(mesopic vision)の適用量。0で無効、1で完全適用。
        //
        // 暗所では錐体が働かなくなり桿体だけの視覚に移る。桿体は1種類しか無いので色を
        // 判別できず、実際の月明かりの下では「形は見えるのに色がほとんど無い」見え方になる。
        // 露出を下げるだけでは「暗いが色鮮やかな夜」にしかならず、肉眼で見た夜と一致しない。
        // 桿体の分光感度が短波長寄り(507nm)であることから来るプルキンエ現象も同時に入る
        // (詳細はTonemap.hlsl の ApplyMesopicVision)。
        // 既定は無効。効果が強く画作りの好みが分かれるため、使うときに明示的に上げる
        float m_MesopicStrength = Defaults::MesopicStrength;

        // 出力8bit量子化の直前に加えるディザリング。実測(Bistro Interior)では走査線上に
        // 同一色が24px連続しており、これは中間バッファをHDR化しても変わらなかった。
        // つまり暗部のバンディングの主因は最終8bit量子化であり、ここでしか直せない。
        // 効果をA/B比較できるようトグルにしてある
        bool m_DitherEnabled = Defaults::DitherEnabled;

        // 自動露出(eye adaptation)パス: SceneColorの輝度ヒストグラムをGPUで作り、
        // 低/高パーセンタイルを除外した加重平均から目標EV100を求めて時間方向に追従させる。
        // 結果はm_ExposureTextureへ書かれ、Tonemapパスが読んで露出倍率に変換する。
        //
        // 露出そのものはCPU側でライト強度へ事前乗算されている(プリ露出方式、
        // m_SceneExposureEV100)。自動露出の結果をライト強度へ戻すとフィードバックループになり、
        // かつGPU→CPUのリードバック(同期待ち)が要るため、プリ露出は固定のままにして
        // 「プリ露出EVと自動露出EVの差」だけをTonemapで掛ける構成にしている
        // (詳細はAutoExposure.hlsl冒頭)
        std::unique_ptr<RHI::IRHIShader> m_AutoExposureClearComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_AutoExposureClearPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_AutoExposureHistogramComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_AutoExposureHistogramPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_AutoExposureResolveComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_AutoExposureResolvePipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_ExposureHistogramBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_AutoExposureConstantBuffer;
        // 2x1のR32_Float。texel(0,0)=平滑化後のEV100、texel(1,0)=初期化済みフラグ。
        // フレームをまたいで保持する必要があるためCreateRenderTargetsではなく一度だけ作る
        // (ウィンドウリサイズで作り直すと順応がリセットされてしまうため)
        std::unique_ptr<RHI::IRHITexture> m_ExposureTexture;
        // 輝度ヒストグラムのビン数。AutoExposure.hlslのHISTOGRAM_BINSと一致させること
        static constexpr uint32_t kExposureHistogramBins = 256;

        bool m_AutoExposureEnabled = Defaults::AutoExposureEnabled;
        // 露出のクランプ範囲(EV100)。ヒストグラムのビン割りもこの範囲で行うため、
        // 実シーンの輝度がこの外に出ると端に張り付く
        // 下限-6は月夜の地表(反射率0.2の面で約0.016 cd/m^2 = EV100約-3)を余裕をもって含む値。
        // 星明かりだけの夜まで追うならさらに下げる必要があるが、実写の夜景もEV -3〜-5程度で
        // 撮るのが普通なので実用上はここで足りる。
        // 上限18は、正規化後の昼の空(約6400 cd/m^2 = EV100約15.6)に余裕を持たせた値
        float m_AutoExposureMinEV100 = Defaults::AutoExposureMinEV100;
        float m_AutoExposureMaxEV100 = Defaults::AutoExposureMaxEV100;
        // 明順応(暗→明)と暗順応(明→暗)の速度。人間の目は暗順応のほうが遅いため既定値も分けている
        float m_AutoExposureSpeedUp = Defaults::AutoExposureSpeedUp;
        float m_AutoExposureSpeedDown = Defaults::AutoExposureSpeedDown;
        // 加重平均から除外する下側/上側の累積割合。暗すぎる画素・明るすぎる画素に露出が
        // 引きずられるのを防ぐ
        float m_AutoExposureLowPercentile = Defaults::AutoExposureLowPercentile;
        float m_AutoExposureHighPercentile = Defaults::AutoExposureHighPercentile;
        // 測定結果に対してユーザーが意図的に足すオフセット(EV)
        float m_AutoExposureCompensation = Defaults::AutoExposureCompensation;
        // 暗いシーンをわざと暗いまま写すための補正量[EV]。
        // 自動露出は測ったものを中庸なグレーへ持ち上げるので、これが0だと夜が昼と同じ明るさで
        // 出てしまう(実測: 補正なしでは22時と12時の空の明度がほぼ一致する)。
        // 実写でも夜景はわざと露出を切り詰めて撮るため、既定で4.5段暗くする。
        // 既定値は「肉眼で見た月明かりの夜」に合わせて実測で決めた
        // (m_MesopicStrength=1のときの、月光を受ける壁 / 夜空の8bitコード):
        //   3.5段 … 壁19 / 空70  形も質感もはっきり読め、夜というより夕暮れ寄り
        //   4.5段 … 壁 6 / 空44  空が一番明るく、建物は輪郭と影がかろうじて読める ← 既定
        //   5.5段 … 壁 2 / 空23  建物がほぼ完全に沈み、空しか見えない
        //
        // **m_MesopicStrengthとセットで意味を持つ**点に注意。露出を下げるだけでは
        // 「暗いが色鮮やかな夜」にしかならず、肉眼で見た夜と一致しない。
        //
        // 0にすると「常に中庸なグレーへ合わせる」挙動になる。
        //
        // **m_AutoExposureKeyCeilingEVとセットで意味を持つ値**である点に注意。
        // 上のクランプが無いと測光値が構図で2〜3.5段振れるので、この値をいくつにしても
        // カメラの向きで夜の明るさが変わってしまう
        float m_AutoExposureNightRolloffEV = Defaults::AutoExposureNightRolloffEV;
        // 補正カーブの折れ点[EV100]。測定値がDark以下で補正量が最大、Bright以上で0、間は線形。
        // Darkの-2は満月の夜の地表(反射率0.2の面で約0.016 cd/m^2 = EV100約-3)のすぐ上、
        // Brightの10は曇天の屋外あたりで、日中は補正が掛からない値にしてある
        float m_AutoExposureNightRolloffDarkEV100 = Defaults::AutoExposureNightRolloffDarkEV100;
        float m_AutoExposureNightRolloffBrightEV100 = Defaults::AutoExposureNightRolloffBrightEV100;
        // 測光値がキー照度の基準EV(ComputeReferenceEV100。構図に依存しない)から
        // 何段上まで行くのを許すか[EV]。十分大きな値(16など)で無効になる。
        //
        // 【位置づけ】構図で露出が振れる問題そのものは、AutoExposure.hlslで
        // **空を測光から外した**ことで根本的に解決している(21.9.8節)。
        // こちらは残った病的なケースへの保険で、通常は発動しない:
        // 夜の街で明るい看板が画面の大半を占めるようなとき、明るい側に寄った測光範囲
        // (50〜95パーセンタイル)がその看板に支配され、街並みが黒く沈むのを防ぐ。
        //
        // **上側だけを止める**のは、屋内のように実際の輝度が屋外のキー照度よりずっと低い
        // シーンでは測光値が下へ振れるのが正しいため(両側を締めると屋内が真っ暗になる)。
        // 下側はm_AutoExposureMinEV100が絶対的な下限として効く。
        //
        // 既定の+2は「通常のシーンでは発動しないが、極端なケースは止まる」余裕を見た値。
        // 測光から空を外してある(AutoExposure.hlsl)ため、-1のような強い値にすると締めすぎになる
        float m_AutoExposureKeyCeilingEV = Defaults::AutoExposureKeyCeilingEV;

        // 次のAutoExposureパスで順応を飛ばして測光値へ即座に合わせる要求。LoadSceneが立て、
        // パスを積んだ時点で消費する。
        //
        // 順応の状態はGPU側のm_ExposureTexture(2x1)に入っており、初回だけ順応を飛ばすための
        // フラグもそこのテクセル(1,0)にある(UAVがゼロ初期化されることを利用している)。
        // つまりCPU側からは「初回に戻す」手段が無く、シーンを切り替えても前のシーンの露出から
        // 順応が続いてしまう。シーン切り替えは視点の移動ではなく場面の切り替わりなので、
        // 目の順応を模す理由が無い(AutoExposure.hlslのCSResolveのコメントもそう宣言している)
        bool m_AutoExposureResetRequested = false;

        // ブルームパス(Bloom.hlsl): 半解像度から始まるピラミッドを段階的にダウンサンプルし、
        // 3x3テントで戻しながら加算することで広く滑らかな光の裾を作る。
        //
        // ピラミッドをミップチェーン1枚ではなくレベルごとの独立テクスチャで持っているのは、
        // 同一リソースのSRV/UAV同時バインドを避けるため(理由の詳細はBloom.hlsl冒頭)。
        // m_BloomDownTexturesがダウンサンプル結果、m_BloomUpTexturesがアップサンプルの累積で、
        // 最終的にm_BloomUpTextures[0](半解像度)をTonemapパスが読む
        std::unique_ptr<RHI::IRHIShader> m_BloomDownsampleComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_BloomDownsamplePipelineState;
        std::unique_ptr<RHI::IRHIShader> m_BloomUpsampleComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_BloomUpsamplePipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_BloomConstantBuffer;
        std::vector<std::unique_ptr<RHI::IRHITexture>> m_BloomDownTextures;
        std::vector<std::unique_ptr<RHI::IRHITexture>> m_BloomUpTextures;
        // ピラミッドの段数。半解像度を第0段として、これ以上小さくしても見た目が変わらない範囲で選ぶ
        static constexpr uint32_t kBloomLevelCount = 6;
        // 各段の解像度(CreateRenderTargetsで内部解像度から決まる)
        std::vector<DirectX::XMUINT2> m_BloomLevelSizes;

        bool m_BloomEnabled = Defaults::BloomEnabled;
        // 最終合成の混合比。エネルギー保存のため加算ではなくlerpで混ぜるので、
        // 物理的にレンズ散乱が持ち去る割合(数%)に相当する小さい値が既定になる
        float m_BloomStrength = Defaults::BloomStrength;
        // しきい値は既定で十分低くしてある(物理的にはブルームは全輝度に掛かるのが正しい)。
        // アート制御として上げられるようにだけしてある
        float m_BloomThreshold = Defaults::BloomThreshold;
        float m_BloomSoftKnee = Defaults::BloomSoftKnee;

        // 垂直同期。既定で無効。有効にするとPresentがvblankまでブロックするため、GPU負荷が軽い
        // シーンではvsync待ちの間GPUがアイドル→省電力クロックに落ち、次フレームの立ち上がりが
        // 遅くなる・待ち時間自体もジッタで1vblank/2vblank分を行き来するなど計測値が不安定になる。
        // 既定はGPU/CPU双方の実処理時間を素直に見られるOFFとし、ティアリングを許容する
        // (ON時はPresentが即座に返らず、モニタのリフレッシュレートにFPSが制限される)
        bool m_VSyncEnabled = Defaults::VSyncEnabled;

        // 固定FPSモード。有効時、Renderスレッドが目標FPSより速く回った分だけ待機してフレーム間隔を
        // 一定に保つ。VSyncはモニタのリフレッシュレート依存かつティアリング防止が目的だが、こちらは
        // 任意のFPS値に固定できる(物理更新の再現性確保や環境間でのフレーム時間比較などが目的)。
        // 既定で60fps固定を有効にする
        bool m_FixedFPSEnabled = Defaults::FixedFPSEnabled;
        float m_TargetFPS = Defaults::TargetFPS;

        // Presentパス(選択中のレンダーターゲットをアスペクト比を保ってバックバッファへ拡大縮小表示)
        std::unique_ptr<RHI::IRHIShader> m_PresentVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_PresentPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PresentPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_PresentConstantBuffer;

        // デバッグ表示用: Presentパスで最終的に表示するレンダーターゲットの種類
        enum class DebugView
        {
            Final,
            Albedo,
            Normal,
            Material,
            Emissive,
            Depth,
            DepthRaw,           // 深度テクスチャの生値(0〜1)を加工せずそのままグレースケール表示
            DirectLight,        // DirectLightingパスの結果(HDR、シャドウ適用済みの直接光)をトーンマッピングして表示
            AOIndirectLight,    // AO/GIバッファのrgb(間接拡散光、ブラー後)をそのまま表示
            AOIndirectLightRaw, // AO/GIバッファのrgb(間接拡散光、ブラー前の生値)
            AOOcclusion,        // AO/GIバッファのa(遮蔽率、ブラー後)をグレースケール表示
            AOOcclusionRaw,     // AO/GIバッファのa(遮蔽率、ブラー前の生値)
            ShadowMap,          // m_ShadowDebugCascadeで選択したカスケードのシャドウマップを表示
            RTShadow,           // RTシャドウの可視率(0=影, 1=光)をグレースケール表示。RTシャドウ未実行時は最終結果
            SSR,                // 反射パスの出力(SceneColor+反射)。反射がOffのときはSceneColorと同一
            HiZ,                // Hi-Zミップチェーンの指定ミップ(m_HiZDebugMipLevel)をグレースケール表示
            IBLIrradiance,      // IBL拡散イラディアンスマップ(TextureCube。現在の視線方向で球面を見回す表示)
            IBLPrefilter,       // IBLプリフィルタ済み鏡面マップの指定ミップ(m_IBLPrefilterDebugMipLevel、TextureCube)
            IBLBRDFLUT,         // IBL BRDF積分LUT(x=NdotV, y=ラフネス。R=A, G=B, B=Eavg)
            Bloom,              // ブルームのピラミッド最上段(半解像度、HDR)をトーンマッピングして表示
            LightTiles,         // タイルライトカリングのライトグリッド(タイルあたりのライト数)をヒートマップ表示
            // 反射プローブは鏡面専任なので拡散イラディアンスの表示は持たない(拡散はDDGIIrradiance)
            ProbePrefilter,     // 反射プローブのプリフィルタ済み鏡面(ミップ0がキャプチャ結果そのもの)
            ProbeInfluence,     // どのプローブが効いているかをプローブ番号ごとの色で塗り分けて表示
            ProbeDistance,      // 反射プローブの距離キューブ(プローブから見た各方向の被写体までの距離)
            MotionVector,       // モーションベクター(速度バッファ)。静止で灰色、動くと移動方向に応じて色が付く
            SceneColorRaw,      // トーンマップ前のHDRシーンカラーをリニアのまま無加工で表示(測定用)
            DDGIIrradiance,     // DDGIのイラディアンスアトラス(オクタヘドラル2D、22章)
            DDGIDistance,       // DDGIの距離モーメントアトラス(R=平均距離、G=平均二乗距離)
            BentNormal,         // bent normal(34章)。Debug View Gainが1なら軸を色表示、
                                // 1.5より大きいと長さ(=aoB)をグレースケール表示。
                                // データを持たないマテリアルはマゼンタで塗る
            WaterMask,          // G-BufferのMaterial.a(水面のマテリアルID)をグレースケール表示
            PlanarReflection,   // 平面反射パスの出力(m_PlanarReflectionColor)をトーンマッピングして表示
            CloudNoiseSlice,    // 雲の3Dノイズの任意スライス。m_CloudNoiseDebugSlice/Detailで選ぶ
            AtmosphereLUT,      // 大気散乱のLUT。m_AtmosphereLUTDebugMultiで2枚を切り替える
            DDGIProbeBackface,  // DDGIのプローブ裏面率(イラディアンスアトラスのα、22章)。
                                // 白いほど「面の裏側ばかり見ている」=壁の内部に埋まっている。
                                // 分類のしきい値を実測で決めるための表示。ラスタ経路では常に黒
            // 以下3つは自前ソフトウェアラスタライザ(46章)の出力。パスが実行されていない
            // フレームでは中身が前フレーム/未定義の残骸なので、最終結果のまま切り替えない
            SoftwareRaster,       // ソフトウェアラスタライザのフラットな陰影(HDR)
            SoftwareRasterDepth,  // 同 深度(生値)。DebugView::DepthRawと並べて差分を取る
            SoftwareRasterNormal, // 同 法線。DebugView::Normalとまったく同じ符号化・同じ表示
            // MegaLightsパスが書いたポイント/スポットライトの直接光(トーンマップして表示)。
            // 上の3つと同じく、パスが実行されていないフレームでは中身が前フレーム/未定義の
            // 残骸なので、最終結果のまま切り替えない
            MegaLights,
            // MegaLightsの候補プールが数えた「そのタイルへ届いたライト数」。
            // 色付けは DebugView::LightTiles とまったく同じで、両者は同じ判定を使うので
            // 同じシーン・同じカメラなら画素単位で一致するはず(定義域のずれの検出用)
            MegaLightsTilePool,
            // MegaLightsの出力を線形空間で蓄積した平均(計測専用)。参照実装と確率的サンプリングの
            // これどうしを比べて、平均が真値へ寄るかを測る。蓄積が無効なら最終結果のまま
            MegaLightsAverage,
        };
        // デバッグ表示の総数。**enumの末尾を足したらここも直すこと**。
        // enumのすぐ隣に置いてあるのは、離れた場所にあると更新を忘れるため
        // (実際に DDGIProbeBackface を足したとき、範囲チェックが古い末尾のままで
        //  起動オプションからの選択が弾かれた)
        static constexpr int kDebugViewCount = static_cast<int>(DebugView::MegaLightsAverage) + 1;

        DebugView m_DebugView = DebugView::Final;
        // デバッグ表示の輝度倍率(Present.hlslのGain)。AO/GIバッファの間接拡散光のように
        // 値そのものが小さいバッファ(この暗い室内では0.02〜0.1程度)は、等倍で表示しても
        // ほぼ真っ黒で階調の粗さが判別できない。持ち上げて表示することで、8bit格納時の
        // ポスタリゼーションが何段あるかを目視で確認できるようにする。
        // 色として表示するモード(Present.hlsl Mode 0/3/4)にのみ効く
        float m_DebugViewGain = Defaults::DebugViewGain;
        // DebugView::CloudNoiseSlice で表示する3Dノイズのスライス位置(0〜1、W方向)と、
        // 形状(128^3)とディテール(32^3)のどちらを見るか。タイル境界に継ぎ目が出ていないかを
        // 目と数値の両方で確認するために用意してある
        float m_CloudNoiseDebugSlice = 0.0f;
        bool m_CloudNoiseDebugShowDetail = false;
        // DebugView::AtmosphereLUT で表示するLUT
        // (0=Transmittance、1=MultiScattering、2=SkyView)
        int m_AtmosphereLUTDebugIndex = 0;

        // シャドウパス(平行光のライト視点から深度のみを描画する)。カメラ視錐台をkCascadeCount個の
        // 深度範囲に分割し(Practical Split Scheme)、それぞれ専用の正射影・シャドウマップを持たせる
        // カスケードシャドウマップ(CSM)。近いカスケードほどテクセル密度が高く、遠いカスケードほど
        // 広い範囲を粗くカバーする
        static constexpr uint32_t kShadowMapSize = 2048;
        std::unique_ptr<RHI::IRHIShader> m_ShadowVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ShadowPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowPipelineStateMirrored;
        // メッシュシェーダー版のシャドウ(Shaders/3D/ShadowMeshlet.hlsl)。
        // 非対応環境ではすべてnullptrのままで、描画側は従来のメッシュ単位経路を使う
        std::unique_ptr<RHI::IRHIShader> m_ShadowAmplificationShader;
        std::unique_ptr<RHI::IRHIShader> m_ShadowMeshShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowMeshletPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowMeshletPipelineStateMirrored;
        // アルファカットアウト(glTFのalphaMode=MASK)の影。ピクセルシェーダーは
        // 頂点シェーダー経路とメッシュシェーダー経路で共有する。
        // **DX11でも効く**(bindlessもメッシュシェーダーも要らない)
        std::unique_ptr<RHI::IRHIShader> m_ShadowCutoutVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ShadowCutoutPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowCutoutPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowCutoutPipelineStateMirrored;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowMeshletCutoutPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowMeshletCutoutPipelineStateMirrored;
        // 全カスケードの深度を1つのTexture2DArray(スライス番号=カスケード番号)として保持する。
        // 書き込みはスライスごとの個別DSV(RenderGraphPassDesc::DepthTargetArraySlice)で行い、
        // 読み取りは配列全体を指す1本のSRV(t4)を1回バインドするだけでよい。シェーダ側は
        // ShadowMapArray.Sample(DataSampler, float3(uv, cascadeIndex))で動的にカスケードを選べる
        // (ShadowSampling.hlsli参照)
        std::unique_ptr<RHI::IRHITexture> m_ShadowCascadeArray;
        // シャドウパスの各カスケード描画で使う専用の定数バッファ(カスケードごとに値を更新して使い回す)
        std::unique_ptr<RHI::IRHIBuffer> m_ShadowCascadeConstantBuffer;

        // 太陽(平行光)の影の手法。値はDirectLighting.hlslのLightingConstants.LightCount.zへ
        // そのまま渡すため、シェーダ側の分岐と番号を一致させること
        enum class ShadowMode
        {
            Off,                // 影を落とさない
            CascadedShadowMap,  // カスケードシャドウマップ+PCSS(ShadowSampling.hlsli)
            Raytraced,          // RTシャドウ(RTShadow.hlsl)。DX12かつDXR Tier 1.1が要る
        };
        // 現在の手法。RaytracedはSupportsRaytracing()がtrueの環境でしか選べない
        // (UI側で選択不可にし、シーン読み込み時にも非対応ならCascadedShadowMapへ落とす)。
        //
        // 【重要】Raytracedでもシャドウパス(CSMの描画)はスキップしない。半透明
        // (Transparent.hlsl)と反射プローブのキャプチャ(ProbeCapture.hlsl)は
        // カメラ視点の画面空間テクスチャを使えず、CSMのシャドウマップを必要とするため
        // (RTシャドウは不透明サーフェスの直接光パスだけを置き換える。26章)
        //
        // 既定の手法はDefaultShadowModeが決める(反射のDefaultReflectionModeと同じ理由で1か所に置く)。
        // ここの初期値はm_RaytracingAvailableが確定する前の値でしかなく、
        // 実際の既定はシーン読み込み時に決め直される
        // 【反射と違い「出すか」と「どの手法か」を分けていない】Defaults::ShadowEnabledがtrueで
        // あるため、シーンがShadow = trueと書いたときにこの関数へ問い合わせても
        // 手法の選択と同じ結果になるため問題にならない。ただし構造は反射と同じ危うさを持つ
        // (DefaultReflectionModeのコメント参照)ので、Defaults::ShadowEnabledを
        // falseにするなら反射と同じ形(ShadowModeForCapabilityへの分割)へ直すこと
        static constexpr ShadowMode DefaultShadowMode(bool raytracingAvailable)
        {
            if (!Defaults::ShadowEnabled)
            {
                return ShadowMode::Off;
            }
            return raytracingAvailable ? ShadowMode::Raytraced : ShadowMode::CascadedShadowMap;
        }
        ShadowMode m_ShadowMode = DefaultShadowMode(false);
        // PCSS(Percentage Closer Soft Shadows)のライトサイズ。シャドウマップUV空間での
        // ブロッカーサーチ・半影の広さを決める係数(値が大きいほど半影が広く柔らかくなる)
        float m_ShadowLightSize = Defaults::ShadowLightSize;
        // デバッグ表示(Render Targets - Shadow Map)で確認するカスケード番号(0=カメラに近い方)
        int32_t m_ShadowDebugCascade = 0;

        // 太陽(平行光)そのものの有効/無効。.ksceneの[Sun]Enabledで設定される。
        // TimeOfDayを夜にすると昼度(AmbientColor.a)も一緒に落ちて環境光まで消えてしまうため、
        // 「昼のまま太陽だけ消す」にはこちらを使う(White Furnace Testが必要とする)。
        // 無効時はFrameConstants.LightColorをゼロにするだけでよく、シェーダー側の変更は不要
        bool m_SunEnabled = Defaults::SunEnabled;

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
        // 焼いてm_IBLBakedを立て、以降は焼き直さない(詳細はdocs/Architecture.html参照)。
        // 拡散イラディアンス・プリフィルタ済み鏡面はいずれも本物のTextureCube
        // (CreateUAVTextureCube/CreateMippedUAVTextureCube、面ごとに個別のUAVを持つ)で、
        // IBLConvolve.hlslが面ごとに1回ずつディスパッチして書き込む
        // キューブマップの面数(D3D標準順: +X,-X,+Y,-Y,+Z,-Z)。IBLの2つのキューブマップは
        // いずれもこの順で面ごとにディスパッチする(IBLConvolve.hlsl CubeFaceDirectionと一致させる)
        static constexpr uint32_t kCubeFaceCount = 6;
        static constexpr uint32_t kIBLIrradianceSize = 32;
        static constexpr uint32_t kIBLPrefilterBaseSize = 128;
        // プリフィルタ済み鏡面マップのミップ数(128,64,32,16,8,4の6段)。ラフネス[0,1]を
        // [0, kIBLPrefilterMipLevels-1]のミップ番号へ線形マッピングする(DeferredLighting.hlsl参照)
        static constexpr uint32_t kIBLPrefilterMipLevels = 6;
        static constexpr uint32_t kIBLBRDFLUTSize = 128;
        // ボリュメトリック雲の3Dノイズの1辺のテクセル数。
        // Shapeは128^3のRGBA8で8MB、Detailは32^3のRGBA8で128KB。合わせて約8.1MB。
        // Shapeを128にしているのは、雲1つが画面上で数百画素に広がるため塊の形にはこの程度の
        // 解像度が要る一方、これ以上上げるとメモリが4倍(256^3で64MB)に跳ねるため。
        // Detailは縁を削るだけで低周波成分を持たないので32で足りる
        // 大気散乱のLUT(Hillaire 2020)。解像度は論文の推奨値。
        // Transmittanceは高度×視線天頂角、MultiScatteringは高度×太陽天頂角で、
        // どちらも大気パラメータだけで決まるためカメラにも時刻にも依存しない。
        // SkyViewは空そのもの(太陽の子午線からの方位×天頂角)で、太陽が動くと変わる。
        // **kSkyViewLUTWidth/Heightはシェーダ側(AtmosphereCommon.hlsliの
        // kSkyViewLUTWidthF/kSkyViewLUTHeightF)と一致させること** — UVの半テクセル補正に
        // 解像度が要るため、焼く側・引く側の両方が同じ値を知っている必要がある
        static constexpr uint32_t kTransmittanceLUTWidth = 256;
        static constexpr uint32_t kTransmittanceLUTHeight = 64;
        static constexpr uint32_t kMultiScatteringLUTSize = 32;
        static constexpr uint32_t kSkyViewLUTWidth = 192;
        static constexpr uint32_t kSkyViewLUTHeight = 108;
        static constexpr uint32_t kCloudShapeNoiseSize = 128;
        static constexpr uint32_t kCloudDetailNoiseSize = 32;
        // 手続き空(SkyGenerate.hlsl): Perez分布をGPUで評価してキューブマップを生成する。
        // オフラインで焼いたDDS(Sky.dds)と違い、太陽が動くと空の輝度分布の「形」も追従する
        // (circumsolarの明るい領域が太陽と一緒に動く)。詳細はSkyGenerate.hlsl冒頭。
        //
        // .ksceneで[Scene]Skyboxを明示しているシーン(White Furnace TestのUniformWhite.dds)は
        // 従来どおりDDSを使う必要があるため、手続き空は別テクスチャに持ち、
        // ActiveSkyTexture()がフレームごとにどちらを使うか決める
        static constexpr uint32_t kProceduralSkySize = 256;
        std::unique_ptr<RHI::IRHITexture> m_ProceduralSkyTexture;
        std::unique_ptr<RHI::IRHIShader> m_SkyGenerateComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SkyGeneratePipelineState;
        // SkyGenerate用の専用定数バッファ。m_IBLPrefilterConstantBufferと共用しないこと
        // (UpdateBuffer→SetComputeConstantBufferの順序制約があり、共用すると事故りやすい。
        //  詳細はRHI/IRHICommandList.hのSetConstantBufferのコメント)
        std::unique_ptr<RHI::IRHIBuffer> m_SkyBakeConstantBuffer;
        bool m_ProceduralSkyEnabled = Defaults::ProceduralSkyEnabled;
        // 手続き空を焼き直す必要があるか。太陽が動いたとき等に立てる
        bool m_SkyBakeDirty = true;
        // 最後に焼いたときの太陽の向き。これと現在の向きの角度差が閾値を超えたら焼き直す。
        // 毎フレーム焼くと空生成6回+プリフィルタ36回が常時走って無駄なため
        DirectX::XMFLOAT3 m_LastBakedSunPosition{ 0.0f, 0.0f, 0.0f };
        // 最後に焼いたときの実効プリ露出。空はプリ露出済みの値で焼かれるため、
        // 露出が動いたときも焼き直さないと空だけ古い露出のまま取り残される
        float m_LastBakedExposureEV100 = 0.0f;
        // 最後に焼いたときのタービディティ。m_SkyTurbidityが動いたときも、Preethamの
        // xyYモデルの形自体が変わるため焼き直しが要る(exposureMovedと同じ形の判定。Render()参照)
        float m_LastBakedTurbidity = 0.0f;
        // 最後に焼いたときの空の彩度。タービディティと同じ理由で、動いたら焼き直す
        float m_LastBakedSkySaturation = 0.0f;
        // 焼き直しの角度閾値(度)。Auto Advance既定(1h/s)では太陽は15度/秒動くので、
        // 1.0度なら毎秒15回の焼き直しになる。空の見た目は15Hz更新でも連続に見える
        float m_SkyBakeAngleThresholdDegrees = 1.0f;

        // 背景(深度が書かれていない画素)をキューブマップのサンプルではなく、Sky.hlsliの
        // SkyColorを画面解像度で直接評価するか。キューブマップは256px/面しかなく
        // 3840px・水平画角68度のカメラでは約20倍に拡大表示されるため、既定で有効にしてある。
        // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、この設定に関わらずキューブマップを使う
        // (DeferredLighting.hlslへ渡すSkyParams.yはActiveSkyTexture()の結果とのANDで決める)
        bool m_SkyAnalyticBackground = Defaults::SkyAnalyticBackground;

        // 空パラメータ(ティント4本+照度正規化済みの天頂輝度)をGPU側で計算するコンピュートシェーダー
        // (SkyIntegrate.hlsl)。**CPU側に同じ式のミラーを置いてはいけない**(二重実装になる)。
        // 結果はm_SkyParametersBuffer(SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslが読む)へ書く
        std::unique_ptr<RHI::IRHIShader> m_SkyIntegrateComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SkyIntegratePipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_SkyIntegrateConstantBuffer;
        // SkyIntegrate.hlslが書き、SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslが読む
        // 要素数1のStructuredRWバッファ(Sky.hlsliのGPUSkyParametersと一致させること)。
        // 【なぜ毎フレーム作り直さないのか】ベイク時の値をそのまま使うことで、背景とキューブマップ
        // (IBL・反射)が常に同一の空パラメータを見る。毎フレーム作り直すと、太陽の角度閾値で
        // ベイクを間引いている間だけ背景とIBLの空がずれてしまう。加えて積分はθ64×φ256=16,384
        // サンプルなので、背景評価のためだけに毎フレーム走らせるのは無駄が大きい
        std::unique_ptr<RHI::IRHIBuffer> m_SkyParametersBuffer;
        // m_SkyParametersBufferへSkyIntegrateパスが一度でも書き込んだかどうか。手続き空を使わない
        // シーン(.ksceneのDDSスカイボックス使用時)ではbakeSkyThisFrameが常にfalseになりSkyIntegrate
        // パスも通常は走らないため、このフラグがfalseの間だけRender()がskyIntegrateThisFrameを
        // trueにして1回だけ強制的に走らせ、未初期化のまま読まれることを防ぐ。
        // 【なぜCPU側からのUpdateBufferでゼロ埋めしないのか】DX12のStructuredRWバッファは
        // GPU専用(UAV/SRV)のDEFAULTヒープに確保しておりCPUから書き込む経路を持たないため、
        // UpdateBufferを呼ぶとクラッシュする(m_SkyParametersBuffer作成箇所のコメント参照)
        bool m_SkyParametersBufferInitialized = false;

        bool m_IBLBaked = false;
        // BRDF積分LUTを焼き終えたか(m_IBLBakedとは別管理)。このLUTは(NdotV, ラフネス)の
        // 2Dテーブルでスカイボックスにも太陽の位置にも一切依存しないため、起動後に一度焼けば
        // 二度と焼き直す必要がない。プリフィルタ済み鏡面が空の変化に追従して再ベイクされるように
        // なった以降も巻き込まれて焼き直されないよう、専用のフラグとパスに分離してある
        // (128x128 x 1024サンプル = 約1,680万イテレーションあり、毎回焼くと丸損になる)
        bool m_BRDFLUTBaked = false;
        // 検証用の拡散イラディアンスマップを焼き終えたか(m_IBLBakedとは別管理)。既定の描画経路は
        // プリフィルタ済み鏡面の最終ミップなので、こちらは検証を有効にしたときにだけ焼く
        bool m_IBLIrradianceBaked = false;
        std::unique_ptr<RHI::IRHITexture> m_IrradianceTexture;
        std::unique_ptr<RHI::IRHITexture> m_PrefilteredEnvTexture;
        // BRDF積分LUT。float4(A, B, Eavg, 0)。第3成分Eavgはスペキュラのエネルギー補正のうち
        // Kulla-Conty(加算ローブ)方式だけが使う半球平均で、行(ラフネス)内では同じ値が入る
        std::unique_ptr<RHI::IRHITexture> m_BRDFLUTTexture;
        // 上のLUTを焼く2パス構成の中間バッファ。パス1が(A, B)をここへ書き、パス2がこれをSRVで
        // 読んでEavgを足しつつ最終LUTへ書く。同一リソースをSRVとUAVへ同時バインドできないため必要
        std::unique_ptr<RHI::IRHITexture> m_BRDFLUTScratchTexture;
        std::unique_ptr<RHI::IRHIShader> m_BRDFLUTComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_BRDFLUTPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_BRDFLUTCombineComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_BRDFLUTCombinePipelineState;

        // --- ボリュメトリック雲の3Dノイズ ---
        //
        // 雲の形状ノイズ。カメラにも太陽にも空の状態にも一切依存しない純粋な手続き生成なので、
        // BRDF積分LUTとまったく同じ理由で起動後に一度だけ焼き、二度と焼き直さない
        // (m_CloudNoiseBaked)。生成の中身はShaders/3D/CloudNoiseGenerate.hlsl。
        //
        // 【なぜ2枚に分けるか】Shapeは雲の大まかな塊、Detailはその縁を削る高周波成分で、
        // 必要な解像度が2桁違う。1枚にまとめると細かい側に合わせた巨大なテクスチャが要る
        std::unique_ptr<RHI::IRHITexture> m_CloudShapeNoiseTexture;
        std::unique_ptr<RHI::IRHITexture> m_CloudDetailNoiseTexture;
        std::unique_ptr<RHI::IRHIShader> m_CloudShapeNoiseComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_CloudShapeNoisePipelineState;
        std::unique_ptr<RHI::IRHIShader> m_CloudDetailNoiseComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_CloudDetailNoisePipelineState;
        bool m_CloudNoiseBaked = false;

        // --- 大気散乱のLUT(Hillaire 2020) ---
        //
        // TransmittanceとMultiScatteringは大気パラメータ(AtmosphereLUT.hlsl冒頭の定数と、
        // 実行時に動かせる濁り)だけで決まり、カメラにも太陽にも時刻にも依存しない。
        // そのためBRDF積分LUT・雲の3Dノイズとほぼ同じ「一度だけ焼く」作法に乗せ、
        // 濁りが変わったときだけ焼き直す(m_AtmosphereLUTBakedTurbidity)。
        //
        // SkyViewは空そのもので太陽の位置に依存するため、太陽か濁りが動いたときに焼き直す
        // (m_SkyViewBakedSunPosition)。
        // 【毎フレーム焼いていた頃の実測】192x108=20,736テクセルと小さいので「負荷は実質的に無い」と
        // 書いていたが、Intel UHD Graphics 620 / DX11 / Release の実測では1.15〜1.53msあった。
        // 1テクセルあたり視線32段+天頂32段の計64段のレイマーチで、各段が
        // Transmittance LUTとMultiScattering LUTのサンプルを伴うため、テクセル数の割に高い。
        // **このLUTを読むパス(SkyIntegrate/SkyGenerate/Lighting/SSR/AerialPerspective/
        // PlanarReflection)より前に実行される必要がある**が、順序はレンダーグラフが
        // Reads/Writesの依存から自動で決めるので、パスの登録順に依存しない
        std::unique_ptr<RHI::IRHITexture> m_TransmittanceLUT;
        std::unique_ptr<RHI::IRHITexture> m_MultiScatteringLUT;
        std::unique_ptr<RHI::IRHITexture> m_SkyViewLUT;
        std::unique_ptr<RHI::IRHIBuffer> m_AtmosphereConstantBuffer;
        std::unique_ptr<RHI::IRHIShader> m_TransmittanceComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_TransmittancePipelineState;
        std::unique_ptr<RHI::IRHIShader> m_MultiScatteringComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_MultiScatteringPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_SkyViewComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SkyViewPipelineState;
        // Transmittance/MultiScatteringを焼いたときの濁り。負の値は「まだ一度も焼いていない」。
        // 濁りが変わるとエアロゾルの量が変わるので、この2枚も焼き直す必要がある
        float m_AtmosphereLUTBakedTurbidity = -1.0f;
        // SkyView LUTを最後に焼いたときの太陽の向きと濁り。負の濁りは「まだ一度も焼いていない」。
        // 【なぜ毎フレーム焼かなくてよいのか】CSSkyViewの入力はこの2つだけである
        // (視点位置はkSkyViewHeightKm固定でカメラに依存しない。AtmosphereLUT.hlsl参照)。
        // どちらも動いていなければ、まったく同じ内容のLUTを焼き直しているだけになる。
        // 手続き空の焼き直し(m_LastBakedSunPosition)とまったく同じ判定の形だが、
        // あちらは露出・彩度にも依存するため条件を共用はできない
        DirectX::XMFLOAT3 m_SkyViewBakedSunPosition{ 0.0f, 0.0f, 0.0f };
        float m_SkyViewBakedTurbidity = -1.0f;
        // 太陽がこの角度以上動いたらSkyView LUTを焼き直す。LUTは天頂方向180度を108テクセルで
        // 持つので1テクセルあたり約1.67度あり、その1/30以下しかずらさない値にしてある。
        //
        // 【意図的に手続き空のm_SkyBakeAngleThresholdDegrees(1.0度)より桁で細かくしている】
        // このLUTは背景の空(Sky.hlsliのSkyColor)が画面解像度で毎フレーム引くもので、
        // 間引きの粒度がそのまま背景の時間解像度になる。一方あちらが焼くIBLキューブは
        // 6面+プリフィルタ36回のディスパッチを伴う重いベイクで、間接光にしか効かない。
        // 変更前は「毎フレーム焼く」だったので、それに最も近い挙動を選んでいる。
        //
        // 【時刻を自動で進めるシーンでは削減にならない】Auto Advance既定(1h/s)では太陽は
        // 15度/秒動くため、60fpsでも毎フレームこの閾値を超えて結局毎フレーム焼く。
        // 削減が効くのは太陽が止まっているシーン(Defaults::TimeAutoAdvanceは既定false)で、
        // その場合は起動直後の1回だけになる
        static constexpr float kSkyViewRebakeAngleDegrees = 0.05f;
        std::unique_ptr<RHI::IRHIShader> m_IrradianceComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_IrradiancePipelineState;
        std::unique_ptr<RHI::IRHIShader> m_PrefilterComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PrefilterPipelineState;
        // 拡散イラディアンスの球面調和関数(SH L2)経路。CSIrradianceの高速な
        // 代替で、m_IBLUseSHIrradianceでA/B比較できるようトグルにしてある。詳細は
        // IBLConvolve.hlsl冒頭のコメントとdocs/Architecture.htmlを参照
        static constexpr uint32_t kSHCoeffCount = 9; // 実数SH L2(l<=2)の項数
        // CSProjectSHの射影に使う離散化解像度(1面の1辺のテクセル数)。
        // 【SourceSkyboxの実解像度とは無関係】スカイボックスはDDS(シーンごとに任意の解像度)や
        // 手続き空(256)など実行時に変わりうる一方、IRHITextureには解像度を問い合わせる手段が
        // 無いため、射影側は独立した固定解像度を持つ(IBLConvolve.hlslのSHProjectionSizeコメント参照)。
        // 64×64×6=24,576テクセルはCSIrradianceの約9,750万サンプルに対し十分密で、
        // 9個の係数を求めるだけの積分には(理論上は32でも足りる範囲)余裕を持たせた値
        static constexpr uint32_t kSHProjectionSize = 64;
        std::unique_ptr<RHI::IRHIShader> m_ProjectSHComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ProjectSHPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_ProjectSHFinalComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ProjectSHFinalPipelineState;
        std::unique_ptr<RHI::IRHIShader> m_EvaluateSHComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_EvaluateSHPipelineState;
        // CSProjectSHのグループごとの部分和(9係数×グループ数)と、CSProjectSHFinalが
        // 合算した最終係数(9個)。どちらもRGB(float4のxyz、wは詰め物)
        std::unique_ptr<RHI::IRHIBuffer> m_SHPartialSumsBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_SHCoefficientsBuffer;
        // trueならCSIrradianceの代わりにSH L2経路を使う。既定false(検証で選べるようにしてあるが、
        // どちらを既定にするかはリンギングの実測(ProbeTestのエミッシブ帯周り)で決めること。
        // m_IBLUseDedicatedIrradiance/デバッグビューでイラディアンス焼き込みが要る場面でのみ意味を持つ
        bool m_IBLUseSHIrradiance = Defaults::IBLUseSHIrradiance;
        // SHのウィンドウ関数(Sloan)の強さ。0=無効(既定)。リンギングが実測で出た場合のつまみ
        float m_SHWindowLambda = Defaults::SHWindowLambda;
        // プリフィルタ済み鏡面のミップごとの畳み込みで使うラフネス値を渡す専用の定数バッファ
        std::unique_ptr<RHI::IRHIBuffer> m_IBLPrefilterConstantBuffer;
        // デバッグ表示(Render Targets)で確認するプリフィルタ済み鏡面マップのミップレベル
        int32_t m_IBLPrefilterDebugMipLevel = 0;
        // IBL(拡散イラディアンス+プリフィルタ済み鏡面)のON/OFFと強度。無効時はシェーダ側
        // (DeferredLighting.hlsl)でEvaluateIBLの代わりに定数色アンビエント
        // (AmbientColor.rgb)へフォールバックする(真っ暗にはしない)。既定値を1.0でなく0.5に
        // しているのは、明るく補正した空の輝度分布(14.6節)ではIBL全体の寄与が強すぎるため
        bool m_IBLEnabled = Defaults::IBLEnabled;
        float m_IBLIntensity = Defaults::IBLIntensity;
        // 拡散イラディアンスを専用マップ(m_IrradianceTexture)から取るかどうか。既定はfalseで、
        // プリフィルタ済み鏡面の最終ミップ(roughness=1)を使う。CSPrefilterがV=R=Nを仮定して
        // いるためroughness=1ではGGXの実効カーネルがコサイン畳み込みへ厳密に退化し、両者は同じ
        // E(N)/πを格納する(14.10節)。White Furnace Testで画素一致、実スカイボックスでも
        // 最大2〜4/255の差しか出ないことを実機で確認してあるため、既定では専用マップを使わない。
        // これによりリフレクションプローブのような実行時のキューブマップ焼き直しから、最も重い
        // CSIrradiance(約9750万サンプル)を丸ごと省ける。
        // 畳み込み処理自体はいつでも検証できるよう残してあり、このトグルをONにすると
        // その場で焼いて(m_IBLIrradianceBaked)従来経路に切り替わる
        bool m_IBLUseDedicatedIrradiance = Defaults::IBLUseDedicatedIrradiance;
        // bent normalによる遮蔽(34章)。FrameConstants::OcclusionParamsへ載る
        bool m_BentNormalAOSource = Defaults::BentNormalAOSource;
        // スペキュラ遮蔽の方式。FrameConstants.OcclusionParams.yへ数値として渡し、
        // SpecularEnergy.hlsliのComposeSpecularOcclusionが切り替える。
        // 値はComposeSpecularOcclusionのsoModeと一致させること
        enum class SpecularOcclusionMode
        {
            Legacy = 0,  // Frostbite近似(方向を見ない従来近似)
            Cone = 1,    // 球冠交差(SpecularOcclusionBand。d >= av+as で厳密に0になる)
            SG = 2,      // 球面ガウス(SpecularOcclusionSG、34.11節。常に正なので凹部が純黒へ潰れない)
        };
        SpecularOcclusionMode m_SpecularOcclusionMode =
            static_cast<SpecularOcclusionMode>(Defaults::SpecularOcclusionMode);
        bool m_MultiBounceAOEnabled = Defaults::MultiBounceAOEnabled;
        // 環境光(間接光)の拡散・鏡面それぞれの倍率。FrameConstants.IBLParams.y / .z として渡す。
        //
        // m_IBLIntensityが拡散と鏡面へ一様に掛かる「環境光全体の明るさ」なのに対し、こちらは
        // 両者の比率を意図的に崩すための画作り用のつまみ。金属やガラスの映り込みだけを強めたい、
        // 逆に環境の照り返しを残したまま反射を抑えたい、といった調整がIBL強度単独ではできないため
        // 分けている。
        //
        // 【IBLの有効/無効に関わらず効く】無効時の定数色アンビエントにも同じ倍率を掛ける。
        // 片方にしか効かないとトグルを切り替えたときにつまみの意味が変わり、比較にならないため。
        // 【間接光にのみ効く】直接光・自発光には掛けない(遮蔽マップと同じ方針。22.1節)。
        // SSILの間接拡散光にも掛けない ―― あれはスクリーンスペースで得た周囲のサーフェスからの
        // 光であって、ここで言う環境(空・プローブ)由来のアンビエントとは別の項のため
        float m_AmbientDiffuseScale = Defaults::AmbientDiffuseScale;
        float m_AmbientSpecularScale = Defaults::AmbientSpecularScale;
        // スペキュラBRDFのmultiple-scattering energy compensation(Kulla & Conty 2017)の方式。
        // IBL鏡面・直接光鏡面の両方に効くため、Enable IBLとは独立した選択肢にしている。
        // FrameConstants.ShadowParams.wへ数値として渡し、共有ヘッダーSpecularEnergy.hlsliを
        // インクルードする各シェーダー(DirectLighting / DeferredLighting / Transparent /
        // ProbeCapture、および係数を共有するReflectionProbe.hlsli経由のSSR)が方式を切り替える。
        // 値はSpecularEnergy.hlsliのKURENAI_SPEC_COMP_*と一致させること。
        //
        // 既定がLinearなのは、実使用域(エンジンはラフネスを[0.045, 1.0]にクランプする)では
        // 3方式のうち最も真値に近いことを多重散乱ランダムウォークとの比較で確認したため(14.9.8節)。
        // Offは補正しない状態がエネルギー的に不正(粗い面ほど暗い)であることを見るための比較用
        enum class SpecularCompensationMode
        {
            Off = 0,         // 補正なし
            Linear = 1,      // 1 + F0(1/Ess - 1)  等比級数の第1項
            Series = 2,      // 1 / (1 - F0(1-Ess)) 等比級数の全項
            KullaConty = 3,  // 加算ローブ(本来のKulla-Conty。IBL側はFdez-Agüera 2019のsplit-sum形)
        };
        SpecularCompensationMode m_SpecularCompensationMode =
            static_cast<SpecularCompensationMode>(Defaults::SpecularCompensationMode);
        // Enable IBL無効時に使う定数色アンビエントフォールバックの強度倍率。シェーダ側ではなく
        // Render()がFrameConstants.AmbientColorへ書き込む時点でrgb(alphaのdayFactorは除く)に
        // 乗算する(HLSL側は素のAmbientColor.rgbを読むだけでよい)
        float m_AmbientScale = Defaults::AmbientScale;

        // シーン全体の自発光(エミッシブ)の強度倍率。MakeObjectConstantsがmesh.EmissiveFactorへ
        // 乗算する。glTFのemissiveFactorは通常1.0以下に収まるため、G-Bufferのエミッシブを
        // HDR化(R11G11B10_Float)しただけでは照明器具の輝度が1.0を超えず、ブルームが効かない。
        // アセットを再オーサリングせずにHDRな自発光を得るための倍率
        // (KHR_materials_emissive_strengthをインポータが読むようになれば本来はそちらが正しい)
        float m_EmissiveIntensity = Defaults::EmissiveIntensity;

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
        bool m_EmissiveLightsEnabled = Defaults::EmissiveLightsEnabled;
        // DDGIにも自発光を加算したままにするか(=二重に数えるか)。既定は抑止する
        bool m_EmissiveLightsDoubleCountGI = Defaults::EmissiveLightsDoubleCountGI;
        float m_EmissiveLightsCutoffIrradiance = Defaults::EmissiveLightsCutoffIrradiance;
        int m_EmissiveLightsMaxCount = Defaults::EmissiveLightsMaxCount;
        // RangeのクランプにつかうシーンAABBの対角。LoadSceneで一度だけ求める
        float m_EmissiveLightsMaxRange = 0.0f;
        // 直近のフレームで実際にGPUへ送ったプロキシの数(ImGuiとログの表示用)
        uint32_t m_EmissiveLightsUsedCount = 0;
        // 上限で切り捨てたときの「採用した集合」の指紋。切り捨てが起きなければ0。
        //
        // 【プローブの署名に混ぜるためだけにある】採用順はカメラからの照度で決まるので、
        // 上限に当たっているシーンではカメラを動かすだけで焼く光源の集合が変わる。
        // 署名へ入れないと、収束済みのプローブだけ古い集合のまま残る
        uint64_t m_EmissiveLightsSelectionHash = 0;
        bool m_EmissiveLightsCapLogged = false;
        // DDGIの二重計上の抑止が「実際に何をしたか」を1回だけログへ出したか。
        // 【絵から分からない】抑止はプローブのイラディアンスにしか出ず、しかも
        // 「効いていない」と「効いた結果が小さい」が同じ絵になる。実効値を出すしかない。
        //
        // 【2経路で別々に持つ】1つのフラグを共有すると、先に走ったほうがもう一方のログを
        // 永久に潰す。どちらの経路の話なのか区別できないログは、切り分けの役に立たない
        bool m_DDGIEmissiveSuppressLoggedRaster = false;
        bool m_DDGIEmissiveSuppressLoggedTrace = false;
        // 送信した灯の実効値を1回だけログへ出したか(「走っていない」と「暗い」の切り分け用)
        bool m_EmissiveLightsValuesLogged = false;

        // 反射プローブ(19章): プローブ位置から6方向をProbeCapture.hlslで2Dレンダーターゲットへ描き、
        // IBLConvolve.hlsl CSCopyCaptureToCubeFaceでスクラッチのキューブマップへ組み上げてから、
        // IBLと同じCSIrradiance/CSPrefilterで畳み込んでプローブごとのキューブマップ配列へ書き込む。
        // 環境ソースを差し替えるだけなので、シェーダー側の評価式(EvaluateIBL)はIBLと完全に共通。
        //
        // キューブマップ配列の枚数上限。TextureCubeArrayは実行時に伸縮できないため固定容量で確保し、
        // これを超えるプローブが置かれたシーンは先頭からこの数だけを採用する(警告ログを出す)
        static constexpr uint32_t kMaxReflectionProbes = 8;
        // キャプチャ解像度。プリフィルタ済み鏡面のベース解像度(kIBLPrefilterBaseSize)と揃えることで、
        // ミップ0が「畳み込み無しのキャプチャそのもの」になりデバッグ表示で生の映り込みを確認できる
        static constexpr uint32_t kProbeCaptureSize = kIBLPrefilterBaseSize;
        std::unique_ptr<RHI::IRHIShader> m_ProbeCaptureVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ProbeCapturePixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ProbeCapturePipelineState;
        // 1面ぶんのキャプチャ先(6面で使い回す)。HDRのままキューブへ写すためG-Bufferと違いFloat
        std::unique_ptr<RHI::IRHITexture> m_ProbeCaptureColor;
        // 同じキャプチャの2枚目のレンダーターゲット(SV_TARGET1)。プローブ位置から描画点までの
        // ワールド距離をそのまま書く。深度バッファから逆算せずMRTで直に出しているのは、
        // 面ごとの逆投影を組む必要がなくキャプチャシェーダーの1行で済むため(19.12節)
        std::unique_ptr<RHI::IRHITexture> m_ProbeCaptureDistance;
        std::unique_ptr<RHI::IRHITexture> m_ProbeCaptureDepth;
        std::unique_ptr<RHI::IRHIShader> m_ProbeCubeCopyComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ProbeCubeCopyPipelineState;
        // キャプチャした6面を組み上げるスクラッチのキューブマップ(単一キューブ)。畳み込みの入力に
        // なるためTextureCubeArrayではなくTextureCubeである必要がある(IBLConvolve.hlslのSourceSkyboxは
        // TextureCube宣言のまま。これによりIBLの畳み込みシェーダーを一切変更せず再利用できる)。
        // プローブは1つずつ順に焼くため1枚で足りる
        std::unique_ptr<RHI::IRHITexture> m_ProbeRadianceCube;
        // 畳み込み結果(プローブごと)。DeferredLighting.hlslがTextureCubeArrayとして読む。
        // 反射プローブは鏡面専任なので拡散イラディアンス側の配列は持たない(拡散はDDGIへ一本化)
        std::unique_ptr<RHI::IRHITexture> m_ProbePrefilteredArray;
        // 距離キューブ(プローブごと、19.12節)。プローブ位置から各方向の被写体までのワールド距離。
        // 放射輝度と違い畳み込まないため、キャプチャからキューブ配列へ直接書き込む
        // (スクラッチのキューブマップを経由しない)。用途は2つ:
        //   1. 視差補正を「箱との交差」から「実際に記録された形状との交差」へ精密化する
        //   2. プローブから見えない位置(壁の向こう)のピクセルで重みを落とし、光漏れを抑える
        std::unique_ptr<RHI::IRHITexture> m_ProbeDistanceArray;
        // プローブの影響範囲(位置・半径)をシェーダーへ渡すStructuredBuffer(t13)
        std::unique_ptr<RHI::IRHIBuffer> m_ProbeBuffer;
        // キャプチャの面ごとに値を更新して使い回すFrameConstants(共有のm_FrameConstantBufferとは別。
        // ViewProj/CameraPositionだけをプローブのものへ差し替える。詳細はProbeCapture.hlsl冒頭)
        std::unique_ptr<RHI::IRHIBuffer> m_ProbeCaptureConstantBuffer;
        // ApplyLoadedSceneがm_Scene.ReflectionProbesからコピーし、以降ImGuiが編集する(m_Lightsと同じ方針)。
        // どちらもRenderスレッド専有のためロックは不要
        std::vector<Assets::ReflectionProbe> m_ReflectionProbes;
        int m_SelectedProbeIndex = -1;
        // 次のRender()でプローブを焼き直す要求。シーン読み込み時とImGuiのBakeボタンで立てる。
        // スカイボックス由来のIBLと違いシーンのジオメトリ・ライトに依存するため、
        // 「一度焼いたら二度と焼かない」ではなく明示的な要求ベースにしている
        bool m_ProbeBakeRequested = false;
        // 一度でも焼けたか。焼く前のプローブは中身が未定義なので、それまでは影響を無効にして
        // グローバルIBLのまま描く(未初期化のキューブマップが映り込むのを防ぐ)
        bool m_ProbeBaked = false;
        bool m_ReflectionProbeEnabled = Defaults::ReflectionProbeEnabled;
        // 視差補正(box projection)を行うか。Box形状のプローブにのみ効く。無効にすると
        // 反射ベクトルをそのまま引くPhase 1相当の挙動になり、壁際で反射位置がずれるのを確認できる
        bool m_ProbeParallaxCorrectionEnabled = Defaults::ProbeParallaxCorrectionEnabled;
        // プローブ間・プローブとグローバルIBLの重み付きブレンドを行うか。無効にすると
        // 「影響範囲に入る最も近い1つだけを使う」Phase 1相当の挙動になり、境界の継ぎ目を確認できる
        bool m_ProbeBlendingEnabled = Defaults::ProbeBlendingEnabled;
        // 視差補正に距離キューブを使うか(19.12節)。無効にすると箱との交差だけで補正する
        // 従来の挙動になる。有効時も、箱との交点を探索範囲の上限として使う点は変わらない。
        //
        // 既定でfalseなのは、二重像が軽減される代わりにレイマーチの結果へ距離キューブの
        // テクセルの階段状のエッジが乗るためで、実機で見比べると「全体としては良くなった
        // とは言えない」ため。式としては正しく動いており(19.12節の検証参照)、
        // 距離キューブの解像度を上げるか2次モーメントを持って確率的に扱えば伸ばせる余地が
        // あるので、比較用のトグルとして残してある
        bool m_ProbeDepthParallaxEnabled = Defaults::ProbeDepthParallaxEnabled;
        // 距離キューブによる遮蔽判定で、プローブから見えない位置のピクセルの重みを落とすか
        // (光漏れの抑制)。無効にすると影響範囲に入っているだけで重みが立つ従来の挙動になる。
        //
        // 既定でfalseなのは、プローブが疎な現状では副作用のほうが大きいため(19.12節)。
        // 重みを落とした分はグローバルIBL(=空)が埋めるので、「プローブから見えない」だけの
        // 場所——例えば球の真下の床——が空の色で明るくなり、影のはずの位置に白いハローが出る。
        // 落ちた重みを別のプローブが引き取れる密度になって初めて素直に使える機能なので、
        // 効果と副作用を見比べられるトグルとして残し、既定は従来の挙動にしてある
        bool m_ProbeOcclusionEnabled = Defaults::ProbeOcclusionEnabled;
        // デバッグ表示(Render Targets)で確認するプローブ番号とプリフィルタのミップレベル
        int32_t m_ProbeDebugIndex = 0;
        int32_t m_ProbePrefilterDebugMipLevel = 0;
        // 距離キューブのデバッグ表示で白になる距離(メートル相当)。距離は色ではないので
        // 表示輝度の倍率(1〜64倍)ではなくこちらで正規化する(Present.hlsl Mode 13へは
        // 逆数をGainとして渡す)
        float m_ProbeDistanceDebugRange = Defaults::ProbeDistanceDebugRange;

        // プローブの更新モード。焼き直しのコストと「シーンの変化への追従」のどちらを取るかの選択で、
        // ImGuiで切り替えて負荷と品質を比較できるようにしてある(19.10節)
        enum class ProbeUpdateMode
        {
            // シーン読み込み時とImGuiのBakeボタンのときだけ焼く。実行時コストはゼロだが、
            // ライトや時刻を動かしても反射は焼いた時点のまま止まる
            Baked,
            // 上に加えて、焼き上がりに影響する状態(時刻・太陽・ライト)の変化を検出して自動で焼き直す。
            // 変化していないフレームのコストはゼロだが、変化したフレームは全プローブぶんの
            // フルベイクが1フレームに集中する
            OnDemand,
            // 上に加えて、1プローブを12フレームかけて焼き直し、次のプローブへ回る(ラウンドロビン)。
            // 内訳は「6フレームで1面ずつキャプチャ」→「6フレームで1面ぶんのミップチェーンずつ畳み込み」
            // (畳み込みの割り当ての根拠はKurenaiEngine3D.cppのプリフィルタフェーズ参照)。
            // 全プローブを毎フレーム焼くとドローコールがプローブ数×6倍になり非現実的なため、
            // 時間分割を既定の実装方式にしている
            Realtime,
        };
        ProbeUpdateMode m_ProbeUpdateMode = ProbeUpdateMode::Baked;
        // Realtimeの進行状態。次に焼くプローブ番号と面番号
        uint32_t m_ProbeRealtimeProbeIndex = 0;
        uint32_t m_ProbeRealtimeFace = 0;
        // プリフィルタ畳み込み(6ミップ×6面=36ディスパッチ)を1フレームへ集中させず、
        // kProbeRealtimePrefilterStepsPerFrameずつ複数フレームへ分ける(集中させると
        // 「6フレームに1回のスパイク」になる)。
        // kProbePrefilterStepCount(36)が「プリフィルタ中でない」を表す番兵値
        static constexpr uint32_t kProbePrefilterStepCount = kIBLPrefilterMipLevels * kCubeFaceCount;
        // 1フレームに進めるステップ数。ステップ番号は「面を外側・ミップを内側」で(face, mip)へ
        // 割り当てるため(KurenaiEngine3D.cppのRealtimeプリフィルタフェーズのコメント参照)、
        // ここを kIBLPrefilterMipLevels と一致させると
        // 「1フレーム = 1面ぶんのミップチェーン全部」となり6フレームすべてが厳密に同じ量になる。
        // 一致させないとフレームごとにミップ0の面の数が0個/1個/2個とばらつき、
        // ミップ0が畳み込み全体の75%を占めるためそのままスパイクの高さのばらつきになる。
        // capture フェーズ(6面=6フレーム)ともデューティ比が対称になる
        static constexpr uint32_t kProbeRealtimePrefilterStepsPerFrame = kIBLPrefilterMipLevels;
        uint32_t m_ProbeRealtimePrefilterStep = kProbePrefilterStepCount;
        // OnDemandの変化検出用。最後にフルベイクを発行した時点の状態の署名。
        // 毎フレームの署名と突き合わせ、変わっていれば焼き直しを要求する
        uint64_t m_ProbeBakeSignature = 0;
        // 焼き上がりに影響する状態(時刻・太陽・シャドウ・IBL強度・全ライト)から署名を作る。
        // 影響範囲(形状・半径・ブレンド距離)はキャプチャ内容を変えないため含めない
        uint64_t ComputeProbeBakeSignature() const;
        // 最後にキャプチャしたときの実効プリ露出(m_EffectiveExposureEV100)。
        //
        // 【なぜ記録しておく必要があるか】プローブのキューブマップにはプリ露出済みの放射輝度が
        // 入っている(21.5節)。その倍率は時刻に連動して最大18段(約26万倍)動くのに対し、
        // Bakedモードのプローブはシーン読み込み時に一度焼いたきり更新されない。そのため
        // 焼いた時点と現在とで実効プリ露出が食い違うと、プローブの寄与だけが桁違いの明るさで
        // 合成される。実測では夜のProbeTestからSponzaへ切り替えたとき、EV100=-2.36で焼かれた
        // プローブをEV100=15.0のフレームが読み、17.4段(約17万倍)過剰になって画面が
        // 白飛びしたまま戻らなくなっていた。
        //
        // 空(手続き空)は実効プリ露出が0.05段動くたびに焼き直して追従しているが、プローブは
        // 1回のフルベイクが全プローブ×6面の描画になり同じ頻度では焼き直せない。そこで
        // 焼き直す代わりに、読み出し時へ 2^(焼いたEV - 現在のEV) を掛けて現在の露出へ
        // 換算する(FrameConstants.ProbeParams2.w、ReflectionProbe.hlsliのSampleEnvironment)。
        // キューブマップの中身は触らないのでfp16の値域も変わらない
        float m_ProbeBakedExposureEV100 = 15.0f;
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
        static constexpr uint32_t kDDGIIrradianceTexels = 6;
        static constexpr uint32_t kDDGIDistanceTexels = 14;
        // 各辺に足す境界の幅。オクタヘドラルは正方形の縁が球面上で折り返して繋がるため、
        // その繋がる先のテクセルを外周へ複製しておかないと、バイリニア補間が縁で破綻する
        // (隣のプローブのテクセルを拾ってしまうことの防止も兼ねる)
        static constexpr uint32_t kDDGIProbeBorder = 1;
        // アトラス上の1プローブぶんのセルの1辺(境界込み)
        static constexpr uint32_t kDDGIIrradianceCell = kDDGIIrradianceTexels + kDDGIProbeBorder * 2;
        static constexpr uint32_t kDDGIDistanceCell = kDDGIDistanceTexels + kDDGIProbeBorder * 2;
        // プローブ数の上限。反射プローブと違いアトラスはシーン読み込み時に確保し直すので
        // 技術的な固定容量ではないが、.ksceneの書き間違いで数GBのアトラスを作らないための歯止め
        // シーン全体で確保してよいプローブ数の上限。**容量の限界ではなく、`.kscene`の
        // 打ち間違いでギガバイト単位を確保しないための番人**である。
        // クリップマップLODでプローブ総数が「格子の積 × LOD段数」になったので引き上げた
        // (8192でもイラディアンス8MB + 距離16MB程度で、実際の律速は更新スループット側)
        static constexpr uint32_t kDDGIMaxProbes = 8192;
        // クリップマップLODの最大段数。FrameConstantsへ段数ぶんの配列を持つので有界にしておく。
        // SceneLoaderのLODCountの検証範囲と一致させること
        static constexpr uint32_t kDDGIMaxLODCount = 4;
        // キャプチャ解像度(1面あたり)。6面ぶんで 16×16×6 = 1536方向がレイの代わりになる。
        // 反射プローブのkProbeCaptureSize(128)と違い小さくてよいのは、DDGIが必要とするのが
        // 「低周波の拡散イラディアンス」であって鏡面の映り込みではないため
        static constexpr uint32_t kDDGICaptureSize = 16;

        // シーンから読み込んだボリューム(先頭の1つだけを使う)。m_HasGIVolumeがfalseの間は
        // アトラスは1プローブぶんのダミーとして確保され、シェーダー側もDDGIParams0.w=0で無効になる
        Assets::GIVolume m_GIVolume;
        bool m_HasGIVolume = false;
        // シーン全体の総プローブ数(= ProbeCountsの3軸の積 × LOD段数)。ダミー時は1。
        // アトラスの確保と更新のラウンドロビンはこの数で回る
        uint32_t m_DDGIProbeCount = 1;
        // LOD 1段ぶんのプローブ数(ProbeCountsの3軸の積)。通し番号からLODを割り出すのに使う
        uint32_t m_DDGIProbesPerLOD = 1;
        // 実際に使うLOD段数(m_GIVolume.LODCountをkDDGIMaxLODCountでクランプしたもの)
        uint32_t m_DDGILODCount = 1;
        // 格子を追従させる中心(カメラのワールド座標)。
        // 【Render中に固定する】格子の原点・プローブ位置・dirty判定・シェーダーへ渡す値が
        // すべてこれを基準に決まるので、1フレームの途中で動くと食い違う
        DirectX::XMFLOAT3 m_DDGIFollowCenter{ 0.0f, 0.0f, 0.0f };

        // オクタヘドラル2Dアトラス。RGBがイラディアンス、距離側はR=平均距離・G=平均二乗距離。
        // どちらもR32系で確保する。更新CSがヒステリシスのために「前の値を読んでから書く」ため、
        // 型付きUAV読み出しがR32系しか保証されていないという制約に従う必要がある
        // (AutoExposure.hlslの同じ判断を参照)
        std::unique_ptr<RHI::IRHITexture> m_DDGIIrradianceAtlas;
        std::unique_ptr<RHI::IRHITexture> m_DDGIDistanceAtlas;
        std::unique_ptr<RHI::IRHIShader> m_DDGIProbeUpdateComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_DDGIProbeUpdatePipelineState;
        std::unique_ptr<RHI::IRHIShader> m_DDGIBorderCopyComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_DDGIBorderCopyPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_DDGIUpdateConstantBuffer;
        // スクロールで担当する場所が変わったプローブを、焼き直されるまでサンプリングから外すパス。
        // 詳細はDDGIProbeUpdate.hlslのCSInvalidateProbesを参照
        std::unique_ptr<RHI::IRHIShader> m_DDGIInvalidateProbesComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_DDGIInvalidateProbesPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_DDGIDirtyProbeBuffer;
        // 各スロットが「最後に焼いたときのワールド格子座標」。いまの座標と違えば未確定(dirty)。
        // 【ワールド座標で持つこと】アトラスのセル番号で持つと、スクロールしてもセル番号は
        // 変わらないので「別の場所を担当するようになった」ことを検出できない
        std::vector<DirectX::XMINT3> m_DDGIProbeBakedCoord;
        // 焼き直し待ちのスロット番号(毎フレーム組み直す。GPUへ渡す一時の並び)
        std::vector<uint32_t> m_DDGIDirtyProbeList;
        // DDGIのレイ取得をDXRで行う経路(DDGIProbeTrace.hlsl)。
        // m_RaytracingAvailableがtrueのときだけ作る(RTAO/RT反射と同じ扱い)
        std::unique_ptr<RHI::IRHIShader> m_DDGIProbeTraceComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_DDGIProbeTracePipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_DDGITraceConstantBuffer;
        // DDGIのキャプチャ先(反射プローブとは解像度が違うため別に持つ)
        std::unique_ptr<RHI::IRHITexture> m_DDGICaptureColor;
        std::unique_ptr<RHI::IRHITexture> m_DDGICaptureDistance;
        std::unique_ptr<RHI::IRHITexture> m_DDGICaptureDepth;
        // キャプチャした6面を組み上げるスクラッチのキューブ(放射輝度・距離の2本)。
        // 更新CSは「6面ぶんのレイ」をまとめて走査するため、面ごとの2Dではなくキューブで受ける
        std::unique_ptr<RHI::IRHITexture> m_DDGICaptureRadianceCube;
        std::unique_ptr<RHI::IRHITexture> m_DDGICaptureDistanceCube;

        bool m_DDGIEnabled = Defaults::DDGIEnabled;
        // 拡散間接光の強度倍率。DDGIとSSILは近傍/遠方で寄与が重なるため、実測で決めるための倍率
        float m_DDGIIntensity = Defaults::DDGIIntensity;
        // 全プローブが一度でも書かれたか。書かれる前のアトラスは中身が未定義なので、
        // それまではDDGIを無効にして従来のIBLのまま描く(反射プローブのm_ProbeBakedと同じ方針)
        bool m_DDGIBaked = false;
        // 初回の一巡が終わっていないか。
        //
        // 【反射プローブと違い「フルベイク」を持たない】反射プローブは8個までなので全プローブを
        // 1フレームで焼けるが、DDGIは数百個ある。同じことをするとBistroのようなシーンでは
        // 数百×6回のシーン描画が1フレームに集中して数秒のハングになる。
        // DDGIはヒステリシスで時間収束させる手法なので、初回も時間分割で埋めるのが素直。
        // ただし初回だけは「前の値」が存在しないため、一巡目はヒステリシスを使わず上書きする
        // (未初期化のアトラスと混ぜてはいけない)
        bool m_DDGIWarmingUp = true;
        // ヒステリシスを使わず上書きで焼き直す残りプローブ数。
        //
        // 【なぜ要るか】実効プリ露出は時刻に連動して最大18段動く(21.5節)。アトラス自体は
        // 露出非依存の物理量で持っているので数値が壊れることはないが、ヒステリシス0.97と
        // ラウンドロビンの積で時定数が約17秒あるため、時刻を大きく動かすとその間ずっと
        // 「前の時刻の間接光」が表示され続ける。露出が急変する時間帯ほど、この遅れが
        // 露出倍率で拡大されて目に見える(夕方に昼の間接光を夕方の露出で見ることになる)。
        // そこで露出が一定以上動いたら、一巡ぶんだけ上書きへ切り替えて即座に追従させる。
        // m_DDGIWarmingUpと違いDDGI自体は有効なまま(無効にすると従来のIBLとの間でちらつく)
        uint32_t m_DDGIOverwriteRemaining = 0;
        // 最後にアトラスを追従させた時点の実効プリ露出EV100
        float m_DDGILastExposureEV100 = 0.0f;
        bool m_DDGILastExposureValid = false;
        // これを超えて実効プリ露出が動いたら追従させる(段)。1段=明るさ2倍ぶん
        static constexpr float kDDGIExposureRewarmEV = 0.5f;
        // 時間分割の進行状態。1フレームにm_DDGIProbesPerFrame個ずつ順に焼き直す
        uint32_t m_DDGIUpdateCursor = 0;
        int32_t m_DDGIProbesPerFrame = Defaults::DDGIProbesPerFrame;

        // DDGIの更新モード。反射プローブのProbeUpdateModeと同じ考え方だが、
        // **「1フレームでフルベイク」に相当するモードは持たない** ――
        // DDGIは全プローブ×6面(455プローブなら2730回の描画)を1フレームでは焼けないため。
        // どのモードでも時間分割(1フレームm_DDGIProbesPerFrame個)であることは変わらず、
        // 違うのは「いつ止めるか」だけである。
        //
        // 【なぜ止める必要があるか】実測(Intel UHD Graphics 620 / 1280x720 / DX11)で、
        // GIVolumeを持つシーンのプローブ更新は**GPU 40〜47ms + CPU 30ms**あり、
        // どちらもフレームの最大要素だった。しかも収束後も止まらず課金され続けていた
        enum class DDGIUpdateMode
        {
            // 常に焼き続ける。ライトや時刻が動き続けるシーンでも必ず追従する
            Always,
            // 焼き上がりに影響する状態(ComputeProbeBakeSignature)が変わらなくなったら、
            // 多重バウンスが積み上がるkDDGIBounceCycles巡だけ焼いて停止する
            ConvergeThenStop,
            // 同じく停止するが、こちらは一巡だけで止める。最も速く止まる代わりに
            // 多重バウンスが1回ぶんしか乗らない
            OverwriteThenStop,
        };
        // 【既定はAlways(従来どおり)】止める側を既定にすると既存シーンの実行時の挙動が変わるため。
        // 止めたい場合はこのつまみか品質プリセット(低/中)から選ぶ
        DDGIUpdateMode m_DDGIUpdateMode = DDGIUpdateMode::Always;

        // プローブへ入れる放射輝度・距離を、どうやって集めるか。
        //
        // Raytracedを末尾に置くこと ―― UI側が「レイトレーシング非対応なら選択肢の末尾を
        // 削って出す」形で分岐しており(DrawSSRSectionと同じ作法)、並びを変えると
        // 非対応環境で別の項目が消える
        enum class DDGIRayMode
        {
            // 従来のラスタライズ。プローブ1個につきシーンを6回描く(ProbeCapture.hlsl)。
            // 1フレームの描画回数がメッシュ数に比例して増えるため、
            // ClampDDGIProbesPerFrameToConstantRingで更新プローブ数を抑える必要がある
            Raster,
            // DXR(インラインRayQuery)。1スレッド1レイでスクラッチキューブを直接埋める
            // (DDGIProbeTrace.hlsl)。メッシュ数はBVHが吸収するので描画回数の制約が無く、
            // 太陽の影もカスケードシャドウマップではなく影レイで求まる
            Raytraced,
        };
        // 「レイをどう集めるか」を環境から選ぶ。DXRが使えるなら常にそちら ――
        // 更新コストが下がり、カメラから遠いプローブにも影が落ちるようになるため
        // (ReflectionModeForCapabilityと同じ考え方)
        static constexpr DDGIRayMode DDGIRayModeForCapability(bool raytracingAvailable)
        {
            return raytracingAvailable ? DDGIRayMode::Raytraced : DDGIRayMode::Raster;
        }
        // 【既定はDXRが使えるならDXR】DX11とDXR非対応機は自動的にラスタのまま。
        // 実際の値はレイトレーシングの可否が分かった時点(Initialize)で入れ直す
        DDGIRayMode m_DDGIRayMode = DDGIRayMode::Raster;
        // プローブ分類(壁や地面の内部に埋まったプローブをサンプリングから外す)を行うか。
        //
        // 【レイトレース経路でのみ意味を持つ】裏面に当たったことを記録できるのはDXR経路だけで、
        // ラスタ経路は裏面カリングの結果それを「空」として見てしまうため分類できない。
        // ラスタ経路ではこのフラグに関わらず分類は掛からない
        bool m_DDGIProbeClassificationEnabled = true;
        // 裏面ヒット率がこれを超えたプローブを「信用しない」と判定する。
        //
        // 【既定値0.5の根拠】2つのシーンで分布と効果を実測して決めた。分布そのものは
        // デバッグ表示「DDGI - プローブ裏面率」で確認できる。
        //
        //   Sponza(1152プローブ)      … きれいに二山。76%が0.05未満、21%が0.5超で、
        //                                その間(0.05〜0.50)はほぼ空。谷が広いので
        //                                この範囲のどこに置いてもほぼ同じ集合になる
        //   BistroInteriorLit(480個) … 二山にならない。0から滑らかに減る連続分布で、
        //                                0.5を超えるのは1.9%(9個)、0.9超は0.6%(3個)だけ
        //
        // つまり「谷に置く」という決め方はSponzaでしか使えない。そこで**効果の向きが
        // 両シーンで揃う位置**を採った ―― 0.5では両シーンとも「わずかに明るくなり、
        // 暗くなる画素は0.1%未満」で一致する(埋まったプローブが配っていた偽の暗さが消えるため)。
        //
        // 【0.25を採らなかった理由】RTXGIの既定値だが、Bistroの連続分布を途中で切るため
        // 壁の領域が18〜22%暗くなる。しきい値を0.75(3個だけ無効)にすると同じ領域の変化は
        // +0.004%(82800画素中28画素が±1階調)まで落ちるので、この暗化は
        // 「明らかに埋まったプローブ」ではなく中間の率を持つプローブを落としたことによると分かる。
        // それが正しい方向だと言える根拠が無いため採らなかった。
        //
        // 【この根拠の弱いところ】数字を額面どおりに受け取らないこと。
        //   - BistroInteriorLitは画面がほぼ真っ暗(平均輝度1.19/255、95%の画素が4未満)で、
        //     「-18%」は平均にすると0.5階調ほどしかない。個々の画素では最大41〜66階調
        //     動いているので実体はあるが、百分率だけを根拠に使わないこと
        //   - Sponza側の変化はほとんどが±1階調で、8bitの量子化の底に張り付いている
        //   - 分布は8bitのデバッグ表示越しに読んでいる。率は約1/1536刻みで計算されるので、
        //     しきい値の境目にいるプローブの数え方には±数個の誤差がある
        //
        // 0.5は物理的な意味も明確で、「全レイの半分より多くが面の裏側に当たった」
        // = そのプローブは外より内側にいる、という判定になる
        float m_DDGIBackfaceThreshold = 0.5f;

        // レイトレース経路で太陽の影レイを撃つか。
        //
        // 【何のためにつまみにしてあるのか】これを切ると「影が落ちない」ラスタ経路の
        // 既知の制約と同じ状態になる。切り替えて絵と数値が動くことが、
        // レイトレース経路が実際に走っていることの対照実験になる(差分ゼロは合格ではない)。
        // 常用の想定は有効側で、ラスタ経路には効かない
        bool m_DDGISunShadowRayEnabled = true;
        // どちらの経路が実際に走ったかを、切り替わったときだけログへ出すための状態。
        // 毎フレーム出すと埋もれるが、出さないと「切り替えたつもり」の取り違えに気づけない
        bool m_DDGIRayModeReported = false;
        bool m_DDGIRayModeReportedRaytraced = false;
        // 停止判定用。最後に「焼き上がりに影響する状態」が変わった時点の署名。
        // 反射プローブと同じComputeProbeBakeSignature()を使う ―― DDGIのキャプチャも
        // 同じFrameConstants(太陽・時刻・影・ライト・IBL・自発光)を読むため、影響する状態は同じ
        uint64_t m_DDGIBakeSignature = 0;
        bool m_DDGIBakeSignatureValid = false;
        // 署名が変わらないまま完了した巡回数。停止判定に使う
        uint32_t m_DDGIStableCycles = 0;
        // 収束済みとみなして更新を止めている状態。署名が変わると倒れる
        bool m_DDGIUpdateSuspended = false;
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
        static constexpr uint32_t kDDGIBounceCycles = 4;

        // 格子上のプローブ番号からワールド座標を求める。番号の分解は
        // index = x + y*Cx + z*Cx*Cy で、シェーダー側の並びと一致させること
        // --- クリップマップLODの格子 ---
        //
        // LOD k は間隔が ProbeSpacing * 2^k。プローブ数は全LOD共通なので、覆う範囲は
        // LODが1つ上がるごとに2倍になる。アトラスはLODを縦に積むだけで済む ――
        // 通し番号 slot = k*(Cx*Cy*Cz) + z*Cx*Cy + y*Cx + x を使うと、既存の
        // 「行 = slot/(Cx*Cy)、列 = slot%(Cx*Cy)」がそのまま LOD k の行 [k*Cz, (k+1)*Cz) を指す。
        // このおかげで**更新CSのアトラス座標式は1文字も変えなくてよい**。
        DirectX::XMFLOAT3 ComputeDDGILODSpacing(uint32_t lod) const;
        // そのLODの格子の原点。追従するときはLOD自身の格子へスナップし、カメラを中心に置く。
        // 追従しないときは、LOD0は.ksceneのOriginそのまま、上のLODは中心を保ったまま広がる
        DirectX::XMFLOAT3 ComputeDDGILODOrigin(uint32_t lod) const;
        // 原点に対応する格子の整数座標。トロイダル(剰余)addressingの基準になる。
        // **CPUとシェーダーで同じ値を使う必要があるので、CPU側で求めて渡す**
        // (原点÷間隔をシェーダー側でも計算すると、丸めが食い違ったときに
        //  プローブの位置とアトラスのセルがずれる)
        DirectX::XMINT3 ComputeDDGILODBaseIndex(uint32_t lod) const;

        // そのスロットがいま担当しているワールド格子座標。dirty判定の基準になる
        DirectX::XMINT3 ComputeDDGIProbeWorldCoord(uint32_t probeIndex) const;

        DirectX::XMFLOAT3 ComputeDDGIProbePosition(uint32_t probeIndex) const;

        // m_GIVolumeのProbeCountsに合わせてアトラス2枚を確保し直す。ボリュームが無いシーンでは
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
        uint32_t ClampDDGIProbesPerFrameToConstantRing(uint32_t requested);
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

        // 昼夜サイクル: ImGuiで操作する時刻(0〜24時)。太陽の向き・色・環境光・空の明るさに反映される
        float m_TimeOfDay = Defaults::TimeOfDay;
        bool m_TimeAutoAdvance = Defaults::TimeAutoAdvance;
        float m_TimeAdvanceSpeed = Defaults::TimeAdvanceSpeed; // 自動進行時、1秒あたりに進む時間(時)

        // 水面。m_TimeOfDayの自動進行とまったく同じ方針
        // (RenderThreadMainが同じ場所・同じ条件分岐の形で進める)で、水面法線マップの
        // スクロール位相を[0,1)で持つ。FrameConstants.TimeParams.xとしてWater.hlslへ渡る
        float m_WaterScrollOffset = 0.0f;
        // trueにすると波のスクロールが止まる(m_TimeAutoAdvanceの水面版に近いが、
        // 「動かす/止める」の2値なので速度ではなくフラグにしている)
        bool m_WaterTimeFrozen = Defaults::WaterTimeFrozen;
        // シーン読み込み時にScene::WaterWaveScale等から初期化され、以降はUIで実行時上書きできる
        // (m_ReflectionModeがScene.SSREnabledから初期化されるのと同じ設計、ApplyLoadedScene参照)。
        // m_WaterWaveSpeedはm_WaterScrollOffsetの進行速度に使われる。m_WaterWaveScale/
        // m_WaterWaveStrengthはFrameConstants.TimeParams.y/zとしてWater.hlslへ渡り、層のUV
        // スケール(kWaterLayerAUvScale等への倍率)・波の振幅(距離減衰のweightへの倍率)に効く
        float m_WaterWaveScale = Defaults::WaterWaveScale;
        float m_WaterWaveSpeed = Defaults::WaterWaveSpeed;
        float m_WaterWaveStrength = Defaults::WaterWaveStrength;
        // 水面の反射に解析空フォールバックを使うか(SSRの水面分岐)。SSRレイが画面外へ抜けた・
        // 最大距離まで判定がつかなかった水面画素で、プリフィルタ済み鏡面IBL(128pxベースの
        // キューブマップをラフネス由来のミップで引くため広い水面ではにじむ)の代わりに
        // Sky.hlsliのSkyColorを画面解像度で直接評価する(SSR.hlslのPSMain参照)。
        // 効果が出るのはm_ReflectionMode==ScreenSpaceのときだけで、かつ手続き空が無効な
        // シーンでは常に無効化される(SSRパスのExecute内、usingProceduralSkyとのAND判定)
        bool m_WaterAnalyticSkyReflection = Defaults::WaterAnalyticSkyReflection;

        // --- 平面反射 ---
        // 水面に不透明ジオメトリの鏡像を映す専用フォワードパス。設計判断の詳細は
        // Shaders/3D/PlanarReflection.hlsl冒頭のコメントを参照。反射解像度はレンダー解像度に
        // m_PlanarReflectionResolutionScaleを掛けた値で、実際の作成はCreatePlanarReflectionTargetsが行う
        std::unique_ptr<RHI::IRHITexture> m_PlanarReflectionColor;
        std::unique_ptr<RHI::IRHITexture> m_PlanarReflectionDepth;
        std::unique_ptr<RHI::IRHIShader> m_PlanarReflectionVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_PlanarReflectionPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PlanarReflectionPipelineState;
        // 鏡映カメラで描くとワインディングが全反転するため、m_GBufferPipelineStateMirroredと同じ
        // 仕組み(FrontCounterClockwiseの反転)で吸収する。ただし選択条件はinstance.IsMirroredの
        // 否定になる(Render()側のExecute内のbindPipelineStateラムダ参照)
        std::unique_ptr<RHI::IRHIPipelineState> m_PlanarReflectionPipelineStateMirrored;
        // captureProbeFaceと同じ役割の専用FrameConstants(共有のm_FrameConstantBufferとは別インスタンス)。
        // ViewProj/CameraPosition/PlanarReflectionPlaneだけをこのパス用に差し替える
        std::unique_ptr<RHI::IRHIBuffer> m_PlanarReflectionConstantBuffer;
        bool m_PlanarReflectionEnabled = Defaults::PlanarReflectionEnabled;
        // 反射解像度の倍率(レンダー解像度に対する比)。EngineDefaults.hのコメント参照
        float m_PlanarReflectionResolutionScale = Defaults::PlanarReflectionResolutionScale;
        // 波の法線による画面UVのずらし量(SSR.hlslが読む)
        float m_PlanarReflectionDistortion = Defaults::PlanarReflectionDistortion;
        // 「システム」パネルのm_PendingRenderWidth/Height・m_RenderResolutionDirtyとまったく同じ方式
        // (要求を記録するだけにしてRender()の先頭でまとめて反映する。理由はCreateRenderTargets/
        // RequestRenderResolutionのコメント参照。GPUがまだ参照しているテクスチャを
        // 即座には破棄できないため)
        float m_PendingPlanarReflectionResolutionScale = Defaults::PlanarReflectionResolutionScale;
        bool m_PlanarReflectionResolutionDirty = false;
        // CreatePlanarReflectionTargetsが確保した反射解像度の実値(幅・高さ)。デバッグ表示
        // (Present.hlslのレターボックス計算)が実寸を必要とするため保持しておく
        uint32_t m_PlanarReflectionWidth = 0;
        uint32_t m_PlanarReflectionHeight = 0;
        // 複数の水面インスタンスが異なる高さで見つかったことを検出した最初のフレームだけ
        // 警告ログを出すためのフラグ(m_LightTileOverflowLoggedと同じ作法)。平面反射は
        // 「水面は単一の水平な平面である」という前提に立っており、複数ある場合は最初のものだけを使う
        bool m_PlanarReflectionMultipleWaterLogged = false;

        // --- 雲 ---
        // 積雲(1層目)の有効/無効。巻雲は m_CirrusEnabled が別に持つ。
        // 無効時はFrameConstants.CloudParams0.xへ被覆率0を渡し、Sky.hlsli側の早期脱出
        // (SkyColor)を通す。CloudCoverageスライダー自体は動かせるが効果が出ない状態になる
        bool m_CloudEnabled = Defaults::CloudEnabled;
        // 被覆率。0.40は写真の見た目に寄せて選んだ値であり、物理的な導出ではない
        // (実測で調整可能。EngineDefaults.h参照)
        float m_CloudCoverage = Defaults::CloudCoverage;
        // 雲底の高度[m](カメラのワールドY基準。Sky.hlsli EvaluateCloudLayerが視線との交点を
        // 求めるのに使う)
        float m_CloudAltitude = Defaults::CloudAltitude;
        float m_CloudUvScale = Defaults::CloudUvScale;
        float m_CloudDensity = Defaults::CloudDensity;
        // 風速[m/s]。実世界の速度としてUIで直感的に扱えるようにしてあり、ノイズ空間の移動量への
        // 換算(CloudUvScaleを掛ける)はRenderThreadMainのm_CloudScrollOffset更新側で行う
        float m_CloudWindSpeed = Defaults::CloudWindSpeed;
        // 風向き(度)。太陽方位角(m_SunAzimuthDegrees)と同じ規約(X軸0度、Z軸(+方向)90度)
        float m_CloudWindDirectionDegrees = Defaults::CloudWindDirectionDegrees;
        float m_CloudForwardG = Defaults::CloudForwardG;
        // 積雲をボリューム(スラブのレイマーチ)として描くか。falseで従来の平面へ戻る。
        // シェーダー側へはCloudParams1.wの厚みを0にすることで伝える(専用のフラグは持たない)
        bool m_CloudVolumetric = Defaults::CloudVolumetric;
        // 雲底から雲頂までの厚み[m]。EngineDefaults::CloudThicknessのコメント参照
        float m_CloudThickness = Defaults::CloudThickness;
        // trueにすると雲のスクロールが止まる(m_WaterTimeFrozenの雲版。A/B比較などスクロールが
        // 揺れると困る場面で使う)
        bool m_CloudTimeFrozen = Defaults::CloudTimeFrozen;
        // 積雲のボリュームレイマーチの段数。**このパスのコストの主なつまみ**。
        // FrameConstants::CloudQualityParams.xとして渡り、SkyCloud.hlslだけが読む
        // (ボリューム経路を持つのがこのシェーダーだけのため。詳細はそちらのコメント)。
        // 減らすと雲の内部の階調が粗くなる=絵が変わるので、41.17までの「見た目を変えない削減」
        // とは性質が違う。品質プリセットの低/中から振るための値である
        uint32_t m_CloudRaymarchSteps = Defaults::CloudRaymarchSteps;
        // 段数の上限。**Sky.hlsliのkCumulusRaymarchStepsMaxと一致させること**
        static constexpr uint32_t kCloudRaymarchStepsMax = 32;
        // 風によるノイズ空間の移動量。m_WaterScrollOffsetと同じくUIつまみではなく内部状態で、
        // RenderThreadMainがSky.hlsliのkCloudNoisePeriodと同じ周期でstd::fmodしながら進める
        DirectX::XMFLOAT2 m_CloudScrollOffset{ 0.0f, 0.0f };
        // 判断B(被覆率による平均透過率をIBLキューブのベイク時にだけ掛ける)のキャッシュ。
        // bakeSkyThisFrameブロックで確定させ、ベイクとFrameConstantsが同じタイミングの
        // 値を見るようにする(GPU側のm_SkyParametersBufferと同じ更新タイミング)。
        // 巻雲(m_CirrusCoverage)も加味した2層の積になる
        // (ComputeCloudAverageTransmittance参照)
        float m_ActiveCloudTransmittance = 1.0f;

        // --- 巻雲(高層のレイヤーを2層目として追加し雲を多層化する) ---
        // m_CloudEnabled=falseのときと同じく、無効時はFrameConstants.CloudParams2.xへ
        // 被覆率0を渡し、Sky.hlsli側の早期脱出(SkyColor、判断C)を通す
        bool m_CirrusEnabled = Defaults::CirrusEnabled;
        float m_CirrusCoverage = Defaults::CirrusCoverage;
        float m_CirrusAltitude = Defaults::CirrusAltitude;
        float m_CirrusUvScale = Defaults::CirrusUvScale;
        float m_CirrusDensity = Defaults::CirrusDensity;
        // 風速[m/s]。風向はm_CloudWindDirectionDegreesを積雲と共有する(同じ風系という前提)
        float m_CirrusWindSpeed = Defaults::CirrusWindSpeed;
        // fBmのUV(U方向)を伸ばして筋状にする倍率
        float m_CirrusAnisotropy = Defaults::CirrusAnisotropy;
        // 風によるノイズ空間の移動量(巻雲側)。m_CloudScrollOffsetとまったく同じ形で
        // RenderThreadMainがkCloudNoisePeriodの周期でstd::fmodしながら進める。
        // 凍結トグルはm_CloudTimeFrozenを共有する(片方にしか効かないとA/B比較の対照が
        // 崩れるため。RenderThreadMainのスクロール更新箇所を参照)
        DirectX::XMFLOAT2 m_CirrusScrollOffset{ 0.0f, 0.0f };

        // --- 星空 ---
        // 夜空の星。Sky.hlsliのSkyColorが方向ハッシュで解析的に描く(テクスチャは使わない)。
        // **IBLキューブ(SkyGenerate.hlsl)へは焼かない**ので、これらを変えても空の焼き直しは要らない
        // (雲の風と同じ扱い。m_SkyBakeDirtyを立てないこと)
        bool m_StarsEnabled = Defaults::StarsEnabled;
        float m_StarsDensity = Defaults::StarsDensity;
        float m_StarsBrightness = Defaults::StarsBrightness;
        // またたきの強さ。既定0。上げるとTAAがちらつきとして拾い、A/B比較の再現性も落ちる
        float m_StarsTwinkle = Defaults::StarsTwinkle;

        // 太陽が昇ってくる方位角(度)。X軸を0度、Z軸(+方向)を90度とした水平面上の角度で、
        // ImGuiで調整する(ComputeSunLightingが太陽の日の出側水平方向として使用する)
        float m_SunAzimuthDegrees = Defaults::SunAzimuthDegrees;

        // 大気の濁り具合(Preetham xyYモデルのタービディティ)。値が大きいほど地平線が白く
        // 霞み、天頂の青が薄くなる。定義域はおおむね1.7〜10(EngineDefaults.h::SkyTurbidity参照)。
        // 変更すると空の焼き直しが必要(Render()のturbidityMoved判定参照)
        float m_SkyTurbidity = Defaults::SkyTurbidity;
        // 空の彩度(アート指定)。既定1.0=Preethamそのまま。詳細はEngineDefaults::SkySaturation。
        // .ksceneの[Scene]SkySaturationで初期化され、以降はUIで上書きできる
        // (m_ReflectionModeがScene.SSREnabledから初期化されるのと同じ設計)
        float m_SkySaturation = Defaults::SkySaturation;

        // 月の位置。**時刻には連動せず、ここで指定した固定位置に居続ける**。
        // 実際の月は太陽とは独立した周期(朔望月)で動くので、反太陽方向に固定するのは
        // 「常に満月かつ常に真夜中に南中する」という二重の簡略化になってしまう。
        // 任意の月齢・任意の時刻の見え方を作れるよう、位置は手動指定にしている。
        // 方位角の規約は太陽と同じ(X軸が0度、Z軸(+方向)が90度)。
        // 仰角が0度以下なら月は地平線下にあり、月光は出ない。
        // シーンを切り替えても引き継がれる(.ksceneのキーは持たない)
        float m_MoonAzimuthDegrees = Defaults::MoonAzimuthDegrees;
        float m_MoonElevationDegrees = Defaults::MoonElevationDegrees;

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

        // ポイント/スポットライトのリスト(t8、StructuredReadOnly)と、有効ライト数を渡すb1。
        // 太陽(平行光)はb0のLightDirection/LightColorのまま(詳細はdocs/Architecture.html参照)
        std::unique_ptr<RHI::IRHIBuffer> m_LightBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_LightingConstantBuffer;
        // 容量(kMaxLights)超過を検出した最初のフレームだけ警告ログを出すためのフラグ
        bool m_LightOverflowLogged = false;

        // ポイント/スポットライトのスクリーンスペースシャドウ(接触影)の設定。
        // シャドウマップを増やさず、G-Bufferの深度バッファをライト方向へレイマーチして影を出す
        // (Shaders/3D/ScreenSpaceShadow.hlsli、docs/Architecture.html 18章)
        bool m_ScreenSpaceShadowEnabled = Defaults::ScreenSpaceShadowEnabled;
        // レイマーチのステップ数。ScreenSpaceShadow.hlsliのkSSSMaxStepCount(64)が上限
        int m_ScreenSpaceShadowStepCount = Defaults::ScreenSpaceShadowStepCount;
        // 1本のレイが伸びる最大のワールド距離。ライトまでの距離がこれより短ければそちらが優先される。
        // 短いほど「接触影」寄りになり、コストも下がる
        float m_ScreenSpaceShadowMaxRayLength = Defaults::ScreenSpaceShadowMaxRayLength;
        // 遮蔽と判定する深度差の上限。深度バッファがサーフェスの厚みを持たないための近似で、
        // 大きすぎると遠景が無限に厚い遮蔽物として振る舞い、小さすぎると薄い物体を貫通する
        float m_ScreenSpaceShadowThickness = Defaults::ScreenSpaceShadowThickness;
        // レイ始点を法線方向へ押し出す量(View空間深度に比例させる係数)。自己遮蔽(シャドウアクネ)対策
        float m_ScreenSpaceShadowNormalBias = Defaults::ScreenSpaceShadowNormalBias;
        // ヒット位置が画面端に近いときに影を弱める幅(UV単位)。SSRのkSSREdgeFadeDistanceと同じ役割
        float m_ScreenSpaceShadowEdgeFade = Defaults::ScreenSpaceShadowEdgeFade;
        // 1ピクセルが撃てるシャドウレイ数の上限。ライトを増やしてもレイマーチのコストが
        // 線形に伸び続けないようにするための予算
        int m_ScreenSpaceShadowMaxLightsPerPixel = Defaults::ScreenSpaceShadowMaxLightsPerPixel;

        // タイルライトカリング(Shaders/3D/LightCulling.hlsl)。画面を16x16ピクセルのタイルに分け、
        // タイルごとに「そのタイルに届くライト」のインデックスリストをコンピュートシェーダーで作る。
        // 直接光パスはそのリストだけをループするため、ピクセルあたりのコストが
        // シーン全体のライト数ではなくタイル内のライト数になる。
        // これは純粋な最適化であり、有効/無効で最終画像が変わってはならない
        // タイルライトカリングのタイルサイズ(1辺のピクセル数)。
        // LightCulling.hlsl の kTileSize および numthreads と必ず一致させること
        static constexpr uint32_t kLightTileSize = 16;
        // 1タイルが保持できるライト数の上限。LightCulling.hlsl の kMaxLightsPerTile および
        // DirectLighting.hlsl の同名の定数と必ず一致させること(バッファのストライドがこの値で決まる)。
        // HLSL側はgroupshared配列のサイズに使うためコンパイル時定数である必要があり、
        // C++からの受け渡しでは代用できないので、3箇所で同じ値を書く形になっている。
        // .cppの無名名前空間ではなくここに置いてあるのは、DebugViewPanelがヒートマップの
        // 上限としてこの値を使うため(UIパネルはfriendなのでprivateのまま参照できる)
        static constexpr uint32_t kLightTileCapacity = 64;
        // ライトグリッド1タイルぶんの要素数(先頭1個がライト数、残りがライトインデックス)
        static constexpr uint32_t kLightTileStride = 1 + kLightTileCapacity;

        // MegaLightsの候補プールが1タイルあたりに抽出する候補の数(K)。
        // ライトタイルの容量と違い**これは打ち切りではなく抽出数**で、タイルへ何灯届いていても
        // ここで決めた本数だけを重みつきで取り出す。届いた灯が欠落するわけではない
        // (どの灯も w_i / SumW の確率で選ばれる)ため、容量超過のような静かな欠落は起きない。
        // 【実行時に振れる。ここは確保の上限】1タイルの抽出数Kは
        // m_MegaLightsTilePoolCapacity が持ち、シェーダへは定数バッファで渡している。
        // バッファの確保だけがコンパイル時の上限を要るのでここに残す
        static constexpr uint32_t kMegaLightsTilePoolCapacity = 128;
        // Kの下限。これを下回るとタイルに届く灯を代表できない
        static constexpr int32_t kMegaLightsTilePoolMinCapacity = 8;
        // 候補プール1タイルぶんの要素数。先頭6個がヘッダ(SumW / 届いた灯数 / 有効候補数 / 予約 /
        // 手前のViewZ / 奥のViewZ)、
        // 以降は候補1つにつき2個(ライト番号と重み)。MegaLightsTilePool.hlsl 冒頭のレイアウトと一致させること
        static constexpr uint32_t kMegaLightsTilePoolStride = 6 + 2 * kMegaLightsTilePoolCapacity;
        // 1画素あたりの標本数の上限。リザーババッファはこの倍数まで太る
        //(16バイト x 画素数 x 標本数。2560x1440・4本で236MB)ので、際限なく上げさせない。
        // クアッド層化は4層なので、4を超えると層の割り当てが一巡して効きが鈍る
        static constexpr int32_t kMegaLightsMaxSamplesPerPixel = 4;

        std::unique_ptr<RHI::IRHIShader> m_LightCullingComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_LightCullingPipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_LightCullingConstantBuffer;
        // ライトグリッド本体(BufferUsage::StructuredRW)。コンピュートがUAVで書き、
        // 直接光パスのピクセルシェーダがSRVで読む。解像度に依存するためCreateRenderTargetsで作り直す
        std::unique_ptr<RHI::IRHIBuffer> m_LightTileBuffer;
        uint32_t m_LightTileCountX = 0;
        uint32_t m_LightTileCountY = 0;
        bool m_LightCullingEnabled = Defaults::LightCullingEnabled;
        // タイル容量の超過"条件"(シーンのライト数が容量を超えている)を検出した最初のフレームだけ
        // 警告ログを出すためのフラグ(m_LightOverflowLoggedと同じ作法)。
        // 実際に超過したかはGPU側にしか無いため、確認はDebugView::LightTilesのマゼンタで行う
        bool m_LightTileOverflowLogged = false;
        // DebugView::LightTilesのヒートマップで赤に振り切る基準のライト数。容量(64)を基準にすると
        // 実データ(数灯)ではほぼ真っ青で差が読めないため、別のつまみにしてある
        int m_LightTileHeatmapMax = Defaults::LightTileHeatmapMax;

        // --- 自前ソフトウェアラスタライザ(46章) -------------------------------------------
        //
        // 三角形をコンピュートシェーダーで自前にラスタライズする比較用の経路。
        // 既存のG-Buffer経路には一切影響せず、専用のバッファへ描いてDebugViewで見る。
        // 詳細はShaders/3D/SoftwareRasterCommon.hlsli冒頭。
        //
        // DX12かつSM 6.6 + Int64ShaderOps + bindlessの環境でのみ動く
        // (IRHIDevice::SupportsSoftwareRaster)。

        // スクリーンbboxの画素面積がこれを超えた三角形は、1スレッドでラスタライズせず
        // 巨大三角形リストへ回す既定値。4096 = 64x64相当。
        //
        // 【この値が上限を決めている】小三角形パスは1スレッド1三角形なので、
        // このしきい値がそのまま「1スレッドが回す最大ループ回数」になる。
        // 上げすぎると画面を覆う三角形1個でTDRに達する
        static constexpr uint32_t kSWRasterDefaultLargeTriangleArea = 4096;
        // しきい値の可動範囲。UIから振って2つの経路を突き合わせるために使う(下のメンバ参照)
        static constexpr uint32_t kSWRasterMinLargeTriangleArea = 16;
        static constexpr uint32_t kSWRasterMaxLargeTriangleArea = 1u << 24;
        // 巨大三角形リストの容量(要素数)。超えた分は描かれず、CSResolveが画面左上を
        // マゼンタで塗って知らせる
        static constexpr uint32_t kSWRasterLargeListCapacity = 4096;
        // 1フレームに扱えるメッシュレコード数の上限。Bistro Exteriorで約400
        static constexpr uint32_t kSWRasterMaxMeshes = 2048;
        // CSRasterの1グループのスレッド数。SoftwareRaster.hlslの
        // KURENAI_SWRASTER_GROUP_SIZEと一致させること
        static constexpr uint32_t kSWRasterGroupSize = 64;
        // Dispatchの1次元あたりの上限(65535)に収めるための2D分解の刻み
        static constexpr uint32_t kSWRasterMaxGroupsPerAxis = 32768;

        std::unique_ptr<RHI::IRHIShader> m_SoftwareRasterComputeShader;
        std::unique_ptr<RHI::IRHIShader> m_SoftwareRasterLargeComputeShader;
        std::unique_ptr<RHI::IRHIShader> m_SoftwareRasterResolveComputeShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_SoftwareRasterPipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_SoftwareRasterLargePipelineState;
        std::unique_ptr<RHI::IRHIPipelineState> m_SoftwareRasterResolvePipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_SoftwareRasterConstantBuffer;
        // メッシュ1件 = 1レコード。毎フレームm_Scene.Instancesから組み直す
        std::unique_ptr<RHI::IRHIBuffer> m_SoftwareRasterMeshInfoBuffer;
        // 巨大三角形の通し番号リストと、その個数を兼ねた間接ディスパッチ引数
        std::unique_ptr<RHI::IRHIBuffer> m_SoftwareRasterLargeEntriesBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_SoftwareRasterIndirectArgsBuffer;
        // 以下は解像度に依存するためCreateRenderTargetsで作る。
        // visibility bufferは画素あたり64bit(深度32 + 三角形番号32)
        std::unique_ptr<RHI::IRHIBuffer> m_SoftwareRasterVisibilityBuffer;
        std::unique_ptr<RHI::IRHITexture> m_SoftwareRasterColor;
        std::unique_ptr<RHI::IRHITexture> m_SoftwareRasterDepth;
        // m_GBufferNormalとまったく同じR16G16_Floatのオクタヘドラル符号化。
        // Present.hlslのMode 7で並べて差分を取れるようにするため
        std::unique_ptr<RHI::IRHITexture> m_SoftwareRasterNormal;

        // デバイスが対応していて、かつシェーダー/リソースの作成に成功したか。
        // どちらかが欠けたらUIのチェックボックスごと無効化する
        bool m_SoftwareRasterAvailable = false;
        bool m_SoftwareRasterEnabled = Defaults::SoftwareRasterEnabled;
        // 巨大三角形とみなすbbox画素面積のしきい値。
        //
        // 【実行時に振れるようにしている理由】小三角形パス(CSRaster)と巨大三角形パス
        // (CSRasterLarge)は同じ三角形を別のコードで塗る。極端に小さくすればほぼ全三角形が
        // 巨大リストへ回り、極端に大きくすればすべてCSRaster単独になるので、
        // **両極端で同じ絵が出ること**を確かめれば2つの経路が一致していると言える。
        // ビルドし直さずにこの対照実験ができるよう定数ではなくメンバにしてある
        // (「片方が実行されていない」という失敗を先に潰すための手順。ab-compareスキル)
        int m_SoftwareRasterLargeTriangleArea = static_cast<int>(kSWRasterDefaultLargeTriangleArea);
        // メッシュレコード数が容量を超えた最初のフレームだけ警告を出すためのフラグ
        // (m_LightTileOverflowLoggedと同じ作法)
        bool m_SoftwareRasterMeshOverflowLogged = false;

        // --- 品質プリセット(41章) ---------------------------------------------------------
        //
        // 個別のつまみを一括で振るための横断的な設定。UIの「システム」パネルから適用する。
        // ここに置いているのは、この時点までにReflectionMode等の入れ子の列挙がすべて
        // 宣言済みだからで、機能上の所属を示すものではない。
        //
        // 【どの項目を入れるかは実測で決めている】Intel UHD Graphics 620 / 1280x720 / DX11 /
        // Release で5シーンを計測した結果、フレーム時間を支配していたのは以下だった:
        //   DDGIのプローブ更新 40〜47ms(GIVolumeを持つシーン。フレームの約4割)
        //   SSR 31ms(水面のあるシーン)
        //   ボリュメトリック積雲 約10ms(空が画面の大半を占めるシーン)
        // 逆にシャドウは全シーンで4カスケード合計1ms未満だったため、シャドウ関連は一切触らない。
        // タイルドライトカリングは見た目を変えない最適化なので常に有効のままにする。
        // 内部レンダー解像度は独立したつまみ(同じパネルの「解像度」節)であり、ここからは変えない
        enum class QualityPreset
        {
            Low,     // 低
            Medium,  // 中
            High,    // 高(= シーンを読み込んだ直後の状態へ戻す)
        };

        // 品質プリセットが触る設定の一式。
        //
        // 【プリセット「高」はエンジンの静的な既定ではなく「シーン読み込み直後の値」へ戻す】
        // .ksceneはSSR・TAAを自分で指定できる(ApplyLoadedScene参照。実例として
        // MontSaintMichel.ksceneはどちらも明示的に有効化している)。静的なDefaults::へ戻すと
        // 「高にしたらシーンが要求した反射が消える」ことになる。m_SceneDefaultReflectionModeが
        // UIの右クリック(既定値へ戻す)に対して同じ問題を解いており、プリセットもそれに倣う
        struct QualitySettings
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
        QualitySettings CaptureQualitySettings() const;
        // 一式を各メンバへ書き戻す。平面反射の解像度倍率だけはレンダーターゲットの作り直しを
        // 伴うため直接代入せず、RequestPlanarReflectionResolutionScale()経由で要求する
        void ApplyQualitySettings(const QualitySettings& settings);
        // プリセットを適用する(SystemPanel = Renderスレッドから呼ばれる)
        void ApplyQualityPreset(QualityPreset preset);

        // シーンを読み込んだ直後の値。ApplyLoadedSceneが控え、プリセット「高」が戻る先になる
        QualitySettings m_SceneDefaultQuality;
        // 最後に適用したプリセット。UIのComboの表示位置に使う。
        // 【現在の状態を表すものではない】プリセットを適用した後に個別のつまみを動かしても
        // ここは追従しない(全つまみの変更を捕まえる仕掛けを持たないため)。
        // Comboは「今どれか」ではなく「どれを一括適用するか」の選択として読むこと
        QualityPreset m_QualityPreset = QualityPreset::High;

        // 現在描画しているシーン。ApplyLoadedScene(Renderスレッド)だけが差し替え、
        // Render()とUIパネル(いずれもRenderスレッド)だけが読む。つまりRenderスレッド専有の状態で、
        // ミューテックスによる保護は不要(LoadSceneがUpdateスレッドから直接書き換える構成だと
        // ミューテックスが要る。26章)。
        //
        // 【読み込み中は空になる】シーン切り替えを開始した時点で旧シーンを手放すため
        // (VRAMの二重常駐を避けるため)、読み込みが終わるまでInstancesが空のまま描画される
        Assets::Scene m_Scene;
        // m_Sceneに対応するレイトレーシングの高速化構造(BLAS/TLAS)とシーンジオメトリの
        // 統合バッファ。Loaderスレッドがm_Sceneと一緒に構築し、ApplyLoadedSceneが差し替える。
        // デバイスがレイトレーシング非対応(DX11、またはDXR Tier 1.1未満のアダプタ)の
        // 場合は空のまま(IsValid()==false)で、描画側は従来のスクリーンスペース手法を使う。
        //
        // 【破棄順】m_Sceneより後に宣言することで、メンバ破棄順(宣言の逆順)により
        // m_Sceneの頂点/インデックスバッファより先に破棄される
        Assets::RaytracingScene m_RaytracingScene;
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

        // WASD/E/Qの移動速度[m/s]。Shiftを押している間はDefaults::CameraSpeedShiftMultiplier倍。
        //
        // 【スレッド】書き手はRenderスレッド(ScenePanelのスライダとResetSceneDependentParams)、
        // 読み手はUpdateスレッド(UpdateMovement)。m_TargetFPSと同じく、単一のfloatを跨いで
        // 読み書きするだけなので同期は置かない ―― 途中の値が1フレーム見えても
        // 「その1フレームだけ移動量が古い速度で計算される」以上のことは起きない。
        // m_Camera本体はUpdateスレッド専有のまま(この値はそこへ入力されるだけ)。
        //
        // 値はシーン対角から決まるためResetSceneDependentParams()が上書きする。
        // ここの初期化子は最初のシーンを読むまでの値でしかない
        float m_CameraSpeed = Defaults::CameraSpeed;

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

        // 自動監視の有効/無効。**既定はオフ**。A/B比較の最中に勝手に再読み込みが走ると
        // 「同一条件で2回撮る」対照が壊れるため、明示的に入れてもらう
        bool m_SceneAutoReloadEnabled = false;
        // リロード時に現在のカメラを保持するか。オフ(既定)ならファイルの[Camera]を適用する。
        // [Camera]を詰めるときと、飛び回りながら空・水面・露出を詰めるときで要求が逆になる
        bool m_SceneReloadKeepsCamera = false;
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
        // 実在の写真露出値(EV100)。太陽・環境光・ポイント/スポットライトすべてに同じ値がかかる、
        // シーン全体で単一の露出設定(詳細はdocs/Architecture.html参照)
        float m_SceneExposureEV100 = Defaults::SceneExposureEV100;

        // 実際にライト強度へ事前乗算される「実効プリ露出」。m_SceneExposureEV100(ユーザー設定)に
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
        // 実効プリ露出の時間平滑化の速さ[1/秒]。段付きを防ぐために指数追従させる
        float m_EffectiveExposureAdaptSpeed = 2.0f;

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
        // (m_TimeOfDayと同じ扱い)
        float m_RenderDeltaTime = 0.0f;

        // 統計表示用: 1フレームあたりのCPU時間(Renderの呼び出し時間)と、指数移動平均によるFPS。
        // どちらもRenderスレッドのみが書き込み、ImGui描画(同じくRenderスレッド)のみが読むため
        // 追加の排他制御は不要
        float m_CPUFrameTimeMs = 0.0f;
        float m_FPS = 0.0f;

        // 性能ログ(LogFrameStatsIfDue)。プロファイラパネルの表示はその場で消えてしまい後から
        // 比較できないため、FPS・CPU/GPUフレーム時間を一定間隔でログファイルへ残す。
        // すべてRenderスレッドのみが読み書きするため追加の排他制御は不要
        bool m_FrameStatsLoggingEnabled = Defaults::FrameStatsLoggingEnabled;
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
        // 段の切り替えにかける秒数。0にするとポップする(1.1km四方のタイルが丸ごと入れ替わるため
        // 目立つ)。根拠は docs/ImplementationDetail.md
        float m_LODFadeDuration = 0.25f;
        // 切り替え距離のヒステリシス幅。切替点の±5%を不感帯にして、境界での往復を防ぐ
        float m_LODHysteresis = 0.05f;
        // 統計。1フレームあたりの段の切り替え回数と、そのフレームでフェード中のインスタンス数。
        // 【0なら一度も切り替わっていない】LODが効いているかはここでしか分からない
        uint32_t m_LODSwitchCount = 0;
        uint32_t m_LODFadingCount = 0;
        uint64_t m_FrameStatsLODSwitchSum = 0;
        // 【瞬間値ではなく積算する】m_LODFadingCountをそのままログへ出していたときは、
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
        // いま選ばれている段を1つだけ返す(フェード中でも切り替え先だけ)。
        // 半透明・平面反射・ソフトウェアラスタライザ用 ―― これらはクロスディザを実装しておらず、
        // 2段を重ねると同じ画素に両方が描かれてしまうため、フェード中も1段に決め打つ
        const Assets::Model* GetCurrentLOD(size_t instanceIndex) const;
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

        // パス別のドローコール数(1フレーム分)。フラスタムカリングの統計と同じく
        // フレーム先頭でリセットし、LogFrameStatsIfDueが集計期間の平均として出す。
        //
        // 【なぜパスごとに分けるのか】フラスタムカリングの統計が全パス合計になっていて、
        // どのパスが何回描いているのかが分からない。ドローコールの削減はこのエンジンで
        // これから何度も測る対象(メッシュレットによる1モデル1ドロー化、GPU駆動描画)で、
        // 「G-Bufferは減ったがシャドウは減っていない」のような片手落ちは
        // パス別に見ないと気づけない。
        //
        // 数えるのはCPUが発行したDrawIndexed/DispatchMeshの回数で、
        // 増幅シェーダーがカリングした後に実際にラスタライズされた塊の数ではない
        uint32_t m_DrawCallsGBuffer = 0;
        uint32_t m_DrawCallsShadow = 0;
        uint32_t m_DrawCallsDepthPrepass = 0;
        // 直前に描き終えたフレームの値。**UIパネルはこちらを読むこと** ――
        // 上のカウンタはフレーム先頭で0に戻るため、Renderの外で描かれるUIからは常に0に見える
        uint32_t m_DrawCallsGBufferLastFrame = 0;
        uint32_t m_DrawCallsShadowLastFrame = 0;
        uint32_t m_DrawCallsDepthPrepassLastFrame = 0;
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

        // 完成した最後のフレームの値。UIパネルはRenderの外で描かれるため、上のカウンタを
        // そのまま読むとリセット直後の0になる(ドローコール数のm_DrawCalls*LastFrameと同じ)
        uint32_t m_FrustumCullTestedLastFrame = 0;
        uint32_t m_FrustumCullCulledLastFrame = 0;
        uint32_t m_MeshCullTestedLastFrame = 0;
        uint32_t m_MeshCullCulledLastFrame = 0;
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
