#include "triangle.h"
#include <windowsx.h>

#define TRIANGLE_CLASS L"SotekSpiteTriangle"
#define KIND_MESSAGE 1
#define KIND_CHOICE 2
#define TIMER_LOG 1
#define TIMER_COPIED 2
#define HIT_NONE (-1)
#define HIT_SCROLL 1000
#define MESSAGE_WIDTH 720.0f
#define MESSAGE_HEIGHT 620.0f
#define MESSAGE_APEX 16.0f
#define MESSAGE_BASE 600.0f
#define MESSAGE_BASE_HALF 352.0f
#define MESSAGE_TITLE_BOTTOM 205.0f
#define MESSAGE_FOOTER_TOP 528.0f

typedef struct TriangleState
{
    int kind;
    HWND window;
    HWND owner;
    const Theme *theme;
    Surface surface;
    Pyramid pyramid;
    int width;
    int height;
    float titleBottom;
    float footerTop;
    float listTop;
    float listBottom;
    float rowHeight;
    size_t slotCount;
    size_t *slotCapacity;
    ErrorLog *log;
    LONG seenVersion;
    LONG cachedVersion;
    int cachedValid;
    size_t cachedMaximum;
    size_t firstLine;
    size_t lineCount;
    size_t shownLines;
    size_t maximumFirst;
    int followTail;
    int copied;
    wchar_t *titleLineOne;
    wchar_t *titleLineTwo;
    wchar_t **choices;
    int choiceCount;
    float *edges;
    int answered;
    int hovered;
    int pressed;
    int tracking;
    int dragging;
    POINT dragCursor;
    POINT dragWindow;
    int thumbDragging;
    float thumbGrab;
} TriangleState;

static HWND messageWindow;

static float Scale(const TriangleState *state, float value)
{
    return value * state->theme->scale;
}

static float SlotTop(const TriangleState *state, size_t slot)
{
    return state->listTop + state->rowHeight * (float)slot;
}

static float SlotWidth(const TriangleState *state, size_t slot)
{
    return 2 * (PyramidHalf(&state->pyramid, SlotTop(state, slot)) - Scale(state, 34));
}

static int BreaksAfter(wchar_t character)
{
    return character == L'\\' || character == L'/' || character == L' ' || character == L'_' || character == L'-';
}

static size_t SegmentLength(const wchar_t *text, size_t length, size_t capacity)
{
    size_t index;
    size_t lowest = capacity - capacity / 3;

    if (length <= capacity)
        return length;
    for (index = capacity; index > lowest; index--)
        if (text[index - 1] == L'\\' || text[index - 1] == L'/' || text[index - 1] == L' ')
            return index;
    for (index = capacity; index > lowest; index--)
        if (BreaksAfter(text[index - 1]) || text[index] == L'.')
            return index;
    return capacity;
}

static void DrawSegment(TriangleState *state, size_t slot, const wchar_t *text, size_t length, size_t line, float top, float bottom)
{
    float rowTop = SlotTop(state, slot);
    float width = SlotWidth(state, slot);
    RectF layout;

    if (!length || rowTop + state->rowHeight < top || rowTop > bottom || width <= 0)
        return;
    layout.X = state->pyramid.centerX - width / 2;
    layout.Y = rowTop;
    layout.Width = width;
    layout.Height = state->rowHeight;
    GdipDrawString(state->surface.graphics, text, (INT)length, state->theme->rowFont, &layout, state->theme->rowFormat,
                   (GpBrush *)(line % 2 ? state->theme->textDim : state->theme->text));
}

static size_t PlaceLines(TriangleState *state, size_t first, int draw, float top, float bottom)
{
    ErrorLog *log = state->log;
    size_t slot = 0;
    size_t line;

    for (line = first; line < log->lineCount && slot < state->slotCount; line++)
    {
        const wchar_t *text = log->text + log->lineStarts[line];
        size_t remaining = log->lineLengths[line];

        do
        {
            size_t capacity = state->slotCapacity[slot];
            size_t take = capacity ? SegmentLength(text, remaining, capacity) : 0;

            if (draw)
                DrawSegment(state, slot, text, take, line, top, bottom);
            text += take;
            remaining -= take;
            slot++;
        } while (remaining && slot < state->slotCount);
        if (remaining)
            break;
    }
    return line - first;
}

