#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "Core/Logger.h"
#include "../Passes/DDGIPasses.h"

// DDGI(動的拡散GI)のプローブ配置とアトラスの管理。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま)
namespace Kurenai
{
    void KurenaiEngine3D::RecreateDDGIAtlases()
    {
        // アトラスの並び: 列 = ProbeCounts.x * ProbeCounts.y、行 = ProbeCounts.z。
        // XY平面のスライスを横に並べ、Zをそのまま行にする(RTXGIと同じ並び)。
        // プローブ番号との対応は index = x + y*Cx + z*Cx*Cy で、シェーダー側の
        // DDGIProbeAtlasCoord()と一致させること
        const uint32_t countX = m_GIResources.HasGIVolume ? m_GIResources.GIVolume.ProbeCounts[0] : 1u;
        const uint32_t countY = m_GIResources.HasGIVolume ? m_GIResources.GIVolume.ProbeCounts[1] : 1u;
        const uint32_t countZ = m_GIResources.HasGIVolume ? m_GIResources.GIVolume.ProbeCounts[2] : 1u;

        m_DDGIGrid.Configure(
            m_GIResources.HasGIVolume
                ? std::clamp(m_GIResources.GIVolume.LODCount, 1u, kDDGIMaxLODCount)
                : 1u,
            countX * countY * countZ);
        m_GIResources.DDGIProbeCount = m_DDGIGrid.GetProbesPerLOD() * m_DDGIGrid.GetLODCount();

        // 【LODは縦に積むだけ】列は変えず、行だけ段数倍にする。こうすると通し番号
        // slot = k*(Cx*Cy*Cz) + z*Cx*Cy + y*Cx + x に対して
        // 「行 = slot/(Cx*Cy)、列 = slot%(Cx*Cy)」がそのまま LOD k の行 [k*Cz, (k+1)*Cz) を指すので、
        // 更新CS(DDGIProbeUpdate.hlsl)のアトラス座標式を1文字も変えずに済む
        const uint32_t columns = countX * countY;
        const uint32_t rows = countZ * m_DDGIGrid.GetLODCount();

        // 【R32系である必要がある】更新CSはヒステリシス(前の値と新しい値のlerp)のために
        // アトラスをRWTexture2Dとして読んでから書く。型付きUAV読み出しはR32系しか保証されておらず、
        // fp16で読むにはTypedUAVLoadAdditionalFormatsが要る(AutoExposure.hlslが同じ理由で
        // R32_Floatを2テクセル並べる構成にしている)。
        // アトラスは455プローブでも合計1.4MB程度と小さいため、精度と可搬性を取って素直にR32にする
        m_GIResources.DDGIIrradianceAtlas = m_Device->CreateUAVTexture(
            columns * Passes::kDDGIIrradianceCell, rows * Passes::kDDGIIrradianceCell, RHI::Format::R32G32B32A32_Float);
        // R=平均距離、G=平均二乗距離
        m_GIResources.DDGIDistanceAtlas = m_Device->CreateUAVTexture(
            columns * Passes::kDDGIDistanceCell, rows * Passes::kDDGIDistanceCell, RHI::Format::R32G32_Float);

        // 進行状態は Passes::DDGIPasses が持つ。中身が未定義の新しいアトラスに対して
        // 「もう焼いてある」と誤判定させないため、確保の直後に一巡目からやり直させる
        m_DDGIPasses->ResetProgress(m_GIResources.DDGIProbeCount);
        // シーンが変わればメッシュ数も変わるので、クランプの報告も出し直す
        m_DDGIProbesPerFrameClampReported = false;

        if (m_GIResources.HasGIVolume)
        {
            Core::Logger::Info(
                "KurenaiEngine3D",
                "DDGIボリューム '" + m_GIResources.GIVolume.Name + "' を確保しました: " +
                    std::to_string(countX) + "x" + std::to_string(countY) + "x" + std::to_string(countZ) +
                    " = " + std::to_string(m_GIResources.DDGIProbeCount) + "プローブ, アトラス " +
                    std::to_string(columns * Passes::kDDGIIrradianceCell) + "x" + std::to_string(rows * Passes::kDDGIIrradianceCell) +
                    " / " + std::to_string(columns * Passes::kDDGIDistanceCell) + "x" + std::to_string(rows * Passes::kDDGIDistanceCell));
        }
    }

