#pragma once

#include <cstdint>

#include "../EngineDefaults.h"

namespace Kurenai
{
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

    struct DDGISettings
    {
        // 「レイをどう集めるか」を環境から選ぶ。DXRが使えるなら常にそちら ――
        // 更新コストが下がり、カメラから遠いプローブにも影が落ちるようになるため
        // (ReflectionModeForCapabilityと同じ考え方)
        static constexpr DDGIRayMode DDGIRayModeForCapability(bool raytracingAvailable)
        {
            return raytracingAvailable ? DDGIRayMode::Raytraced : DDGIRayMode::Raster;
        }

        // DDGIを低解像度パスから引くか。実測(ProbeTest / 1280x720 / DX11)では
        // Lightingパス23.9msのうちDDGIのサンプリングが10.2msを占めていた
        bool HalfResolution = Defaults::DDGIHalfResolution;
        bool Enabled = Defaults::DDGIEnabled;
        // 拡散間接光の強度倍率。DDGIとSSILは近傍/遠方で寄与が重なるため、実測で決めるための倍率
        float Intensity = Defaults::DDGIIntensity;
        int32_t ProbesPerFrame = Defaults::DDGIProbesPerFrame;
        // 【既定はAlways(従来どおり)】止める側を既定にすると既存シーンの実行時の挙動が変わるため。
        // 止めたい場合はこのつまみか品質プリセット(低/中)から選ぶ
        DDGIUpdateMode UpdateMode = DDGIUpdateMode::Always;
        // 【既定はDXRが使えるならDXR】DX11とDXR非対応機は自動的にラスタのまま。
        // 実際の値はレイトレーシングの可否が分かった時点(Initialize)で入れ直す
        DDGIRayMode RayMode = DDGIRayMode::Raster;
        // プローブ分類(壁や地面の内部に埋まったプローブをサンプリングから外す)を行うか。
        //
        // 【レイトレース経路でのみ意味を持つ】裏面に当たったことを記録できるのはDXR経路だけで、
        // ラスタ経路は裏面カリングの結果それを「空」として見てしまうため分類できない。
        // ラスタ経路ではこのフラグに関わらず分類は掛からない
        bool ProbeClassificationEnabled = true;
        // 裏面ヒット率がこれを超えたプローブを「信用しない」と判定する。
        //
        // 【既定値0.5の根拠】2つのシーンで分布と効果を実測し、**効果の向きが両シーンで
        // 揃う位置**を採った(分布・掃引・この根拠の弱いところは docs/ImplementationDetail.md 31.9.6)。
        // 分布そのものはデバッグ表示「DDGI - プローブ裏面率」で確認できる。
        //
        // 0.5は物理的な意味も明確で、「全レイの半分より多くが面の裏側に当たった」
        // = そのプローブは外より内側にいる、という判定になる
        float BackfaceThreshold = 0.5f;

        // レイトレース経路で太陽の影レイを撃つか。
        //
        // 【何のためにつまみにしてあるのか】これを切ると「影が落ちない」ラスタ経路の
        // 既知の制約と同じ状態になる。切り替えて絵と数値が動くことが、
        // レイトレース経路が実際に走っていることの対照実験になる(差分ゼロは合格ではない)。
        // 常用の想定は有効側で、ラスタ経路には効かない
        bool SunShadowRayEnabled = true;
    };
}
