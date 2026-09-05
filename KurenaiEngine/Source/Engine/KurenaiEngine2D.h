#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "KurenaiEngineBase.h"
#include "KurenaiTypes.h"

#include "Core/Camera.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai
{
    // KurenaiEngine2D::LoadTexture / CreateSolidColorTexture が返す不透明なテクスチャハンドル。
    // 内部型を一切持たないため、KurenaiEngine.dllのエクスポート境界をまたいでも安全に値渡しできる
    class TextureHandle
    {
    public:
        TextureHandle() = default;
        bool IsValid() const { return m_Handle != nullptr; }

    private:
        explicit TextureHandle(void* handle) : m_Handle(handle) {}
        void* m_Handle = nullptr;
        friend class KurenaiEngine2D;
    };

    // 2Dサンプルプログラム向けの公開API。正射影カメラとアルファブレンドによる
    // スプライト描画を提供する。ワールド座標=ピクセル座標(原点は画面左下、Y-up)。
    //
    // 使い方:
    //   KurenaiEngine2D renderer(L"Title", 1280, 720);
    //   TextureHandle tex = renderer.CreateSolidColorTexture(255, 255, 255, 255);
    //   while (!renderer.ShouldClose())
    //   {
    //       renderer.PumpEvents();
    //       renderer.BeginFrame(0.1f, 0.1f, 0.1f);
    //       renderer.DrawSprite(x, y, w, h, 0.0f, tex, 1, 1, 1, 1);
    //       renderer.EndFrame();
    //   }
    class KURENAI_2D_API KurenaiEngine2D : public KurenaiEngineBase
    {
    public:
        KurenaiEngine2D(const std::wstring& title, uint32_t width, uint32_t height, GraphicsAPI api = GraphicsAPI::DX11);
        ~KurenaiEngine2D();

        TextureHandle LoadTexture(const std::wstring& filePath, bool sRGB = false);
        TextureHandle CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

        // 画面をクリアし、以後のDrawSprite呼び出しの準備をする。1フレームにつき1回だけ呼ぶ
        void BeginFrame(float clearR, float clearG, float clearB, float clearA = 1.0f);

        // --- 2Dカメラ ---
        //
        // これらを一度も呼ばなければ、従来どおり「クライアント領域を過不足なく映す」状態になる
        // (ワールド座標0〜幅・0〜高さがそのまま画面いっぱいに出て、ウィンドウのリサイズにも追従する)。
        // 反映はBeginFrame()の時点なので、設定はBeginFrame()より前に行うこと。

        // カメラ中心のワールド座標。設定するとウィンドウサイズへの自動追従をやめ、この値で固定する
        void SetCameraPosition(float x, float y);
        // 実際に使われているカメラ中心を返す(SetCameraPosition未設定の場合も既定値が返る)
        void GetCameraPosition(float& outX, float& outY) const;
        // 1.0で等倍、2.0で2倍に拡大表示(= 見えるワールドの範囲が1/2になる)。
        // 0以下・NaNはログを出して無視する
        void SetCameraZoom(float zoom);
        float GetCameraZoom() const { return m_CameraZoom; }

        // 論理解像度を指定する。指定するとワールド座標0〜width・0〜heightの範囲が、
        // アスペクト比を保ったままクライアント領域の中央へ収まるようスケール＋センタリングされる
        // (余る側にはレターボックス/ピラーボックスが出る。余白はBeginFrameのクリア色になる)。
        // ウィンドウサイズが変わっても見えるワールドの範囲は変わらないため、
        // 「1600x900で組んだ盤面を、どのウィンドウサイズでも同じ構図で見せる」用途に使う。
        // (0, 0)を渡すと解除され、クライアント領域をそのまま論理解像度として使う既定へ戻る
        // (このときsnapToIntegerScaleの値は無視される)。
        //
        // snapToIntegerScale=trueにすると、拡大率をfloorして整数倍へ落とす(余った分は
        // レターボックス/ピラーボックスの余白へ回る)。ビューポートの原点も整数へ丸めるため、
        // 論理解像度の1テクセルが画面の整数個の画素へ正確に対応する。
        // 【ドット絵にはSetSpriteFilter(Point)と両方が必要】拡大率が実数のままだと、
        // 点サンプリングにしてもテクセル中心が画素中心からずれて輪郭が滲む。
        //
        // 整数倍スナップの制限:
        // - クライアント領域が論理解像度より小さいと整数倍が取れない(floorが0になる)。
        //   この場合はスナップせず実数倍へフォールバックする(絵が消えるのを避けるため。
        //   最初の1回だけログに残す)
        // - SetCameraZoomと併用する場合、画面上の総拡大率は「整数倍 × ズーム倍率」になる。
        //   ズームに整数以外を指定すると総拡大率が実数へ戻るため、ドット絵では整数のズームを使うこと
        void SetVirtualResolution(float width, float height, bool snapToIntegerScale = false);

        // クライアント座標(GetClientMousePositionが返す、原点は左上・Y-downのピクセル座標)を
        // ワールド座標(原点は左下・Y-up)へ変換する。カメラ位置・ズーム・論理解像度による
        // レターボックスをすべて考慮するため、マウスの当たり判定はこれを通して行うこと。
        // レターボックスの余白の上を指した場合は論理解像度の外側の座標が返る
        void ClientToWorld(float clientX, float clientY, float& outWorldX, float& outWorldY) const;
        // ClientToWorldの逆変換
        void WorldToClient(float worldX, float worldY, float& outClientX, float& outClientY) const;
        // GetClientMousePosition()にClientToWorldを適用した結果
        void GetMouseWorldPosition(float& outWorldX, float& outWorldY) const;

        // --- スプライトのサンプリング方法 ---
        //
        // 以後のDrawSprite / DrawSpriteUV / DrawLineに適用される(描画状態として保持されるので、
        // 呼び出しごとに指定する必要はない)。DrawCircle / DrawRoundedRect / DrawPolylineは
        // テクスチャを読まないため影響を受けず、DrawTextは常にLinear+Clampの専用サンプラーを使う
        // (フォントアトラスを点サンプリングにすると文字が汚れるため、ここの設定では変わらない)。
        //
        // フレームの途中でも切り替えられる(「盤面のドット絵はPoint、UIの素材はLinear」のように
        // 1フレーム内で混ぜてよい)。既定はAnisotropic + Wrapで、従来の見た目と同一。
        //
        // 無効な値を渡した場合はログを出して呼び出しを無視する

        void SetSpriteFilter(SpriteFilter filter);
        SpriteFilter GetSpriteFilter() const { return m_SpriteFilter; }
        void SetSpriteAddressMode(SpriteAddressMode addressMode);
        SpriteAddressMode GetSpriteAddressMode() const { return m_SpriteAddressMode; }

        // x, y はワールド=ピクセル座標(原点は画面左下、Y-up)のスプライト中心位置。
        // width, height はピクセル単位のスプライトサイズ。rotationRadiansはZ軸(画面手前向き)回転。
        // r, g, b, a はテクスチャに乗算されるティント色(半透明にしたい場合はaを1未満にする)
        void DrawSprite(
            float x, float y, float width, float height, float rotationRadians,
            TextureHandle texture, float r, float g, float b, float a);

        // DrawSpriteの、テクスチャの一部だけを描画する版。srcU0, srcV0, srcU1, srcV1は
        // 0.0〜1.0の正規化UVで、原点は左上・V下向き(テクスチャの標準的な向き)。
        // 複数のアイコンを1枚のアトラスにまとめ、テクスチャの切り替え回数を減らす用途で使う
        // (x, y, width, height, rotationRadians, r, g, b, aの意味はDrawSpriteと同じ)。
        //
        // 【アトラスを作るときの注意】既定のアドレスモードは繰り返し(Wrap)なので、
        // 区画をぴったり詰めると縮小表示時にフィルタのタップが反対側の端へ回り込み、
        // 隣の区画の色がにじむ。SetSpriteAddressMode(Clamp)にすると回り込み自体は無くなるが、
        // 【隣接する区画どうしのにじみはClampでも消えない】(タップが反対側へ回らず隣の区画へ
        // はみ出すだけになる)。どちらの設定でも区画の周囲には1px以上の余白
        // (同じ色で埋めたパディング)を入れること
        void DrawSpriteUV(
            float x, float y, float width, float height, float rotationRadians,
            TextureHandle texture, float srcU0, float srcV0, float srcU1, float srcV1,
            float r, float g, float b, float a);

        // テクスチャのピクセルサイズを返す。アトラスの区画をピクセルで管理してから
        // DrawSpriteUVへ渡す正規化UVを求める用途で使う。
        // 無効なハンドルの場合はログを出して0を返す
        void GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) const;

        // 中心(x, y)、半径radiusの塗り円を描画する。r, g, b, aは塗りつぶし色(半透明可)。
        // borderThicknessPixelsを0より大きくすると、塗りの内側にborderR/G/B/Aの枠線を
        // 重ねて描画する(DrawRoundedRectと同じ形。既定では枠線なし)。
        // 塗りをa=0にして枠線だけを指定すると、中が完全に透明なリングになる
        // (射程円のように下の描画を隠したくない用途向け)
        void DrawCircle(
            float x, float y, float radius,
            float r, float g, float b, float a,
            float borderThicknessPixels = 0.0f,
            float borderR = 0.0f, float borderG = 0.0f, float borderB = 0.0f, float borderA = 0.0f);

        // (x1, y1)-(x2, y2)を結ぶ、太さthicknessの線分を描画する。r, g, b, aは色(半透明可)
        void DrawLine(float x1, float y1, float x2, float y2, float thickness, float r, float g, float b, float a);

        // pointsは{x0, y0, x1, y1, ...}の順に並んだ点列。thicknessは帯の太さ(ピクセル)。
        // 角の接合はマイター(鋭角時はベベルへフォールバック)、端は切りっぱなし(バットキャップ)。
        //
        // DrawLineを繋いで折れ線を描くと角の外側に扇形の隙間が空くが、これは点列をまとめて
        // 1つのジオメトリ(重なりの無い三角形の集まり)として描くため隙間ができない。
        // また各画素がきっかり1回だけブレンドされるので、【半透明でも接合部の色が濃くならない】。
        //
        // 制限:
        // - 折れ線が【自分自身と交差する】場合、その交点だけは2回ブレンドされる
        //   (1パスのアルファブレンドでは原理的に解決できない)
        // - 1本あたりの点数の上限は1024点。超えた場合はログを出して先頭1024点へ切り詰める
        // - 1フレームあたり32本まで。超えた場合はログを出してその呼び出しを描画しない
        //   (DX12のステージングリングの段数で決まる上限)
        void DrawPolyline(const std::vector<float>& points, float thickness, float r, float g, float b, float a);

        // 中心(x, y)、サイズwidth x height、角丸半径cornerRadiusPixelsの角丸矩形を描画する。
        // r, g, b, aは塗りつぶし色(半透明可)。borderThicknessPixelsを0より大きくすると、
        // 塗りの内側にborderR/G/B/Aの枠線を重ねて描画する(既定では枠線なし)。
        // rotationRadiansはDrawSpriteと同じZ軸(画面手前向き)回転で、中心(x, y)まわりに回る。
        // width/heightと角丸半径は回転前のローカル空間での値なので、回しても角丸・枠線の
        // 太さは変わらない(既定値0なので、回転を使わない呼び出しは従来どおり)
        void DrawRoundedRect(
            float x, float y, float width, float height, float cornerRadiusPixels,
            float r, float g, float b, float a,
            float borderThicknessPixels = 0.0f,
            float borderR = 0.0f, float borderG = 0.0f, float borderB = 0.0f, float borderA = 0.0f,
            float rotationRadians = 0.0f);

        // fontSizeはおおよその文字高さ(ピクセル単位)。ビットマップフォント方式のため、
        // 厳密なフォントレンダリング(ヒンティング等)は行わない。ASCII印字可能文字(0x20〜0x7E)に
        // 加え、かな漢字を含む任意のUnicode文字(BMP範囲)に対応する。
        // ただし初めて描画する文字はその場ではアトラスに含まれていないため1フレームだけ表示されず、
        // 次のBeginFrame()でアトラスへ追加されてから以降のフレームで表示される
        // (フレーム中にテクスチャを作り直すとDX12でレンダーターゲット/パイプラインステートの設定が
        // 失われるため、追加はBeginFrame()の先頭でのみ行う設計になっている)。
        // bold=trueの場合、通常とは別に構築される太字(FW_BOLD)アトラスを使って描画する。
        // xの意味はalignで変わる(Left=テキスト左端基準、Center(既定)=テキスト中央基準、
        // Right=テキスト右端基準)。yの意味はverticalAlignで変わる(Bottom=テキスト下端基準、
        // Middle(既定)=テキスト上下中央基準、Top=テキスト上端基準)。align=Center/Rightまたは
        // verticalAlign=Bottom/Middleの場合、内部でMeasureText相当の幅・高さ計測を行ってから
        // 描画開始位置を決めるため、呼び出し側で手動に幅・高さを計算する必要はない
        // (align=Left・verticalAlign=Topの組み合わせのみ、計測なしでそのまま(x, y)を使う)。
        //
        // textに含まれる'\n'で改行し、2行目以降はGetLineHeight()ぶん下へ送って描画する
        // ("\r\n"の'\r'は読み飛ばす)。alignは【行ごと】に適用し(Centerなら行ごとの中央揃え)、
        // verticalAlignは【テキストブロック全体】に対して適用する。
        // 指定幅での自動折り返しは行わないため、折り返しが要る場合は呼び出し側がMeasureTextで
        // 折り返し位置を決め、'\n'を挿入した文字列を渡すこと
        void DrawText(
            float x, float y, const std::wstring& text, float fontSize, float r, float g, float b, float a,
            bool bold = false, TextAlign align = TextAlign::Center, TextVerticalAlign verticalAlign = TextVerticalAlign::Middle);

        // textをfontSize(・bold)で描画した場合の実測済み幅(ピクセル単位、AdvancePixelsの合計)を返す。
        // ボタンラベル等の正確な中央揃えに使う。DrawTextと同様、アトラス未収録の文字はその場では
        // 幅0として扱われ、次のBeginFrame()でアトラスに追加された以降は正しい幅が返る。
        // 改行は解釈しないため、複数行の文字列にはMeasureTextBlockを使うこと
        float MeasureText(const std::wstring& text, float fontSize, bool bold = false);

        // fontSize(・bold)で描画したときの1行ぶんの高さ(ピクセル単位)。
        // fontSizeからの推測ではなく、GDIのTEXTMETRICW::tmHeightから決まる実際のセル高さを
        // 返すため、フォントを差し替えても正しい値になる
        float GetLineHeight(float fontSize, bool bold = false) const;

        // 改行を考慮したテキストブロック全体の幅(最も長い行の幅)と高さ(行高さ×行数)を返す。
        // 説明文を囲むパネルの大きさを、描画前にAPI呼び出し1回で決めるために使う。
        // MeasureTextと同様、アトラス未収録の文字はその場では幅0として扱われる
        void MeasureTextBlock(
            const std::wstring& text, float fontSize, float& outWidth, float& outHeight, bool bold = false);

        // 以後の描画を、ワールド座標(=ピクセル座標。原点は画面左下、Y-up)の矩形の内側だけに
        // 制限する。x, yは矩形の【中心】、width/heightはサイズで、DrawSprite/DrawRoundedRectと
        // まったく同じ引数の意味にしてある(パネルをDrawRoundedRectで描いた直後に同じ引数で
        // PushClipRectすれば、その内側へ子要素を閉じ込められる、というのが最も多い使い方のため)。
        //
        // ネストした場合は現在の矩形との積が有効になる。積が空になった場合、対応する
        // PopClipRectまでの描画は1ピクセルも出ない。カメラ位置・ズーム・論理解像度による
        // レターボックスはすべて考慮される。クリップできるのは軸平行な矩形のみで、
        // 回転や角丸には追従しない(角丸パネルの内側を切りたい場合は外接矩形になる)。
        //
        // BeginFrameとEndFrameの間で呼ぶこと(BeginFrameがスタックを空に戻す)
        void PushClipRect(float x, float y, float width, float height);
        // 直近のPushClipRectを取り消す。対応するPushClipRectが無い場合はログを出して何もしない
        void PopClipRect();

        // 描画コマンドを確定してバックバッファへ表示する。1フレームにつき1回だけ呼ぶ
        void EndFrame(bool vsync = true);

    private:
        // DrawText用の1文字ぶんのメトリクス。すべてBuildFontAtlasの生成時解像度(m_FontAtlasPixelHeight)基準の値
        struct GlyphMetrics
        {
            float U0 = 0.0f, V0 = 0.0f, U1 = 0.0f, V1 = 0.0f; // アトラス内のUV矩形
            float AdvancePixels = 0.0f;
            float WidthPixels = 0.0f, HeightPixels = 0.0f;
        };
        // GDIでcharsに含まれる文字一式をラスタライズし、bold=falseならm_FontAtlasTexture/m_Glyphsを、
        // bold=trueならm_BoldFontAtlasTexture/m_BoldGlyphsを(既存の内容を置き換えて)再構築する。
        // コンストラクタ、またはBeginFrame()の先頭でのみ呼ぶ必要がある
        // (DX12のCreateTextureFromMemoryは内部でコマンドリストをフラッシュ・リセットするため、
        // 通常の描画コマンドを積んだ後のフレーム中に呼ぶと、それらの設定が失われクラッシュする)
        void BuildFontAtlas(const std::vector<wchar_t>& chars, bool bold);

        // m_PendingChars(bold=false)またはm_PendingBoldChars(bold=true)に溜まった文字があれば、
        // 対応する既存グリフ一式と合わせてBuildFontAtlasで再構築する。BeginFrame()の先頭から呼ぶ
        void RebuildFontAtlasIfPending(bool bold);

        // 文字chのメトリクスを返す。boldに応じてm_Glyphs/m_BoldGlyphsを参照し、未収録の場合は
        // 対応するpendingキューへ積んでnullptrを返す(DrawText/MeasureText共通のヘルパー)
        const GlyphMetrics* FindGlyph(wchar_t ch, bool bold);

        // 初回のASCII一式(0x20〜0x7E)を返す。コンストラクタでのBuildFontAtlas呼び出し用
        static std::vector<wchar_t> DefaultAsciiChars();

        // textを'\n'で行へ分割する(CRLFの'\r'は読み飛ばす)。改行が無ければ1要素のまま返る。
        // DrawTextとMeasureTextBlockが同じ行分割を使うためのヘルパー
        static std::vector<std::wstring> SplitTextIntoLines(const std::wstring& text);

        Core::Camera m_Camera;

        // --- 2Dカメラの状態(SetCameraPosition / SetCameraZoom / SetVirtualResolution) ---
        //
        // 「一度も設定していなければクライアント領域を過不足なく映す」という既定を保つため、
        // 値そのものではなく「設定されたか」をフラグで持つ。未設定の間はComputeViewStateが
        // 論理解像度の中央を実効カメラ位置として毎フレーム計算するので、ウィンドウの
        // リサイズに自動追従する(= これらのAPIを一切呼ばないアプリの見た目は従来と同一)
        float m_CameraX = 0.0f;
        float m_CameraY = 0.0f;
        bool m_HasCameraPosition = false;
        float m_CameraZoom = 1.0f;
        float m_VirtualWidth = 0.0f;
        float m_VirtualHeight = 0.0f;
        bool m_HasVirtualResolution = false;
        bool m_SnapVirtualResolutionToIntegerScale = false;
        // 整数倍スナップを要求されたがクライアント領域が論理解像度より小さく、実数倍へ
        // フォールバックしたことを最初の1回だけ知らせるためのフラグ。ComputeViewStateは
        // constで毎フレーム(かつ座標変換のたびに)呼ばれるため、mutableにして中で立てる
        // (毎回出すとログが埋まる。m_ClipRectLeakLoggedと同じ方針)
        mutable bool m_IntegerScaleFallbackLogged = false;

        // BeginFrameと座標変換が共有する「そのフレームの実効値」。2か所で別々に計算すると、
        // 片方だけ直したときにマウス座標だけずれるという気付きにくい不整合が起きるため、
        // 計算はComputeViewState()の1か所に閉じる
        struct ViewState
        {
            // クライアント領域内の描画先(レターボックス適用済み。ピクセル、原点は左上)
            float ViewportX = 0.0f, ViewportY = 0.0f;
            float ViewportWidth = 0.0f, ViewportHeight = 0.0f;
            // 論理解像度(SetVirtualResolution未設定ならクライアント領域と同じ)
            float LogicalWidth = 0.0f, LogicalHeight = 0.0f;
            // 実効カメラ中心(ワールド座標)と実効ズーム
            float CameraCenterX = 0.0f, CameraCenterY = 0.0f;
            float Zoom = 1.0f;
        };
        ViewState ComputeViewState() const;

        // PushClipRect/PopClipRectのスタック。各要素は「そこまでのネストの積を取り終えた後の」
        // クライアント座標(原点は左上・Y-down、ピクセル)の矩形。積は浮動小数のまま取り、
        // RHIへ渡す直前にだけ整数へ丸める(先に丸めるとネストのたびに誤差が積み上がるため)
        struct ClipRect
        {
            float Left = 0.0f, Top = 0.0f, Right = 0.0f, Bottom = 0.0f;
        };
        std::vector<ClipRect> m_ClipRectStack;
        // Push/Popの数が合わないままEndFrameを迎えたことを、最初の1回だけログに残すためのフラグ
        // (毎フレーム出すとログが埋まる)
        bool m_ClipRectLeakLogged = false;
        // m_ClipRectStackの先頭(空ならビューポート全体)をコマンドリストへ反映する
        void ApplyClipRect();

        std::unique_ptr<RHI::IRHIShader> m_VertexShader;
        std::unique_ptr<RHI::IRHIShader> m_PixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PipelineState;

        // DrawCircle用。頂点シェーダー・頂点/インデックスバッファはスプライトと共用し、
        // ピクセルシェーダーとパイプラインステートのみ専用のものを使う
        std::unique_ptr<RHI::IRHIShader> m_CirclePixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_CirclePipelineState;

        // DrawRoundedRect用。DrawCircleと同様、頂点シェーダー・頂点/インデックスバッファは
        // スプライトと共用し、ピクセルシェーダーとパイプラインステートのみ専用のものを使う
        std::unique_ptr<RHI::IRHIShader> m_RoundedRectPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_RoundedRectPipelineState;

        // DrawLineは太さ・長さに拡縮縮小した矩形として、この不透明白テクスチャを使ってDrawSpriteと
        // 同じスプライトパイプラインで描画する
        TextureHandle m_WhiteTexture;

        // DrawPolyline用。頂点バッファを使わず、CPUで接合まで済ませた三角形リストを
        // StructuredBufferへ載せて頂点シェーダがSV_VertexIDで引く(Shaders/2D/Polyline2D.hlsl)。
        // DX12は頂点バッファを毎フレーム書き換えられないため、この経路しか採れない
        struct PolylineVertex
        {
            float Position[2]; // ワールド=ピクセル座標。HLSL側のPolylineVertexと一致させること
        };
        std::unique_ptr<RHI::IRHIShader> m_PolylineVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_PolylinePixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PolylinePipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_PolylineVertexBuffer;
        // 毎フレームの再確保を避けるための作業領域(BuildPolylineGeometryが書き込む)
        std::vector<PolylineVertex> m_PolylineVertices;
        // 1フレームあたりの本数制限のカウンタ(BeginFrameでリセット)。DX12のステージングリングを
        // 周回して描画結果が静かに壊れる前に、こちら側で先回りして弾くために持つ
        uint32_t m_PolylineDrawsThisFrame = 0;
        bool m_PolylineOverflowLogged = false;

        // pointsからマイター/ベベル接合済みの三角形リストを組み、m_PolylineVerticesへ書き込む。
        // 戻り値は生成した頂点数(生成できなかった場合は0)
        uint32_t BuildPolylineGeometry(const std::vector<float>& points, float halfThickness);

        std::unique_ptr<RHI::IRHIBuffer> m_QuadVertexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_QuadIndexBuffer;

        // 2Dはスプライトを1枚読むだけなのでs0(MaterialSampler)しか使わない。
        // スロットの役割はShaders/3D/Samplers.hlsliの定義に揃えてある(Sprite2D.hlsl参照)。
        //
        // 【セットを作り置きする理由】IRHIDevice::CreateSamplerSetは描画開始前(初期化時)にしか
        // 呼べない(セットの中身が不変であることが前提の設計。理由はRHI/IRHISamplerSet.h)。
        // そのためSetSpriteFilter/SetSpriteAddressModeで都度作るわけにはいかず、
        // フィルタ×アドレスモードの全組み合わせをコンストラクタで作り、
        // 以後は「どれをバインドするか」の選択だけを行う
        static constexpr uint32_t kSpriteFilterCount = 3;      // SpriteFilterの要素数
        static constexpr uint32_t kSpriteAddressModeCount = 2; // SpriteAddressModeの要素数
        std::unique_ptr<RHI::IRHISamplerSet> m_SpriteSamplerSets[kSpriteFilterCount * kSpriteAddressModeCount];
        // DrawText専用。フォントアトラスは点サンプリングにすると文字が汚れるため常にLinear、
        // アトラスの端のセルでタップが反対側へ回り込まないよう常にClampにする
        std::unique_ptr<RHI::IRHISamplerSet> m_TextSamplerSet;
        SpriteFilter m_SpriteFilter = SpriteFilter::Anisotropic;      // 既定は従来と同じ
        SpriteAddressMode m_SpriteAddressMode = SpriteAddressMode::Wrap;
        // 直近にコマンドリストへバインドしたセット。同じセットの積み直しを省くためだけに持つ
        // (BeginFrameでnullptrへ戻す)
        RHI::IRHISamplerSet* m_BoundSamplerSet = nullptr;
        // m_SpriteSamplerSetsの添字。全組み合わせが作り置きされている前提の単純な計算
        static uint32_t SpriteSamplerIndex(SpriteFilter filter, SpriteAddressMode addressMode);
        // samplerSetが既にバインド済みなら何もしない。テクスチャを読む描画(DrawSpriteUV/DrawText)が
        // Drawの直前に呼ぶ
        void BindSamplerSet(RHI::IRHISamplerSet* samplerSet);
        std::unique_ptr<RHI::IRHIBuffer> m_FrameConstantBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_ObjectConstantBuffer;

        // LoadTexture/CreateSolidColorTextureで読み込んだテクスチャの実体を保持する
        // (TextureHandleは所有権を持たない借用ポインタのため)
        std::vector<std::unique_ptr<RHI::IRHITexture>> m_Textures;

        // DrawText用。コンストラクタでBuildFontAtlasにより生成される
        std::unique_ptr<RHI::IRHITexture> m_FontAtlasTexture;
        std::unordered_map<wchar_t, GlyphMetrics> m_Glyphs;
        // BuildFontAtlasが生成したフォントの基準ピクセル高さ。DrawTextのfontSizeはこれに対する
        // 拡大率(fontSize / m_FontAtlasPixelHeight)としてグリフの表示サイズに反映される
        float m_FontAtlasPixelHeight = 0.0f;
        // アトラス内の1文字ぶんのセル高さ(パディング込み。GlyphMetrics::HeightPixelsと同じ値で、
        // 文字集合によらずアトラス全体で共通)。DrawTextのverticalAlign(Middle/Top)で、個々の文字を
        // 探さずに済むよう、この値だけでテキスト全体の高さを算出するために使う
        float m_FontAtlasCellHeight = 0.0f;
        // DrawTextでm_Glyphsに見つからなかった(=アトラス未収録の)文字を一時的に溜めておくキュー。
        // 次のBeginFrame()の先頭でm_Glyphsの既存キーと合わせてBuildFontAtlasに渡され、消費後クリアされる
        std::vector<wchar_t> m_PendingChars;

        // DrawText(bold=true)/MeasureText(bold=true)用の太字版一式。通常版とは別のフォント
        // (FW_BOLD)・別のアトラス・別の文字集合として独立に管理する(役割は上記の各メンバと同様)
        std::unique_ptr<RHI::IRHITexture> m_BoldFontAtlasTexture;
        std::unordered_map<wchar_t, GlyphMetrics> m_BoldGlyphs;
        float m_BoldFontAtlasPixelHeight = 0.0f;
        float m_BoldFontAtlasCellHeight = 0.0f;
        std::vector<wchar_t> m_PendingBoldChars;
    };
}

#pragma warning(pop)
