// 三角形メッシュライト1枚ぶんの評価を1か所へ集めた共有ヘッダー。
// PunctualLighting.hlsli と**並行**に置く。あちらが「点へ潰した光源」を評価するのに対し、
// こちらは発光面そのものを面積分する。
//
// 【この2つの違いは減衰の式だけである】BRDF・透過ローブ・影の掛け方・エネルギー補正は
// まったく同じものを使う。だから**このヘッダーは BRDF を絶対に再定義しない。**
// 必ず PunctualLighting.hlsli の EvaluateDirectBRDF / EvaluateTranslucency /
// ComposeSurfaceContribution を呼ぶ。式を複製すると「一致させ続けなければならない式」が
// 増え、片方だけ直したときにコンパイルも通り絵も「それらしく」出てしまう。
//
// 【インクルードする側の責務】
//   - KURENAI_MESH_LIGHT_REGISTER を、三角形テーブル
//     (StructuredBuffer<GPUEmissiveTriangle>)を置くレジスタへ #define しておくこと
//   - **このヘッダーより前に** PunctualLighting.hlsli を
//     KURENAI_PUNCTUAL_LIGHTING_BRDF つきでインクルードしておくこと
//     (ComposeSurfaceContribution と SpecularEnergyContext をそこから借りる)
//   - LightAttenuation.hlsli は PunctualLighting.hlsli が連れてくる(打ち切りの窓を共有する)
//
// 【段階1のプロキシとの関係】同じクラスタを、段階1は重心1点で、段階2は三角形の束のまま
// 積分する。遠方では両者は一致しなければならない ―― これが段階1↔段階2の突き合わせ検証になる。
//
//     段階1: 寄与 = f_r * cosθ_x * (E*A) * lobe(κ,θ_l) / (d^2 + R^2)   ← 重心1点
//     段階2: 寄与 = f_r * cosθ_x * E * cosθ_l / d^2 * A                 ← 面積一様の1サンプル

#ifndef KURENAI_MESH_LIGHTING_HLSLI
#define KURENAI_MESH_LIGHTING_HLSLI

#ifndef KURENAI_MESH_LIGHT_REGISTER
#error "MeshLighting.hlsli をインクルードする前に KURENAI_MESH_LIGHT_REGISTER を定義すること"
#endif

// 発光三角形1枚。C++側 Assets::GPUEmissiveTriangle(Assets/MeshLight.h)と
// 並び・ストライド(64バイト)を一致させること。パッキング規則の解釈揺れを避けるため
// メンバはすべて float4 単位で宣言する(GPULight と同じ作法)
struct GPUEmissiveTriangle
{
    float4 P0AndRadius;      // xyz = 頂点0(ワールド), w = 影響半径 R[m]
    float4 E1AndArea;        // xyz = P1 - P0,         w = 面積 A[m^2]
    float4 E2AndFlags;       // xyz = P2 - P0,         w = asfloat(Flags)
    // rgb = 放射輝度 L_e(EmissiveFactor × テクスチャ平均。**倍率も露出も掛かっていない**),
    // a = Luminance(L_e) * A
    //
    // 【露出を掛けてはいけない】自発光は G-Buffer からそのまま加算される経路で、
    // **露出を通らない**(ImGui の EV100 のツールチップ自身が太陽・環境光・
    // ポイント/スポットにしか掛からないと書いている)。段階1のプロキシも同じ理由で
    // ColorRange に E*A を露出抜きで入れている。ここで露出を掛けると EV100=15 で
    // 1/39322 倍になり、**絵は「真っ暗だが厳密に0ではない」という紛らわしい形になる**
    // (実際に踏んだ。総和比 3e-5 = ちょうど ComputeExposure(15))。
    //
    // 【シーン全体の自発光倍率はここに入っていない】毎フレーム変わるので焼けない。
    // 段階1が MakeGPULightFromEmissiveProxy で毎フレーム掛けているのと同じものを、
    // こちらはフレーム定数から掛ける。**掛け忘れると ImGui の「自発光の強度」が
    // メッシュライトにだけ効かなくなる**(これも実際に踏んだ)
    float4 RadianceAndFlux;
};

StructuredBuffer<GPUEmissiveTriangle> EmissiveTriangles : register(KURENAI_MESH_LIGHT_REGISTER);

// GPUEmissiveTriangle::E2AndFlags.w のビット定義(Assets/MeshLight.h と一致させること)
static const uint kMeshLightFlagDoubleSided = 1u;

