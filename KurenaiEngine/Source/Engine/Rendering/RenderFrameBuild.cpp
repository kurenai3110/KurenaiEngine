#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "Core/Logger.h"

#include "DroneShowResources.h"
#include "GPULight.h"
#include "GPULightBuild.h"
#include "RenderFrameContext.h"
#include "SampleSequence.h"

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
        const bool megaLightsTileJitterEnabled = m_MegaLightsSettings.TileJitterMode != 0;
        DirectX::XMUINT2 megaLightsTileOffset{ 0u, 0u };
        if (m_MegaLightsSettings.TileJitterMode == 1)
        {
            // Halton(2,3)を16段階へ量子化する。RadicalInverseは[0,1)だが、丸め誤差でも
            // 16にならないようタイル幅-1で明示的に押さえる
            megaLightsTileOffset.x = std::min<uint32_t>(
                static_cast<uint32_t>(Rendering::RadicalInverse(m_TAAFrameIndex, 2u) * kLightTileSize),
                kLightTileSize - 1u);
            megaLightsTileOffset.y = std::min<uint32_t>(
                static_cast<uint32_t>(Rendering::RadicalInverse(m_TAAFrameIndex, 3u) * kLightTileSize),
                kLightTileSize - 1u);
        }
        frameContext.MegaLightsTileOffset = megaLightsTileOffset;
        // 無効時だけ従来のタイル数をそのまま使い、添字・乱数の種・ディスパッチ数を保存する。
        // モード2は対照実験なので、オフセット0でも有効側と同じ+1タイルを通す
        frameContext.MegaLightsEffectiveTilesX =
            megaLightsTileJitterEnabled ? (m_RenderTargets.LightTileCountX + 1u) : m_RenderTargets.LightTileCountX;
        frameContext.MegaLightsEffectiveTilesY =
            megaLightsTileJitterEnabled ? (m_RenderTargets.LightTileCountY + 1u) : m_RenderTargets.LightTileCountY;

        DirectX::XMFLOAT2 jitterOffsetPixels{ 0.0f, 0.0f };
        if (m_PostProcessSettings.TAAEnabled)
        {
            // Halton列の添字は1から始める。添字0はradical inverseの定義上どの基数でも0となり、
            // オフセットがピクセルの角(-0.5, -0.5)へ偏ってしまう
            const uint32_t haltonIndex = (m_TAAFrameIndex % Rendering::kTAAJitterSampleCount) + 1;
            jitterOffsetPixels.x =
                (Rendering::RadicalInverse(haltonIndex, 2) - 0.5f) * m_PostProcessSettings.TAAJitterScale;
            jitterOffsetPixels.y =
                (Rendering::RadicalInverse(haltonIndex, 3) - 0.5f) * m_PostProcessSettings.TAAJitterScale;
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
            m_MeshletLODFrame.Quality = m_GeometrySettings.MeshletLODEnabled ? m_GeometrySettings.MeshletLODQuality : 0.0f;
            m_MeshletLODFrame.Forced = m_GeometrySettings.MeshletLODEnabled ? m_GeometrySettings.MeshletLODForcedLevel : -1;
            m_MeshletLODFrame.DebugColorByLOD = m_GeometrySettings.MeshletLODDebugColorEnabled;
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
        // 【毎フレーム作り直す】m_EmissiveLightSettings.Intensity のスライダーとτを即座に反映するため。
        // プロキシ側は倍率も露出も持たない値(RadianceBase)で保持してある
        m_RenderStats.EmissiveLightsUsedCount = 0;
        // 切り捨てが起きたときだけ、採用した集合の指紋を残す(起きなければ0のまま)。
        //
        // 【プローブの署名に要る】採用順はカメラからの照度で決まるので、**カメラを動かすだけで
        // プローブが焼く光源の集合が変わる**。署名が変わらないと反射プローブはOnDemandで
        // 焼き直さず、DDGIは更新を止めたまま、収束済みのプローブだけ古い集合で残る。
        // 切り捨てが起きない限り集合はシーン固定なので、そのときは0で十分
        m_EmissiveLightsSelectionHash = 0;
        if (m_EmissiveLightSettings.LightsEnabled && !m_EmissiveProxies.empty() && manualLightCount < Rendering::kMaxLights)
        {
            const size_t budget = std::min<size_t>(
                static_cast<size_t>(std::max(0, m_EmissiveLightSettings.LightsMaxCount)), Rendering::kMaxLights - manualLightCount);

            if (m_EmissiveProxies.size() <= budget)
            {
                for (const Assets::EmissiveProxy& proxy : m_EmissiveProxies)
                {
                    gpuLights.push_back(MakeGPULightFromEmissiveProxy(
                        proxy, m_EmissiveLightSettings.Intensity, m_EmissiveLightSettings.LightsCutoffIrradiance,
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
                                       m_EmissiveLightSettings.Intensity * p.Area;
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
                        proxy, m_EmissiveLightSettings.Intensity, m_EmissiveLightSettings.LightsCutoffIrradiance, m_EmissiveLightsMaxRange));
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
}
