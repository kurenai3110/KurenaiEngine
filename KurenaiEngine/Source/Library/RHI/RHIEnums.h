#pragma once

namespace Kurenai::RHI
{
    enum class ShaderStage
    {
        Vertex,
        Pixel,
        Compute,
        // メッシュシェーダーパイプライン(DX12かつメッシュシェーダー Tier 1 以上、SM 6.5)。
        // 入力アセンブラを持たず、頂点/インデックスはシェーダー自身がバッファから読む。
        //
        // Amplification(増幅シェーダー)はメッシュシェーダーの前段で、メッシュレット単位の
        // カリング(錐台・法線コーン)を行って生き残ったメッシュレットだけをDispatchMeshで
        // 起動する。Meshはメッシュレット1つ分の頂点/三角形をラスタライザへ出力する。
        //
        // DX11にはこのパイプラインが存在しないため、DX11バックエンドはこの2つのステージで
        // シェーダーを作成しようとするとログを出してnullptrを返す
        // (呼び出し側はIRHIDevice::SupportsMeshShader()で事前に確認すること)
        Amplification,
        Mesh,
    };

    enum class BufferUsage
    {
        Vertex,
        Index,
        Constant,
        // コンピュートシェーダーから読み書きする構造化バッファ(RWStructuredBuffer)。
        // UAV+SRVの両方を持つDEFAULT/GPU専用ヒープに作成される
        Structured,
        // CPUから毎フレーム書き換え、シェーダからは読み取り専用(StructuredBuffer<T>)の構造化バッファ。
        // ライトリストのような「要素数が可変で定数バッファに収めるには大きい配列」を
        // グラフィックスパイプラインのピクセルシェーダへSRVとしてバインドする用途向け
        StructuredReadOnly,
        // コンピュートシェーダーがUAVで書き、ピクセルシェーダがSRVで読む構造化バッファ。
        // CPUからは書き込まない(GPUだけで完結する中間データ)。タイルライトカリングの
        // ライトグリッドがこれにあたる。
        //
        // 既存の2種はどちらも片側しか持たないためこの用途に使えない:
        //   Structured         … UAVのみ。SRVディスクリプタが無くPSから読めない
        //   StructuredReadOnly … SRVのみ。UPLOADヒープ経由のCPU書き込み専用でUAVが無い
        // DX12ではUNORDERED_ACCESSとPIXEL_SHADER_RESOURCEの間を明示的に遷移させる
        // (DX12Buffer::TransitionTo。バインド時に暗黙に発行される)。
        // DX11はUAVとSRVを同時にバインドできないが、DX11CommandList::DispatchがDispatch直後に
        // UAVを全解除しているため追加の対処は要らない
        StructuredRW,
        // 作成時に初期データを与えたあと二度と書き換えない、読み取り専用の構造化バッファ。
        // DEFAULTヒープにSRVだけを持ち、CPU書き込み用のステージングリングを一切持たない。
        //
        // StructuredReadOnlyとの違いはこのステージングリングの有無で、あちらは毎フレームの
        // UpdateBufferに備えて「本体サイズ×リング段数」ぶんのUPLOADヒープを常時確保する。
        // レイトレーシングのシーンジオメトリ(頂点属性・インデックス)は本体だけで数十MBに達し、
        // かつシーン読み込み時に一度書いたら変わらないため、あちらを使うと使いもしない
        // ステージング領域が本体の数倍のUPLOADヒープを占有してしまう。
        // このUsageはUpdateBufferを受け付けない(呼ぶとログを出して無視される)
        StructuredImmutable,
    };

    enum class PrimitiveTopology
    {
        TriangleList,
    };

    // パイプラインステートのアルファブレンド設定。2Dスプライトやエフェクト描画で半透明合成を行う場合に指定する
    enum class BlendMode
    {
        Opaque,             // ブレンドなし(不透明。3Dの通常描画はこちら)
        AlphaBlend,         // src.rgb * src.a + dst.rgb * (1 - src.a) の標準アルファブレンド
        Additive,           // src.rgb * src.a + dst.rgb の加算合成(炎・光などの発光エフェクト向け)
        Multiply,           // src.rgb * dst.rgb の乗算合成(影・すりガラスなどの減光エフェクト向け)
        PremultipliedAlpha, // src.rgb + dst.rgb * (1 - src.a) の事前乗算済みアルファブレンド(テクスチャ側でRGBに既にAを乗算済みの場合に使う)
    };

    enum class Format
    {
        R32G32_Float,
        R32G32B32_Float,
        R32G32B32A32_Float,
        R8G8B8A8_UNorm,
        // Hi-Zミップチェーン用の単チャンネル深度フォーマット。RWTexture2D<float>としてUAV書き込みする
        R32_Float,
        // G-Bufferのオクタヘドラルエンコード法線用。浮動小数点フォーマットのため[-1,1]の符号付き値を
        // そのまま格納でき、従来のR8G8B8A8(RGBに0〜1へ再マップして格納)よりチャンネル数を
        // 2つに減らしつつビット深度を増やせるため、低ラフネスの鏡面ハイライトのバンディングを抑えられる
        R16G16_Float,
        // HDR中間バッファ用(SceneColor等)。トーンマップ前の1.0を超える輝度値を保持できる
        R16G16B16A16_Float,
        // アルファを持たないHDRバッファ用(G-Bufferのエミッシブ、ブルームのミップチェーン)。
        // R16G16B16A16_Floatの半分の帯域で1.0を超える輝度を保持できる。
        // ただし仮数はR/G=6bit・B=5bitで相対精度は約1.6%と、1.0付近ではR8G8B8A8_UNorm(0.39%)より粗い。
        // 買っているのは精度ではなく1.0超のヘッドルームなので、「1.0を超えたい」場合にのみ使うこと
        // (広い面積の淡いHDR値を格納するとバンディングし得る。その場合はR16G16B16A16_Floatへ上げる)
        R11G11B10_Float,
    };

    // サンプラーのフィルタリング方式
    enum class SamplerFilter
    {
        Linear,
        // 浅い角度で見る面(床・路面など)のボケを抑える異方性フィルタリング
        Anisotropic,
        // 深度・オクタヘドラルエンコードされた法線・metallic/roughnessのように、
        // 「テクセルに格納された値そのもの」に意味があり補間してはいけないデータ用。
        // バイリニア補間するとシルエットを跨いだタップが実在しない中間値を作り、
        // そこから再構成したワールド座標や法線が破綻する
        // (参照実装のXeGTAO/Bevyが深度・法線にポイントサンプラーを使うのと同じ理由)
        Point,
    };

    // サンプラーのアドレッシング方式(UVが[0,1]の外に出たときの扱い)
    enum class SamplerAddressMode
    {
        // タイリングするマテリアルテクスチャ用。UVを繰り返す
        Wrap,
        // ルックアップテーブルのように「UVの端が定義域の端」であるテクスチャ用。端のテクセルを引き伸ばす。
        // Wrapのままだと端でバイリニア/異方性フィルタのタップが反対側の端へ回り込み、
        // 無関係な値が混ざる(BRDF積分LUTのNdotV=1付近で顕著。docs/Architecture.html 14.2節)
        Clamp,
    };

    // 使用するグラフィックスAPIバックエンドの選択
    enum class GraphicsAPI
    {
        DX11,
        DX12,
    };
}