static size_t MaximumFirstLine(TriangleState *state)
{
    size_t count = state->log->lineCount;
    size_t first = count ? count - 1 : 0;

    if (state->cachedValid && state->cachedVersion == state->log->version)
        return state->cachedMaximum;
    while (first > 0 && PlaceLines(state, first - 1, 0, 0, 0) == count - first + 1)
        first--;
    state->cachedMaximum = first;
    state->cachedVersion = state->log->version;
    state->cachedValid = 1;
    return first;
}

static size_t CurrentMaximumFirst(TriangleState *state)
{
    size_t maximum;

    AcquireSRWLockShared(&state->log->lock);
    maximum = MaximumFirstLine(state);
    ReleaseSRWLockShared(&state->log->lock);
    return maximum;
}

static long long Page(const TriangleState *state)
{
    return state->shownLines ? (long long)state->shownLines : 1;
}

static float TrackX(const TriangleState *state, float y)
{
    return state->pyramid.centerX + PyramidHalf(&state->pyramid, y) - Scale(state, 14);
}

static void TrackRange(const TriangleState *state, float *top, float *bottom)
{
    *top = state->listTop + Scale(state, 6);
    *bottom = state->listBottom - Scale(state, 6);
}

static void ThumbRange(const TriangleState *state, float *top, float *bottom)
{
    float trackTop;
    float trackBottom;
    float span;
    float size;
    float position = state->maximumFirst ? (float)state->firstLine / (float)state->maximumFirst : 0.0f;

    TrackRange(state, &trackTop, &trackBottom);
    span = trackBottom - trackTop;
    size = state->lineCount ? span * (float)state->shownLines / (float)state->lineCount : span;
    if (size < Scale(state, 28))
        size = Scale(state, 28);
    if (size > span)
        size = span;
    *top = trackTop + (span - size) * position;
    *bottom = *top + size;
}

static void DrawScrollbar(TriangleState *state)
{
    float trackTop;
    float trackBottom;
    float thumbTop;
    float thumbBottom;

    TrackRange(state, &trackTop, &trackBottom);
    ThumbRange(state, &thumbTop, &thumbBottom);
    GdipDrawLine(state->surface.graphics, state->theme->scrollTrack, TrackX(state, trackTop), trackTop, TrackX(state, trackBottom), trackBottom);
    GdipDrawLine(state->surface.graphics, state->hovered == HIT_SCROLL || state->thumbDragging ? state->theme->scrollThumbHover : state->theme->scrollThumb,
                 TrackX(state, thumbTop), thumbTop, TrackX(state, thumbBottom), thumbBottom);
}

static void FillHalf(TriangleState *state, float top, float bottom, int right, GpSolidFill *fill)
{
    GpPointF points[4];
    float topHalf = PyramidHalf(&state->pyramid, top);
    float bottomHalf = PyramidHalf(&state->pyramid, bottom);
    float center = state->pyramid.centerX;
    float sign = right ? 1.0f : -1.0f;

    points[0].X = center + sign * topHalf;
    points[0].Y = top;
    points[1].X = center;
    points[1].Y = top;
    points[2].X = center;
    points[2].Y = bottom;
    points[3].X = center + sign * bottomHalf;
    points[3].Y = bottom;
    GdipFillPolygon(state->surface.graphics, (GpBrush *)fill, points, 4, FillModeAlternate);
    GdipDrawPolygon(state->surface.graphics, state->theme->redDark, points, 4);
}

