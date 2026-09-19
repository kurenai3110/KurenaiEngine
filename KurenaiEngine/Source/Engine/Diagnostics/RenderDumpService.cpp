#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "Core/StringUtil.h"
// パス群のカウンタを m_XxxPasses->Get...() で読むため、前方宣言では足りない
#include "../Passes/GeometryPasses.h"
#include "../Passes/MegaLightsPasses.h"
#include "../Passes/ShadowPasses.h"
#include "RenderDumpService.h"

// 中間レンダーターゲットのダンプ(-dumptex)、パスマニフェストの書き出し(-passmanifest)、
// 性能記録のログ出力。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま)
namespace Kurenai
{
    namespace
    {
        // MegaLights のパス群が作られていない環境(DX11・非DXR・PSO未生成)では
        // 添字を引けないので0を返す。**表のエントリ自体は載せる** ―― 中身がnullptrでも
        // 「名前が無い」と「今は作られていない」を呼び出し側が区別できるようにするため
        uint32_t DenoiseHistoryIndexOf(const Passes::MegaLightsPasses* passes)
        {
            return passes != nullptr ? passes->GetDenoiseHistoryIndex() : 0u;
        }
    }

    std::vector<KurenaiEngine3D::DumpableTexture> KurenaiEngine3D::BuildDumpableTextureTable() const
    {
        const uint32_t denoiseHistoryIndex = DenoiseHistoryIndexOf(m_MegaLightsPasses.get());
        // 名前 -> 中間テクスチャ。AddTextureDump(起動オプション -dumptex)が引く。
        //
        // 【DebugViewの番号と共有しない】あちらは「表示モード」でテクスチャと1対1ではない
        // (DepthとDepthRawは同じm_RenderTargets.GBufferDepth、LightTilesはテクスチャではなくバッファを読む)。
        // さらに切り分けで見たいもの ―― SSILRaw / TransmittanceLUT / TAAHistory / ExposureTexture ――
        // はDebugViewに存在せず、足すにはPresent.hlslの表示モードを増やすことになる。
        // 加えてDebugViewの番号は -debugview N として既に契約になっており、
        // 途中に足すとdocsと履歴に記録済みの番号が全部ずれる。
        //
        // 【CreateRenderTargetsの直後に置いてある】ポインタが生まれる場所の隣なら、
        // テクスチャを増やしたときにここへ足し忘れにくい。
        // **CreateRenderTargets等でテクスチャを増やしたらここにも足すこと。**
        //
        // 名前はメンバ名から m_ を外したもの。中身がnullptr(機能が無効・非対応環境)の
        // エントリも表には載せる ―― 「名前が無い」と「今は作られていない」は別のことで、
        // 呼び出し側にそれぞれ別のログを出させるため
        return {
            // G-Buffer
            { "GBufferAlbedo", m_RenderTargets.GBufferAlbedo.get() },
            { "GBufferNormal", m_RenderTargets.GBufferNormal.get() },
            { "GBufferMaterial", m_RenderTargets.GBufferMaterial.get() },
            { "GBufferEmissive", m_RenderTargets.GBufferEmissive.get() },
            { "GBufferDepth", m_RenderTargets.GBufferDepth.get() },
            { "GBufferVelocity", m_RenderTargets.GBufferVelocity.get() },
            { "GBufferBentNormal", m_RenderTargets.GBufferBentNormal.get() },
            // ライティングと間接光
            { "DirectLightTexture", m_RenderTargets.DirectLightTexture.get() },
            { "SSAORawTexture", m_RenderTargets.SSAORawTexture.get() },
            { "SSAOTexture", m_RenderTargets.SSAOTexture.get() },
            { "SSILRawTexture", m_RenderTargets.SSILRawTexture.get() },
            { "SSILTexture", m_RenderTargets.SSILTexture.get() },
            { "RTAORawTexture", m_RenderTargets.RTAORawTexture.get() },
            { "RTAOTexture", m_RenderTargets.RTAOTexture.get() },
            { "RTShadowTexture", m_RenderTargets.RTShadowTexture.get() },
            { "SceneColor", m_RenderTargets.SceneColor.get() },
            // 反射
            { "SSRTexture", m_RenderTargets.SSRTexture.get() },
            { "RTReflectionTexture", m_RenderTargets.RTReflectionTexture.get() },
            { "PlanarReflectionColor", m_RenderTargets.PlanarReflectionColor.get() },
            { "PlanarReflectionDepth", m_RenderTargets.PlanarReflectionDepth.get() },
            // MegaLights
            { "MegaLightsTexture", m_RenderTargets.MegaLightsTexture.get() },
            { "MegaLightsDenoisedTexture", m_RenderTargets.MegaLightsDenoisedTexture.get() },
            // デノイザの履歴とモーメント。TAAHistory / TAAHistoryPrev とまったく同じ扱いで、
            // 添字は**今フレームの書き込み先**(MegaLightsPasses::GetDenoiseHistoryIndex のコメント)。
            //
            // 【何のために出せるようにしたか】Moments の **z 成分が履歴長**で、履歴が棄却された
            // 画素は 1.0 に落ちる(MegaLightsDenoise.hlsl の CSTemporalAccum)。つまりこれは
            // 「時間方向の記憶が実際に何フレームぶん効いているか」「どこで履歴を捨てているか」の
            // 直接の観測になる。いまはどこからも読めず、移動中の粒の原因を棄却へ帰属できない。
            //
            // 【デノイザが走らないフレームは添字が据え置かれる】-megalightsdenoise 0 では
            // Moments と MomentsPrev が同じ絵を指し続ける。正しい挙動であって配線のバグではない
            { "MegaLightsDenoiseHistory",
              m_RenderTargets.MegaLightsDenoiseHistory[denoiseHistoryIndex].get() },
            { "MegaLightsDenoiseHistoryPrev",
              m_RenderTargets.MegaLightsDenoiseHistory[denoiseHistoryIndex ^ 1u].get() },
            { "MegaLightsDenoiseMoments",
              m_RenderTargets.MegaLightsDenoiseMoments[denoiseHistoryIndex].get() },
            { "MegaLightsDenoiseMomentsPrev",
              m_RenderTargets.MegaLightsDenoiseMoments[denoiseHistoryIndex ^ 1u].get() },
            // 履歴長の適応が「どこで、どれだけ撃ったか」を数値で読むための入口。
            // タイル解像度(画面の1/8)で、値は 0〜1 の λ(1で履歴を捨てきる)。
            // **絵で見ずにここを数える** ―― 静止での偽陽性率も、変化への追従も、
            // 目視では「それらしく見える」だけで判定できない
            { "MegaLightsDenoiseTileGradient", m_RenderTargets.MegaLightsDenoiseTileGradient.get() },
            // 影・Hi-Z
            { "ShadowCascadeArray", m_RenderTargets.ShadowCascadeArray.get() },
            { "HiZTexture", m_RenderTargets.HiZTexture.get() },
            // 空と大気
            { "SkyCloudTexture", m_RenderTargets.SkyCloudTexture.get() },
            { "SkyCloudFogTexture", m_RenderTargets.SkyCloudFogTexture.get() },
            { "AerialPerspectiveTexture", m_RenderTargets.AerialPerspectiveTexture.get() },
            { "TransmittanceLUT", m_SkyResources.TransmittanceLUT.get() },
            { "MultiScatteringLUT", m_SkyResources.MultiScatteringLUT.get() },
            { "SkyViewLUT", m_SkyResources.SkyViewLUT.get() },
            // DDGI
            { "DDGIIrradianceAtlas", m_GIResources.DDGIIrradianceAtlas.get() },
            { "DDGIDistanceAtlas", m_GIResources.DDGIDistanceAtlas.get() },
            { "DDGIResolveTexture", m_GIResources.DDGIResolveTexture.get() },
            { "DDGIResolveDepthTexture", m_GIResources.DDGIResolveDepthTexture.get() },
            // IBL
            { "BRDFLUTTexture", m_IBLResources.BRDFLUTTexture.get() },
            // ポストプロセスと最終段
            { "TonemapTexture", m_RenderTargets.TonemapTexture.get() },
            { "UpscaleTexture", m_RenderTargets.UpscaleTexture.get() },
            { "UpscaleSharpTexture", m_RenderTargets.UpscaleSharpTexture.get() },
            // DLSSの出力(出力解像度・プリ露出済みHDR)。Tonemapより前の段なので
            // TonemapTextureとは値域が違う(あちらは表示レンジのLDR)
            { "DLSSOutputTexture", m_RenderTargets.DLSSOutputTexture.get() },
            { "ExposureTexture", m_RenderTargets.ExposureTexture.get() },
            // TAAの履歴。今フレームの書き込み先が m_History.HistoryIndex なので、
            // 「前フレームの履歴」を見たいときは Prev のほうを指定する
            { "TAAHistory", m_RenderTargets.TAAHistory[m_History.HistoryIndex].get() },
            { "TAAHistoryPrev", m_RenderTargets.TAAHistory[m_History.HistoryIndex ^ 1u].get() },
            // 自前ソフトウェアラスタライザ
            { "SoftwareRasterColor", m_RenderTargets.SoftwareRasterColor.get() },
            { "SoftwareRasterDepth", m_RenderTargets.SoftwareRasterDepth.get() },
            { "SoftwareRasterNormal", m_RenderTargets.SoftwareRasterNormal.get() },
        };
    }

