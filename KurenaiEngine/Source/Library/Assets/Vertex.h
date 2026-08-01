#pragma once

namespace Kurenai::Assets
{
    struct Vertex
    {
        float Position[3];
        float Normal[3];
        // マテリアルテクスチャ用のUV(TEXCOORD0)。ソースモデル由来。タイリングを前提に
        // 作られていることが多く、[0,1]の外へ出たりチャート同士が重なったりする
        float UV[2];
        // xyzは接線、wは従法線の向き(+1/-1)。法線マップ適用時のTBN行列構築に使う
        float Tangent[4];
        // ライトマップUV(TEXCOORD1)。遮蔽マップ(ベイク済みAO)のサンプリング専用。
        // KurenaiPackerが--bake-occlusion指定時にxatlasで生成し、その場合はメッシュ全体で
        // 重なりが無く[0,1]に収まることが保証される(docs/Architecture.html 22章)。
        //
        // 【ベイクしない場合はUVと同じ値が入る】glTFのocclusionTextureのように元から
        // 遮蔽マップを持つアセットは、その画像がTEXCOORD0の空間で作られている。UV1を0で
        // 埋めてしまうとそれらが引けなくなるため、既定ではUVをそのまま複製する。
        // これによりシェーダー側は「遮蔽マップは常にUV1で引く」と1通りに書ける
        //
        // 【UVを使い回さない理由】TEXCOORD0はタイリング前提のUVであり、そこへAOを焼くと
        // 一点の遮蔽がタイル全面に現れて破綻する。実測ではSponzaの25メッシュ中14が[0,1]を
        // はみ出し、残りのうち10はUV三角形の面積の総和が1.0を超えていて(最大65.9)、
        // 鳩の巣原理でチャートが必ず重なっていた
        float UV1[2];
    };
}
