# コンピュートシェーダによる自前ソフトウェアラスタライザ

## Context

KurenaiEngine の不透明ジオメトリは現在すべてハードウェアラスタライザ (GBuffer パス、`KurenaiEngine3D.cpp:5624`) で描かれている。ここに「三角形をコンピュートシェーダで自前にラスタライズする経路」を追加する。

目的は GPU がブラックボックスで行っている処理 — 頂点変換・背面カリング・スクリーン空間への投影・エッジ関数による被覆判定・深度テスト・透視補正補間 — を明示的なコードとして持ち、ハードウェアの結果と直接突き合わせられるようにすること。

**既存の GBuffer 経路は一切変更しない。** 独立した自己完結パスとして追加し、ImGui のトグルで ON/OFF、DebugView で表示して比較する。本番のシェーディング経路を置き換えるのは対象外。

**方式は 1パス・64bit アトミック。** シェーダモデル 6.6 の 64bit `InterlockedMax` で「深度と三角形ID」を1個の値に詰めて解決する。教科書どおりの visibility buffer の最短経路で、ラスタライズは1回だけ、中間のビンやレコードも要らない。

RHI に足りない機能 (SM 6.6 の許可・UAV クリア・間接ディスパッチ) は回避策で誤魔化さず、**RHI 側に実装して足す**。

## 方式

### なぜ 64bit が要るのか

深度テストと ID の書き込みが別の操作だと必ず競合する:

```
時刻  スレッドA (z=0.5, ID=1)          スレッドB (z=0.8, ID=2)
 1    InterlockedMax(D, 0.5) → D=0.5
 2    D を読む → 0.5 == 自分。書くと決める
 3                                     InterlockedMax(D, 0.8) → D=0.8
 4                                     D を読む → 0.8 == 自分 → ID=2 を書く
 5    ID=1 を書く   ← 敗者が勝者を上書き
      結果: D=0.8 なのに ID=1
```

**判定と書き込みを1個のアトミックにまとめれば消える。** そのためには深度と ID が同じワードに入る必要があるが、BistroExterior の 2,837,209 三角形は ID に 22bit 使うので 32bit では深度に 10bit しか残らない (Zファイティングで使い物にならない)。よって 64bit にする。

```hlsl
// Reverse-Z なので大きい z = 手前。InterlockedMax がそのまま最近傍を選ぶ
const uint64_t packed = (uint64_t(asuint(z_ndc)) << 32) | uint64_t(globalTriangleIndex);
InterlockedMax(Visibility[pixelIndex], packed);
```

- `z_ndc` は [0,1] の非負 float なので `asuint` がビットパターンのまま順序を保つ (`LightCulling.hlsl:57-58` と同じ手)
- クリア値は 0。`z_ndc == 0` は遠平面なので書き込まれない → **高位32bitが0なら「当たり無し」**
- 深度が完全に同値なら三角形番号の大きい方が勝つ = **完全に決定的**。`LightCulling.hlsl:236-254` が `InterlockedAdd` の戻り順の非決定性のためにやっている順位ソートのような小細工が要らない
- 1920×1080 で 2,073,600 × 8B = **16.6MB**

`RWStructuredBuffer<uint64_t>` (raw/structured) を使うので、必要なのは SM 6.6 + `Int64ShaderOps` だけ。**typed リソース (`RWTexture2D<uint64_t>`) なら追加で `OPTIONS9::AtomicInt64OnTypedResourceSupported` が要るがそれを避けられる**。同時に `RHIEnums.h` の `Format` enum にも触らずに済む (`ToDXGIFormat` は `DX11Device.cpp:29-51` と `DX12Device.cpp:103-125` の2か所にあり、`default` が `R32G32B32A32_FLOAT` に落ちるため追加し忘れが無警告で通る)。

> **検証時の確認事項**: `RWStructuredBuffer<uint64_t>` に対する `InterlockedMax` が dxc の `cs_6_6` で通ることを最初に確認する。通らない場合の代替は `RWByteAddressBuffer::InterlockedMax64` で、その場合はバッファを raw で作る `BufferUsage` が1つ要る (下の `IndirectArgs` と同じ作り方)。

### 三角形レコードを持たない

`Camera::GetProjectionMatrix()` (`Camera.cpp:91-106`) は `_33 = a = n/(n-f)`, `_43 = b = -a*f`, `_34 = 1` なので `clip.w = viewZ`、**`z_ndc = a + b/viewZ = a + b * invW`**。z_ndc は invW の1次関数であり、これはハードウェアが z をスクリーン空間で線形補間するのと数学的に同値。`_33`/`_43` は TAA ジッター (`_31`/`_32` のみ書き換え) で変化しない。

