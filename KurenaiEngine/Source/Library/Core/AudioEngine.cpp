#include "AudioEngine.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace Kurenai::Core
{
    // 単発再生(loop=false)のボイスが再生を終えたことを検知するためのコールバック。
    // OnStreamEndはXAudio2の内部スレッドから呼ばれ、その中でDestroyVoiceを呼ぶことは禁止されているため
    // (公式ドキュメントで明記。呼ぶとデッドロック/クラッシュする)、ここではFinishedフラグを立てるのみとし、
    // 実際のボイス破棄はAudioEngine::CleanupFinishedOneShotVoices(呼び出し元スレッド側)で行う
    struct AudioEngine::OneShotVoiceCallback : public IXAudio2VoiceCallback
    {
        IXAudio2SourceVoice* Voice = nullptr;
        std::atomic<bool> Finished{ false };

        void __stdcall OnStreamEnd() noexcept override { Finished.store(true); }
        void __stdcall OnVoiceProcessingPassStart(UINT32) noexcept override {}
        void __stdcall OnVoiceProcessingPassEnd() noexcept override {}
        void __stdcall OnBufferStart(void*) noexcept override {}
        void __stdcall OnBufferEnd(void*) noexcept override {}
        void __stdcall OnLoopEnd(void*) noexcept override {}
        void __stdcall OnVoiceError(void*, HRESULT) noexcept override {}
    };

    namespace
    {
        void ThrowIfFailed(HRESULT hr, const char* message)
        {
            if (FAILED(hr))
            {
                throw std::runtime_error(std::string(message) + " (HRESULT: 0x" + std::to_string(static_cast<uint32_t>(hr)) + ")");
            }
        }

        // 4文字のチャンクIDを比較する
        bool IdEquals(const char id[4], const char* expected)
        {
            return std::memcmp(id, expected, 4) == 0;
        }
    }

    AudioEngine::AudioEngine()
    {
        ThrowIfFailed(XAudio2Create(&m_XAudio2, 0), "XAudio2の初期化に失敗しました");
        ThrowIfFailed(m_XAudio2->CreateMasteringVoice(&m_MasteringVoice), "マスタリングボイスの作成に失敗しました");
    }

    AudioEngine::~AudioEngine()
    {
        if (m_MasteringVoice)
        {
            m_MasteringVoice->DestroyVoice();
        }
    }

    uint32_t AudioEngine::LoadSound(const std::wstring& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("WAVファイルを開けませんでした");
        }

        char riffId[4]{};
        uint32_t riffSize = 0;
        char waveId[4]{};
        file.read(riffId, 4);
        file.read(reinterpret_cast<char*>(&riffSize), 4);
        file.read(waveId, 4);
        if (!file || !IdEquals(riffId, "RIFF") || !IdEquals(waveId, "WAVE"))
        {
            throw std::runtime_error("WAVファイルの形式が不正です(RIFF/WAVEヘッダが見つかりません)");
        }

        auto sound = std::make_unique<SoundData>();
        bool hasFormat = false;
        bool hasData = false;

        while (file && !(hasFormat && hasData))
        {
            char chunkId[4]{};
            uint32_t chunkSize = 0;
            file.read(chunkId, 4);
            file.read(reinterpret_cast<char*>(&chunkSize), 4);
            if (!file)
            {
                break;
            }

            if (IdEquals(chunkId, "fmt "))
            {
                std::vector<uint8_t> fmtBytes(chunkSize);
                file.read(reinterpret_cast<char*>(fmtBytes.data()), chunkSize);
                // fmtチャンクは16バイト(cbSize無し)の場合と18バイト以上(cbSize有り)の場合があるため、
                // 小さい方に合わせてコピーする(WAVEFORMATEXは事前にゼロ初期化済みなのでcbSize欠落時は0のまま)
                const size_t copySize = fmtBytes.size() < sizeof(WAVEFORMATEX) ? fmtBytes.size() : sizeof(WAVEFORMATEX);
                std::memcpy(&sound->Format, fmtBytes.data(), copySize);
                hasFormat = true;
            }
            else if (IdEquals(chunkId, "data"))
            {
                sound->PcmData.resize(chunkSize);
                file.read(reinterpret_cast<char*>(sound->PcmData.data()), chunkSize);
                hasData = true;
            }
            else
            {
                file.seekg(chunkSize, std::ios::cur);
            }

            // RIFFチャンクは偶数バイト境界に揃えられる(奇数サイズの場合1バイトのパディングが付く)
            if (chunkSize % 2 != 0)
            {
                file.seekg(1, std::ios::cur);
            }
        }

        if (!hasFormat || !hasData)
        {
            throw std::runtime_error("WAVファイルの解析に失敗しました(fmt/dataチャンクが見つかりません)");
        }

        m_Sounds.push_back(std::move(sound));
        return static_cast<uint32_t>(m_Sounds.size() - 1);
    }

    void AudioEngine::CleanupFinishedOneShotVoices()
    {
        for (size_t i = 0; i < m_PendingOneShotCallbacks.size();)
        {
            if (m_PendingOneShotCallbacks[i]->Finished.load())
            {
                m_PendingOneShotCallbacks[i]->Voice->DestroyVoice();
                m_PendingOneShotCallbacks.erase(m_PendingOneShotCallbacks.begin() + i);
            }
            else
            {
                ++i;
            }
        }
    }

    void AudioEngine::PlaySound(uint32_t soundIndex, float volume, bool loop)
    {
        // 前回までに再生完了した単発ボイスをここでまとめて破棄する(呼び出し元スレッド側)
        CleanupFinishedOneShotVoices();

        if (soundIndex >= m_Sounds.size())
        {
            return;
        }
        const SoundData& sound = *m_Sounds[soundIndex];

        // ループ再生は停止APIが無いため使い捨てコールバックを付けない(AudioEngine破棄まで鳴り続ける)。
        // 単発再生は再生完了を検知するコールバックを付け、CleanupFinishedOneShotVoicesで破棄する
        std::unique_ptr<OneShotVoiceCallback> callback = loop ? nullptr : std::make_unique<OneShotVoiceCallback>();

        IXAudio2SourceVoice* voice = nullptr;
        const HRESULT hr = m_XAudio2->CreateSourceVoice(&voice, &sound.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, callback.get());
        ThrowIfFailed(hr, "サウンドボイスの作成に失敗しました");

        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(sound.PcmData.size());
        buffer.pAudioData = sound.PcmData.data();
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;

        voice->SetVolume(volume);
        voice->SubmitSourceBuffer(&buffer);
        voice->Start();

        if (callback)
        {
            callback->Voice = voice;
            m_PendingOneShotCallbacks.push_back(std::move(callback));
        }
    }
}