static void RenderMessage(TriangleState *state, float top, float bottom)
{
    const Theme *theme = state->theme;
    Surface *surface = &state->surface;
    float center = state->pyramid.centerX;
    float footerMiddle = (state->footerTop + state->pyramid.baseY) / 2;
    float footerHalf = PyramidHalf(&state->pyramid, footerMiddle);
    float subY = state->titleBottom - Scale(state, 30);
    wchar_t *subline;

    SurfaceBeginBand(surface, top, bottom);
    DrawBand(surface, theme, &state->pyramid, state->pyramid.apexY, state->titleBottom, theme->lilacDeep, 0);
    DrawBand(surface, theme, &state->pyramid, state->titleBottom, state->footerTop, theme->lilac, 1);
    FillHalf(state, state->footerTop, state->pyramid.baseY, 0, state->hovered == 0 ? theme->lilacHover : theme->lilacDeep);
    FillHalf(state, state->footerTop, state->pyramid.baseY, 1, state->hovered == 1 ? theme->lilacHover : theme->lilacDeep);
    GdipDrawLine(surface->graphics, theme->red, center - PyramidHalf(&state->pyramid, state->footerTop), state->footerTop, center + PyramidHalf(&state->pyramid, state->footerTop), state->footerTop);
    DrawLabel(surface, theme->titleFont, theme->text, theme->centered, L"Errors", center, state->titleBottom - Scale(state, 62), Scale(state, 200), Scale(state, 30));
    DrawLabel(surface, theme->labelFont, theme->text, theme->centered, L"Copy all", center - footerHalf / 2, footerMiddle, footerHalf, Scale(state, 30));
    DrawLabel(surface, theme->labelFont, theme->text, theme->centered, L"Close", center + footerHalf / 2, footerMiddle, footerHalf, Scale(state, 30));
    AcquireSRWLockShared(&state->log->lock);
    state->lineCount = state->log->lineCount;
    state->maximumFirst = MaximumFirstLine(state);
    if (state->followTail || state->firstLine > state->maximumFirst)
        state->firstLine = state->maximumFirst;
    state->shownLines = PlaceLines(state, state->firstLine, 1, top, bottom);
    ReleaseSRWLockShared(&state->log->lock);
    if (state->maximumFirst)
        DrawScrollbar(state);
    if (state->copied)
        subline = WideFormat(L"Copied to the clipboard");
    else if (!state->lineCount)
        subline = WideFormat(L"No errors");
    else if (!state->shownLines)
        subline = WideFormat(L"Line %zu of %zu", state->firstLine + 1, state->lineCount);
    else
        subline = WideFormat(L"Lines %zu to %zu of %zu", state->firstLine + 1, state->firstLine + state->shownLines, state->lineCount);
    DrawLabel(surface, theme->statusFont, theme->textDim, theme->centered, subline, center, subY, 2 * PyramidHalf(&state->pyramid, subY - Scale(state, 8)), Scale(state, 20));
    MemoryRelease(subline);
    SurfaceEndBand(surface);
    SurfacePresent(surface, state->window, top, bottom);
}

static void RenderChoice(TriangleState *state, float top, float bottom)
{
    const Theme *theme = state->theme;
    Surface *surface = &state->surface;
    float center = state->pyramid.centerX;
    int index;

    SurfaceBeginBand(surface, top, bottom);
    DrawBand(surface, theme, &state->pyramid, state->pyramid.apexY, state->titleBottom, theme->lilacDeep, 0);
    for (index = 0; index <= state->choiceCount; index++)
    {
        GpSolidFill *fill = state->hovered == index ? theme->lilacHover : (index % 2 == 0 ? theme->lilac : theme->lilacDeep);
        float slabTop = state->edges[index];
        float slabBottom = state->edges[index + 1];
        float middle = (slabTop + slabBottom) / 2;
        const wchar_t *label = index < state->choiceCount ? state->choices[index] : L"Cancel";

        DrawBand(surface, theme, &state->pyramid, slabTop, slabBottom, fill, 1);
        DrawLabel(surface, theme->labelFont, theme->text, theme->centered, label, center, middle, 2 * PyramidHalf(&state->pyramid, slabTop), slabBottom - slabTop);
    }
    DrawLabel(surface, theme->titleFont, theme->text, theme->centered, state->titleLineOne, center, state->titleBottom - Scale(state, 58), Scale(state, 200), Scale(state, 28));
    DrawLabel(surface, theme->titleFont, theme->text, theme->centered, state->titleLineTwo, center, state->titleBottom - Scale(state, 30), Scale(state, 200), Scale(state, 28));
    SurfaceEndBand(surface);
    SurfacePresent(surface, state->window, top, bottom);
}

static void Render(TriangleState *state, float top, float bottom)
{
    if (state->kind == KIND_MESSAGE)
        RenderMessage(state, top, bottom);
    else
        RenderChoice(state, top, bottom);
}

static void RenderAll(TriangleState *state)
{
    Render(state, 0, (float)state->surface.height);
}

static void RenderList(TriangleState *state)
{
    Render(state, state->titleBottom - Scale(state, 45), state->footerTop);
}

