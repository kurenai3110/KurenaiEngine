// 深度プリパス用のピクセルシェーダー。
//
// 【何のためのパスか】G-Bufferパスは1画素あたり6枚のレンダーターゲットへ書き、
// 6本のテクスチャを引く。奥のものから手前のものへ描いていくと、あとで隠れる画素にも
// そのすべてを払うことになる(オーバードロー)。先に深度だけを埋めてから
// 深度比較をGREATER_EQUAL(Reverse-Z)にしてG-Bufferを描けば、最前面の断片だけが
// テストを通り、隠れる断片は早期Zでピクセルシェーダーごと落ちる。
//
// G-Bufferパスの時間はほとんどがピクセルシェーダー側で、プリパスの追加コストは
// 残りの頂点処理ぶんにあたる。内訳の実測と、元が取れるオーバードローの倍率は
// docs/ImplementationHistory.md 41.22。
//
// 【不透明マテリアルにはピクセルシェーダーを使わない】深度だけを書けばよいので、
// パイプラインのピクセルシェーダーをnullptrにして段ごと省く(KurenaiEngine3D側)。
// このファイルのエントリポイントは、アルファカットアウト(glTFのalphaMode=MASK)の
// メッシュだけが使う ―― 切り抜かれる部分の深度まで書いてしまうと、
// G-Bufferパス側のclipで穴が開いたまま「深度は手前にある」という矛盾した状態になり、
// 背景が抜けて見える
#include "GBufferCommon.hlsli"

void PSMainCutout(PSInput input)
{
    // 判定はGBuffer.hlslのPSMainと同一でなければならない。ここで通した断片が
    // 向こうで捨てられる(あるいはその逆)と、深度と実際の書き込みが食い違う。
    // マテリアルの読み出し方も向こうと同じ関数を通す ―― 1モデル1ドローの経路では
    // マテリアルテーブルから、従来経路では定数バッファから読む
    const GpuMaterial material = LoadSurfaceMaterial(input.MaterialIndex);
    float4 baseColorSample =
        SampleMaterialTexture(material.BaseColorTextureIndex, BaseColorTexture, input.UV)
        * material.BaseColorFactor;
    clip(baseColorSample.a - material.AlphaCutoff);

    // 【モデルLODの切り替え中もこのシェーダーを通す】アルファカットアウトが無い
    // マテリアル(AlphaCutoff<=0)でも、クロスディザで捨てる画素があるなら
    // 深度を書いてはいけない。C++側はcutoutだけでなく「フェード中」でもこのPSO を選ぶ。
    // GBuffer.hlslのPSMainとまったく同じ呼び出しであること
    ApplyLODDither(input.Position.xy);
}
