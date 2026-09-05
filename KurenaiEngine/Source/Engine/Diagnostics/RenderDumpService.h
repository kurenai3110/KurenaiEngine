#pragma once

#include <cstdint>
#include <string>

namespace Kurenai
{
    // 連番で書き出すダンプの出力パスを作る。targetFramesが1以下ならpathをそのまま返し、
    // 複数枚なら拡張子の直前へ "_0000" 形式の連番を差し込む。
    //
    // 【翻訳単位をまたぐためここで宣言している】定義はRenderDumpService.cppにあるが、
    // Render()の中のパスマニフェスト書き出し(KurenaiEngine3D.cpp)からも呼ぶ。
    // KurenaiEngine3D.hは公開ヘッダなので、内部だけで使うものはこちらへ置く
    std::wstring MakeTextureDumpSequencePath(const std::wstring& path, uint32_t targetFrames, uint32_t sequenceIndex);
}
