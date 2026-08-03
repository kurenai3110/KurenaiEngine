// 全シェーダ共通のサンプラー宣言。NormalEncoding.hlsli・SpecularEnergy.hlsliに次ぐ3つ目の共有ヘッダー。
//
// サンプラーは「役割」で宣言し、実体(フィルタ・アドレスモード)はパスごとにエンジン側が選ぶ。
// エンジンはパスの先頭でサンプラーセット(RHI::IRHISamplerSet)を1つバインドし、
// そのセットのi番目がここのs(i)になる。
//
//   register  役割              入るもの
//   s0        MaterialSampler   タイリングするマテリアルテクスチャ、キューブマップ、スプライト
//   s1        ColorSampler      色バッファ(SceneColor / AO / Present)、BRDF積分LUT
//   s2        DataSampler       補間してはいけないデータ(深度、エンコード法線、metallic/roughness、シャドウマップ)
//   s3        VolumeSampler     ワールド空間でタイリングするボリュームテクスチャ(3Dノイズ)
//
// エンジンが用意しているセット(KurenaiEngine3D::CreateSamplerSetsの2つ):
//
//   セット                s0 MaterialSampler        s1 ColorSampler   s2 DataSampler   s3 VolumeSampler
//   MaterialSamplers      Anisotropic 16x + Wrap    Linear + Clamp    Point + Clamp    Linear + Wrap
//   ScreenSpaceSamplers   Linear + Clamp            Linear + Clamp    Point + Clamp    Linear + Wrap
//
// MaterialSamplersはG-Buffer・半透明フォワード・スプライト・IBL畳み込みが、
// ScreenSpaceSamplersはそれ以外のフルスクリーンパスが使う。
// s0の実体がセットで異なるのが要点で、スクリーン空間パスにはWrapのサンプラーが1つもバインドされない
// (=画面端でUVが反対側へ回り込む不具合が構造的に起きない)。**s3だけはこの原則の例外**で、
// 両方のセットにWrapが入る。理由と安全性の根拠は下のVolumeSamplerの宣言に書いてある。
//
// なぜサンプラーを1個ずつ差し替えるのではなくセット単位なのか、という設計理由は
// Source/Library/RHI/IRHISamplerSet.h に書いてある(DX12のディスクリプタヒープ上書き問題)。

#ifndef KURENAI_SAMPLERS_HLSLI
#define KURENAI_SAMPLERS_HLSLI

// タイリングするマテリアルテクスチャ用。UVが[0,1]の外へ出ることを前提とした繰り返しと、
// 浅い角度で見る床・路面のボケを抑える異方性フィルタリングが要る。
// キューブマップ(スカイボックス・IBL)もここを使う。TextureCubeはハードウェアが面をまたいで
// シームレスにフィルタするため、アドレスモードは実質的に効かない
SamplerState MaterialSampler : register(s0);

// 色バッファ・ルックアップテーブル用。バイリニア補間したい一方で、UVの端が定義域の端なので
// Wrapで引いてはいけないもの。
//
// 特にBRDF積分LUTは、この区別を怠って実際に不具合を出した箇所である。LUTはUVそのものが
// 定義域(u = NdotV、v = roughness、どちらも[0,1])なので、Wrapで引くと u→1(視線が法線と一致する
// 面の中央)でバイリニアのタップが u≈0(グレージング角)のテクセルへ回り込み、まったく別のEssが混ざる。
// White Furnace Testの球の中心に数ピクセルの斑点として現れた(当時は異方性フィルタも併用しており、
// 画面空間の勾配のぶんだけ回り込みが広がっていた)。v(roughness)側も同様で、
// roughness=0の面にroughness≈1の値が混ざる。詳細はdocs/Architecture.html 14.2.1節
SamplerState ColorSampler : register(s1);

// 深度・オクタヘドラルエンコードされた法線・metallic/roughness・シャドウマップ用。
// これらは「テクセルに格納された値そのもの」に意味があり、バイリニア補間すると
// シルエットを跨いだタップが実在しない中間値を作る。そこから再構成したワールド座標や法線は
// どのジオメトリにも対応しない偽の値になる(参照実装のXeGTAO/Bevyが深度・法線に
// ポイントサンプラーを使うのと同じ理由)。
// シャドウマップも同様で、深度を補間してから比較するとPCF本来の「比較してから平均」と順序が逆になり、
// ブロッカー平均深度がシルエット跨ぎで実在しない値になる。
// アドレスモードがClampなのは、PCFのタップが[0,1]をはみ出したときに
// シャドウマップの反対側の端を読んでカスケード境界へ偽の影・光漏れを作らないため
SamplerState DataSampler : register(s2);

// ワールド空間でタイリングするボリュームテクスチャ(3Dノイズ)用。Linear + Wrap。
//
// 【なぜスクリーン空間のセットにもWrapを入れてよいのか】上の役割表のとおり、この宣言は
// 「スクリーン空間パスにはWrapを1つも置かない」という原則の唯一の例外である。原則が防いでいたのは
// 「画面UV(0〜1が画面の端)をWrapで引いて反対側の端へ回り込む」不具合だが、このサンプラーで引くのは
// 画面UVではなく**ワールド空間の3D座標から作ったUVW**であり、そもそも回り込む「反対側の画面端」が
// 存在しない。役割で分ける方式を採っている以上、役割そのものが違えば同じセットに同居してよい。
//
// 【なぜWrapでなければならないのか】ボリュームノイズは有限のテクスチャを無限にタイリングして
// 使う。Clampで引くと周期の境界でトライリニア補間のタップが端のテクセルに張り付き、
// 一定間隔で継ぎ目が出る。**シェーダー側でfrac(uvw)してもこの継ぎ目は消せない** ——
// u=0.999のテクセルは本来u=0.0のテクセルと補間されるべきだが、Clampでは端のテクセルと
// 補間されてしまい、補間そのものがテクスチャの端を跨げないため。
//
// 異方性フィルタではなくLinearなのは、ボリュームをレイマーチで等方的に刻んで引くため
// 画面空間の勾配に沿った異方性が意味を持たないから(かつ3Dの異方性フィルタは高価)
SamplerState VolumeSampler : register(s3);

#endif // KURENAI_SAMPLERS_HLSLI