visibility buffer にグローバル三角形番号が入っているので、Resolve は**その番号からジオメトリを引き直して再変換すれば**スクリーン座標も invW も重心座標も復元できる。頂点3個の変換は数十 FLOP なので、中間レコードバッファ (Bistro 級で 48〜136MB) を丸ごと省ける。重心座標を visibility buffer に詰める必要もないので、その精度の議論も消える。

### 巨大三角形を別パスへ逃がす

1スレッド1三角形方式の唯一の弱点は、画面全体を覆う三角形を1スレッドが 200万回ループして TDR を起こすこと。

`CSRaster` は、スクリーン bbox のピクセル面積が `kSWRasterLargeTriangleArea` (4096 = 64×64 相当) を超えた三角形を**自分ではラスタライズせず、グローバル三角形番号を `LargeEntries` へ `InterlockedAdd` 1回で登録して return する**。これで小三角形パスの1スレッドあたりの仕事量は必ず 4096 画素以下に収まる。

`CSRasterLarge` は **1スレッドグループ = 1個の巨大三角形** (256スレッド)。256スレッドがその三角形のスクリーン bbox をストライドしながら分担する。画面全体を覆う三角形でも 1スレッドあたり 2,073,600 / 256 ≒ 8100 反復で有界。

**この巨大三角形の個数は GPU 上でしか分からない**ため、`CSRasterLarge` は間接ディスパッチでなければ発行できない。これが `DispatchIndirect` を RHI へ足す理由。

## RHI に足りないものを実装する

### 1. シェーダモデル 6.6 を使えるようにする

現状 SM 6.6 は使えない。確認済みの事実:

- `DX12Device.cpp:1704-1706` の `kCandidates` は `D3D_SHADER_MODEL_6_5` までしか問い合わせない
- `DX12ShaderCompiler.cpp:192` が `m_ShaderModel` を 6.5 で頭打ちにしている
- 配布中の `dxcompiler.dll` は **10.0.19041.685 (dxc 1.5、SM 6.5 上限)**。ただしこのマシンには **SDK 10.0.26100.0 (dxc 1.8、SM 6.8 対応)** が入っており、`KurenaiEngineLibrary.vcxproj:101` の `$(WindowsSdkDir)bin\$(TargetPlatformVersion)\x64` は `<WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>` により最新 SDK へ解決されるので、再ビルドすれば 1.8 が配布される

**全シェーダを一律 6.6 に上げてはいけない。** `m_ShaderModel` は全シェーダ共通で、しかも**デバイスの対応状況から決まり、配布された dxcompiler.dll が実際にそのプロファイルを出せるかは検証していない**。一律で上げると、古い dxc が配布された環境で全シェーダのコンパイルが失敗して**エンジンが丸ごと壊れる** (レイトレーシングも道連れ)。`DX12ShaderCompiler.cpp:191` のコメントが警告しているのはまさにこれ。

**シェーダ単位で最低シェーダモデルを要求できるようにする:**

- `RHIDesc.h` の `ShaderDesc` に `uint32_t MinimumShaderModel = 0;` を追加 (0 = 既定どおり。6.6 は `0x66`)
- `DX12ShaderCompiler` は「既定のコンパイル対象」を **6.5 のまま据え置き**、`MinimumShaderModel` が指定されたシェーダだけそのプロファイルでコンパイルする
- `SoftwareRaster.hlsl` だけが 6.6 を要求する。**コンパイルに失敗したらその場で捕まえて `m_SoftwareRasterAvailable = false` にし、`Logger::Warning` を出して機能を無効化する** (バージョン判定より確実。dxc が古ければ必ずここで落ちる)
- 既存シェーダの挙動は1つも変わらない = 回帰リスクゼロ

さらに:
- `kCandidates` の先頭に SM 6.6 を足す。`D3D_SHADER_MODEL_6_6` は古い SDK ヘッダに無いので **`static_cast<D3D_SHADER_MODEL>(0x66)` と書く** (`ToProfileSuffix` は 0x66 → `"6_6"` を汎用に導出するので変更不要)
- `DetectRaytracingSupport` (`DX12Device.cpp:1732-`) に倣って `DetectSoftwareRasterSupport()` を足す。判定は (a) `m_ShaderCompiler.GetShaderModel() >= 0x66`、(b) `D3D12_FEATURE_DATA_D3D12_OPTIONS1::Int64ShaderOps`。落ちた理由を必ずログへ
- `IRHIDevice` に `virtual bool SupportsSoftwareRaster() const = 0;` を追加。**DX11 は常に false を返し `Logger::Info` を出す** (`SupportsRaytracing()` と完全に同じ扱い。D3D11 は `cs_5_0` 固定なので原理的に不可)
- 診断のため `DX12ShaderCompiler::Initialize` で **ロードした `dxcompiler.dll` のファイルバージョンをログに出す**。古い DLL が残っているのが最も起きやすい失敗なので切り分けが一撃で済む

