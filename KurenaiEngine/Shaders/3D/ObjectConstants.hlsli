// モデル描画のパスすべてが共有する、メッシュ単位の定数バッファ(register b1)と、
// インスタンシングのためのインスタンスバッファ。
//
// 【なぜ1本にまとめてあるのか】以前はGBuffer / Shadow / Transparent / ProbeCapture /
// PlanarReflectionがそれぞれ同じレイアウトを手書きで複製していた。cbufferは宣言順に
// レイアウトされるため、「読みたいフィールドまでの全フィールドを正しい順序で宣言する」
// 必要があり、複製のどれか1つで並びがずれると、その1パスだけが静かに壊れる。
// 実際に2件起きている:
//   - PlanarReflection.hlsl が宣言を1本落として16バイトずれ、水面の鏡像が丸ごと消えた
//   - Shadow.hlsl が DitherFade を落とし、MaterialTableIndex 以降を4バイトずれて読んでいた
//     (アルファカットアウトの影が、マテリアルテーブルの番号として DitherFade のビット列
//      1065353216 を使い、bindlessヒープの範囲外を引いていた)
// 定義を1箇所にすれば、この失敗の型そのものが無くなる。
//
// C++側 KurenaiEngine3D.cpp の struct ObjectConstants とレイアウトを一致させること。
// フィールドを足すときは必ず末尾へ足す(先頭までしか使わないパスがあっても、
// 既存フィールドのオフセットが1バイトも動かないため)。
#ifndef KURENAI_OBJECT_CONSTANTS_HLSLI
#define KURENAI_OBJECT_CONSTANTS_HLSLI

