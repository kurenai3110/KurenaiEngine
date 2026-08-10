// KurenaiEngine2D::DrawPolylineが使う、太さ付き折れ線の描画シェーダ。
//
// 【なぜ頂点バッファを使わないのか】DX12の頂点バッファはDEFAULTヒープに置かれており
// 毎フレーム書き換えられない(DX12Device::CreateBuffer参照)。そのため
// Shaders/3D/DroneShow.hlslと同じ「頂点バッファ無し + SV_VertexID + StructuredBuffer」で描く。
//
// 【なぜSprite2D.hlslと別ファイルなのか】このStructuredBufferと
// Sprite2D.hlslのSpriteTextureはどちらもt0を使う。実際に衝突するわけではない
// (下記のとおり参照する経路が別)が、同一ファイル内で同じレジスタへ2つ宣言すると
// コンパイラが警告・エラーにし得るため、ファイルごと分けてある。
//
// 定数バッファはSprite2D.hlslと同じレイアウト(b0=フレーム共通、b1=描画単位)。
// C++側のFrameConstants/ObjectConstantsと一致させること

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
};

cbuffer ObjectConstants : register(b1)
{
    float4x4 World; // DrawPolylineでは未使用(頂点は既にワールド座標)
    float4 Color;
    float4 UVOffsetScale; // 未使用
    float4 ShapeParams;   // 未使用
    float4 BorderColor;   // 未使用
};

// CPU側で接合(マイター/ベベル)まで済ませた三角形リストの1頂点。
// C++側のKurenaiEngine2D::PolylineVertexとバイト単位で一致させること(8バイト)
struct PolylineVertex
{
    float2 Position; // ワールド=ピクセル座標
};

// このt0はIRHICommandList::SetVertexShaderResourceBuffer専用の経路
// (DX12は可視性VERTEXのルートSRV、DX11は頂点シェーダステージのSRVスロット)を通るため、
// ピクセルシェーダ側のテクスチャ(Sprite2D.hlslのt0)とは別空間で衝突しない
StructuredBuffer<PolylineVertex> PolylineVertices : register(t0);

struct PolylinePSInput
{
    float4 Position : SV_POSITION;
};

PolylinePSInput VSPolyline(uint vertexID : SV_VertexID)
{
    PolylinePSInput output;
    // 頂点は既にワールド(=ピクセル)座標なのでWorld行列は掛けない
    output.Position = mul(float4(PolylineVertices[vertexID].Position, 0.0f, 1.0f), ViewProj);
    return output;
}

float4 PSPolyline(PolylinePSInput input) : SV_TARGET
{
    // 折れ線全体を1つのジオメトリ(重なりの無い三角形の集まり)として描くため、
    // 各画素はきっかり1回だけブレンドされる。半透明でも接合部の色が濃くならない
    return Color;
}