### 2. UAV クリア (`ClearUnorderedAccessBufferUint`)

visibility buffer は散布書き込みなので、三角形が当たらなかった画素には前フレームの値が残る。毎フレーム 0 に戻す必要があるが RHI にその手段が無い。間接ディスパッチ引数も同様。

```cpp
// IRHICommandList.h
// UAVを持つバッファ全体を指定した符号なし整数値で埋める。visibility bufferや
// 間接ディスパッチ引数を毎フレーム初期値へ戻す用途向け。
// UAVを持たないバッファを渡すとログを出して何もしない。
//
// 【この呼び出しはバインド状態を変えない】SetComputeUnorderedAccess*で張ったスロットには
// 影響しない(内部で一時的なディスクリプタを使うため)。
// 【DX12の要件】ClearUnorderedAccessViewUintはシェーダ可視ヒープ上のGPUハンドルと
// 非シェーダ可視ヒープ上のCPUハンドルの両方を要求するため、コンピュートディスクリプタ
// リングから1枠を借りてディスクリプタをコピーしてから発行する
virtual void ClearUnorderedAccessBufferUint(IRHIBuffer* buffer, uint32_t value) = 0;
```

**DX12** — 必要な部品はすべて既存:
- CPU ハンドル: `DX12Buffer::GetUavCpuHandle()` (`DX12Buffer.cpp:196`)。`m_RenderSrvCpuHeap` (非シェーダ可視) 上にある
- GPU ハンドル: `m_Device->AllocateComputeTableBlock(1)` (`DX12Device.cpp:573`) で `m_ShaderVisibleSrvHeap` から1枠借り、`CopyDescriptorsSimple` でコピーする。このヒープは `SetDescriptorHeaps` 済み (`DX12Device.cpp:338-339, 497-498`)
- 発行前に `DX12Buffer::TransitionTo(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)`、発行後に UAV バリアを積んで後続の Dispatch から見えるようにする

**DX11** — `ID3D11DeviceContext::ClearUnorderedAccessViewUint(uav, values)` を呼ぶだけ。`SetComputeUnorderedAccessBuffer` (`DX11CommandList.cpp:323-333`) と同じく `UnbindPixelSrvForResource` を先に呼ぶ。ソフトウェアラスタライザ自体は DX11 で動かないが、RHI の抽象を片肺にしないため両バックエンドとも実装する。

> **検証時の確認事項**: 構造化バッファ (`DXGI_FORMAT_UNKNOWN` + StructureByteStride) の UAV に対する `ClearUnorderedAccessViewUint` がデバッグレイヤに弾かれないことを、**デバッグレイヤを有効にして必ず確認する**。弾かれた場合の代替は「visibility buffer を raw UAV にする」で、raw UAV のクリアは仕様上必ず通る。

### 3. 間接ディスパッチ (`DispatchIndirect` と `BufferUsage::IndirectArgs`)

巨大三角形の個数は GPU 上でしか分からないので、`CSRasterLarge` のグループ数は CPU から書けない。

```cpp
// RHIEnums.h — BufferUsage に追加
// 間接ディスパッチ(DispatchIndirect)の引数バッファ。uint3(スレッドグループ数X/Y/Z)を
// コンピュートシェーダーがRWByteAddressBufferとして書き、そのままDispatchIndirectへ渡す。
//
// 【構造化バッファにできない】D3D11はD3D11_RESOURCE_MISC_DRAWINDIRECT_ARGSと
// D3D11_RESOURCE_MISC_BUFFER_STRUCTUREDを同時に指定できないため、
// raw(ByteAddress)バッファとして作りHLSL側もRWByteAddressBufferで受ける。
// DX12にはこの制約は無いが、シェーダを1本で済ませるため同じ形に揃える
IndirectArgs,

// IRHICommandList.h
// BufferUsage::IndirectArgsで作ったバッファのoffsetInBytesの位置にあるuint3を
// スレッドグループ数として解釈してディスパッチする。offsetInBytesは4の倍数であること。
// IndirectArgs以外のバッファを渡すとログを出して何もしない
virtual void DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t offsetInBytes) = 0;
```

**DX11** — バッファは `MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS`、`BindFlags = D3D11_BIND_UNORDERED_ACCESS`。UAV は `Format = DXGI_FORMAT_R32_TYPELESS` / `Flags = D3D11_BUFFER_UAV_FLAG_RAW`。`CreateBuffer` は Usage ごとの `if` 分岐チェーン (`DX11Device.cpp:196-255` 付近) なので既存の型どおりブロックを1つ足す。発行は `m_Context->DispatchIndirect(buffer, offset)` で、`Dispatch` と同じく直後に UAV を全解除する。

