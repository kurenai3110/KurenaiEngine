#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ShaderPackage.h"

namespace Kurenai::Assets
{
    // .kshader(事前コンパイル済みシェーダー)の読み込み。書式の定義はShaderPackage.hを参照。
    // 失敗時はstd::runtime_errorを投げる(LoadModel / LoadShowと同じ扱い)。
    //
    // 【DLLエクスポートしない理由】この形式を読むのはKurenaiEngineLibrary内のRHI実装
    // (DX11Device::CreateShader / DX12Device::CreateShader)だけで、
    // KurenaiEngine3D.dll側はIRHIDevice::CreateShader越しにしか触らない。
    // 書き込み側(KurenaiShaderPacker.exe)はこのソースを直接コンパイルして取り込む。

    // 1エントリ。ファイル上のShaderEntryから、StringPoolの参照を解決した形
    struct ShaderPackageEntry
    {
        std::string Name;                 // エントリポイント名("VSMain"等)
        std::string Profile;              // 実際に使ったプロファイル("ps_6_6"等。ログ用)
        ShaderPackageStage Stage = ShaderPackageStage::Vertex;
        ShaderVariant Variant = ShaderVariant::Dxbc50;
        uint32_t BytecodeOffset = 0;      // Bytecode内のオフセット
        uint32_t BytecodeSize = 0;
    };

    struct ShaderPackageData
    {
        std::vector<ShaderPackageEntry> Entries;
        // 全エントリのバイトコードを連結したもの。個々のエントリはOffset/Sizeで指す
        std::vector<uint8_t> Bytecode;
        // 収録済みバリアントのビット集合(1u << ShaderVariant)
        uint32_t VariantMask = 0;
        // Debug構成で焼かれたパッケージか(診断用)
        bool DebugBuild = false;

        bool HasVariant(ShaderVariant variant) const
        {
            return (VariantMask & (1u << static_cast<uint32_t>(variant))) != 0u;
        }
    };

    ShaderPackageData LoadShaderPackage(const std::wstring& filePath);

    // 指定のエントリポイント・ステージ・バリアントのバイトコードを引く。
    // 見つからなければnullptrを返す(呼び出し側が縮退させるか、明示的なエラーにするかを決める)
    const ShaderPackageEntry* FindShaderEntry(
        const ShaderPackageData& package,
        const std::string& entryPoint,
        ShaderPackageStage stage,
        ShaderVariant variant);

    // パス→パッケージのキャッシュ。
    //
    // 【なぜ要るか】1つの.kshaderは複数のエントリから引かれる
    // (GBuffer.kshaderはVSMain / PSMain / PSMainCutoutの3回)。キャッシュが無いと
    // 同じファイルを何度も開いて読み直すことになる。
    //
    // 【なぜ解放できるか】シェーダーの生成は起動時のCreateSceneResources()の1回きりで、
    // 以降このデータは誰も読まない。保持し続ける意味が無いのでClear()で明示的に捨てる
    class ShaderPackageCache
    {
    public:
        // 読み込み済みならそれを返し、未読なら読み込む。失敗時はstd::runtime_errorを投げる
        const ShaderPackageData& Get(const std::wstring& filePath);

        void Clear();

        // 現在キャッシュしているバイトコードの合計バイト数(ログ用)
        size_t TotalBytecodeSize() const;

    private:
        std::map<std::wstring, ShaderPackageData> m_Packages;
    };
}