// 近距離での 1/d^2 の発散を止める下限。**三角形自身の大きさ基準にする。**
//
// 【絶対長にしない理由】シーンスケール依存になる(DistanceAttenuation の既存の判断と同じ)。
// 【小さく取る理由】ここは物理的な近傍軟化ではなく、ゼロ除算を防ぐだけの床である。
// 段階1の d^2 + R^2 は「面を点へ潰した」ことの補正だったが、段階2は面積分そのものなので、
// 近傍の軟化は積分が自分で行う。ここを大きく取ると**積分の答えを変えてしまう。**
// 1e-6 * A は面積 1m^2 の三角形で下限距離 1mm にあたる。
// 【張り付き率を測ること】クランプに当たった画素の割合を出さずに使ってはいけない。
// 当たっているなら、それは分散が原理的に手に負えない配置(2.13-1)か、三角形が大きすぎて
// 分割(2g)が要るかのどちらかで、どちらも「数字は出るが実物に無い形」になる
static const float kMeshLightMinDistSqAreaScale = 1e-6f;
// 面積が0に潰れた三角形での保険。ここへ落ちる三角形はそもそも寄与0になる
static const float kMeshLightMinDistSqFloor = 1e-8f;

// 三角形上を**面積一様**に引く。u は [0,1)^2 の乱数2つ。
// 返すのは重心座標 (b1, b2) で、点は P0 + b1*E1 + b2*E2。
//
// 【この写像が受光点 x を含まないことが段階2の要である】リザーバが運ぶのは
// 「シーン中の1点 y」で、測度は dA、密度は 1/A。別の画素で使い直すと被積分関数は
// 変わるが**サンプル空間も測度も変わらない**ので、時空間再利用のシフトは恒等・
// ヤコビアンは厳密に1になる。
//
// 【だから次を絶対に入れてはいけない】球面三角形の立体角一様(Arvo 1995)、
// 投影立体角・cos重みの面積サンプリング、受光点の cosθ/d^2 で歪めた面積サンプリング、
// 「大きい三角形は近い側を厚く引く」適応サンプリング。いずれも写像が x に依存するので
// ヤコビアンが要るようになる。**このヘッダーの API が (triangle, bary) しか受け取らず、
// worldPos からサンプル点を作る経路を持たないのは、それを構造的に禁じるためである。**
//
// 【混同しやすい2つ】
//   サンプル空間のパラメータ化が x に依存する  → ヤコビアンが要る。**禁止**
//   どのサンプルを選ぶ確率が x やタイルに依存する → **問題ない。**それは提案分布であり、
//                                                   不偏寄与重みは提案分布に対して不可知
float2 MeshLightSampleBarycentric(float2 u)
{
    const float su = sqrt(u.x);
    return float2(1.0f - su, u.y * su);
}

// 重心座標から三角形上のワールド座標を作る
float3 MeshLightSamplePosition(GPUEmissiveTriangle tri, float2 bary)
{
    return tri.P0AndRadius.xyz + bary.x * tri.E1AndArea.xyz + bary.y * tri.E2AndFlags.xyz;
}

// 三角形1枚について、シャドウを掛ける前に決まる幾何と面積測度の幾何項。
// PunctualGeometry と同じ役割で、early-out もここで済ませる
struct MeshLightGeometry
{
    float3 L;          // 受光点からサンプル点へ向かう単位ベクトル
    float G;           // 面積測度の幾何項 A * cosθ_y * window^2 / max(d^2, k*A)
    float Distance;    // サンプル点までの距離
    bool Contributes;  // false ならこの三角形の寄与は厳密に0
};

