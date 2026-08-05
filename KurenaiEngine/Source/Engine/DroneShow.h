#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Assets/ShowLoader.h"

namespace Kurenai
{
    // 読み込んだドローンショー(.kshow)を時間で再生し、1フレームぶんの機体の状態を作る。
    //
    // 【編隊の形をここに持たないこと】形はAssets::ShowDataとして外から与えられ、
    // ここは補間だけを担う。形を作るのはTools/KurenaiShowEditorで、エンジンは形の作り方を
    // 知らない。ここへ生成手続きを置くと、新しい形を足すのにエンジンの再ビルドが要るようになり、
    // 手続きで書ける形しか作れなくなる。
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

    class DroneShow
    {
    public:
        // 再生するショーを差し替える。点は正規化空間(原点中心・代表半径1)のまま保持し、
        // ワールドへの配置はEvaluateがcenter/scaleで行う。
        // 点数が編隊間で食い違うデータは受け付けない(モーフの途中で機体が消えるため)
        void SetData(const Assets::ShowData& data);

        // showTime[秒]における全機の状態をoutDronesへ書く(サイズはDroneCountに揃えられる)。
        // center/scaleはシーンが持つ配置で、ショーのデータ側には無い。
        // SetDataを呼んでいない場合は空を返す
        void Evaluate(
            float showTime, const DirectX::XMFLOAT3& center, float scale, std::vector<GPUDrone>& outDrones) const;

        // 1巡にかかる時間[秒]。0を返す場合は未読み込み(呼び出し側はこの0でfmodしないこと)
        float LoopDuration() const;

        // 再生中のショー。Brightness等、描画側が必要とする値の取り出し口でもある。
        // 未読み込みのときは既定値のままのShowDataを返す
        const Assets::ShowData& Data() const { return m_Data; }
        bool HasData() const { return m_HasData; }

    private:
        Assets::ShowData m_Data{};
        bool m_HasData = false;
    };
}
