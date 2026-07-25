#pragma once

namespace Kurenai::Assets
{
    struct Vertex
    {
        float Position[3];
        float Normal[3];
        float UV[2];
        // xyzは接線、wは従法線の向き(+1/-1)。法線マップ適用時のTBN行列構築に使う
        float Tangent[4];
    };
}