**DX12** — `ID3D12CommandSignature` を `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH` 1個・`ByteStride = 12` で `DX12Device` の初期化時に1度だけ作る。発行は `TransitionTo(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)` してから `ExecuteIndirect(signature, 1, resource, offset, nullptr, 0)`。`DX12Buffer::TransitionTo` (`DX12Buffer.cpp:206`) は任意の `D3D12_RESOURCE_STATES` を取るのでそのまま使える。UAV は raw (`DXGI_FORMAT_R32_TYPELESS` / `D3D12_BUFFER_UAV_FLAG_RAW`) で作る。

### コンピュート UAV スロットは拡張不要

この方式は1ディスパッチあたり最大 UAV 3本しか使わないため、現在の `kComputeUavSlotCount = 4` のままで足りる (下のバインド表)。触らない。

## パス構成とバインド表

| # | 種別 | CBV | SRV | UAV | Dispatch |
|---|---|---|---|---|---|
| 0 | RHI 呼び出し | — | — | — | `ClearUnorderedAccessBufferUint(Visibility, 0)` / `(IndirectArgs, 0)` |
| 1 | `CSRaster` | b0, b1 | t0=Position, t1=Index, t2=MeshInfo, t3=InstanceInfo | u0=Visibility, u1=LargeEntries, u2=IndirectArgs | 2D分解 (下記) |
| 2 | `CSRasterLarge` | b0, b1 | t0=Position, t1=Index, t2=MeshInfo, t3=InstanceInfo, t4=LargeEntries | u0=Visibility | **`DispatchIndirect(IndirectArgs, 0)`** |
| 3 | `CSResolve` | b0, b1 | t0=Position, t1=Index, t2=MeshInfo, t3=InstanceInfo, t4=Material | u0=SWColor, u1=SWDepth, u2=Visibility | `ceil(w/8), ceil(h/8), 1` |

ディスパッチは3つ、RHI クリアが2回。最大 UAV 3本 / SRV 5本で既存の枠 (u0〜u3 / t0〜t16) に収まる。

呼び出し順の規約 (`IRHICommandList.h:54-129`): `UpdateBuffer(cb)` はパス冒頭で1回 → 各ディスパッチで `SetComputePipelineState` → `SetComputeConstantBuffer` → SRV → UAV → `Dispatch`。**UAV は Dispatch 直後に全解除されるので毎回張り直す** (`AutoExposure` パス L6586-6658 が同じ作法)。b0 に `FrameConstants` を置くのは RTShadow パス (L5819) と同じ。

### `CSRaster` — 間接ディスパッチ引数の作り方

巨大三角形を登録するとき、**引数バッファの X フィールドをそのままカウンタとして使う**:

```hlsl
uint slot;
IndirectArgs.InterlockedAdd(0, 1, slot);         // 戻り値が自分の書き込み先
if (slot < kSWRasterLargeListCapacity) { LargeEntries[slot] = triangleIndex; }
// Y=Z=1 は何度書いても同じなので、競合を気にせず全スレッドが書いてよい
IndirectArgs.Store(4, 1);
IndirectArgs.Store(8, 1);
```

別のカウンタバッファも集計用ディスパッチも要らない。`CSRasterLarge` は `groupID.x` が巨大三角形リストの添字になる。

> 溢れ (`slot >= 容量`) は登録せずに落とす。**生の件数は打ち切らずに増やす**ので、`CSResolve` 側で容量超過を検出して画面隅にマゼンタを出せる。CPU 側でも1回だけ `Logger::Warning`。

### `CSRaster` の Dispatch 数

三角形数はシーン読み込み時に確定する静的な値なので CPU が持てばよい (`SoftwareRasterScene::GetTriangleCount()`)。ここは間接ディスパッチにする理由が無い。Dispatch は1次元 65535 上限なので2D分解する:

```cpp
const uint32_t groupsTotal = (triangleCount + 63) / 64;
const uint32_t groupsX = std::min(groupsTotal, 32768u);
const uint32_t groupsY = (groupsTotal + groupsX - 1) / groupsX;
// SWRasterConstants.DispatchGroupsX = groupsX
cmd->Dispatch(groupsX, groupsY, 1);
```

HLSL 側は `linearGroup = groupID.y * DispatchGroupsX + groupID.x` で復元し、`triangleIndex >= TotalTriangleCount` なら return。三角形番号 → メッシュの逆引きは `MeshInfo[].FirstTriangle` に対する二分探索 (BistroExterior は 132 メッシュ = 8ステップ)。専用テーブル (4B × 2.84M = 11MB) は要らない。

## ジオメトリの供給

