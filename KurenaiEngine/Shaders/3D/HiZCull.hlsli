// Hi-Zによるオクルージョンカリングの判定。増幅シェーダー(メッシュレット単位)と
// コンピュートシェーダー(モデル単位)の両方から呼ぶ。
//
// 【なぜ共有するのか】判定の向きはReverse-Zで決まり、逆に書いてもコンパイルは通り
// 絵もそれらしく出る。実装を2つに増やすと、片方だけが「間引きすぎる」あるいは
// 「一度も間引かない」状態のまま気づけない(Meshlet.hlsliのMeshletSphereInFrustumを
// 共有しているのと同じ理由。実装史39章の視錐台平面の取り違えを参照)。

#ifndef KURENAI_HIZ_CULL_HLSLI
#define KURENAI_HIZ_CULL_HLSLI

// ワールド空間のAABBが、Hi-Zに対して完全に隠れているか。
//
// 【判定の向きはReverse-Zで決まる】このエンジンはReverse-Z(近平面 NDC z=1.0 /
// 遠平面 z=0.0、深度比較はGREATER)で、HiZ.hlslはミップを2x2の**最小値**で縮約している。
// つまり
//
//     Hi-Zの1テクセル = そのブロック内で「最も遠い」可視サーフェスの深度
//
// なので、AABBの最も手前の点(=NDC zが最大の点)ですらその値より奥(小さい)なら、
// ブロック内のどの画素から見てもAABBは隠れている:
//
//     遮蔽されている ⟺ AABBのmaxNdcZ < カバーするテクセルのHi-Zのmin
//
// 保守側(間引きすぎない側)はHi-Zの値を小さく取る方向なので、複数テクセルをまとめるときも
// minで正しい。maxを取ると「本当は見えているものを消す」側へ倒れる。
//
// 【viewProjには、そのHi-Zの元になった深度を描いた行列を渡すこと】
// Hi-Zの構築パスはG-Bufferパスより後に登録されているため、描画中に読めるのは
// 前フレームのチェーンになる。その場合に渡すのはFrameConstants::PrevViewProjで、
// 今フレームのViewProjではない。呼び出し側が取り違えると、ずれた位置のHi-Zと
// 深度を比べることになる。
//
// 【呼び出し側が保守的に膨らませてから渡すこと】1フレーム遅れの視差ずれは
// この関数では吸収しない(膨張量の決め方は用途で違うため)。
bool IsAabbOccludedByHiZ(
    Texture2D<float> hiZTexture, float4x4 viewProj,
    float3 aabbMin, float3 aabbMax,
    float2 hiZSize, uint mipCount)
{
    if (mipCount == 0)
    {
        return false;
    }

    // AABBの8頂点をクリップ空間へ運ぶ。
    // 球ではなくAABBで扱うのは、透視投影で球の輪郭が楕円になり、保守的な画面矩形を
    // 閉じた式で出すのが面倒なため。球を包むAABBは球より大きいので必ず保守側に倒れる
    float2 ndcMin = float2(1e30f, 1e30f);
    float2 ndcMax = float2(-1e30f, -1e30f);
    float maxNdcZ = -1e30f;

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        const float3 corner = float3(
            (i & 1) ? aabbMax.x : aabbMin.x,
            (i & 2) ? aabbMax.y : aabbMin.y,
            (i & 4) ? aabbMax.z : aabbMin.z);

        const float4 clip = mul(float4(corner, 1.0f), viewProj);

        // 【wが0以下の頂点が1つでもあれば判定を諦めて通す】カメラの後ろ、あるいは
        // 近平面をまたぐAABBでは、w除算が符号を反転させて画面矩形が裏返る。
        // そのまま進めると「足元の巨大なタイルが丸ごと消える」という壊れ方をする。
        // ここは間引かない側へ倒すのが常に安全
        if (clip.w <= 0.0f)
        {
            return false;
        }

        const float3 ndc = clip.xyz / clip.w;
        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        // Reverse-Zなのでzが大きいほど手前。「最も手前の点」を取る
        maxNdcZ = max(maxNdcZ, ndc.z);
    }

    // NDC(x,y ∈ [-1,1]、yは上が+1)からUV(y は下が+1)へ。**ここでYの符号を反転する。**
    // 反転を忘れると上下が入れ替わったブロックのHi-Zと比べることになり、
    // 「空を見上げているのに間引き率だけは出る」というもっともらしい壊れ方をする
    const float2 uvMin = float2(ndcMin.x * 0.5f + 0.5f, -ndcMax.y * 0.5f + 0.5f);
    const float2 uvMax = float2(ndcMax.x * 0.5f + 0.5f, -ndcMin.y * 0.5f + 0.5f);

    // 【画面からはみ出していたら判定を諦めて通す】
    // 視錐台判定は**今フレームの**行列で行うのに対し、こちらは(1フレーム遅れの場合)
    // 前フレームの行列で投影している。カメラが回った直後は「今フレームは画面内だが
    // 前フレームは画面外」という対象が画面の縁に必ず生まれ、そのUVは[0,1]の外へ出る。
    //
    // そこでUVを画面端へクランプすると、**まったく別の場所のHi-Zと深度を比べる**ことになり、
    // たまたまそこに手前の面があれば消える。カメラを振ったときだけ画面の縁が欠ける、という
    // 追いにくい壊れ方をするので、はみ出した時点で間引かない側へ倒す
    if (uvMin.x < 0.0f || uvMin.y < 0.0f || uvMax.x > 1.0f || uvMax.y > 1.0f)
    {
        return false;
    }

    const float2 texelMin = uvMin * hiZSize;
    const float2 texelMax = uvMax * hiZSize;

    // 矩形が高々2x2テクセルに収まる段を選ぶ。ceil(log2(辺の長さ))が
    // 「1テクセルの幅が辺の長さ以上になる最小の段」になる
    const float sizeInTexels = max(texelMax.x - texelMin.x, texelMax.y - texelMin.y);
    const uint mip = (uint)clamp(ceil(log2(max(sizeInTexels, 1.0f))), 0.0f, (float)(mipCount - 1));

    // 選んだ段でのテクセル座標。ミップNの解像度は floor(mip0 / 2^N)(1未満にはならない)で、
    // これはHiZ.hlslが1段ずつ半分にしていった結果ともD3Dのミップ寸法とも一致する
    // (floor(floor(x/2)/2) = floor(x/4) のため)
    const float2 mipSize = max(floor(hiZSize / (float)(1u << mip)), float2(1.0f, 1.0f));
    const int2 mipMaxCoord = (int2)mipSize - int2(1, 1);
    const int2 coordMin = clamp((int2)floor(uvMin * mipSize), int2(0, 0), mipMaxCoord);
    const int2 coordMax = clamp((int2)floor(uvMax * mipSize), int2(0, 0), mipMaxCoord);

    // 2x2を読んでminを取る。段の選び方から矩形はこの範囲に収まっているはずだが、
    // 端数の丸めで1テクセルはみ出しうるので、座標はクランプ済みのものを使う
    const float d00 = hiZTexture.Load(int3(coordMin.x, coordMin.y, mip));
    const float d10 = hiZTexture.Load(int3(coordMax.x, coordMin.y, mip));
    const float d01 = hiZTexture.Load(int3(coordMin.x, coordMax.y, mip));
    const float d11 = hiZTexture.Load(int3(coordMax.x, coordMax.y, mip));
    const float hiZMin = min(min(d00, d10), min(d01, d11));

    // 最も手前の点ですら、そのブロックで最も遠い可視面より奥なら隠れている
    return maxNdcZ < hiZMin;
}

#endif // KURENAI_HIZ_CULL_HLSLI
