#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

#include "RHI/IRHIBuffer.h"
#include "RHI/IRHICommandList.h"

// Hi-Z とモデル単位GPUカリングの、C++側の cbuffer の写しと候補レコード(段階6)。
//
// 【なぜ独立したヘッダーなのか】パスの登録側(Passes/GeometryPasses.cpp)と、
// 定数バッファを作る側(KurenaiEngine3D.cpp)の**両方**が sizeof で使う。
//
// 【区画番号と kModelCullArgsBaseOffset の唯一の出所】段階6以前は
// KurenaiEngine3D.cpp の無名名前空間にも同じ値の写しがあり、片方だけ直すと
// 静かに食い違う状態だった。ここへ寄せて写しは消してある。
// KurenaiEngine3D 側にあるのは移行中の別名で、この値を引くだけ。
//
// 【static_assert が守るのはC++側だけ】**通すために期待値を書き換えないこと。**
namespace Kurenai::Passes
{
        // 増幅シェーダーが数え上げる先。uint×3 = [判定, 視錐台+コーンで間引き, オクルージョンで間引き]
        inline constexpr uint32_t kMeshletCullStatsCount = 3;

        // 間接描画の行き先の区画。**PSOごとに1区画**で、1区画につき1回ExecuteIndirectする。
        // 1回のExecuteIndirectで切り替えられるのは引数に含めたルートパラメータだけで、
        // PSOは切り替えられないため、まとめられない。ミラーリングの有無と、深度プリパスの
        // 不透明/カットアウトはPSOが違うので区画を分ける。
        //
        // 【プリパスとG-Bufferを同じ引数で描く理由】片方だけ間引くと絵が壊れる。
        // プリパスが深度を書いたものをG-Bufferが描かないと、その画素は
        // 「深度はあるのに色が無い」穴になる(逆向き ―― プリパスが描かずG-Bufferが描く ――
        // は早期Zが効かなくなるだけで絵は正しい)
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
        // 8バイト境界に載る位置から始めるため。24バイト刻みの配列は先頭さえ揃えば
        // 以降もすべて8の倍数になる(24は8の倍数)
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
        // ModelCull.hlsl の struct ModelCullInstance と1対1で対応(48バイト)。
        // **構造化バッファは詰めて並ぶ**ので、float3の直後にuintが来る並びをそのまま守る
        struct GpuModelCullInstance
        {
            float BoundsMin[3];
            uint32_t GroupCount;
            float BoundsMax[3];
            // 出力先の区画番号(= PSO。kModelCullRegion* を参照)
            uint32_t RegionIndex;
            // このドローが使うObjectConstantsのGPU仮想アドレス([0]=下位32bit, [1]=上位32bit)
            uint32_t CbvAddress[2];
            uint32_t Padding[2];
        };
        static_assert(sizeof(GpuModelCullInstance) == 48, "ModelCull.hlslのModelCullInstanceと一致していない");
        // 区画1つぶんのバイト数。区画の境目も8バイト境界に載せたいので256へ切り上げる
        inline uint32_t ComputeModelCullRegionStride(uint32_t capacity)
        {
            const uint32_t bytes = RHI::IRHICommandList::kDispatchMeshIndirectArgStride * capacity;
            return (bytes + 255u) & ~255u;
        }

        // --- 自前ソフトウェアラスタライザ(46章) -------------------------------------------
        //
        // 三角形をコンピュートシェーダーで自前にラスタライズする比較用の経路。
        // 既存のG-Buffer経路には一切影響せず、専用のバッファへ描いてDebugViewで見る。
        // 詳細はShaders/3D/SoftwareRasterCommon.hlsli冒頭

        // 巨大三角形リストの容量(要素数)。超えた分は描かれず、CSResolveが画面左上を
        // マゼンタで塗って知らせる
        inline constexpr uint32_t kSWRasterLargeListCapacity = 4096;
        // 1フレームに扱えるメッシュレコード数の上限。Bistro Exteriorで約400
        inline constexpr uint32_t kSWRasterMaxMeshes = 2048;
        // CSRasterの1グループのスレッド数。SoftwareRaster.hlslの
        // KURENAI_SWRASTER_GROUP_SIZEと一致させること
        inline constexpr uint32_t kSWRasterGroupSize = 64;
        // Dispatchの1次元あたりの上限(65535)に収めるための2D分解の刻み
        inline constexpr uint32_t kSWRasterMaxGroupsPerAxis = 32768;
        // 自前ソフトウェアラスタライザ用。Shaders/3D/SoftwareRasterCommon.hlsliの
        // cbuffer SWRasterConstants(b1)と並び・サイズを一致させること
        struct alignas(16) SWRasterConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
            // xy=レンダー解像度(画素)、zw=その逆数
            DirectX::XMFLOAT4 RenderSize;
            // xyz=太陽光が進む向き(正規化済み)、w=未使用
            DirectX::XMFLOAT4 SunDirection;
            // x=CSRasterのX方向グループ数(2D分解の復元用)、y=シーン全体の三角形数、
            // z=メッシュレコード数、w=巨大三角形とみなすbbox画素面積のしきい値
            DirectX::XMUINT4 DispatchParams;
            // x=巨大三角形リストの容量、yzw=未使用
            DirectX::XMUINT4 LargeParams;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(SWRasterConstants, ViewProj) == 0, "ViewProj のレイアウトが変わっている");
        static_assert(offsetof(SWRasterConstants, RenderSize) == 64, "RenderSize のレイアウトが変わっている");
        static_assert(offsetof(SWRasterConstants, SunDirection) == 80, "SunDirection のレイアウトが変わっている");
        static_assert(offsetof(SWRasterConstants, DispatchParams) == 96, "DispatchParams のレイアウトが変わっている");
        static_assert(offsetof(SWRasterConstants, LargeParams) == 112, "LargeParams のレイアウトが変わっている");
        static_assert(sizeof(SWRasterConstants) == 128, "SWRasterConstants の総サイズが変わっている");

        // 自前ソフトウェアラスタライザが読むメッシュ1件ぶんの情報。
        // Shaders/3D/SoftwareRasterCommon.hlsliのSWRasterMeshInfoと並び・サイズを一致させること。
        //
        // 【構造化バッファは詰めて並ぶ】定数バッファと違いHLSLのStructuredBuffer<T>は
        // C++と同じ詰め方になるため、このままのレイアウトで一致する
        struct SWRasterMeshInfo
        {
            DirectX::XMFLOAT4X4 World;
            DirectX::XMFLOAT4X4 NormalMatrix;
            // 頂点/インデックスバッファのbindless番号(IRHIBuffer::GetBindlessIndex)
            uint32_t VertexBufferIndex;
            uint32_t IndexBufferIndex;
            // シーン全体の通し三角形番号における、このメッシュの先頭。シェーダー側の二分探索のキー
            uint32_t FirstTriangle;
            uint32_t TriangleCount;
            // ミラーリングされたインスタンス(ModelInstance::IsMirrored)なら-1。
            // 表裏判定の符号を反転させる
            float FrontFaceSign;
            // bit0 = アルファカットアウト(フェーズ2で使う。現在は常に0)
            uint32_t Flags;
            uint32_t Padding[2];
        };

        static_assert(sizeof(SWRasterMeshInfo) == 160, "HLSL側のSWRasterMeshInfoと一致させるため160バイト固定");

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
