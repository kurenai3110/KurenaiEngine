#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "Core/Logger.h"
// パス群のベイク状態と能力を問い合わせるため、前方宣言では足りない
#include "../Passes/DDGIPasses.h"
#include "../Passes/EnvironmentPasses.h"
#include "../Passes/GeometryPasses.h"
#include "../Passes/LightingConstants.h"
#include "../Passes/ReflectionProbePasses.h"
#include "../ShaderInterop/FrameConstants.h"

#include "CloudTransmittance.h"
#include "DroneShowResources.h"
#include "GPULight.h"
#include "GPULightBuild.h"
#include "GPUReflectionProbe.h"
#include "RenderFrameContext.h"
#include "SampleSequence.h"
#include "SunLighting.h"

// BuildFrameContext から切り出した、フレームの値を組み立てる各段(段階7.5)。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま。RenderFrame.cpp と同じ作法)。
//
// **定義の並びは BuildFrameContext から呼ぶ順に合わせてある。** 呼ぶ順が実行順の一部で、
// 後段が前段の書いた frameContext のフィールドを読むため、追うときはこの順で読む。
//
// 【ラムダを1つも書かないこと】段階6の寿命事故はすべてラムダの参照捕捉だった
// (Render() のローカルを捕捉したまま graph.Execute() まで生き延びる)。
// このファイルに `[&]` / `[=]` / `[this]` が1つも無いことを機械で確かめられるよう、
// 切り出し先は必ず名前付きメンバ関数にする。
//
// 【出力は必ず参照で受ける】とくに std::vector<GPULight> を値で受ける signature を
// 作らないこと。コンパイラは黙って通すが、組み立てた配列が捨てられる。
//
// 【frameContext のポインタ型フィールドへアドレスを書くのは BuildFrameContext の末尾だけ】
// 実体は Render() のローカルで、graph.Execute() まで生きている必要がある。
// その関係を1つの関数の中で読み切れるようにしておく
namespace Kurenai
{
    // フレームのジッターと、カメラ由来の行列を確定させる。
    //
    // 【最初に呼ぶこと】m_TAAFrameIndex の前進がここの最初の実行文で、
    // MegaLights のタイル格子ジッターと TAA のサブピクセルジッターの両方が
    // この番号から導かれる。呼ぶ位置が下がると、両者が別のフレーム番号を見る
    void KurenaiEngine3D::DecideFrameJitterAndCamera(
        const KurenaiEngine3D::FrameState& frameState, Rendering::RenderFrameContext& frameContext)
    {
        // --- TAAのサブピクセルジッター ---
        // 投影行列を1ピクセル未満だけずらして、同じ画素が毎フレームわずかに違う位置をサンプルする
        // ようにする。TAAが複数フレームぶんを蓄積することで実質的なスーパーサンプリングになる。
        // TAA無効時はジッターも必ず0にすること(ジッターだけ残ると画面が振動するだけになる)
        ++m_TAAFrameIndex;

        // --- MegaLights候補プールのタイル格子ジッター ---
        // 書き手・Initial/Spatial・Presentへ配る値をここで一度だけ決める。
        // 各パスが個別にフレーム番号から導くと、式の片側だけを直した際に別タイルを静かに読むため
        const bool megaLightsTileJitterEnabled = m_Settings.MegaLights.TileJitterMode != 0;
        DirectX::XMUINT2 megaLightsTileOffset{ 0u, 0u };
        if (m_Settings.MegaLights.TileJitterMode == 1)
        {
            // Halton(2,3)を16段階へ量子化する。RadicalInverseは[0,1)だが、丸め誤差でも
            // 16にならないようタイル幅-1で明示的に押さえる
            megaLightsTileOffset.x = std::min<uint32_t>(
                static_cast<uint32_t>(Rendering::RadicalInverse(m_TAAFrameIndex, 2u) * Passes::kLightTileSize),
                Passes::kLightTileSize - 1u);
            megaLightsTileOffset.y = std::min<uint32_t>(
                static_cast<uint32_t>(Rendering::RadicalInverse(m_TAAFrameIndex, 3u) * Passes::kLightTileSize),
                Passes::kLightTileSize - 1u);
        }
        frameContext.MegaLightsTileOffset = megaLightsTileOffset;
        // 無効時だけ従来のタイル数をそのまま使い、添字・乱数の種・ディスパッチ数を保存する。
        // モード2は対照実験なので、オフセット0でも有効側と同じ+1タイルを通す
        frameContext.MegaLightsEffectiveTilesX =
            megaLightsTileJitterEnabled ? (m_RenderTargets.LightTileCountX + 1u) : m_RenderTargets.LightTileCountX;
        frameContext.MegaLightsEffectiveTilesY =
            megaLightsTileJitterEnabled ? (m_RenderTargets.LightTileCountY + 1u) : m_RenderTargets.LightTileCountY;

        DirectX::XMFLOAT2 jitterOffsetPixels{ 0.0f, 0.0f };
        if (m_Settings.PostProcess.TAAEnabled)
        {
            // Halton列の添字は1から始める。添字0はradical inverseの定義上どの基数でも0となり、
            // オフセットがピクセルの角(-0.5, -0.5)へ偏ってしまう
            const uint32_t haltonIndex = (m_TAAFrameIndex % Rendering::kTAAJitterSampleCount) + 1;
            jitterOffsetPixels.x =
                (Rendering::RadicalInverse(haltonIndex, 2) - 0.5f) * m_Settings.PostProcess.TAAJitterScale;
            jitterOffsetPixels.y =
                (Rendering::RadicalInverse(haltonIndex, 3) - 0.5f) * m_Settings.PostProcess.TAAJitterScale;
        }
        // ピクセル単位のオフセットをNDCとUVの2つの単位へ直す。
        // ピクセル座標は右が+x・下が+yなのに対しNDCは上が+yなので、yだけ符号が反転する
        // (この符号を落とすと縦方向のジッターと速度が逆向きになる)
        const DirectX::XMFLOAT2 jitterNdc{
            2.0f * jitterOffsetPixels.x / static_cast<float>(m_RenderWidth),
            -2.0f * jitterOffsetPixels.y / static_cast<float>(m_RenderHeight),
        };
        // NDC→UVは xy * (0.5, -0.5) + 0.5 なので、ジッターのUV換算はピクセル数/解像度そのものになる
        frameContext.JitterUv = {
            jitterOffsetPixels.x / static_cast<float>(m_RenderWidth),
            jitterOffsetPixels.y / static_cast<float>(m_RenderHeight),
        };

        // ビュー行列と「ジッター済み」射影行列をここで一度だけ確定させ、以降のカメラ由来の行列は
        // すべてこれらから作る。
        //
        // 【なぜ行列の掛け算でジッターを入れられるのか】Camera::GetProjectionMatrixは行ベクトル規約
        // (clip = view * P)で、第3列が(0,0,1,0)すなわち clip.w = viewZ である。
        // XMMatrixTranslationは行ベクトル規約では第3行が(jx, jy, 0, 1)になるので、P * T を展開すると
        // 変化するのは要素[2][0]と[2][1]、つまり clip.xy += jitterNdc * clip.w だけになる。
        // w除算後には ndc.xy += jitterNdc という定数オフセットになり、狙いどおり平行移動として効く。
        //
        // 【なぜ全パスで統一するのか】深度バッファはこのジッター済み行列でラスタライズされる。
        // 深度から位置を復元する側(SSAO/SSIL/SSR/スクリーンスペースシャドウ)がジッター前の行列を
        // 使うと、再構成した位置がサブピクセルぶんずれて自己遮蔽やハローの原因になる。
        // なお射影行列の_33/_43(深度のリニアライズ係数)はジッターでは変化しない
        frameContext.ViewMatrix = frameState.Camera.GetViewMatrix();
        frameContext.JitteredProj =
            frameState.Camera.GetProjectionMatrix() * DirectX::XMMatrixTranslation(jitterNdc.x, jitterNdc.y, 0.0f);
    }
    // メッシュレットLODの段を選ぶ入力を、このフレームぶん一度だけ確定させる
    void KurenaiEngine3D::UpdateMeshletLODFrame(const KurenaiEngine3D::FrameState& frameState)
    {
        //
        // 【全パスへ同じものを配る】シャドウと深度プリパスは同じ増幅シェーダーを使うが、
        // ViewProjは光源やカスケードのものに差し替わっている。各パスのカメラで段を選ぶと
        // 影を落とす形と本体の形が違う段になるため、主カメラの値をここで決めて配る。
        //
        // 【ジッターの影響を受けない値を使う】拡大率_22はジッター(平行移動)では変化しない。
        // 仮に変化する量を使うと、段の境目でTAAのジッター周期に合わせて段が振動する
        {
            DirectX::XMFLOAT4X4 projForLOD;
            DirectX::XMStoreFloat4x4(&projForLOD, frameState.Camera.GetProjectionMatrix());
            m_MeshletLODFrame.CameraPos = frameState.Camera.GetPosition();
            // 射影行列の_22 = 1/tan(fovY/2)。画面の高さ全体が 2*tan(fovY/2) なので、
            // 距離1メートルの1メートルは _22 * 高さ / 2 画素になる
            m_MeshletLODFrame.PixelScale = 0.5f * projForLOD._22 * static_cast<float>(m_RenderHeight);
            m_MeshletLODFrame.Quality = m_Settings.Geometry.MeshletLODEnabled ? m_Settings.Geometry.MeshletLODQuality : 0.0f;
            m_MeshletLODFrame.Forced = m_Settings.Geometry.MeshletLODEnabled ? m_Settings.Geometry.MeshletLODForcedLevel : -1;
            m_MeshletLODFrame.DebugColorByLOD = m_Settings.Geometry.MeshletLODDebugColorEnabled;
        }
    }

    // ドローンショーの機体を評価する。
    //
    // 【ライトリストの組み立てより前で呼ぶこと】機体を光源として送るので、
    // ここが後ろにあると灯が1フレーム遅れる(編隊が動いている間ずっと、光だけが
    // 前フレームの位置から当たり続ける)
    void KurenaiEngine3D::EvaluateDroneShowFrame()
    {
        //
        // 【ライトリストの組み立てより前でなければならない】機体を光源として送るので、
        // ここが後ろにあると灯が1フレーム遅れる(編隊が動いている間ずっと、光だけが
        // 前フレームの位置から当たり続ける)。GPUバッファへの転送は下のグラフ構築直前のまま。
        // m_DroneShowTimeの更新はRenderThreadMainで既に済んでいる
        m_DroneInstances.clear();
        if (m_DroneShowEnabled)
        {
            m_DroneShow.Evaluate(m_DroneShowTime, m_DroneShowCenter, m_DroneShowScale, m_DroneInstances);
            // 【バッファの容量を超える機体は描かない】m_DroneShowResources.BufferはkMaxDrones分を固定確保して
            // いるので、それを超えた分をUpdateBufferへ渡すと書き込みが範囲外になる。
            // .kshowの機体数はエディタ側で上限を掛けているが、外から来たファイルでも
            // 壊れないよう、ここで切り詰める(光源を作るのも切り詰めた後の配列から)
            if (m_DroneInstances.size() > Rendering::kMaxDrones)
            {
                m_DroneInstances.resize(Rendering::kMaxDrones);
            }
        }
    }