// メッシュ単位(将来的にはシーン上のモデルインスタンス単位)の情報。
// DX12のルートシグネチャがCBVをb0/b1の2枠しか持たないため、モデル行列もここへ同居させている
cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
    // Worldの3x3部分の逆転置(4x4に格納)。回転+非一様スケールで法線が歪むのを防ぐため、
    // 位置と同じWorldではなくこちらを法線の変換に使う(Architecture.html「法線マッピングの
    // 接線ベクトル計算」参照)
    float4x4 NormalMatrix;
    float MetallicFactor;
    float RoughnessFactor;
    // Worldの行列式が負(ミラーリングを含む非一様スケール)の場合は-1。従法線の向きが
    // 反転するため、頂点接線のw成分(従法線の向き)に掛け合わせて補正する
    float TangentSignFlip;
    // 0以下ならアルファカットアウト無効(常に不透明として扱う)。glTFのalphaMode=MASKの
    // マテリアルのみalphaCutoff(既定0.5)が設定される
    float AlphaCutoff;
    float3 EmissiveFactor;
    // glTFのocclusionTexture.strength(既定1.0)。遮蔽マップの効き具合をlerp(1, ao, strength)で
    // 調整する
    float OcclusionStrength;
    // glTFのpbrMetallicRoughness.baseColorFactor(既定[1,1,1,1])。glTF仕様では
    // baseColor = baseColorTexture * baseColorFactor と定義されており、テクスチャの有無に
    // 関わらず常に掛ける。半透明パス(Transparent.hlsl)・プローブ焼き込み(ProbeCapture.hlsl)・
    // レイトレーシング(RaytracingScene.hlsli)も同じく掛ける。**どれか1つでも落としてはいけない**
    // ―― 同じメッシュでも「直接見たとき」と「反射プローブ/RT反射に映ったとき」で
    // 色が食い違う(14章参照)
    float4 BaseColorFactor;
    // マテリアル種別ID(末尾に追加)。0=通常マテリアル、1=水面(kMaterialIDWater、上記参照)。
    // C++側 KurenaiEngine3D::MakeObjectConstants が instance.IsWater に応じて設定する
    float MaterialID;

    // --- メッシュシェーダー経路(GBufferMeshlet.hlsl)専用 -------------------------------
    //
    // メッシュシェーダーには入力アセンブラが無く、頂点もメッシュレットも自分でバッファから
    // 読むしかない。読む先はbindlessディスクリプタ番号でここから受け取る
    // (IRHIDevice::RegisterBindlessが払い出した番号。Bindless.hlsli参照)。
    //
    // 【末尾に足してあるので既存シェーダーへの影響は無い】Shadow.hlslのように
    // 先頭までしか宣言していないシェーダーがあっても、定数バッファのオフセットは1バイトも
    // 動かない(上のMaterialID・BaseColorFactorのコメントと同じ理由)。
    // 頂点シェーダー経路ではどれも読まれないため、C++側は0のままでも構わない
    //
    // 【表はモデル単位】かつてメッシュごとに別のバッファを指していたが、1回のDispatchMeshで
    // モデル全体を描けるようにするためモデル単位の1本へ統合した(Assets::GpuMeshlet)。
    // 頂点バッファの番号はメッシュレット1件ごとが持つので、ここでは渡さない
    uint MeshletOffset;              // MeshletBufferの中で、このドローが見る範囲の先頭
    uint MeshletBufferIndex;         // StructuredBuffer<Meshlet>
    uint MeshletVertexBufferIndex;   // StructuredBuffer<uint>(頂点バッファへのインデックス)
    uint MeshletTriangleBufferIndex; // StructuredBuffer<uint>(ローカル頂点番号3つを詰めたもの)
    uint MeshletCount;               // このドローで見るメッシュレット数(増幅シェーダーの範囲外判定用)

    // 透過率(0=不透明、1=完全に透ける)。葉・花弁のように薄いものが、裏から当たった光を
    // 透かして表側を光らせる量。GBuffer.hlslがG-BufferのAlbedo.aへ書き、
    // DeferredLighting.hlsl/DirectLighting.hlslの透過項が読む(45章)。
    // 末尾に足しているので、先頭までしか宣言していないシェーダー(Shadow.hlsl等)への影響は無い
    float Translucency;

    // モデルLODの切り替え中に2段をクロスディザで重ねるための係数(末尾に追加)。
    //
    //   1.0        LOD切替中ではない。全画素を描く(既定値。これ以外を書かない限り従来と同じ)
    //   0 < f < 1  切り替え「先」の段。ノイズ < f の画素だけを描く
    //  -1 < f < 0  切り替え「元」の段。ノイズ >= -f の画素だけを描く
    //
    // 【±で対にする理由】先と元がまったく同じノイズを読み、しきい値の両側で分け合うので、
    // 2段が同じ画素を取り合わない。独立な模様にすると、同じ画素にLOD1の箱とLOD2の建物が
    // 両方描かれてZファイティングになる(PLATEAUはLOD1とLOD2が同じ建物を二重に持つ)。
    // 逆に隙間もできない ―― どの画素も必ずどちらか一方が描く。
    //
    // 末尾に足した4バイトのスカラーなので、既存シェーダーのオフセットは動かない
    float DitherFade;
    // --- マテリアルテーブル経路(1モデル1ドロー)専用 -----------------------------------
    //
    // 1回のDispatchMeshでモデル全体を描くと、上のMetallicFactor〜Translucencyのような
    // 「メッシュごとに違う値」を定数バッファで渡せなくなる。代わりにマテリアルを
    // StructuredBuffer<GpuMaterial>へ載せ、メッシュシェーダーが出力したMaterialIndexで引く。
    //
    // kInvalidBindlessIndexなら従来経路(t0〜t6と上の定数)を使う
    uint MaterialTableIndex;
    // 増幅シェーダーがメッシュレットを取捨するマスク(Assets::kGpuMaterialFlag*)。
    // Rejectのビットが1つでも立っていれば捨て、Requireのビットが揃っていなければ捨てる。
    // 1ドローでモデル全体を描くと、ドローやPSOの分割では材質を出し分けられないため、
    // 「どのマテリアルを描くパスなのか」をここで指定する
    uint MeshletFilterReject;
    uint MeshletFilterRequire;
    // シーン全体の自発光の強度倍率と、遮蔽マップの有効/無効(1.0 or 0.0)。
    //
    // 【なぜ定数で渡すのか】従来この2つはC++側(MakeObjectConstants)が係数へ
    // 掛けてから定数バッファへ入れていた。マテリアルテーブルは読み込み時に焼くので
    // 同じ手が使えず、シェーダー側で掛けるしかない。
    // **どちらの経路でもピクセルシェーダーは必ず掛ける** ―― 従来経路では
    // C++側が1.0を入れることで二重に掛からないようにしている
    float EmissiveIntensity;
    float OcclusionMapScale;
    // このドローでメッシュレットカリングの統計を数えるか(0/1)。
    //
    // 【パスで分ける必要がある】深度プリパスは G-Buffer とまったく同じ増幅シェーダーを
    // 使うため、フレーム全体のフラグ(MeshletCullStatsParams.x)だけで判定すると
    // 同じ塊を1フレームに2回数えてしまい、「1フレームあたりの判定数」が倍になる。
    // 数えるのは G-Buffer パスだけにする
    uint MeshletStatsEnabled;

    // --- インスタンシング -------------------------------------------------------------
    //
    // InstancingEnabledが0以外のとき、このドローは DrawIndexed(..., instanceCount) で
    // 複数体まとめて発行されており、World/NormalMatrix/TangentSignFlipは上の値ではなく
    // 下のModelInstancesバッファの ModelInstances[InstanceBase + SV_InstanceID] から取る
    // (下のFetchModelInstance)。
    //
    // 【なぜ開始位置を定数で渡すのか】D3D11/D3D12ともStartInstanceLocationは
    // SV_InstanceIDへ加算されない(SV_InstanceIDは常に0から始まる)ため、
    // バッファ内のどこから読むかはドロー引数では伝えられない
    uint InstanceBase;
    uint InstancingEnabled;
};

