#pragma once

#include <cstdint>

// KurenaiEngine専用シェーダーパッケージ形式(.kshader)の定義。
// KurenaiShaderPacker(ビルド時に走るオフラインツール)とランタイム(ShaderLoader.cpp)の
// 両方から参照される、フォーマットの単一の正とするヘッダー。ヘッダオンリーで
// KURENAI_API(DLLエクスポート)は不要(値はコンパイル時定数、構造体はPOD)。
//
// 設計方針: 起動時に .hlsl をその場でコンパイルしていたものを、ビルド時に焼いた
// バイトコードへ置き換える。実行時コンパイルは残していない。
// 実測(RTX 4070 Ti / Release / Sample3D)で、この起動時コンパイルはDX11で約17.6秒、
// DX12で約2.1秒を占めていた。DX11が桁違いに遅いのは、fxc(SM 5.0)がSM 6.xの
// dxcより遅いことに加え、80本近くを1本ずつ逐次コンパイルしていたため。
//
// バイト列はすべてリトルエンディアン。#pragma packは使わない
// (各構造体は自然アラインメントのままパディングが生じない配置に設計済みで、
// static_assertでサイズを固定している。フィールドの追加・削除・並び替えを行う場合は
// このstatic_assertも必ず更新し、ランタイム側のVersion検証に頼って互換性を保つこと)。
// 文字列はUTF-8・NUL終端なし(長さで管理)で、.kmodelのStringPoolと同じ規約に従う。

namespace Kurenai::Assets
{
    // === .kshader ===
    //
    // ファイルレイアウト:
    //   [ShaderPackageHeader]
    //   [ShaderEntry × EntryCount]
    //   [StringPool (StringPoolSize bytes)]   ← エントリポイント名とプロファイル文字列
    //   [Bytecode   (BytecodeSize bytes)]     ← 各ShaderEntryのBytecodeOffset/Sizeが指す
    //
    // 【粒度は .hlsl 1本につき 1パッケージ】GBuffer.hlsl → GBuffer.kshader。
    // .kmodelが1アセット1ファイルなのと揃えてあり、増分ビルドの単位がそのまま
    // ファイル単位になる。1つのパッケージには、そのファイルが持つ全エントリポイントの、
    // 全バリアントぶんのバイトコードが入る。

    constexpr char kShaderMagic[4] = { 'K', 'S', 'H', 'D' };
    constexpr uint32_t kShaderVersion = 1;

    // シェーダーステージ。RHI::ShaderStageと同じ並びだが、ファイル形式はRHIに依存させたくないため
    // ここで独立に定義する(RHI側のenumを並び替えても、焼き済みの.kshaderの意味が変わらないように)。
    // 変換はShaderLoader.hのToPackageStage/ToRHIStageが受け持つ
    enum class ShaderPackageStage : uint32_t
    {
        Vertex = 0,
        Pixel = 1,
        Compute = 2,
        Amplification = 3,
        Mesh = 4,
    };

    // バリアント。同じエントリポイントを、実行環境の能力に応じて使い分けるために複数焼いておく。
    //
    // 【なぜ *_6_0 〜 *_6_4 の段を作らないか】インラインレイトレーシング(RayQuery)も
    // メッシュシェーダーもSM 6.5以上を要求するため、6.0〜6.4のデバイスではどのみち
    // それらの機能が無効になる。そして D3D12 は DXBC(SM 5.x)のバイトコードを受け付けるので、
    // その層は Dxbc50 へ落とせば従来の「dxcompiler.dllが無いときのd3dcompilerフォールバック」と
    // まったく同じ縮退になる。段を増やすとビルド時間だけが増えて、得るものが無い
    enum class ShaderVariant : uint32_t
    {
        // D3DCompile(d3dcompiler_47) / vs_5_0・ps_5_0・cs_5_0 / defineなし。
        // DX11の全経路と、SM 6.5未満のDX12デバイスが使う。
        // SM6専用機能(RayQuery・ResourceDescriptorHeap等)を使うファイルと
        // Amplification/Meshステージには存在しない
        Dxbc50 = 0,
        // dxc / *_6_5 / defineなし。DX12でbindless非対応の環境が使う
        Dxil65 = 1,
        // dxc / *_6_6 / KURENAI_BINDLESS=1。DX12でbindless対応の環境が使う
        Dxil66 = 2,
    };
    constexpr uint32_t kShaderVariantCount = 3;

    // Flagsのビット
    // bit0: Debug構成で焼いた(dxcなら -Zi -Qembed_debug -Od、D3DCompileなら
    //       D3DCOMPILE_DEBUG|D3DCOMPILE_SKIP_OPTIMIZATION)。
    //       Release構成の実行ファイルがDebugのパッケージを掴んでいないかを診断するためだけに持つ
    constexpr uint32_t kShaderFlagDebugBuild = 1u << 0;

    struct ShaderPackageHeader
    {
        char     Magic[4];        // 'K','S','H','D' (kShaderMagic)
        uint32_t Version;         // kShaderVersion。不一致なら読み込み拒否
        uint32_t EntryStride;     // sizeof(ShaderEntry)。不一致なら読み込み拒否
        uint32_t EntryCount;      // 収録されている(エントリポイント × バリアント)の総数
        uint32_t StringPoolSize;
        uint32_t BytecodeSize;
        uint32_t Flags;           // kShaderFlagDebugBuild
        // 収録済みバリアントのビット集合(1u << ShaderVariant)。
        // ビルドマシンのWindows SDKが古くdxcがSM 6.6を知らない場合、Dxil66のビットが立たない。
        // ランタイムはこれを見てbindlessの可否を決める(実行時にdxcのバージョンを見る代わり)
        uint32_t VariantMask;
        uint32_t Reserved[4];     // 0固定
    };
    static_assert(sizeof(ShaderPackageHeader) == 48, "ShaderPackageHeaderのレイアウトは48バイト固定");

    struct ShaderEntry
    {
        uint32_t NameOffset;      // StringPool内オフセット。エントリポイント名("VSMain"等)
        uint32_t NameLength;
        // StringPool内オフセット。実際に使ったプロファイル文字列("ps_6_6"等)。
        // 描画には使わず、ログとダンプツールのためだけに持つ
        uint32_t ProfileOffset;
        uint32_t ProfileLength;
        uint32_t Stage;           // ShaderPackageStage
        uint32_t Variant;         // ShaderVariant
        uint32_t BytecodeOffset;  // Bytecodeブロブ先頭からのオフセット
        uint32_t BytecodeSize;
    };
    static_assert(sizeof(ShaderEntry) == 32, "ShaderEntryのレイアウトは32バイト固定");
}
