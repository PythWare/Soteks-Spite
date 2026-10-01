#ifndef SOTEK_SURFACE_H
#define SOTEK_SURFACE_H

#include "common.h"
#include <objidl.h>
#include <gdiplus.h>

#define COLOR_LILAC 0xFFBF98D9u
#define COLOR_LILAC_DEEP 0xFFB089CDu
#define COLOR_LILAC_HOVER 0xFFD2B5E6u
#define COLOR_LILAC_BUSY 0xFFA37BBFu
#define COLOR_RED 0xFFB8233Fu
#define COLOR_RED_DARK 0xFF8E1B31u
#define COLOR_TEXT 0xFF5A1630u
#define COLOR_TEXT_DIM 0xFF6E4A85u

typedef struct Theme
{
    float scale;
    GpSolidFill *lilac;
    GpSolidFill *lilacDeep;
    GpSolidFill *lilacHover;
    GpSolidFill *lilacBusy;
    GpSolidFill *text;
    GpSolidFill *textDim;
    GpPen *red;
    GpPen *redDark;
    GpPen *scrollTrack;
    GpPen *scrollThumb;
    GpPen *scrollThumbHover;
    GpFontFamily *family;
    GpFont *labelFont;
    GpFont *titleFont;
    GpFont *statusFont;
    GpFont *rowFont;
    GpStringFormat *centered;
    GpStringFormat *rowFormat;
    float rowAdvance;
    float rowPadding;
} Theme;

typedef struct Surface
{
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ previous;
    void *bits;
    int width;
    int height;
    GpBitmap *image;
    GpGraphics *graphics;
    GpSolidFill *clear;
} Surface;

typedef struct Pyramid
{
    float centerX;
    float apexY;
    float baseY;
    float baseHalf;
} Pyramid;

int ThemeCreate(Theme *theme, float scale);
void ThemeDestroy(Theme *theme);

int SurfaceCreate(Surface *surface, int width, int height);
void SurfaceDestroy(Surface *surface);
void SurfaceBeginBand(Surface *surface, float top, float bottom);
void SurfaceEndBand(Surface *surface);
void SurfacePresent(Surface *surface, HWND window, float top, float bottom);

float PyramidHalf(const Pyramid *pyramid, float y);
int PyramidContains(const Pyramid *pyramid, float x, float y, float top, float bottom);
void DrawBand(Surface *surface, const Theme *theme, const Pyramid *pyramid, float top, float bottom, GpSolidFill *fill, int topEdgeBright);
void DrawLabel(Surface *surface, GpFont *font, GpSolidFill *brush, GpStringFormat *format, const wchar_t *text, float centerX, float centerY, float width, float height);

#endif
