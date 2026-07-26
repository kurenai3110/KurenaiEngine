// シャドウパス: ライト視点から深度のみを描画する(頂点位置以外は不要)。
// カスケードシャドウマップ(CSM)のため、カスケードごとに1回ずつこのパスを実行し、
// その都度CascadeConstantsを該当カスケードのビュー・プロジェクション行列で更新して呼び出す。
// 共有のFrameConstantsとは別の専用バッファ(このシェーダはFrameConstantsを一切使わない)
cbuffer CascadeConstants : register(b0)
{
    float4x4 ViewProj;
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
    output.Position = mul(float4(input.Position, 1.0f), ViewProj);
    return output;
}

void PSMain(PSInput input)
{
    // 深度のみを書き込むためカラー出力は不要
}
