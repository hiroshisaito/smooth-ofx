//---------------------------------------------------------------------------------------//
// 概要: smooth内でいろいろ使う関数など
// 更新: 2003 4/21  (AE original)
//       2026 OFX port: BlendingInfo から PF_LayerDef 依存を除去
//---------------------------------------------------------------------------------------//

#ifndef __DEFINE_H
#define __DEFINE_H

// 型 //
typedef unsigned char   u_char;
typedef unsigned int    u_int;
typedef unsigned short  u_short;
typedef unsigned long   u_long;



// OFXはRGBA (r, g, b, a の順)


struct Cinfo
{
    long    length;     // カウントした画素数
    float   start, end; // 実際にブレンドする座標値(その該当する方向のものしか有効でない)
    u_int   flg;        // 特殊処理用のフラグ

    Cinfo()  {length = 0; flg = 0;} // コンストラクタ
};

// ↑のフラグ //
#define CR_FLG_FILL         (1<<0) // べた塗りモード


// 合成関数用の情報用構造体 //
// OFX 移植版: PF_LayerDef の代わりに stride/height を直接保持
template <typename PixelType>
struct BlendingInfo
{
    PixelType   *in_ptr, *out_ptr;  // 画像データへのポインタ
    int         in_stride;          // 入力の1行あたりピクセル数 (rowbytes / sizeof(PixelType))
    int         out_stride;         // 出力の1行あたりピクセル数
    int         height;             // 画像の高さ (ピクセル単位)
    int         i, j;               // 現在処理中の座標値
    long        in_target,          // 処理中の座標値の1次配列インデックス (input に対する)
                out_target;         //                                    (output に対する)
    Cinfo       core[4];            // 処理すべき長さ、フラグなどのコアの情報 0:left 1:right 2:top 3:bottom
    int         flag;               // 制御用のフラグ(補正用のカウントなど)
    unsigned int range;             // 同じ色とみなす範囲
    int         mode;               // 処理するモード
    float       LineWeight;         // ラインの太さ

    BlendingInfo()
        : in_ptr(nullptr), out_ptr(nullptr),
          in_stride(0), out_stride(0), height(0),
          i(0), j(0), in_target(0), out_target(0),
          flag(0), range(0), mode(0), LineWeight(0.0f)
    {}
};

// ↑のフラグ
#define SECOND_COUNT    (1<<0)      // 補正用の2度目のカウント

// ↑のMode //
enum
{
    BLEND_MODE_UP_H = 0,            // 上向きの角の横方向
    BLEND_MODE_UP_V,                // 上向きの角の縦方向
    BLEND_MODE_DOWN_H,              // 下向きの角の横方向
    BLEND_MODE_DOWN_V,              // 下向きの角の縦方向
};


#endif
