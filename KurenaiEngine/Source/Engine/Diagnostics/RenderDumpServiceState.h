#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RHI/IRHIDevice.h"
#include "RHI/IRHIBuffer.h"
#include "RHI/IRHITexture.h"

namespace Kurenai::Core { class RenderGraph; class Window; }

// 中間レンダーターゲットの生値ダンプ(-dumptex)と、パスマニフェストの書き出し(-passmanifest)。
// どちらも検証専用で、通常の描画には一切関わらない。
//
// 【テクスチャの表を持たない】どのテクスチャがダンプできるかを決めるのはエンジンで、
// この型は渡された表を引くだけにしてある。表はリサイズやバッファ精度の切り替えで
// ポインタごと作り直されるため、**呼ぶたびに作り直したものを渡すこと**。
// ここで抱えるとキャッシュになり、解放済みのテクスチャを指す。
namespace Kurenai::Diagnostics
{
    // 名前 -> テクスチャ の対応表。CreateRenderTargetsでテクスチャを増やしたら
    // BuildDumpableTextureTableにも足すこと(表の実体はそちらのコメントを参照)
    struct DumpableTexture
    {
        const char* Name = nullptr;
        RHI::IRHITexture* Texture = nullptr;
    };

    struct DumpableBuffer
    {
        const char* Name = nullptr;
        RHI::IRHIBuffer* Buffer = nullptr;
        uint32_t ElementCount = 0;
        uint32_t StrideInBytes = 0;
    };

    class RenderDumpService
    {
    public:
        // --- 起動オプションからの予約 ---
        void AddTextureDump(
            const wchar_t* name, const wchar_t* path, int mipLevel, int arraySlice, int frames, int stride);
        void AddBufferDump(const wchar_t* name, const wchar_t* path);
        void SetTextureDumpFrame(int frame);
        void SetExitAfterDump(bool enabled);
        void SetPassManifest(const wchar_t* path, int frames);

        // --- フレーム中に呼ぶもの。表とフレーム番号は毎回渡すこと ---
        void ApplyDebugNames(const std::vector<DumpableTexture>& table) const;
        void ApplyDebugNamesIfDirty(const std::vector<DumpableTexture>& table);
        // レンダーターゲットを作り直したら立てる。**毎フレーム焼かない**
        void MarkDebugNamesDirty() { m_DebugNamesDirty = true; }
        void IssueTextureDumps(
            Core::RenderGraph& graph, const std::vector<DumpableTexture>& table,
            uint32_t frameIndex, RHI::IRHIDevice& device);
        void IssueBufferDumps(
            Core::RenderGraph& graph, const std::vector<DumpableBuffer>& table,
            uint32_t frameIndex, RHI::IRHIDevice& device);
        void ResolveTextureDumps(uint32_t frameIndex, Core::Window* window, bool isDX12);
        void WritePassManifestIfDue(Core::RenderGraph& graph, uint32_t frameIndex, bool isDX12);

    private:
        // 連番ダンプの受け皿1枚ぶん。
        //
        // 【なぜ1枚では足りないのか】コピーを積んでから読めるようになるまで
        // kTextureDumpReadDelayFrames ぶん空ける必要がある。受け皿が1枚しか無いと、
        // 毎フレーム積んだときに**まだ読んでいない中身へ次のコピーを上書きしてしまう**。
        // エラーにはならず、静かに同じ絵が並ぶ or 途中のフレームが消えるという形で出る
        struct TextureDumpSlot
        {
            // 受け皿。m_DeviceはKurenaiEngineBase(基底)のメンバで、派生クラスのメンバは
            // 基底より先に破棄されるため、デバイスより後に解放される心配は無い
            // (MegaLightsの読み戻しも同じ理由で Passes::MegaLightsPasses のメンバに置いてある)
            std::unique_ptr<RHI::IRHITexture> Readback;
            // 【寸法は積むときに控える】あとで引き直すと、その間のリサイズで
            // 受け皿の中身と食い違う値をヘッダへ書いてしまう
            RHI::TextureReadbackDesc Desc{};
            // コピーを積んだフレーム番号。GPUの実行はCPUより数フレーム遅れるので、
            // 積んだ直後に読んではいけない(IRHICommandList::CopyTextureToReadback のコメント)。
            // **ファイルのFrameIndex欄にもこの値を書く** —— 画素の中身が属するのはこのフレーム
            uint32_t CopyFrame = 0;
            // 連番の何枚目か。ファイル名の _%04u になる
            uint32_t SequenceIndex = 0;
            // 読み戻しに失敗し続けたフレーム数。**無人実行が静かに固まるのを防ぐための打ち切り用**
            uint32_t FailedFrames = 0;
            // 積んであり、まだ回収していない
            bool Busy = false;
        };

