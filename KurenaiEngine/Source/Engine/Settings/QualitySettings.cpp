#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <string>

#include "Core/Logger.h"
#include "../Passes/DDGIPasses.h"

// 品質プリセットの取得・適用。KurenaiEngine3D のメンバ関数のまま、
// 翻訳単位だけをここへ分けている(宣言は KurenaiEngine3D.h のまま)。
// プリセットに入れる項目の実測根拠は Settings/QualitySettings.h のコメントにある
namespace Kurenai
{
    KurenaiEngine3D::QualitySnapshot KurenaiEngine3D::CaptureQualitySettings() const
    {
        QualitySnapshot settings;
        settings.Reflection = m_Settings.Reflection.Mode;
        settings.PlanarReflectionEnabled = m_Settings.Reflection.PlanarEnabled;
        settings.PlanarReflectionResolutionScale = m_Settings.Reflection.PlanarResolutionScale;
        settings.CloudVolumetric = m_Settings.Cloud.Volumetric;
        settings.CirrusEnabled = m_Settings.Cloud.CirrusEnabled;
        settings.StarsEnabled = m_Settings.Stars.Enabled;
        settings.TAAEnabled = m_Settings.PostProcess.TAAEnabled;
        settings.BloomEnabled = m_Settings.PostProcess.BloomEnabled;
        settings.ScreenSpaceShadowEnabled = m_Settings.Shadow.ScreenSpaceEnabled;
        settings.DDGIProbesPerFrame = m_Settings.DDGI.ProbesPerFrame;
        settings.SSAOKernelSize = m_Settings.AmbientOcclusion.SSAOKernelSize;
        settings.CloudRaymarchSteps = m_Settings.Cloud.RaymarchSteps;
        settings.DDGIUpdate = m_Settings.DDGI.UpdateMode;
        settings.DDGIHalfResolution = m_Settings.DDGI.HalfResolution;
        return settings;
    }

    void KurenaiEngine3D::ApplyQualitySettings(const QualitySnapshot& settings)
    {
        m_Settings.Reflection.Mode = settings.Reflection;
        m_Settings.Reflection.PlanarEnabled = settings.PlanarReflectionEnabled;
        m_Settings.Cloud.Volumetric = settings.CloudVolumetric;
        m_Settings.Cloud.CirrusEnabled = settings.CirrusEnabled;
        m_Settings.Stars.Enabled = settings.StarsEnabled;
        m_Settings.PostProcess.TAAEnabled = settings.TAAEnabled;
        m_Settings.PostProcess.BloomEnabled = settings.BloomEnabled;
        m_Settings.Shadow.ScreenSpaceEnabled = settings.ScreenSpaceShadowEnabled;
        m_Settings.DDGI.ProbesPerFrame = settings.DDGIProbesPerFrame;
        // カーネル自体の作り直しはSSAOパスの中で行う(段数が変わったことを見て作り直す)
        m_Settings.AmbientOcclusion.SSAOKernelSize = settings.SSAOKernelSize;
        m_Settings.Cloud.RaymarchSteps = settings.CloudRaymarchSteps;
        m_Settings.DDGI.HalfResolution = settings.DDGIHalfResolution;

        // 更新モードを変えたら停止状態は倒しておく。倒さないと「常時更新へ戻したのに
        // 止まったまま」になる(署名が変わるまで再開しないため)
        if (m_Settings.DDGI.UpdateMode != settings.DDGIUpdate)
        {
            m_Settings.DDGI.UpdateMode = settings.DDGIUpdate;
            m_DDGIPasses->GetUpdateSuspended() = false;
            m_DDGIPasses->GetStableCycles() = 0;
        }

        // 平面反射の解像度倍率だけはレンダーターゲットの作り直しを伴う。GPUがまだ参照している
        // 可能性があるためここでは直接代入せず、要求として積んでRender()の先頭で反映させる
        // (UI関数の中で直接リソースを作り直さない、という既存の作法に合わせる)
        RequestPlanarReflectionResolutionScale(settings.PlanarReflectionResolutionScale);
    }

