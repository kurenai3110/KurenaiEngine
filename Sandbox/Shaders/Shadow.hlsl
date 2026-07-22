// シャドウパス: ライト視点から深度のみを描画する(頂点位置以外は不要)
// FrameConstantsはライティングパスと共通のバッファを使い回すため、
// このシェーダで使わない末尾のフィールドも含めて同じ並び順で宣言する
cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 LightViewProj;
};

struct VSInput
{
    float3 Position : POSITION;
};

struct PSInput
{
    float4 Position : SV_POSITION;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position = mul(float4(input.Position, 1.0f), LightViewProj);
    return output;
}

void PSMain(PSInput input)
{
    // 深度のみを書き込むためカラー出力は不要
}
