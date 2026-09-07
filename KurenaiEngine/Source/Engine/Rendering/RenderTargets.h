#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <DirectXMath.h>

#include "RHI/IRHIDevice.h"

namespace Kurenai::Rendering
{
    // 複数のパス群が読み書きする共有レンダーターゲット。
    // 【なぜ群へ渡さないか】G-Buffer は8群が触る。どれか1つの群に持たせると、
    // 他の群がその群を経由して取りに行くことになり、依存が増える。ここが唯一の持ち主。
    struct RenderTargets
    {
        std::unique_ptr<RHI::IRHITexture> GBufferAlbedo;
        std::unique_ptr<RHI::IRHITexture> GBufferNormal;
        std::unique_ptr<RHI::IRHITexture> GBufferMaterial;
        // 自発光(エミッシブ)。AO/シャドウの影響を受けずライティングパスで常に加算される
        std::unique_ptr<RHI::IRHITexture> GBufferEmissive;
        std::unique_ptr<RHI::IRHITexture> GBufferDepth;
        // モーションベクター(速度バッファ)。「この画素に映っているものが前フレームでは画面の
        // どこにいたか」をUV単位の2Dベクトルで持ち、TAAが履歴を引く位置の決定に使う。
        // 現在のシーンは全インスタンスが静的(ModelInstance::Worldは読み込み時に確定し以降
        // 変わらない)なので、速度の発生源はカメラの移動・回転だけである。そのためGBuffer.hlslは
        // 同じワールド座標を今フレームと前フレームのビュー射影行列で投影して差を取るだけでよく、
        // インスタンスごとの前フレームのワールド行列(PrevWorld)を持つ必要がない。
        // 動的オブジェクトを入れる際はObjectConstantsへPrevWorldを追加すること
        std::unique_ptr<RHI::IRHITexture> GBufferVelocity;
        // bent normal(ワールド空間の正規化しない可視方向の平均)。.rgb = bRaw、.a = 有効フラグ
        std::unique_ptr<RHI::IRHITexture> GBufferBentNormal;
        // RTシャドウの可視率(0〜1のスカラー)。RWTexture2D<float>として書くため単チャンネルの
        // R32_Floatにする(型付きUAVの読み書きが保証されているのはR32系のみ。AutoExposure.hlsl参照)
        std::unique_ptr<RHI::IRHITexture> RTShadowTexture;
        // RT反射の出力。コンピュートシェーダーがUAVで書くためレンダーターゲットではなく
        // UAVテクスチャで作る。後段(Tonemap)から見るとSSRTextureと入れ替え可能なバッファで、
        // どちらを渡すかはKurenaiEngine3D::GetActiveReflectionOutputが決める。
        // RTShadowTextureと同じくDXR対応環境でだけ確保される
        std::unique_ptr<RHI::IRHITexture> RTReflectionTexture;

        // 全カスケードの深度を1つのTexture2DArray(スライス番号=カスケード番号)として保持する。
        // 書き込みはスライスごとの個別DSV(RenderGraphPassDesc::DepthTargetArraySlice)で行い、
        // 読み取りは配列全体を指す1本のSRV(t4)を1回バインドするだけでよい。シェーダ側は
        // ShadowMapArray.Sample(DataSampler, float3(uv, cascadeIndex))で動的にカスケードを選べる
        // (ShadowSampling.hlsli参照)。ウィンドウ/レンダー解像度に依存しないため一度だけ作成し、
        // 解像度変更時に作り直す他のメンバとは生成契機が異なる。
        std::unique_ptr<RHI::IRHITexture> ShadowCascadeArray;

        // 直接光(シャドウ適用済みのPBR直接光をHDRで持つ)。DeferredLightingパスと
        // SSIL_VisibilityBitmask.hlslの両方が読むため、G-Bufferと同じレンダー解像度で保つ
        std::unique_ptr<RHI::IRHITexture> DirectLightTexture;
        // AO/GIの生バッファとブラー後。フォーマットはどちらもGetAOFormat()に従う
        // (バッファ精度の設定に追従する)
        std::unique_ptr<RHI::IRHITexture> SSAORawTexture;
        std::unique_ptr<RHI::IRHITexture> SSAOTexture;
        std::unique_ptr<RHI::IRHITexture> SSILRawTexture;
        std::unique_ptr<RHI::IRHITexture> SSILTexture;
        // ライティングパスの出力。トーンマッピング前のHDR値をそのまま持つ
        std::unique_ptr<RHI::IRHITexture> SceneColor;
        // SSRの出力。後段(Tonemap)から見るとSceneColorと入れ替え可能なバッファになる
        std::unique_ptr<RHI::IRHITexture> SSRTexture;
        // Tonemapの出力(LDR)。内部レンダー解像度で、超解像の出力とは作り直す契機が違う
        std::unique_ptr<RHI::IRHITexture> TonemapTexture;
        // TAAの履歴2枚。読みながら同じテクスチャへ書けないので毎フレーム役割を入れ替える
        // (どちらが今フレームの書き込み先かはKurenaiEngine3D::m_TAAHistoryIndexが持つ)
        std::unique_ptr<RHI::IRHITexture> TAAHistory[2];
        // 階層深度。ミップ段数はKurenaiEngine3D側が決めてCreateHiZへ渡す
        std::unique_ptr<RHI::IRHITexture> HiZTexture;

