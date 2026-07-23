// SSIL(Screen Space Indirect Lighting) - Visibility Bitmaskパス。
// Olivier Therrien et al. "Screen Space Indirect Lighting with Visibility Bitmask"(2023)の
// アイデアを参考にした実装: GTAO/HBAOと同様に法線周りを複数の「スライス」(視線ベクトルVと
// 画面空間方向Dが張る平面)に分割する。各スライスは1本の直線(角度0..PIの範囲で十分。
// 角度iとi+PIは同一直線になるため)であり、+D側と-D側の両方をスクリーン空間で水平線サーチして
// 同じビットマスクへ積算してからAOへ加算する。遮蔽は2本の水平線角度ではなく
// 32個のセクタを持つビットマスク(uint)で表現する。
// これにより「手前の1点が遮蔽している角度範囲」だけをビットとして立てられるため、
// 薄いオブジェクトの裏に光が回り込む(Thickness Heuristic)表現ができ、
// 遠くの遮蔽物によって近くの隙間が誤って塗りつぶされる問題(GTAOの弱点)も緩和される。
//
// 間接光(GI)は、各サンプルで新規に隠れたビット数(=このサンプルが担う可視立体角の割合)を重みとして、
// サンプル地点のDirectLightingパスの結果(シャドウ適用済みの直接光)を間接反射光として積算する。
// 環境光はDirectLightに含まれないため、間接光の反射光源としても使わない(DirectLightingパスは
// このSSILパスより前に実行されるため、事前計算済みの値をそのままサンプルできる)。
//
// PSMain: G-BufferのNormal/Depthと直接光バッファから遮蔽率(AO, alpha)と間接拡散光(GI, rgb)を計算する
// PSMainBlur: SSAOパスと共有する汎用RGBAボックスブラー(SSAO.hlsl側)を使い回すため、ここには実装しない
static const float PI = 3.14159265359f;
static const float HALF_PI = 1.57079632679f;
static const uint kSectorCount = 32u;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 LightViewProj;
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
};

cbuffer SSILConstants : register(b1)
{
    float4 Params0; // x: 半径(ワールド空間), y: 厚み(Thickness Heuristic), z: 間接光の強さ, w: AOのコントラスト(べき乗)
    uint4 Params1;  // x: スライス数, y: スライスあたりのステップ数, z/w: 未使用
};

// Texture0=World Normal, Texture1=Depth (どちらもG-Buffer、レンダー解像度と同じ)
// Texture2=DirectLighting.hlslで計算済みの直接光(シャドウ適用済み、レンダー解像度と同じ)
Texture2D Texture0 : register(t0);
Texture2D Texture1 : register(t1);
Texture2D Texture2 : register(t2);
SamplerState DefaultSampler : register(s0);

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

// ビュー空間の点を画面UVへ投影する(SSAOパスの流儀と同じくmul(vec,Proj)してw除算する)
float2 ProjectToUV(float3 viewPos)
{
    float4 clipPos = mul(float4(viewPos, 1.0f), Proj);
    float w = abs(clipPos.w) > 1e-4f ? clipPos.w : 1e-4f;
    float2 ndc = clipPos.xy / w;
    return float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
}

// UVを最近傍テクセル中心へスナップする。深度は非線形かつグレージング面で急激に変化するため、
// バイリニアフィルタで読むとエッジ付近で存在しない中間深度が得られ、再構成した位置が大きくずれる
// (特に手前・グレージングの床)。テクセル中心ちょうどをサンプルすればバイリニアでも補間が起きず、
// 実質ポイントサンプリングになる(参照実装XeGTAO/Bevyが深度・法線にポイントサンプラーを使う理由と同じ)。
// スナップ後のUVを深度サンプルと位置再構成の両方に使うことで、深度とNDC-xyの整合も保たれる
float2 SnapToTexel(float2 uv, float2 texSize)
{
    return (floor(uv * texSize) + 0.5f) / texSize;
}

