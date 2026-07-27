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
//
// エンジンが用意しているセット(KurenaiEngine3D::CreateSamplerSetsの2つ):
//
//   セット                s0 MaterialSampler        s1 ColorSampler   s2 DataSampler
//   MaterialSamplers      Anisotropic 16x + Wrap    Linear + Clamp    Point + Clamp
//   ScreenSpaceSamplers   Linear + Clamp            Linear + Clamp    Point + Clamp
//
// MaterialSamplersはG-Buffer・半透明フォワード・スプライト・IBL畳み込みが、
// ScreenSpaceSamplersはそれ以外のフルスクリーンパスが使う。
// s0の実体がセットで異なるのが要点で、スクリーン空間パスにはWrapのサンプラーが1つもバインドされない
// (=画面端でUVが反対側へ回り込む不具合が構造的に起きない)。
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

#endif // KURENAI_SAMPLERS_HLSLI
