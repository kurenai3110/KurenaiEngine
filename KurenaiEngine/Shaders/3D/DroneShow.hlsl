// ドローンショーの機体(発光点)を描くパス。
//
// 【何を描くか】夜空を編隊飛行する多数のドローンを、1機につきカメラへ正対する
// ビルボード(クアッド1枚)として加算合成で描く。機体そのものの形は描かない
// (実際のショーでも見えているのは機体ではなくLEDの光点である)。光芒は
// このパスでは作らず、HDRのまま出力して後段のブルームに任せる。
//
// 【頂点バッファを持たない理由】このエンジンのRHIはハードウェアインスタンシングを
// 持たず(IRHICommandList::DrawはinstanceCountを取らない)、かつDX12の頂点バッファは
// DEFAULTヒープに置かれるため毎フレームの書き換えができない。そこで
// Tonemap.hlsl/Present.hlslのフルスクリーン三角形と同じ「頂点バッファ無し+SV_VertexID」で
// 描き、機体データはStructuredBufferから頂点シェーダーが直接引く。
// Draw(6 * 機体数, 0) で呼ぶこと。
//
// 【t0を頂点シェーダーから読める理由】通常のSetTexture/SetShaderResourceBufferが使う
// SRVディスクリプタテーブルはピクセルシェーダーからしか見えない。このバッファは
// IRHICommandList::SetVertexShaderResourceBufferでバインドする専用の経路(DX12では
// 可視性をVERTEXに限定したルートSRV)を通っているため、同じt0でもピクセル側とは衝突しない。
//
// 【cbufferをb0のFrameConstantsに相乗りさせない理由】FrameConstantsは巨大で、
// cbufferが宣言順レイアウトである以上「読みたいフィールドまでの全フィールドを
// 正しい順序で宣言する」必要がある。実際にPlanarReflection.hlslでは宣言を1本忘れて
// 16バイトずれ、水面の鏡像が丸ごと消える不具合を出している。このパスが必要とするのは
// 行列2本とスカラー数個だけなので、b1に専用の小さなcbufferを持たせてその危険を断つ。
// この設計のおかげで、平面反射パス(鏡映カメラ)からも同じシェーダーを
// 「Viewだけ差し替えて」再利用できる。

// 機体1機ぶん。C++側の Kurenai::GPUDrone とバイト単位で一致させること
// (32バイト。ずれると位置と色が入れ替わって一切それらしく見えなくなる)
struct Drone
{
    float3 Position;   // ワールド座標[m]
    float  Radius;     // ビルボードの半径[m]
    float3 Color;      // 線形RGB
    float  Intensity;  // 発光強度(HDR。1.0を超えないとブルームのしきい値に届かない)
};

StructuredBuffer<Drone> Drones : register(t0);

cbuffer DroneShowConstants : register(b1)
{
    // ビュー行列。メインの描画ではカメラのビュー行列、平面反射パスでは
    // 「水面での鏡映 × カメラのビュー行列」が入る。ビルボードをこの空間で張るため、
    // 鏡映側では自動的に鏡映カメラへ正対する
    float4x4 View;
    // 射影行列。どちらのパスでもメインカメラのジッター済みProjを使う
    // (平面反射がメインの画面UVとサブピクセル単位でずれないようにするため)
    float4x4 Proj;
    // x=明るさ倍率、y=最小の画面上半径(NDC単位)、z=射影行列の[0][0]成分、
    // w=未使用。yとzの用途はVSMainの最小サイズのクランプを参照
    float4 Params0;
    // 平面反射で水面より下の機体を落とすためのクリップ平面。
    // xyz=法線(現状は常に(0,1,0))、w=距離項。水面より上でdot(pos,xyz)+wが正になる
    // (PlanarReflection.hlslのFrameConstants.PlanarReflectionPlaneと同じ規約)
    float4 ClipPlane;
    // x=クリップ平面を使うか(0でメイン描画、1で平面反射)、yzw=未使用
    float4 Params1;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    // ビルボード内のローカル座標。中心が(0,0)、四隅が(±1,±1)
    float2 UV : TEXCOORD0;
    // 既に Intensity と明るさ倍率を掛け終えた発光色
    float3 Color : TEXCOORD1;
    // 水面より下の機体をラスタライズ前に落とす(平面反射パスのみ)。正なら残す、負なら破棄。
    // fxc(SM5.0)・dxc(SM6.0)の両方で通ることはPlanarReflection.hlslで確認済み
    float ClipDistance : SV_ClipDistance0;
};