        // 自前ソフトウェアラスタライザ(46章)の出力3枚。
        // 【なぜここが持つか】書くのはGeometryPassesだけだが、PresentPassのデバッグ表示
        // (Mode 4/5/7)が読む。群に持たせると群間の依存になるため、共有の持ち主をここに置く。
        // フォーマットはハードウェア側と揃えてある ―― 色はHDR、深度は生値、法線は
        // GBufferNormalと同じR16G16_Floatのオクタヘドラル符号化。揃えていないと差分が取れない
        std::unique_ptr<RHI::IRHITexture> SoftwareRasterColor;
        std::unique_ptr<RHI::IRHITexture> SoftwareRasterDepth;
        std::unique_ptr<RHI::IRHITexture> SoftwareRasterNormal;

        // 平面反射の専用レンダーターゲット2枚。書くのはReflectionPassesだけだが、
        // PresentPassのデバッグ表示(DebugView::PlanarReflection)が色と実寸を読む。
        // 解像度はレンダー解像度 × PlanarResolutionScaleで、他のメンバとは作り直す契機が違う
        // (KurenaiEngine3D::CreatePlanarReflectionTargetsが単独で呼ぶ)。
        // SceneColorと同じHDR形式で固定 ―― 水面はラフネスが低く反射がそのまま見えるため、
        // CreateRenderTargetsのLegacy8bitフォールバックの対象外にしてある
        std::unique_ptr<RHI::IRHITexture> PlanarReflectionColor;
        std::unique_ptr<RHI::IRHITexture> PlanarReflectionDepth;
        // 上2枚の実寸。デバッグ表示(Present.hlslのレターボックス計算)が実寸を必要とする
        uint32_t PlanarReflectionWidth = 0;
        uint32_t PlanarReflectionHeight = 0;

        // ブルームのピラミッド。第0段が半解像度で、以降1段ごとに半分になる。
        // ピラミッドをミップチェーン1枚ではなくレベルごとの独立テクスチャで持っているのは、
        // 同一リソースのSRV/UAV同時バインドを避けるため(理由の詳細はBloom.hlsl冒頭)。
        // BloomDownがダウンサンプル結果、BloomUpがアップサンプルの累積で、
        // 最終的にBloomUp[0](半解像度)をTonemapパスが読む。
        // 【なぜここが持つか】書くのはPostProcessPassesだけだが、PresentPassのデバッグ表示が
        // BloomUp[0]とBloomLevelSizes[0]を読む
        std::vector<std::unique_ptr<RHI::IRHITexture>> BloomDownTextures;
        std::vector<std::unique_ptr<RHI::IRHITexture>> BloomUpTextures;
        // 各段の解像度。内部解像度から決まる
        std::vector<DirectX::XMUINT2> BloomLevelSizes;

        // 超解像(Upscale.hlsl)の出力2枚。どちらも**出力解像度**で、内部レンダー解像度で作る
        // TonemapTextureとは作り直す契機が違う。分けているのはRCASがEASUの結果を読むためで、
        // 同一リソースのSRV/UAV同時バインドを避ける。
        // 【なぜここが持つか】書くのはPostProcessPassesだけだが、RCASの出力と実寸を
        // PresentPassが読む(超解像が有効なフレームは、これがそのまま最終画になる)
        std::unique_ptr<RHI::IRHITexture> UpscaleTexture;      // EASUの出力
        std::unique_ptr<RHI::IRHITexture> UpscaleSharpTexture; // RCASの出力(Presentが読む)
        // 実際に確保済みの出力解像度用テクスチャのサイズ。0なら未確保(超解像が無効)
        uint32_t UpscaleTargetWidth = 0;
        uint32_t UpscaleTargetHeight = 0;

        // タイルライトカリングのライトグリッド(BufferUsage::StructuredRW)。コンピュートがUAVで書き、
        // 直接光パスのピクセルシェーダがSRVで読む。タイル数は解像度に依存する。
        // 【なぜここが持つか】書くのはMegaLightsPassesだが、LightingPassesが直接光で読み、
        // PresentPassのライトグリッド表示(Mode 11)も読む。3群にまたがる
        std::unique_ptr<RHI::IRHIBuffer> LightTileBuffer;
        uint32_t LightTileCountX = 0;
        uint32_t LightTileCountY = 0;
        // MegaLightsの候補プール。タイルの切り方はライトグリッドと同じで、1タイルあたりの
        // 要素数だけが違う。非対応環境ではパス自体が走らないので確保しない(nullptrのまま)
        std::unique_ptr<RHI::IRHIBuffer> MegaLightsTilePoolBuffer;

