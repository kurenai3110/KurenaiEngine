#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

#include "KurenaiTypes.h"

// dllexportされたクラスが非export型(std::wstring/std::vector)をメンバに持つことによる
// C4251警告を抑制する。Camera.hと同じ理由・同じ扱い
#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Assets
{
    // 決定的なカメラ経路。**フレーム番号だけから姿勢を決める。**
    //
    // 【何のためにあるか】「カメラを動かしたときのノイズと遅れ」を測るには、同じ軌跡を
    // 何度でも再現できる必要がある。ところが通常の操作経路は
    //   - 移動量が Δt に比例する(KurenaiEngine3D::UpdateMovement)
    //   - 視点回転が GetAsyncKeyState(VK_RBUTTON) を見ており、PostMessage では原理的に駆動できない
    //     (これは事故ではなく、動作確認のPostMessageが実カーソルを動かさないことを
    //      構造的に保証するための意図した設計。KurenaiEngine3D::UpdateMouseLook のコメント参照)
    // ため、外部入力からは同じ軌跡を2回作れない。だからエンジンの内側に経路を持つ。
    //
    // 【時刻を持たない理由】DroneShow は `Time += renderDeltaTime` と**時刻を累積**しているが、
    // その方式は浮動小数の累積誤差が入るので**採らない**。EvaluatePose は整数のフレーム番号
    // だけを引数に取り、内部にも状態を持たない。したがって同じフレーム番号なら必ず同じ姿勢になる。
    //
    // 【カメラへは絶対値を置くこと】Camera::SetPosition / SetYawPitch を使う。
    // Camera::Move / Rotate は累積するので使ってはいけない(それが UpdateMovement が
    // Δt 依存になっている理由そのもの)。

    // キー間の補間方法
    enum class CameraPathInterp : uint8_t
    {
        // 区間内の速度が一定になる。解析的な期待値(等速なら誤差 = 速度 × 遅れ)が使えるので、
        // 遅れの物差しの校正に使う
        Linear,
        // 速度が連続になる。実測にはこちらを使う ―― 折れ点で速度が不連続だと、
        // その1フレームだけ再投影が大きく外れて遅れの相関にスパイクが乗る
        CatmullRom,
    };

    // 最終キーより後のフレームの扱い
    enum class CameraPathEnd : uint8_t
    {
        Hold, // 最後の姿勢で止まる
        Loop, // 先頭へ戻る
    };

    // 1つのキーフレーム。角度はラジアン(.kscene上は度で、SceneLoaderが読み込み時に変換する)
    struct CameraPathKey
    {
        uint32_t Frame = 0;
        float Position[3] = { 0.0f, 0.0f, 0.0f };
        float YawRadians = 0.0f;
        float PitchRadians = 0.0f;
    };

    struct CameraPathPose
    {
        DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };
        float YawRadians = 0.0f;
        float PitchRadians = 0.0f;
    };

    // 「この経路は本当に画面を動かしているか」の検算結果。
    // 静止カメラのまま測ってしまうと、測りたかったものが1つも測れていないのに
    // 数値だけは出てしまう ―― それを着手前に落とすためにある
    struct CameraPathMotionStats
    {
        uint32_t FrameCount = 0;
        // 位置の1フレーム差分のノルム [m/frame]
        float MinMetersPerFrame = 0.0f;
        float MedianMetersPerFrame = 0.0f;
        float MaxMetersPerFrame = 0.0f;
        // 画面中心の視線方向の1フレーム角度差 [deg/frame]
        float MinDegreesPerFrame = 0.0f;
        float MedianDegreesPerFrame = 0.0f;
        float MaxDegreesPerFrame = 0.0f;
        // 上の角度差を画面上の見かけ速度へ換算したもの [px/frame]。
        // **位置の移動による見かけ速度は距離に依存するので、ここには含まれない**
        // (シーンのジオメトリを知らないと出せない)。したがってこの値は下界であり、
        // 実際の画面速度はこれ以上になる。上界の確認は GBufferVelocity の実測で行う
        float MinPixelsPerFrame = 0.0f;
        float MedianPixelsPerFrame = 0.0f;
        float MaxPixelsPerFrame = 0.0f;
        // 回転由来の見かけ速度も位置の移動も、どちらもほぼ0だったフレームの数。
        // 1つでもあれば警告、全フレームがそうなら経路を拒否する
        uint32_t StillFrameCount = 0;
    };

    class KURENAI_LIB_API CameraPath
    {
    public:
        // キーを差し込む。**ここで検証と yaw の連続化(unwrap)まで済ませる。**
        // 失敗した場合は false を返し、error に理由を入れる(呼び出し側がログへ出す)。
        //
        // 検証するもの:
        //   - キーが2つ以上あること(1つでは「動く経路」にならない)
        //   - Frame が狭義単調増加であること(同じフレームに2つの姿勢は置けない)
        //   - 位置・角度がすべて有限であること
        bool SetKeys(
            std::wstring name, CameraPathInterp interp, CameraPathEnd end,
            std::vector<CameraPathKey> keys, std::string& error);

        const std::wstring& GetName() const { return m_Name; }
        bool IsValid() const { return m_Keys.size() >= 2; }
        CameraPathInterp GetInterp() const { return m_Interp; }
        CameraPathEnd GetEnd() const { return m_End; }
        size_t GetKeyCount() const { return m_Keys.size(); }

        // 経路1周のフレーム数(最終キーのFrame + 1)。無効な経路では0
        uint32_t FrameCount() const;

        // **この関数だけが姿勢を決める。** 引数は整数のフレーム番号のみで、内部状態を持たない。
        // 先頭キーより前のフレームは先頭キーの姿勢、最終キーより後は End の指定に従う
        CameraPathPose EvaluatePose(uint32_t pathFrame) const;

        // 経路が本当に動いているかを数える。fovYRadians と renderHeight は
        // 角度差を画面上の画素へ換算するために要る。無効な経路では false
        bool ComputeMotionStats(
            float fovYRadians, uint32_t renderHeight, CameraPathMotionStats& outStats) const;

    private:
        std::wstring m_Name;
        CameraPathInterp m_Interp = CameraPathInterp::CatmullRom;
        CameraPathEnd m_End = CameraPathEnd::Hold;
        std::vector<CameraPathKey> m_Keys;
    };
}

#pragma warning(pop)
