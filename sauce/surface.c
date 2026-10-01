#include "surface.h"

static GpFont *CreateThemeFont(GpFontFamily *family, float pixels, int style)
{
    GpFont *font = NULL;

    GdipCreateFont(family, pixels, style, UnitPixel, &font);
    return font;
}

static GpPen *CreateRoundPen(ARGB color, float width)
{
    GpPen *pen = NULL;

    if (GdipCreatePen1(color, width, UnitPixel, &pen) != Ok)
        return NULL;
    GdipSetPenStartCap(pen, LineCapRound);
    GdipSetPenEndCap(pen, LineCapRound);
    return pen;
}

static GpStringFormat *CreateFormat(StringTrimming trimming)
{
    GpStringFormat *format = NULL;

    if (GdipCreateStringFormat(StringFormatFlagsNoWrap, LANG_NEUTRAL, &format) != Ok)
        return NULL;
    GdipSetStringFormatAlign(format, StringAlignmentCenter);
    GdipSetStringFormatLineAlign(format, StringAlignmentCenter);
    GdipSetStringFormatTrimming(format, trimming);
    return format;
}

static void MeasureRows(Theme *theme)
{
    GpBitmap *bitmap = NULL;
    GpGraphics *graphics = NULL;
    wchar_t sample[64];
    RectF layout = {0, 0, 8192, 512};
    RectF longBox = {0, 0, 0, 0};
    RectF shortBox = {0, 0, 0, 0};
    size_t index;

    for (index = 0; index < 64; index++)
        sample[index] = L'M';
    if (theme->rowFont && theme->rowFormat &&
        GdipCreateBitmapFromScan0(1, 1, 0, PixelFormat32bppPARGB, NULL, &bitmap) == Ok &&
        GdipGetImageGraphicsContext((GpImage *)bitmap, &graphics) == Ok)
    {
        GdipSetTextRenderingHint(graphics, TextRenderingHintAntiAliasGridFit);
        GdipMeasureString(graphics, sample, 64, theme->rowFont, &layout, theme->rowFormat, &longBox, NULL, NULL);
        GdipMeasureString(graphics, sample, 32, theme->rowFont, &layout, theme->rowFormat, &shortBox, NULL, NULL);
        theme->rowAdvance = (longBox.Width - shortBox.Width) / 32;
        theme->rowPadding = longBox.Width - 64 * theme->rowAdvance;
    }
    if (graphics)
        GdipDeleteGraphics(graphics);
    if (bitmap)
        GdipDisposeImage((GpImage *)bitmap);
    if (theme->rowAdvance <= 0)
    {
        theme->rowAdvance = 8.0f * theme->scale;
        theme->rowPadding = 6.0f * theme->scale;
    }
    if (theme->rowPadding < 0)
        theme->rowPadding = 0;
}

int ThemeCreate(Theme *theme, float scale)
{
    memset(theme, 0, sizeof(*theme));
    theme->scale = scale;
    GdipCreateSolidFill(COLOR_LILAC, &theme->lilac);
    GdipCreateSolidFill(COLOR_LILAC_DEEP, &theme->lilacDeep);
    GdipCreateSolidFill(COLOR_LILAC_HOVER, &theme->lilacHover);
    GdipCreateSolidFill(COLOR_LILAC_BUSY, &theme->lilacBusy);
    GdipCreateSolidFill(COLOR_TEXT, &theme->text);
    GdipCreateSolidFill(COLOR_TEXT_DIM, &theme->textDim);
    GdipCreatePen1(COLOR_RED, 2.0f * scale, UnitPixel, &theme->red);
    GdipCreatePen1(COLOR_RED_DARK, 2.0f * scale, UnitPixel, &theme->redDark);
    theme->scrollTrack = CreateRoundPen(0x808E1B31u, 2.0f * scale);
    theme->scrollThumb = CreateRoundPen(COLOR_RED_DARK, 7.0f * scale);
    theme->scrollThumbHover = CreateRoundPen(COLOR_RED, 9.0f * scale);
    if (GdipCreateFontFamilyFromName(L"Consolas", NULL, &theme->family) != Ok)
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &theme->family);
    if (theme->family)
    {
        theme->labelFont = CreateThemeFont(theme->family, 17.0f * scale, FontStyleBold);
        theme->titleFont = CreateThemeFont(theme->family, 19.0f * scale, FontStyleBold);
        theme->statusFont = CreateThemeFont(theme->family, 12.0f * scale, FontStyleRegular);
        theme->rowFont = CreateThemeFont(theme->family, 13.0f * scale, FontStyleRegular);
    }
    theme->centered = CreateFormat(StringTrimmingEllipsisCharacter);
    theme->rowFormat = CreateFormat(StringTrimmingEllipsisCharacter);
    MeasureRows(theme);
    return theme->lilac && theme->lilacDeep && theme->lilacHover && theme->lilacBusy && theme->text && theme->textDim && theme->red && theme->redDark &&
           theme->scrollTrack && theme->scrollThumb && theme->scrollThumbHover &&
           theme->labelFont && theme->titleFont && theme->statusFont && theme->rowFont && theme->centered && theme->rowFormat;
}