    // 機体をGPUへ送る。ライトリストとまったく同じ理由でグラフ構築の前に1回だけ更新する。
    // このバッファは本描画パスと平面反射パスの2箇所から読まれるため、パスの中で更新すると
    // 先に走る側が未更新の内容を読んでしまう
    void KurenaiEngine3D::UploadDroneInstances(RHI::IRHICommandList* commandList)
    {
        // ライトリストとまったく同じ理由でグラフ構築の前に1回だけ更新する。このバッファは
        // 本描画パスと平面反射パスの2箇所から読まれるため、パスの中で更新すると
        // 先に走る側が未更新の内容を読んでしまう。
        // 【m_DroneInstancesを作るのはここではない】ライトリストの組み立てが機体の位置を要るため、
        // Evaluateはそれより前(gpuLightsの直前)へ移してある。ここは転送だけ
        if (m_DroneShowEnabled && !m_DroneInstances.empty())
        {
            commandList->UpdateBuffer(
                m_DroneShowResources.Buffer.get(), m_DroneInstances.data(), m_DroneInstances.size() * sizeof(GPUDrone));
        }
    }
    // t8のライトリストを組み立てる。作者が置いた灯 → 自発光のプロキシ → ドローンの灯 の順に
    // 連結し、最後に容量で丸める。
    //
    // 【この順序は意味を持つ】プロキシは手置きの後ろに置く(容量超過の切り捨てをプロキシ側だけへ
    // 掛けるため)。ドローンの灯はさらにその後ろ(編隊が毎フレーム動くので、反射プローブの
    // 焼き直し判定に入れたくない)。
    //
    // 【実体は Render() のローカル】frameContext.Lights がこれを指し、graph.Execute() の
    // 時点でも生きている必要がある。出力は必ず参照で受けること
    void KurenaiEngine3D::BuildGpuLightList(
        std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition, size_t& bakedLightCount)
    {
        // 有効なライトだけを詰めてt8のライトリストへ渡す。シェーダはLightCount(・ActiveLightCount)の
        // 数までしかループしないため、無効なライトはそもそもGPUへ送らない。DirectLight/Transparentの
        // 両パスがこの1つのリストを共有する(FrameConstants.ActiveLightCountに人数を書き込むため、
        // 各パスのExecute内ではなくFrameConstants確定より前にここで組み立てる必要がある)
        // 【実体はRender()にある】frameContext.Lightsがこれを指し、graph.Execute()の時点でも生きている必要がある
        gpuLights.reserve(m_Lights.size());
        for (const Assets::Light& light : m_Lights)
        {
            if (!light.Enabled)
            {
                continue;
            }
            gpuLights.push_back(Rendering::MakeGPULight(light, m_EffectiveExposureEV100));
        }
        // ここまでが作者の置いたライト。以降のプロキシと切り分けるために数を控える
        const size_t manualLightCount = gpuLights.size();

        AppendEmissiveProxyLights(gpuLights, cameraPosition, manualLightCount, bakedLightCount);
        AppendDroneLights(gpuLights, cameraPosition);
        ClampLightsToCapacity(gpuLights, cameraPosition, bakedLightCount);
    }

    // 自発光メッシュのプロキシ光源を、手置きの灯の後ろへ連結する
    void KurenaiEngine3D::AppendEmissiveProxyLights(
        std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition,
        size_t manualLightCount, size_t& bakedLightCount)
    {
        // --- エミッシブ光源のプロキシを後ろへ連結する ---
        //
        // 【手置きの後ろに置く】容量超過の切り捨ては下でプロキシ側だけに掛ける。
        // 全体をカメラ距離でソートして切ると、**手置きの遠いライトが黙って消える**。
        //
        // 【毎フレーム作り直す】m_Settings.EmissiveLight.Intensity のスライダーとτを即座に反映するため。
        // プロキシ側は倍率も露出も持たない値(RadianceBase)で保持してある
        m_RenderStats.EmissiveLightsUsedCount = 0;
        // 切り捨てが起きたときだけ、採用した集合の指紋を残す(起きなければ0のまま)。
        //
        // 【プローブの署名に要る】採用順はカメラからの照度で決まるので、**カメラを動かすだけで
        // プローブが焼く光源の集合が変わる**。署名が変わらないと反射プローブはOnDemandで
        // 焼き直さず、DDGIは更新を止めたまま、収束済みのプローブだけ古い集合で残る。
        // 切り捨てが起きない限り集合はシーン固定なので、そのときは0で十分
        m_EmissiveLightsSelectionHash = 0;
        if (m_Settings.EmissiveLight.LightsEnabled && !m_EmissiveProxies.empty() && manualLightCount < Rendering::kMaxLights)
        {
            const size_t budget = std::min<size_t>(
                static_cast<size_t>(std::max(0, m_Settings.EmissiveLight.LightsMaxCount)), Rendering::kMaxLights - manualLightCount);

            if (m_EmissiveProxies.size() <= budget)
            {
                for (const Assets::EmissiveProxy& proxy : m_EmissiveProxies)
                {
                    gpuLights.push_back(MakeGPULightFromEmissiveProxy(
                        proxy, m_Settings.EmissiveLight.Intensity, m_Settings.EmissiveLight.LightsCutoffIrradiance,
                        m_EmissiveLightsMaxRange));
                }
            }
            else
            {
                // 【スコアはカメラ位置に届く表示空間の照度】単なるカメラ距離だと、
                // 遠くの明るい看板より近くの暗い豆電球が残る。
                // 同値のときは (インスタンス, メッシュ, かたまり) の辞書順で決める ――
                // 順序が揺れるとライトが出入りしてちらつく
                std::vector<size_t> order(m_EmissiveProxies.size());
                for (size_t i = 0; i < order.size(); ++i)
                {
                    order[i] = i;
                }
                const auto scoreOf = [this, &cameraPosition](size_t index)
                {
                    const Assets::EmissiveProxy& p = m_EmissiveProxies[index];
                    const float dx = p.Position[0] - cameraPosition.x;
                    const float dy = p.Position[1] - cameraPosition.y;
                    const float dz = p.Position[2] - cameraPosition.z;
                    const float distSq = dx * dx + dy * dy + dz * dz;
                    const float peak = std::max({ p.RadianceBase[0], p.RadianceBase[1], p.RadianceBase[2] }) *
                                       m_Settings.EmissiveLight.Intensity * p.Area;
                    return peak / std::max(distSq, p.SourceRadius * p.SourceRadius + 1e-6f);
                };
                std::stable_sort(
                    order.begin(), order.end(),
                    [this, &scoreOf](size_t a, size_t b)
                    {
                        const float sa = scoreOf(a);
                        const float sb = scoreOf(b);
                        if (sa != sb) { return sa > sb; }
                        const Assets::EmissiveProxy& pa = m_EmissiveProxies[a];
                        const Assets::EmissiveProxy& pb = m_EmissiveProxies[b];
                        if (pa.InstanceIndex != pb.InstanceIndex) { return pa.InstanceIndex < pb.InstanceIndex; }
                        if (pa.MeshIndex != pb.MeshIndex) { return pa.MeshIndex < pb.MeshIndex; }
                        return pa.ClusterIndex < pb.ClusterIndex;
                    });
                // FNV-1a(64bit)。採用したプロキシの識別子だけを順に混ぜる。
                // 位置や強さは混ぜない ―― それらはシーン固定で、変わるのは「どれを採ったか」だけ
                uint64_t selectionHash = 1469598103934665603ull;
                const auto mixIndex = [&selectionHash](uint32_t value)
                {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
                    for (size_t b = 0; b < sizeof(value); ++b)
                    {
                        selectionHash ^= bytes[b];
                        selectionHash *= 1099511628211ull;
                    }
                };
                for (size_t i = 0; i < budget; ++i)
                {
                    const Assets::EmissiveProxy& proxy = m_EmissiveProxies[order[i]];
                    mixIndex(proxy.InstanceIndex);
                    mixIndex(proxy.MeshIndex);
                    mixIndex(proxy.ClusterIndex);
                    gpuLights.push_back(MakeGPULightFromEmissiveProxy(
                        proxy, m_Settings.EmissiveLight.Intensity, m_Settings.EmissiveLight.LightsCutoffIrradiance, m_EmissiveLightsMaxRange));
                }
                m_EmissiveLightsSelectionHash = selectionHash;

                // 【切り捨ては発光を捨てている】併合で減らせないか先に疑うこと。
                // EmeraldSquare の実測では、面積の大きい順に上位256個を残しても
                // 総面積の46.7%にしかならない(上位1024個でも84.9%)
                if (!m_EmissiveLightsCapLogged)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "エミッシブ光源が上限(" + std::to_string(budget) + ")を超えたため" +
                            std::to_string(m_EmissiveProxies.size() - budget) +
                            "個を捨てました。捨てたぶんの発光は絵から消えます" +
                            (ShouldSuppressEmissiveForGI()
                                 ? "。**しかもDDGIからは抑止されたまま**です ―― 捨てた面は"
                                   "直接光にも間接光にも入らず、純粋なエネルギー損失になります"
                                 : ""));
                    m_EmissiveLightsCapLogged = true;
                }
            }
            m_RenderStats.EmissiveLightsUsedCount = static_cast<uint32_t>(gpuLights.size() - manualLightCount);

