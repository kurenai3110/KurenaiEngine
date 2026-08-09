#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "KurenaiTypes.h"

namespace Kurenai::Assets
{
    // .kshow(ドローンショー)の読み書き。ランタイムは読みだけ、KurenaiShowEditorは両方を使う。
    // 書式の定義はShowPackage.hを参照。失敗時はstd::runtime_errorを投げる(LoadModelと同じ扱い)。
    //
    // 【ShowDataをModel.hのように独立したヘッダーへ置かない理由】Modelは実体としてGPUリソースを
    // 持ち、ローダー以外の多くの場所が触るためファイルを分けている。ShowDataは配列だけのPODで、
    // 触るのは読み書きの当事者(ローダー・エディタ)とその2つの消費者だけなのでここに置く。

    // 1つの編隊。PositionsとColorsの要素数はどちらもShowData::DroneCountに揃っている
    struct ShowFormation
    {
        // 編隊の表示名(UTF-8)。エディタの一覧に出るだけで、ランタイムの絵には影響しない
        std::string Name;
        // 正規化空間(原点中心・代表半径1)。ワールドへの配置は消費側がCenter/Scaleで行う
        std::vector<DirectX::XMFLOAT3> Positions;
        // 線形RGB(正規化された色。明るさはBrightnessが持つ)
        std::vector<DirectX::XMFLOAT3> Colors;
    };

    // ショー1本ぶん。既定値はShowHeaderのコメントを参照。
    // 【既定値をここに書くのはエディタが新規作成するときのため】ランタイムは必ずファイルから
    // 読むので既定値には到達しない
    struct ShowData
    {
        uint32_t DroneCount = 1500u;
        float Speed = 1.0f;
        float HoldSeconds = 6.0f;
        float MorphSeconds = 4.0f;
        float Brightness = 1.0f;
        float Radius = 4.0f;
        float HoverAmplitude = 0.6f;
        uint32_t Seed = 20260804u;
        // 並んだ順にショーが進み、最後から最初へ戻る
        std::vector<ShowFormation> Formations;
    };

    // 1巡にかかる時間[秒]。Speedは含まない(showTimeを進める側が掛ける)。
    // 編隊が無いときは0を返す。呼び出し側はこの0でfmodしないようガードすること
    KURENAI_LIB_API float ShowLoopDuration(const ShowData& data);

    KURENAI_LIB_API ShowData LoadShow(const std::wstring& filePath);

    // 中間ディレクトリが無ければ作る。DroneCountと各編隊の点数が食い違う場合は投げる
    // (点数の統一はエディタの責任で、ここは最後の関所として検査だけする)
    KURENAI_LIB_API void SaveShow(const std::wstring& filePath, const ShowData& data);
}