描画用の頂点バッファは **GPU 上に頂点バッファとしてしか存在せず SRV を持たない** (`Model.h:114-117`)。`RaytracingScene` の統合バッファは (a) `SupportsRaytracing()` が true のときしか作られない、(b) `RaytracingVertexAttribute` (16B) は **位置を持たない** (`RaytracingGeometry.h:11-16`)。よって位置を持つ専用の統合バッファを新設する。

CPU 側の元データ (`geometryPayload`) は `ModelLoader.cpp` の `LoadModel` 内でしか生存しないため、`buildRaytracingGeometry` (L488) と**同じ場所・同じ手口**で作る。

### 構造体 (新規 `Source/Library/Assets/SoftwareRasterGeometry.h`)

`RaytracingGeometry.h` と同じ体裁 — HLSL 側の宣言をコメントで併記し `static_assert` でサイズ固定。

- `SoftwareRasterPosition` = `float Position[3]` (12B)
- `SoftwareRasterMeshInfo` (32B) = `PositionOffset / IndexOffset / FirstTriangle / TriangleCount / InstanceIndex / MaterialIndex / Flags / Padding`
  - `FirstTriangle` は二分探索のキー。`Flags` bit0 = 半透明 (スキップ)、bit1 = アルファカットアウト
- `SoftwareRasterInstanceInfo` (144B) = `World[16] / NormalMatrix[16] / FrontFaceSign / Padding[3]`
  - `FrontFaceSign` は `ModelInstance::IsMirrored` (`Scene.h:27`) が true なら -1。既存の GBuffer パスがミラーリング済みインスタンスを `FrontCounterClockwise = true` の別 PSO で描いている (`RHIDesc.h:120-129`) のと同じ扱いをしないと、鏡像配置のモデルだけ表裏が反転する
- `SoftwareRasterMaterial` (48B) = `RaytracingMaterial` と同じ形。独立定義にして `RaytracingGeometry.h` に依存させない

### `Model` / `Mesh` への追加

`Model` に `std::vector<SoftwareRasterPosition> SoftwareRasterPositions` と `std::vector<uint32_t> SoftwareRasterIndices`、`Mesh` に対応する2つのオフセット。寿命コメントは `RaytracingAttributes` (`Model.h:111-122`) に倣う — `SoftwareRasterScene::Build` が GPU へ送った時点で `shrink_to_fit()` で解放。

`RaytracingIndices` を共用しないのは、`RaytracingScene::Build` が `RaytracingScene.cpp:152-158` でそれを解放するため、どちらが先に走るかに依存する結合が生まれるから。CPU 側の一時重複 (Bistro で 34MB、Build 後に即解放) を払って独立させる。

### フラグの通し方

`LoadModel` の呼び出し元は `SceneLoader.cpp:1123` の1か所だけ。既定引数付きのオプション構造体を足す:

```cpp
// ModelLoader.h
struct ModelLoadOptions { bool BuildSoftwareRasterGeometry = false; };
KURENAI_LIB_API Model LoadModel(RHI::IRHIDevice&, const std::wstring&, const ModelLoadOptions& = {});
// SceneLoader.h も同様に SceneLoadOptions を追加して中継
```

エンジン側は `m_LoadRequestSceneIndex` と同じ `m_LoadRequestMutex` の下に `m_LoadRequestBuildSWRaster` をスナップショットして渡す (Loader スレッドがエンジンの生の状態を読まない既存方針を守る)。

**実行時トグルとの整合**: ジオメトリはシーン読み込み時にしか作れないので、チェックボックスを ON にしたとき `m_SoftwareRasterScene.IsValid() == false` なら `RequestSceneLoad(m_CurrentSceneIndex)` を自動発行し `Logger::Info` を出す。`ApplyLoadedScene` の `isSameSceneReload` (L2929) がカメラを保持するので体感は「一瞬シーンが消えて戻る」だけ。パネルにもその旨を明記する。

### `SoftwareRasterScene` (新規 `Source/Library/Assets/SoftwareRasterScene.h/.cpp`)

`RaytracingScene` を丸ごと手本にする — `Build(device, scene)` / `Reset()` / `IsValid()` / アクセサ / 統計 / 例外を捕まえて `Logger::Error` + `Reset()` + `false`。`SupportsRaytracing()` の代わりに `SupportsSoftwareRaster()` を見る。`CreateImmutableStructuredBuffer` (`RaytracingScene.cpp:18-29`) と同じテンプレートで全バッファを `BufferUsage::StructuredImmutable` で作る。

### メモリ増加 (BistroExterior 実測: 3,379,846頂点 / 8,511,627インデックス / 2,837,209三角形 / 132メッシュ)