void ThemeDestroy(Theme *theme)
{
    GpSolidFill *fills[] = {theme->lilac, theme->lilacDeep, theme->lilacHover, theme->lilacBusy, theme->text, theme->textDim};
    GpFont *fonts[] = {theme->labelFont, theme->titleFont, theme->statusFont, theme->rowFont};
    size_t index;

    for (index = 0; index < sizeof(fills) / sizeof(fills[0]); index++)
        if (fills[index])
            GdipDeleteBrush((GpBrush *)fills[index]);
    for (index = 0; index < sizeof(fonts) / sizeof(fonts[0]); index++)
        if (fonts[index])
            GdipDeleteFont(fonts[index]);
    if (theme->red)
        GdipDeletePen(theme->red);
    if (theme->redDark)
        GdipDeletePen(theme->redDark);
    if (theme->scrollTrack)
        GdipDeletePen(theme->scrollTrack);
    if (theme->scrollThumb)
        GdipDeletePen(theme->scrollThumb);
    if (theme->scrollThumbHover)
        GdipDeletePen(theme->scrollThumbHover);
    if (theme->family)
        GdipDeleteFontFamily(theme->family);
    if (theme->centered)
        GdipDeleteStringFormat(theme->centered);
    if (theme->rowFormat)
        GdipDeleteStringFormat(theme->rowFormat);
    memset(theme, 0, sizeof(*theme));
}

