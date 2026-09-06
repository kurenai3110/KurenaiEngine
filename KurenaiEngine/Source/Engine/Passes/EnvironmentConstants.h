#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

// 空・大気・IBL のシェーダーが読む cbuffer の C++ 側の写し(段階6)。
//
// 【なぜ独立したヘッダーなのか】パスの登録側(Passes/EnvironmentPasses.cpp)と、
// 定数バッファを作る側(KurenaiEngine3D.cpp の CreateSceneResources)の**両方**が
// sizeof で使う。どちらかの翻訳単位へ閉じ込めるともう片方が見られない。
//
// 【static_assert が守るのはC++側だけ】HLSL の宣言と突き合わせているわけではない。
// ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う。
// **通すために期待値を書き換えないこと。**
namespace Kurenai::Passes
{
        inline constexpr uint32_t kIBLIrradianceSize = 32;
        inline constexpr uint32_t kIBLPrefilterBaseSize = 128;
        // プリフィルタ済み鏡面マップのミップ数(128,64,32,16,8,4の6段)。ラフネス[0,1]を
        // [0, kIBLPrefilterMipLevels-1]のミップ番号へ線形マッピングする(DeferredLighting.hlsl参照)
        inline constexpr uint32_t kIBLPrefilterMipLevels = 6;
        inline constexpr uint32_t kIBLBRDFLUTSize = 128;
        // ボリュメトリック雲の3Dノイズの1辺のテクセル数。
        // Shapeは128^3のRGBA8で8MB、Detailは32^3のRGBA8で128KB。合わせて約8.1MB。
        // Shapeを128にしているのは、雲1つが画面上で数百画素に広がるため塊の形にはこの程度の
        // 解像度が要る一方、これ以上上げるとメモリが4倍(256^3で64MB)に跳ねるため。
        // Detailは縁を削るだけで低周波成分を持たないので32で足りる
        // 大気散乱のLUT(Hillaire 2020)。解像度は論文の推奨値。
        // Transmittanceは高度×視線天頂角、MultiScatteringは高度×太陽天頂角で、
        // どちらも大気パラメータだけで決まるためカメラにも時刻にも依存しない。
        // SkyViewは空そのもの(太陽の子午線からの方位×天頂角)で、太陽が動くと変わる。
        // **kSkyViewLUTWidth/Heightはシェーダ側(AtmosphereCommon.hlsliの
        // kSkyViewLUTWidthF/kSkyViewLUTHeightF)と一致させること** — UVの半テクセル補正に
        // 解像度が要るため、焼く側・引く側の両方が同じ値を知っている必要がある
        inline constexpr uint32_t kTransmittanceLUTWidth = 256;
        inline constexpr uint32_t kTransmittanceLUTHeight = 64;
        inline constexpr uint32_t kMultiScatteringLUTSize = 32;
        inline constexpr uint32_t kSkyViewLUTWidth = 192;
        inline constexpr uint32_t kSkyViewLUTHeight = 108;
        inline constexpr uint32_t kCloudShapeNoiseSize = 128;
        inline constexpr uint32_t kCloudDetailNoiseSize = 32;
        // ウェザーマップ(H3)。ノイズ空間の1周期(256セル=358km)を1枚で覆うので、
        // 4096なら88m/テクセル。**CloudNoiseGenerate.hlsl の kWeatherNoiseSize と同じ値であること**
        // (片方だけ変えるとテクセル中心がずれ、バイリニアが半テクセル分ぼける)。
        // R8G8B8A8で4096^2 = 67MB。解像度の実測はSky.hlsliのウェザーマップの節
        inline constexpr uint32_t kCloudWeatherNoiseSize = 4096;
        // 手続き空(SkyGenerate.hlsl): Perez分布をGPUで評価してキューブマップを生成する。
        // オフラインで焼いたDDS(Sky.dds)と違い、太陽が動くと空の輝度分布の「形」も追従する
        // (circumsolarの明るい領域が太陽と一緒に動く)。詳細はSkyGenerate.hlsl冒頭。
        //
        // .ksceneで[Scene]Skyboxを明示しているシーン(White Furnace TestのUniformWhite.dds)は
        // 従来どおりDDSを使う必要があるため、手続き空は別テクスチャに持ち、
        // ActiveSkyTexture()がフレームごとにどちらを使うか決める
        inline constexpr uint32_t kProceduralSkySize = 256;
        // 太陽がこの角度以上動いたらSkyView LUTを焼き直す。LUTは天頂方向180度を108テクセルで
        // 持つので1テクセルあたり約1.67度あり、その1/30以下しかずらさない値にしてある。
        inline constexpr float kSkyViewRebakeAngleDegrees = 0.05f;
        // CSProjectSHの射影に使う離散化解像度(1面の1辺のテクセル数)。
        // 【SourceSkyboxの実解像度とは無関係】スカイボックスはDDS(シーンごとに任意の解像度)や
        // 手続き空(256)など実行時に変わりうる一方、IRHITextureには解像度を問い合わせる手段が
        // 無いため、射影側は独立した固定解像度を持つ(IBLConvolve.hlslのSHProjectionSizeコメント参照)。
        // 64×64×6=24,576テクセルはCSIrradianceの約9,750万サンプルに対し十分密で、
        // 9個の係数を求めるだけの積分には(理論上は32でも足りる範囲)余裕を持たせた値
        inline constexpr uint32_t kSHProjectionSize = 64;

