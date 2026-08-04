#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

namespace Kurenai
{
    // ドローンショーの編隊を手続き的に生成し、時間で形から形へ変形させる。
    //
    // 【このエンジンにアニメーションの仕組みが無いことについて】KurenaiEngineは
    // ModelInstance::Worldをシーン読み込み時に一度だけ書くきりで、時間で動くのは
    // 昼夜サイクル・水面/雲のスクロール位相といったスカラーだけである。ドローンショーは
    // 「多数の点が独立に動く」ため既存のどれにも乗らず、ここで独立に持つ。
    //
    // 出力はGPUDroneの配列だけで、描画方法(ビルボード・加算合成)には関知しない。
    // 描画側はKurenaiEngine3DのDroneShowパスとShaders/3D/DroneShow.hlsl。

    // GPUへ送る1機ぶんの状態。Shaders/3D/DroneShow.hlslのstruct Droneと
    // **バイト単位で一致させること**(GPULightがDirectLighting.hlsliと一致必須なのと同じ扱い。
    // ずれても絵は「それらしく」出てしまうので気付きにくい)
    struct alignas(16) GPUDrone
    {
        DirectX::XMFLOAT3 Position;  // ワールド座標[m]
        float Radius;                // ビルボードの半径[m]
        DirectX::XMFLOAT3 Color;     // 線形RGB(正規化された色。明るさはIntensityが持つ)
        float Intensity;             // 発光強度。プリ露出は掛けない(描画側が掛ける)
    };
    static_assert(sizeof(GPUDrone) == 32, "GPUDroneはDroneShow.hlslのstruct Droneと同じ32バイトであること");

    // 編隊の形。並んだ順にショーが進む
    enum class FormationShape : uint32_t
    {
        Sphere = 0,  // フィボナッチ球(黄金角で均等分布。極に密集しない)
        Ring,        // 水平の円環を高さ方向に数段
        Helix,       // 二重らせん
        Grid,        // 鉛直な平面格子(「空に浮かぶスクリーン」の見立て)
        Heart,       // ハート曲線を塗りつぶした平面
        Spiral,      // 対数らせん(銀河状)の平面
        Count
    };

    const char* FormationShapeName(FormationShape shape);

    struct DroneShowSettings
    {
        uint32_t Count = 1500;
        // 編隊の中心(ワールド座標)。高さは水面より十分上に置くこと
        DirectX::XMFLOAT3 Center = { 0.0f, 220.0f, 260.0f };
        // 編隊の代表的な半径[m]。各形状はこの長さを基準に組み立てられる
        float Scale = 130.0f;
        // 1機のビルボード半径[m]。遠方で1画素を割る場合はシェーダ側が最小サイズまで持ち上げる
        float Radius = 1.2f;
        // 1つの形を保つ時間[秒]
        float HoldSeconds = 6.0f;
        // 次の形へ変形する時間[秒]
        float MorphSeconds = 4.0f;
        // 機体ごとの微小な揺れ(ホバリング)の振幅[m]。0で完全に静止する。
        // 全機が数学的に完全な位置で静止すると模型のように見えるため既定で少しだけ入れる
        float HoverAmplitude = 0.6f;
        // 揺れと出発タイミングのばらつきを決める種。固定しておけば毎回同じ絵になる
        // (A/B比較の再現性のために乱数を実行時の時刻から取らない)
        uint32_t Seed = 20260804u;
    };

    class DroneShow
    {
    public:
        // 設定に合わせて全形状の点群を作り直す。機体数・Scale・Center・Seedを変えたら呼ぶ。
        // 生成した点はモーフで軌跡が交差しないよう方位角順に並べ替えてある(下記の実装参照)
        void Configure(const DroneShowSettings& settings);

        // 点群の形に影響しないパラメータだけを差し替える(生成と並べ替えをやり直さない)。
        // UIのスライダーを動かしている間に毎フレームConfigureが走るのを避けるためにある。
        // Configureを呼んでいない場合は何もしない
        void UpdateTimingSettings(float radius, float holdSeconds, float morphSeconds, float hoverAmplitude);

        // showTime[秒]における全機の状態をoutDronesへ書く(サイズはCountに揃えられる)。
        // Configureを呼んでいない場合は空を返す
        void Evaluate(float showTime, std::vector<GPUDrone>& outDrones) const;

        // showTimeがいまどの形にいるか(UIの表示用)。遷移中は「遷移元」を返す
        FormationShape CurrentShape(float showTime) const;
        // 1巡にかかる時間[秒]。0を返す場合は未初期化
        float LoopDuration() const;

        const DroneShowSettings& Settings() const { return m_Settings; }
        bool IsConfigured() const { return m_Configured; }

    private:
        // 1つの形の点群と色。要素数はどれもm_Settings.Count
        struct Formation
        {
            std::vector<DirectX::XMFLOAT3> Positions;
            std::vector<DirectX::XMFLOAT3> Colors;
        };

        DroneShowSettings m_Settings{};
        Formation m_Formations[static_cast<size_t>(FormationShape::Count)]{};
        bool m_Configured = false;
    };
}
