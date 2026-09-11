#pragma once

#include <atomic>
#include <cstdint>

#include <DirectXMath.h>

// フレームをまたいで持ち越す値。TAAの履歴と、前フレームのカメラ由来の値。
//
// 【履歴テクスチャの有効性と、カメラ行列の有効性は別管理】シーン切り替えや
// バッファ精度の変更では履歴の中身は捨てるが、カメラ行列そのものは前フレームのものが
// 正しく残っている。まとめて落とすと速度バッファまで0に潰すことになる。
namespace Kurenai::Rendering
{
    struct FrameHistoryState
    {
        // 履歴バッファ2枚。読みながら同じテクスチャへ書けないため役割を毎フレーム入れ替える。
        // HistoryIndexが今フレームの書き込み先で、もう一方が前フレームの結果(=履歴)。
        // このパスの出力がそのまま後段(自動露出/ブルーム/トーンマップ)の入力にもなる
        uint32_t HistoryIndex = 0;
        // 履歴の内容が信用できるか。falseの間、TAAは履歴を「サンプルすらせず」今フレームの色を返す。
        // ブレンド率を0にするだけでは不十分で、未初期化fp16のNaNはlerp(NaN, x, 1.0)でもNaNのまま
        // 伝播し、一度混入すると履歴に固着し続ける。
        // 落とすのは (1)履歴バッファ作成直後(初回・バッファ精度変更) (2)シーン切り替え
        // (3)TAAのON/OFFトグル。(2)はUpdateスレッドのLoadSceneから書くためatomicにする
        std::atomic<bool> HistoryValid{ false };
        // ジッターのサンプル列を進めるフレーム番号(Halton列の添字に使う)
        uint32_t FrameIndex = 0;
        // 前フレームのビュー射影行列(ジッター済み・転置済み=シェーダへ渡す形のまま)。
        // Renderスレッドのみが読み書きするため追加の排他は不要。
        // 履歴テクスチャの有効性(HistoryValid)とは意図的に別管理にしている。シーン切り替えや
        // バッファ精度変更では履歴の中身は捨てるが、カメラ行列そのものは前フレームのものが正しく
        // 残っているため、速度バッファまで0に潰す必要がない
        DirectX::XMFLOAT4X4 PrevViewProj{};
        // PrevViewProj / PrevJitterUv に実際の前フレームの値が入っているか。
        // 初回のRender()でのみfalseで、以降はずっとtrue
        bool PrevViewProjValid = false;
        // 前フレームのジッター量(UV単位)。速度からジッター差分を取り除くのに使う
        DirectX::XMFLOAT2 PrevJitterUv{ 0.0f, 0.0f };
        // 前フレームのカメラ位置(ワールド)。有効性は PrevViewProjValid と同じ
        // (同じ場所で同じタイミングに書くため)。
        //
        // 【何に使うか】Hi-Zオクルージョンカリングが判定に使うHi-Zは1フレーム古く、
        // シーンが静的である以上ずれの原因はカメラの移動だけ。移動距離をバウンディング球の
        // 半径へ足せば、そのずれを1次の範囲で保守側へ吸収できる(FrameConstants::OcclusionCullParams.z)
        DirectX::XMFLOAT3 PrevCameraPosition{ 0.0f, 0.0f, 0.0f };
        // 前フレームの実効プリ露出EV100。このエンジンはSceneColorへプリ露出を掛け込んでおり、
        // その値が時間順応で毎フレーム変わる(m_EffectiveExposureEV100)。補正しないと
        // 露出が動いている間ずっと履歴が古い明るさを引きずり、明るさの尾を引く
        float PrevEffectiveExposureEV100 = 0.0f;
    };
}
