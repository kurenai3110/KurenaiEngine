// スレッドグループのサイズと、間接引数の刻み。**HLSL側ではここが唯一の定義。**
//
// 以前は GBufferMeshlet.hlsl と ShadowMeshlet.hlsl が増幅32・メッシュ128を
// それぞれ #define しており、片方だけ直すと静かに食い違う状態だった。
//
// 【C++側は Source/Engine/ShaderInterop/GroupSizes.h が持ち、一致は機械で確かめる】
// KurenaiShaderPacker が C++ 側の値を KURENAI_EXPECT_* として -D で渡してくるので、
// ファイル末尾の #if がここの値と突き合わせ、食い違っていれば #error で落とす。
// 食い違ったときに何が起きるかは、それぞれの値のコメントに書いてある。
//
// 【実数値をここにも書く理由】-D が来ない経路(shader-check スキルが fxc/dxc を
// 直接叩く場合)でもコンパイルできる必要があるため。末尾の #if は
// KURENAI_EXPECT_* が未定義なら丸ごと飛ぶ

#ifndef KURENAI_SHADERINTEROP_GROUPSIZES_HLSLI
#define KURENAI_SHADERINTEROP_GROUPSIZES_HLSLI

// 増幅シェーダー1グループが判定するメッシュレット数。
// 生き残ったメッシュレット番号をペイロードで渡すため、ペイロードの配列長でもある。
// メッシュシェーダーのペイロードは16KBまでだが、ここでは32×4バイト=128バイトしか使わない。
//
// 【C++側が起動グループ数の割り算に使う】食い違うと、大きいほうを使った側の
// メッシュレットが描かれないか、範囲外を読む
#define KURENAI_AMPLIFICATION_GROUP_SIZE 32

// メッシュシェーダーの1グループのスレッド数。1スレッドが頂点1つと三角形1つを担当するため、
// メッシュレットの上限(頂点64・三角形124、Assets::kMeshletMax*)以上あればよい
#define KURENAI_MESH_GROUP_SIZE 128

// モデル単位のカリング(ModelCull.hlsl)の1グループのスレッド数。
// C++側が「候補数 ÷ これ」でディスパッチする
#define KURENAI_MODEL_CULL_GROUP_SIZE 64

// ソフトウェアラスタライザの解決パス(SoftwareRasterResolve.hlsl)のタイル1辺。
// 2次元グループなので1グループは 8×8=64 スレッドになる
#define KURENAI_SWRASTER_RESOLVE_GROUP_SIZE 8

// DispatchMeshIndirect の引数1件ぶんのバイト数。ModelCull.hlsl がこの刻みで書き込む。
//   +0  : このドローが使う定数バッファ(b1)のGPU仮想アドレス(64bit)
//   +8  : DispatchMeshのスレッドグループ数X/Y/Z
//   +20 : 詰め物(次の要素のアドレスを8バイト境界に載せるため)
//
// 【C++側は RHI::IRHICommandList::kDispatchMeshIndirectArgStride】
// 食い違うと2件目以降の引数を読む位置がずれ、まったく別のドローが発行される
#define KURENAI_INDIRECT_ARG_STRIDE 24


// --- C++側(GroupSizes.h)との突き合わせ。パッカー経由のときだけ有効になる ---
#if defined(KURENAI_EXPECT_AMPLIFICATION_GROUP_SIZE) && (KURENAI_AMPLIFICATION_GROUP_SIZE != KURENAI_EXPECT_AMPLIFICATION_GROUP_SIZE)
#error "KURENAI_AMPLIFICATION_GROUP_SIZE が Source/Engine/ShaderInterop/GroupSizes.h と食い違っている"
#endif
#if defined(KURENAI_EXPECT_MESH_GROUP_SIZE) && (KURENAI_MESH_GROUP_SIZE != KURENAI_EXPECT_MESH_GROUP_SIZE)
#error "KURENAI_MESH_GROUP_SIZE が Source/Engine/ShaderInterop/GroupSizes.h と食い違っている"
#endif
#if defined(KURENAI_EXPECT_MODEL_CULL_GROUP_SIZE) && (KURENAI_MODEL_CULL_GROUP_SIZE != KURENAI_EXPECT_MODEL_CULL_GROUP_SIZE)
#error "KURENAI_MODEL_CULL_GROUP_SIZE が Source/Engine/ShaderInterop/GroupSizes.h と食い違っている"
#endif
#if defined(KURENAI_EXPECT_SWRASTER_RESOLVE_GROUP_SIZE) && (KURENAI_SWRASTER_RESOLVE_GROUP_SIZE != KURENAI_EXPECT_SWRASTER_RESOLVE_GROUP_SIZE)
#error "KURENAI_SWRASTER_RESOLVE_GROUP_SIZE が Source/Engine/ShaderInterop/GroupSizes.h と食い違っている"
#endif
#if defined(KURENAI_EXPECT_INDIRECT_ARG_STRIDE) && (KURENAI_INDIRECT_ARG_STRIDE != KURENAI_EXPECT_INDIRECT_ARG_STRIDE)
#error "KURENAI_INDIRECT_ARG_STRIDE が Source/Engine/ShaderInterop/GroupSizes.h と食い違っている"
#endif

#endif // KURENAI_SHADERINTEROP_GROUPSIZES_HLSLI
