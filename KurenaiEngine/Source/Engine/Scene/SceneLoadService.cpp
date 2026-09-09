#include "../KurenaiEngine3D.h"

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include "Assets/SceneLoader.h"
#include "Core/Logger.h"
#include "Core/StringUtil.h"
#include "../Passes/EnvironmentPasses.h"
#include "../Passes/GeometryPasses.h"
#include "../Passes/PostProcessPasses.h"
#include "../Passes/ReflectionProbePasses.h"

// シーンの探索・読み込み要求・ホットリロード監視と、Loaderスレッドでの読み込み、
// 読み込み済みシーンのエンジンへの反映。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま)。
//
// 【スレッドの境界は変えていない】LoadSceneOnLoaderThread はLoaderスレッド、
// ApplyLoadedScene はRenderスレッドのフレーム境界でのみ走る
// (docs/Architecture.html 23章)
namespace Kurenai
{
    namespace
    {
        using Core::GetModuleDirectory;
        using Core::WideToUtf8;

        // シーン読み込みの進捗をログへ落とす最短間隔[秒]。
        // 1モデルごとに出すと767モデルのシーンで767行になるため間引く。
        // 最初(0/N)と最後(N/N)だけは間隔に関わらず必ず出す
        constexpr float kSceneLoadProgressLogIntervalSeconds = 1.0f;
    }

