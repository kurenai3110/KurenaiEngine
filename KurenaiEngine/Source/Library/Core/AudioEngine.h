#pragma once

#include <xaudio2.h>

#include <cstdint>
#include <memory>
#include <string>
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
    // XAudio2による簡易なサウンド再生。WAV(PCM)ファイルの読み込みと再生のみに対応した最小限の実装で、
    // 再生中のサウンドを個別に停止・音量変更する機能は持たない(効果音・BGM程度の単純な再生が目的)。
    // 使用にはCoInitializeEx済みであること(WICテクスチャ読み込みと同じCOM初期化要件)
    class KURENAI_API AudioEngine
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
        // loop=trueの場合、そのボイスはAudioEngineが破棄されるまで無限ループし続ける
        // (停止APIは提供していないため、ループ再生は用途を選んで使うこと)
        void PlaySound(uint32_t soundIndex, float volume, bool loop);

    private:
        struct SoundData
        {
            WAVEFORMATEX Format{};
            std::vector<uint8_t> PcmData;
        };
        // IXAudio2VoiceCallback実装の詳細(定義は.cpp)。前方宣言のみ公開ヘッダーに出す
        struct OneShotVoiceCallback;

        // 再生完了を検知済みだが未破棄の単発ボイスをまとめて破棄する。XAudio2のコールバックは
        // 内部スレッドから呼ばれ、そのスレッド内でDestroyVoiceを呼ぶことは禁止されているため
        // (公式ドキュメントで明記)、破棄は呼び出し元スレッド(PlaySound呼び出し時)で行う
        void CleanupFinishedOneShotVoices();

        Microsoft::WRL::ComPtr<IXAudio2> m_XAudio2;
        IXAudio2MasteringVoice* m_MasteringVoice = nullptr;
        std::vector<std::unique_ptr<SoundData>> m_Sounds;
        std::vector<std::unique_ptr<OneShotVoiceCallback>> m_PendingOneShotCallbacks;
    };
}

#pragma warning(pop)