    std::vector<std::string> KurenaiEngine3D::GetDumpableTextureNames() const
    {
        std::vector<std::string> names;
        for (const DumpableTexture& entry : BuildDumpableTextureTable())
        {
            names.emplace_back(entry.Name);
        }
        return names;
    }

    std::vector<KurenaiEngine3D::DumpableBuffer> KurenaiEngine3D::BuildDumpableBufferTable() const
    {
        const uint64_t tileCount = static_cast<uint64_t>(m_RenderTargets.LightTileCountX) *
            m_RenderTargets.LightTileCountY;
        return {
            { "MegaLightsTilePoolBuffer", m_RenderTargets.MegaLightsTilePoolBuffer.get(),
                static_cast<uint32_t>(tileCount * kMegaLightsTilePoolStride), static_cast<uint32_t>(sizeof(uint32_t)) },
            { "MegaLightsVisibleLists[0]", m_RenderTargets.MegaLightsVisibleLists[0].get(),
                static_cast<uint32_t>(tileCount * kMegaLightsVisibleListStride), static_cast<uint32_t>(sizeof(uint32_t)) },
            { "MegaLightsVisibleLists[1]", m_RenderTargets.MegaLightsVisibleLists[1].get(),
                static_cast<uint32_t>(tileCount * kMegaLightsVisibleListStride), static_cast<uint32_t>(sizeof(uint32_t)) },
            { "MegaLightsReservoirBuffer", m_RenderTargets.MegaLightsReservoirBuffer.get(),
                static_cast<uint32_t>(static_cast<uint64_t>(m_RenderWidth) * m_RenderHeight *
                    static_cast<uint32_t>(m_MegaLightsAllocatedSamplesPerPixel)), static_cast<uint32_t>(sizeof(uint32_t)) * 4u },
        };
    }


    void KurenaiEngine3D::ApplyDebugNamesIfDirty()
    {
        // 【表は毎回作り直す】理由は BuildDumpableTextureTable のコメント
        m_DumpService.ApplyDebugNamesIfDirty(BuildDumpableTextureTable());
    }

    void KurenaiEngine3D::IssueTextureDumps(Core::RenderGraph& graph)
    {
        m_DumpService.IssueTextureDumps(graph, BuildDumpableTextureTable(), m_History.FrameIndex, *m_Device);
        m_DumpService.IssueBufferDumps(graph, BuildDumpableBufferTable(), m_History.FrameIndex, *m_Device);
    }

    void KurenaiEngine3D::ResolveTextureDumps()
    {
        m_DumpService.ResolveTextureDumps(
            m_History.FrameIndex, m_Window.get(), m_GraphicsAPI == GraphicsAPI::DX12);
    }

    void KurenaiEngine3D::WritePassManifestIfDue(Core::RenderGraph& graph)
    {
        m_DumpService.WritePassManifestIfDue(graph, m_History.FrameIndex, m_GraphicsAPI == GraphicsAPI::DX12);
    }
}