    void KurenaiEngine3D::ApplyQualityPreset(QualityPreset preset)
    {
        m_Settings.Quality.Preset = preset;

        // 「高」はシーンを読み込んだ直後の状態へ戻す(QualitySnapshotのコメント参照)。
        // 静的な既定へ戻すと、SSRやTAAを自分で指定しているシーンの意図を壊す
        if (preset == QualityPreset::High)
        {
            ApplyQualitySettings(m_SceneDefaultQuality);
            Core::Logger::Info("KurenaiEngine3D", "品質プリセット「高」を適用しました(シーン読み込み直後の状態へ戻しました)");
            return;
        }

        // 「低」「中」はシーン既定を出発点にして、そこから重い項目だけを落とす。
        // シーンが元から無効にしているものを勝手に有効化しないよう、有効化は一切行わない
        QualitySnapshot settings = m_SceneDefaultQuality;

        // 実測でGIVolumeを持つシーンの最大負荷(40〜47ms、フレームの約4割)。
        // 1プローブにつきシーンを6回描くため、この値にほぼ比例する
        settings.DDGIProbesPerFrame = (preset == QualityPreset::Low) ? 2 : 4;
        // 焼き上がりが落ち着いたら止める。低は一巡だけ(最速で止まる代わりに間接光の
        // バウンスが1回ぶん)、中はkDDGIBounceCycles巡だけ焼いてから止める
        settings.DDGIUpdate = (preset == QualityPreset::Low) ? DDGIUpdateMode::OverwriteThenStop
                                                            : DDGIUpdateMode::ConvergeThenStop;
        // DDGIのサンプリングは実測でLightingパス23.9msのうち10.2msを占めていた。
        // 低解像度化は輪郭で滲む近似なので既定は無効だが、プリセットでは有効にする
        settings.DDGIHalfResolution = true;
        // 実測でジオメトリが画面を占めるシーンの4.8〜11.0ms。コストはほぼ段数に比例する。
        // シーン既定より増やすことはしない(プリセットは落とす方向のみ)
        settings.SSAOKernelSize = std::min(
            m_SceneDefaultQuality.SSAOKernelSize, (preset == QualityPreset::Low) ? 4u : 8u);
        // 実測31ms(水面のあるシーン)。低・中とも切る
        settings.Reflection = ReflectionMode::Off;
        settings.TAAEnabled = false;
        settings.BloomEnabled = false;
        settings.ScreenSpaceShadowEnabled = false;

        if (preset == QualityPreset::Low)
        {
            // ボリュメトリック積雲は実測で約10ms。低ではまるごと切る
            // (手続き雲そのものは残るので、空が真っ青になるわけではない。平面レイヤーへ落ちる)
            settings.CloudVolumetric = false;
            settings.PlanarReflectionEnabled = false;
            settings.CirrusEnabled = false;
            settings.StarsEnabled = false;
        }
        else
        {
            // 中はボリュームを残したまま段数だけ落とす。コストはほぼ段数に比例するため、
            // 「立体的な雲は残しつつ半分の値段にする」という中間段が作れる
            // (段数を実行時に変えられるようにしたのはこのため)。
            // SSAOの段数と同じく、シーン既定より増やすことはしない
            settings.CloudRaymarchSteps =
                std::min(m_SceneDefaultQuality.CloudRaymarchSteps, 6u);
        }
        // 平面反射は残す場合でも解像度を落とす。1.0を超える指定は
        // RequestPlanarReflectionResolutionScaleが弾くため、下げる方向のみで安全
        settings.PlanarReflectionResolutionScale =
            std::min(m_SceneDefaultQuality.PlanarReflectionResolutionScale, 0.25f);

        ApplyQualitySettings(settings);
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("品質プリセット「") + (preset == QualityPreset::Low ? "低" : "中") + "」を適用しました");
    }
}