static int HitTest(TriangleState *state, float x, float y)
{
    int index;

    if (state->kind == KIND_MESSAGE)
    {
        float trackTop;
        float trackBottom;
        float distance;

        TrackRange(state, &trackTop, &trackBottom);
        distance = x - TrackX(state, y);
        if (y >= trackTop - Scale(state, 6) && y <= trackBottom + Scale(state, 6) && distance <= Scale(state, 11) && distance >= -Scale(state, 11) && state->maximumFirst)
            return HIT_SCROLL;
        if (!PyramidContains(&state->pyramid, x, y, state->footerTop, state->pyramid.baseY))
            return HIT_NONE;
        return x < state->pyramid.centerX ? 0 : 1;
    }
    for (index = 0; index <= state->choiceCount; index++)
        if (PyramidContains(&state->pyramid, x, y, state->edges[index], state->edges[index + 1]))
            return index;
    return HIT_NONE;
}

static void ScrollTo(TriangleState *state, long long first)
{
    size_t maximum = CurrentMaximumFirst(state);

    if (first < 0)
        first = 0;
    if ((unsigned long long)first > maximum)
        first = (long long)maximum;
    state->followTail = (size_t)first >= maximum;
    if ((size_t)first == state->firstLine)
        return;
    state->firstLine = (size_t)first;
    RenderList(state);
}

static void ScrollBy(TriangleState *state, long long delta)
{
    ScrollTo(state, (long long)state->firstLine + delta);
}

static void DragThumb(TriangleState *state, float y)
{
    float trackTop;
    float trackBottom;
    float thumbTop;
    float thumbBottom;
    float travel;

    TrackRange(state, &trackTop, &trackBottom);
    ThumbRange(state, &thumbTop, &thumbBottom);
    travel = (trackBottom - trackTop) - (thumbBottom - thumbTop);
    if (travel <= 0)
        return;
    ScrollTo(state, (long long)((y - state->thumbGrab - trackTop) / travel * (float)state->maximumFirst + 0.5f));
}

static void PressScrollbar(TriangleState *state, float y)
{
    float thumbTop;
    float thumbBottom;

    ThumbRange(state, &thumbTop, &thumbBottom);
    if (y >= thumbTop - Scale(state, 4) && y <= thumbBottom + Scale(state, 4))
    {
        state->thumbDragging = 1;
        state->thumbGrab = y - thumbTop;
        SetCapture(state->window);
        RenderList(state);
    }
    else
        ScrollBy(state, y < thumbTop ? -Page(state) : Page(state));
}

static void CopyAll(TriangleState *state)
{
    wchar_t *joined = ErrorLogJoined(state->log);
    size_t bytes = (wcslen(joined) + 1) * sizeof(wchar_t);
    HGLOBAL memory;

    if (OpenClipboard(state->window))
    {
        EmptyClipboard();
        memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory)
        {
            void *target = GlobalLock(memory);

            memcpy(target, joined, bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
        }
        CloseClipboard();
    }
    MemoryRelease(joined);
    state->copied = 1;
    SetTimer(state->window, TIMER_COPIED, 1500, NULL);
    Render(state, state->titleBottom - Scale(state, 45), state->titleBottom);
}

static void Answer(TriangleState *state, int choice)
{
    if (!state->answered)
    {
        state->answered = 1;
        PostMessageW(state->owner, WM_TRIANGLE_CHOICE, (WPARAM)(INT_PTR)choice, 0);
    }
    DestroyWindow(state->window);
}

static void Activate(TriangleState *state, int hit)
{
    if (state->kind == KIND_MESSAGE)
    {
        if (hit == 0)
            CopyAll(state);
        else if (hit == 1)
            DestroyWindow(state->window);
        return;
    }
    Answer(state, hit < state->choiceCount ? hit : -1);
}

static void SetHovered(TriangleState *state, int hovered)
{
    int previous = state->hovered;

    if (hovered == previous)
        return;
    state->hovered = hovered;
    if (state->kind == KIND_MESSAGE && (hovered == HIT_SCROLL || previous == HIT_SCROLL))
        Render(state, state->listTop - Scale(state, 3), state->listBottom + Scale(state, 3));
    if (state->kind == KIND_MESSAGE)
        Render(state, state->footerTop - Scale(state, 3), state->pyramid.baseY + Scale(state, 3));
    else
        Render(state, state->titleBottom - Scale(state, 3), state->pyramid.baseY + Scale(state, 3));
}

