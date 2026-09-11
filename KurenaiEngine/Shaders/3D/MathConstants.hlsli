#ifndef KURENAI_MATH_CONSTANTS_HLSLI
#define KURENAI_MATH_CONSTANTS_HLSLI

// シェーダー全体で使う数学定数。**同じ値を各ファイルで宣言し直さない。**
//
// 値が同じであるうちは複製の害が出ないが、桁を足す・減らすといった変更を1か所だけに
// 入れると、どのパスが古い値で走っているのかを追う手立てがない
// (散っていた頃の経緯は docs/ImplementationHistory.md 82章)。
//
// 【ここに置くのは「どのパスから見ても同じ意味の定数」だけ】レイの押し出し量のように
// 経路ごとに調整しうるものは、その経路のファイルに置いたままにする。
#ifndef KURENAI_MATH_PI
#define KURENAI_MATH_PI
static const float PI = 3.14159265359f;
#endif

#endif // KURENAI_MATH_CONSTANTS_HLSLI
