// キューブマップの「面 → 方向」対応を1か所へ集めた共有ヘッダー。
// NormalEncoding.hlsli / Samplers.hlsli / Bindless.hlsli と同じ
// 「全シェーダ共通の宣言を1か所へ集める」枠組みに従う。
//
// 【なぜ共有しなければならないのか】この対応は「焼く側」と「読む側」で1文字でも違うと、
// 間接光が見当違いの方向から来る。しかもコンパイルは通り、絵も「それらしく」出るため
// 静かに間違ったまま進む。以前は IBLConvolve.hlsl / DDGIProbeUpdate.hlsl / SkyGenerate.hlsl の
// 3か所へ同じ式が複製され、それぞれのコメントが「他と一致させること」と警告し合っていた。
// 一致させ続ける約束ではなく、定義を1つにして構造的に保証する。
//
// 【Pythonの参照実装】Tools/generate_sky_cubemap.py の face_direction_grid が同じ規約を持つ。
// スカイボックスDDSはそちらで生成されるので、片方だけ変えてはいけない。

#ifndef KURENAI_CUBEFACE_HLSLI
#define KURENAI_CUBEFACE_HLSLI

// キューブマップの1面上のUV([0,1]^2)から、その面・そのテクセルが表す方向を求める。
// 面の番号はD3Dのキューブマップ標準順(+X=0, -X=1, +Y=2, -Y=3, +Z=4, -Z=5)。
float3 CubeFaceDirection(uint face, float2 uv)
{
    const float2 ndc = uv * 2.0f - 1.0f;
    const float u = ndc.x;
    const float v = ndc.y;

    float3 dir;
    if (face == 0)      dir = float3(1.0f, -v, -u);   // +X
    else if (face == 1) dir = float3(-1.0f, -v, u);   // -X
    else if (face == 2) dir = float3(u, 1.0f, v);     // +Y
    else if (face == 3) dir = float3(u, -1.0f, -v);   // -Y
    else if (face == 4) dir = float3(u, -v, 1.0f);    // +Z
    else                dir = float3(-u, -v, -1.0f);  // -Z

    return normalize(dir);
}

#endif // KURENAI_CUBEFACE_HLSLI
