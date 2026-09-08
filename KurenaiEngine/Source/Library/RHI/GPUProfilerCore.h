#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "IRHIGPUProfiler.h"

// GPU区間計測のうち、**タイムスタンプの取り方に依らない部分**をDX11とDX12で共有する層。
//
// 【何をここへ置き、何を置かないのか】置くのはリングの段数と進め方、1フレームあたりの
// 区間数の上限とその超過時の振る舞い、区間名の保持、そして結果の集計。
// 置かないのは ID3D11Query / ID3D12QueryHeap の発行と読み出し —— そこは機構が別物。
//
// 【なぜ共有するのか】上限(kMaxScopesPerFrame)がバックエンドごとに違うと、
// 同じシーンでもDX11とDX12で計測できるパスの数が変わり、GPU Frame Timeを比べられなくなる。
// 以前は両方のヘッダに同じ値を書き、「必ず同じ値にすること」というコメントで
// 手で同期させていた。段数・上限・集計方法をここへ寄せると、その義務自体が消える
namespace Kurenai::RHI
{
    class GPUProfilerCore
    {
    public:
        // GPU実行がCPUの記録より数フレーム遅れてもクエリ結果を取りこぼさないためのリングバッファ段数
        static constexpr uint32_t kFrameLatency = 4;

        // RenderGraphは1フレームに34種以上のパスを登録し、DDGI有効シーンではさらにプローブ数分
        // (DDGIProbesPerFrame、既定16)が加算される。この値が足りないと超過した区間の計測が捨てられ、
        // 「各パスの計測値の合計」であるGPU Frame Time(AddScopeResult参照)まで過小報告される。
        //
        // 64から96へ上げた理由: DDGI有効シーンの常時分(固定パス + Shadow4本 + ModelCull2本 +
        // DDGI16本)だけで50本前後に達しており、プローブのベイクが走るフレームではさらに増える。
        // 64のままだと数パス足すだけで超過し、「そのパスはタダらしい」という誤った結論に直結する。
        //
        // 【増やす費用はバックエンドで違う】DX12はクエリヒープとリードバックバッファの
        // バイト数(kFrameLatency × (2 + 上限×2) × 8B)だけで済むが、DX11は ID3D11Query の
        // オブジェクトを1区間につき2個、リングの段数だけ前もって作る(96なら776個)。
        // それでも生成は起動時の1回きり
        static constexpr uint32_t kMaxScopesPerFrame = 96;

        // 1フレームぶんの記録。クエリそのものはバックエンド側が同じ添字の配列で持つ
        struct FrameSlot
        {
            std::array<std::string, kMaxScopesPerFrame> ScopeNames;
            uint32_t ScopeCount = 0;
            // EndFrame済みで結果の確定を待っている状態か
            bool Pending = false;
        };

        // backendTag は上限超過の警告に使うログのタグ("DX11" / "DX12")
        explicit GPUProfilerCore(const char* backendTag)
            : m_BackendTag(backendTag)
        {
        }

        // いま記録しているスロットの番号。バックエンドはこの番号で自分のクエリを選ぶ
        uint32_t GetWriteIndex() const { return m_WriteIndex; }
        FrameSlot& GetWriteSlot() { return m_Slots[m_WriteIndex]; }

        // BeginFrameの先頭で呼ぶ。区間数を0へ戻す(結果の確定は呼び出し側がPendingを見て行う)
        void ResetWriteSlotScopes() { GetWriteSlot().ScopeCount = 0; }

        // 区間の開始を受け付ける。受け付けたら outScopeIndex に何本目かを書いて true。
        // 上限を超えている場合は**一度だけ**警告を出して false を返す
        // (毎フレーム出るとログのflushでフレーム時間そのものが崩れるため)。
        // 計測のみスキップし、描画には影響しない
        bool TryBeginScope(const std::string& name, uint32_t& outScopeIndex);

        // 区間の終了を受け付ける。TryBeginScopeと同じ番号を outScopeIndex に書いてから
        // 区間数を1つ進める。上限を超えている場合は false(このときは何も進めない)
        bool TryEndScope(uint32_t& outScopeIndex);

        // EndFrameの末尾で呼ぶ。いまのスロットを結果待ちにして、リングを1つ進める
        void MarkFrameRecorded();

        // --- 結果の確定 ---------------------------------------------------------------------
        // BeginResults → 区間ごとに AddScopeResult → EndResults の順で呼ぶ。
        // 途中で抜けると前回の値が残ったままになるので、必ず最後まで通すこと

        void BeginResults(uint32_t scopeCount);

        // 1区間ぶんの結果を積む。frequency は0でないこと(0なら呼び出し側が確定を諦める)
        void AddScopeResult(const std::string& name, uint64_t beginTicks, uint64_t endTicks, uint64_t frequency);

        void EndResults();

        const std::vector<GPUTimingResult>& GetResults() const { return m_Results; }
        float GetTotalFrameTimeMs() const { return m_TotalFrameTimeMs; }

    private:
        const char* m_BackendTag;
        std::array<FrameSlot, kFrameLatency> m_Slots;
        uint32_t m_WriteIndex = 0;

        std::vector<GPUTimingResult> m_Results;
        float m_TotalFrameTimeMs = 0.0f;
        // BeginResults〜EndResultsのあいだの積算。確定するまで m_TotalFrameTimeMs へは入れない
        float m_PendingTotalMs = 0.0f;
        bool m_ScopeOverflowLogged = false;
    };
}