    void KurenaiEngine3D::DiscoverScenes()
    {
        const std::wstring sceneDirectory = GetModuleDirectory() + L"Assets\\Scenes\\";

        std::vector<std::wstring> fileNames;
        WIN32_FIND_DATAW findData{};
        HANDLE findHandle = FindFirstFileW((sceneDirectory + L"*.kscene").c_str(), &findData);
        if (findHandle != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    fileNames.push_back(findData.cFileName);
                }
            } while (FindNextFileW(findHandle, &findData));
            FindClose(findHandle);
        }

        // ImGuiのシーン一覧・LoadSceneのインデックスをビルドのたびに変わらないようにする
        std::sort(fileNames.begin(), fileNames.end(), [](const std::wstring& a, const std::wstring& b)
        {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        m_SceneFilePaths.clear();
        m_SceneDisplayNames.clear();
        for (const std::wstring& fileName : fileNames)
        {
            const std::wstring fullPath = sceneDirectory + fileName;
            try
            {
                m_SceneDisplayNames.push_back(Assets::ReadSceneName(fullPath));
                m_SceneFilePaths.push_back(fullPath);
            }
            catch (const std::exception& e)
            {
                // 1ファイルの不備でアプリ全体が起動できなくなるのを避け、そのファイルだけ除外して続行する
                Core::Logger::Error("KurenaiEngine3D", "シーンファイルの読み込みに失敗したため一覧から除外します (" + WideToUtf8(fullPath) + "): " + e.what());
            }
        }

        if (m_SceneFilePaths.empty())
        {
            const std::string message = "有効なシーンファイル(.kscene)が見つかりませんでした: " + WideToUtf8(sceneDirectory);
            Core::Logger::Error("KurenaiEngine3D", message);
            throw std::runtime_error(message);
        }
    }

    void KurenaiEngine3D::RequestSceneLoad(size_t sceneIndex)
    {
        if (sceneIndex >= m_SceneFilePaths.size())
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "RequestSceneLoad: シーン番号" + std::to_string(sceneIndex) + "が範囲外です(シーン数: " +
                    std::to_string(m_SceneFilePaths.size()) + ")。要求を無視します");
            return;
        }

        // UIパネルもRenderスレッドで動くため、ここは単なるRenderスレッド内の受け渡しでよい。
        // 実際の発注はUpdateSceneStreaming(フレーム先頭)がまとめて行う
        m_PendingSceneRequest = static_cast<int>(sceneIndex);
    }

    uint64_t KurenaiEngine3D::GetCurrentSceneFileWriteTime() const
    {
        if (m_CurrentSceneIndex >= m_SceneFilePaths.size())
        {
            return 0;
        }

        // DiscoverScenesがFindFirstFileWを使っているのと同じWin32の流儀に揃える。
        // <filesystem>は例外を投げるうえ、このコードベースでは1箇所でしか使っていない
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(m_SceneFilePaths[m_CurrentSceneIndex].c_str(), GetFileExInfoStandard, &attributes))
        {
            // 保存の瞬間にエディタがファイルを置き換えていると一時的に開けないことがある。
            // 0を返して「今回は見送る」ことで、次のポーリングが正しい値を拾う
            return 0;
        }

        return (static_cast<uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
               static_cast<uint64_t>(attributes.ftLastWriteTime.dwLowDateTime);
    }

    void KurenaiEngine3D::UpdateSceneHotReloadWatch()
    {
        // 読み込み中・要求が既に積まれている場合は何もしない(多重発注を避ける)
        if (!m_SystemSettings.SceneAutoReloadEnabled || m_SceneLoadInFlight || m_PendingSceneRequest >= 0)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < m_NextSceneWatchTime)
        {
            return;
        }
        m_NextSceneWatchTime = now + std::chrono::milliseconds(250);

        const uint64_t writeTime = GetCurrentSceneFileWriteTime();
        if (writeTime == 0 || writeTime == m_WatchedSceneWriteTime)
        {
            return;
        }

        // 【書式の検証を門番にする】保存の途中で書きかけのファイルを掴むと、LoadSceneが失敗して
        // シーンが空のまま残る(UpdateSceneStreamingはVRAMの二重常駐を避けるため、読み込みを
        // 始める前に旧シーンを手放す設計のため)。ValidateSceneはデバイスもジオメトリも要らない
        // 軽い検証なので、通ったときだけ発注することでこれを防ぐ。
        // 失敗した更新時刻は覚えておき、同じ内容で警告を繰り返さない(保存し直せば次の変更で拾う)
        const std::wstring assetRootDirectory = GetModuleDirectory() + L"Assets\\";
        try
        {
            Assets::ValidateScene(m_SceneFilePaths[m_CurrentSceneIndex], assetRootDirectory);
        }
        catch (const std::exception& e)
        {
            if (writeTime != m_SceneReloadRejectedWriteTime)
            {
                m_SceneReloadRejectedWriteTime = writeTime;
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "シーンファイルの変更を検出しましたが、書式が不正なため再読み込みを見送りました("
                    "シーンはそのまま残ります): " + WideToUtf8(m_SceneFilePaths[m_CurrentSceneIndex]) + " : " + e.what());
            }
            return;
        }

        // 検証を通った時点で基準時刻を進める。発注が消費されるまでの数フレームで
        // 同じ変更を何度も拾わないようにするため、ApplyLoadedSceneの取り直しより先に行う
        m_WatchedSceneWriteTime = writeTime;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "シーンファイルの変更を検出したため再読み込みします: " +
                WideToUtf8(m_SceneFilePaths[m_CurrentSceneIndex]));
        RequestSceneLoad(m_CurrentSceneIndex);
    }

    void KurenaiEngine3D::LoaderThreadMain()
    {
        // TextureImage::LoadFromFileがWICを使う経路(.dds/.tga以外)に備えてCOMを初期化しておく。
        // COMはスレッドごとに初期化が必要で、未初期化のままWICを呼ぶとハングする
        // (packedアセットは.ktex=DDSなので通常この経路には入らないが、保険として揃えておく)
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        // 破棄依頼を引き取って実際に解放する。アセット用ディスクリプタヒープを触るのは
        // このスレッドだけ、という不変条件を保つための処理(RetiredAssetsのコメント参照)
        const auto destroyRetiredAssets = [this]()
        {
            std::vector<RetiredAssets> retired;
            {
                std::lock_guard<std::mutex> lock(m_RetiredAssetsMutex);
                retired.swap(m_RetiredAssets);
            }
            // retiredのデストラクタでGPUリソースが解放される
        };

        // ストリーミングで遠ざかったモデルの破棄。Renderスレッドが
        // kStreamingReleaseDelayFrames フレーム寝かせたものだけがここへ来る
        // (RetiredAssetsと違いWaitForGPUIdleは通っていない。遅延がその代わり)
        const auto destroyStreamedModels = [this]()
        {
            std::vector<std::shared_ptr<Assets::Model>> release;
            {
                std::lock_guard<std::mutex> lock(m_StreamingReleaseMutex);
                release.swap(m_StreamingRelease);
            }
            // releaseのデストラクタでGPUリソースが解放される

            // 差し替えられた旧RaytracingSceneも同じ理由でこのスレッドで解放する
            // (BLAS/TLASと統合バッファのディスクリプタはアセット用ヒープから取られている)
            std::vector<std::unique_ptr<Assets::RaytracingScene>> scenes;
            {
                std::lock_guard<std::mutex> lock(m_RaytracingReleaseMutex);
                scenes.swap(m_RaytracingRelease);
            }
        };

        for (;;)
        {
            int sceneIndex = -1;
            std::vector<StreamingRequest> streamingRequests;
            bool raytracingRebuild = false;
            {
                std::unique_lock<std::mutex> lock(m_LoadRequestMutex);
                m_LoadRequestCV.wait(lock, [this] {
                    if (m_LoadRequestSceneIndex >= 0 || !m_StreamingRequests.empty() ||
                        m_RaytracingRebuildRequested || m_StopLoaderThread)
                    {
                        return true;
                    }
                    // 常駐ミップの読み直しもこのスレッドが行う(専用スレッドは立てない)。
                    // モデルの発注が無い間もこれだけで起きる必要がある
                    if (m_TextureStreaming.HasPendingRequests())
                    {
                        return true;
                    }
                    // 破棄だけが積まれている場合も起きる(読み込みが止まっている間に
                    // 破棄が溜まり続けると、遠ざかったモデルのVRAMが解放されない)
                    {
                        std::lock_guard<std::mutex> releaseLock(m_StreamingReleaseMutex);
                        if (!m_StreamingRelease.empty()) { return true; }
                    }
                    std::lock_guard<std::mutex> rtLock(m_RaytracingReleaseMutex);
                    return !m_RaytracingRelease.empty();
                });
                if (m_StopLoaderThread && m_LoadRequestSceneIndex < 0)
                {
                    break;
                }
                sceneIndex = m_LoadRequestSceneIndex;
                m_LoadRequestSceneIndex = -1;
                // 【シーン切り替えが来たら、溜まっているストリーミング発注は捨てる】
                // それらは切り替え前のシーンのもので、読んでも差し込む先が無い
                if (sceneIndex >= 0)
                {
                    m_StreamingRequests.clear();
                    // 切り替え前のシーンへの再構築要求は無意味。
                    // 【フラグを降ろすのを忘れない】立てたままだとRenderスレッドの
                    // 差し込みと破棄が永久に止まる
                    m_RaytracingRebuildRequested = false;
                    m_RaytracingRebuildInFlight.store(false, std::memory_order_release);
                }
                else
                {
                    streamingRequests.swap(m_StreamingRequests);
                    raytracingRebuild = m_RaytracingRebuildRequested;
                    m_RaytracingRebuildRequested = false;
                }
            }

            // 破棄は毎ループ引き取る。読み込みより先に行うことでVRAMのピークを下げる
            destroyStreamedModels();

            // 常駐ミップの読み直し。**モデルの読み込みより先に、そして1件ごとに挟む**
            // (下のループの中でも呼ぶ)。モデル1件の読み込みはPLATEAUのLOD2タイルで
            // 秒の単位かかるため、まとめて後回しにすると街を流している間じゅう
            // ミップの差し替えが止まり、近づいた面がぼけたまま残る。
            // 1回あたりの件数を絞ってあるので、逆にモデルの読み込みが待たされることもない
            constexpr size_t kTextureRequestsPerSlice = 4;
            m_TextureStreaming.ProcessRequests(*m_Device, kTextureRequestsPerSlice);

            // --- ストリーミングの読み込み ---------------------------------------------------
            if (!streamingRequests.empty())
            {
                if (!m_StreamingTexturePool)
                {
                    m_StreamingTexturePool = std::make_unique<Assets::SharedTexturePool>();
                }
                for (const StreamingRequest& request : streamingRequests)
                {
                    std::shared_ptr<Assets::Model> model;
                    try
                    {
                        model = std::make_shared<Assets::Model>(
                            Assets::LoadModel(*m_Device, request.Path, m_StreamingTexturePool.get()));
                    }
                    catch (const std::exception& error)
                    {
                        // 1件の失敗でストリーミング全体を止めない。そのモデルだけが出ないまま続く
                        Core::Logger::Error(
                            "KurenaiEngine3D",
                            "ストリーミングの読み込みに失敗しました: " + WideToUtf8(request.Path) + " (" +
                                error.what() + ")");
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_StreamingLoadedMutex);
                        // 失敗しても空のまま返す。Renderスレッドが「発注中」から外せないと
                        // 同じものを永久に再発注し続ける
                        m_StreamingLoaded.push_back({ request.Path, std::move(model), request.Generation });
                    }
                    // 1件読むごとにミップの差し替えを挟む(このループの外のコメント参照)
                    m_TextureStreaming.ProcessRequests(*m_Device, kTextureRequestsPerSlice);
                }
                // 【ここでcontinueしない】読み込みと再構築が同時に積まれることがある。
                // 抜けると再構築要求だけが失われ、m_RaytracingRebuildInFlightが立ったまま戻らない
            }

            // --- レイトレーシングの作り直し(Loaderスレッドで行う) ---------------------------
            if (raytracingRebuild)
            {
                // 【計測用の一時スイッチ】KURENAI_NO_RT が設定されていたら高速化構造を作らない。
                // VRAMの内訳(BLAS/TLASがどれだけ占めているか)を切り分けるためだけのもの
                size_t envLength = 0;
                char envValue[8] = {};
                const bool skipRaytracing =
                    (getenv_s(&envLength, envValue, sizeof(envValue), "KURENAI_NO_RT") == 0 && envLength > 0);
                if (skipRaytracing)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "KURENAI_NO_RTが設定されているため、レイトレーシングの高速化構造を構築しません(計測用)");
                }
                const auto startTime = std::chrono::steady_clock::now();
                auto rebuilt = std::make_unique<Assets::RaytracingScene>();
                if (!skipRaytracing && rebuilt->Build(*m_Device, m_Scene))
                {
                    const double elapsedMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
                    std::lock_guard<std::mutex> lock(m_RaytracingRebuiltMutex);
                    m_RaytracingRebuildLastMs = elapsedMs;
                    m_RaytracingRebuilt = std::move(rebuilt);
                    m_RaytracingRebuiltGeneration = m_StreamingGeneration;
                }
                // 【成否にかかわらず必ず降ろす】
                m_RaytracingRebuildInFlight.store(false, std::memory_order_release);
            }

            if (sceneIndex < 0)
            {
                continue;
            }

            // 先に破棄を済ませてから読み込む(Renderスレッドは手放す前にWaitForGPUIdle済み)。
            // 新シーンを作る前に旧シーンを解放することで、VRAMの二重常駐を避ける
            destroyRetiredAssets();

            if (sceneIndex < 0)
            {
                continue;
            }

            std::unique_ptr<LoadedScene> loaded = LoadSceneOnLoaderThread(static_cast<size_t>(sceneIndex));
            if (!loaded)
            {
                // 読み込みに失敗した場合も「読み込み中」状態を解除しないとUIが固まるため、
                // 空の完成品を渡してRenderスレッドに終了を知らせる(シーンは空のままになる)
                loaded = std::make_unique<LoadedScene>();
                loaded->SceneIndex = static_cast<size_t>(sceneIndex);
                loaded->Camera = ComputeInitialCamera(loaded->Scene);
            }

            {
                std::lock_guard<std::mutex> lock(m_LoadedSceneMutex);
                m_LoadedScene = std::move(loaded);
            }
        }

        // 停止時に残っている破棄依頼をこのスレッドで片付ける
        destroyRetiredAssets();

        // 破棄待ちの残りもここで片付ける
        destroyStreamedModels();

        // ストリーミング用の共有テクスチャも、確保したのと同じLoaderスレッドで解放する
        // (アセット用ディスクリプタヒープはロックを持たない。RetiredAssetsのコメント参照)
        m_StreamingTexturePool.reset();

        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
    }

    std::unique_ptr<KurenaiEngine3D::LoadedScene> KurenaiEngine3D::LoadSceneOnLoaderThread(size_t sceneIndex)
    {
        if (sceneIndex >= m_SceneFilePaths.size())
        {
            Core::Logger::Error("KurenaiEngine3D", "LoadSceneOnLoaderThread: シーン番号が範囲外です");
            return nullptr;
        }

        // [Model]Pathの基準ディレクトリ(Assetsルート)。.kmodel自身の内部パス(.kmodelがある
        // ディレクトリからの相対)とは基準が異なる点に注意(SceneLoader.h参照)
        const std::wstring assetRootDirectory = GetModuleDirectory() + L"Assets\\";

        auto loaded = std::make_unique<LoadedScene>();
        loaded->SceneIndex = sceneIndex;

        // 読み込み進捗。UIの進捗ウィンドウ(UIManager)がatomicを読んで出す。
        //
        // 【ログにも出す】UIを開いていない・F1で隠している・ヘッドレスに近い確認では
        // 画面の表示が見えない。一定間隔でログへ落としておけば後からでも追える。
        // 1件ごとに出すと767行になるため、間隔を空けて間引く
        m_SceneLoadProgressLoaded.store(0, std::memory_order_relaxed);
        m_SceneLoadProgressTotal.store(0, std::memory_order_relaxed);
        const std::wstring& progressSceneFileName = m_SceneFilePaths[sceneIndex];
        auto lastProgressLogTime = std::chrono::steady_clock::now();
        const auto onProgress =
            [this, &lastProgressLogTime, &progressSceneFileName](size_t loadedModels, size_t totalModels)
        {
            m_SceneLoadProgressLoaded.store(static_cast<uint32_t>(loadedModels), std::memory_order_relaxed);
            m_SceneLoadProgressTotal.store(static_cast<uint32_t>(totalModels), std::memory_order_relaxed);

            const auto now = std::chrono::steady_clock::now();
            const bool isFirstOrLast = (loadedModels == 0) || (loadedModels == totalModels);
            const bool intervalElapsed =
                std::chrono::duration<float>(now - lastProgressLogTime).count() >= kSceneLoadProgressLogIntervalSeconds;
            if (!isFirstOrLast && !intervalElapsed)
            {
                return;
            }
            lastProgressLogTime = now;
            Core::Logger::Info(
                "KurenaiEngine3D",
                "シーン読み込み: " + std::to_string(loadedModels) + " / " + std::to_string(totalModels) +
                    " モデル (" + WideToUtf8(progressSceneFileName) + ")");
        };

        try
        {
            loaded->Scene = Assets::LoadScene(*m_Device, m_SceneFilePaths[sceneIndex], assetRootDirectory, onProgress);
        }
        catch (const std::exception& e)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "シーンの読み込みに失敗しました: " + WideToUtf8(m_SceneFilePaths[sceneIndex]) + " : " + e.what());
            return nullptr;
        }

        // [Scene]Skyboxでスカイボックスを差し替える(指定が無ければ既定へ戻す)。
        // 「今どのスカイボックスを読み込み済みか」を知っているのはこのスレッドだけなので、
        // 差し替えが要るかの判定もここで行う(不要ならSkyboxTextureをnullptrのままにして
        // Renderスレッドへ「現状維持」を伝える)
        const std::wstring desiredSkyboxPath =
            loaded->Scene.SkyboxPath.empty() ? m_DefaultSkyboxPath : loaded->Scene.SkyboxPath;
        if (desiredSkyboxPath != m_LoaderSkyboxPath)
        {
            try
            {
                loaded->SkyboxTexture = m_Device->CreateTextureFromFile(desiredSkyboxPath, false);
                loaded->SkyboxPath = desiredSkyboxPath;
                m_LoaderSkyboxPath = desiredSkyboxPath;
                Core::Logger::Info("KurenaiEngine3D", "スカイボックスを差し替えました: " + WideToUtf8(desiredSkyboxPath));
            }
            catch (const std::exception& e)
            {
                // 読み込みに失敗しても現在のスカイボックスのまま描画を続ける(シーン切り替え自体は成立させる)
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "スカイボックスの読み込みに失敗しました。現在のスカイボックスを維持します: " +
                        WideToUtf8(desiredSkyboxPath) + " : " + e.what());
            }
        }

        // [Water]NormalMapで水面法線マップを差し替える(水面マテリアル基盤)。
        // スカイボックスと同じ「このスレッドだけが現在の読み込み済みパスを知っている」方式だが、
        // 空文字列が「1x1のフラット法線フォールバックを使う」という有効な指定である点が異なる
        // (スカイボックスの空文字列は「既定のSky.ddsを使う」という意味で、常に何らかのファイルを
        // 読む。水面はファイルを読まない状態そのものが正しいシーンがあるため、ここは分岐が要る)
        const std::wstring& desiredWaterNormalMapPath = loaded->Scene.WaterNormalMapPath;
        if (desiredWaterNormalMapPath != m_LoaderWaterNormalMapPath)
        {
            if (desiredWaterNormalMapPath.empty())
            {
                // フラット法線(128,128,255,255=接線空間で真上を向く法線)へ戻す。
                // ModelLoader.cppが法線マップ未指定のマテリアルに使うプレースホルダーと同じ値
                loaded->WaterNormalMapTexture = m_Device->CreateSolidColorTexture(128, 128, 255, 255);
                loaded->WaterNormalMapPath.clear();
                m_LoaderWaterNormalMapPath.clear();
                Core::Logger::Info("KurenaiEngine3D", "水面法線マップをフラットへ戻しました(NormalMap未指定)");
            }
            else
            {
                try
                {
                    loaded->WaterNormalMapTexture = m_Device->CreateTextureFromFile(desiredWaterNormalMapPath, false);
                    loaded->WaterNormalMapPath = desiredWaterNormalMapPath;
                    m_LoaderWaterNormalMapPath = desiredWaterNormalMapPath;
                    Core::Logger::Info(
                        "KurenaiEngine3D", "水面法線マップを差し替えました: " + WideToUtf8(desiredWaterNormalMapPath));
                }
                catch (const std::exception& e)
                {
                    // 読み込みに失敗しても現在の水面法線マップ(またはフラット法線)のまま描画を続ける
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        "水面法線マップの読み込みに失敗しました。現在の状態を維持します: " +
                            WideToUtf8(desiredWaterNormalMapPath) + " : " + e.what());
                }
            }
        }

        // レイトレーシングの高速化構造(BLAS/TLAS)とシーンジオメトリの統合バッファを構築する。
        // 非対応環境(DX11、Tier 1.1未満のアダプタ)では何も作らず、描画側は従来の
        // スクリーンスペース手法のまま動く。構築に失敗しても描画は継続する
        //
        // 【ストリーミング中のシーンでは構築しない】読み込み時点でモデルの実体が1つも無く、
        // BLASを作る材料が無い。常駐が増減するたびにTLASと統合バッファを作り直す仕組みは
        // まだ入れていないため、いまは構築を見送って理由をログに残す
        // (ストリーミングは既定で無効なので、既存シーンのレイトレーシングは何も変わらない)
        if (m_Device->SupportsRaytracing())
        {
            if (loaded->Scene.HasStreamingDistance)
            {
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "ストリーミング対象のシーンでは、モデルが常駐してからレイトレーシングの"
                    "高速化構造を構築します(常駐が変わるたびに作り直します)");
            }
            else
            {
                loaded->RaytracingScene.Build(*m_Device, loaded->Scene);
            }
        }

        // メッシュライトの三角形テーブル(段階2)。
        //
        // 【DXRの有無に関係なく作る】使うのは MegaLights の経路(DX12+DXR)だけだが、
        // 構築そのものは頂点の変換とバッファ確保しかしておらず、レイトレーシングに依存しない。
        // 対応環境でだけ作る形にすると、非対応機で「三角形が出ない」のか
        // 「そもそも作っていない」のかがログから切り分けられなくなる。
        // 【打ち切り照度はフレーム不変の定数を渡す】露出や現在のτから導くと、
        // 参照実装が非決定的になって「同じ入力で同じ真値」が崩れる
        // 【打ち切り照度は実効値を渡すこと】既定の定数を渡すと -emissivelightscutoff や
        // ImGui の指定が三角形テーブルへ一切届かず、**つまみが静かに効かなくなる**
        // (実際に踏んだ。τを100分の1にしてもダンプがバイト完全一致した)
        loaded->MeshLightScene.Build(*m_Device, loaded->Scene, m_EmissiveLightSettings.LightsCutoffIrradiance);

        loaded->Camera = ComputeInitialCamera(loaded->Scene);
        return loaded;
    }

    void KurenaiEngine3D::ApplyLoadedScene(LoadedScene& loaded)
    {
        // カメラ保持は「同じシーンをもう一度読む」ときにだけ効かせる。
        // 別のシーンへ切り替えたときまで前のカメラを引き継ぐと、まったく違う縮尺・位置の
        // シーンの外へ放り出される(near/farも前のシーンのAABB由来のまま残る)。
        // m_CurrentSceneIndexはこの直後に上書きされるため、比較はここで済ませておく
        const bool isSameSceneReload = (loaded.SceneIndex == m_CurrentSceneIndex);

        m_Scene = std::move(loaded.Scene);
        m_SceneGPUResources.RaytracingScene = std::move(loaded.RaytracingScene);
        m_MeshLightScene = std::move(loaded.MeshLightScene);
        m_CurrentSceneIndex = loaded.SceneIndex;

        // ストリーミングの状態もシーンに紐づく。世代を進めることで、切り替え前に発注して
        // まだ届いていない完成品を確実に捨てる(そのまま差し込むと別シーンのモデルが混ざる)
        ++m_StreamingGeneration;
        m_StreamingInFlight.clear();
        m_RaytracingRebuildPending = false;
        {
            // 【ここでresetしてはいけない】Renderスレッドでの解放になる。
            // 受け取り待ちの完成品も、破棄はLoaderスレッドへ回す
            std::unique_ptr<Assets::RaytracingScene> stale;
            {
                std::lock_guard<std::mutex> lock(m_RaytracingRebuiltMutex);
                stale = std::move(m_RaytracingRebuilt);
            }
            if (stale)
            {
                std::lock_guard<std::mutex> lock(m_RaytracingReleaseMutex);
                m_RaytracingRelease.push_back(std::move(stale));
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_StreamingLoadedMutex);
            m_StreamingLoaded.clear();
        }

        // モデルLODの状態はシーンに紐づくので必ず捨てる。
        // 【要素数の一致だけを見て使い回してはいけない】たまたま同じインスタンス数の
        // シーンへ切り替えたときに、前のシーンの段とフェード途中の状態が残る
        m_InstanceLODStates.assign(m_Scene.Instances.size(), InstanceLODState{});

        // インスタンシング用のインスタンスバッファをシーンの規模で作り直す。
        //
        // 【容量はインスタンス数×2】バッチの組は「そのフレームに選ばれた段」と
        // 「最も粗い段」の2組あり、同じインスタンスが両方に載る。最悪でも全インスタンスが
        // 両組でバッチに入るだけなので、これで足りる。
        // インスタンスが1つも無いシーンではバッファを作らない
        // (BuildInstanceBatchesがnullptrを見て何もしない)
        m_SceneGPUResources.ModelInstanceBuffer.reset();
        if (!m_Scene.Instances.empty())
        {
            RHI::BufferDesc instanceBufferDesc;
            instanceBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
            instanceBufferDesc.SizeInBytes =
                static_cast<uint32_t>(sizeof(GPUModelInstance) * m_Scene.Instances.size() * 2);
            instanceBufferDesc.StrideInBytes = sizeof(GPUModelInstance);
            // 更新はBuildInstanceBatchesの1フレーム1回だけ。DX12のステージングリングは
            // この値×kFrameCount+1段を常時確保するので、必要最小限にしておく
            instanceBufferDesc.MaxUpdatesPerFrame = 1;
            m_SceneGPUResources.ModelInstanceBuffer = m_Device->CreateBuffer(instanceBufferDesc);
        }
        m_InstanceBatchesCurrentLOD.clear();
        m_InstanceBatchesCoarsestLOD.clear();
        m_InstanceBatchedCurrentLOD.clear();
        m_InstanceBatchedCoarsestLOD.clear();
        m_ModelInstanceRecords.clear();

        // [Sun]/[Camera]セクションが無いシーンでは、Sceneの側でこのメンバの既定値
        // (従来のKurenaiEngine3Dの初期値と同じ)が使われるため、常にそのまま反映してよい
        m_SkySettings.TimeOfDay = m_Scene.SunTimeOfDay;
        m_SkySettings.SunAzimuthDegrees = m_Scene.SunAzimuthDegrees;
        // .ksceneが持つのは「影を出すか」の真偽値だけなので、手法の選択はエンジン側で決める
        // (反射のm_ReflectionSettings.Modeと同じ扱い)。規則はDefaultShadowModeに1か所だけ置いてある
        m_ShadowSettings.Mode = m_Scene.ShadowEnabled ? ShadowSettings::DefaultShadowMode(m_RenderCapabilities.RaytracingAvailable) : ShadowMode::Off;
        m_SkySettings.SunEnabled = m_Scene.SunEnabled;
        m_AmbientOcclusionSettings.Enabled = m_Scene.AOEnabled;
        // .ksceneが持つのは「反射を使うか」の真偽値だけなので、手法の選択はエンジン側で決める。
        //
        // 【キーを書いたシーンと書いていないシーンを区別する】書いていなければエンジンの既定
        // (DefaultReflectionMode。RTが使えない環境では反射なし)に従い、書いてあればその指定を
        // 優先して手法だけを環境から選ぶ(ReflectionModeForCapability)。
        // 区別せずに「= true」のときもエンジンの既定へ問い合わせ直すと、DX11ではシーンの指定が
        // 握り潰されて反射が出なくなる(両関数のコメント参照)
        m_ReflectionSettings.Mode = m_Scene.HasSSREnabledOverride
            ? (m_Scene.SSREnabled ? ReflectionSettings::ReflectionModeForCapability(m_RenderCapabilities.RaytracingAvailable) : ReflectionMode::Off)
            : ReflectionSettings::DefaultReflectionMode(m_RenderCapabilities.RaytracingAvailable);
        // UIの「既定値に戻す」はエンジンの既定ではなくここへ戻す(m_SceneDefaultReflectionMode参照)
        m_SceneDefaultReflectionMode = m_ReflectionSettings.Mode;
        // TAAと内部レンダー解像度。どちらも反射と同じく「キーを書いたシーンだけ」上書きし、
        // 書いていないシーンはエンジンの既定のまま(Assets::Scene の Has〜Override のコメント参照)
        if (m_Scene.HasTAAOverride)
        {
            m_PostProcessSettings.TAAEnabled = m_Scene.TAAEnabled;
        }
        if (m_Scene.HasRenderResolutionOverride)
        {
            // 即時に作り直すとGPUがまだ参照しているテクスチャを壊すので、
            // UIのシステムパネルと同じく要求だけ記録してRender()の先頭で反映させる。
            //
            // 超解像が有効なときはシーンの指定を「出力解像度」として解釈する。シーンが意図して
            // いるのは「この絵をこの大きさで見せたい」であって内部で何画素描くかではないため。
            // 超解像が無効ならRequestUpscaleSettingsは中でRequestRenderResolutionを呼ぶだけなので、
            // 従来とまったく同じ動作になる
            RequestUpscaleSettings(
                m_PostProcessSettings.UpscaleEnabled, m_PostProcessSettings.UpscaleQuality, m_Scene.RenderWidth, m_Scene.RenderHeight);
        }
        // トーンマップのカーブと空の彩度(アート指定)をシーンから受け取る。
        // Source/LibraryはSource/Engineに依存できないため、Scene側は同じ並びの独立した列挙を持つ。
        // 【並びを変えたら両方直すこと】(Assets/Scene.h の TonemapCurveSetting)
        switch (m_Scene.Tonemap)
        {
        case Assets::Scene::TonemapCurveSetting::Reinhard: m_PostProcessSettings.Curve = TonemapCurve::Reinhard; break;
        case Assets::Scene::TonemapCurveSetting::ACES:     m_PostProcessSettings.Curve = TonemapCurve::ACES;     break;
        case Assets::Scene::TonemapCurveSetting::AgX:      m_PostProcessSettings.Curve = TonemapCurve::AgX;      break;
        }
        // 黒の締め。Tonemap/SkySaturationと同じく無条件に反映する(既定0で恒等のため)
        m_PostProcessSettings.TonemapBlackPoint = m_Scene.TonemapBlackPoint;
        m_SkySettings.Saturation = m_Scene.SkySaturation;
        // タービディティは指定されたときだけ上書きする(Scene.h の HasSkyTurbidity 参照)。
        // 値が動けばRender()側のturbidityMoved判定が大気LUTを焼き直す
        if (m_Scene.HasSkyTurbidity) { m_SkySettings.Turbidity = m_Scene.SkyTurbidity; }
        if (m_Scene.HasIBLIntensityOverride)
        {
            m_IBLSettings.Intensity = m_Scene.IBLIntensity;
        }
        // シーン全体の露出。IBLIntensityと同じく指定されたときだけ上書きする。
        // 屋外の風景と屋内では被写体の輝度が桁で違うため、エンジンの既定値(屋内基準)を
        // 動かさずにシーン側で持てるようにしてある(Scene.h の HasExposureOverride 参照)
        if (m_Scene.HasExposureOverride)
        {
            m_PostProcessSettings.SceneExposureEV100 = m_Scene.ExposureEV100;
        }
        // 雲。天候はシーンの性質なので[Cloud]セクションで持てるようにした。
        // 露出と同じく指定されたキーだけを上書きする。CellSizeだけは.kscene側が「雲の塊1つの
        // 大きさ[m]」で持ち、エンジン側はその逆数(UVスケール)を持つので変換する
        if (m_Scene.HasCloudCoverage)  { m_CloudSettings.Coverage = m_Scene.CloudCoverage; }
        if (m_Scene.HasCloudAltitude)  { m_CloudSettings.Altitude = m_Scene.CloudAltitude; }
        if (m_Scene.HasCloudThickness) { m_CloudSettings.Thickness = m_Scene.CloudThickness; }
        if (m_Scene.HasCloudDensity)   { m_CloudSettings.Density = m_Scene.CloudDensity; }
        if (m_Scene.HasCloudTypeBias) { m_CloudSettings.TypeBias = m_Scene.CloudTypeBias; }
        if (m_Scene.HasCloudCellSize)  { m_CloudSettings.UvScale = 1.0f / std::max(m_Scene.CloudCellSize, 1.0f); }
        // 巻雲(P11)。CirrusCellSizeも積雲のCellSizeと同じく逆数へ直す
        if (m_Scene.HasCirrusCoverage)   { m_CloudSettings.CirrusCoverage = m_Scene.CirrusCoverage; }
        if (m_Scene.HasCirrusAltitude)   { m_CloudSettings.CirrusAltitude = m_Scene.CirrusAltitude; }
        if (m_Scene.HasCirrusCellSize)   { m_CloudSettings.CirrusUvScale = 1.0f / std::max(m_Scene.CirrusCellSize, 1.0f); }
        if (m_Scene.HasCirrusDensity)    { m_CloudSettings.CirrusDensity = m_Scene.CirrusDensity; }
        if (m_Scene.HasCirrusAnisotropy) { m_CloudSettings.CirrusAnisotropy = m_Scene.CirrusAnisotropy; }
        if (m_Scene.HasCirrusWindSpeed)  { m_CloudSettings.CirrusWindSpeed = m_Scene.CirrusWindSpeed; }
        // 大気遠近。[Cloud]と同じく指定されたキーだけを上書きする。
        // 【この値は遠景の霞だけの設定ではない】消散係数は雲がどれだけ空から浮き上がって
        // 見えるかも一手に決める(Scene.h の HasFogDensity 付近のコメントに実測を残してある)
        if (m_Scene.HasFogEnabled)     { m_FogSettings.Enabled = m_Scene.FogEnabled; }
        if (m_Scene.HasFogDensity)     { m_FogSettings.Density = m_Scene.FogDensity; }
        if (m_Scene.HasFogScaleHeight) { m_FogSettings.ScaleHeight = m_Scene.FogScaleHeight; }
        if (m_Scene.HasFogRefHeight)   { m_FogSettings.RefHeight = m_Scene.FogRefHeight; }
        // ブルーム。エンジンの既定は無効なので、夜景で光源が主役になるシーンは
        // ここで有効にしないと発光体に光芒が出ない
        if (m_Scene.HasBloomEnabled)   { m_PostProcessSettings.BloomEnabled = m_Scene.BloomEnabled; }
        if (m_Scene.HasBloomStrength)  { m_PostProcessSettings.BloomStrength = m_Scene.BloomStrength; }
        if (m_Scene.HasBloomThreshold) { m_PostProcessSettings.BloomThreshold = m_Scene.BloomThreshold; }
        // 星空。[Cloud]/[Fog]と同じく指定されたキーだけを上書きする
        if (m_Scene.HasStarsEnabled)    { m_StarsSettings.Enabled = m_Scene.StarsEnabled; }
        if (m_Scene.HasStarsDensity)    { m_StarsSettings.Density = m_Scene.StarsDensity; }
        if (m_Scene.HasStarsBrightness) { m_StarsSettings.Brightness = m_Scene.StarsBrightness; }
        if (m_Scene.HasStarsTwinkle)    { m_StarsSettings.Twinkle = m_Scene.StarsTwinkle; }
        // ドローンショー。[Cloud]/[Fog]と同じく指定されたキーだけを上書きする。
        // ショーの中身(点・機体数・秒数・明るさ)は.kshowが持ち、Loaderスレッドで読み込み済み
        if (m_Scene.HasDroneShowEnabled) { m_DroneShowEnabled = m_Scene.DroneShowEnabled; }
        if (m_Scene.HasDroneShowCenter)
        {
            m_DroneShowCenter = { m_Scene.DroneShowCenter[0], m_Scene.DroneShowCenter[1], m_Scene.DroneShowCenter[2] };
        }
        if (m_Scene.HasDroneShowScale)          { m_DroneShowScale = m_Scene.DroneShowScale; }
        if (m_Scene.HasDroneShowCastLight)      { m_DroneShowCastLight = m_Scene.DroneShowCastLight; }
        if (m_Scene.HasDroneShowCastLightScale) { m_DroneShowCastLightScale = m_Scene.DroneShowCastLightScale; }
        // 実効値のログと容量の警告はシーンごとに1回ずつ出す(シーンが変われば灯の値も変わる)
        m_DroneShowLightValuesLogged = false;
        m_DroneShowLightTileOverflowLogged = false;
        // 【Formationsが空でもSetDataを呼ぶ】呼ばなければ前のシーンのショーがそのまま残る。
        // 空を渡せばDroneShow側がエラーを出してm_HasDataをfalseにするので、
        // 「ショーを持たないシーンへ切り替えたのに前の編隊が飛び続ける」を構造的に防げる
        m_DroneShow.SetData(m_Scene.DroneShowData);
        // シーンを跨いでショーの進行が引き継がれると、切り替えるたびに違う編隊から始まって
        // A/B比較の対照が取れなくなる(EV100が引き継がれるのと同じ落とし穴)。必ず0へ戻す
        m_DroneShowTime = 0.0f;

        // 【ドローンショーの有無で反射手法を書き換えてはいけない】
        // 1つの機能の有効/無効が、それとは別の機能の設定(反射手法)を黙って書き換えると、
        // シーンの指定が機能側の都合で覆り、「なぜこのシーンだけ反射手法が違うのか」を
        // 追えなくなる。機体は手続き的に展開するビルボードでTLASに入っておらず、
        // RT反射のレイからは原理的に見えないため、DXR対応環境(DX12)では水面に編隊が映らない。
        // これは既知の制限として受け入れる。映したい場合はUIの「反射」セクションから
        // 手動でScreenSpaceへ切り替える。

        // 水面。[Water]が無いシーンでもScene::WaterWaveScale等はリテラル既定値
        // (EngineDefaults.hを複製したもの、Scene.h参照)を持っているため、常にそのまま反映してよい
        // (m_SkySettings.TimeOfDay/m_SkySettings.SunAzimuthDegreesと同じ扱い)
        m_WaterSettings.WaveScale = m_Scene.WaterWaveScale;
        m_WaterSettings.WaveSpeed = m_Scene.WaterWaveSpeed;
        m_WaterSettings.WaveStrength = m_Scene.WaterWaveStrength;

        // スカイボックスが差し替わった場合のみ非nullptr。IBLの拡散イラディアンス・プリフィルタ済み
        // 鏡面はスカイボックスから焼かれるため、差し替えたら焼き上がりの旗を倒して焼き直させる
        if (loaded.SkyboxTexture)
        {
            // 旧スカイボックスもアセット由来なのでLoaderスレッドへ破棄を委ねる。
            // 直前(UpdateSceneStreaming)のWaitForGPUIdleによりGPUはもう参照していない
            RetiredAssets retiredSkybox;
            retiredSkybox.SkyboxTexture = std::move(m_SkyboxTexture);
            RetireAssets(std::move(retiredSkybox));

            m_SkyboxTexture = std::move(loaded.SkyboxTexture);
            m_CurrentSkyboxPath = loaded.SkyboxPath;
            m_EnvironmentPasses->GetIBLBaked() = false;
            // 検証用の拡散イラディアンスマップも古いスカイボックス由来のものになるため倒す
            // (実際に焼き直すのは検証トグル・デバッグ表示が有効なときだけ)
            m_EnvironmentPasses->GetIBLIrradianceBaked() = false;
        }

        // 水面法線マップが差し替わった場合のみ非nullptr。スカイボックスとまったく同じ方式で
        // 旧テクスチャをLoaderスレッドへ破棄依頼する
        if (loaded.WaterNormalMapTexture)
        {
            RetiredAssets retiredWaterNormalMap;
            retiredWaterNormalMap.WaterNormalMapTexture = std::move(m_WaterNormalMapTexture);
            RetireAssets(std::move(retiredWaterNormalMap));

            m_WaterNormalMapTexture = std::move(loaded.WaterNormalMapTexture);
            m_CurrentWaterNormalMapPath = loaded.WaterNormalMapPath;
        }

        // アセット由来のライトをユーザー編集用のコピーへ複製する(m_Scene.Lightsは直接編集しない。
        // シーンを再読み込みすればアセット既定値に戻るようにするため)。m_Scene.Lightsは
        // SceneLoaderが各ModelInstanceのModel::Lightsをワールド空間へ変換し、.kscene自身の
        // [Light]セクションのライトと合成済みのシーン全体のライト一覧(Scene.h参照)
        m_Lights = m_Scene.Lights;
        m_SelectedLightIndex = m_Lights.empty() ? -1 : 0;
        m_LightOverflowLogged = false;

        // エミッシブ光源のプロキシ(ワールド空間)。**m_Lightsへは混ぜない**(宣言側の注記参照)。
        // ImGuiのライト一覧にも出さないので、m_SelectedLightIndexの範囲は変わらない
        m_EmissiveProxies = m_Scene.EmissiveProxies;
        // インスタンスごとの「プロキシを起こしたか」。DDGIのラスタ経路で引く。
        // ストリーミング中のインスタンスはプロキシを作らないので、ここも自動的に立たない
        m_EmissiveProxyInstances.assign(m_Scene.Instances.size(), false);
        for (const Assets::EmissiveProxy& proxy : m_EmissiveProxies)
        {
            if (proxy.InstanceIndex < m_EmissiveProxyInstances.size())
            {
                m_EmissiveProxyInstances[proxy.InstanceIndex] = true;
            }
            else
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "エミッシブ光源のインスタンス番号がシーンの範囲外です: " +
                        std::to_string(proxy.InstanceIndex) + " / " +
                        std::to_string(m_Scene.Instances.size()) + "個");
            }
        }
        m_RenderStats.EmissiveLightsUsedCount = 0;
        m_EmissiveLightsCapLogged = false;
        m_EmissiveLightsValuesLogged = false;
        // Rangeの上限。自発光の強度を上げたときにRangeが数kmまで伸びて、タイルカリングが
        // 全タイルにヒットするのを止める安全弁。シーンAABBの対角より長いRangeに意味は無い
        {
            float diagonalSq = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float extent = m_Scene.BoundsMax[axis] - m_Scene.BoundsMin[axis];
                diagonalSq += extent * extent;
            }
            m_EmissiveLightsMaxRange = (diagonalSq > 0.0f) ? std::sqrt(diagonalSq) : 0.0f;
        }
        // 平面反射。新しいシーンでは水面の構成が変わるため、複数水面高さの警告も仕切り直す
        m_PlanarReflectionMultipleWaterLogged = false;

        // 反射プローブもライトと同じ方針でユーザー編集用のコピーへ複製する。
        // プローブの中身(キューブマップ)はシーンのジオメトリ・ライトに依存するため、
        // シーンを読み込んだら必ず焼き直す必要がある
        m_GIResources.ReflectionProbes = m_Scene.ReflectionProbes;
        if (m_GIResources.ReflectionProbes.size() > kMaxReflectionProbes)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "反射プローブ数が上限(" + std::to_string(kMaxReflectionProbes) + ")を超えたため、先頭から" +
                    std::to_string(kMaxReflectionProbes) + "個のみ使用します: " + std::to_string(m_GIResources.ReflectionProbes.size()) + "個");
            m_GIResources.ReflectionProbes.resize(kMaxReflectionProbes);
        }
        // テクスチャの常駐ミップ制御。既定はoffで、.ksceneが明示したシーンだけが有効になる
        // (未指定のシーンは従来どおり全ミップ常駐のままで、見え方もVRAMも変わらない)
        m_TextureStreaming.Configure(m_Scene.TextureStreamingEnabled, m_Scene.TextureStreamingBias);
        // 【読み出しはLoaderスレッドに相乗りする】専用スレッドは立てない(TextureStreaming.h参照)。
        // 要求が積まれたらLoaderスレッドを起こす必要があるので、その手段を渡しておく
        m_TextureStreaming.SetRequestNotifier([this] {
            // 【notifyの前に必ずm_LoadRequestMutexを取る】Loaderスレッドは
            // このミューテックスを持ったまま述語を評価してからwaitへ入る。
            // 取らずにnotifyすると、述語がfalseと出てからwaitへ入るまでの隙間に通知が落ちる。
            // カメラが止まっていてモデルの発注が無いシーンでは、
            // 次に起こす材料が他に無いのでミップの差し替えがそのまま止まる
            { std::lock_guard<std::mutex> lock(m_LoadRequestMutex); }
            m_LoadRequestCV.notify_one();
        });
        m_TextureStreaming.Build(m_Scene, *m_Device);

        m_SelectedProbeIndex = m_GIResources.ReflectionProbes.empty() ? -1 : 0;
        m_ReflectionProbeSettings.DebugIndex = 0;
        m_ReflectionProbePasses->GetProbeBaked() = false;
        m_ReflectionProbePasses->GetProbeBakeRequested() = !m_GIResources.ReflectionProbes.empty();
        // Realtimeのラウンドロビンは先頭から仕切り直す(シーンが変わればプローブの数も並びも変わる)
        m_ReflectionProbePasses->ResetRealtimeProgress();

        // DDGIボリューム(22章)。現状は先頭の1つだけを使う。複数ボリュームは重なりと優先順位を
        // 決める仕組みがまだ無いため、2つ目以降は警告を出して切り捨てる
        m_GIResources.HasGIVolume = !m_Scene.GIVolumes.empty();
        if (m_Scene.GIVolumes.size() > 1)
        {
            Core::Logger::Warning(
                "KurenaiEngine3D",
                "[GIVolume]が複数ありますが、現状は先頭の1つだけを使用します: " +
                    std::to_string(m_Scene.GIVolumes.size()) + "個");
        }
        if (m_GIResources.HasGIVolume)
        {
            m_GIResources.GIVolume = m_Scene.GIVolumes.front();
            const uint32_t lodCount = std::clamp(m_GIResources.GIVolume.LODCount, 1u, kDDGIMaxLODCount);
            if (m_GIResources.GIVolume.LODCount > kDDGIMaxLODCount)
            {
                Core::Logger::Warning(
                    "KurenaiEngine3D",
                    "[GIVolume]のLODCountが上限(" + std::to_string(kDDGIMaxLODCount) + ")を超えているため丸めます: " +
                        std::to_string(m_GIResources.GIVolume.LODCount));
                m_GIResources.GIVolume.LODCount = lodCount;
            }
            // 【段数ぶん掛けること】クリップマップLODはプローブ数が段数倍になる。
            // 掛け忘れると上限のチェックが素通りし、確保だけが膨らむ
            const uint64_t probeCount =
                static_cast<uint64_t>(m_GIResources.GIVolume.ProbeCounts[0]) *
                static_cast<uint64_t>(m_GIResources.GIVolume.ProbeCounts[1]) *
                static_cast<uint64_t>(m_GIResources.GIVolume.ProbeCounts[2]) *
                static_cast<uint64_t>(lodCount);
            if (probeCount > Passes::kDDGIMaxProbes)
            {
                // 切り捨てでは格子が歪んで意味を成さない(反射プローブのように「先頭N個」で
                // 済ませられない)ため、ボリュームごと無効にして従来のIBLのまま描く
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "[GIVolume]のプローブ数が上限(" + std::to_string(Passes::kDDGIMaxProbes) + ")を超えたためDDGIを無効にします: " +
                        std::to_string(probeCount) + "個(格子 × LOD" + std::to_string(lodCount) +
                        "段)。ProbeCountsかLODCountを減らすかProbeSpacingを広げてください");
                m_GIResources.HasGIVolume = false;
            }
        }
        RecreateDDGIAtlases();

        // SSAO/SSILの半径やSSRの距離はシーンの規模から決まるため、差し替え後のm_Sceneで計算し直す
        ResetSceneDependentParams();

        // 露出の追従状態はシーンをまたいで持ち越さない。時刻が入れ替わると実効プリ露出は
        // 最大18段跳ぶため、追従の途中で反射プローブが焼かれると桁違いの明るさで固定される
        // (ReflectionProbePasses::m_ProbeBakedExposureEV100・PostProcessPasses::m_AutoExposureResetRequestedのコメント参照)
        m_EffectiveExposureInitialized = false;
        m_PostProcessPasses->GetAutoExposureResetRequested() = true;

        // TAAの履歴には前のシーンの絵が入っており、この後カメラも新シーンの初期位置へ飛ぶため、
        // 再投影しても対応する画素が存在しない。捨てて今フレームの色から積み直す。
        // ApplyLoadedSceneはRenderスレッドから呼ばれるため、m_Cameraは直接書けないがatomicなら書ける
        // (Renderスレッドが読む。カメラ自体はこの後m_AppliedSceneCamera経由でUpdateスレッドへ渡す)
        m_TAAHistoryValid.store(false, std::memory_order_relaxed);

        // Hi-Zにも前のシーンの深度が入っている。カメラが新シーンの初期位置へ飛ぶ以上、
        // それで遮蔽を判定すると見えているものを消しうる。TAAの履歴と同じ理由で捨てる
        // (ApplyLoadedSceneはRenderスレッドから呼ばれ、Hi-Zの有効フラグもRenderスレッドしか触らない)
        m_GeometryPasses->InvalidateHiZ();

        // ホットリロードの基準時刻を、いま読んだファイルの更新時刻で取り直す。
        // これをしないと (1)シーンを切り替えたあとも前のファイルを見続ける
        // (2)手動の再読み込み直後に「変更あり」と誤検出して延々と再読み込みし続ける
        m_WatchedSceneWriteTime = GetCurrentSceneFileWriteTime();
        m_SceneReloadRejectedWriteTime = 0;

        // 品質プリセット「高」が戻る先を、いまの状態(= .ksceneの指定をすべて反映し終えた状態)で
        // 控える。【この関数内のm_Scene.Has〜による上書きより後で呼ぶこと】先に控えると
        // シーンがSSR/TAA/ブルーム/星を指定していても、エンジンの既定を控えることになる。
        // シーンを切り替えたらプリセットの選択も「高」へ戻す(新しいシーンに対して前のシーンで
        // 選んだ「低」が適用されたままになるわけではなく、実際に高相当の状態になっているため)
        m_SceneDefaultQuality = CaptureQualitySettings();
        m_QualitySettings.Preset = QualityPreset::High;

        // 初期カメラとウィンドウタイトルはUpdateスレッドが適用する。m_Cameraの書き込み手を
        // 1スレッドに保ち、ウィンドウタイトルもウィンドウを所有するスレッドから設定するため
        // (UpdateAppliedSceneHandoff参照)
        {
            const wchar_t* apiName = (m_GraphicsAPI == GraphicsAPI::DX12) ? L"DX12" : L"DX11";
            std::lock_guard<std::mutex> lock(m_AppliedSceneMutex);
            // カメラを適用するかどうか。「現在のカメラを保持する」が入っていても
            // ウィンドウタイトルは更新したいので、引き渡し自体は毎回行う。
            // 保持が効くのは同じシーンの読み直しのときだけ(上のisSameSceneReload参照)
            m_AppliedSceneApplyCamera = !(m_SystemSettings.SceneReloadKeepsCamera && isSameSceneReload);
            m_AppliedSceneCamera = loaded.Camera;
            m_AppliedSceneTitle = std::wstring(L"Kurenai Engine [") + apiName + L"] - " + m_Scene.Name;
        }
        m_AppliedScenePending.store(true, std::memory_order_release);
    }
}
