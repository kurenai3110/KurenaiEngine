#ifndef KURENAI_CONSTANTS_2D_HLSLI
#define KURENAI_CONSTANTS_2D_HLSLI

// KurenaiEngine2D が使う定数バッファ。**同じレイアウトを各シェーダで宣言し直さない。**
//
// 以前は Sprite2D.hlsl と Polyline2D.hlsl が同じ2つの cbuffer をそれぞれ書いており、
// C++ 側(KurenaiEngine2D.cpp)と合わせて同じレイアウトが3回書かれていた。
// フィールドを1か所だけ足すと、残りは黙って別のレイアウトのまま焼かれる。
//
// 【C++ 側との一致は static_assert が守る】KurenaiEngine2D.cpp の offsetof の
// static_assert がこのファイルの並びと対になっている。**片方だけ直さないこと。**
// ただし守れるのはオフセットとサイズだけで、フィールドの意味までは見ていない。

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
};

cbuffer ObjectConstants : register(b1)
{
    // DrawPolyline では未使用(頂点は既にワールド座標で渡す)
    float4x4 World;
    float4 Color;
    // xy=UVオフセット, zw=UVスケール。DrawText(フォントアトラスの1文字ぶんの矩形)専用で、
    // それ以外(DrawSprite/DrawCircle/DrawRoundedRect)は(0, 0, 1, 1)の恒等変換で呼ぶ。
    // DrawPolyline では未使用
    float4 UVOffsetScale;
    // DrawRoundedRect/DrawCircle専用。xy=半幅・半高さ(ピクセル), z=角丸半径(ピクセル),
    // w=枠線太さ(ピクセル)。DrawCircleはxy・zすべてに半径を入れる(= 角丸半径が半幅・半高さと
    // 等しい角丸矩形は円そのもの、という関係だが、円は専用の距離関数のほうが安いのでPSは分けてある)。
    // DrawPolyline では未使用
    float4 ShapeParams;
    // DrawRoundedRect/DrawCircle専用。枠線の色(borderThicknessPixels<=0のときは未使用)。
    // DrawPolyline では未使用
    float4 BorderColor;
};

#endif // KURENAI_CONSTANTS_2D_HLSLI
