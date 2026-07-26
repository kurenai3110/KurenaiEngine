#pragma once

#include <string>

#include "KurenaiTypes.h"
#include "RHI/IRHIDevice.h"
#include "Scene.h"

namespace Kurenai::Assets
{
    // .kscene(UTF-8のテキスト形式)を読み込み、参照されている各.kmodelをLoadModelで
    // 実際に読み込んでSceneを構築する。書式・パース規則の詳細はdocs/KurenaiEngine.htmlの
    // 「シーンファイル(.kscene)リファレンス」を参照。失敗時はstd::runtime_errorを投げる
    // (Core::Logger::Errorにファイル名・行番号・該当行の内容を添えて出力する)。
    //
    // sceneFilePath: 読み込む.ksceneのパス
    // assetRootDirectory: [Model]Pathの基準ディレクトリ(通常は<DLLフォルダ>/Assets/)。
    //                      .kmodel自身の内部パス(StringPool)の基準とは異なる点に注意
    KURENAI_API Scene LoadScene(RHI::IRHIDevice& device, const std::wstring& sceneFilePath, const std::wstring& assetRootDirectory);

    // ImGuiのシーン一覧構築用。[Model]の実体(.kmodel)は読み込まず、[Scene]Nameだけを取り出す
    // (省略されていればファイル名(拡張子なし)を返す)。失敗時はstd::runtime_errorを投げる
    KURENAI_API std::wstring ReadSceneName(const std::wstring& sceneFilePath);

    // KurenaiPacker.exeの--sceneモードが使う軽量な検証。書式の妥当性(ParseSceneFileと同じ規則)に加え、
    // 各[Model]Pathが指す.kmodelが実在し、ヘッダ(マジックナンバー・バージョン)が読めることまで確認する。
    // LoadSceneと異なりIRHIDeviceを必要とせず、ジオメトリ/テクスチャの実読み込みも行わない
    // (KurenaiPackerはGPUデバイスを持たないため)。失敗時はstd::runtime_errorを投げる
    KURENAI_API void ValidateScene(const std::wstring& sceneFilePath, const std::wstring& assetRootDirectory);
}