| リソース | サイズ |
|---|---|
| Position (12B) | 40.6 MB |
| Index (4B) | 34.0 MB |
| MeshInfo / InstanceInfo / Material | 合計 ~11 KB |
| Visibility (uint64) | 16.6 MB |
| LargeEntries / IndirectArgs | ~16 KB |
| SWDepth (R32_Float) | 8.3 MB |
| SWColor (R16G16B16A16_Float) | 16.6 MB |
| **合計** | **約 116.1 MB** (無効時は 0) |

CPU 側の一時ピークは `geometryPayload` (既存) + 位置 40.6MB + インデックス 34MB。`Build` の末尾で解放する。

## 実装ステップ (フェーズ1)

ゴール: **「ソフトウェアラスタライザ」を選ぶと、フラット陰影の Bistro が正しい深度で描かれる。**

### A. RHI の拡張 (先に単独で通すこと)

| 順 | ファイル | 内容 |
|---|---|---|
| 1 | `RHI/RHIDesc.h` | `ShaderDesc` に `uint32_t MinimumShaderModel = 0;` (6.6 は `0x66` とコメント) |
| 2 | `RHI/DX12/DX12ShaderCompiler.h/.cpp` | 既定は 6.5 据え置き。`Compile` が `MinimumShaderModel` を見てプロファイルを切り替え、非対応なら `Logger::Error` を出して失敗を返す。`Initialize` で `dxcompiler.dll` のファイルバージョンをログ出力 |
| 3 | `RHI/DX12/DX12Device.cpp:1704` | `kCandidates` の先頭に `static_cast<D3D_SHADER_MODEL>(0x66)` を追加 |
| 4 | `RHI/DX12/DX12Device.cpp:1732` 付近 | `DetectSoftwareRasterSupport()`。SM ≥ 0x66 と `OPTIONS1::Int64ShaderOps` を判定し、落ちた理由をログへ |
| 5 | `RHI/IRHIDevice.h` / `DX11Device` / `DX12Device` | `SupportsSoftwareRaster()`。DX11 は常に false + `Logger::Info` |
| 6 | `RHI/RHIEnums.h` | `BufferUsage::IndirectArgs` (DX11 の構造化との排他をコメントに明記) |
| 7 | `RHI/IRHICommandList.h` | `ClearUnorderedAccessBufferUint` / `DispatchIndirect` |
| 8 | `RHI/DX12/DX12Device.*` `DX12Buffer.*` `DX12CommandList.*` | `IndirectArgs` の raw UAV 生成、ディスパッチ用 `ID3D12CommandSignature` の作成、2つの API の実装 |
| 9 | `RHI/DX11/DX11Device.*` `DX11Buffer.*` `DX11CommandList.*` | 同上 (`MISC_DRAWINDIRECT_ARGS \| ALLOW_RAW_VIEWS` + raw UAV) |

**ここで一度ビルドして確認する**: 起動ログの `dxcompiler.dll` が **1.8 系**であること (19041.685 のままなら古い DLL が残っている)、既存シェーダが全部これまでどおり `6_5` でコンパイルされること、レイトレーシングが有効なままであること、デバッグレイヤを有効にして UAV クリアと間接ディスパッチが検証エラーを出さないこと。

### B. ジオメトリ供給

| 順 | ファイル | 内容 |
|---|---|---|
| 10 | 新規 `Source/Library/Assets/SoftwareRasterGeometry.h` | POD 4種 + `static_assert` + HLSL 対応コメント |
| 11 | `Source/Library/Assets/Model.h` | `Mesh` に2オフセット、`Model` に2配列 (寿命コメント必須) |
| 12 | `Source/Library/Assets/ModelLoader.h/.cpp` | `ModelLoadOptions`。L488 付近にフラグ、L523-539 のループ内で位置+インデックス構築 |
| 13 | `Source/Library/Assets/SceneLoader.h/.cpp` | `SceneLoadOptions`、L1123 で `LoadModel` へ中継 |
| 14 | 新規 `Source/Library/Assets/SoftwareRasterScene.h/.cpp` | `RaytracingScene.cpp` を手本に。統合 → `StructuredImmutable`×5 → CPU側解放 → 統計ログ |
| 15 | `KurenaiEngineLibrary.vcxproj` | `ClInclude`×2 / `ClCompile`×1 |

### C. シェーダとパス

