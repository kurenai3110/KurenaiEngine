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
};

cbuffer PresentConstants : register(b1)
{
    int Mode;
    float MipLevel;
    float ArraySlice;
    // デバッグ表示の輝度倍率。AO/GIバッファの間接拡散光のように値そのものが小さいバッファは
    // 等倍表示ではほぼ真っ黒になり、8bit格納時のポスタリゼーションが何段あるのか判別できない。
    // 色として表示するモード(0/3/4)にだけ適用する(深度・法線のように値の絶対値そのものに
    // 意味があるモードへ掛けると、かえって読み取れなくなるため)
    float Gain;
};

Texture2D SourceTexture : register(t0);
// IBLのIrradiance/PrefilteredEnv(Mode 9)専用。それ以外のModeでは未使用(t0と違いTextureCube
// でなければならないため、専用の登録スロットを分けている)
TextureCube DebugCubeTexture : register(t1);
// カスケードシャドウマップ(Mode 10)専用。t1と同じ理由で、Texture2DArrayを受けるための
// 専用スロットを分けている
Texture2DArray DebugArrayTexture : register(t2);

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

    // Mode 1/2/5は深度、Mode 7はオクタヘドラルエンコードされた法線を読むため、補間されると
    // ジオメトリの縁で実在しない値になる(Mode 7は縁がにじみ、Mode 2は再構成位置がずれる)。
    // これらだけDataSamplerで引く。それ以外は色バッファなので、レターボックスの拡縮で
    // ブロック状にならないようColorSamplerで引く。
    // Modeは定数バッファ由来で波面内で一様のため、この分岐のコストは実質ゼロ
    const bool readsRawData = (Mode == 1 || Mode == 2 || Mode == 5 || Mode == 7);
    float4 sourceColor = readsRawData
        ? SourceTexture.Sample(DataSampler, input.UV)
        : SourceTexture.Sample(ColorSampler, input.UV);

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

    return float4(sourceColor.rgb * Gain, 1.0f);
}
