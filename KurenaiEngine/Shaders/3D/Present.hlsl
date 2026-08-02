// Mode: 0=RGB(SceneColor/Albedo/Normal/Material/SSILの間接光など、そのまま表示できるバッファ用)
//       1=単チャンネルの深度をそのままpow()でコントラストを持ち上げて表示(シャドウマップ用。
//         正射影のため深度がライト視点距離に対して線形に分布し、これで十分見やすくなる)
//       2=透視投影のGBuffer深度用。NDC深度は遠方ほど値が1.0f付近に密集する非線形分布のため、
//         そのままpow()しても見分けがつかない。ワールド座標を再構成しカメラからの距離を
//         線形にグレースケール化する
//       3=AO/GIバッファのa(遮蔽率)チャンネルをグレースケール表示(SSAOのrgbは常に0のため専用)
//       4=直接光パスの結果(HDR、トーンマッピング前)をReinhardトーンマッピング+ガンマ補正して表示
//       5=深度の生値(0〜1)を加工せずそのままグレースケール表示(reverse-z等の生値確認用)
//       6=Hi-Zミップチェーンの指定ミップ(MipLevel)をSampleLevelで読み、生値のままグレースケール表示
//       7=G-Bufferのオクタヘドラルエンコード法線(R16G16_Float)をデコードし、[-1,1]を[0,1]へ
//         再マップして表示(法線マップのデバッグ表示で見慣れた配色にするため)
//       8=IBLプリフィルタ済み鏡面マップ(HDR、ミップごとにラフネスが異なる)の指定ミップ(MipLevel)を
//         SampleLevelで読み、Mode4と同じくReinhardトーンマッピング+ガンマ補正して表示
//       9=IBLの拡散イラディアンス/プリフィルタ済み鏡面(いずれも本物のTextureCube)のデバッグ表示。
//         SourceTexture(t0)ではなくDebugCubeTexture(t1)を、現在のカメラ視線方向で球面を
//         見回すように(背景スカイの表示と同じ要領でカメラ位置→ピクセル方向のレイを再構成して)
//         サンプルする。右クリックドラッグで視点を回せば球面全体を確認できる
//      10=カスケードシャドウマップ(Texture2DArray)の指定スライス(ArraySlice)をMode 1と同じ要領で
//         表示する。SourceTexture(t0)はTexture2Dのためテクスチャ配列を受け取れず、Mode 9と同じく
//         専用のDebugArrayTexture(t2)を使う
//      11=タイルライトカリング(LightCulling.hlsl)のライトグリッドをヒートマップ表示する。
//         タイルあたりのライト数を青→緑→赤で示し、容量超過のタイルはマゼンタで塗る。
//         カリングが効いているか・容量が足りているかを目視で確認する唯一の手段なので、
//         色は「数が読める」ことより「異常が目立つ」ことを優先している
//      12=反射プローブのキューブマップ配列(DebugCubeArrayTexture、t4)のデバッグ表示。
//         Mode 9と同じ見回し方で、ArraySliceで指定した番号のプローブをサンプルする
//      13=反射プローブの距離キューブ(同じくt4)。格納値はワールド距離なのでGainで縮めて
//         グレースケール表示する(19.12節)
//         (TextureCubeArrayはMode 10のTexture2DArrayとも別の型のため、さらにスロットを分ける)
//      14=モーションベクター(速度バッファ)。格納値はUV単位で1画素ぶんの移動が1/解像度と極端に
//         小さいため、ピクセル単位へ換算してから中間灰色(0.5)を「動いていない」として色付けする。
//         R>0.5=右へ、R<0.5=左へ、G>0.5=下へ、G<0.5=上へ画面内容が動いたことを表す
//      15=DDGIのイラディアンスアトラス(t0、22章)。Mode 4と同じくトーンマッピングして表示するが、
//         DataSampler(Point)で読む点が違う。アトラスは1プローブ8x8テクセルと極端に小さく、
//         画面いっぱいへ引き伸ばされる。バイリニアで読むとプローブ同士が溶け合って
//         「1プローブぶんのセル」の切れ目が見えなくなり、境界の複製が効いているかを確認できない
//      16=DDGIの距離モーメントアトラス(t0)。R=平均距離をGainで縮めてグレースケール表示する。
//         Gが平均二乗距離だが、そのまま出しても読めないためRのみを見る
//      17=G-BufferのMaterial.a(水面のマテリアルID、kMaterialIDWater。P2: 水面マテリアル基盤)を
//         そのままグレースケール表示する(水面=白、それ以外=黒)。0/1の二値でジオメトリの縁を
//         跨いで補間されると意味のない中間値になるため、Mode 1/2/5/7/14と同じくDataSamplerで
//         生値のまま読む
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ用(このシェーダでは未使用。オフセット合わせのためだけに宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    // 大気遠近(P8)・水中項(P8)。このシェーダーでは未使用だが、cbufferのレイアウトは宣言順で
    // 決まるため、末尾に追加のみで既存フィールドのオフセットを動かさない(この末尾へ追加する限り、
    // ここより前のフィールドしか読まないこのシェーダーは今回の変更の影響を受けない)
    float4 FogParams0;
    float4 FogParams1;
    float4 WaterBodyColor;
};

