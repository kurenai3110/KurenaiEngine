#pragma once

// DX12Device*.cpp だけが読む内部ヘッダー。**RHI のインターフェースではない。**
//
// 【なぜヘッダーにしたか】DX12Device.cpp を機能ごとの翻訳単位へ割るにあたり、
// もともと無名名前空間にあった定数と変換関数を複数の .cpp から使う必要が出た。
// ここに置くものは DX12 のバックエンド内部の都合で決まる値ばかりで、
// RHI 層の利用者へ見せるものは1つも無い。**Source/Library/RHI/ の IRHI*.h とは別物。**

#include <cstdint>

#include <d3d12.h>
#include <d3dx12.h>

#include "RHI/RHIDesc.h"
#include "RHI/RHIBindingLimits.h"
#include "RHI/PipelineStateNormalize.h"

namespace Kurenai::RHI::DX12Internal
{
    // この実行ファイルがDebug構成でビルドされているか。
    // .kshader側のフラグと突き合わせて、構成の取り違えを警告するためだけに使う
#if defined(_DEBUG)
    constexpr bool kIsDebugBuild = true;
#else
    constexpr bool kIsDebugBuild = false;
#endif

    // ルートシグネチャのSRVレンジ幅。【定義は RHI/RHIBindingLimits.h】が唯一の出所で、
    // ここはそこから引くだけ。なぜ23必要かの内訳と、超えたときに何が起きるかもあちらにある
    using RHIBindingLimits::kTextureSlotCount;
    // ObjectConstants(b1)を受けるルートパラメータの番号。
    // CreateRootSignature / CreateMeshRootSignature の rootParams[1] と一致させること。
    // 間接DispatchMeshのコマンドシグネチャが、この番号のCBVをドローごとに差し替える
    constexpr uint32_t kRootParamObjectConstants = 1;
    // 1つのサンプラーセット(=1つのディスクリプタテーブル)が持つスロット数。
    // s0 = MaterialSampler、s1 = ColorSampler、s2 = DataSampler、s3 = VolumeSampler
    // (役割の定義はShaders/Samplers.hlsli)。どの実体が入るかはパスごとにエンジン側が選んだセットで決まる。
    // 3必要だったのはTransparent.hlslが「マテリアル・シャドウマップ・BRDF積分LUT」の3種類を
    // 1回のピクセルシェーダ実行で同時に使うためで、4つ目はボリュームテクスチャ(3Dノイズ)を
    // Wrapで引くためのVolumeSampler。
    // 一部のスロットしか宣言しないシェーダーでもテーブルはこの個数ぶんまとめてバインドされる
    // (CreateSamplerSet参照)。【定義は RHI/RHIBindingLimits.h】
    using RHIBindingLimits::kSamplerSlotCount;
    // 作成できるサンプラーセットの最大数。セットは初期化時にだけ作られ解放されないため、
    // 用途の種類数に余裕を持たせた値でよい。
    // シェーダ可視Samplerヒープの上限はD3D12の仕様で2048ディスクリプタ
    // (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE)なので、この程度なら十分収まる。
    //
    // 内訳: KurenaiEngine3Dが2つ(マテリアル用・スクリーン空間用)、KurenaiEngine2Dが7つ
    // (スプライトのフィルタ3種×アドレスモード2種を作り置き + DrawText専用の1つ。
    //  理由はRHI/IRHISamplerSet.h「セットの中身は生成後に書き換えない」)。
    // 3Dと2Dは別プロセス・別デバイスなので同時に使われることはないが、
    // 超えるとAllocateBlockが初期化時に例外を投げるため、増やす側に余裕を取っている
    constexpr uint32_t kMaxSamplerSets = 16;
    // 1フレームあたりに払い出せるSRVテーブルブロック(t0〜t14のkTextureSlotCount個ひと組)の最大数。
    // 1フレーム中の(メッシュ数×パス数)を十分上回る値にしておく。実際に確保するヒープ容量は
    // これのkFrameCount倍(CPUがGPU完了を待たずに次フレームを記録し始めるため、直近kFrameCount
    // フレームぶんのブロックがまだGPUに読まれている可能性がある)
    constexpr uint32_t kMaxSrvTableBlocksPerFrame = 4096;
    // 定数バッファ(Usage==Constant)がリングとして持つスロット数。CPUがGPU完了を待たずに次フレームを
    // 記録し始めるため、直近kFrameCountフレームぶんのUpdateBuffer回数(メッシュ数など)を
    // 十分上回る値にしておく。
    //
    // 【8192から32768へ引き上げた理由】1フレームに安全に書ける回数はこの値÷kFrameCountで、
    // 8192では4096回だった。多数の.kmodelを並べるシーン(PLATEAUの東京23区LOD1は671モデル)では
    // G-Buffer 671 + シャドウ4カスケード×671 = 3355回、深度プリパスが走る構成だと4026回に達し、
    // 余裕が1.7%しか残らない。**超過しても例外は投げずログを1回出して続行する**ため、
    // 気づかないまま描画結果が壊れる(DX12Buffer.hのコメント参照)。
    // 1スロット256バイトなので、32768段でもUPLOADヒープの消費は8MB。
    constexpr uint32_t kConstantBufferRingCapacity = 32768;

