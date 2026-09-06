#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

#include "RHI/IRHIBuffer.h"

// Hi-Z とモデル単位GPUカリングの、C++側の cbuffer の写しと候補レコード(段階6)。
//
// 【なぜ独立したヘッダーなのか】パスの登録側(Passes/GeometryPasses.cpp)と、
// 定数バッファを作る側(KurenaiEngine3D.cpp)の**両方**が sizeof で使う。
//
// 【KurenaiEngine3D.cpp にも同じ名前の写しが残っている】区画番号と
// kModelCullArgsBaseOffset は、あちらの無名名前空間にも同じ値で宣言されている
// (段階6以前からの重複)。値が同じなのでどちらを引いても結果は変わらないが、
// **片方だけ直すと静かに食い違う**。消すのは別の関心事なので手を付けていない。
//
// 【static_assert が守るのはC++側だけ】**通すために期待値を書き換えないこと。**
namespace Kurenai::Passes
{
        // 増幅シェーダーが数え上げる先。uint×3 = [判定, 視錐台+コーンで間引き, オクルージョンで間引き]
        inline constexpr uint32_t kMeshletCullStatsCount = 3;

        enum : uint32_t
        {
            kModelCullRegionGBuffer = 0,
            kModelCullRegionGBufferMirrored,
            kModelCullRegionPrepassOpaque,
            kModelCullRegionPrepassOpaqueMirrored,
            kModelCullRegionPrepassCutout,
            kModelCullRegionPrepassCutoutMirrored,
            kModelCullRegionCount,
        };
        // 引数バッファの先頭に置く「区画ごとの発行数」の領域。ExecuteIndirectの
        // 件数バッファとしてそのまま渡す(1区画あたりuint1つ)。
        //
        // 【256バイトに切り上げる】後ろに続く引数配列の先頭を、定数バッファのGPUアドレスが
        // 8バイト境界に載る位置から始めるため
        inline constexpr uint32_t kModelCullArgsBaseOffset = 256;
        // [判定, 視錐台で間引き, オクルージョンで間引き, 生き残り] + 区画ごとの発行数。
        //
        // 【前の4つはモデル数】数えるのはG-Bufferぶんの候補だけで、そこは1モデル1件になる
        // (m_ModelCullPrepassCandidateCount のコメント参照)。深度プリパスぶんも数えると
        // 1モデルを2回数えてしまい、CPU側の判定と単位が合わなくなる
        inline constexpr uint32_t kModelCullCounterCount = 4 + kModelCullRegionCount;

        // HiZ.hlsl側のcbuffer HiZConstantsと一致させる必要がある
        struct alignas(16) HiZConstants
        {
            DirectX::XMUINT2 SrcSize;
            DirectX::XMUINT2 DstSize;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(HiZConstants, SrcSize) == 0, "SrcSize のレイアウトが変わっている");
        static_assert(offsetof(HiZConstants, DstSize) == 8, "DstSize のレイアウトが変わっている");
        static_assert(sizeof(HiZConstants) == 16, "HiZConstants の総サイズが変わっている");

        // ModelCull.hlsl の cbuffer ModelCullConstants と並びを一致させること
        struct alignas(16) ModelCullConstants
        {
            // 視錐台判定に使う。**今フレームの**ビュー射影行列(CPU側の判定と揃える)
            DirectX::XMFLOAT4X4 CullViewProj;
            // Hi-Z判定に使う。そのHi-Zの元になった深度を描いた行列。
            // 深度プリパスから作る経路では今フレーム、そうでなければ前フレームのもの
            DirectX::XMFLOAT4X4 CullPrevViewProj;
            // x=候補数、y=Hi-Zのミップ段数、z=オクルージョン判定の有効フラグ、
            // w=引数配列の先頭オフセット[バイト]
            DirectX::XMUINT4 CullParams;
            // x=区画1つぶんのバイト数、y=区画数、
            // z=このディスパッチが受け持つ候補の先頭番号、
            // w=統計を数え始める候補番号(G-Bufferぶんの先頭)
            DirectX::XMUINT4 CullRegionParams;
            // xy=Hi-Zのミップ0の解像度[画素]、zw=未使用
            DirectX::XMFLOAT4 CullHiZScreenParams;
            // x=AABBを膨らませる量[m](前フレームからのカメラ移動距離)、yzw=未使用
            DirectX::XMFLOAT4 CullExpandParams;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(ModelCullConstants, CullViewProj) == 0, "CullViewProj のレイアウトが変わっている");
        static_assert(offsetof(ModelCullConstants, CullPrevViewProj) == 64, "CullPrevViewProj のレイアウトが変わっている");
        static_assert(offsetof(ModelCullConstants, CullParams) == 128, "CullParams のレイアウトが変わっている");
        static_assert(offsetof(ModelCullConstants, CullRegionParams) == 144, "CullRegionParams のレイアウトが変わっている");
        static_assert(offsetof(ModelCullConstants, CullHiZScreenParams) == 160, "CullHiZScreenParams のレイアウトが変わっている");
        static_assert(offsetof(ModelCullConstants, CullExpandParams) == 176, "CullExpandParams のレイアウトが変わっている");
        static_assert(sizeof(ModelCullConstants) == 192, "ModelCullConstants の総サイズが変わっている");

        // 間接描画の候補1件。カリング前の列挙結果で、行き先の区画(= PSO)まで確定している。
        //
        // 【ObjectConstantsの中身はここに持たない】GPUへ載せる直前(ModelCullパスの中)で作る。
        // 引数に書き込むのは定数バッファのリングスロットのGPUアドレスで、それは
        // UpdateBufferを呼んだ後にしか分からないため
        struct ModelCullDrawCandidate
        {
            const Assets::ModelInstance* Instance;
            const Assets::Model* Model;
            uint32_t Region;
            uint32_t GroupCount;
            // 増幅シェーダーがメッシュレットを取捨するマスク(Assets::kGpuMaterialFlag*)
            uint32_t RejectMask;
            uint32_t RequireMask;
            float DitherFade;
            // メッシュレット単位のカリング統計を数えるか。
            // **G-Bufferの区画だけtrue** ―― プリパスでも数えると同じメッシュレットを二重に数える
            bool CountCullStats;
            // Hi-Zオクルージョン判定のモード(0=しない / 1=前フレーム / 2=今フレーム)。
            // 深度プリパスとG-Bufferで読むHi-Zの中身が違うため区画ごとに変わる
            uint32_t OcclusionMode;
        };
}