// ピクセル座標から[0,1)の疑似乱数を得るハッシュ関数(Dave Hoskinsのhash12)。
// スライスの向きを毎ピクセルわずかに回転させ、バンディングを高周波ノイズに変換する(後段でブラーする)
float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

// ベクトルwをスライス平面の基底(e1, V)上の符号付き角度(atan2(dot(w,e1), dot(w,V))と等価)として測る。
// sign(x)*acos(y/r) は θ∈(-π,π) の範囲でatan2(x,y)と厳密に一致する(x=r sinθ, y=r cosθより)ため、
// wの平面内接線成分(dot(w,e1))を別途計算しなくても、xの符号さえ分かれば角度が求まる。
// サンプルの水平線サーチでは常に既知の方向(+dir2D側/-dir2D側)へ進むため、そのxの符号は
// サーチ方向の符号(sideSign)と一致する(w=sampleViewPos-Pがスライス平面上にほぼ乗るため)。
// これによりサンプルごとにe1との内積を取る必要がなくなる
// HLSLのsign()は入力が正確に0のとき0を返してしまい、その場合角度情報が失われる
// (acosの値が0とπのどちらでも符号0では区別できなくなる)ため、0を含めない符号関数を使う
float SignNonZero(float x)
{
    return (x >= 0.0f) ? 1.0f : -1.0f;
}

