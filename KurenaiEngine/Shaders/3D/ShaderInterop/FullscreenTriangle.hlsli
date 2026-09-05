// 画面全体を1枚の三角形で覆うポストプロセス用の頂点シェーダーと、その出力構造体。
// **どちらもこのリポジトリで唯一の定義。** 以前は 11 本のシェーダーが同じものを持っていた。
//
// 【Common.hlsli と分けている理由】エントリポイントを含むヘッダーだからである。
// パッカーはインクルードを展開してからエントリを走査するので、このヘッダーを取り込んだ
// .hlsl はすべて VSMain を焼くことになる。ReconstructWorldPos だけが要る
// コンピュートシェーダー(MegaLights 各パスなど)へ VSMain が付いて回らないよう、
// 頂点シェーダーが実際に要るシェーダーだけがこちらを取り込む。

#ifndef KURENAI_SHADERINTEROP_FULLSCREENTRIANGLE_HLSLI
#define KURENAI_SHADERINTEROP_FULLSCREENTRIANGLE_HLSLI

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// 頂点バッファを持たず、SV_VertexID (0,1,2) だけから画面を覆う三角形を作る。
//
// 【矩形2枚ではなく三角形1枚である理由】対角線をまたぐクアッドの境目が無くなり、
// クアッド単位で走る PS の無駄と、境目に出る補間のつなぎ目が消える。
// UV は (0,0)-(2,0)-(0,2) となり、画面外へはみ出した部分はクリップで捨てられる。
//
// 【z は 0】ポストプロセスは深度テストを行わない。Reverse-Z では 0 が遠平面にあたるが、
// このパスは深度を書かないので値そのものに意味は無い
PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

#endif // KURENAI_SHADERINTEROP_FULLSCREENTRIANGLE_HLSLI
