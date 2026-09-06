#pragma once

#include <cstddef>
#include <cstdint>

// ジオメトリ描画ループ(Rendering/GeometryDrawLoop.h の ForEachGeometryDraw)が
// 受け渡す型。
//
// 【なぜループ本体と別のヘッダにするか】本体は KurenaiEngine3D のメンバテンプレートで、
// KurenaiEngine3D.h をインクルードする。型のほうは逆に KurenaiEngine3D.h が要る
// (メンバの宣言に使う)ので、同じヘッダには置けない。

namespace Kurenai
{
    namespace Assets
    {
        struct Model;
        struct ModelInstance;
    }

    namespace Rendering
    {
        // 実体は Rendering/GeometryDrawLoop.h にあり、そちらはこのヘッダより後に来る
        // (KurenaiEngine3D.h 経由)。ここではポインタでしか使わないので前方宣言で止める
        struct FrustumPlanes;

        // 各パスが1回のドローで描く単位。バッチ(InstanceCount>=2)と、まとめられなかった
        // 1体(InstanceCount==1)を同じ形で扱うためのもの。
        //
        // 【1つのループで両方を回すため】バッチ用の描画コードを別に書くと、
        // 「まとめたときだけ条件を間違える」類のずれが入り込む。判定も定数もドロー発行も
        // 1か所に保つ
        struct InstanceDrawUnit
        {
            // 代表インスタンス。World以外の値(IsMirrored / IsWater / メッシュ単位AABB)を読む。
            // Worldはバッチのときインスタンスバッファ側から引かれるので使われない
            const Assets::ModelInstance* Instance = nullptr;
            // 代表のシーン内番号。単体のときに呼び出し側がGetLODDraws/GetCurrentLODを引くのに使う
            size_t InstanceIndex = 0;
            // バッチのときだけ非nullptr。単体のときは呼び出し側が段を決める
            const Assets::Model* Model = nullptr;
            uint32_t InstanceBase = 0;
            uint32_t InstanceCount = 1;
            // カリングに使うAABB。バッチでは構成インスタンスの包絡。
            // 【参照ではなく値で持つ】IsAABBVisibleがfloat[3]への参照を取るのに合わせる
            float WorldBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
            float WorldBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
            bool IsBatch() const { return InstanceCount > 1; }
        };

        // どの段を描くか
        enum class GeometryLODMode
        {
            // 常に最も粗い段(シャドウ / 反射プローブ / DDGI)。テクスチャを読まないので
            // 詳細な段を描く意味が無い
            Coarsest,
            // そのフレームに選ばれた段を1つだけ(半透明 / 平面反射 / ソフトウェアラスタライザ)。
            // **これらはクロスディザを実装していない**ため、フェード中でも1段に決め打つ
            Current,
            // そのフレームに選ばれた段。フェード中は2段をクロスディザで重ねる
            // (深度プリパス / G-Buffer)
            Fade,
        };

        // どちらのメッシュを描くか。BLEND(mesh.IsTransparent)はG-Bufferに書けないため、
        // 不透明のパスとは排他になる
        enum class GeometryMeshFilter
        {
            Opaque,
            Transparent,
            // 落とさない。**シャドウパスだけがこれを使う** ―― 従来からBLENDのメッシュも
            // 実体のまま影を落としており、ここでふるい分けると影の出方が変わってしまう
            All,
        };

        struct GeometryDrawLoopDesc
        {
            // カリングに使う錐台。**nullptrならカリングを一切行わず統計にも入れない**
            // (DDGIのラスタ経路がそう。理由はDDGISystem.cppの同箇所)
            const FrustumPlanes* Frustum = nullptr;
            // 真ならインスタンシングのバッチを含む組(GetInstanceDrawUnits)、
            // 偽なら全インスタンスを単体として回す(BuildSingleInstanceDrawUnits)
            bool UseDrawUnits = true;
            GeometryLODMode LODMode = GeometryLODMode::Fade;
            GeometryMeshFilter MeshFilter = GeometryMeshFilter::Opaque;
            // メッシュ単位のカリングを行うか。設定(m_GeometrySettings.MeshCullingEnabled)との論理積を取る
            bool MeshCulling = true;
        };
    }
}
