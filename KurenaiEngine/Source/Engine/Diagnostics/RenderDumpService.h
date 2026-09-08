#pragma once

#include <cstdint>
#include <string>

namespace Kurenai
{
    // 連番で書き出すダンプの出力パスを作る。targetFramesが1以下ならpathをそのまま返し、
    // 複数枚なら拡張子の直前へ "_0000" 形式の連番を差し込む。
    //
    // 【いまの呼び出し元はRenderDumpService.cppの中だけ】パスマニフェストの書き出しを
    // 段階6.6でそちらへ移したため、翻訳単位をまたぐ必要は無くなっている。
    // KurenaiEngine3D.hは公開ヘッダなので、また跨ぐことになってもここへ置くこと
    std::wstring MakeTextureDumpSequencePath(const std::wstring& path, uint32_t targetFrames, uint32_t sequenceIndex);
}