static void FreeState(TriangleState *state)
{
    int index;

    SurfaceDestroy(&state->surface);
    for (index = 0; index < state->choiceCount; index++)
        MemoryRelease(state->choices[index]);
    MemoryRelease(state->choices);
    MemoryRelease(state->edges);
    MemoryRelease(state->slotCapacity);
    MemoryRelease(state->titleLineOne);
    MemoryRelease(state->titleLineTwo);
    MemoryRelease(state);
}

static LRESULT CALLBACK TriangleProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    TriangleState *state = (TriangleState *)GetWindowLongPtrW(window, GWLP_USERDATA);

    if (message == WM_NCCREATE)
    {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lParam;

        state = create->lpCreateParams;
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state);
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (!state)
        return DefWindowProcW(window, message, wParam, lParam);
    switch (message)
    {
    case WM_MOUSEMOVE:
        if (state->thumbDragging)
        {
            DragThumb(state, (float)GET_Y_LPARAM(lParam));
            return 0;
        }
        if (state->dragging)
        {
            POINT cursor;

            GetCursorPos(&cursor);
            SetWindowPos(window, NULL, state->dragWindow.x + cursor.x - state->dragCursor.x, state->dragWindow.y + cursor.y - state->dragCursor.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (!state->tracking)
        {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};

            TrackMouseEvent(&track);
            state->tracking = 1;
        }
        SetHovered(state, HitTest(state, (float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam)));
        return 0;
    case WM_MOUSELEAVE:
        state->tracking = 0;
        SetHovered(state, HIT_NONE);
        return 0;
    case WM_LBUTTONDOWN:
        state->pressed = HitTest(state, (float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
        if (state->kind == KIND_MESSAGE && state->pressed == HIT_SCROLL)
        {
            PressScrollbar(state, (float)GET_Y_LPARAM(lParam));
            state->pressed = HIT_NONE;
        }
        return 0;
    case WM_LBUTTONUP:
    {
        int hit = HitTest(state, (float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));

        if (state->thumbDragging)
        {
            state->thumbDragging = 0;
            ReleaseCapture();
            RenderList(state);
            return 0;
        }
        if (hit != HIT_NONE && hit != HIT_SCROLL && hit == state->pressed)
            Activate(state, hit);
        state->pressed = HIT_NONE;
        return 0;
    }
    case WM_RBUTTONDOWN:
    {
        RECT rect;

        GetCursorPos(&state->dragCursor);
        GetWindowRect(window, &rect);
        state->dragWindow.x = rect.left;
        state->dragWindow.y = rect.top;
        state->dragging = 1;
        SetCapture(window);
        return 0;
    }
    case WM_RBUTTONUP:
        state->dragging = 0;
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        state->dragging = 0;
        if (state->thumbDragging)
        {
            state->thumbDragging = 0;
            RenderList(state);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (state->kind == KIND_MESSAGE)
            ScrollBy(state, -(long long)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 3);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            if (state->kind == KIND_CHOICE)
                Answer(state, -1);
            else
                DestroyWindow(window);
        }
        else if (state->kind == KIND_MESSAGE)
        {
            if (wParam == VK_UP)
                ScrollBy(state, -1);
            else if (wParam == VK_DOWN)
                ScrollBy(state, 1);
            else if (wParam == VK_PRIOR)
                ScrollBy(state, -Page(state));
            else if (wParam == VK_NEXT)
                ScrollBy(state, Page(state));
            else if (wParam == VK_HOME)
                ScrollTo(state, 0);
            else if (wParam == VK_END)
                ScrollTo(state, (long long)ErrorLogCount(state->log));
            else if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000))
                CopyAll(state);
        }
        return 0;
    case WM_CHAR:
        if (state->kind == KIND_CHOICE && wParam >= L'0' && (int)(wParam - L'0') < state->choiceCount)
            Answer(state, (int)(wParam - L'0'));
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_LOG && state->kind == KIND_MESSAGE)
        {
            LONG version = InterlockedCompareExchange(&state->log->version, 0, 0);

            if (version != state->seenVersion)
            {
                state->seenVersion = version;
                RenderList(state);
            }
        }
        else if (wParam == TIMER_COPIED)
        {
            KillTimer(window, TIMER_COPIED);
            state->copied = 0;
            Render(state, state->titleBottom - Scale(state, 45), state->titleBottom);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorW(NULL, state->hovered != HIT_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        KillTimer(window, TIMER_LOG);
        KillTimer(window, TIMER_COPIED);
        if (state->kind == KIND_CHOICE && !state->answered)
        {
            state->answered = 1;
            PostMessageW(state->owner, WM_TRIANGLE_CHOICE, (WPARAM)(INT_PTR)-1, 0);
        }
        if (window == messageWindow)
            messageWindow = NULL;
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        FreeState(state);
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int TriangleRegister(HINSTANCE instance)
{
    WNDCLASSEXW windowClass;

    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = TriangleProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.lpszClassName = TRIANGLE_CLASS;
    return RegisterClassExW(&windowClass) != 0;
}

static int WorkArea(HWND anchor, RECT *area)
{
    MONITORINFO monitor;

    memset(&monitor, 0, sizeof(monitor));
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &monitor))
        return 0;
    *area = monitor.rcWork;
    return 1;
}

static void PlaceWindow(HWND anchor, int width, int height, int *x, int *y)
{
    RECT rect;
    RECT area;

    GetWindowRect(anchor, &rect);
    *x = (rect.left + rect.right - width) / 2;
    *y = (rect.top + rect.bottom - height) / 2;
    if (!WorkArea(anchor, &area))
        return;
    if (*x + width > area.right)
        *x = area.right - width;
    if (*y + height > area.bottom)
        *y = area.bottom - height;
    if (*x < area.left)
        *x = area.left;
    if (*y < area.top)
        *y = area.top;
}

static HWND CreateTriangleWindow(TriangleState *state, HWND owner, HINSTANCE instance, int width, int height, const wchar_t *title)
{
    int x;
    int y;

    PlaceWindow(owner, width, height, &x, &y);
    state->owner = owner;
    state->hovered = HIT_NONE;
    state->pressed = HIT_NONE;
    if (!SurfaceCreate(&state->surface, width, height))
        return NULL;
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, TRIANGLE_CLASS, title, WS_POPUP, x, y, width, height, owner, NULL, instance, state);
}