// インスタンス1体ぶんの変換。C++側の Kurenai::GPUModelInstance と
// バイト単位で一致させること(144バイト)
struct ModelInstanceRecord
{
    float4x4 World;
    float4x4 NormalMatrix;
    float TangentSignFlip;
    float3 Padding;
};

// インスタンスの変換の一覧。頂点シェーダー専用のSRV(t0)へ
// IRHICommandList::SetVertexShaderResourceBufferでバインドされる。
//
// 【ピクセルシェーダーのt0(BaseColorTexture)と衝突しない】DX12ではこちらは
// D3D12_SHADER_VISIBILITY_VERTEXのルートSRV、テクスチャ側はVISIBILITY_PIXELの
// ディスクリプタテーブルで、可視ステージが分離している。DX11もステージごとに
// バインド空間が独立している。同じファイル内で両方をt0へ宣言してもfxc/dxcとも
// エントリポイントごとに片方しか残らないため、コンパイルも通る(実験で確認済み)
StructuredBuffer<ModelInstanceRecord> ModelInstances : register(t0);

// このドロー・このインスタンスの変換を取り出す。
// InstancingEnabledが0(従来の単発描画)ならObjectConstantsの値をそのまま返すので、
// 呼び出し側は分岐を書かなくてよい。
//
// 【一様分岐なのでPSOは増えない】条件は定数バッファの値で、ワープ内で分岐しない。
// メッシュレット経路のようにPSOを何本も増やさずにインスタンシングを足せるのはこのため
ModelInstanceRecord FetchModelInstance(uint instanceID)
{
    if (InstancingEnabled != 0)
    {
        return ModelInstances[InstanceBase + instanceID];
    }

    ModelInstanceRecord record;
    record.World = World;
    record.NormalMatrix = NormalMatrix;
    record.TangentSignFlip = TangentSignFlip;
    record.Padding = float3(0.0f, 0.0f, 0.0f);
    return record;
}

#endif // KURENAI_OBJECT_CONSTANTS_HLSLI
