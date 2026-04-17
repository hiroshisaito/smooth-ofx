
#ifndef __UTIL_H
#define __UTIL_H


#include <string>
#include <math.h>

#include "ofxPixels.h"

#include "define.h"

#ifdef _WIN32
#include <windows.h>
#endif


//---------------------------------------------------------------------------//
// Debugファイル
#ifdef  _DEBUG

#include <stdarg.h>

void DebugPrint(const char *format, ...);

#ifdef _WIN32
#define DEBUG_STR( str )        OutputDebugStringA( (str) )
#else
#define DEBUG_STR( str )        ((void)0)
#endif

#else

#define DEBUG_STR( str )        ((void)0)

#endif  /* _DEBUG */


//---------------------------------------------------------------------------//
// スピード測定
#define	_PROFILE	(0)

#if _PROFILE && defined(_WIN32)

struct ProfileData
{
	bool			isCounting;
	LARGE_INTEGER	lapStart;
	LARGE_INTEGER	sum;
	int				lapCount;

	ProfileData()
	{
		isCounting	= false;
		lapStart.QuadPart	= 0LL;
		sum.QuadPart		= 0LL;
		lapCount			= 0;
	}
};
void BeginProfile();
void EndProfile();
void BeginProfileLap(int index);
void EndProfileLap(int index);

#define BEGIN_PROFILE()   BeginProfile()
#define END_PROFILE()     EndProfile()
#define BEGIN_LAP(x)   BeginProfileLap((x))
#define END_LAP(x)     EndProfileLap((x))

#else

#define BEGIN_PROFILE()   ((void)0)
#define END_PROFILE()     ((void)0)
#define BEGIN_LAP(x)		((void)0)
#define END_LAP(x)			((void)0)

#endif




//---------------------------------------------------------------------------//
// 画像処理関連
//---------------------------------------------------------------------------//


// ピクセルタイプごとの最大値
template<typename PixelType> static inline unsigned int getMaxValue() { return ~0; }
template <> inline unsigned int getMaxValue<OfxRGBAColourB>() { return 0xFF; }
template <> inline unsigned int getMaxValue<OfxRGBAColourS>() { return 0xFFFF; }




//----------- 算術系 ------------//
#define CEIL(a)         (int)ceil(a)    // floatの切り上げ //

#ifndef MAX
    #define MAX(a, b)   ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
    #define MIN(a, b)   ((a) < (b) ? (a) : (b))
#endif

#ifndef ABS
    #define ABS(a)      ((a) < 0 ? -(a) : (a))
#endif


#define GET_SIGN(a)     ((a) / ABS((a)))            // 符号を得る マイナスだったら -1、プラスだったら+1




//---------------------------------------------------------------------------//
//// 関数 ///////
//---------------------------------------------------------------------------//


//-------------------------------------//
//     Pixel比較関数
//-------------------------------------//

#define ComparePixel(p0, p1)            RangeComparePixelNotEqual( &(info->in_ptr[p0]), &(info->in_ptr[p1]), info->range)
#define ComparePixelEqual(p0, p1)       RangeComparePixelEqual( &(info->in_ptr[p0]), &(info->in_ptr[p1]), info->range)



//---------------------------------------------------------------------------//
// Pixel比較関数
//---------------------------------------------------------------------------//
template <typename PixelType>
static inline bool RangeComparePixelNotEqual( const PixelType *p0, const PixelType *p1, const unsigned int range )
{
    unsigned int delta;
    delta = ABS(p0->r - p1->r) +
            ABS(p0->g - p1->g) +
            ABS(p0->b - p1->b) +
            ABS(p0->a - p1->a);

    return (delta > range);
}


template <typename PixelType>
static inline bool RangeComparePixelEqual( const PixelType *p0, const PixelType *p1, const unsigned int range )
{
    unsigned int delta;
    delta = ABS(p0->r - p1->r) +
            ABS(p0->g - p1->g) +
            ABS(p0->b - p1->b) +
            ABS(p0->a - p1->a);

    return (delta <= range);
}

// グラデーションを検知
// 3つのピクセルの平均値とCenterのピクセルを比較して、range以下だったらtrue。
// ref1,とcenterが別の色なのが前提
template <typename PixelType>
static inline bool DetectGradation( const PixelType *center,
									const PixelType *ref1,
									const PixelType *ref2,
									const unsigned int range )
{
	// 平均ピクセルを作る
	PixelType	ave;
	ave.r	= (center->r + ref1->r + ref2->r)/3;
	ave.b	= (center->b + ref1->b + ref2->b)/3;
	ave.g	= (center->g + ref1->g + ref2->g)/3;
	ave.a	= (center->a + ref1->a + ref2->a)/3;

	// centerと平均が同じで、そのほか２つとは別の色
    return RangeComparePixelEqual( center, &ave, 1 * 255 * 4 );
}