    // コンピュートシェーダー用ルートシグネチャのSRV/UAVディスクリプタテーブルレイアウト(t0〜t16, u0〜u3)。
    // SRVが17必要なのはレイトレーシングのパス(RT反射)で、TLAS + G-Buffer(Albedo/Normal/Material/Depth) +
    // SceneColor + スカイボックス + シーンジオメトリ4本(頂点属性・インデックス・メッシュ情報・
    // マテリアル) + インスタンス情報 + bent normal(t16、34章) + メッシュレット表(t17、38章)
    // を1回のディスパッチで同時に読むため。
    // DX12CommandList.h側の同名の定数と必ず一致させること
    // 【定義は RHI/RHIBindingLimits.h】ルートシグネチャのSRV/UAVレンジ幅
    using RHIBindingLimits::kComputeSrvSlotCount;
    using RHIBindingLimits::kComputeUavSlotCount;
    constexpr uint32_t kComputeTableSlotCount = kComputeSrvSlotCount + kComputeUavSlotCount;
    // 1フレームあたりに払い出せるコンピュートSRV+UAVテーブルブロックの最大数(Dispatch呼び出し回数の上限)。
    // 反射プローブのベイクは1プローブあたり6(面コピー)+6(イラディアンス)+36(プリフィルタ6ミップ×6面)=48回
    // ディスパッチし、複数プローブを同一フレームでまとめて焼くため、プローブ数ぶんの余裕が要る
    constexpr uint32_t kMaxComputeDispatchesPerFrame = 1024;
    // グラフィックス用SRVテーブル領域の1フレームあたりのディスクリプタ数。m_ShaderVisibleSrvHeap内では
    // 先頭からこの数×kFrameCountぶんをグラフィックス用が占有し、コンピュートシェーダー用のSRV+UAVテーブルは
    // それより後ろの区画に別リングとして確保する(kFrameCountはDX12Deviceのprivateメンバのため、
    // 実際の掛け合わせはこれを参照できるメンバ関数側で行う)
    constexpr uint32_t kGraphicsSrvHeapCapacityPerFrame = kTextureSlotCount * kMaxSrvTableBlocksPerFrame;
    constexpr uint32_t kComputeSrvHeapCapacityPerFrame = kComputeTableSlotCount * kMaxComputeDispatchesPerFrame;

    // bindless区画(HLSLのResourceDescriptorHeapが直接添字する恒久ディスクリプタ)の容量。
    // シェーダ可視SRVヒープの**末尾**に、上の2つのリングより後ろへ切り出す。
    //
    // 【なぜ末尾なのか】先頭に置くとAllocateSrvTableBlock/AllocateComputeTableBlockの
    // 区画先頭の計算を両方ずらす必要があり、リングの巻き戻り位置の議論をやり直すことになる。
    // 末尾なら既存2つのリングの番号空間に一切触れずに済む。
    //
    // 【容量の根拠】登録するのは「シェーダーが動的な番号で選びたいもの」だけで、
    // 内訳は (1) マテリアルテクスチャ、(2) メッシュごとの頂点/インデックスバッファ、
    // (3) モデルごとのメッシュレット表・メッシュ表・マテリアルテーブル(5本)。
    //
    // 【8192では足りない】当初の根拠はBistro Exterior(テクスチャ182枚 + メッシュ約400×5本
    // ≒ 2182)だったが、PLATEAU LOD2は**1タイルだけでマテリアル1,715・テクスチャ1,714**ある
    // (実測)。メッシュレット表をモデル単位へ統合した後でも
    //   テクスチャ 1,714 + メッシュ 1,715×2(頂点+インデックス) + モデル 5 = 5,149 / タイル
    // で、丸の内2タイルなら約10.3k、23区で LOD2 を4タイル常駐させ LOD1 の671モデル
    // (671×(2+5) ≒ 4.7k)を足すと約25kになる。
    //
    // 【超えたときに静かに壊れる】DX12BindlessTable::Registerは満杯でも例外を投げず、
    // エラーログを出してkInvalidBindlessIndexを返す。消費側はそれを「テクスチャ無し」と
    // 解釈して白1x1へ落とすため、**絵はそれらしく出たまま間違う**。
    // だから上限は「足りるはず」ではなく明確に余裕のある側へ倒す。
    //
    // シェーダ可視CBV_SRV_UAVヒープの上限はTier 1でも1,000,000ディスクリプタあり、
    // 65536でもディスクリプタ1つ32バイト換算で2MBに過ぎない
    constexpr uint32_t kBindlessDescriptorCapacity = 65536;

