// bindless(HLSLのResourceDescriptorHeap、シェーダーモデル6.6)の共有ヘッダー。
// NormalEncoding.hlsli / Samplers.hlsli と同じ「全シェーダ共通の宣言を1か所へ集める」枠組みに従う。
//
// 【何を可能にするのか】従来このエンジンのシェーダーは、リソースをt0〜t20の固定スロットで
// しか受け取れなかった。HLSLはリソースそのものを実行時の番号で選べないため、
// 「ヒットした三角形のマテリアルのテクスチャを引く」「メッシュレットごとに違うバッファを読む」
// といった処理が書けず、レイトレーシングのヒット面はマテリアルの定数色しか使えなかった
// (Assets::RaytracingMaterialの旧「Phase 1の制約」)。
// ResourceDescriptorHeapはシェーダ可視ディスクリプタヒープ全体を1つの配列として直接添字でき、
// この制約を取り払う。番号を払い出すのはC++側のDX12BindlessTableで、
// IRHIDevice::RegisterBindlessが返した値がそのままここでの添字になる。
//
// 【使えない環境がある】必要なのはDX12・SM 6.6・リソースバインディングTier 3・
// SM 6.6を知っているdxcompiler.dllのすべてで、DX11には存在しない。
// DX12ShaderCompilerは条件を満たすときだけ -D KURENAI_BINDLESS=1 を渡すため、
// このヘッダーはマクロの有無で実装を切り替える。
//
// 【切り替えの原則】非対応側は「エラーにする」のではなく、**従来のプレースホルダーと
// 同じ既定値を返す**。こうしておけば消費側のシェーダーに #if を書かずに済み、
// bindlessが無い環境では自動的に従来と同じ見た目(定数色のみ)へ縮退する。

#ifndef KURENAI_BINDLESS_HLSLI
#define KURENAI_BINDLESS_HLSLI

// ディスクリプタが割り当てられていないことを表す番号。
// C++側(Source/Library/RHI/RHIBindless.h の kInvalidBindlessIndex)と同じ値にすること
static const uint kInvalidBindlessIndex = 0xFFFFFFFFu;

// bindless番号で指したTexture2Dをサンプルする。
//
// index が無効、あるいは環境がbindless非対応の場合は defaultValue をそのまま返す。
// 呼び出し側は用途に応じた既定値を渡すこと(このエンジンのプレースホルダーの規約):
//   ベースカラー / metallic-roughness / エミッシブ … float4(1, 1, 1, 1)  (白1x1)
//   法線マップ                                      … float4(0.5, 0.5, 1, 1) (フラット法線)
//
// 【SampleLevelでLODを明示する理由】レイトレーシングのヒット面にはピクセル間の
// UV勾配が存在せず、Sample()が使う暗黙のミップ選択ができない。呼び出し側が
// レイの距離などから決めたLODを渡す
float4 BindlessSampleLevel(uint index, SamplerState samplerState, float2 uv, float lod, float4 defaultValue)
{
#if defined(KURENAI_BINDLESS)
    if (index == kInvalidBindlessIndex)
    {
        return defaultValue;
    }

    Texture2D<float4> tex = ResourceDescriptorHeap[index];
    return tex.SampleLevel(samplerState, uv, lod);
#else
    // bindless非対応環境。引数はどれも使わず、従来のプレースホルダーと同じ値を返す
    // (HLSLには (void)x のような未使用を明示する書き方が無いため、そのまま放置する。
    //  未使用の引数はdxcの警告対象ではない)
    return defaultValue;
#endif
}

// bindless番号で構造化バッファを取り出すためのマクロ。
//
// 関数にできないのは、HLSL 2018には要素型を引数に取れる仕組みが無く、
// StructuredBuffer<T> を返す関数が書けないため(ResourceDescriptorHeap[i]は
// 代入先の型に応じて解決される)。使い方:
//
//   StructuredBuffer<Meshlet> meshlets = KURENAI_BINDLESS_BUFFER(MeshletBufferIndex);
//
// 【非対応環境では使えない】このマクロを使うシェーダーはbindlessを前提としたもの
// (メッシュシェーダー経路)に限られ、そこはデバイス側で既に弾かれているため、
// 上のBindlessSampleLevelのような既定値へのフォールバックは用意していない
#if defined(KURENAI_BINDLESS)
    #define KURENAI_BINDLESS_BUFFER(index) ResourceDescriptorHeap[index]
#endif

#endif // KURENAI_BINDLESS_HLSLI