//---------------------------------------------------------------------------------------//
//概要:     ピクセル同士をブレンドする
//引数:     input,output:   おなじみ入出力画像へのポインタ
//          blend_target:   ブレンドするターゲット。結果はここのピクセルに入る
//          ref_target:     ブレンドするターゲット２。ここと上のやつのブレンドになる
//          ratio:          ブレンドする割合。1.0fでblend_targetのまんまになる 0.0fでref
//---------------------------------------------------------------------------------------//
template <typename PixelType>
static inline void BlendingPixelf(  PixelType *target_pixel,
                                    PixelType *ref_pixel,
                                    PixelType *output_pixel,
                                    float   ratio )
{
    unsigned int  max_value = getMaxValue<PixelType>();
    unsigned int alpha = (unsigned int)(max_value * ratio), r_alpha;

    r_alpha = max_value - alpha;

    if(target_pixel->a == max_value && ref_pixel->a == max_value )
    {
		// どちらも不透明
        output_pixel->a     = max_value;

        output_pixel->r     = (((target_pixel->r * alpha)+
                                (ref_pixel->r * r_alpha))/max_value);
        output_pixel->g     = (((target_pixel->g * alpha)+
                                (ref_pixel->g * r_alpha))/max_value);
        output_pixel->b     = (((target_pixel->b * alpha)+
                                (ref_pixel->b * r_alpha))/max_value);
    }
    else if(target_pixel->a == 0 )
    {
		// ターゲットが抜き
        output_pixel->a = (((target_pixel->a * alpha)+
                                        (ref_pixel->a * r_alpha))/max_value);

        output_pixel->r     = ref_pixel->r;
        output_pixel->g     = ref_pixel->g;
        output_pixel->b     = ref_pixel->b;

    }
    else if(ref_pixel->a == 0 )
    {
		// refが抜き
        output_pixel->a = (((target_pixel->a * alpha)+
                                        (ref_pixel->a * r_alpha))/max_value);

        output_pixel->r     = target_pixel->r;
        output_pixel->g     = target_pixel->g;
        output_pixel->b     = target_pixel->b;
    }
	else
	{
		// 半透明
        output_pixel->a = (((target_pixel->a * alpha)+
                                        (ref_pixel->a * r_alpha))/max_value);
        output_pixel->r     = (((target_pixel->r * alpha)+
                                (ref_pixel->r * r_alpha))/max_value);
        output_pixel->g     = (((target_pixel->g * alpha)+
                                (ref_pixel->g * r_alpha))/max_value);
        output_pixel->b     = (((target_pixel->b * alpha)+
                                (ref_pixel->b * r_alpha))/max_value);
	}
}




// float版のブレンディング命令 //
template <typename PixelType>
static inline void Blendingf(   PixelType	*in_ptr,
                                PixelType	*out_ptr,
                                long        blend_target,
                                long        ref_target,
                                long        output_target,
                                float       ratio )
{
    BlendingPixelf<PixelType>(	&(in_ptr[blend_target]),
								&(in_ptr[ref_target]),
								&(out_ptr[output_target]),
								ratio );
}


// ガンマテーブル作成 //
void CreateGanmmaTable(u_char table[256], float Ganmma);


///// デバック色の種類 ///////
// OFX 移植版: PF_LayerDef を取らず、out_stride を直接受け取る
template<typename PixelType> void SetDebugPixel(PixelType *out_ptr, int out_stride, int x, int y);
template<typename PixelType> void SetDebugPixel(PixelType *out_ptr, long target );


#ifdef _DEBUG
#define DEBUG_PIXEL(out_ptr, out_stride, x, y )    SetDebugPixel( (out_ptr), (out_stride), (x), (y) )
#define DEBUG_TARGET_PIXEL(out_ptr, t )            SetDebugPixel( (out_ptr), (t) )
#else
#define DEBUG_PIXEL(out_ptr, out_stride, x, y )    ((void)0)
#define DEBUG_TARGET_PIXEL(out_ptr, t )            ((void)0)
#endif



template<typename PixelType>
void BlendLine(     BlendingInfo<PixelType>    *pinfo,                 //
                    double          length,                 // このパターンの長さ
                    long            blend_target,           // ブレンド元のターゲット(input)
                    long            out_target,             // ブレンド先のターゲット(output)
                    int             ref_offset,             // ブレンド参照先のターゲット(input)
                    int             next_pixel_step_in,     // 次のピクセルへ移動するときこの値を足す(input)
                    int             next_pixel_step_out,    // 次のピクセルへ移動するときこの値を足す(output)
                    bool            ratio_invert,
                    bool            no_line_weight);




//-------------------------------------------------------------------------------------------
// util
//-------------------------------------------------------------------------------------------
#define SAFE_DELETE(x)	if((x)!=NULL)	{ delete (x); (x) = NULL; }



#endif