        struct TextureDumpRequest
        {
            std::string Name; // 表の名前(ファイルのヘッダにも書く)
            std::wstring Path;
            uint32_t MipLevel = 0;
            uint32_t ArraySlice = 0;
            // 何枚撮るか。**既定1のときは受け皿も深さ1**なので、連番を使わない従来の
            // 呼び出しはメモリ使用量も発行のタイミングも1ミリも変わらない
            uint32_t TargetFrames = 1;
            // 何フレームおきに撮るか。1なら連続フレーム。
            // 【間隔を記録できることに意味がある】画面キャプチャの連写は撮影間隔が
            // 撮る側の都合で揺れ、同じ構成の2回で時間統計が3〜4倍動いた(61.7i)。
            // ここでは間隔が指定値として決まり、ファイルのFrameIndexから検算もできる
            uint32_t Stride = 1;
            // 実際に確保した受け皿の枚数。解像度が大きいと上限で削られるので、
            // kTextureDumpRingDepth とは一致しないことがある
            uint32_t RingDepth = 0;
            // 連番に異なる寸法の画像を混在させないため、最初の読み戻し形式を固定する。
            RHI::TextureReadbackDesc FirstDesc{};
            std::vector<TextureDumpSlot> Slots;
            // 積んだ枚数と、実際にファイルへ書けた枚数。
            // 【2つ分けて数える】これまでは「諦めた」も完了として扱われ、1枚も書けなくても
            // -exitafterdump が正常終了していた。書けた数を別に持って報告する
            uint32_t IssuedCount = 0;
            uint32_t WrittenCount = 0;
            // 直近で積んだフレーム(Strideの間引き用)と、最初に積んだフレーム(打ち切りの起点)
            uint32_t LastIssueFrame = 0;
            uint32_t FirstIssueFrame = 0;
            bool AnyIssued = false;
            bool Done = false;
        };

        struct BufferDumpRequest
        {
            std::string Name;
            std::wstring Path;
            std::unique_ptr<RHI::IRHIBuffer> Readback;
            uint32_t ElementCount = 0;
            uint32_t StrideInBytes = 0;
            uint32_t CopyFrame = 0;
            uint32_t FailedFrames = 0;
            bool Issued = false;
            bool Done = false;
        };

        // 読み戻しを何フレーム失敗し続けたら諦めるか。DX11のMap(DO_NOT_WAIT)は
        // GPUが詰まっていると何度も失敗しうるので、1フレームで諦めてはいけない
        static constexpr uint32_t kTextureDumpMaxFailedFrames = 60;
        // コピーを積んでから読むまでに空けるフレーム数(MegaLightsのダンプと同じ値)
        static constexpr uint32_t kTextureDumpReadDelayFrames = 5;
        // 遅延中のコピーを連続発行できる深さ。これ以上は回収より先に増えてメモリだけを使う。
        static constexpr uint32_t kTextureDumpRingDepth = kTextureDumpReadDelayFrames + 1;
        // 高解像度バッファの連番が無制限にメモリを消費しないための上限。
        static constexpr size_t kTextureDumpRingMaxBytes = 512ull * 1024 * 1024;

        std::vector<TextureDumpRequest> m_TextureDumps;
        std::vector<BufferDumpRequest> m_BufferDumps;
        // 何フレーム目で撮るか。負なら Passes::kMegaLightsAccumWarmup を使う
        // (新しい定数を作らないのは、あちらのコメントに書かれた「整定を待つ理由」が
        //  そのまま当てはまり、値が2つに割れると片方だけ直す事故が起きるため)
        int32_t m_TextureDumpFrame = -1;
        std::wstring m_PassManifestPath;
        uint32_t m_PassManifestTargetFrames = 1;
        uint32_t m_PassManifestIssuedFrames = 0;
        bool m_PassManifestIssued = false;
        bool m_ExitAfterDump = false;
        bool m_ExitAfterDumpRequested = false;
        // 名前を焼き直す必要があるか。レンダーターゲットを作り直すと立てる。
        // **毎フレーム焼かない** —— 43本のSetNameを60回/秒で呼ぶ意味がない
        bool m_DebugNamesDirty = true;

        // 1件ぶんをファイルへ書く。書けたらtrue
        bool WriteTextureDumpFile(
            const TextureDumpRequest& request, const TextureDumpSlot& slot, const std::vector<uint8_t>& pixels,
            bool isDX12) const;
        bool WriteBufferDumpFile(const BufferDumpRequest& request, const std::vector<uint8_t>& bytes) const;
    };
}
