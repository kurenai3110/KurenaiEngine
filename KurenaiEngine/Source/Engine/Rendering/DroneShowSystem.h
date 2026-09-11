#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <DirectXMath.h>

#include "DroneShow.h"
#include "DroneShowResources.h"
#include "../EngineDefaults.h"

// ドローンショー(.kshowで動く多数の発光体)の一式。
//
// 【資源の生成位置は動かさないこと】シェーダーとバッファはCreateSceneResourcesが
// 作る。DX12はディスクリプタ枠を生成順に割り当てるので、持ち主をここへ移しても
// **生成を呼ぶ行の位置は元のまま**にしてある。
namespace Kurenai::Rendering
{
    struct DroneShowSystem
    {
        // --- ドローンショー(発光点の描画) ---------------------------------------------
        // 夜空を編隊飛行する多数のドローンを、1機につきカメラ正対のビルボード1枚として
        // 加算合成で描く。編隊の生成と時間補間はDroneShow.h/.cppが持ち、ここは描画だけを担う。
        //
        // 頂点バッファを持たず、Draw(6 * 機体数, 0)とSV_VertexIDでクアッドを展開する
        // (理由はShaders/3D/DroneShow.hlsl冒頭)。機体データはResources.Bufferから
        // 頂点シェーダーが直接読む(SetVertexShaderResourceBuffer)。
        //
        // 【PSOは1本でよい】平面反射(鏡映カメラ)でもこれをそのまま使う。メッシュ描画のように
        // ワインディングを反転したPSOを別に持つ必要は無い ―― 理由はPSO生成箇所のコメント
        std::unique_ptr<RHI::IRHIShader> VertexShader;
        std::unique_ptr<RHI::IRHIShader> PixelShader;
        // PSO・定数バッファ・機体データは本描画と平面反射の2群が同じものを使うため、
        // 持ち主を Rendering/DroneShowResources.h へ移した
        Rendering::DroneShowResources Resources;
        // 1フレームぶんの機体の状態。毎フレームDroneShow::Evaluateが書き、
        // グラフ構築前に1回だけResources.BufferへUpdateBufferする
        // (m_SceneGPUResources.LightBufferと同じ理由: 本描画と平面反射の2パスから読まれるため、
        //  パスの中で更新すると先に走る側が未更新の内容を読む)
        std::vector<GPUDrone> Instances;
        // 機体を光源として送るときの、間引いた灯。毎フレームDroneShow::BuildLightSamplesが書き、
        // gpuLightsの組み立てで手置きライト・エミッシブプロキシの後ろへ連結する
        std::vector<DroneLightSample> LightSamples;
        // 再生器。編隊の点そのものはここが持つ(.kshowから読み込む)
        DroneShow Show;

        // ショーの進行時刻[秒]。RenderThreadMainがm_CloudScrollOffsetと同じ場所で進める
        float Time = 0.0f;

        // --- .ksceneが持つパラメータ ---
        //
        // 【ショーの中身に属する値はここに無い】機体数・保持/変形秒・明るさ・ビルボード半径・
        // 揺れ・再生速度・種はすべて.kshowが持つ(Show.Data()から読む)。
        // シーンが決めてよいのは「出すかどうか」と「どこにどの大きさで置くか」だけで、
        // 同じショーを別のシーンへ置けるのはこの分担があるため
        bool Enabled = Defaults::DroneShowEnabled;
        DirectX::XMFLOAT3 Center{
            Defaults::DroneShowCenterX, Defaults::DroneShowCenterY, Defaults::DroneShowCenterZ };
        float Scale = Defaults::DroneShowScale;
        // 遠方の機体が1画素を割ってTAAのジッターでちらつくのを防ぐ、画面上の最小半径(NDC単位)。
        // 【これだけはシーンにもショーにも持たせない】ショーの表現ではなく描画側の下限で、
        // 「1画素を割ったらちらつく」という事実はどのシーン・どのショーでも変わらないため
        float MinScreenRadius = Defaults::DroneShowMinScreenRadius;
        // 機体を光源としても送るか。シーンが決める(「出すか」の一種)
        bool CastLight = Defaults::DroneShowCastLight;
        // 灯の明るさの倍率。1.0がスプライトから導いた物理的な値で、演出用にシーンが上げられる
        float CastLightScale = Defaults::DroneShowCastLightScale;
        // 光源として送る灯の数と、Rangeを逆算する打ち切り照度[lx]。
        // 【これらもシーンにもショーにも持たせない】MinScreenRadiusと同じで、
        // タイルライトカリングの容量という描画側の事情で決まる値だから
        int LightSampleCount = Defaults::DroneShowLightSampleCount;
        float LightCutoffLux = Defaults::DroneShowLightCutoffLux;
        // 実際に送った灯の数。ログとUIの表示用
        uint32_t LightUsedCount = 0;
        // 容量超過の警告と実効値ログを、それぞれ1回だけ出すためのフラグ
        bool LightTileOverflowLogged = false;
        bool LightValuesLogged = false;
    };
}