    uint32_t KurenaiEngine3D::ClampDDGIProbesPerFrameToConstantRing(uint32_t requested)
    {
        // DX11はどちらの上限も持たない(UINT32_MAXが返る)
        const uint32_t maxDraws = m_Device->GetMaxDrawsPerFrame();
        const uint32_t maxWrites =
            m_ObjectConstantBuffer ? m_ObjectConstantBuffer->GetSafeUpdatesPerFrame() : UINT32_MAX;
        // 描画1回につきObjectConstantsを1回書くので、厳しい方に合わせれば両方を満たす
        const uint32_t maxWork = std::min(maxDraws, maxWrites);
        if (maxWork == UINT32_MAX)
        {
            return requested;
        }

        // ラスタ経路が1プローブあたり描き直す不透明メッシュの数。
        // captureDDGIProbeFaceの描画ループと同じ条件で数えること
        uint32_t opaqueMeshCount = 0;
        for (const auto& instance : m_Scene.Instances)
        {
            // 【7545行目のDDGIラスタ経路と同じ段を数えること】ここの数が定数バッファリングの
            // 予算(ClampDDGIProbesPerFrameToConstantRing)を決めるため、実際に描く段と食い違うと
            // 予算の見積もりが狂う
            // ストリーミング中で未読み込みなら描かない
            const Assets::Model* const coarsestModel = GetCoarsestLOD(instance);
            if (!coarsestModel) { continue; }
            for (const auto& mesh : coarsestModel->Meshes)
            {
                if (!mesh.IsTransparent)
                {
                    ++opaqueMeshCount;
                }
            }
        }
        if (opaqueMeshCount == 0)
        {
            return requested;
        }

        const uint32_t reserved = opaqueMeshCount * kDDGIFrameBudgetReserveDrawsPerMesh;
        const uint32_t perProbe = kCubeFaceCount * opaqueMeshCount;
        // 本編のパスだけで予算を使い切る規模のシーンでは、DDGIへ回せる分が残らない。
        // それでも0にはせず1プローブは進める(進めないと永久に焼き上がらないため)。
        // 予算そのものの超過は、それぞれの上限側がエラーとして報告する
        const uint32_t budget = (maxWork > reserved) ? (maxWork - reserved) : 0u;
        const uint32_t allowed = std::max<uint32_t>(1u, budget / perProbe);

        if (requested <= allowed)
        {
            return requested;
        }

        if (!m_DDGIProbesPerFrameClampReported)
        {
            m_DDGIProbesPerFrameClampReported = true;
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "DDGIのラスタ経路が1フレームの予算を超えるため、更新プローブ数を " +
                    std::to_string(requested) + " から " + std::to_string(allowed) +
                    " へ制限しました(不透明メッシュ " + std::to_string(opaqueMeshCount) +
                    " × 6面 × プローブ数。1フレームの上限は 描画 " + std::to_string(maxDraws) +
                    " 回 / 定数書き込み " + std::to_string(maxWrites) +
                    " 回)。収束は遅くなりますが描画は壊れません。レイトレース経路にはこの制限は掛かりません");
        }
        return allowed;
    }


    uint64_t KurenaiEngine3D::ComputeProbeBakeSignature() const
    {
        // FNV-1a(64bit)。焼き上がりに影響する値だけを順に混ぜる。衝突しても起きるのは
        // 「本来必要な焼き直しを1回取りこぼす」だけで破綻はしないため、この程度の強度で足りる
        uint64_t hash = 1469598103934665603ull;
        const auto mixBytes = [&hash](const void* data, size_t size)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
        };
        const auto mixFloat = [&mixBytes](float value) { mixBytes(&value, sizeof(value)); };
        const auto mixBool = [&mixBytes](bool value) { const unsigned char v = value ? 1u : 0u; mixBytes(&v, sizeof(v)); };

        // 太陽と昼夜サイクル。ProbeCapture.hlslは共有のFrameConstantsから太陽の向き・色を読むため、
        // 時刻を動かすと焼き上がりが変わる
        mixFloat(m_Settings.Sky.TimeOfDay);
        mixFloat(m_Settings.Sky.SunAzimuthDegrees);
        mixBool(m_Settings.Sky.SunEnabled);
        // 影の手法ではなく「影を落とすかどうか」だけを混ぜる。ProbeCapture.hlslが読むのは
        // 常にカスケードシャドウマップで、そのシャドウマップはRTシャドウ選択時も同じように
        // 描かれるため、CascadedShadowMapとRaytracedでプローブの焼き上がりは変わらない
        mixBool(m_Settings.Shadow.Mode != ShadowMode::Off);
        // DDGIのレイの取得(ラスタライズ / レイトレーシング)と、その影レイの有無。
        //
        // 【混ぜ忘れると「つまみが効かない」型の不具合になる】切り替えても署名が変わらないため
        // 焼き直しが起きず、収束済みで停止しているモードでは絵が一切変わらない。
        // 反射プローブはこの2つの影響を受けないが、署名を共有しているため一緒に焼き直しになる
        // (余分な焼き直しが1回起きるだけで、破綻はしない)
        mixBool(m_Settings.DDGI.RayMode == DDGIRayMode::Raytraced);
        mixBool(m_Settings.DDGI.SunShadowRayEnabled);
        // 月は時刻に連動せず手動指定なので、太陽とは別に混ぜる必要がある。太陽が沈むと
        // 平行光源の枠が月へ切り替わり、キャプチャの直接光がそのまま変わる
        mixFloat(m_Settings.Sky.MoonAzimuthDegrees);
        mixFloat(m_Settings.Sky.MoonElevationDegrees);
        // キャプチャ内の環境項はグローバルIBLを引くため、その強度も焼き上がりに影響する。
        // 手続き空か.ksceneのDDSかで空そのものが変わるため、その切り替えも含める。
        // 拡散・鏡面の倍率もProbeCapture.hlslが同じように適用するため署名へ含める
        // (含め忘れると、つまみを動かしてもプローブの中身だけ古い倍率のまま残る)
        mixFloat(m_Settings.IBL.Enabled ? m_Settings.IBL.Intensity : 0.0f);
        mixFloat(m_Settings.IBL.AmbientDiffuseScale);
        mixFloat(m_Settings.IBL.AmbientSpecularScale);
        mixBool(m_Settings.Sky.ProceduralEnabled);
        // 自発光の強度倍率はキャプチャのエミッシブ項へそのまま乗る
        mixFloat(m_Settings.EmissiveLight.Intensity);
        // エミッシブ光源(62章)。プロキシはProbeCapture.hlslのライトループ(t8)にも入るので、
        // 有効/無効・打ち切り照度・採用数の上限はどれも焼き上がりを変える。
        // 二重計上の抑止はDDGIのキャプチャから自発光を抜くので、これも焼き上がりを変える。
        // **混ぜ忘れると「つまみが効かない」型の不具合になる**(このすぐ上の注記と同じ)
        mixBool(m_Settings.EmissiveLight.LightsEnabled);
        mixFloat(m_Settings.EmissiveLight.LightsCutoffIrradiance);
        mixFloat(static_cast<float>(m_Settings.EmissiveLight.LightsMaxCount));
        mixBool(m_Settings.EmissiveLight.LightsDoubleCountGI);
        // 【上限に当たると採用集合がカメラ依存になる】採用順はカメラからの照度で決まるため、
        // 上の4つだけでは「カメラを動かしただけで焼く光源が変わったのに署名は同じ」になる。
        // 切り捨てが起きていないフレームでは0で固定なので、余分な焼き直しは起きない
        mixBytes(&m_EmissiveLights.SelectionHash, sizeof(m_EmissiveLights.SelectionHash));

        // bent normalによる遮蔽(34章)。ProbeCapture.hlslが同じ分岐を持つため、
        // 含め忘れるとつまみを動かしてもプローブの中身だけ古いまま残る
        mixBool(m_Settings.AmbientOcclusion.BentNormalAOSource);
        mixFloat(static_cast<float>(m_Settings.AmbientOcclusion.SpecularOcclusion));
        mixBool(m_Settings.AmbientOcclusion.MultiBounceAOEnabled);

        // ライトは構造体ごとダンプすると詰め物(padding)の未初期化バイトを拾い得るため、
        // 使うフィールドだけを明示的に混ぜる
        for (const Assets::Light& light : m_Lights)
        {
            mixBytes(&light.Type, sizeof(light.Type));
            for (int i = 0; i < 3; ++i) mixFloat(light.Position[i]);
            for (int i = 0; i < 3; ++i) mixFloat(light.Direction[i]);
            for (int i = 0; i < 3; ++i) mixFloat(light.Color[i]);
            mixFloat(light.Intensity);
            mixFloat(light.Range);
            mixFloat(light.SpotInnerConeAngle);
            mixFloat(light.SpotOuterConeAngle);
            mixBool(light.Enabled);
        }

        // プローブの位置はキャプチャ地点そのものなので含める(影響範囲は含めない。
        // 形状・半径・ブレンド距離を変えてもどこから撮るかは変わらないため)
        for (const Assets::ReflectionProbe& probe : m_GIResources.ReflectionProbes)
        {
            for (int i = 0; i < 3; ++i) mixFloat(probe.Position[i]);
        }

        return hash;
    }
}
