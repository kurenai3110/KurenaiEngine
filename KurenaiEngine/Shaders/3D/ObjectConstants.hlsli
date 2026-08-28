// モデル描画のパスすべてが共有する、メッシュ単位の定数バッファ(register b1)と、
// インスタンシングのためのインスタンスバッファ。
//
// 【なぜ1本にまとめてあるのか】以前はGBuffer / Shadow / Transparent / ProbeCapture /
// PlanarReflectionがそれぞれ同じレイアウトを手書きで複製していた。cbufferは宣言順に
// レイアウトされるため、「読みたいフィールドまでの全フィールドを正しい順序で宣言する」
// 必要があり、複製のどれか1つで並びがずれると、その1パスだけが静かに壊れる
// (実際にPlanarReflection.hlslが宣言を1本落として16バイトずれ、水面の鏡像が丸ごと
// 消える不具合を出している)。定義を1箇所にすれば、この失敗の型そのものが無くなる。
//
// C++側 KurenaiEngine3D.cpp の struct ObjectConstants とレイアウトを一致させること。
// フィールドを足すときは必ず末尾へ足す(先頭までしか使わないパスがあっても、
// 既存フィールドのオフセットが1バイトも動かないため)。
#ifndef KURENAI_OBJECT_CONSTANTS_HLSLI
#define KURENAI_OBJECT_CONSTANTS_HLSLI

// メッシュ単位の情報。
// DX12のルートシグネチャがCBVをb0/b1の2枠しか持たないため、モデル行列もここへ同居させている
cbuffer ObjectConstants : register(b1)
{
    // インスタンシングが有効なドローでは使われない(下のFetchModelInstance参照)
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
    // 関わらず常に掛ける。半透明パス・プローブ焼き込み・レイトレーシング
    // (RaytracingScene.hlsli)も同じく掛ける。**どれか1つでも落としてはいけない**
    // ―― 同じメッシュでも「直接見たとき」と「反射プローブ/RT反射に映ったとき」で
    // 色が食い違う(14章参照)
    float4 BaseColorFactor;
    // マテリアル種別ID。0=通常マテリアル、1=水面(kMaterialIDWater、GBufferCommon.hlsli)。
    // C++側 KurenaiEngine3D::MakeObjectConstants が instance.IsWater に応じて設定する
    float MaterialID;

    // --- メッシュシェーダー経路(GBufferMeshlet.hlsl)専用 -------------------------------
    //
    // メッシュシェーダーには入力アセンブラが無く、頂点もメッシュレットも自分でバッファから
    // 読むしかない。読む先はbindlessディスクリプタ番号でここから受け取る
    // (IRHIDevice::RegisterBindlessが払い出した番号。Bindless.hlsli参照)。
    // 頂点シェーダー経路ではどれも読まれないため、C++側は0のままでも構わない
    uint VertexBufferIndex;          // StructuredBuffer<MeshVertex>(Assets::Vertexと同じ並び)
    uint MeshletBufferIndex;         // StructuredBuffer<Meshlet>
    uint MeshletVertexBufferIndex;   // StructuredBuffer<uint>(頂点バッファへのインデックス)
    uint MeshletTriangleBufferIndex; // StructuredBuffer<uint>(ローカル頂点番号3つを詰めたもの)
    uint MeshletCount;               // このメッシュのメッシュレット数(増幅シェーダーの範囲外判定用)

    // 透過率(0=不透明、1=完全に透ける)。葉・花弁のように薄いものが、裏から当たった光を
    // 透かして表側を光らせる量。GBuffer.hlslがG-BufferのAlbedo.aへ書き、
    // DeferredLighting.hlsl/DirectLighting.hlslの透過項が読む(45章)
    float Translucency;

    // --- インスタンシング ---------------------------------------------------------------
    //
    // InstancingEnabledが0以外のとき、このドローは DrawIndexed(..., instanceCount) で
    // 複数体まとめて発行されており、World/NormalMatrix/TangentSignFlipは上の値ではなく
    // 下のModelInstancesバッファの ModelInstances[InstanceBase + SV_InstanceID] から取る。
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