// 【この関数を参照実装と確率的サンプリングで共有する理由】PunctualLighting.hlsli の
// EvaluatePunctualGeometry とまったく同じ。early-out の並びは寄与の値を変えないが、
// **どの三角形が寄与0とみなされるかを決めている**ので、式そのものと同じ重さで
// 一致させる必要がある。定義域がずれると期待値がずれる(=バイアス)
// rangeScale は影響半径の伸縮。sqrt(自発光の強度倍率) を渡す。
//
// 【なぜ伸縮が要るのか】影響半径は読み込み時に「倍率1」で焼いてある。段階1の Range は
// peak = max(RadianceBase) * intensity * Area から毎フレーム解き直されるので、
// 倍率を上げると伸びる。こちらが固定のままだと、倍率を上げたときだけ段階1と段階2で
// 届く距離が食い違う。R ∝ sqrt(peak) なので sqrt(intensity) を掛ければ一致する
MeshLightGeometry EvaluateMeshLightGeometry(
    GPUEmissiveTriangle tri, float2 bary, float3 worldPos, float3 N, float translucency,
    float rangeScale)
{
    MeshLightGeometry result;
    result.L = float3(0.0f, 1.0f, 0.0f);
    result.G = 0.0f;
    result.Distance = 1e30f;
    result.Contributes = false;

    const float3 samplePos = MeshLightSamplePosition(tri, bary);
    const float3 toLight = samplePos - worldPos;
    const float distSq = dot(toLight, toLight);

    // 打ち切り。三角形には Range が無いので、読み込み時に焼いた影響半径 R で切る。
    // 【打ち切りを入れない選択肢は無い】入れないとタイルへ届くかの判定を保守的に書けず、
    // 候補プールが成立しない。正しさの契約は「参照実装と確率的サンプリングで定義域が
    // 一致すること」であって「物理的に無限遠まで積むこと」ではない(punctual も Range で
    // 既に切っている)
    const float range = tri.P0AndRadius.w * rangeScale;
    if (distSq > range * range)
    {
        return result;
    }

    const float window = LightRangeWindow(distSq, range);
    if (window <= 0.0f)
    {
        return result;
    }

    const float dist = sqrt(max(distSq, 1e-16f));
    const float3 L = toLight / dist;

    // 発光面の向き。**幾何法線で決める**(頂点法線ではない)。
    // 光っている面積を決めているのは幾何であり、シェーディング法線を使うと
    // 面積分の対象とずれる
    const float3 nGeo = normalize(cross(tri.E1AndArea.xyz, tri.E2AndFlags.xyz));
    float cosLight = dot(nGeo, -L);

    // 片面発光が既定。glTF の doubleSided な発光面だけ裏からも光る。
    // 【この述語は参照実装・提案分布・評価で完全に同一でなければならない】
    // ここがずれると「片方だけが裏面を光らせる」形になり、絵は出るが真値と食い違う
    const uint flags = asuint(tri.E2AndFlags.w);
    if ((flags & kMeshLightFlagDoubleSided) != 0u)
    {
        cosLight = abs(cosLight);
    }
    if (cosLight <= 0.0f)
    {
        return result;
    }

    // 【透過するマテリアルでは NdotL<=0 でも打ち切らない】薄いものは裏から当たった光を
    // 透かすため、その側にこそ寄与がある(EvaluatePunctualGeometry と同じ扱い)
    if (dot(N, L) <= 0.0f && translucency <= 0.0f)
    {
        return result;
    }

    const float area = tri.E1AndArea.w;
    const float minDistSq = max(kMeshLightMinDistSqAreaScale * area, kMeshLightMinDistSqFloor);

    result.L = L;
    result.G = area * cosLight * (window * window) / max(distSq, minDistSq);
    result.Distance = dist;
    result.Contributes = true;
    return result;
}

// 三角形1枚ぶんの寄与(反射 + 透過、シャドウ適用済み)。shadow は可視率(0=完全に影, 1=遮蔽なし)。
//
// 【emissiveIntensity を引数で受け取る理由】シーン全体の自発光倍率は毎フレーム変わるので
// テーブルへ焼けない。段階1が MakeGPULightFromEmissiveProxy で毎フレーム掛けているのと
// 同じ値を、こちらはフレーム定数から掛ける。これで ImGui のスライダーがライブに効く。
//
// **露出はここに現れない。** 自発光は露出を通らない経路であり、段階1のプロキシも
// 露出抜きで ColorRange を作っている。掛けると 1/39322 倍になる(EV100=15)。
//
// 【lightColor と geometryTerm を分けたまま ComposeSurfaceContribution へ渡す】
// 掛け算の順序を punctual と揃えるため。積を先に作ると結合が変わって丸めが動く
float3 EvaluateMeshLightContribution(
    GPUEmissiveTriangle tri, MeshLightGeometry geometry, float3 N, float3 V, float NdotV,
    float3 albedo, float metallic, float roughness, float translucency,
    SpecularEnergyContext energy, float shadow, float emissiveIntensity)
{
    return ComposeSurfaceContribution(
        N, V, geometry.L, NdotV, albedo, metallic, roughness, translucency, energy, shadow,
        tri.RadianceAndFlux.rgb * emissiveIntensity, geometry.G);
}

#endif // KURENAI_MESH_LIGHTING_HLSLI
