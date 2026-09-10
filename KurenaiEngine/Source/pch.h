#pragma once

// KurenaiEngineLibrary と KurenaiEngine3D が共有するプリコンパイル済みヘッダー。
//
// 【自前のヘッダーを1本も入れないこと】入れた瞬間、そのヘッダーを直すたびに
// このプロジェクトの全翻訳単位が焼き直しになる。ここに置いてよいのは
// 「変更頻度がゼロのもの」= Windows SDK と標準ライブラリだけ。
//
// 【/FI(強制インクルード)で全 .cpp へ入る】各 .cpp に #include "pch.h" を書いていない。
// 82本の .cpp を触らずに済ませるためだが、**その TU が何を引いているかがソースから
// 読めなくなる**という欠点がある。これは「pch.h の中身を全部コメントアウトしても
// ビルドが通る」という試験でしか担保できない。**PCH無しでも成立する木を保つこと。**
//
// 【ThirdParty は対象外】imgui 7本と DirectXTexD3D12.cpp は
// <PrecompiledHeader>NotUsing</PrecompiledHeader> と空の <ForcedIncludeFiles> を
// 個別に指定してある。ここを外すと ThirdParty のビルドが壊れる。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <DirectXMath.h>
#include <wrl/client.h>

// 20以上の翻訳単位が引いている標準ライブラリ(実測して選んだ)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
