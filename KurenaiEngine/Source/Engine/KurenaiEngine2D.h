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
        // 【アトラスを作るときの注意】2Dのサンプラーは繰り返し(Wrap)を使っているため、
        // 区画をぴったり詰めると縮小表示時に隣の区画の色がにじむ。区画の周囲には
        // 1px以上の余白(同じ色で埋めたパディング)を入れること
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

        std::unique_ptr<RHI::IRHIBuffer> m_QuadVertexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_QuadIndexBuffer;

        // 2Dはスプライトを1枚読むだけなのでs0(MaterialSampler)しか使わない。
        // スロットの役割はShaders/3D/Samplers.hlsliの定義に揃えてある(Sprite2D.hlsl参照)
        std::unique_ptr<RHI::IRHISamplerSet> m_SamplerSet;
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