float SignedAngleFromV(float3 w, float3 V, float sideSign)
{
    float len = length(w);
    float cosAngle = (len > 1e-6f) ? clamp(dot(w, V) / len, -1.0f, 1.0f) : 1.0f;
    return sideSign * acos(cosAngle);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float depth = Texture1.Sample(DefaultSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)は遮蔽なし・間接光なし
        // Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const float radius = Params0.x;
    const float thickness = Params0.y;
    const float intensity = Params0.z;
    const float aoPower = Params0.w;
    const uint sliceCount = max(Params1.x, 1u);
    const uint stepCount = max(Params1.y, 1u);

    // 深度テクスチャの解像度(サンプルUVをテクセル中心へスナップするために使う)
    uint depthTexW, depthTexH;
    Texture1.GetDimensions(depthTexW, depthTexH);
    float2 depthTexSize = float2(depthTexW, depthTexH);

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 normalWorld = normalize(Texture0.Sample(DefaultSampler, input.UV).xyz * 2.0f - 1.0f);

    float3 P = mul(float4(worldPos, 1.0f), View).xyz;
    float3 N = normalize(mul(normalWorld, (float3x3)View));
    float3 V = normalize(-P);

    // ワールド半径を、現在の画素の深度における画面UV半径に変換する(視点から遠いほどUV上では小さくなる)。
    // ここではビュー空間X方向のオフセットで測るため、得られるのはUVのX方向スケール。
    float2 uvAtRadius = ProjectToUV(P + float3(radius, 0.0f, 0.0f));
    float screenRadiusUV = clamp(length(uvAtRadius - input.UV), 0.0f, 0.5f);
    if (screenRadiusUV < 1e-5f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    // アスペクト比補正。UV空間は非正方形(16:9等)なので、X方向で校正したscreenRadiusUVを
    // dir2Dへ等方的に適用すると、UVの円が世界空間では横長の楕円になり、縦方向スライスの
    // サンプルが世界空間でradius/aspectまでしか届かず非等方になる。dir2DのY成分をaspect倍して
    // 世界空間で等方な円になるようにする(proj[1][1]=proj[0][0]*aspectの関係より)
    float aspect = depthTexSize.x / depthTexSize.y;
    float2 uvRadiusScale = float2(1.0f, aspect) * screenRadiusUV;

    float jitter = Hash12(input.Position.xy);

    float ao = 0.0f;
    float3 gi = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (uint i = 0; i < sliceCount; ++i)
    {
        // 1つのスライスは1本の直線(角度iとi+PIは同一直線なので0..PIだけを走査すればよい)。
        // この直線の+dir2D側と-dir2D側の両方をサーチしてから、まとめてAOへ加算する
        float angle = (float(i) + jitter) / float(sliceCount) * PI;
        float2 dir2D = float2(cos(angle), sin(angle));
        // dir2DはUV空間の方向(yは下向きが正)。Dはビュー空間の方向として使うため、
        // UV→NDC変換でyが反転する分をここで打ち消す(ProjectToUV/ReconstructWorldPosのy反転と対応)
        float3 D = normalize(float3(dir2D.x, -dir2D.y, 0.0f));

        // スライス平面(DとVが張る平面)の法線と、その平面内でVに直交するタンジェント方向を求める。
        // 法線Nはこの平面上にあるとは限らない(サンプルと違って自由な3次元ベクトルな)ので、
        // 平面へ投影したうえでe1を基準にした符号付き角度を求める必要がある(スライスごとに1回だけ)
        float3 planeNormal = cross(D, V);
        float planeNormalLen = length(planeNormal);
        if (planeNormalLen < 1e-5f)
        {
            continue;
        }
        planeNormal /= planeNormalLen;
        float3 e1 = normalize(cross(V, planeNormal));

        float3 projectedNormal = N - planeNormal * dot(N, planeNormal);
        float projectedNormalLen = length(projectedNormal);
        if (projectedNormalLen < 1e-5f)
        {
            // 法線がこのスライス平面にほぼ垂直: このスライスはNの遮蔽判定にほとんど寄与しない
            continue;
        }
        // 投影した法線とVのなす角(V軸基準の符号付き角度)。法線はカメラを向いている(front-facing)前提で
        // cosをsaturateし、normalAngleを[-HALF_PI, HALF_PI]に収める(参照実装XeGTAO/Bevyと同じ)。
        // これにより後段のangleFront/angleBackが範囲外へ飛んでも、単純なクランプで正しく地平線に収まる
        float normalSign = SignNonZero(dot(projectedNormal, e1));
        float cosNormal = saturate(dot(projectedNormal, V) / projectedNormalLen);
        float normalAngle = normalSign * acos(cosNormal);

        uint sliceMask = 0u;

        [loop]
        for (uint side = 0; side < 2u; ++side)
        {
            // side=0: +dir2D方向, side=1: -dir2D方向
            float sideSign = (side == 0u) ? 1.0f : -1.0f;

            [loop]
            for (uint s = 1; s <= stepCount; ++s)
            {
                float t = float(s) / float(stepCount + 1u);
                float2 sampleUV = input.UV + dir2D * (sideSign * t * uvRadiusScale);
                if (sampleUV.x < 0.0f || sampleUV.x > 1.0f || sampleUV.y < 0.0f || sampleUV.y > 1.0f)
                {
                    continue;
                }

                // 深度・法線・直接光のサンプルは、以降すべてこのスナップ後のUVで読む。
                // これにより深度がポイントサンプリング相当になり、再構成位置(sampleViewPos)が
                // テクセルの実際の値と一致する
                sampleUV = SnapToTexel(sampleUV, depthTexSize);

                float sampleDepth = Texture1.Sample(DefaultSampler, sampleUV).r;
                if (sampleDepth <= 0.0f)
                {
                    continue;
                }

                float3 sampleWorldPos = ReconstructWorldPos(sampleUV, sampleDepth);
                float3 sampleViewPos = mul(float4(sampleWorldPos, 1.0f), View).xyz;

                float3 offsetFront = sampleViewPos - P;
                float distFront = length(offsetFront);
                if (distFront < 1e-5f || distFront > radius)
                {
                    continue;
                }

                // Thickness Heuristic: サンプル点は無限に薄い点ではなく、Vの逆方向(カメラから遠ざかる方向)に
                // thicknessぶん奥行きを持つ板とみなす。offsetFront/offsetBackはスライス平面(e1とVが張る平面)上に
                // ほぼ乗っているため、Vずらしても平面上に留まる(e1・Vの線形結合のまま)
                float3 offsetBack = offsetFront - V * thickness;

                // offsetFront/offsetBackはスライス平面上にほぼ乗っており、+dir2D側/-dir2D側どちらを
                // サーチしているかは既知(sideSign)なので、e1との内積を別途取らなくても
                // sign(x)*acos(y/r) ≡ atan2(x,y) (x=平面内接線成分, y=dot(w,V))の関係から符号付き角度が求まる。
                // 法線基準の角度に変換する(ここでは周期のラップはしない。範囲外は後段のクランプで地平線に収める。
                // ラップすると±πを跨いだサンプルが反対側の半球へ符号反転して偽の遮蔽を作るため)
                float angleFront = SignedAngleFromV(offsetFront, V, sideSign) - normalAngle;
                float angleBack = SignedAngleFromV(offsetBack, V, sideSign) - normalAngle;

                // 法線の接平面より下(可視半球の外)は寄与しないので範囲外として扱う
                if (angleFront < -HALF_PI && angleBack < -HALF_PI)
                {
                    continue;
                }
                if (angleFront > HALF_PI && angleBack > HALF_PI)
                {
                    continue;
                }

                float angleFrontClamped = clamp(angleFront, -HALF_PI, HALF_PI);
                float angleBackClamped = clamp(angleBack, -HALF_PI, HALF_PI);

                float theta01Min = (min(angleFrontClamped, angleBackClamped) + HALF_PI) / PI;
                float theta01Max = (max(angleFrontClamped, angleBackClamped) + HALF_PI) / PI;

                // 角度幅が32セクタの分解能に満たない(丸めるとほぼ0になる)場合、無理に1ビット占有させると
                // 平坦な床のようにほぼ同一平面上のサンプルが多数ある場面で偽の遮蔽・ノイズが積み重なってしまう。
                // 真に角度幅が無い(=遮蔽していない)サンプルはビットを1つも立てない(bitSpan=0を許容する)
                uint startBit = min((uint)floor(saturate(theta01Min) * float(kSectorCount)), kSectorCount - 1u);
                uint bitSpan = (uint)ceil(saturate(theta01Max - theta01Min) * float(kSectorCount));
                bitSpan = min(bitSpan, kSectorCount - startBit);
                uint sampleMaskBits = (bitSpan >= kSectorCount) ? 0xFFFFFFFFu : (((1u << bitSpan) - 1u) << startBit);

                // このサンプルで新規に隠れたビット数を、サンプルが担う可視立体角の割合とみなす。
                // 既に(+側/-側どちらかの)手前のサンプルで隠れていた区間は二重にカウントしない
                // (=多重遮蔽物の裏に光が回り込む)
                uint newlySetBits = sampleMaskBits & ~sliceMask;
                uint newlySetCount = countbits(newlySetBits);
                if (newlySetCount > 0u)
                {
                    float3 sampleNormalWorld = normalize(Texture0.Sample(DefaultSampler, sampleUV).xyz * 2.0f - 1.0f);

                    float3 lightDirWorld = normalize(sampleWorldPos - worldPos);
                    float receiverNdotL = saturate(dot(normalWorld, lightDirWorld));
                    float emitterNdotL = saturate(dot(sampleNormalWorld, -lightDirWorld));

                    // サンプル地点の直接光(DirectLighting.hlslで計算済み、シャドウ適用済み)を
                    // このサンプルが反射・再放射する放射輝度とみなす(環境光は含めない)
                    float3 sampleRadiance = Texture2.Sample(DefaultSampler, sampleUV).rgb;

                    float weight = float(newlySetCount) / float(kSectorCount);
                    gi += sampleRadiance * weight * receiverNdotL * emitterNdotL;
                }

                sliceMask |= sampleMaskBits;
                if (sliceMask == 0xFFFFFFFFu)
                {
                    // このスライスの全セクタが既に埋まった: これ以上新しい区間は増えないため打ち切る
                    break;
                }
            }
        }

        ao += 1.0f - float(countbits(sliceMask)) / float(kSectorCount);
    }

    ao = saturate(ao / float(sliceCount));
    ao = pow(ao, aoPower);
    gi = saturate(gi / float(sliceCount) * intensity);

    return float4(gi, ao);
}