    // シェーダーがResourceDescriptorHeapでヒープを直接添字することを許可するルートシグネチャの
    // フラグ(D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)。
    //
    // 【自前で持つ理由】この列挙子はWindows SDK 10.0.20348で追加されたもので、
    // それ以前の10.0.19041のd3d12.hには無い。
    // このリポジトリは10.0.26100以降を前提にしている(READMEの必要環境を参照。
    // SM 6.6を吐けるdxcもSDKに同梱される1.8系が要る)ため通常は列挙子を使えるが、
    // ここを列挙子に置き換えると、古いSDKしか無い環境ではDX12以外も含めて
    // ライブラリ全体がコンパイルすら通らなくなる。
    // bindless非対応環境ではこのフラグを立てずに従来どおり動く縮退を用意してあるのに、
    // ビルドできないのでは縮退が働く前に詰んでしまう。
    //
    // 値0x400はD3D12のABIとして固定で、10.0.26100のd3d12.hでも同じ値が入っていることを確認済み。
    // SDKの列挙子と名前が衝突しないよう、エンジン側の命名で持つ
    constexpr D3D12_ROOT_SIGNATURE_FLAGS kRootSignatureFlagCbvSrvUavHeapDirectlyIndexed =
        static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x400);

    // --- 意味値 → D3D12 の型 ---------------------------------------------------------
    // どう振る舞うかを決めているのは PipelineStateNormalize.{h,cpp} で、ここは型の詰め替えだけ。
    // 判断がここに入り込むと、DX11側と食い違っても誰も気づけなくなる

    inline D3D12_BLEND ToD3D12Blend(BlendFactorValue factor)
    {
        switch (factor)
        {
        case BlendFactorValue::Zero:
            return D3D12_BLEND_ZERO;
        case BlendFactorValue::SrcAlpha:
            return D3D12_BLEND_SRC_ALPHA;
        case BlendFactorValue::InvSrcAlpha:
            return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactorValue::DestColor:
            return D3D12_BLEND_DEST_COLOR;
        case BlendFactorValue::DestAlpha:
            return D3D12_BLEND_DEST_ALPHA;
        case BlendFactorValue::One:
        default:
            return D3D12_BLEND_ONE;
        }
    }

    inline D3D12_BLEND_OP ToD3D12BlendOp(BlendOpValue op)
    {
        switch (op)
        {
        case BlendOpValue::Add:
        default:
            return D3D12_BLEND_OP_ADD;
        }
    }

    inline D3D12_COMPARISON_FUNC ToD3D12Comparison(DepthCompareValue compare)
    {
        switch (compare)
        {
        case DepthCompareValue::LessEqual:
            return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case DepthCompareValue::Greater:
            return D3D12_COMPARISON_FUNC_GREATER;
        case DepthCompareValue::GreaterEqual:
            return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case DepthCompareValue::Less:
        default:
            return D3D12_COMPARISON_FUNC_LESS;
        }
    }

    inline D3D12_FILTER ToD3D12Filter(SamplerFilterValue filter)
    {
        switch (filter)
        {
        case SamplerFilterValue::Anisotropic:
            return D3D12_FILTER_ANISOTROPIC;
        case SamplerFilterValue::Point:
            return D3D12_FILTER_MIN_MAG_MIP_POINT;
        case SamplerFilterValue::Linear:
        default:
            return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        }
    }

    inline D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(SamplerAddressValue addressMode)
    {
        switch (addressMode)
        {
        case SamplerAddressValue::Clamp:
            return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case SamplerAddressValue::Wrap:
        default:
            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }
    }

    // RHIのBlendModeをD3D12のレンダーターゲットブレンド設定へ写す。
    // 通常のグラフィックスPSOとメッシュシェーダーPSOの両方から使う
    // (2つのPSO作成関数で同じ詰め替えを書き写すと、片方だけ直して静かに挙動がずれる)
    inline void ApplyBlendMode(D3D12_RENDER_TARGET_BLEND_DESC& rt, BlendMode blendMode)
    {
        const BlendStateValues blend = NormalizeBlendMode(blendMode);
        rt.BlendEnable = blend.Enable ? TRUE : FALSE;
        if (!blend.Enable)
        {
            return;
        }
        rt.SrcBlend = ToD3D12Blend(blend.SrcColor);
        rt.DestBlend = ToD3D12Blend(blend.DestColor);
        rt.BlendOp = ToD3D12BlendOp(blend.ColorOp);
        rt.SrcBlendAlpha = ToD3D12Blend(blend.SrcAlpha);
        rt.DestBlendAlpha = ToD3D12Blend(blend.DestAlpha);
        rt.BlendOpAlpha = ToD3D12BlendOp(blend.AlphaOp);
    }

    // 非シェーダー可視のCBV_SRV_UAVヒープ(テクスチャ/構造化バッファ作成時に
    // CreateShaderResourceView等の恒久的なビューを1つずつ確保する)の容量。
    //
    // DX12DescriptorHeapはロックを持たないため、確保・解放するスレッドごとに別のヒープへ
    // 分けてある(DX12Device.hのGetAssetSrvCpuHeap/GetRenderSrvCpuHeapのコメント参照)。
    //
    // アセット側: Bistro Exteriorでテクスチャ182枚 + RT統合バッファ5本 + TLAS 1本に加え、
    // メッシュごとのジオメトリバッファが効く。メッシュ数は約400で、1メッシュあたり
    // 頂点1 + メッシュレット3 + インデックス1 = 5本(ShaderReadableな頂点/インデックスは
    // メッシュシェーダーとソフトウェアラスタライザのどちらかが使える環境でのみ作られる)。
    // 合計で 182 + 400×5 + 6 ≒ 2188 だった。
    // シーン切り替え時は旧シーンを先に破棄してから新シーンを読むため二重確保は起きない。
    //
    // 【4096から16384へ引き上げた理由】上の見積もりは「1モデルに数百メッシュ」という
    // 構成しか想定していない。多数の.kmodelを[Model]として並べるシーン(PLATEAUの
    // 東京23区LOD1は671タイル=671モデル)では、モデルごとに1x1のプレースホルダ3枚が
    // 加わるため 671×(3+2+3) = 5368 となり、約512モデル目でthrowしていた。
    // プレースホルダをデバイス単位で共有するようにしたので実際の消費は
    // 671×5 + 3 = 3358 まで下がるが、モデル数が増える方向に余裕を持たせておく。
    // 非シェーダー可視ヒープなのでCPUメモリしか消費せず(16384エントリで約512KB)、
    // 引き上げの副作用は小さい。
    //
    // 【16384から65536へさらに引き上げた理由】bindless区画へ登録するディスクリプタは、
    // 必ずこの非シェーダー可視ヒープのSRVを元にコピーされる。つまり
    // **bindlessの実質的な上限を決めているのはkBindlessDescriptorCapacityではなくこちら**で、
    // 片方だけ上げても意味がない。PLATEAU LOD2は1タイルでテクスチャ1,714 +
    // メッシュ1,715×2本 ≒ 5,149を消費するため、複数タイル常駐で16384を超える。
    // 65536エントリでも約2MB(1ディスクリプタ32バイト換算)。
    //
    // レンダー側: レンダーターゲットのSRV/UAVに加え、Hi-Zとブルームのミップ別UAV、
    // IBL・反射プローブのキューブマップ(プローブ数×6面×ミップ数のUAV)が効く。
    // どちらも非シェーダー可視ヒープでCPUメモリのみを消費するため、余裕を持った値にしておく
    constexpr uint32_t kAssetSrvCpuHeapCapacity = 65536;
    constexpr uint32_t kRenderSrvCpuHeapCapacity = 2048;
}
