#pragma once

#include <cstdint>
#include <vector>

#include "Assets/MeshLightScene.h"
#include "Assets/Scene.h"

// 自発光メッシュを光源として扱うための、シーン読み込みで決まる一式。
//
// 【シーンの寿命に紐づく】どれもシーンを読み込んだときに作られ、切り替えで作り直される。
// フレームごとに変わるのは下の3つのログ・署名の状態だけで、それらも「一度だけ出す」
// ためのものなので、シーンと一緒に持たせておくのが素直。
namespace Kurenai::Scene
{
    struct EmissiveLightSet
    {
        // SceneLoaderがワールド空間へ変換したプロキシ。**手置きのライトとは別に持つ。**
        // 作者が置いたライトと自動生成の光源を同じ配列にすると、ImGuiのライト一覧から
        // 消せてしまい元のメッシュと食い違う。上限超過時に手置きを押し出さないためでもある
        std::vector<Assets::EmissiveProxy> Proxies;
        // インスタンスごとに「このインスタンスからプロキシを起こしたか」。
        // LoadSceneでProxiesから作る(**要素数はScene.Instances.size()と一致させること**。
        // ずれるとDDGIが範囲外を引く)。
        //
        // 【DDGIのラスタ経路で要る】あちらはモデルLODの粗い段を描くので、
        // プロキシが持つMeshIndex(段0の番号)では引けない。インスタンス単位で
        // 判定し、メッシュ側はEmissiveClustersの有無で見る
        std::vector<bool> ProxyInstances;

        // 発光面を三角形のまま面積分するための集合(MegaLights経路)
        Assets::MeshLightScene MeshLightScene;
        // 発光面を三角形のまま面積分するか。MegaLights 経路でのみ効く
        // (有効なフレームは参照実装が型3のプロキシを読み飛ばし、代わりに三角形を積む)
        bool MeshLightsEnabled = false;

        // RangeのクランプにつかうシーンAABBの対角。LoadSceneで一度だけ求める
        float MaxRange = 0.0f;

        // 上限で切り捨てたときの「採用した集合」の指紋。切り捨てが起きなければ0。
        //
        // 【プローブの署名に混ぜるためだけにある】採用順はカメラからの照度で決まるので、
        // 上限に当たっているシーンではカメラを動かすだけで焼く光源の集合が変わる。
        // 署名へ入れないと、収束済みのプローブだけ古い集合のまま残る
        uint64_t SelectionHash = 0;
        bool CapLogged = false;
        // 送信した灯の実効値を1回だけログへ出したか(「走っていない」と「暗い」の切り分け用)
        bool ValuesLogged = false;
    };
}