int SurfaceCreate(Surface *surface, int width, int height)
{
    BITMAPINFO info;

    memset(surface, 0, sizeof(*surface));
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    surface->dc = CreateCompatibleDC(NULL);
    if (!surface->dc)
        return 0;
    surface->bitmap = CreateDIBSection(surface->dc, &info, DIB_RGB_COLORS, &surface->bits, NULL, 0);
    if (!surface->bitmap || !surface->bits)
    {
        SurfaceDestroy(surface);
        return 0;
    }
    surface->previous = SelectObject(surface->dc, surface->bitmap);
    surface->width = width;
    surface->height = height;
    if (GdipCreateBitmapFromScan0(width, height, width * 4, PixelFormat32bppPARGB, surface->bits, &surface->image) != Ok ||
        GdipGetImageGraphicsContext((GpImage *)surface->image, &surface->graphics) != Ok ||
        GdipCreateSolidFill(0x00000000u, &surface->clear) != Ok)
    {
        SurfaceDestroy(surface);
        return 0;
    }
    GdipSetSmoothingMode(surface->graphics, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(surface->graphics, PixelOffsetModeHalf);
    GdipSetTextRenderingHint(surface->graphics, TextRenderingHintAntiAliasGridFit);
    GdipSetCompositingQuality(surface->graphics, CompositingQualityHighSpeed);
    return 1;
}

void SurfaceDestroy(Surface *surface)
{
    if (surface->clear)
        GdipDeleteBrush((GpBrush *)surface->clear);
    if (surface->graphics)
        GdipDeleteGraphics(surface->graphics);
    if (surface->image)
        GdipDisposeImage((GpImage *)surface->image);
    if (surface->dc && surface->previous)
        SelectObject(surface->dc, surface->previous);
    if (surface->bitmap)
        DeleteObject(surface->bitmap);
    if (surface->dc)
        DeleteDC(surface->dc);
    memset(surface, 0, sizeof(*surface));
}

void SurfaceBeginBand(Surface *surface, float top, float bottom)
{
    if (top < 0)
        top = 0;
    if (bottom > (float)surface->height)
        bottom = (float)surface->height;
    GdipSetClipRect(surface->graphics, 0, top, (float)surface->width, bottom - top, CombineModeReplace);
    GdipSetCompositingMode(surface->graphics, CompositingModeSourceCopy);
    GdipFillRectangle(surface->graphics, (GpBrush *)surface->clear, 0, top, (float)surface->width, bottom - top);
    GdipSetCompositingMode(surface->graphics, CompositingModeSourceOver);
}

void SurfaceEndBand(Surface *surface)
{
    GdipFlush(surface->graphics, FlushIntentionSync);
    GdipResetClip(surface->graphics);
}

void SurfacePresent(Surface *surface, HWND window, float top, float bottom)
{
    SIZE size;
    POINT origin = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    RECT dirty;
    UPDATELAYEREDWINDOWINFO info;

    size.cx = surface->width;
    size.cy = surface->height;
    dirty.left = 0;
    dirty.right = surface->width;
    dirty.top = top < 0 ? 0 : (LONG)top;
    dirty.bottom = bottom > (float)surface->height ? surface->height : (LONG)(bottom + 1);
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.psize = &size;
    info.hdcSrc = surface->dc;
    info.pptSrc = &origin;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = &dirty;
    if (!UpdateLayeredWindowIndirect(window, &info))
    {
        info.prcDirty = NULL;
        UpdateLayeredWindowIndirect(window, &info);
    }
}

float PyramidHalf(const Pyramid *pyramid, float y)
{
    return pyramid->baseHalf * (y - pyramid->apexY) / (pyramid->baseY - pyramid->apexY);
}

int PyramidContains(const Pyramid *pyramid, float x, float y, float top, float bottom)
{
    float delta = x - pyramid->centerX;

    if (y < top || y > bottom)
        return 0;
    return (delta < 0 ? -delta : delta) <= PyramidHalf(pyramid, y);
}

void DrawBand(Surface *surface, const Theme *theme, const Pyramid *pyramid, float top, float bottom, GpSolidFill *fill, int topEdgeBright)
{
    GpPointF points[4];
    float bottomHalf = PyramidHalf(pyramid, bottom);
    int count;

    if (top <= pyramid->apexY + 0.5f)
    {
        points[0].X = pyramid->centerX;
        points[0].Y = pyramid->apexY;
        points[1].X = pyramid->centerX + bottomHalf;
        points[1].Y = bottom;
        points[2].X = pyramid->centerX - bottomHalf;
        points[2].Y = bottom;
        count = 3;
    }
    else
    {
        float topHalf = PyramidHalf(pyramid, top);

        points[0].X = pyramid->centerX - topHalf;
        points[0].Y = top;
        points[1].X = pyramid->centerX + topHalf;
        points[1].Y = top;
        points[2].X = pyramid->centerX + bottomHalf;
        points[2].Y = bottom;
        points[3].X = pyramid->centerX - bottomHalf;
        points[3].Y = bottom;
        count = 4;
    }
    GdipFillPolygon(surface->graphics, (GpBrush *)fill, points, count, FillModeAlternate);
    GdipDrawPolygon(surface->graphics, count == 3 ? theme->red : theme->redDark, points, count);
    if (count == 4 && topEdgeBright)
        GdipDrawLine(surface->graphics, theme->red, points[0].X, points[0].Y, points[1].X, points[1].Y);
}

void DrawLabel(Surface *surface, GpFont *font, GpSolidFill *brush, GpStringFormat *format, const wchar_t *text, float centerX, float centerY, float width, float height)
{
    RectF layout;

    layout.X = centerX - width / 2;
    layout.Y = centerY - height / 2;
    layout.Width = width;
    layout.Height = height;
    GdipDrawString(surface->graphics, text, -1, font, &layout, format, (GpBrush *)brush);
}