        // MegaLightsの生出力。R32G32B32A32_Floatで確保する ―― 物差し自体が系統的に
        // 暗い側へ寄っていると、確率的サンプリングのバイアス検査が汚染されるため
        // (fp16との実測差はKurenaiEngine3D::CreateRenderTargetsの当該箇所を参照)。
        // 【なぜここが持つか】書くのはMegaLightsPassesだが、直接光パスがt7で読み、
        // PresentPassのデバッグ表示も読む
        std::unique_ptr<RHI::IRHITexture> MegaLightsTexture;
        // 復調を戻したデノイズ後の最終出力。DirectLightingはこれをt7で読む
        std::unique_ptr<RHI::IRHITexture> MegaLightsDenoisedTexture;
        // 蓄積バッファ(計測専用)。1画素につきfloat4。
        // 非対応環境でも、Presentがt6へ張るための1要素のダミーとして必ず作る
        // (DX12はSetPipelineStateのたびにルート引数が無効化されるため、シェーダが
        // 宣言しているリソースを未バインドのままDrawできない)
        std::unique_ptr<RHI::IRHIBuffer> MegaLightsAccumBuffer;

        // G-Buffer の生成は元の位置ごとに3つへ分ける。間に他のテクスチャ生成があるため、
        // 順序を変えるとDX12のディスクリプタ枠の割り当て順が変わり、意味の無い差分になる。
        // 呼び出し元のtry内から呼ぶこと。確保失敗時のHDR→Legacy8bitフォールバックは
        // KurenaiEngine3D::CreateRenderTargets が持つ。
        void CreateGBufferCore(RHI::IRHIDevice& device, uint32_t width, uint32_t height, RHI::Format emissiveFormat);
        void CreateGBufferVelocity(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateGBufferBentNormal(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateLightingChain(RHI::IRHIDevice& device, uint32_t width, uint32_t height, RHI::Format aoFormat);
        void CreateTonemap(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateRTReflection(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateRTShadow(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateTAAHistory(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void CreateHiZ(RHI::IRHIDevice& device, uint32_t width, uint32_t height, uint32_t mipLevels);
        void CreateShadowCascadeArray(RHI::IRHIDevice& device, uint32_t size, uint32_t cascadeCount);
        // ソフトウェアラスタライザの出力3枚。visibility bufferの生成に挟まれた位置で呼ぶこと。
        // 失敗時にこの機能だけを無効化する縮退はKurenaiEngine3D::CreateRenderTargetsが持つ
        void CreateSoftwareRasterOutputs(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        void ResetSoftwareRasterOutputs();
        // 平面反射の2枚を作り、成功したときだけ実寸を記録する。
        // 失敗を送出したまま返すので、確保に失敗したら実寸は前の値のまま残る。
        // 呼び出し元(KurenaiEngine3D::CreatePlanarReflectionTargets)のtry内から呼ぶこと
        void CreatePlanarReflection(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        // ブルームのピラミッドをlevelCount段ぶん作り直す。段の解像度は半解像度から1段ごとに半分。
        // 呼び出し元のtry内から呼ぶこと(確保失敗時のフォールバックはCreateRenderTargetsが持つ)
        void CreateBloomPyramid(RHI::IRHIDevice& device, uint32_t width, uint32_t height, uint32_t levelCount);
        // 超解像の出力2枚を出力解像度で作り、実寸を記録する
        void CreateUpscale(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        // 上を解放し、実寸を0(未確保)に戻す
        void ResetUpscale();
        // ライトグリッドを作り直す。タイル数は解像度から切り上げで決まり、ここで記録する。
        // strideは1タイルあたりのuint数(KurenaiEngine3D::kLightTileStride)
        void CreateLightTiles(
            RHI::IRHIDevice& device, uint32_t width, uint32_t height, uint32_t tileSize, uint32_t stride);
        // MegaLightsの候補プールを作り直す。**CreateLightTilesの後に呼ぶこと**
        // (上で記録したタイル数から大きさが決まる)。
        // ジッター有効時は右端・下端のタイル座標が1つ増える。トグル変更でGPUを待って
        // 再確保しなくて済むよう、無効時も常に+1ぶんを確保しておく
        void CreateMegaLightsTilePool(RHI::IRHIDevice& device, uint32_t stride);
        // MegaLightsの生出力。呼び出し元のtry内から、元の行位置で呼ぶこと
        void CreateMegaLightsOutput(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        // デノイズ後の最終出力。デノイザ用の履歴を作る位置で呼ぶ
        void CreateMegaLightsDenoised(RHI::IRHIDevice& device, uint32_t width, uint32_t height);
        // 蓄積バッファ。elementCountは対応環境なら width*height、非対応なら1(ダミー)
        void CreateMegaLightsAccum(RHI::IRHIDevice& device, uint32_t elementCount);
    };
}