| 順 | ファイル | 内容 |
|---|---|---|
| 16 | 新規 `Shaders/3D/SoftwareRasterCommon.hlsli` | cbuffer `SWRasterConstants`(b1)、共有構造体、エッジ関数、二分探索、`z = a + b*invW`、三角形の取得と変換 |
| 17 | 新規 `Shaders/3D/SoftwareRaster.hlsl` | `CSRaster` / `CSRasterLarge` / `CSResolve` (**BOM無しUTF-8**) |
| 18 | `KurenaiEngine3D.vcxproj` | `<None Include>`×2 (任意。IDE 表示用) |
| 19 | `Source/Engine/EngineDefaults.h` | L298 の `LightCullingEnabled` 近くに `SoftwareRasterEnabled = false` |
| 20 | `KurenaiEngine3D.cpp` 匿名名前空間 (L~1010) | `struct alignas(16) SWRasterConstants` + 「SoftwareRaster.hlsl側のcbufferと一致させる必要がある」コメント |
| 21 | `KurenaiEngine3D.h` | `static constexpr` 定数群 (`kSWRasterLargeTriangleArea=4096` / `LargeListCapacity=4096`)、シェーダ・PSO・CB・バッファのメンバ、`m_SoftwareRasterAvailable`。`m_SoftwareRasterScene` は `m_RaytracingScene` (L1746) の直後 (破棄順の不変条件に乗せる) |
| 22 | `KurenaiEngine3D.cpp` L1330 直後 | シェーダ3本 (`MinimumShaderModel = 0x66`) + PSO3本 + 定数バッファ。**`try/catch` で囲み、失敗したら `Logger::Warning` + `m_SoftwareRasterAvailable = false`** (L1318-1330 の LightCulling ブロックが雛形) |
| 23 | `KurenaiEngine3D.cpp` `CreateRenderTargets` L2450 直後 | 解像度依存リソース。**SWラスタ側だけ内側の `try/catch` で包み、失敗したら `Logger::Error` + 無効化して続行** (`catch` の Legacy8bit フォールバックは VRAM 不足を解決しないため対象外。`m_PlanarReflectionColor` L2531 と同じ判断)。無効時は全部 `nullptr` のまま |
| 24 | `KurenaiEngine3D.cpp` L3962 | 再確保条件に `\|\| m_SoftwareRasterResourcesDirty` を追加 (`WaitForGPUIdle` 済み L3981 の唯一安全な場所) |
| 25 | `KurenaiEngine3D.cpp` L2735 / L2916 / L2932 + `.h` L185-210 | `RetiredAssets` / `LoadedScene` に `SoftwareRasterScene` を追加し、`RaytracingScene` と同じ扱いにする |
| 26 | `KurenaiEngine3D.cpp` L5796 直後 | `graph.AddPass({.Name="SWRaster", .Writes={SWColor,SWDepth}, .BufferWrites={Visibility,LargeEntries,IndirectArgs}, .Execute=...})` |

### D. デバッグ表示と UI

| 順 | ファイル | 内容 |
|---|---|---|
| 27 | `KurenaiEngine3D.h` L888 の後 | `DebugView::SoftwareRaster` / `SoftwareRasterDepth` |
| 28 | `KurenaiEngine3D.cpp` L7049 付近 | `case` 2つ。**Present の Mode 4 (HDR→Reinhard+ガンマ) と Mode 5 (深度生値) を再利用**。パス未実行なら Final のまま (`PlanarReflection` L6999-7012 のパターン) |
| 29 | `UI/DebugViewPanel.cpp` L33-75 | 名前2つ追加 + `static_assert` を `SoftwareRasterDepth == 34` へ更新 |
| 30 | `UI/RenderingPanel.h/.cpp` | L52 直後に `CollapsingHeader("ソフトウェアラスタライザ###SoftwareRaster")` + `DrawSoftwareRasterSection()` (L117-137 が雛形)。`SupportsSoftwareRaster()` が false なら理由を表示してチェックボックスを無効化 |
| 31 | `README.md` / `docs/Architecture.html` | ユーザー向け = 使い方・要求環境 (DX12 + SM 6.6)・既知の制約。実装者向け = 深度テストとID書き込みの競合と64bitで解く理由・`z = a + b·invW` の導出・巨大三角形を別パスへ逃がす理由・SM 6.6 をシェーダ単位で要求する理由・RHI に足した3機能 |

**`Present.hlsl` / `RHIEnums.h` の `Format` / `ToDXGIFormat` / `kComputeUavSlotCount` には触らない。**

## 既知の制約 (フェーズ1、README に明記する)

- **DX12 かつ SM 6.6 + `Int64ShaderOps` 対応環境でのみ動く。** DX11 は `cs_5_0` 固定のため原理的に不可。非対応環境ではログを出して静かに無効化する (レイトレーシングと同じ扱い)
- **アルファカットアウト未対応。** ピクセルごとに異なるベースカラーテクスチャを引くにはバインドレス (`ResourceDescriptorHeap`) が必要。SM 6.6 を使う以上**将来的に解消できる**が、ルートシグネチャに `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` を足し、マテリアルテクスチャを永続的なシェーダ可視ヒープへ載せ替える必要があり、`DX12Device` のディスクリプタ管理 (ディスパッチごとのリングコピー) に手を入れることになる。フェーズ3へ回す。フェーズ1では Bistro の植栽・日除けは板になる
- **近平面クリッピング未実装** — near より手前に頂点がある三角形は棄却。壁に近づくと消える (フェーズ2)
- **頂点属性 (法線・UV) を持たない** — 幾何法線 (3頂点の外積) のみ。曲面がファセット状に見える (フェーズ2で +54MB)
- **フィルルール** — D3D の top-left ルールとサブピクセルスナップは再現しない。3辺とも `>= 0` で判定するので共有辺は二重被覆になるが隙間は開かない。シルエットに ±1px の差が出る
- **巨大三角形リストの容量上限 4096。** 超えた分は描かれず、マゼンタで可視化される

