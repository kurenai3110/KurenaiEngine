#pragma once

#include <string>
#include <vector>

#include "Assets/ShaderPackage.h"

namespace Kurenai::ShaderPacker
{
    // .hlsl のソースから、エントリポイントとそのステージを構文的に検出する。
    //
    // 【手書きのマニフェストを持たない理由】「どのシェーダーのどのエントリを焼くか」の
    // 一覧を人が管理すると、エンジン側へシェーダーを足したときに登録し忘れ、
    // ビルドは通るのに起動時に落ちる、という壊れ方をする。ソースから直接見つければ
    // その障害モードごと消える。エンジンが実際に使う本数(74本)より多く焼くことになるが、
    // 増分ビルドで吸収できる範囲に収まる。
    //
    // 【検出規則の出どころ】.claude/skills/shader-check/scripts/check-shaders.ps1 の
    // Get-Entries と同一の正規表現を移植したもの。あちらは現行の全 .hlsl に対して
    // 失敗0で回っており、この規則がこのコードベースの命名規約と噛み合うことは実証済み。
    // 【両方を直すこと】規則を変えるときは check-shaders.ps1 側も必ず合わせる
    // (片方だけ直すと、検証が通るのに焼けない/焼けるのに検証されないシェーダーが出る)。

    struct ScannedEntry
    {
        std::string Name;
        Assets::ShaderPackageStage Stage = Assets::ShaderPackageStage::Vertex;
    };

    // 検出したエントリを (ステージ, 名前) で重複排除して返す。1つも無ければ空。
    // 渡すソースは ExpandIncludes で展開済みのものにすること
    std::vector<ScannedEntry> ScanEntryPoints(const std::string& source);

    // #include "..." を再帰的に展開したソースを返す。
    //
    // 【展開しないと取りこぼす】エントリポイントは .hlsl 本体にあるとは限らない。
    // 例えば GBuffer.hlsl の VSMain は実体が GBufferCommon.hlsli:224 にあり、
    // .hlsl だけを走査すると「頂点シェーダーが無いファイル」に見える
    // (エンジンは GBuffer.hlsl の VSMain を要求するので、取りこぼすと起動時に落ちる)。
    // .claude/skills/shader-check/scripts/check-shaders.ps1 は .hlsl しか走査しないため、
    // この種のエントリを今まで一度も検証できていない。
    //
    // 同じファイルは1度だけ展開する(#pragma once 相当。循環インクルードで止まらなくなるのを防ぐ)。
    // 山括弧の #include <...> は使っていないため扱わない。
    // 読めないインクルードは、そこだけ空にして続行する(本当に必要ならコンパイラが弾く)
    std::string ExpandIncludes(const std::wstring& filePath, std::string& outError);

    // プロファイル文字列の接頭辞("vs"/"ps"/"cs"/"as"/"ms")
    const char* StagePrefix(Assets::ShaderPackageStage stage);
}