        // IBLConvolve.hlsl(CSIrradiance/CSPrefilter)へ、処理対象の面(キューブマップは面ごとに
        // 個別ディスパッチが必要)とCSPrefilterのみが使うラフネス値を渡す専用の定数バッファ
        struct alignas(16) IBLFaceConstants
        {
            uint32_t Face = 0;
            float Roughness = 0.0f;
            // SH経路用。IBLConvolve.hlslのIBLFaceConstantsコメント参照。
            // CSIrradiance/CSPrefilterはどちらも参照しないため、設定しなくても既定の値初期化(0)で動く
            float SHWindowLambda = 0.0f;
            float SHProjectionSize = 0.0f;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(IBLFaceConstants, Face) == 0, "Face のレイアウトが変わっている");
        static_assert(offsetof(IBLFaceConstants, Roughness) == 4, "Roughness のレイアウトが変わっている");
        static_assert(offsetof(IBLFaceConstants, SHWindowLambda) == 8, "SHWindowLambda のレイアウトが変わっている");
        static_assert(offsetof(IBLFaceConstants, SHProjectionSize) == 12, "SHProjectionSize のレイアウトが変わっている");
        static_assert(sizeof(IBLFaceConstants) == 16, "IBLFaceConstants の総サイズが変わっている");

        // SkyIntegrate.hlsl側のcbuffer SkyIntegrateConstantsと一致させる必要がある
        struct alignas(16) SkyIntegrateConstants
        {
            // xyz=太陽が「ある」向き(正規化済み。光が進む向きとは符号が逆)、w=未使用
            DirectX::XMFLOAT4 SunDirection;
            // x=目標照度[lx](SunLighting::SkyIlluminanceLux)、y=実効プリ露出(effectiveExposure)、
            // z=タービディティ(m_SkySettings.Turbidity)、w=空の彩度(m_SkySettings.Saturation)
            DirectX::XMFLOAT4 IntegrateParams;

