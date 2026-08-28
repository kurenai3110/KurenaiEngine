#pragma once

#include <functional>
#include <string>

#include "KurenaiTypes.h"
#include "RHI/IRHIDevice.h"
#include "Scene.h"

namespace Kurenai::Assets
{
    // モデル1つを読み終えるたびにLoadSceneが呼ぶ進捗の通知。
    // loadedModels: ここまでに読み終えた数、totalModels: このシーンの[Model]の総数。
    //
    // 【なぜ要るか】LoadSceneは全[Model]を1回のブロッキング呼び出しで読むため、外からは
    // 「読み込み中」の真偽しか分からない。767モデルのシーンでは数十秒かかり、その間は
    // シーンが描かれない(UIとスカイボックスだけになる)ので、進んでいるのか固まったのかを
    // 区別できない。
    //
    // 【呼ばれるスレッド】LoadSceneを呼んだスレッドから同期的に呼ばれる(エンジンでは
    // Loaderスレッド)。UIを直接触ってはならず、atomicへ書くなど受け渡しに徹すること。
    // ここから投げられた例外はLoadScene側が握り潰してログに出し、読み込みは続行する
    using SceneLoadProgressCallback = std::function<void(size_t loadedModels, size_t totalModels)>;

    // .kscene(UTF-8のテキスト形式)を読み込み、参照されている各.kmodelをLoadModelで
    // 実際に読み込んでSceneを構築する。書式・パース規則の詳細はdocs/KurenaiEngine.htmlの
    // 「シーンファイル(.kscene)リファレンス」を参照。失敗時はstd::runtime_errorを投げる
    // (Core::Logger::Errorにファイル名・行番号・該当行の内容を添えて出力する)。
    //
    // sceneFilePath: 読み込む.ksceneのパス
    // assetRootDirectory: [Model]Pathの基準ディレクトリ(通常は<DLLフォルダ>/Assets/)。
    //                      .kmodel自身の内部パス(StringPool)の基準とは異なる点に注意
    // progress: 進捗の通知(省略可)。上のSceneLoadProgressCallbackを参照
    KURENAI_LIB_API Scene LoadScene(
        RHI::IRHIDevice& device, const std::wstring& sceneFilePath, const std::wstring& assetRootDirectory,
        const SceneLoadProgressCallback& progress = {});

    // ImGuiのシーン一覧構築用。[Model]の実体(.kmodel)は読み込まず、[Scene]Nameだけを取り出す
    // (省略されていればファイル名(拡張子なし)を返す)。失敗時はstd::runtime_errorを投げる
    KURENAI_LIB_API std::wstring ReadSceneName(const std::wstring& sceneFilePath);

    // KurenaiPacker.exeの--sceneモードが使う軽量な検証。書式の妥当性(ParseSceneFileと同じ規則)に加え、
    // 各[Model]Pathが指す.kmodelが実在し、ヘッダ(マジックナンバー・バージョン)が読めることまで確認する。
    // LoadSceneと異なりIRHIDeviceを必要とせず、ジオメトリ/テクスチャの実読み込みも行わない
    // (KurenaiPackerはGPUデバイスを持たないため)。失敗時はstd::runtime_errorを投げる
    KURENAI_LIB_API void ValidateScene(const std::wstring& sceneFilePath, const std::wstring& assetRootDirectory);
}