            // 【「効いていない」と「暗すぎて見えない」を切り分けられるようにする】
            // 絵の差だけを見ていると、経路が走っていないのか寄与が小さいだけなのかが分からない。
            // 実際に送った灯数と、代表1灯の強さ・Range・κ を1回だけ出す
            if (!m_EmissiveLightsValuesLogged && m_RenderStats.EmissiveLightsUsedCount > 0)
            {
                const GPULight& sample = gpuLights[manualLightCount];
                // 【RGBの最大を出す。Rだけを出さない】Rangeはmax(R,G,B)から解いているので、
                // 色付きの自発光(赤い看板など)でRだけを見るとログからRangeを検算できない
                const float samplePeak =
                    std::max({ sample.ColorRange.x, sample.ColorRange.y, sample.ColorRange.z });
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "エミッシブ光源を送信: " + std::to_string(m_RenderStats.EmissiveLightsUsedCount) + "灯(手置き " +
                        std::to_string(manualLightCount) + "灯) / 先頭の灯 強さ(RGBの最大) " +
                        std::to_string(samplePeak) + " Range " + std::to_string(sample.ColorRange.w) +
                        "m 半径 " + std::to_string(sample.Params.z) + "m κ " + std::to_string(sample.Params.w));
                m_EmissiveLightsValuesLogged = true;
            }
        }

        // ここまでが「焼き込みに入れてよい灯」。プローブと DDGI はこの数までしか舐めない。
        //
        // 【ドローンの灯をこの後ろへ置く理由】編隊は毎フレーム動く。反射プローブは
        // OnDemand で焼くので「焼いた瞬間の編隊」が環境キューブに固定で残り、DDGI は
        // ヒステリシスで編隊を追いかけ続けて収束しない。どちらも動く光を入れる前提の
        // 構造になっていないため、この2つからは外す
        bakedLightCount = gpuLights.size();
    }

    // ドローンショーの機体を光源として、プロキシのさらに後ろへ連結する
    void KurenaiEngine3D::AppendDroneLights(
        std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition)
    {
        // --- ドローンショーの機体を光源として後ろへ連結する ---
        //
        // 【エミッシブプロキシとは単位系が違う】プロキシは露出を掛けない(自発光がG-Bufferで
        // 露出を通らないため、I*exposure の中で相殺する)。一方ドローンのスプライトは
        // Passes::DroneShowConstants.Params0.x = Brightness * effectiveExposure として露出を通っており、
        // 単位系としては手置きライト(カンデラ)の側にいる。**ここは掛ける側が正しい**。
        // 向こうの慣習を写すと桁で外す(docs/ImplementationDetail.md 62.4の表)
        m_DroneShowLightUsedCount = 0;
        if (m_DroneShowEnabled && m_DroneShowCastLight && !m_DroneInstances.empty())
        {
            // 手置き+プロキシを押し出さないよう、残り容量だけを使う
            const size_t budget =
                (gpuLights.size() < Rendering::kMaxLights) ? (Rendering::kMaxLights - gpuLights.size()) : 0u;
            const uint32_t sampleCount = static_cast<uint32_t>(
                std::min<size_t>(budget, static_cast<size_t>(std::max(0, m_DroneShowLightSampleCount))));

            m_DroneShow.BuildLightSamples(m_DroneInstances, sampleCount, m_DroneLightSamples);

            // 【bakedLightCountを添字に使い回さない】あちらは「焼き込みに入れてよい灯の数」で、
            // 容量超過の切り詰めが走ると意味が変わる(下の再代入を参照)。
            // ここで欲しいのは「ドローンの灯の先頭の位置」という別の量なので、別に持つ
            const size_t firstDroneLightIndex = gpuLights.size();

            const float exposure = ComputeExposure(m_EffectiveExposureEV100);
            const float cutoffLux = std::max(m_DroneShowLightCutoffLux, 1e-9f);
            // 演出用の倍率。1.0がスプライトから導いた物理的な値。
            // 【Rangeにも効かせる】強くした灯を同じRangeで打ち切ると、届くはずの距離で
            // 切れて「明るくしたのに広がらない」になる。下でpeakから解き直すので自動的に効く
            const float lightScale = std::max(m_DroneShowCastLightScale, 0.0f);
            for (const DroneLightSample& rawSample : m_DroneLightSamples)
            {
                DroneLightSample sample = rawSample;
                sample.Intensity = { rawSample.Intensity.x * lightScale, rawSample.Intensity.y * lightScale,
                                     rawSample.Intensity.z * lightScale };

                GPULight light{};
                light.PositionType = { sample.Position.x, sample.Position.y, sample.Position.z,
                                       static_cast<float>(Assets::LightType::Point) };

                // 【Rangeは打ち切り照度から逆算する】MakeGPULightFromEmissiveProxy と同じ考え方。
                // 点光源(型1)の減衰に半径の項は無いので、あちらの -R² は付けない。
                // RGBの最大から解くのは、色付きの灯で1chだけ見るとRangeが検算できないため
                const float peak = std::max({ sample.Intensity.x, sample.Intensity.y, sample.Intensity.z });
                float range = (peak > 0.0f) ? std::sqrt(peak / cutoffLux) : 0.0f;
                // 下限は光源自身の広がりを覆う分。上限はエミッシブ光源と同じシーンAABB対角で、
                // タイルライトカリングが全タイルにヒットするのを止める安全弁
                range = std::max(range, 2.0f * sample.SourceRadius);
                if (m_EmissiveLightsMaxRange > 0.0f)
                {
                    range = std::min(range, m_EmissiveLightsMaxRange);
                }

                light.ColorRange = { sample.Intensity.x * exposure, sample.Intensity.y * exposure,
                                     sample.Intensity.z * exposure, range };
                light.DirectionAngle = { 0.0f, -1.0f, 0.0f, 0.0f };
                // 【スクリーンスペースシャドウは立てない】画素あたりのシャドウレイ本数には
                // 上限(Defaults::ScreenSpaceShadowMaxLightsPerPixel)があり、数十灯を入れると
                // 手置きライトの接触影を食い潰す(docs/ImplementationDetail.md 62.7と同じ理由)。
                // Params.z は光源の半径で、減衰には効かずMegaLightsの半影の広がりだけを決める
                light.Params = { 0.0f, static_cast<float>(kLightShadowRaytraced), sample.SourceRadius, 0.0f };
                gpuLights.push_back(light);
            }
            m_DroneShowLightUsedCount = static_cast<uint32_t>(m_DroneLightSamples.size());

            // 【「効いていない」と「暗すぎて見えない」を切り分ける】エミッシブ光源と同じ理由で、
            // 実際に送った灯数と代表1灯の実効値を1回だけ出す
            if (!m_DroneShowLightValuesLogged && m_DroneShowLightUsedCount > 0)
            {
                float totalCd = 0.0f;
                for (const DroneLightSample& sample : m_DroneLightSamples)
                {
                    totalCd += std::max({ sample.Intensity.x, sample.Intensity.y, sample.Intensity.z });
                }
                const GPULight& first = gpuLights[firstDroneLightIndex];
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "ドローンを光源として送信: " + std::to_string(m_DroneShowLightUsedCount) + "灯(機体 " +
                        std::to_string(m_DroneInstances.size()) + "機) / 倍率 " +
                        std::to_string(m_DroneShowCastLightScale) + " / 総光度(RGBの最大の和。倍率込み) " +
                        std::to_string(totalCd * m_DroneShowCastLightScale) + "cd / 先頭の灯 露出後の強さ " +
                        std::to_string(std::max({ first.ColorRange.x, first.ColorRange.y, first.ColorRange.z })) +
                        " Range " + std::to_string(first.ColorRange.w) + "m 半径 " +
                        std::to_string(first.Params.z) + "m");
                m_DroneShowLightValuesLogged = true;
            }

            // 【条件をbudgetで見る】m_DroneShowLightUsedCount == 0 で判定すると、
            // 灯数の設定を0にしただけのときにも「容量を使い切っています」と誤報する
            if (budget == 0u && !m_DroneShowLightTileOverflowLogged)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "ドローンを光源として送れませんでした(ライトの容量" + std::to_string(Rendering::kMaxLights) +
                        "灯を手置きライトとエミッシブ光源で使い切っています)");
                m_DroneShowLightTileOverflowLogged = true;
            }
        }
        else
        {
            m_DroneLightSamples.clear();
        }
    }

    // 容量を超えた場合の安全弁。
    //
    // 【ここのラムダは外へ出ない】std::sort の比較子で、呼び出しの中で使い切られる。
    // このファイルが禁じているのは Render() のローカルを捕捉したまま graph.Execute() まで
    // 生き延びる捕捉一括(`[&]` / `[=]` / `[this]`)であって、名前を挙げた同期的な捕捉ではない
    void KurenaiEngine3D::ClampLightsToCapacity(
        std::vector<GPULight>& gpuLights, const DirectX::XMFLOAT3& cameraPosition, size_t& bakedLightCount)
    {
        // 容量(Rendering::kMaxLights)を超える場合は、カメラに近い順に先頭kMaxLights灯のみ採用する。
        // 全画面ディファードなのでフラスタムカリングは効果が薄く、これは容量超過時の
        // 安全弁としてのみ機能する。
        //
        // 【ここへ来るのは手置きライトだけで超えたとき】プロキシは上で別枠に収めてある
        if (gpuLights.size() > Rendering::kMaxLights)
        {
            std::sort(
                gpuLights.begin(), gpuLights.end(),
                [&cameraPosition](const GPULight& a, const GPULight& b)
                {
                    const float dxA = a.PositionType.x - cameraPosition.x;
                    const float dyA = a.PositionType.y - cameraPosition.y;
                    const float dzA = a.PositionType.z - cameraPosition.z;
                    const float dxB = b.PositionType.x - cameraPosition.x;
                    const float dyB = b.PositionType.y - cameraPosition.y;
                    const float dzB = b.PositionType.z - cameraPosition.z;
                    return (dxA * dxA + dyA * dyA + dzA * dzA) < (dxB * dxB + dyB * dyB + dzB * dzB);
                });
            gpuLights.resize(Rendering::kMaxLights);

            // 【bakedLightCountの前提が崩れる】上のソートは配列全体を並べ替えるので、
            // 「先頭bakedLightCount灯が焼き込みに入れてよい灯」という対応が失われる。
            // bakedLightCount自身もkMaxLightsを超えうるので、そのまま
            // ActiveLightCountとして渡すと配列長を超えた読み出しになる。
            // ここまで来るのは手置きライトだけで1024灯を超えた場合で、そのときは
            // どれが焼き込み対象かを区別できないため、**全灯を焼き込みへ入れる**側へ倒す
            // (プローブに入り過ぎるほうが、範囲外を読むより安全)
            bakedLightCount = gpuLights.size();

            if (!m_LightOverflowLogged)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "ライト数が上限(" + std::to_string(Rendering::kMaxLights) + ")を超えたため、カメラに近い順に描画します"
                    "(この場合ドローンの灯と焼き込み対象の切り分けは失われます)");
                m_LightOverflowLogged = true;
            }
        }
    }
    // このフレームの空の状態を確定させる。
    //
    // 【FrameConstants より前に呼ぶこと】constants.SkyParams.y が UsingProceduralSky を要る
    void KurenaiEngine3D::ResolveSkyFrameState(
        const SunLighting& sunLighting, Rendering::RenderFrameContext& frameContext)
    {
        // このフレームで空として使うキューブマップ。手続き空(SkyGenerate)か.ksceneのDDSかが
        // ここで確定する。**RenderGraphのReads宣言と実際のバインドの両方でこのローカルを使うこと**
        // (ActiveSkyTexture()を都度呼ぶと両者が食い違って依存解決が壊れる)。
        // 【ここで確定させる理由】この下のFrameConstants(constants.SkyParams.y)が
        // usingProceduralSkyを必要とするため、FrameConstantsを埋めるより前に確定させる
        frameContext.SkyTexture = ActiveSkyTexture();
        frameContext.UsingProceduralSky = (frameContext.SkyTexture == m_SkyResources.ProceduralSkyTexture.get());

        // 太陽が閾値以上動いていたら手続き空を焼き直す。毎フレーム焼くと
        // 空生成6回+プリフィルタ36回のディスパッチが常時走って無駄になる。
        // 空はプリ露出済みの値で焼かれるため、実効プリ露出が動いたときも焼き直す必要がある
        // (焼き直さないと空だけ古い露出のまま取り残される)
        if (frameContext.UsingProceduralSky && !m_SkyBakeDirty)
        {
            const DirectX::XMVECTOR current = DirectX::XMLoadFloat3(&sunLighting.SunPosition);
            const DirectX::XMVECTOR baked = DirectX::XMLoadFloat3(&m_LastBakedSunPosition);
            const float cosAngle = DirectX::XMVectorGetX(DirectX::XMVector3Dot(current, baked));
            const bool sunMoved =
                cosAngle < std::cos(DirectX::XMConvertToRadians(m_Settings.Sky.BakeAngleThresholdDegrees));
            // 露出が0.05段(約3.5%)以上動いたら焼き直す。時刻変化に伴う露出の追従でも
            // 動くため、太陽の角度閾値とあわせて実質的に連続した更新になる
            const bool exposureMoved =
                std::abs(m_EffectiveExposureEV100 - m_LastBakedExposureEV100) > 0.05f;
            // タービディティが動いたら焼き直す。PreethamのxyYモデルの形自体が変わるため、
            // exposureMovedと同じ形の判定をここへ追加する
            const bool turbidityMoved = std::abs(m_Settings.Sky.Turbidity - m_LastBakedTurbidity) > 0.01f;
            // 空の彩度(アート指定)もPreethamの色度を動かすため、タービディティと同じ扱いで焼き直す
            const bool saturationMoved = std::abs(m_Settings.Sky.Saturation - m_LastBakedSkySaturation) > 0.005f;
            // 雲のパラメータが動いたら焼き直す(P18)。
            //
            // 【なぜ要るか】ここまでの4つは晴天の空の形を決める値だけで、雲は「晴天の空を
            // 変えない」ため入っていなかった。P18でSkyIntegrateが雲込みの空の照度を積むように
            // なったので、被覆率を動かしても焼き直しが走らないと**古い被覆率で積んだ
            // CloudSkyLightが残り続ける**。実際これで被覆率0でも比が1にならず、雲を持たない
            // シーンの遠景が動いた(切り分け: SkyIntegrateへ1を直書きした絵と、消費側で1へ
            // 潰した絵は画素まで一致した=経路は正しく、値だけが古かった)。
            //
            // 【風のスクロールを入れない】スクロール量は毎フレーム動くので、入れると毎フレーム
            // 焼き直しになる。求めているのは半球平均なので、雲の場が平行移動しても値はほとんど
            // 変わらない。同じ理由でカメラ位置も入れない。
            //
            // 【IBLの雲減光もこれで直る】m_ActiveCloudTransmittance(キューブへ焼く平均透過率)も
            // このブロックの中でしか更新されないため、被覆率を動かしても環境光が追従しない
            // という同じ形の取りこぼしがあった
            const CloudBakeSignature cloudSignature = MakeCloudBakeSignature();
            const bool cloudChanged = !m_HasBakedCloudSignature
                                      || cloudSignature != m_LastBakedCloudSignature;
            if (sunMoved || exposureMoved || turbidityMoved || saturationMoved || cloudChanged)
            {
                m_SkyBakeDirty = true;
            }
        }

        // このフレームで手続き空を焼くかどうか。下のSkyGenerateパス登録とキャッシュ更新の
        // 両方をこのフラグで判定する
        frameContext.BakeSkyThisFrame = frameContext.UsingProceduralSky && m_SkyBakeDirty;

        // このフレームでSkyIntegrateパス(m_SkyResources.ParametersBufferへ書く)を実行するかどうか。
        // 通常はbakeSkyThisFrameと同じタイミングだが、m_SkyResources.ParametersBufferが一度も書かれていない
        // 場合はusingProceduralSkyがfalse(.ksceneのDDSスカイボックス使用時)でも1回だけ実行する。
        //
        // 【なぜCPU側からのゼロ初期化ではなくこの形にしたのか】DX12のStructuredRWバッファは
        // UAV/SRVでのGPUアクセス専用にDEFAULTヒープへ作成されており、CPUから書き込むための
        // マップ済みポインタ・ステージングリングを一切持たない(m_SkyResources.ParametersBuffer作成箇所の
        // コメント参照)。そのためUpdateBufferでのゼロ埋めはDX12でクラッシュする。GPU側のパスを
        // 1回だけ走らせれば、DX11/DX12のどちらでも安全に(積分結果自体は使われないが)未初期化状態を
        // 解消できる。太陽方向・目標照度はusingProceduralSkyに関わらず既に計算済みのsunLightingを
        // そのまま使えるため、余分な分岐を増やさずに済む
        frameContext.SkyIntegrateThisFrame = frameContext.BakeSkyThisFrame || !m_EnvironmentPasses->IsSkyParametersBufferInitialized();

        // --- 空パラメータ(tintと天頂輝度)の確定はGPU側(SkyIntegrate.hlsl)で行う ---
        // 【なぜベイクと同じタイミングか】背景の解析評価(DeferredLighting.hlsl)は、下のFrameConstants
        // (SkySunDirection)とm_SkyResources.ParametersBuffer(SkyIntegrate.hlslの出力)を組み合わせて使う。
        // ベイクと同じタイミングでSkyIntegrateパスを実行することで、背景とキューブマップ
        // (IBL・反射)が常に同一の空パラメータを見る。毎フレーム走らせると、太陽の角度閾値で
        // ベイクを間引いている間だけ背景とIBLの空がずれてしまう。実際のディスパッチとcbuffer更新は
        // 下のSkyIntegrateパス登録側で行うため、ここではフラグ更新のみ済ませる
        if (frameContext.BakeSkyThisFrame)
        {
            // 雲(判断B)による平均透過率をベイクと同じタイミングで確定させ、メンバへキャッシュする。
            // **この値はm_SkyResources.ParametersBuffer側の天頂輝度には掛けない**——キューブへ焼く
            // Passes::SkyBakeConstants::CloudTransmittance(下のSkyGenerateパス参照)にだけ掛ける。
            // SkyParametersBufferの天頂輝度を減光すると、雲の隙間から見える青空まで暗くなり、
            // Sky.hlsli側のSkyColorがそこへさらに雲を重ねることで二重に暗くなってしまう
            m_ActiveCloudTransmittance = Rendering::ComputeCloudAverageTransmittance(
                m_Settings.Cloud.Enabled, m_Settings.Cloud.Coverage, m_Settings.Cloud.CirrusEnabled, m_Settings.Cloud.CirrusCoverage);

            // P18: この焼き直しがどの雲パラメータで行われたかを覚えておく。
            // 上の焼き直し判定(cloudChanged)がこれと比べる
            m_LastBakedCloudSignature = MakeCloudBakeSignature();
            m_HasBakedCloudSignature = true;

            // 空が変わったのでプリフィルタ済み鏡面も焼き直す必要がある。
            // 焼き直し要否のフラグ更新はここ(キャッシュを書いた場所)に一本化し、
            // 下のSkyGenerateパス登録側では行わない(二重更新・更新漏れを避けるため)
            m_SkyBakeDirty = false;
            m_LastBakedSunPosition = sunLighting.SunPosition;
            m_LastBakedExposureEV100 = m_EffectiveExposureEV100;
            m_LastBakedTurbidity = m_Settings.Sky.Turbidity;
            m_LastBakedSkySaturation = m_Settings.Sky.Saturation;
            m_EnvironmentPasses->GetIBLBaked() = false;
            m_EnvironmentPasses->GetIBLIrradianceBaked() = false;
        }
    }

    // このフレームに「何を描くか」の述語をまとめて確定させる。
    //
    // 【FrameConstants より前に呼ぶこと】定数バッファの更新はパス登録より前に一度だけ行うので、
    // パスを積むかどうかの判断もそこより前で確定していなければならない
    void KurenaiEngine3D::ResolveFrameDrawDecisions(Rendering::RenderFrameContext& frameContext)
    {
        // 平面反射: 水面インスタンスを探し、その高さ(ワールドY)を水面の平面とする。
        // 水面メッシュはローカルY=0の水平な板(Tools/generate_water_plane.py参照)なので、
        // ワールド変換の平行移動Y(instance.World._24。転置済みのため列に入っている。
        // Transparentパスの距離ソートと同じ規約)がそのまま水面の高さになる。
        // 複数の水面インスタンスが異なる高さで見つかった場合は最初のものだけを使い、警告を1度だけ出す
        // (「水面は単一の水平な平面である」という前提を明示する)
        
        
        for (const auto& instance : m_Scene.Instances)
        {
            if (!instance.IsWater)
            {
                continue;
            }
            const float instanceWaterY = instance.World._24;
            if (!frameContext.HasWaterInstance)
            {
                frameContext.HasWaterInstance = true;
                frameContext.WaterPlaneY = instanceWaterY;
            }
            else if (std::abs(instanceWaterY - frameContext.WaterPlaneY) > 0.01f && !m_PlanarReflectionMultipleWaterLogged)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "複数の水面インスタンスが異なる高さ(Y=" + std::to_string(frameContext.WaterPlaneY) + "とY=" +
                        std::to_string(instanceWaterY) +
                        ")で見つかりました。平面反射は最初の水面のみを使用します"
                        "(水面は単一の水平な平面である前提のため)");
                m_PlanarReflectionMultipleWaterLogged = true;
            }
        }
        // このフレームで平面反射パスを実行するか。
        // 【反射の手法がSSRのときだけ実行する】このパスの出力(m_RenderTargets.PlanarReflectionColor)を読むのは
        // SSR.hlslだけである。手法がRaytracedやOffのときに走らせても、不透明メッシュ全体を
        // もう1回フォワードで描いた結果を誰も読まないまま捨てることになる
        // (DXR対応環境ではDefaultReflectionModeがRaytracedを返すため、この条件が無いと
        //  DX12では常に丸ごと無駄になる。実測でもDX12起動時に水面へ映っていたのはRT反射の結果で、
        //  平面反射パスの出力ではなかった)
        frameContext.PlanarReflectionPassRuns =
            m_Settings.Reflection.PlanarEnabled && frameContext.HasWaterInstance && m_Settings.Reflection.Mode == ReflectionMode::ScreenSpace;

        // 大気遠近パスを実行するか。UIで無効化されているか、密度が0以下(効果が無い)なら
        // パス自体を登録しない(GetActiveReflectionOutput()の結果がそのままTAA/Tonemapへ渡る)。
        // 手続き空が無効なシーンかどうかの判断(FogParams0.w)はパスの実行有無とは別に、
        // 下のconstants.FogParams0組み立て時にusingProceduralSkyを見て決める
        // (SSRパスのwaterAnalyticSkyFlagと同じ、パスの実行可否とシェーダー内の有効フラグを分ける設計)
        frameContext.FogPassRuns = m_Settings.Fog.Enabled && m_Settings.Fog.Density > 0.0f;

        // メッシュレット(増幅シェーダー + メッシュシェーダー)経路でG-Bufferを描くか。
        // メッシュシェーダー非対応のデバイスではPSOが作られないためnullptrになる。
        //
        // 【他の「PassRuns」と並べてここに置く理由】この値はG-Bufferパスの登録時だけでなく、
        // その手前で書き上げるFrameConstantsも見る(オクルージョンカリングの有効フラグ)。
        // 定数バッファの更新はパス登録より前に一度だけ行うため、判断もそこより前で確定させる
        frameContext.MeshletPathActive =
            m_Settings.Geometry.MeshletRenderingEnabled && m_GeometryPasses->HasMeshletPipelineState();

        // 増幅シェーダーのHi-Zオクルージョンカリング(Stage 5-2)をこのフレームで行うか。
        // 判定を書いてあるのは増幅シェーダーだけなので、メッシュレット経路に乗らないフレームでは
        // 1つも間引けず、Hi-Zを構築する意味も無い(下のHi-Zパスの登録条件がこれを見る)
        frameContext.OcclusionCullingActive = m_Settings.Geometry.OcclusionCullingEnabled && frameContext.MeshletPathActive;

        // メッシュレットカリングの統計をこのフレームで数えるか。
        // 増幅シェーダーが走らなければ数える相手がいない
        frameContext.MeshletCullStatsActive =
            m_Settings.Geometry.MeshletCullStatsEnabled && frameContext.MeshletPathActive
            && m_GeometryPasses->HasMeshletCullStatsBuffer();
    }

    // FrameConstants と LightingConstants を埋める。
    //
    // 【引数が多いのは分割の結果】13引数の BuildFrameContext を割った先なので当然で、
    // 無理に減らすために一時 struct を増やさない
    void KurenaiEngine3D::FillFrameConstants(
        const KurenaiEngine3D::FrameState& frameState, RHI::IRHICommandList* commandList,
        const SunLighting& sunLighting, float effectiveExposure, float manualExposureScale,
        float keyReferenceEV100, const DirectX::XMFLOAT3& cameraPosition,
        const float (&cascadeSplits)[kCascadeCount],
        const DirectX::XMMATRIX (&cascadeViewProj)[kCascadeCount],
        Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
        ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants,
        size_t& bakedLightCount)
    {
        // 【実体はRender()にある】frameContext.Constantsがこれを指す。元と同じく未初期化のまま
        // 受け取り、以降の代入で全フィールドを埋める
        frameContext.ViewProj = frameContext.ViewMatrix * frameContext.JitteredProj;
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(frameContext.ViewProj));

        // 平面反射用の鏡映カメラ。水面平面 y=frameContext.WaterPlaneY に対する反射行列を、通常のView×Projへ
        // 左から掛ける(PlanarReflection.hlsl冒頭参照)。XMMatrixReflectが受け取る平面の規約は
        // 「点PがAx+By+Cz+D=0を満たす」形(ドキュメント準拠)で、これは
        // FrameConstants.PlanarReflectionPlaneのSV_ClipDistance計算(dot(worldPos, xyz) + w)と
        // 完全に同じ規約なので、同じベクトル(0,1,0,-frameContext.WaterPlaneY)がどちらにもそのまま使える
        // (水面より上のworldPosでdot結果が正になることも、この式から導ける)。
        // 水面が無いシーンでもwaterPlaneY=0で計算はできるが、パスを登録しないため使われない
        frameContext.ReflectMatrix =
            DirectX::XMMatrixReflect(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, -frameContext.WaterPlaneY));
        // メインカメラと同じジッター済みProjを使う(PlanarReflection.hlsl冒頭参照。ジッターが
        // 異なると反射がメインの画面UVとサブピクセル単位でずれてしまう)
        frameContext.ReflectedViewProj = frameContext.ReflectMatrix * frameContext.ViewMatrix * frameContext.JitteredProj;
        DirectX::XMVECTOR determinant;
        frameContext.InvViewProj = DirectX::XMMatrixInverse(&determinant, frameContext.ViewProj);
        DirectX::XMStoreFloat4x4(&constants.InvViewProj, DirectX::XMMatrixTranspose(frameContext.InvViewProj));
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            DirectX::XMStoreFloat4x4(&constants.CascadeViewProj[cascade], DirectX::XMMatrixTranspose(cascadeViewProj[cascade]));
        }
        // 【DDGIのクリップマップの追従中心をここで固定する】このあと組み立てるFrameConstantsの
        // 各LODの原点も、後段のプローブのキャプチャ位置も、すべてこの値を基準に決まる。
        // 1フレームの途中で動かすと「シェーダーが見ている格子」と「実際に焼いた位置」が
        // 食い違い、間接光が別の場所のものになる
        m_DDGIGrid.SetFollowCenter(DirectX::XMFLOAT3{ cameraPosition.x, cameraPosition.y, cameraPosition.z });

        constants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f };
        constants.LightDirection = { sunLighting.Direction.x, sunLighting.Direction.y, sunLighting.Direction.z, 0.0f };
        // 太陽を無効にする場合は色をゼロにするだけでよい(シェーダー側は太陽の寄与に
        // LightColor.rgbを乗算するため、これで完全に消える)。TimeOfDayを夜にする方法と違い
        // 昼度(AmbientColor.a)は下がらないので、環境光だけで照らす状態を作れる
        // sunLighting.Color は絶対的な測光量[lx]なので、ここで実効プリ露出を掛けて表示レンジへ移す
        constants.LightColor = m_Settings.Sky.SunEnabled
            ? DirectX::XMFLOAT4{
                  sunLighting.Color.x * effectiveExposure,
                  sunLighting.Color.y * effectiveExposure,
                  sunLighting.Color.z * effectiveExposure,
                  0.0f }
            : DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f };
        DirectX::XMStoreFloat4x4(&constants.View, DirectX::XMMatrixTranspose(frameContext.ViewMatrix));
        // ジッター済みの射影行列を渡す。SSAO/SSILはこの行列でView空間の点を画面へ投影して
        // 深度バッファと突き合わせるため、深度を描いたときと同じ行列でなければサブピクセルぶんずれる
        DirectX::XMStoreFloat4x4(&constants.Proj, DirectX::XMMatrixTranspose(frameContext.JitteredProj));
        // rgb(環境光の色)にm_Settings.IBL.AmbientScaleを乗算する。Enable IBL無効時のフォールバックアンビエント
        // (DeferredLighting.hlsl)の強度調整用で、alpha(dayFactor、IBLの夜間減光・背景スカイの
        // 昼夜ブレンドに使う)には掛けない
        constants.AmbientColor =
        {
            sunLighting.Ambient.x * m_Settings.IBL.AmbientScale * effectiveExposure,
            sunLighting.Ambient.y * m_Settings.IBL.AmbientScale * effectiveExposure,
            sunLighting.Ambient.z * m_Settings.IBL.AmbientScale * effectiveExposure,
            sunLighting.Ambient.w,
        };
        constants.CascadeSplits = { cascadeSplits[0], cascadeSplits[1], cascadeSplits[2], cascadeSplits[3] };
        const float iblIntensity = m_Settings.IBL.Enabled ? m_Settings.IBL.Intensity : 0.0f;
        const float specularEnergyCompensation = static_cast<float>(m_Settings.Reflection.SpecularCompensation);
        constants.ShadowParams = {
            m_Settings.Shadow.LightSize,
            static_cast<float>(Passes::kIBLPrefilterMipLevels - 1),
            iblIntensity,
            specularEnergyCompensation,
        };
        constants.ActiveLightCount = { static_cast<float>(gpuLights.size()), 0.0f, 0.0f, 0.0f };
        constants.IBLParams = {
            m_Settings.IBL.UseDedicatedIrradiance ? 1.0f : 0.0f,
            m_Settings.IBL.AmbientDiffuseScale,
            m_Settings.IBL.AmbientSpecularScale,
            0.0f,
        };
        constants.OcclusionParams = {
            m_Settings.AmbientOcclusion.BentNormalAOSource ? 1.0f : 0.0f,
            static_cast<float>(m_Settings.AmbientOcclusion.SpecularOcclusion),
            m_Settings.AmbientOcclusion.MultiBounceAOEnabled ? 1.0f : 0.0f,
            0.0f };

        // 空の解析評価用。DeferredLighting.hlslが背景画素でSky.hlsliのSkyColorを画面解像度で
        // 評価するために使う。ティントと天頂輝度はm_SkyResources.ParametersBuffer(直近の手続き空ベイクで
        // SkyIntegrate.hlslが書いた値。上のbakeSkyThisFrameブロック参照)にあり、DeferredLighting.hlsl/
        // SSR.hlslがStructuredBufferとして直接読むため、ここでFrameConstantsへは詰めない。
        // SunDirectionはここで毎フレーム最新のsunLightingから渡す
        // (太陽は角度閾値以下でも連続的に動くため。天頂輝度・色味と違い積分を伴わず、
        // 毎フレーム渡してもコストが無い)。
        // 正規化はSkyGenerate.hlsl側の慣習(呼び出し側=シェーダのSkyParameters組み立て時に
        // normalizeする)に合わせ、C++側では正規化しない(DeferredLighting.hlsl側で行う)
        constants.SkySunDirection = {
            sunLighting.SunPosition.x, sunLighting.SunPosition.y, sunLighting.SunPosition.z, 0.0f
        };
        // 太陽照度と空照度の比。Sky.hlsliのEvaluateCloudLayerが雲の明るさの基準を
        // 「空の天頂輝度」から「太陽の照度」へ切り替えるために使う(雲を照らしているのは
        // 空ではなく太陽であるため。詳細はSky.hlsli側のkCumulusSingleScatterScale等のコメント参照)。
        // SkyIlluminanceLuxが0近傍(理論上は起こらないが)のときのゼロ除算を避けてある
        const float sunToSkyIlluminanceRatio =
            (sunLighting.SkyIlluminanceLux > 1e-6f)
                ? (sunLighting.KeyIlluminanceLux / sunLighting.SkyIlluminanceLux)
                : 0.0f;
        constants.SkyParams = {
            // x=未使用(天頂輝度はSkyParametersBufferにある)
            0.0f,
            // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、この設定に関わらず
            // 常にキューブマップを使う。DDSは任意の絵でPerezモデルとは無関係なため、
            // 解析評価してはいけない
            (m_Settings.Sky.AnalyticBackground && frameContext.UsingProceduralSky) ? 1.0f : 0.0f,
            // z=太陽照度/空照度比(SunToSkyIlluminanceRatio、雲の明るさの基準に使う)
            sunToSkyIlluminanceRatio,
            0.0f,
        };

        // === 実効プリ露出が大きく動いたら、更新モードに関わらずプローブを焼き直す(19.14節) ===
        // 下のProbeParams2.wは「焼いた時点の露出→現在の露出」の換算倍率で、これだけでも
        // プローブの値の解釈は常に正しくなる。ただし換算はあくまで**焼いた時点の環境**を
        // 正しい明るさで見せるだけなので、昼に焼いたプローブを夜の場面へ持ち込めば
        // 「夜の部屋に昼の環境が正しい明るさで映り込む」ことになり、換算前より派手に破綻する
        // (実測: ProbeTestを夜にしたときの平均輝度が213.6→253.9、白飽和78%)。
        //
        // 実効プリ露出が大きく動くのは時刻が大きく動いたときなので、そのときは環境そのものが
        // 古くなっている。Bakedモードが凍結すると宣言しているのはライトやマテリアルの編集に
        // 対してであって、場面全体の明るさが2倍以上変わってもなお昼の映り込みを保持することでは
        // ない。手続き空が同じ理由で焼き直しているのと揃える(閾値は空の0.05段よりずっと粗く
        // 取ってある。フルベイクはプローブ数×6面の描画になるため)。
        // Realtimeは毎フレーム焼き直しているので対象外
        if (m_Settings.ReflectionProbe.UpdateMode != ProbeUpdateMode::Realtime && m_ReflectionProbePasses->GetProbeBaked() &&
            !m_GIResources.ReflectionProbes.empty() &&
            std::abs(m_EffectiveExposureEV100 - m_ReflectionProbePasses->GetProbeBakedExposureEV100()) > kProbeRebakeExposureEV)
        {
            m_ReflectionProbePasses->GetProbeBakeRequested() = true;
            // このフレームの後半で今の露出で焼かれるため、換算倍率もここで合わせておく。
            // ここで合わせないと、焼き直したフレームだけ1フレーム古い倍率が掛かって明滅する
            m_ReflectionProbePasses->GetProbeBakedExposureEV100() = m_EffectiveExposureEV100;
        }

        // 反射プローブの影響範囲をt13のStructuredBufferへ渡す。まだ一度も焼けていない場合
        // (まだ焼けていない)や機能を無効にしている場合はプローブ数を0にして、シェーダー側の
        // 選択ループ自体を回さない=中身が未定義のキューブマップを引かせないようにする
        std::vector<GPUReflectionProbe> gpuProbes;
        if (m_Settings.ReflectionProbe.Enabled && m_ReflectionProbePasses->GetProbeBaked())
        {
            gpuProbes.reserve(m_GIResources.ReflectionProbes.size());
            for (const Assets::ReflectionProbe& probe : m_GIResources.ReflectionProbes)
            {
                // Yawはシェーダー側で毎ピクセル三角関数を回さずに済むよう、ここでsin/cosへ展開しておく
                const float yawRadians = DirectX::XMConvertToRadians(probe.YawDegrees);
                const bool isBox = probe.Shape == Assets::ReflectionProbeShape::Box;

                GPUReflectionProbe gpuProbe{};
                gpuProbe.PositionRadius = { probe.Position[0], probe.Position[1], probe.Position[2], probe.Radius };
                gpuProbe.BoxExtents = {
                    probe.BoxExtents[0], probe.BoxExtents[1], probe.BoxExtents[2], probe.BlendDistance
                };
                gpuProbe.ShapeParams = {
                    isBox ? 1.0f : 0.0f, std::sin(yawRadians), std::cos(yawRadians), 0.0f
                };
                gpuProbes.push_back(gpuProbe);
            }
        }
        if (!gpuProbes.empty())
        {
            commandList->UpdateBuffer(m_GIResources.ProbeBuffer.get(), gpuProbes.data(), sizeof(GPUReflectionProbe) * gpuProbes.size());
        }

        const float probeInfluenceDebug = (m_Settings.DebugView.View == DebugView::ProbeInfluence) ? 1.0f : 0.0f;
        constants.ProbeParams = {
            static_cast<float>(gpuProbes.size()),
            probeInfluenceDebug,
            m_Settings.ReflectionProbe.ParallaxCorrectionEnabled ? 1.0f : 0.0f,
            m_Settings.ReflectionProbe.BlendingEnabled ? 1.0f : 0.0f,
        };
        constants.ProbeParams2 = {
            m_Settings.ReflectionProbe.DepthParallaxEnabled ? 1.0f : 0.0f,
            m_Settings.ReflectionProbe.OcclusionEnabled ? 1.0f : 0.0f,
            static_cast<float>(Passes::kProbeCaptureSize),
            // 焼いた時点の実効プリ露出から現在の実効プリ露出への換算倍率
            // (ReflectionProbePasses::m_ProbeBakedExposureEV100のコメント参照)。ComputeExposure(ev)=1/(1.2*2^ev)
            // なので、比は 2^(焼いたEV - 現在のEV) になる。
            // フルベイクが走るフレームだけは1フレームぶん古い倍率になるが、それが問題になるのは
            // 「焼き直しと大きな露出変化が同じフレームで起きる」ときだけで、シーン読み込み時は
            // 上のm_EffectiveExposureInitialized=falseで露出が既に確定しているため起きない
            std::exp2(m_ReflectionProbePasses->GetProbeBakedExposureEV100() - m_EffectiveExposureEV100),
        };

        // モーションベクター用の前フレーム情報。初回フレームは前フレームの行列が未定義なので、
        // 今フレームと同じものを入れて速度を0にしておく。そうしないとゴミの速度が速度バッファへ
        // 焼き込まれ、画面全体が一度だけゴーストする
        if (m_TAAPrevViewProjValid)
        {
            constants.PrevViewProj = m_TAAPrevViewProj;
            constants.TAAParams = { frameContext.JitterUv.x, frameContext.JitterUv.y, m_TAAPrevJitterUv.x, m_TAAPrevJitterUv.y };
        }
        else
        {
            constants.PrevViewProj = constants.ViewProj;
            constants.TAAParams = { frameContext.JitterUv.x, frameContext.JitterUv.y, frameContext.JitterUv.x, frameContext.JitterUv.y };
        }

        // DDGI(22章)。一度も焼けていない間はアトラスの中身が未定義なので無効にしておく
        // (反射プローブの「一度でも焼けたか」と同じ方針)
        const bool ddgiActive = m_Settings.DDGI.Enabled && m_GIResources.HasGIVolume && m_DDGIPasses->IsBaked();
        constants.DDGIParams0 = {
            m_GIResources.GIVolume.Origin[0], m_GIResources.GIVolume.Origin[1], m_GIResources.GIVolume.Origin[2],
            ddgiActive ? 1.0f : 0.0f,
        };
        constants.DDGIParams1 = {
            m_GIResources.GIVolume.ProbeSpacing[0], m_GIResources.GIVolume.ProbeSpacing[1], m_GIResources.GIVolume.ProbeSpacing[2],
            m_GIResources.GIVolume.NormalBias,
        };
        constants.DDGIParams2 = {
            static_cast<float>(m_GIResources.GIVolume.ProbeCounts[0]),
            static_cast<float>(m_GIResources.GIVolume.ProbeCounts[1]),
            static_cast<float>(m_GIResources.GIVolume.ProbeCounts[2]),
            m_GIResources.GIVolume.ViewBias,
        };
        constants.DDGIParams3 = {
            static_cast<float>(Passes::kDDGIIrradianceTexels),
            static_cast<float>(Passes::kDDGIDistanceTexels),
            m_Settings.DDGI.Intensity,
            static_cast<float>(Passes::kDDGIProbeBorder),
        };
        // y = DeferredLightingがDDGIを低解像度パス(DDGIResolve)から引くか。
        // 【パスが実際に走る条件と一致させること】走らないのに1を渡すと、前フレームの
        // (あるいは未初期化の)低解像度バッファを読んで間接光が固まる/壊れる。
        // 条件はDDGIResolveパスの登録側(ddgiResolvePassRuns)と同じものを並べている
        const bool ddgiHalfResolutionActive =
            m_Settings.DDGI.HalfResolution && m_GIResources.DDGIResolveTexture && m_Settings.DDGI.Enabled && m_GIResources.HasGIVolume && m_DDGIPasses->IsBaked();
        // プローブ分類のしきい値。裏面の情報を持てるのはレイトレース経路だけなので、
        // ラスタ経路では分類そのものを無効(0)にして従来どおりの挙動に保つ
        // (ラスタ経路のαは常に0なのでどのしきい値でも有効側に倒れるが、
        //  「分類は掛かっていない」ことを値として明示しておく)
        const float ddgiBackfaceThreshold =
            (m_Settings.DDGI.ProbeClassificationEnabled && ShouldRunRaytracedDDGITrace()) ? m_Settings.DDGI.BackfaceThreshold : 0.0f;
        constants.DDGIParams4 = {
            effectiveExposure, ddgiHalfResolutionActive ? 1.0f : 0.0f,
            static_cast<float>(m_DDGIGrid.GetLODCount()), ddgiBackfaceThreshold
        };

        // クリップマップLODの各段の原点と、トロイダルaddressingの基準になる格子座標。
        // 使わない段も0で埋めておく(未初期化のまま渡すと、段数を増やした瞬間に
        // ゴミを読んで見当違いの場所からプローブを引く)
        static_assert(
            ShaderInterop::kDDGILODCount == kDDGIMaxLODCount,
            "FrameConstantsのDDGILOD配列の要素数とkDDGIMaxLODCountを一致させること"
            "(ずれるとcbufferのレイアウトが静かに食い違う)");
        for (uint32_t lod = 0; lod < kDDGIMaxLODCount; ++lod)
        {
            if (m_GIResources.HasGIVolume && lod < m_DDGIGrid.GetLODCount())
            {
                const DirectX::XMFLOAT3 lodOrigin = m_DDGIGrid.ComputeLODOrigin(lod);
                const DirectX::XMINT3 lodBase = m_DDGIGrid.ComputeLODBaseIndex(lod);
                constants.DDGILODOrigin[lod] = { lodOrigin.x, lodOrigin.y, lodOrigin.z, 0.0f };
                constants.DDGILODBase[lod] = {
                    static_cast<float>(lodBase.x), static_cast<float>(lodBase.y),
                    static_cast<float>(lodBase.z), 0.0f
                };
            }
            else
            {
                constants.DDGILODOrigin[lod] = { 0.0f, 0.0f, 0.0f, 0.0f };
                constants.DDGILODBase[lod] = { 0.0f, 0.0f, 0.0f, 0.0f };
            }
        }
        // 水面。スクロール位相はRenderThreadMainがm_Settings.Water.TimeFrozen/m_Settings.Water.WaveSpeedに
        // 応じて毎フレーム進める(m_Settings.Sky.TimeOfDayの自動進行と同じ場所・同じ方式)。
        // y=波のスケール倍率(m_Settings.Water.WaveScale)、z=波の強さ(m_Settings.Water.WaveStrength、0〜1)を
        // Water.hlslへ渡す(UIのスライダーが見た目へ反映されるようにするため)
        constants.TimeParams = { m_WaterScrollOffset, m_Settings.Water.WaveScale, m_Settings.Water.WaveStrength, 0.0f };

        // 雲。DeferredLighting.hlsl(背景)とSSR.hlsl(水面反射)の両方が同じ値を読むため、
        // ここで一度だけ組み立てる。m_Settings.Cloud.Enabled=falseのときはCloudParams0.xへ0を渡し、
        // Sky.hlsli側のSkyColorが早期脱出する経路(判断C)を通す
        constants.CloudParams0 = {
            m_Settings.Cloud.Enabled ? m_Settings.Cloud.Coverage : 0.0f,
            m_Settings.Cloud.Altitude,
            m_Settings.Cloud.UvScale,
            m_Settings.Cloud.Density,
        };
        // wには積雲の厚み[m]を詰めてある(FrameConstantsを増やさずに済ませるため)。
        // 0ならシェーダー側はレイマーチせず平面として扱う
        constants.CloudParams1 = {
            m_CloudScrollOffset.x, m_CloudScrollOffset.y, m_Settings.Cloud.ForwardG,
            m_Settings.Cloud.Volumetric ? m_Settings.Cloud.Thickness : 0.0f,
        };
        // 巻雲。積雲と同じ理由でここで一度だけ組み立てる。m_Settings.Cloud.CirrusEnabled=falseのときは
        // CloudParams2.xへ0を渡し、Sky.hlsli側のSkyColorが早期脱出する経路(判断C)を通す
        constants.CloudParams2 = {
            m_Settings.Cloud.CirrusEnabled ? m_Settings.Cloud.CirrusCoverage : 0.0f,
            m_Settings.Cloud.CirrusAltitude,
            m_Settings.Cloud.CirrusUvScale,
            m_Settings.Cloud.CirrusDensity,
        };
        constants.CloudParams3 = { m_CirrusScrollOffset.x, m_CirrusScrollOffset.y, m_Settings.Cloud.CirrusAnisotropy, m_Settings.Cloud.TypeBias };
        // 平面反射(P6)。このフィールドを参照するのはPlanarReflection.hlslだけで、そちらは
        // 専用のm_PlanarReflectionConstantBufferで明示的に上書きした値を使う
        // (Passes/ReflectionPassesが持つ)。共有のm_FrameConstantBufferにも一貫した値を入れておく
        constants.PlanarReflectionPlane = { 0.0f, 1.0f, 0.0f, frameContext.HasWaterInstance ? -frameContext.WaterPlaneY : 0.0f };

        // 大気遠近。AerialPerspective.hlsl/PlanarReflection.hlslの両方が読む。
        // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、m_Settings.Fog.Enabledの値に関わらず
        // 常に無効化する――DDSは任意の絵でPerezモデルとは無関係なため、in-scatter項の
        // 解析評価(SkyColor)をしてはいけない(SSRパスのwaterAnalyticSkyFlagと同じ判断)
        const float fogEnabledFlag = (m_Settings.Fog.Enabled && m_Settings.Fog.Density > 0.0f && frameContext.UsingProceduralSky) ? 1.0f : 0.0f;
        constants.FogParams0 = { m_Settings.Fog.Density, m_Settings.Fog.ScaleHeight, m_Settings.Fog.RefHeight, fogEnabledFlag };
        constants.FogParams1 = { m_Settings.Fog.MaxOpacity, 0.0f, 0.0f, 0.0f };
        // 水中項。Water.hlslのPSMainが読む
        constants.WaterBodyColor = { m_Settings.Water.BodyColor.x, m_Settings.Water.BodyColor.y, m_Settings.Water.BodyColor.z, 0.0f };

        // 星空。
        // 【昼は強度0にしてしまう】星は太陽が地平線下にあるときしか見えない。ここで0に
        // 落としておけば、Sky.hlsli側は最初のif文で抜けるので昼のシーンの絵は1画素も動かない
        // (m_Settings.Stars.Enabledを切ったときとまったく同じ経路を通る)。
        // sunLighting.SunPositionは太陽が「ある」向きなので、yが負なら地平線下。
        // 仰角0度から-8度にかけて滑らかに立ち上げ、市民薄明のあいだに星が出そろう形にする
        const float sunElevationSin = sunLighting.SunPosition.y;
        const float starsNightFactor = std::clamp((-sunElevationSin - 0.005f) * 8.0f, 0.0f, 1.0f);
        // 手続き空を使わないシーン(DDSスカイボックス指定)ではSkyColorの解析評価自体を
        // 通らないため、フォグの有効フラグと同じ判断で0にしておく
        const float starsIntensity =
            (m_Settings.Stars.Enabled && frameContext.UsingProceduralSky) ? (m_Settings.Stars.Brightness * starsNightFactor) : 0.0f;
        // 1画素が張る角度[rad]。射影行列の_22 = 1/tan(fovY/2) から
        // 画面の高さ全体が 2*tan(fovY/2) なので、1画素あたりはそれを縦解像度で割ればよい。
        // 解像度やFOVを変えても星の見かけの下限が追従する
        DirectX::XMFLOAT4X4 projForPixelAngle;
        DirectX::XMStoreFloat4x4(&projForPixelAngle, frameContext.JitteredProj);
        const float pixelAngle =
            (projForPixelAngle._22 > 0.0f && m_RenderHeight > 0)
                ? (2.0f / (projForPixelAngle._22 * static_cast<float>(m_RenderHeight)))
                : 0.001f;
        constants.StarsParams = { starsIntensity, m_Settings.Stars.Density, m_Settings.Stars.Twinkle, pixelAngle };

        // 積雲のボリュームレイマーチの段数。シェーダー側でも上限へ丸めるが、
        // 0以下を渡すと「コンパイル時の既定を使う」の意味になってしまうため下限はここで効かせる
        constants.CloudQualityParams = {
            static_cast<float>(std::clamp(m_Settings.Cloud.RaymarchSteps, 1u, kCloudRaymarchStepsMax)),
            0.0f, 0.0f, 0.0f
        };
    }
    // Hi-Zオクルージョンカリングの判定パラメータを確定させる
    void KurenaiEngine3D::ResolveOcclusionCullingFrameState(
        RHI::IRHICommandList* commandList, const DirectX::XMFLOAT3& cameraPosition,
        Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
        ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants)
    {
        // --- Hi-Zオクルージョンカリング(Stage 5-2)の判定パラメータ ---
        //
        // 判定に使うHi-Zは前フレームのもの(構築パスがG-Bufferパスより後に登録されるため)。
        // したがって「前フレームのHi-Zが実際に作られている」ことと「前フレームのビュー射影行列が
        // 本物である」ことの両方が要る。どちらかが欠けたフレームでは判定を丸ごと止める ――
        // 初回フレームや解像度変更の直後にここを通すと、未定義の深度で視界内をまとめて消す
        frameContext.OcclusionCullEnabledThisFrame =
            frameContext.OcclusionCullingActive && m_GeometryPasses->IsHiZValid() && m_TAAPrevViewProjValid;

        // 深度プリパスが走るなら、その深度からHi-Zを作れる。**そのフレームのG-Bufferは
        // 前フレームのHi-Zを待たなくてよい** ―― 上の2条件はどちらも要らなくなる。
        // 条件の意味と、プリパス自身が今フレームのHi-Zを使えない理由は、
        // 下の frameContext.HiZFromDepthPrepass を定義している箇所のコメントにある
        frameContext.DepthPrepassRuns = m_Settings.Geometry.DepthPrepassEnabled
            && m_GeometryPasses->CanRunDepthPrepass();
        frameContext.HiZFromDepthPrepass =
            m_Settings.Geometry.HiZFromDepthPrepassEnabled && frameContext.OcclusionCullingActive && frameContext.DepthPrepassRuns;

        // 前フレームからのカメラ移動距離。シーンが静的である以上、1フレームぶんの視差ずれの
        // 原因はカメラの移動だけなので、その距離をバウンディング球の半径へ足せば
        // 保守側(間引きすぎない側)へ倒せる。前フレームが無いフレームでは0でよい
        // (そのフレームは上のフラグで判定自体が止まっている)
        frameContext.CameraMoveDistance = 0.0f;
        if (m_TAAPrevViewProjValid)
        {
            const float dx = cameraPosition.x - m_PrevCameraPosition.x;
            const float dy = cameraPosition.y - m_PrevCameraPosition.y;
            const float dz = cameraPosition.z - m_PrevCameraPosition.z;
            frameContext.CameraMoveDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // 【フレーム全体の「判定するか」はここ、「どのHi-Zで判定するか」はドローごと】
        // 深度プリパスから作る経路では、前フレームのHi-Zが無いフレームでも
        // G-Bufferは今フレームのHi-Zで判定できる。どちらの経路も無いときだけ全体を止める
        // (ドローごとの選択は ObjectConstants::MeshletOcclusionMode)
        constants.OcclusionCullParams = {
            (frameContext.OcclusionCullEnabledThisFrame || frameContext.HiZFromDepthPrepass) ? 1.0f : 0.0f,
            m_Settings.Geometry.OcclusionCullRadiusScale,
            frameContext.CameraMoveDistance,
            static_cast<float>(m_GeometryPasses->GetHiZMipLevels()),
        };
        // Hi-Zのミップ0はG-Buffer深度と同じ解像度で作られる(CreateRenderTargets)
        constants.HiZScreenParams = {
            static_cast<float>(m_RenderWidth),
            static_cast<float>(m_RenderHeight),
            1.0f / static_cast<float>(std::max(1u, m_RenderWidth)),
            1.0f / static_cast<float>(std::max(1u, m_RenderHeight)),
        };

        // bindless番号をfloatで運ぶ。番号はkBindlessDescriptorCapacity(8192)未満で、
        // float32が誤差なく表せる整数の範囲(2^24)に十分収まる
        constants.MeshletCullStatsParams = {
            frameContext.MeshletCullStatsActive ? 1.0f : 0.0f,
            frameContext.MeshletCullStatsActive ? static_cast<float>(m_MeshletCullStatsBindlessIndex) : 0.0f,
            0.0f,
            0.0f,
        };

        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &constants, sizeof(constants));

        // スクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)が深度値からView空間Zを1除算で
        // 復元するための定数。Camera::GetProjectionMatrixの射影行列(行ベクトル規約)は
        // clip.z = viewZ * a + b、clip.w = viewZ なので depth = a + b / viewZ となり、
        // 逆に解いて viewZ = b / (depth - a)。近平面・遠平面から直接組み立てず射影行列の要素を
        // 読むのは、Reverse-Zの組み方が変わっても自動的に追従させるため
        // (XMFLOAT4X4の_rcは1始まりの行・列なので、_33が行2列2=a、_43が行3列2=b)。
        // ジッター済みの行列から読むが、TAAのジッターが書き換えるのは_31/_32だけなので
        // _33/_43の値そのものはジッターの有無で変わらない。それでもジッター済みを使うのは、
        // 「深度バッファに関わる計算はすべて深度を描いたときと同じ行列から導く」という
        // 不変条件を1箇所も破らないため(将来ジッターの入れ方を変えたときに黙ってずれない)
        DirectX::XMFLOAT4X4 projectionForDepthLinearize;
        DirectX::XMStoreFloat4x4(&projectionForDepthLinearize, frameContext.JitteredProj);
        const float depthLinearizeA = projectionForDepthLinearize._33;
        const float depthLinearizeB = projectionForDepthLinearize._43;

        // 直接光パスのb1へ渡すスクリーンスペースシャドウのパラメータ。パスのラムダから
        // 値キャプチャできるようここで組み立てておく
        // 太陽の影の手法。RTシャドウを選んでいてもパスを実行できない状況(高速化構造が無い等)では
        // カスケードシャドウマップへ落とす。シャドウマップは手法によらず描いてあるため、
        // 落ちても影が消えることはない
        const ShadowMode effectiveShadowMode =
            (m_Settings.Shadow.Mode == ShadowMode::Raytraced && !ShouldRunRaytracedShadow())
                ? ShadowMode::CascadedShadowMap
                : m_Settings.Shadow.Mode;

        // 【実体はRender()にある】frameContext.Lightingがこれを指す
        lightingConstants.LightCount =
        {
            static_cast<uint32_t>(gpuLights.size()),
            static_cast<uint32_t>(std::max(0, m_Settings.Shadow.ScreenSpaceMaxLightsPerPixel)),
            static_cast<uint32_t>(effectiveShadowMode),
            // MegaLightsが走るフレームは、ポイント/スポットの寄与をあちらが計算済みなので
            // 直接光パス側のライトループを止める。**「パスを積むか」と同じ述語で決めること** ――
            // ずれると二重加算(2倍明るい)か、ローカルライトが全部消えるかのどちらかになる
            ShouldRunMegaLights() ? 1u : 0u,
        };
        lightingConstants.SSSParams0 =
        {
            static_cast<float>(m_Settings.Shadow.ScreenSpaceStepCount),
            m_Settings.Shadow.ScreenSpaceMaxRayLength,
            m_Settings.Shadow.ScreenSpaceThickness,
            m_Settings.Shadow.ScreenSpaceEnabled ? 1.0f : 0.0f,
        };
        lightingConstants.SSSParams1 =
        {
            depthLinearizeA,
            depthLinearizeB,
            m_Settings.Shadow.ScreenSpaceNormalBias,
            m_Settings.Shadow.ScreenSpaceEdgeFade,
        };
        lightingConstants.TileParams =
        {
            m_RenderTargets.LightTileCountX,
            Passes::kLightTileSize,
            Passes::kLightTileCapacity,
            // 「このフレームのライトグリッドは有効か」。**パスを積む述語と同じものを使う** ――
            // トグルの状態(m_Settings.Geometry.LightCullingEnabled)ではなく実際に書いたかどうかで決める。
            // なおMegaLightsが走るフレームはLightCount.wが先に効くのでこの枝には入らない
            ShouldRunLightCulling() ? 1u : 0u,
        };

        // ライトリストの中身の更新。**直接光パスや半透明パスの中で呼んではいけない** ――
        // タイルライトカリングパスが両者より先にこのバッファを読むため、
        // グラフを組み立てる前に1箇所でまとめて済ませる(更新回数も1回で済む)。
        // 0灯のフレームでは更新自体を省略してよい(シェーダはライト数までしかループしないため)
        if (!gpuLights.empty())
        {
            commandList->UpdateBuffer(m_SceneGPUResources.LightBuffer.get(), gpuLights.data(), gpuLights.size() * sizeof(GPULight));
        }

        UploadDroneInstances(commandList);

        // 各パスをリソースの読み書き依存関係から自動的に順序付けて実行するレンダーグラフ。
        // トランジェントリソースの確保は行わず、既存の永続確保済みテクスチャ(G-Buffer・SceneColor等)を
        // そのまま読み書きする(詳細はRenderGraph.h参照)
    }

    // パス群へ配るスナップショットの、残った写しと述語の解決。
    //
    // 【アドレスを取る4行はここに無い】frameContext.Lights / Lighting / Constants / Sun は
    // BuildFrameContext 本体の末尾で結ぶ。実体は Render() のローカルで、
    // graph.Execute() まで生きている必要があるという関係を、1つの関数の中で読み切るため
    void KurenaiEngine3D::FillFrameContextSnapshot(
        const SunLighting& sunLighting, float effectiveExposure, float manualExposureScale,
        float keyReferenceEV100, const DirectX::XMFLOAT3& cameraPosition,
        Rendering::RenderFrameContext& frameContext, std::vector<GPULight>& gpuLights,
        ShaderInterop::FrameConstants& constants, Passes::LightingConstants& lightingConstants)
    {
        // --- パス群へ配るフレームのスナップショット(段階6) ---
        // 【実体はRender()にある】この下で埋めるフィールドのうち、Lights / Lighting / Constants /
        // Sun は生ポインタで、指す先もRender()のローカル
        // パス群が読む設定を、この1箇所でまとめて写す。**UIパネルの描画
        // (m_UIManager->Draw)はこの行より前で終わっている**ので、写しても値は変わらない
        frameContext.Settings.AmbientOcclusion = m_Settings.AmbientOcclusion;
        frameContext.Settings.Cloud = m_Settings.Cloud;
        frameContext.Settings.DDGI = m_Settings.DDGI;
        frameContext.Settings.DebugView = m_Settings.DebugView;
        frameContext.Settings.EmissiveLight = m_Settings.EmissiveLight;
        frameContext.Settings.Geometry = m_Settings.Geometry;
        frameContext.Settings.IBL = m_Settings.IBL;
        frameContext.Settings.MegaLights = m_Settings.MegaLights;
        frameContext.Settings.PostProcess = m_Settings.PostProcess;
        frameContext.Settings.ReflectionProbe = m_Settings.ReflectionProbe;
        frameContext.Settings.Reflection = m_Settings.Reflection;
        frameContext.Settings.Shadow = m_Settings.Shadow;
        frameContext.Settings.Sky = m_Settings.Sky;
        frameContext.Settings.Water = m_Settings.Water;
        // どの経路が走るかを、ここで1回だけ判定して配る。**述語の実装は増やさない**
        // (Should* が唯一の定義。パス群はその結果だけを見る)
        frameContext.MegaLightsRuns = ShouldRunMegaLights();
        frameContext.LightCullingRuns = ShouldRunLightCulling();
        frameContext.RaytracedShadowRuns = ShouldRunRaytracedShadow();
        frameContext.RaytracedAORuns = ShouldRunRaytracedAO();
        frameContext.RaytracedReflectionRuns = ShouldRunRaytracedReflection();
        frameContext.RaytracedDDGITraceRuns = ShouldRunRaytracedDDGITrace();
        frameContext.SuppressEmissiveForGI = ShouldSuppressEmissiveForGI();
        frameContext.MegaLightsSamplesPerPixel = MegaLightsSamplesPerPixel();
        frameContext.MeshletLOD = m_MeshletLODFrame;
        frameContext.EffectiveExposureEV100 = m_EffectiveExposureEV100;
        // 【ここで有効性を解決する】無効なフレームに何を配るかを1箇所で決めておく。
        // 群ごとに判定を書くと、片方だけ条件を変えたときに静かに食い違う
        frameContext.TAAPrevViewProj =
            m_TAAPrevViewProjValid ? m_TAAPrevViewProj : DirectX::XMFLOAT4X4{};
        frameContext.TAAPrevJitterUv = m_TAAPrevJitterUv;
        frameContext.TAAPrevEffectiveExposureEV100 = m_TAAPrevEffectiveExposureEV100;
        frameContext.TAAHistoryIndex = m_TAAHistoryIndex;
        frameContext.DeltaTime = m_RenderDeltaTime;
        frameContext.ActiveCloudTransmittance = m_ActiveCloudTransmittance;
        frameContext.RenderWidth = m_RenderWidth;
        frameContext.RenderHeight = m_RenderHeight;
        frameContext.WindowWidth = m_Window->GetWidth();
        frameContext.WindowHeight = m_Window->GetHeight();
        frameContext.Capabilities = m_RenderCapabilities;
        frameContext.ActiveAOTexture = GetActiveAOTexture();
        frameContext.ActiveAORawTexture = GetActiveAORawTexture();
        frameContext.ActiveReflectionOutput = GetActiveReflectionOutput();
        frameContext.UpscaleAvailable = IsUpscaleActive();
        frameContext.SwapChain = m_SwapChain.get();
        // 【遅延生成のためだけに渡す】使ってよいのはMegaLightsの読み戻しバッファだけ
        frameContext.Device = m_Device.get();
        frameContext.FrameConstantBuffer = m_FrameConstantBuffer.get();
        frameContext.ObjectConstantBuffer = m_ObjectConstantBuffer.get();
        frameContext.MaterialSamplers = m_MaterialSamplers.get();
        frameContext.ScreenSpaceSamplers = m_ScreenSpaceSamplers.get();
        frameContext.EffectiveExposure = effectiveExposure;
        frameContext.ManualExposureScale = manualExposureScale;
        frameContext.KeyReferenceEV100 = keyReferenceEV100;
        frameContext.CameraPosition = cameraPosition;
        frameContext.Lights = &gpuLights;
        frameContext.Lighting = &lightingConstants;
        frameContext.Sun = &sunLighting;
        frameContext.Constants = &constants;
        frameContext.IBL = &m_IBLResources;
        frameContext.Sky = &m_SkyResources;
        frameContext.Scene = &m_SceneGPUResources;
        frameContext.Targets = &m_RenderTargets;
        frameContext.GI = &m_GIResources;
        frameContext.ProbeBakeSignature = ComputeProbeBakeSignature();
        frameContext.DroneShow = &m_DroneShowResources;
        // 【述語をここで1回だけ決める】本描画と平面反射の2群が同じ判定を見る必要がある
        frameContext.DroneShowRuns = m_DroneShowEnabled && !m_DroneInstances.empty();
        frameContext.DroneCount = static_cast<uint32_t>(m_DroneInstances.size());
        frameContext.DroneShowBrightness = m_DroneShow.Data().Brightness;
        frameContext.DroneShowMinScreenRadius = m_DroneShowMinScreenRadius;
    }
}