cbuffer PresentConstants : register(b1)
{
    int Mode;
    float MipLevel;
    // Mode 10ではカスケード番号、Mode 12では表示するプローブ番号として使う
    float ArraySlice;
    // デバッグ表示の輝度倍率。AO/GIバッファの間接拡散光のように値そのものが小さいバッファは
    // 等倍表示ではほぼ真っ黒になり、8bit格納時のポスタリゼーションが何段あるのか判別できない。
    // 色として表示するモード(0/3/4)にだけ適用する(深度・法線のように値の絶対値そのものに
    // 意味があるモードへ掛けると、かえって読み取れなくなるため)
    float Gain;
    // Mode 11(タイルライトカリングのヒートマップ)専用。
    // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの容量, w=ヒートマップの上限ライト数
    float4 TileParams;
    // Mode 11/14が使う。xy=レンダー解像度(Mode 11はUVからタイル座標を求めるのに、
    // Mode 14はUV単位の速度をピクセル単位へ換算するのに使う), zw=未使用
    float4 TileRenderSize;
};

Texture2D SourceTexture : register(t0);
// IBLのIrradiance/PrefilteredEnv(Mode 9)専用。それ以外のModeでは未使用(t0と違いTextureCube
// でなければならないため、専用の登録スロットを分けている)
TextureCube DebugCubeTexture : register(t1);
// カスケードシャドウマップ(Mode 10)専用。t1と同じ理由で、Texture2DArrayを受けるための
// 専用スロットを分けている
Texture2DArray DebugArrayTexture : register(t2);
// タイルライトカリングのライトグリッド(Mode 11)専用。レイアウトはLightCulling.hlsl冒頭を参照
StructuredBuffer<uint> LightTiles : register(t3);
// 反射プローブのキューブマップ配列(Mode 12)専用。TextureCubeArrayはTextureCube(t1)とも
// Texture2DArray(t2)とも別の型のため、さらにスロットを分ける必要がある
TextureCubeArray DebugCubeArrayTexture : register(t4);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// 頂点バッファなしで画面全体を覆う三角形を1枚だけ生成する定番のテクニック
PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    if (Mode == 6)
    {
        // Hi-Zはミップごとに解像度が異なるため、画面スケールから決まる自動ミップ選択(Sample)ではなく
        // 明示的に指定したミップ(MipLevel)を必ず読む。
        // 深度なのでDataSamplerで引き、テクセルの値をそのまま可視化する
        float depth = SourceTexture.SampleLevel(DataSampler, input.UV, MipLevel).r;
        return float4(depth, depth, depth, 1.0f);
    }

    if (Mode == 8)
    {
        float3 color = SourceTexture.SampleLevel(ColorSampler, input.UV, MipLevel).rgb;
        color = color / (color + 1.0f);
        color = pow(color, 1.0f / 2.2f);
        return float4(color, 1.0f);
    }

    if (Mode == 12)
    {
        // Mode 9と同じ見回し方。TextureCubeArrayのサンプリングは float4(方向, 配列番号)
        float3 farPoint = ReconstructWorldPos(input.UV, 0.0f);
        float3 rayDir = normalize(farPoint - CameraPosition.xyz);
        float3 color = DebugCubeArrayTexture.SampleLevel(MaterialSampler, float4(rayDir, ArraySlice), MipLevel).rgb;
        color = color / (color + 1.0f);
        color = pow(color, 1.0f / 2.2f);
        return float4(color, 1.0f);
    }

    if (Mode == 13)
    {
        // 反射プローブの距離キューブ(19.12節)。Mode 12と同じ見回し方だが、格納されているのは
        // 色ではなくワールド距離(メートル相当)なので、そのまま表示すると数メートルで白飛びする。
        // Debug View Gainで割った値をグレースケール表示し、遠いほど明るく見せる。
        // 空(ジオメトリ無し)には巨大な値が入っているため常に白になる。
        // サンプラーがDataSampler(Point)なのはReflectionProbe.hlsliと同じ理由(データの補間を避ける)
        float3 farPoint = ReconstructWorldPos(input.UV, 0.0f);
        float3 rayDir = normalize(farPoint - CameraPosition.xyz);
        float distance = DebugCubeArrayTexture.SampleLevel(DataSampler, float4(rayDir, ArraySlice), 0.0f).r;
        float gray = saturate(distance * Gain);
        return float4(gray, gray, gray, 1.0f);
    }

    if (Mode == 15)
    {
        // DDGIのイラディアンスアトラス(22章)。HDRなのでMode 4と同じくReinhard+ガンマで表示する。
        // DataSampler(Point)で読むのは、1プローブ8x8テクセルのセルが画面いっぱいへ
        // 引き伸ばされるため。バイリニアだとプローブ同士が溶け合い、セルの切れ目も
        // 境界1テクセルの複製も確認できなくなる
        float3 color = SourceTexture.Sample(DataSampler, input.UV).rgb * Gain;
        color = color / (color + 1.0f);
        color = pow(color, 1.0f / 2.2f);
        return float4(color, 1.0f);
    }

    if (Mode == 16)
    {
        // DDGIの距離モーメントアトラス(22章)。R=平均距離、G=平均二乗距離のうち、
        // 読めるのは平均距離だけなのでRのみをGainで縮めてグレースケール表示する
        float meanDistance = SourceTexture.Sample(DataSampler, input.UV).r;
        float gray = saturate(meanDistance * Gain);
        return float4(gray, gray, gray, 1.0f);
    }

    if (Mode == 9)
    {
        // 背景スカイの表示(DeferredLighting.hlsl)と同じ要領で、カメラ位置からこのピクセル方向への
        // レイを再構成する。Reverse-Zのため遠平面(=背景)はNDC z=0.0f付近になる
        float3 farPoint = ReconstructWorldPos(input.UV, 0.0f);
        float3 rayDir = normalize(farPoint - CameraPosition.xyz);
        float3 color = DebugCubeTexture.SampleLevel(MaterialSampler, rayDir, MipLevel).rgb;
        color = color / (color + 1.0f);
        color = pow(color, 1.0f / 2.2f);
        return float4(color, 1.0f);
    }

    if (Mode == 10)
    {
        // Mode 1と同じくpow()でコントラストを持ち上げる(正射影のため深度はライト視点距離に対して線形)。
        // 深度なのでMode 1と同様DataSamplerで引く
        float depth = DebugArrayTexture.Sample(DataSampler, float3(input.UV, ArraySlice)).r;
        depth = pow(saturate(depth), 0.25f);
        return float4(depth, depth, depth, 1.0f);
    }

    if (Mode == 11)
    {
        // UVはレンダーターゲット全体の[0,1]なので、レンダー解像度を掛ければピクセル座標になる。
        // タイル数Xでの割り算ではなくタイルサイズで割るのは、端の半端なタイルも正しく含めるため
        const uint2 pixelCoord = uint2(saturate(input.UV) * TileRenderSize.xy);
        const uint tileSize = max((uint)TileParams.y, 1u);
        const uint2 tileCoord = pixelCoord / tileSize;
        const uint tileCapacity = (uint)TileParams.z;
        const uint tileBase = (tileCoord.y * (uint)TileParams.x + tileCoord.x) * (1u + tileCapacity);

        // カリング側は容量を超えた数もそのまま書いているので、ここで超過を検出できる
        const uint lightCount = LightTiles[tileBase];
        if (lightCount > tileCapacity)
        {
            // 容量超過。ライトが静かに欠落している状態なので、他のどの色とも混ざらないマゼンタで示す
            return float4(1.0f, 0.0f, 1.0f, 1.0f);
        }
        if (lightCount == 0u)
        {
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        }

        // 1灯を青、上限(TileParams.w)を赤として青→緑→赤へ遷移させる
        const float t = saturate(float(lightCount) / max(TileParams.w, 1.0f));
        const float3 heat = (t < 0.5f)
            ? lerp(float3(0.0f, 0.0f, 1.0f), float3(0.0f, 1.0f, 0.0f), t * 2.0f)
            : lerp(float3(0.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), (t - 0.5f) * 2.0f);
        return float4(heat, 1.0f);
    }

    // Mode 1/2/5は深度、Mode 7はオクタヘドラルエンコードされた法線を読むため、補間されると
    // ジオメトリの縁で実在しない値になる(Mode 7は縁がにじみ、Mode 2は再構成位置がずれる)。
    // これらだけDataSamplerで引く。それ以外は色バッファなので、レターボックスの拡縮で
    // ブロック状にならないようColorSamplerで引く。
    // Modeは定数バッファ由来で波面内で一様のため、この分岐のコストは実質ゼロ
    const bool readsRawData = (Mode == 1 || Mode == 2 || Mode == 5 || Mode == 7 || Mode == 14 || Mode == 17);
    float4 sourceColor = readsRawData
        ? SourceTexture.Sample(DataSampler, input.UV)
        : SourceTexture.Sample(ColorSampler, input.UV);

    if (Mode == 17)
    {
        // 水面マスク。G-BufferのMaterial.aは水面で1.0、それ以外で0.0の二値なのでGainは適用しない
        float waterMask = sourceColor.a;
        return float4(waterMask, waterMask, waterMask, 1.0f);
    }

    if (Mode == 7)
    {
        float3 n = OctDecode(sourceColor.xy);
        return float4(n * 0.5f + 0.5f, 1.0f);
    }

    if (Mode == 1)
    {
        float depth = pow(saturate(sourceColor.r), 0.25f);
        return float4(depth, depth, depth, 1.0f);
    }

    if (Mode == 2)
    {
        float3 worldPos = ReconstructWorldPos(input.UV, sourceColor.r);
        float viewZ = mul(float4(worldPos, 1.0f), View).z;
        // カメラからの距離をz/(z+K)で0〜1に正規化する(Kはシーン規模を問わず見やすくなるよう
        // 経験的に選んだ値)。近いほど暗く、遠いほど明るいグレースケールになる
        float depth = saturate(viewZ / (viewZ + 20.0f));
        return float4(depth, depth, depth, 1.0f);
    }

    if (Mode == 3)
    {
        float ao = sourceColor.a * Gain;
        return float4(ao, ao, ao, 1.0f);
    }

    if (Mode == 4)
    {
        float3 hdr = sourceColor.rgb * Gain;
        float3 color = hdr / (hdr + 1.0f);
        color = pow(color, 1.0f / 2.2f);
        return float4(color, 1.0f);
    }

    if (Mode == 5)
    {
        float depth = sourceColor.r;
        return float4(depth, depth, depth, 1.0f);
    }

    if (Mode == 14)
    {
        // UV単位の速度をピクセル単位(1フレームあたり何画素動いたか)へ直す
        float2 velocityPixels = sourceColor.rg * TileRenderSize.xy;
        // 20画素の移動でGain=1のとき±0.5、つまり赤/緑が飽和する目安にする。
        // 速い動きを見たいときはGainを下げ、静止に近い微小な速度を見たいときは上げる
        float2 encoded = 0.5f + velocityPixels * (Gain / 20.0f);
        return float4(saturate(encoded), 0.5f, 1.0f);
    }

    return float4(sourceColor.rgb * Gain, 1.0f);
}