            // --- 以下はP18の第2段(雲込みの空の照度)専用。**FrameConstantsの同名の枠と
            //     完全に同じ値を入れること**。食い違うと「背景に見えている雲」と
            //     「大気遠近が想定している雲」が別物になる ---
            DirectX::XMFLOAT4 CloudParams0;  // x=積雲の被覆率、y=雲底高度[m]、z=UVスケール、w=消散係数
            DirectX::XMFLOAT4 CloudParams1;  // xy=スクロール量、z=前方散乱g、w=厚み[m]
            DirectX::XMFLOAT4 CloudParams2;  // x=巻雲の被覆率、y=高度[m]、z=UVスケール、w=消散係数
            DirectX::XMFLOAT4 CloudParams3;  // xy=スクロール量、z=異方スケール、w=雲の種類の偏り
            DirectX::XMFLOAT4 FogParams0;    // x=消散係数[1/m]、y=スケールハイト[m]、z=基準高度[m]、w=有効フラグ
            // xyz=視点のワールド座標(レイの起点)、w=太陽照度/空照度比
            DirectX::XMFLOAT4 ViewerAndSunRatio;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(SkyIntegrateConstants, SunDirection) == 0, "SunDirection のレイアウトが変わっている");
        static_assert(offsetof(SkyIntegrateConstants, IntegrateParams) == 16, "IntegrateParams のレイアウトが変わっている");
        static_assert(offsetof(SkyIntegrateConstants, CloudParams0) == 32, "CloudParams0 のレイアウトが変わっている");
        static_assert(offsetof(SkyIntegrateConstants, CloudParams1) == 48, "CloudParams1 のレイアウトが変わっている");
        static_assert(offsetof(SkyIntegrateConstants, CloudParams2) == 64, "CloudParams2 のレイアウトが変わっている");
        static_assert(offsetof(SkyIntegrateConstants, CloudParams3) == 80, "CloudParams3 のレイアウトが変わっている");
        static_assert(offsetof(SkyIntegrateConstants, FogParams0) == 96, "FogParams0 のレイアウトが変わっている");
        static_assert(offsetof(SkyIntegrateConstants, ViewerAndSunRatio) == 112, "ViewerAndSunRatio のレイアウトが変わっている");
        static_assert(sizeof(SkyIntegrateConstants) == 128, "SkyIntegrateConstants の総サイズが変わっている");

        // AtmosphereLUT.hlsl側のcbuffer AtmosphereConstantsと一致させる必要がある。
        // 3つのエントリポイント(Transmittance/MultiScattering/SkyView)が共通で読む
        struct alignas(16) AtmosphereConstants
        {
            // xyz=太陽が「ある」向き(正規化済み)、w=未使用。CSSkyViewのみが使う
            DirectX::XMFLOAT4 SunDirection;
            // x=Mie(エアロゾル)密度の倍率(濁りのスライダー由来)、yzw=未使用
            DirectX::XMFLOAT4 Params0;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(AtmosphereConstants, SunDirection) == 0, "SunDirection のレイアウトが変わっている");
        static_assert(offsetof(AtmosphereConstants, Params0) == 16, "Params0 のレイアウトが変わっている");
        static_assert(sizeof(AtmosphereConstants) == 32, "AtmosphereConstants の総サイズが変わっている");

        // SkyGenerate.hlsl側のcbuffer SkyBakeConstantsと一致させる必要がある
        struct alignas(16) SkyBakeConstants
        {
            // 処理対象の面(D3D標準順: +X=0,-X=1,+Y=2,-Y=3,+Z=4,-Z=5)
            uint32_t Face;
            // 雲(判断B)による平均透過率。SkyParametersBuffer[0].Luminance.x
            // (雲を考慮しない晴天基準の天頂輝度)にこの値を掛けてからキューブへ焼く
            float CloudTransmittance;
            float Padding0[2];
            // 太陽が「ある」向き(正規化済み。光が進む向きとは符号が逆)
            DirectX::XMFLOAT4 SunDirection;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(SkyBakeConstants, Face) == 0, "Face のレイアウトが変わっている");
        static_assert(offsetof(SkyBakeConstants, CloudTransmittance) == 4, "CloudTransmittance のレイアウトが変わっている");
        static_assert(offsetof(SkyBakeConstants, Padding0) == 8, "Padding0 のレイアウトが変わっている");
        static_assert(offsetof(SkyBakeConstants, SunDirection) == 16, "SunDirection のレイアウトが変わっている");
        static_assert(sizeof(SkyBakeConstants) == 32, "SkyBakeConstants の総サイズが変わっている");
}
