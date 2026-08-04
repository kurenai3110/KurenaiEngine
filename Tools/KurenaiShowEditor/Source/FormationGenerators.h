#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

namespace Kurenai::ShowEditor
{
    // 編隊の点群を手続き的に撒く。**かつてはエンジン(Source/Engine/DroneShow.cpp)にあった**が、
    // それでは新しい形を足すのにエンジンの再ビルドが要り、形が出荷するDLLの一部になってしまう。
    // ここは「新しい形を足す場所」で、成果物は点データ(.kshow)としてエンジンへ渡る。
    //
    // どの生成器も「原点中心・代表半径1」の正規化空間へ点を撒く。ワールドへの配置
    // (Scale倍してCenterへ移動)はシーン側の仕事で、ここでは扱わない。

    enum class GeneratorKind : uint32_t
    {
        Sphere = 0,  // フィボナッチ球(黄金角で均等分布。極に密集しない)
        Ring,        // 水平の円環を高さ方向に数段
        Helix,       // 二重らせん
        Grid,        // 鉛直な平面格子(「空に浮かぶスクリーン」の見立て)
        Heart,       // ハート曲線を塗りつぶした平面
        Spiral,      // 対数らせん(銀河状)の平面
        Count
    };

    const char* GeneratorName(GeneratorKind kind);

    // 編隊内で下端の色から上端の色へ補間する2色
    struct FormationPalette
    {
        DirectX::XMFLOAT3 Low;
        DirectX::XMFLOAT3 High;
    };

    FormationPalette DefaultPalette(GeneratorKind kind);

    // countちょうどの点を正規化空間へ撒き、モーフの対応づけ(方位角→高さの安定ソート)まで
    // 焼き込んだ状態でoutPositionsへ書く。countは1以上であること
    void GenerateFormation(GeneratorKind kind, uint32_t count, std::vector<DirectX::XMFLOAT3>& outPositions);

    // 点群を、正規化空間での高さでパレットの2色の間に塗る。
    // 位置を並べ替えた**後**に呼ぶこと(色は位置に付随するため、順序を変えたあとで
    // 割り当てないと形と色がずれる)
    void PaintByHeight(
        const std::vector<DirectX::XMFLOAT3>& positions, const FormationPalette& palette,
        std::vector<DirectX::XMFLOAT3>& outColors);

    // 既にある点群を別の機体数へ作り直す。生成器の分かっている編隊は撒き直すのが正しいが、
    // .kshowから読み込んだ編隊は生成器が分からない(点しか残っていない)ため、
    // 添字を等間隔に取り直す近似で揃える
    void ResamplePoints(
        const std::vector<DirectX::XMFLOAT3>& source, uint32_t newCount,
        std::vector<DirectX::XMFLOAT3>& out);
}
