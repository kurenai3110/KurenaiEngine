#include "DX12Device.h"
#include "DX12DeviceInternal.h"

#include <d3dx12.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DX12Buffer.h"
#include "DX12Texture.h"
#include "DX12Util.h"
#include "Core/StringUtil.h"
#include "RHI/RHIShaderPackage.h"

// 実行環境の機能判定(シェーダーモデル / bindless / メッシュシェーダー /
// タイル化リソース / レイトレーシング)と、bindless の登録。
// DX12Device のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は DX12Device.h のまま。IRHIDevice のインターフェースは1行も変えていない)
namespace Kurenai::RHI
{
    using namespace DX12Internal;

    void DX12Device::DetectShaderModelAndSelectVariant()
    {
        // D3D12_FEATURE_SHADER_MODELは「HighestShaderModelへ聞きたい上限を入れて呼ぶと、
        // 対応している値まで引き下げて返す」APIだが、ランタイムが知らない列挙値を渡すと
        // E_INVALIDARGを返す。そのため上から順に下げながら問い合わせる
        // 先頭が6_6なのはbindless(ResourceDescriptorHeap)がSM 6.6で追加されたため。
        // ここを6_5のままにするとデバイスが6.6対応でも6.5としか報告されず、
        // bindlessが常に無効になる
        static constexpr D3D_SHADER_MODEL kCandidates[] = {
            D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3,
            D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0,
        };

        m_HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0);
        for (const D3D_SHADER_MODEL candidate : kCandidates)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ candidate };
            if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))))
            {
                m_HighestShaderModel = shaderModel.HighestShaderModel;
                break;
            }
        }

        if (m_HighestShaderModel == static_cast<D3D_SHADER_MODEL>(0))
        {
            Core::Logger::Warning(
                "DX12", "対応シェーダーモデルを判定できませんでした。SM 5.0のバリアントで動作します");
        }

        // 【どのバリアントを使えるかはパッケージ側にも依存する】以前はここでdxcompiler.dllを
        // ロードし、そのバージョン(IDxcVersionInfo)を見てSM 6.6が使えるかを決めていた。
        // 事前コンパイルへ移したので、実行時にdxcは居ない。代わりに、ビルド時に
        // KurenaiShaderPackerが「どのバリアントを焼けたか」をヘッダーのVariantMaskへ
        // 記録しているので、それを読む。
        // (「dxcのバージョン = Windows SDKのバージョン」という制約は、実行環境ではなく
        //  ビルドマシンの話になった)
        m_ShaderVariantMask = ReadShaderVariantMask();

        const bool hasDxil66 = (m_ShaderVariantMask & (1u << static_cast<uint32_t>(Assets::ShaderVariant::Dxil66))) != 0u;
        const bool hasDxil65 = (m_ShaderVariantMask & (1u << static_cast<uint32_t>(Assets::ShaderVariant::Dxil65))) != 0u;

        if (hasDxil66 && m_HighestShaderModel >= D3D_SHADER_MODEL_6_6)
        {
            m_ShaderVariant = Assets::ShaderVariant::Dxil66;
            Core::Logger::Info("DX12", "事前コンパイル済みシェーダー: DXIL / SM 6.6(bindless有効)を使用します");
        }
        else if (hasDxil65 && m_HighestShaderModel >= D3D_SHADER_MODEL_6_5)
        {
            m_ShaderVariant = Assets::ShaderVariant::Dxil65;
            Core::Logger::Info(
                "DX12",
                std::string("事前コンパイル済みシェーダー: DXIL / SM 6.5 を使用します(bindlessは無効。理由: ") +
                    (hasDxil66 ? "デバイスがSM 6.6に非対応" : "ビルド時のdxcがSM 6.6に非対応でバリアントが焼かれていない") + ")");
        }
        else
        {
            // SM 6.0〜6.4のデバイス、またはSM 6.xのバリアントが1つも焼かれていない場合。
            // DXBCはD3D12でもそのまま受け付けられるため、従来の
            // 「dxcompiler.dllが無いときのd3dcompilerフォールバック」と同じ縮退になる
            // (レイトレーシング・メッシュシェーダー・自前ラスタライザはいずれも無効)
            m_ShaderVariant = Assets::ShaderVariant::Dxbc50;
            Core::Logger::Warning(
                "DX12",
                "事前コンパイル済みシェーダー: DXBC / SM 5.0 へ縮退します"
                "(レイトレーシング・メッシュシェーダー・自前ラスタライザはいずれも無効になります)");
        }
    }

    uint32_t DX12Device::ReadShaderVariantMask()
    {
        // どの.kshaderも同じパッカーの1回の実行で焼かれるため、VariantMaskは全ファイルで同じ。
        // 最初に見つかった1つを読めばよい(3Dと2Dで置き場所が違うので、決め打ちのファイル名は使わない)
        const std::wstring shaderDirectory = Core::GetModuleDirectory() + L"Shaders\\";

        std::error_code ec;
        if (!std::filesystem::is_directory(shaderDirectory, ec))
        {
            Core::Logger::Error(
                "DX12",
                "シェーダーフォルダがありません: " + Core::WideToUtf8(shaderDirectory) +
                    " (ビルド時にKurenaiShaderPackerが.kshaderを生成できていない可能性があります)");
            return 0u;
        }

        for (const auto& entry : std::filesystem::directory_iterator(shaderDirectory, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != L".kshader")
            {
                continue;
            }
            try
            {
                const Assets::ShaderPackageData& package = m_ShaderPackages.Get(entry.path().wstring());
                if (package.DebugBuild != kIsDebugBuild)
                {
                    // 気付きにくい取り違え(Releaseの実行ファイルがDebugの.kshaderを掴む等)を明示する。
                    // 動作はするので警告に留める
                    Core::Logger::Warning(
                        "DX12",
                        std::string(".kshaderの構成が実行ファイルと食い違っています(パッケージ: ") +
                            (package.DebugBuild ? "Debug" : "Release") + " / 実行ファイル: " +
                            (kIsDebugBuild ? "Debug" : "Release") + ")");
                }
                return package.VariantMask;
            }
            catch (const std::exception& e)
            {
                Core::Logger::Error(
                    "DX12", std::string("シェーダーパッケージを読めませんでした: ") + e.what());
                return 0u;
            }
        }

        Core::Logger::Error(
            "DX12",
            ".kshaderが1つも見つかりません: " + Core::WideToUtf8(shaderDirectory) +
                " (ビルド時にKurenaiShaderPackerが実行されていない可能性があります)");
        return 0u;
    }

    void DX12Device::DetectBindlessSupport()
    {
        m_SupportsBindless = false;

        // シェーダー側の条件。KURENAI_BINDLESS 付きで焼かれたSM 6.6のバリアントを
        // 実際に使っていること(デバイスの対応状況と、ビルド時にそのバリアントを
        // 焼けたかの両方で決まる。DetectShaderModelAndSelectVariant参照)
        if (m_ShaderVariant != Assets::ShaderVariant::Dxil66)
        {
            Core::Logger::Info(
                "DX12",
                "bindless非対応: SM 6.6のシェーダーバリアントを使用していません"
                "(ResourceDescriptorHeapにはSM 6.6対応のGPUと、ビルド時にSM 6.6を扱えるdxcが必要です)");
            return;
        }

        // ハードウェア側の条件。SM 6.6の動的リソース(ResourceDescriptorHeap)は
        // 「ヒープ全体をシェーダーから直接添字できる」ことが前提で、これはリソースバインディング
        // Tier 3が保証する性質。Tier 2以下はディスクリプタテーブルの範囲を越えたアクセスを
        // 認めていないため、コンパイルは通っても実行時の挙動が未定義になる
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        {
            Core::Logger::Warning("DX12", "bindless非対応: D3D12_FEATURE_D3D12_OPTIONSの問い合わせに失敗しました");
            return;
        }

        if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
        {
            Core::Logger::Info(
                "DX12",
                "bindless非対応: ResourceBindingTierが" + std::to_string(static_cast<int>(options.ResourceBindingTier)) +
                    "でTier 3に達していません");
            return;
        }

        m_SupportsBindless = true;
        Core::Logger::Info("DX12", "bindless(ResourceDescriptorHeap / SM 6.6)が利用可能です");
    }

    void DX12Device::DetectMeshShaderSupport()
    {
        m_SupportsMeshShader = false;

        // このエンジンのメッシュシェーダーはジオメトリをbindlessで引くため、
        // 増幅/メッシュシェーダーのバイトコードはSM 6.6(bindless)のバリアントにしか焼かれていない。
        // したがって条件はbindlessと同じ
        if (m_ShaderVariant != Assets::ShaderVariant::Dxil66)
        {
            Core::Logger::Info(
                "DX12",
                "メッシュシェーダー非対応: SM 6.6のシェーダーバリアントを使用していません"
                "(このエンジンのメッシュシェーダーはジオメトリをbindlessで引くため)");
            return;
        }

        // メッシュシェーダーPSOの作成にはID3D12Device2::CreatePipelineState(パイプラインステート
        // ストリーム)が要る。ID3D12Device2はWindows 10 1709で追加されたインタフェースで、
        // メッシュシェーダー対応GPUなら必ず取得できるが、無ければPSOを作る手段が無い
        if (FAILED(m_Device.As(&m_Device2)))
        {
            Core::Logger::Info("DX12", "メッシュシェーダー非対応: ID3D12Device2を取得できませんでした");
            return;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
        {
            // OPTIONS7自体がWindows 10 2004で追加された問い合わせのため、
            // それ以前のOSでは失敗する。その場合メッシュシェーダーも存在しない
            Core::Logger::Info("DX12", "メッシュシェーダー非対応: D3D12_FEATURE_D3D12_OPTIONS7の問い合わせに失敗しました");
            m_Device2.Reset();
            return;
        }

        if (options7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1)
        {
            Core::Logger::Info("DX12", "メッシュシェーダー非対応: MeshShaderTierがTier 1に達していません");
            m_Device2.Reset();
            return;
        }

        // 【bindlessを必須にする】このエンジンのメッシュシェーダーは、入力アセンブラの代わりに
        // 頂点・メッシュレットの各バッファをResourceDescriptorHeap経由で読む設計にしてある
        // (Shaders/3D/GBufferMeshlet.hlsl)。bindlessが無い環境向けにSRVテーブル経由の
        // 別実装を持つこともできるが、メッシュシェーダー対応GPUは実質すべてSM 6.6にも
        // 対応しているため、2系統を抱える価値が無い
        if (!m_SupportsBindless)
        {
            Core::Logger::Info(
                "DX12", "メッシュシェーダー非対応: bindlessが利用できないため無効にします");
            m_Device2.Reset();
            return;
        }

        m_SupportsMeshShader = true;
        Core::Logger::Info("DX12", "メッシュシェーダー(Tier 1 / SM 6.5)が利用可能です");
    }

    void DX12Device::DetectSoftwareRasterSupport()
    {
        m_SupportsSoftwareRaster = false;

        // 【bindlessを必須にする】自前ラスタライザは三角形1つにつき頂点バッファと
        // インデックスバッファをResourceDescriptorHeap経由で引く(Shaders/3D/SoftwareRaster.hlsl)。
        // bindlessが立っている時点でシェーダーモデル6.6とリソースバインディングTier 3が
        // 保証されるため、シェーダーモデルの再判定は要らない
        if (!m_SupportsBindless)
        {
            Core::Logger::Info(
                "DX12", "ソフトウェアラスタライザ非対応: bindlessが利用できません");
            return;
        }

        // 深度と三角形IDを1つの64bit値へ詰めてInterlockedMaxで解決するため、
        // 64bit整数のシェーダー演算が要る。SM 6.6に対応していてもこれが無いGPUはあり得るので
        // (機能レベルとは独立した任意機能)、必ず個別に問い合わせる
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))))
        {
            Core::Logger::Info(
                "DX12", "ソフトウェアラスタライザ非対応: D3D12_FEATURE_D3D12_OPTIONS1の問い合わせに失敗しました");
            return;
        }

        if (!options1.Int64ShaderOps)
        {
            Core::Logger::Info(
                "DX12", "ソフトウェアラスタライザ非対応: 64bit整数のシェーダー演算(Int64ShaderOps)に対応していません");
            return;
        }

        m_SupportsSoftwareRaster = true;
        Core::Logger::Info("DX12", "ソフトウェアラスタライザ(SM 6.6 / 64bitアトミック)が利用可能です");
    }

    void DX12Device::DetectTiledResourcesSupport()
    {
        m_TiledResourcesTier = 0;

        // TiledResourcesTierはbindless判定が引いているのと同じD3D12_FEATURE_D3D12_OPTIONSのメンバ。
        // 判定ごとに独立した関数にする既存の形へ揃えるため、ここでもう一度引く(実害は無い)
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        {
            Core::Logger::Info("DX12", "タイルリソース非対応: D3D12_FEATURE_D3D12_OPTIONSの問い合わせに失敗しました");
            return;
        }

        m_TiledResourcesTier = static_cast<uint32_t>(options.TiledResourcesTier);
        if (m_TiledResourcesTier == 0)
        {
            Core::Logger::Info("DX12", "タイルリソース非対応(TiledResourcesTier 0)。常駐ミップ制御のみで動作します");
            return;
        }

        // 【Tier 1 は使わない】Tier 1 では未マップのタイルを読んだときの結果が未定義で、
        // デバイス削除に至ることもある。全域へダミータイルを貼れば回避できるが、
        // それは「常駐していないミップを読んでも落ちないようにする」ための仕掛けであって
        // 常駐量を減らす目的には貢献しない。Tier 2 以上は「未マップは0を読み、書きは捨てる」と
        // 仕様が保証しているので、こちらだけを対象にする
        if (m_TiledResourcesTier < 2)
        {
            const uint32_t reportedTier = m_TiledResourcesTier;
            // 上位層は「0なら使わない」で分岐するため、採らないと決めた時点で0へ落とす
            m_TiledResourcesTier = 0;
            Core::Logger::Info(
                "DX12",
                "タイルリソースはTier " + std::to_string(reportedTier) +
                    "のため使いません(未マップタイルの読み出しが未定義。Tier 2以上が必要)。常駐ミップ制御のみで動作します");
            return;
        }

        Core::Logger::Info(
            "DX12", "タイルリソース(Tier " + std::to_string(m_TiledResourcesTier) + ")が利用可能です");
    }

    uint32_t DX12Device::RegisterBindless(IRHITexture* texture)
    {
        if (!m_SupportsBindless || !m_BindlessTable || !texture)
        {
            return kInvalidBindlessIndex;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        // 既に登録済みなら同じ番号を返す。呼び出し側が重複登録で区画を食い潰さないようにする
        if (dx12Texture->GetBindlessIndex() != kInvalidBindlessIndex)
        {
            return dx12Texture->GetBindlessIndex();
        }

        // SRVを持たないテクスチャ(深度専用など)はbindlessで読めない。
        // 無効なハンドルを渡すとでたらめなディスクリプタが区画へ入るため、ここで弾く
        if (!dx12Texture->HasSrv())
        {
            Core::Logger::Error("DX12", "SRVを持たないテクスチャがbindlessへ登録されようとしました");
            return kInvalidBindlessIndex;
        }

        const uint32_t index = m_BindlessTable->Register(dx12Texture->GetSrvCpuHandle());
        dx12Texture->SetBindlessIndex(index);
        return index;
    }

    uint32_t DX12Device::RegisterBindless(IRHIBuffer* buffer)
    {
        if (!m_SupportsBindless || !m_BindlessTable || !buffer)
        {
            return kInvalidBindlessIndex;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        if (dx12Buffer->GetBindlessIndex() != kInvalidBindlessIndex)
        {
            return dx12Buffer->GetBindlessIndex();
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE srv = dx12Buffer->GetSrvCpuHandle();
        if (srv.ptr == 0)
        {
            // SRVを持たないUsage(Vertex/Index/Constant、およびShaderReadableを指定しなかった
            // 頂点バッファ)。BufferDesc::ShaderReadableの指定漏れがここで表面化する
            Core::Logger::Error(
                "DX12", "SRVを持たないバッファがbindlessへ登録されようとしました(BufferDesc::ShaderReadableの指定漏れ?)");
            return kInvalidBindlessIndex;
        }

        const uint32_t index = m_BindlessTable->Register(srv);
        dx12Buffer->SetBindlessIndex(index);
        return index;
    }

    uint32_t DX12Device::GetBindlessUsedCount() const
    {
        return m_BindlessTable ? m_BindlessTable->GetUsedCount() : 0;
    }

    uint32_t DX12Device::GetBindlessCapacity() const
    {
        return m_BindlessTable ? m_BindlessTable->GetCapacity() : 0;
    }

    uint32_t DX12Device::RegisterBindlessUAV(IRHIBuffer* buffer)
    {
        if (!m_SupportsBindless || !m_BindlessTable || !buffer)
        {
            return kInvalidBindlessIndex;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        if (dx12Buffer->GetBindlessUavIndex() != kInvalidBindlessIndex)
        {
            return dx12Buffer->GetBindlessUavIndex();
        }

        if (!dx12Buffer->HasUav())
        {
            // UAVを持たないUsage(Vertex/Index/Constant/StructuredReadOnly/StructuredImmutable)。
            // 無効なハンドルを渡すとでたらめなディスクリプタが区画へ入るため、ここで弾く
            Core::Logger::Error("DX12", "UAVを持たないバッファがbindless(UAV)へ登録されようとしました");
            return kInvalidBindlessIndex;
        }

        const uint32_t index = m_BindlessTable->Register(dx12Buffer->GetUavCpuHandle());
        dx12Buffer->SetBindlessUavIndex(index);
        return index;
    }

    uint32_t DX12Device::GetMaxDrawsPerFrame() const
    {
        // AllocateSrvTableBlockが1フレームに払い出せるブロック数と同じ。
        // これを超えるとAllocateSrvTableBlockが例外を投げる
        return kMaxSrvTableBlocksPerFrame;
    }

    void DX12Device::DetectRaytracingSupport()
    {
        m_SupportsRaytracing = false;

        // ID3D12Device5はWindows 10 1809(RS5)で追加されたインタフェース。取得できない場合は
        // OS/ドライバがDXR世代に達していないため、それ以上の判定は行えない
        if (FAILED(m_Device.As(&m_Device5)))
        {
            Core::Logger::Info("DX12", "レイトレーシング非対応: ID3D12Device5を取得できませんでした(OS/ドライバがDXR未対応)");
            return;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
        {
            Core::Logger::Warning("DX12", "レイトレーシング非対応: D3D12_FEATURE_D3D12_OPTIONS5の問い合わせに失敗しました");
            m_Device5.Reset();
            return;
        }

        // インラインレイトレーシング(HLSLのRayQuery)はTier 1.1で追加された機能。
        // Tier 1.0はDispatchRaysによるフルパイプラインのみ対応しており、このエンジンが採る
        // インライン方式では使えないため非対応として扱う
        if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
        {
            Core::Logger::Info(
                "DX12",
                "レイトレーシング非対応: RaytracingTierが" + std::to_string(static_cast<int>(options5.RaytracingTier)) +
                    "でTier 1.1(値11)に達していません(インラインレイトレーシングにはTier 1.1が必要)");
            m_Device5.Reset();
            return;
        }

        // インラインレイトレーシングのRayQueryはシェーダーモデル6.5で追加された機能で、
        // DXILでしか表現できない。ハードウェアがTier 1.1でも、DXILのバリアント
        // (Dxil65 / Dxil66)を使っていなければトレースするシェーダーを作れないため
        // 非対応として扱う(Phase 0でdxc/SM 6.xへ移行した理由そのもの)
        if (m_ShaderVariant != Assets::ShaderVariant::Dxil65 && m_ShaderVariant != Assets::ShaderVariant::Dxil66)
        {
            Core::Logger::Warning(
                "DX12",
                "レイトレーシング非対応: DXIL(SM 6.5以上)のシェーダーバリアントを使用していません"
                "(RayQueryにはSM 6.5が必要です。デバイスの対応状況と、ビルド時に.kshaderへSM 6.xのバリアントが"
                "焼かれているかを確認してください)");
            m_Device5.Reset();
            return;
        }

        // AS構築コマンドを積むのはアップロード専用コマンドリスト(Renderスレッド外から呼ばれる
        // LoadSceneと安全に共存させるため)。そちらのList4も取れないと構築できない
        if (FAILED(m_UploadCommandList.As(&m_UploadCommandList4)))
        {
            Core::Logger::Warning(
                "DX12", "レイトレーシング非対応: ID3D12GraphicsCommandList4を取得できませんでした");
            m_Device5.Reset();
            return;
        }

        m_SupportsRaytracing = true;
        Core::Logger::Info("DX12", "レイトレーシング対応: DXR Tier 1.1(インラインレイトレーシングが利用できます)");
    }
}
