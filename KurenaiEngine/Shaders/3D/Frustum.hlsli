// 視錐台平面の取り出しと、球・AABBとの交差判定。
//
// 【なぜ1箇所に集めるのか】平面を行から作るか列から作るかは、過去に取り違えて
// 「真下を向いたときだけ100%誤検出する」壊れ方をした箇所である(実装史39章)。
// しかもYawを振る対照実験は素通りしてしまうため、絵を見ているだけでは気づけない。
// 取り出しの実装が2つあると、片方だけが壊れたまま残る。

#ifndef KURENAI_FRUSTUM_HLSLI
#define KURENAI_FRUSTUM_HLSLI

// ViewProjから視錐台の6平面を取り出す。平面は正規化済みで、
// 「dot(plane.xyz, p) + plane.w >= 0 なら内側」の向きに揃えてある。
//
// このエンジンはmul(vector, matrix)の規約なので
// clip.x = dot(v, ViewProjの0列目)、clip.w = dot(v, ViewProjの3列目) になる。
// クリップ空間の条件 -w <= x <= w、-w <= y <= w、0 <= z <= w をそれぞれ
// 「dot(平面, v) >= 0」の形に直したものが6枚の平面。
//
// 【行ではなく列から作ること】ここが取り違えの本体。
//
// 【Reverse-Zでもこのままでよい】近平面と遠平面の意味は入れ替わるが、
// 0 <= z <= w という条件自体は変わらないため、平面の式は同じで済む。
//
// 【正射影でもそのまま使える】シャドウのカスケードは平行光の正射影だが、
// クリップ空間の条件は透視投影と同じなのでこの導出がそのまま当てはまる。
//
// 退化した平面(長さ0)は法線を0にして返す。判定側は「常に内側」として扱うこと ――
// 射影行列が壊れているときにカリングで全部消すより、間引かないほうが安全
void ExtractFrustumPlanes(float4x4 viewProj, out float4 outPlanes[6])
{
    // ViewProjの列ベクトル(HLSLのfloat4x4は行優先の添字なので、列は_mXY表記で取り出す)
    const float4 col0 = float4(viewProj._m00, viewProj._m10, viewProj._m20, viewProj._m30);
    const float4 col1 = float4(viewProj._m01, viewProj._m11, viewProj._m21, viewProj._m31);
    const float4 col2 = float4(viewProj._m02, viewProj._m12, viewProj._m22, viewProj._m32);
    const float4 col3 = float4(viewProj._m03, viewProj._m13, viewProj._m23, viewProj._m33);

    float4 planes[6];
    planes[0] = col3 + col0; // 左   (x >= -w)
    planes[1] = col3 - col0; // 右   (x <=  w)
    planes[2] = col3 + col1; // 下   (y >= -w)
    planes[3] = col3 - col1; // 上   (y <=  w)
    planes[4] = col2;        // 手前 (z >=  0)
    planes[5] = col3 - col2; // 奥   (z <=  w)

    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        // 正規化しないと「距離」の尺度が平面ごとに変わり、半径と比較できない
        const float length3 = length(planes[i].xyz);
        outPlanes[i] = (length3 > 0.0f) ? (planes[i] / length3) : float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
}

// バウンディング球(ワールド空間)が視錐台と交差するか。
//
// 【TAAのジッターは無視してよい】ViewProjにはサブピクセルのジッターが乗っているが、
// ずれはピクセル単位以下で、バウンディング球という保守的な近似の余裕に埋もれる
bool SphereInFrustumPlanes(float4 planes[6], float3 center, float radius)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        if (dot(planes[i].xyz, center) + planes[i].w < -radius)
        {
            return false;
        }
    }
    return true;
}

// ワールド空間の軸並行バウンディングボックスが視錐台と交わるか。
// 「完全に外」と確定できたときだけfalseを返す保守的な判定(偽陽性は出るが偽陰性は出ない)。
//
// 各平面について、平面の法線方向へ最も進んだ頂点(p-vertex)だけを見る。
// それが平面の裏側にあるなら、AABBの8頂点すべてが裏側にあることになる。
//
// **C++側の IsAABBVisible(KurenaiEngine3D.cpp)と同じ判定にすること。**
// 片方だけで間引くと、CPUが描いたものとGPUが数えたものが食い違う
bool AabbInFrustumPlanes(float4 planes[6], float3 boundsMin, float3 boundsMax)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        const float3 p = float3(
            (planes[i].x >= 0.0f) ? boundsMax.x : boundsMin.x,
            (planes[i].y >= 0.0f) ? boundsMax.y : boundsMin.y,
            (planes[i].z >= 0.0f) ? boundsMax.z : boundsMin.z);

        if (dot(planes[i].xyz, p) + planes[i].w < 0.0f)
        {
            return false;
        }
    }
    return true;
}

#endif // KURENAI_FRUSTUM_HLSLI
