// 深度プリパス用のピクセルシェーダー。
//
// 【何のためのパスか】G-Bufferパスは1画素あたり6枚のレンダーターゲットへ書き、
// 6本のテクスチャを引く。奥のものから手前のものへ描いていくと、あとで隠れる画素にも
// そのすべてを払うことになる(オーバードロー)。先に深度だけを埋めてから
// 深度比較をGREATER_EQUAL(Reverse-Z)にしてG-Bufferを描けば、最前面の断片だけが
// テストを通り、隠れる断片は早期Zでピクセルシェーダーごと落ちる。
//
// 実測(Sponza / 1280x720 / DX11 / Intel UHD Graphics 620)で、G-Bufferパスの
// 内訳は「頂点処理 + ラスタライズ + ドロー発行」が12%、残る88%がピクセルシェーダー側だった
// (同じ描画を深度が埋まった状態でもう一度登録し、全画素が早期Zで落ちるときの時間を測った)。
// プリパスの追加コストはその12%ぶんなので、オーバードローが1.17倍を超えていれば元が取れる。
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
    // 向こうで捨てられる(あるいはその逆)と、深度と実際の書き込みが食い違う
    float4 baseColorSample = BaseColorTexture.Sample(MaterialSampler, input.UV) * BaseColorFactor;
    clip(baseColorSample.a - AlphaCutoff);

    // 【モデルLODの切り替え中もこのシェーダーを通す】アルファカットアウトが無い
    // マテリアル(AlphaCutoff<=0)でも、クロスディザで捨てる画素があるなら
    // 深度を書いてはいけない。C++側はcutoutだけでなく「フェード中」でもこのPSO を選ぶ。
    // GBuffer.hlslのPSMainとまったく同じ呼び出しであること
    ApplyLODDither(input.Position.xy);
}