static float MessageFactor(TriangleState *state, HWND anchor)
{
    const Theme *theme = state->theme;
    float middle = (MESSAGE_TITLE_BOTTOM + MESSAGE_FOOTER_TOP) / 2 - MESSAGE_APEX;
    float middleWidth = 2 * MESSAGE_BASE_HALF * middle / (MESSAGE_BASE - MESSAGE_APEX) * theme->scale;
    float factor;
    float limit = 1;
    size_t longest = 0;
    size_t line;
    RECT area;

    AcquireSRWLockShared(&state->log->lock);
    for (line = 0; line < state->log->lineCount; line++)
        if (state->log->lineLengths[line] > longest)
            longest = state->log->lineLengths[line];
    ReleaseSRWLockShared(&state->log->lock);
    factor = ((float)longest * theme->rowAdvance + theme->rowPadding + 1 + 2 * Scale(state, 34)) / middleWidth;
    if (WorkArea(anchor, &area))
    {
        float wide = 0.96f * (float)(area.right - area.left) / (MESSAGE_WIDTH * theme->scale);
        float tall = 0.96f * (float)(area.bottom - area.top) / (MESSAGE_HEIGHT * theme->scale);

        limit = wide < tall ? wide : tall;
        if (limit < 0.5f)
            limit = 0.5f;
    }
    if (factor < 1)
        factor = 1;
    if (factor > limit)
        factor = limit;
    return factor;
}

static void MessageGeometry(TriangleState *state, float factor)
{
    const Theme *theme = state->theme;
    float unit = theme->scale * factor;
    size_t slot;

    state->width = (int)(MESSAGE_WIDTH * unit);
    state->height = (int)(MESSAGE_HEIGHT * unit);
    state->pyramid.centerX = state->width / 2.0f;
    state->pyramid.apexY = MESSAGE_APEX * unit;
    state->pyramid.baseY = MESSAGE_BASE * unit;
    state->pyramid.baseHalf = MESSAGE_BASE_HALF * unit;
    state->titleBottom = MESSAGE_TITLE_BOTTOM * unit;
    state->footerTop = MESSAGE_FOOTER_TOP * unit;
    state->listTop = state->titleBottom + Scale(state, 6);
    state->listBottom = state->footerTop - Scale(state, 6);
    state->rowHeight = Scale(state, 19);
    state->slotCount = (size_t)((state->listBottom - state->listTop) / state->rowHeight);
    MemoryRelease(state->slotCapacity);
    state->slotCapacity = MemoryAllocateZero(state->slotCount ? state->slotCount : 1, sizeof(size_t));
    for (slot = 0; slot < state->slotCount; slot++)
    {
        float room = SlotWidth(state, slot) - theme->rowPadding - 1;

        state->slotCapacity[slot] = room >= theme->rowAdvance ? (size_t)(room / theme->rowAdvance) : 0;
    }
    state->cachedValid = 0;
}

