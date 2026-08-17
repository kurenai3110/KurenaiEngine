#pragma once

#include <xaudio2.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

#include "KurenaiTypes.h"

// dllexportされたクラスが非export型(Microsoft::WRL::ComPtr<IXAudio2>など)をメンバに持つことによる
// C4251警告を抑制する。KurenaiEngine.dllと各サンプルは常に同一コンパイラ・同一ランタイム
// ライブラリ設定でビルドされるため、実務上は問題にならない
#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Core
{
    // XAudio2による簡易なサウンド再生。WAV(PCM)ファイルの読み込みと再生、再生中ボイスの停止・
    // 音量変更、マスター音量に対応した最小限の実装(効果音・BGM程度の単純な再生が目的で、
    // ピッチ・パン・カテゴリ別のバス等は持たない)。
    // BGMのフェードは、SetVoiceVolumeを毎フレーム呼ぶ形で呼び出し側が実装する。
    // 使用にはCoInitializeEx済みであること(WICテクスチャ読み込みと同じCOM初期化要件)
    class KURENAI_LIB_API AudioEngine
    {
    public:
        AudioEngine();
        ~AudioEngine();

        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        // WAV(PCM)ファイルを読み込み、再生用に登録する。戻り値は以後PlaySoundへ渡すインデックス
        uint32_t LoadSound(const std::wstring& filePath);

        // 登録済みサウンドを再生する。呼び出しのたびに新しいボイスを生成するため、同じサウンドを
        // 重ねて(前の再生が終わる前に再度)鳴らすことができる。volumeは0.0〜1.0。
        // 戻り値はStopSoundへ渡すボイスID(0は無効値として予約。実際に発行されるIDは1以上)
        uint64_t PlaySound(uint32_t soundIndex, float volume, bool loop);

        // PlaySoundが返したボイスIDを指定して再生を即座に停止する。単発再生(loop=false)は通常
        // 自動的に終了するため呼ぶ必要はないが、ループ再生(loop=true)を止めるにはこれを呼ぶ。
        // 既に終了済み/無効なIDの場合は何もしない
        void StopSound(uint64_t voiceId);

        // 再生中のボイスの音量を変更する。volumeは0.0〜1.0(範囲外はログを出してクランプする)。
        // 既に終了済み/無効なIDの場合は何もしない(単発再生はいつ終わるか呼び出し側には
        // 分からないため、これは正常系。ログも出さない)。
        // BGMのフェードイン/フェードアウトはこれを毎フレーム呼ぶことで実現する
        void SetVoiceVolume(uint64_t voiceId, float volume);

        // 全ボイスに掛かるマスター音量。0.0〜1.0(範囲外はログを出してクランプする)。
        // 個々のボイスの音量とは掛け算になる
        void SetMasterVolume(float volume);
        float GetMasterVolume() const { return m_MasterVolume; }

    private:
        struct SoundData
        {
            WAVEFORMATEX Format{};
            std::vector<uint8_t> PcmData;
        };
        // IXAudio2VoiceCallback実装の詳細(定義は.cpp)。前方宣言のみ公開ヘッダーに出す
        struct OneShotVoiceCallback;

        // 再生中のボイス1件ぶんの情報。Callbackは単発再生(loop=false)のみ持ち、再生完了検知に使う
        // (ループ再生はStopSoundで明示的に止めるまで自動終了しないためCallback不要)
        struct VoiceEntry
        {
            IXAudio2SourceVoice* Voice = nullptr;
            std::unique_ptr<OneShotVoiceCallback> Callback;
        };

        // 再生完了を検知済みだが未破棄の単発ボイスをまとめて破棄する。XAudio2のコールバックは
        // 内部スレッドから呼ばれ、そのスレッド内でDestroyVoiceを呼ぶことは禁止されているため
        // (公式ドキュメントで明記)、破棄は呼び出し元スレッド(PlaySound呼び出し時)で行う
        void CleanupFinishedOneShotVoices();

        Microsoft::WRL::ComPtr<IXAudio2> m_XAudio2;
        IXAudio2MasteringVoice* m_MasteringVoice = nullptr;
        std::vector<std::unique_ptr<SoundData>> m_Sounds;
        std::unordered_map<uint64_t, VoiceEntry> m_ActiveVoices;
        uint64_t m_NextVoiceId = 1;
        // GetMasterVolume用。IXAudio2MasteringVoice::GetVolumeでも取れるが、
        // SetMasterVolumeでクランプした後の「エンジンが認識している値」を返したいので保持する
        float m_MasterVolume = 1.0f;
    };
}

#pragma warning(pop)