## 検証

### 前提を固定する

TAA を OFF にする (既定 `Defaults::TAAEnabled = false`)。`m_TAAEnabled` が false なら `jitterOffsetPixels` は必ず 0 (L4090-4098) なので、ハードとソフトが**同じ非ジッター射影行列**を使うことが保証される。自動露出 OFF、固定 FPS、同一カメラ。`verify-app` で `PostMessage` により操作し `PrintWindow` で撮影。

### 照合の3軸 (`ab-compare` を使う)

**(a) 深度の一致 — 最も強い検証。** `DebugView::深度 (生値)` (Mode 5) で `m_GBufferDepth` を撮影 → `DebugView::SW深度` で同じ Mode 5 を撮影 → 差分。`z = a + b·invW` はハードのスクリーン空間線形補間と数学的に同値なので、**シルエットの ±1px 以外は float の丸め (相対 1e-7) に収まるはず**。差が面全体に広く出たら射影行列か invW の取り違え。市松状ならフィルルールの差 (許容)。

**(b) カバレッジの一致。** 深度画像を「0か否か」で2値化して差分。ソフト側だけ穴 → 近平面クリップ未実装 (カメラを後退させて消えるか確認)、巨大三角形リストの溢れ、背面カリング符号の反転。ハード側だけ穴 → ソフトが背面を通している。

**(c) シェーディング (フェーズ2以降)。** `DebugView::法線` と Resolve の補間法線を並べる。フェーズ1は幾何法線なので、平坦面で一致しスムーズシェーディングの曲面で帯状に差が出るのが**正しい**挙動。

### 巨大三角形パスの対照実験

`kSWRasterLargeTriangleArea` を極端に小さくする (例 64) と**ほぼ全三角形が巨大リストへ回る**ので、`CSRasterLarge` 単独の正しさを検証できる。逆に極端に大きくすると `CSRaster` 単独になる。**両極端で同じ絵が出ること**を確認すれば、2つの経路が一致していることが言える (どちらか片方が実行されていない、という失敗を先に潰せる)。

### シーンの順序 (簡単なものから)

1. `MaterialTest.kscene` / `LightTest.kscene` — 三角形が少なくカットアウトも無い。ここが合わなければ座標変換かカリング符号の間違い
2. `Sponza.kscene` — カーテンのカットアウトが板になるのは想定内
3. `BistroExterior.kscene` — 284万三角形

### ビルド

- `.hlsl` を触った直後に必ず `shader-check` を通す。C++ が通っても HLSL は未検証。**ただし `SoftwareRaster.hlsl` は `cs_6_6` でないと通らないため、`shader-check` がどのコンパイラ/プロファイルで検証するかを先に確認し、必要なら SDK 26100 の `dxc.exe -T cs_6_6` を直接叩く手順を用意する**
- **DX11 でも起動して、機能が無効化されるログが出るだけで他が壊れていないことを確認する**

### GPU 時間を測るときの注意

`kMaxScopesPerFrame = 16` (`DX11GPUProfiler.h:30` / `DX12GPUProfiler.h:32`) に対し現状のフレームは既に16パスを超えている。超過分は `BeginScope` が黙って return するので、**"SWRaster" の GPU 時間が Profiler パネルに出てこない可能性が高い**。計測したいときは両バックエンドでこの定数を 24 へ上げる (1行×2ファイル) か、他のパスを一時的に切る。

## フェーズ2以降 (今回の対象外、計画のみ)

- **フェーズ2**: 近平面クリッピング、頂点属性バッファ (`RaytracingVertexAttribute` と同形 16B) による透視補正付き法線・UV 補間、巨大三角形リストの統計を RenderingPanel に表示、`SoftwareRasterScene` の統計表示
- **フェーズ3**: バインドレス (`ResourceDescriptorHeap`) によるマテリアルテクスチャのサンプリングとアルファカットアウト対応、`Present.hlsl` に Mode 20/21 (三角形ID色分け、オーバードロー)、ハード深度との差分をその場で出すデバッグビュー、`kMaxScopesPerFrame` を 24 へ、`RaytracingScene` とのジオメトリ統合と位置の 16bit 量子化