static int MessageResize(TriangleState *state)
{
    float factor = MessageFactor(state, state->window);
    int x;
    int y;

    if ((int)(MESSAGE_WIDTH * state->theme->scale * factor) == state->width)
        return 1;
    SurfaceDestroy(&state->surface);
    MessageGeometry(state, factor);
    if (!SurfaceCreate(&state->surface, state->width, state->height))
    {
        DestroyWindow(state->window);
        return 0;
    }
    PlaceWindow(state->window, state->width, state->height, &x, &y);
    SetWindowPos(state->window, NULL, x, y, state->width, state->height, SWP_NOZORDER | SWP_NOACTIVATE);
    return 1;
}

HWND MessageTriangleShow(HWND owner, const Theme *theme, ErrorLog *log, HINSTANCE instance)
{
    TriangleState *state;

    if (messageWindow)
    {
        TriangleState *existing = (TriangleState *)GetWindowLongPtrW(messageWindow, GWLP_USERDATA);

        if (existing)
        {
            existing->followTail = 1;
            if (!MessageResize(existing))
                return NULL;
            RenderAll(existing);
        }
        SetForegroundWindow(messageWindow);
        return messageWindow;
    }
    state = MemoryAllocateZero(1, sizeof(TriangleState));
    state->kind = KIND_MESSAGE;
    state->theme = theme;
    state->log = log;
    state->followTail = 1;
    MessageGeometry(state, MessageFactor(state, owner));
    state->seenVersion = InterlockedCompareExchange(&log->version, 0, 0);
    messageWindow = CreateTriangleWindow(state, owner, instance, state->width, state->height, L"Sotek's Spite errors");
    if (!messageWindow)
    {
        if (!state->window)
            FreeState(state);
        return NULL;
    }
    RenderAll(state);
    ShowWindow(messageWindow, SW_SHOW);
    SetForegroundWindow(messageWindow);
    SetTimer(messageWindow, TIMER_LOG, 200, NULL);
    return messageWindow;
}

HWND ChoiceTriangleShow(HWND owner, const Theme *theme, HINSTANCE instance, const wchar_t *lineOne, const wchar_t *lineTwo, const wchar_t *const *choices, int count)
{
    TriangleState *state = MemoryAllocateZero(1, sizeof(TriangleState));
    float scale = theme->scale;
    int width = (int)(520 * scale);
    int height = (int)(470 * scale);
    float slab;
    int index;
    HWND window;

    state->kind = KIND_CHOICE;
    state->theme = theme;
    state->titleLineOne = WideDuplicate(lineOne);
    state->titleLineTwo = WideDuplicate(lineTwo);
    state->choiceCount = count;
    state->choices = MemoryAllocateZero((size_t)count, sizeof(wchar_t *));
    for (index = 0; index < count; index++)
        state->choices[index] = WideDuplicate(choices[index]);
    state->width = width;
    state->height = height;
    state->pyramid.centerX = width / 2.0f;
    state->pyramid.apexY = 16 * scale;
    state->pyramid.baseY = 452 * scale;
    state->pyramid.baseHalf = 250 * scale;
    state->titleBottom = 175 * scale;
    state->edges = MemoryAllocate((size_t)(count + 2) * sizeof(float));
    slab = (state->pyramid.baseY - state->titleBottom) / (float)(count + 1);
    for (index = 0; index <= count + 1; index++)
        state->edges[index] = state->titleBottom + slab * (float)index;
    window = CreateTriangleWindow(state, owner, instance, width, height, lineOne);
    if (!window)
    {
        if (!state->window)
            FreeState(state);
        return NULL;
    }
    RenderAll(state);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    return window;
}