PSInput VSMain(uint vertexID : SV_VertexID)
{
    const uint droneIndex = vertexID / 6u;
    const uint corner = vertexID % 6u;

    const Drone drone = Drones[droneIndex];

    // クアッドの4隅を2つの三角形 [0,1,2] [0,2,3] で張る。
    // 隅の並びは 0=(-1,-1) 1=(-1,+1) 2=(+1,+1) 3=(+1,-1)。
    //
    // 【この順序である理由】PSOはカリングを有効にしたまま FrontCounterClockwise=false
    // (時計回りが表)で作る。ビュー空間の+Yは上だが、ビューポート変換で画面Yは下向きに
    // 反転するため、この並びが画面上で時計回りになる。逆順にすると全機が裏面として
    // 捨てられ、1機も映らない。
    //
    // 【平面反射(鏡映カメラ)でも同じPSOでよい】四隅のオフセットはViewで変換した"後"の
    // ビュー空間で足しているため、Viewが鏡映を含んでいてもクアッド自身の巻きは変わらない
    // (鏡映行列を一度も通らない)。メッシュ描画のようにワインディングを反転したPSOへ
    // 切り替えると、逆に1機残らず裏面として捨てられ水面に何も映らなくなる
    const uint quadVertex = (corner < 3u) ? corner : (corner - 1u);
    const float2 offset = float2(
        (quadVertex < 2u) ? -1.0f : 1.0f,
        (quadVertex == 0u || quadVertex == 3u) ? -1.0f : 1.0f);

    float4 viewPos = mul(float4(drone.Position, 1.0f), View);

    // 【最小サイズのクランプ】遠方の機体はワールド半径のままだと1画素を下回り、
    // TAAのサブピクセルジッターでフレームごとに点いたり消えたりする(星でも同じ問題が起きる)。
    // NDCでの半幅は radius * Proj[0][0] / viewZ なので、これを下限 Params0.y へ
    // 押し上げるのに必要なワールド半径を逆算してクランプする。
    // Proj[0][0]はHLSLの行列添字の解釈で迷わないようC++側から明示的に渡している
    const float viewZ = max(viewPos.z, 1e-3f);
    const float minRadius = Params0.y * viewZ / max(Params0.z, 1e-6f);
    const float radius = max(drone.Radius, minRadius);

    viewPos.xy += offset * radius;

    PSInput output;
    output.Position = mul(viewPos, Proj);
    output.UV = offset;
    output.Color = drone.Color * drone.Intensity * Params0.x;
    // クリップを使わないメイン描画では常に正の定数を入れて全機を残す
    output.ClipDistance =
        (Params1.x != 0.0f) ? (dot(drone.Position, ClipPlane.xyz) + ClipPlane.w) : 1.0f;
    return output;
}

// 裾(halo)と芯(core)の鋭さ。値を大きくするほど中心へ集中する。
// cbufferへ出さずシェーダー内定数にしてあるのは、見た目の詰めはブルームのしきい値と
// 明るさ倍率で足りており、ここを振る必要が実際に生じていないため
static const float kHaloExponent = 2.0f;
static const float kCoreExponent = 12.0f;
// 芯の寄与。裾を1としたときの相対値
static const float kCoreWeight = 3.0f;

float4 PSMain(PSInput input) : SV_TARGET
{
    // 四角形の外側を円形に切り落とす。この時点で捨てないと、隣接する機体の
    // クアッドの角どうしが重なって格子状のムラが出る
    const float d = length(input.UV);
    if (d >= 1.0f)
    {
        discard;
    }

    const float falloff = saturate(1.0f - d);
    const float halo = pow(falloff, kHaloExponent);
    const float core = pow(falloff, kCoreExponent);
    const float glow = halo + core * kCoreWeight;

    // 【アルファを1.0にすること】BlendMode::Additiveは src.rgb * src.a + dst.rgb なので、
    // アルファに減衰を入れると二重に掛かる。減衰はrgb側で済ませてある
    return float4(input.Color * glow, 1.0f);
}
