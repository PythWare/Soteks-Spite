#include "gui.h"
#include "triangle.h"
#include "operations.h"
#include <windowsx.h>
#include <shobjidl.h>

#define MAIN_CLASS L"SotekSpiteMain"
#define WM_OPERATION_DONE (WM_APP + 1)
#define TIMER_PROGRESS 1
#define TIMER_NOTICE 2
#define SLAB_COUNT 4
#define SLAB_UNPACK 0
#define SLAB_REBUILD 1
#define SLAB_BATCH 2
#define SLAB_EXIT 3
#define STATE_IDLE 0
#define STATE_RUNNING 1
#define STATE_DONE 2
#define HIT_NONE (-1)

static const wchar_t *const SlabLabels[SLAB_COUNT][3] = {
    {L"Unpack", L"Currently Unpacking", L"Unpacked"},
    {L"Rebuild", L"Currently Rebuilding", L"Rebuilt"},
    {L"Batch Update", L"Currently Updating", L"Updated"},
    {L"Exit", L"Stopping", L"Exit"}};

static const float BaseEdges[SLAB_COUNT + 1] = {205, 315, 425, 538, 650};

typedef struct OperationRequest
{
    int slab;
    wchar_t *first;
    wchar_t *second;
    int mode;
    OperationContext context;
    UnpackSummary unpack;
    RebuildSummary rebuild;
    BatchSummary batch;
    int succeeded;
    int batchRan;
    volatile LONG updating;
} OperationRequest;

typedef struct Application
{
    HINSTANCE instance;
    HWND window;
    Theme theme;
    Surface surface;
    Pyramid pyramid;
    float edges[SLAB_COUNT + 1];
    float titleBottom;
    int hovered;
    int pressed;
    int tracking;
    int dragging;
    POINT dragCursor;
    POINT dragWindow;
    int slabState[SLAB_COUNT];
    wchar_t *subline[SLAB_COUNT];
    wchar_t *notice[SLAB_COUNT];
    ULONGLONG noticeUntil[SLAB_COUNT];
    int busySlab;
    int exitRequested;
    HANDLE thread;
    OperationRequest *request;
    WorkerPool pool;
    int poolStarted;
    ErrorLog errors;
    Companions companions;
    int companionsLoaded;
    wchar_t *gameRoot;
    HWND choiceWindow;
    wchar_t *pendingSource;
} Application;

static Application app;

static float Scale(float value)
{
    return value * app.theme.scale;
}

static void FormatCount(LONG64 value, wchar_t *output, size_t capacity)
{
    wchar_t digits[32];
    size_t length;
    size_t index;
    size_t position = 0;

    swprintf(digits, 32, L"%lld", value < 0 ? -value : value);
    length = wcslen(digits);
    if (value < 0 && position + 1 < capacity)
        output[position++] = L'-';
    for (index = 0; index < length && position + 1 < capacity; index++)
    {
        output[position++] = digits[index];
        if ((length - index - 1) % 3 == 0 && index + 1 < length && position + 1 < capacity)
            output[position++] = L',';
    }
    output[position] = 0;
}

static GpSolidFill *SlabFill(int index)
{
    if (app.busySlab == index)
        return app.theme.lilacBusy;
    if (app.hovered == index && !app.choiceWindow && (app.busySlab < 0 || index == SLAB_EXIT))
        return app.theme.lilacHover;
    return index % 2 == 0 ? app.theme.lilac : app.theme.lilacDeep;
}

static const wchar_t *SublineFor(int index)
{
    if (app.notice[index] && GetTickCount64() < app.noticeUntil[index])
        return app.notice[index];
    return app.subline[index];
}

static void RenderMain(float top, float bottom)
{
    Surface *surface = &app.surface;
    Theme *theme = &app.theme;
    float center = app.pyramid.centerX;
    float titleY = app.pyramid.apexY + 0.68f * (app.titleBottom - app.pyramid.apexY);
    int index;

    SurfaceBeginBand(surface, top, bottom);
    DrawBand(surface, theme, &app.pyramid, app.pyramid.apexY, app.titleBottom, theme->lilacDeep, 0);
    DrawLabel(surface, theme->titleFont, theme->text, theme->centered, L"Sotek's", center, titleY - Scale(13), Scale(200), Scale(28));
    DrawLabel(surface, theme->titleFont, theme->text, theme->centered, L"Spite", center, titleY + Scale(13), Scale(200), Scale(28));
    for (index = 0; index < SLAB_COUNT; index++)
    {
        float slabTop = app.edges[index];
        float slabBottom = app.edges[index + 1];
        float middle = (slabTop + slabBottom) / 2;
        const wchar_t *subline = SublineFor(index);
        const wchar_t *label = index == SLAB_EXIT && app.exitRequested ? SlabLabels[SLAB_EXIT][1] : SlabLabels[index][app.slabState[index]];
        float labelY = subline ? middle - Scale(9) : middle;
        float sublineY = middle + Scale(15);

        DrawBand(surface, theme, &app.pyramid, slabTop, slabBottom, SlabFill(index), 1);
        DrawLabel(surface, theme->labelFont, theme->text, theme->centered, label, center, labelY, 2 * PyramidHalf(&app.pyramid, labelY - Scale(13)) - Scale(24), Scale(26));
        if (subline)
            DrawLabel(surface, theme->statusFont, theme->textDim, theme->centered, subline, center, sublineY, 2 * PyramidHalf(&app.pyramid, sublineY - Scale(10)) - Scale(24), Scale(20));
    }
    SurfaceEndBand(surface);
    SurfacePresent(surface, app.window, top, bottom);
}

static void RenderAll(void)
{
    RenderMain(0, (float)app.surface.height);
}

static void RenderSlab(int index)
{
    RenderMain(app.edges[index] - Scale(3), app.edges[index + 1] + Scale(3));
}

static void SetSubline(int index, wchar_t *owned)
{
    MemoryRelease(app.subline[index]);
    app.subline[index] = owned;
}

static void ShowNotice(int index, const wchar_t *text)
{
    MemoryRelease(app.notice[index]);
    app.notice[index] = WideDuplicate(text);
    app.noticeUntil[index] = GetTickCount64() + 2200;
    SetTimer(app.window, TIMER_NOTICE, 250, NULL);
    RenderSlab(index);
}

static int HitSlab(float x, float y)
{
    int index;

    for (index = 0; index < SLAB_COUNT; index++)
        if (PyramidContains(&app.pyramid, x, y, app.edges[index], app.edges[index + 1]))
            return index;
    return HIT_NONE;
}

static wchar_t *PickFolder(const wchar_t *title)
{
    IFileOpenDialog *dialog = NULL;
    IShellItem *item = NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    PWSTR path = NULL;
    wchar_t *result = NULL;

    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog)))
        return NULL;
    dialog->lpVtbl->GetOptions(dialog, &options);
    dialog->lpVtbl->SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->lpVtbl->SetTitle(dialog, title);
    if (SUCCEEDED(dialog->lpVtbl->Show(dialog, app.window)) && SUCCEEDED(dialog->lpVtbl->GetResult(dialog, &item)))
    {
        if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path)))
        {
            result = PathAbsolute(path);
            CoTaskMemFree(path);
        }
        item->lpVtbl->Release(item);
    }
    dialog->lpVtbl->Release(dialog);
    return result;
}

static int EnsureCompanions(void)
{
    wchar_t *path;

    if (app.companionsLoaded)
        return 1;
    path = PathJoin(app.gameRoot, L"vanilla_companions.txt");
    if (!PathIsFile(path))
        ErrorLogAdd(&app.errors, L"vanilla_companions.txt wasnt found next to Sotek's Spite (%ls). Place the exe in the game folder together with vanilla_companions.txt.", PathDisplay(app.gameRoot));
    else
        app.companionsLoaded = CompanionsLoad(path, &app.companions, &app.errors);
    MemoryRelease(path);
    return app.companionsLoaded;
}

static DWORD WINAPI OperationThread(void *parameter)
{
    OperationRequest *request = parameter;

    if (request->slab == SLAB_UNPACK)
        request->succeeded = UnpackFolder(&request->context, request->first, request->second, request->mode, &request->unpack);
    else if (request->slab == SLAB_REBUILD)
    {
        request->succeeded = RebuildFolder(&request->context, request->first, request->second, &request->rebuild);
        if (request->succeeded && app.companionsLoaded)
        {
            InterlockedExchange(&request->updating, 1);
            request->batchRan = 1;
            request->succeeded = BatchUpdate(&request->context, request->second, &app.companions, &request->batch);
        }
    }
    else
    {
        request->batchRan = 1;
        request->succeeded = BatchUpdate(&request->context, request->first, &app.companions, &request->batch);
    }
    PostMessageW(app.window, WM_OPERATION_DONE, 0, (LPARAM)request);
    return 0;
}

static void FreeRequest(OperationRequest *request)
{
    MemoryRelease(request->first);
    MemoryRelease(request->second);
    MemoryRelease(request);
}

static void StartOperation(int slab, wchar_t *first, wchar_t *second, int mode)
{
    OperationRequest *request = MemoryAllocateZero(1, sizeof(OperationRequest));

    request->slab = slab;
    request->first = first;
    request->second = second;
    request->mode = mode;
    request->context.pool = &app.pool;
    request->context.errors = &app.errors;
    request->context.gameRoot = app.gameRoot;
    app.request = request;
    app.busySlab = slab;
    app.slabState[slab] = STATE_RUNNING;
    SetSubline(slab, WideDuplicate(L"Starting"));
    app.thread = CreateThread(NULL, 0, OperationThread, request, 0, NULL);
    if (!app.thread)
    {
        ErrorLogAdd(&app.errors, L"Couldnt start a worker thread.");
        app.request = NULL;
        app.busySlab = -1;
        app.slabState[slab] = STATE_IDLE;
        SetSubline(slab, NULL);
        FreeRequest(request);
        MessageTriangleShow(app.window, &app.theme, &app.errors, app.instance);
        RenderAll();
        return;
    }
    SetTimer(app.window, TIMER_PROGRESS, 100, NULL);
    RenderAll();
}

static wchar_t *ProgressText(OperationRequest *request)
{
    OperationProgress *progress = &request->context.progress;
    LONG stage = InterlockedCompareExchange(&progress->stage, 0, 0);
    wchar_t done[32];
    wchar_t total[32];
    wchar_t files[32];

    FormatCount(InterlockedCompareExchange64(&progress->done, 0, 0), done, 32);
    FormatCount(InterlockedCompareExchange64(&progress->total, 0, 0), total, 32);
    FormatCount(InterlockedCompareExchange64(&progress->files, 0, 0), files, 32);
    switch (stage)
    {
    case STAGE_SCANNING:
        return WideDuplicate(L"Scanning files");
    case STAGE_UNPACKING:
        return WideFormat(L"Archives %ls of %ls, %ls files", done, total, files);
    case STAGE_REBUILDING_NESTED:
        return WideFormat(L"Nested archives %ls of %ls", done, total);
    case STAGE_REBUILDING:
        return WideFormat(L"Archives %ls of %ls", done, total);
    case STAGE_UPDATING:
        return InterlockedCompareExchange(&request->updating, 0, 0) ? WideFormat(L"Updating companions %ls of %ls", done, total) : WideFormat(L"Containers %ls of %ls", done, total);
    }
    return WideDuplicate(L"Working");
}

static void UpdateProgress(void)
{
    wchar_t *text;

    if (!app.request || app.busySlab < 0)
        return;
    text = ProgressText(app.request);
    if (app.subline[app.busySlab] && wcscmp(app.subline[app.busySlab], text) == 0)
    {
        MemoryRelease(text);
        return;
    }
    SetSubline(app.busySlab, text);
    RenderSlab(app.busySlab);
}

static wchar_t *SummaryText(OperationRequest *request)
{
    wchar_t first[32];
    wchar_t second[32];
    wchar_t third[32];

    if (request->slab == SLAB_UNPACK)
    {
        FormatCount(request->unpack.archives, first, 32);
        FormatCount(request->unpack.files, second, 32);
        return WideFormat(L"%ls archives, %ls files, %.1f s", first, second, request->unpack.seconds);
    }
    if (request->slab == SLAB_REBUILD)
    {
        FormatCount(request->rebuild.archivesWritten, first, 32);
        FormatCount(request->rebuild.nestedRebuilt, second, 32);
        FormatCount(request->batch.copiesUpdated, third, 32);
        if (request->batchRan)
            return WideFormat(L"%ls archives, %ls nested, %ls copies synced", first, second, third);
        return WideFormat(L"%ls archives, %ls nested, %.1f s", first, second, request->rebuild.seconds);
    }
    if (!request->batch.copiesUpdated)
    {
        FormatCount(request->batch.gameArchives, first, 32);
        return WideFormat(L"All companions current, %ls archives checked", first);
    }
    FormatCount(request->batch.copiesUpdated, first, 32);
    FormatCount(request->batch.containersWritten, second, 32);
    return WideFormat(L"%ls copies updated in %ls containers", first, second);
}

static void FinishOperation(OperationRequest *request)
{
    int slab = request->slab;
    size_t errors = ErrorLogCount(&app.errors);
    int cancelled = OperationCancelled(&request->context);

    WaitForSingleObject(app.thread, INFINITE);
    CloseHandle(app.thread);
    app.thread = NULL;
    KillTimer(app.window, TIMER_PROGRESS);
    app.request = NULL;
    app.busySlab = -1;
    if (cancelled)
    {
        app.slabState[slab] = STATE_IDLE;
        SetSubline(slab, WideDuplicate(L"Stopped"));
    }
    else if (request->succeeded)
    {
        wchar_t *summary = SummaryText(request);

        app.slabState[slab] = STATE_DONE;
        if (errors)
        {
            SetSubline(slab, WideFormat(L"%ls, %zu errors", summary, errors));
            MemoryRelease(summary);
        }
        else
            SetSubline(slab, summary);
    }
    else
    {
        app.slabState[slab] = STATE_IDLE;
        SetSubline(slab, WideFormat(L"Failed with %zu errors", errors));
    }
    FreeRequest(request);
    if (app.exitRequested)
    {
        DestroyWindow(app.window);
        return;
    }
    RenderAll();
    if (errors)
        MessageTriangleShow(app.window, &app.theme, &app.errors, app.instance);
}

static void BeginOperation(int slab)
{
    if (slab == SLAB_UNPACK)
    {
        static const wchar_t *const choices[] = {L"0  Top level", L"1  One nested level", L"2  Full unpack"};
        wchar_t *source = PickFolder(L"Select a folder containing BIG archives");

        if (!source)
            return;
        MemoryRelease(app.pendingSource);
        app.pendingSource = source;
        app.choiceWindow = ChoiceTriangleShow(app.window, &app.theme, app.instance, L"Unpack", L"mode", choices, 3);
        if (!app.choiceWindow)
        {
            MemoryRelease(app.pendingSource);
            app.pendingSource = NULL;
        }
        RenderAll();
        return;
    }
    ErrorLogClear(&app.errors);
    if (slab == SLAB_REBUILD)
    {
        wchar_t *input = PickFolder(L"Select the unpacked folder to rebuild");
        wchar_t *output;

        if (!input)
            return;
        output = PickFolder(L"Select the output folder, i.e. your Mods folder");
        if (!output)
        {
            MemoryRelease(input);
            return;
        }
        EnsureCompanions();
        StartOperation(SLAB_REBUILD, input, output, 0);
        return;
    }
    if (slab == SLAB_BATCH)
    {
        wchar_t *folder = PickFolder(L"Select your mods folder");

        if (!folder)
            return;
        if (!EnsureCompanions())
        {
            MemoryRelease(folder);
            MessageTriangleShow(app.window, &app.theme, &app.errors, app.instance);
            return;
        }
        StartOperation(SLAB_BATCH, folder, NULL, 0);
    }
}

static wchar_t *UnpackOutputFor(const wchar_t *source)
{
    const wchar_t *cursor;
    const wchar_t *gameTree = NULL;
    wchar_t *parent;
    wchar_t *unpacked;
    wchar_t *output;

    for (cursor = source; *cursor; cursor++)
    {
        const wchar_t *component;
        const wchar_t *end;
        size_t length;

        if (*cursor != L'\\')
            continue;
        component = cursor + 1;
        end = wcschr(component, L'\\');
        length = end ? (size_t)(end - component) : wcslen(component);
        if ((length == 4 && CompareStringOrdinal(component, 4, L"data", 4, TRUE) == CSTR_EQUAL) ||
            (length == 3 && CompareStringOrdinal(component, 3, L"dlc", 3, TRUE) == CSTR_EQUAL))
        {
            gameTree = component;
            break;
        }
    }
    if (gameTree)
    {
        wchar_t *base = WideDuplicateLength(source, (size_t)(gameTree - source - 1));

        unpacked = PathJoin(base, L"Unpacked");
        output = PathJoin(unpacked, gameTree);
        MemoryRelease(base);
        MemoryRelease(unpacked);
        return output;
    }
    parent = PathParent(source);
    unpacked = PathJoin(parent, L"Unpacked");
    output = PathJoin(unpacked, PathName(source));
    MemoryRelease(parent);
    MemoryRelease(unpacked);
    return output;
}

static void ChooseUnpackMode(int choice)
{
    wchar_t *source = app.pendingSource;

    app.pendingSource = NULL;
    app.choiceWindow = NULL;
    if (choice < 0 || !source)
    {
        MemoryRelease(source);
        RenderAll();
        return;
    }
    ErrorLogClear(&app.errors);
    StartOperation(SLAB_UNPACK, source, UnpackOutputFor(source), choice);
}

static void RequestExit(void)
{
    if (app.busySlab < 0)
    {
        DestroyWindow(app.window);
        return;
    }
    if (app.exitRequested)
        return;
    app.exitRequested = 1;
    if (app.request)
        InterlockedExchange(&app.request->context.cancelled, 1);
    RenderSlab(SLAB_EXIT);
}

static void ActivateSlab(int slab)
{
    if (slab == SLAB_EXIT)
    {
        RequestExit();
        return;
    }
    if (app.choiceWindow)
    {
        SetForegroundWindow(app.choiceWindow);
        return;
    }
    if (app.busySlab >= 0)
    {
        if (slab != app.busySlab)
            ShowNotice(slab, L"Busy, wait for the current job");
        return;
    }
    BeginOperation(slab);
}

static void SetHovered(int hovered)
{
    int previous = app.hovered;

    if (hovered == previous)
        return;
    app.hovered = hovered;
    if (previous != HIT_NONE)
        RenderSlab(previous);
    if (hovered != HIT_NONE)
        RenderSlab(hovered);
}

static void ExpireNotices(void)
{
    ULONGLONG now = GetTickCount64();
    int pending = 0;
    int index;

    for (index = 0; index < SLAB_COUNT; index++)
    {
        if (!app.notice[index])
            continue;
        if (now >= app.noticeUntil[index])
        {
            MemoryRelease(app.notice[index]);
            app.notice[index] = NULL;
            RenderSlab(index);
        }
        else
            pending = 1;
    }
    if (!pending)
        KillTimer(app.window, TIMER_NOTICE);
}

static LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_MOUSEMOVE:
        if (app.dragging)
        {
            POINT cursor;

            GetCursorPos(&cursor);
            SetWindowPos(window, NULL, app.dragWindow.x + cursor.x - app.dragCursor.x, app.dragWindow.y + cursor.y - app.dragCursor.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (!app.tracking)
        {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};

            TrackMouseEvent(&track);
            app.tracking = 1;
        }
        SetHovered(HitSlab((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam)));
        return 0;
    case WM_MOUSELEAVE:
        app.tracking = 0;
        SetHovered(HIT_NONE);
        return 0;
    case WM_LBUTTONDOWN:
        app.pressed = HitSlab((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
    {
        int hit = HitSlab((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));

        if (hit != HIT_NONE && hit == app.pressed)
            ActivateSlab(hit);
        app.pressed = HIT_NONE;
        return 0;
    }
    case WM_RBUTTONDOWN:
    {
        RECT rect;

        GetCursorPos(&app.dragCursor);
        GetWindowRect(window, &rect);
        app.dragWindow.x = rect.left;
        app.dragWindow.y = rect.top;
        app.dragging = 1;
        SetCapture(window);
        return 0;
    }
    case WM_RBUTTONUP:
        app.dragging = 0;
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        app.dragging = 0;
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            RequestExit();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorW(NULL, app.hovered != HIT_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (wParam == TIMER_PROGRESS)
            UpdateProgress();
        else if (wParam == TIMER_NOTICE)
            ExpireNotices();
        return 0;
    case WM_OPERATION_DONE:
        FinishOperation((OperationRequest *)lParam);
        return 0;
    case WM_TRIANGLE_CHOICE:
        ChooseUnpackMode((int)(INT_PTR)wParam);
        return 0;
    case WM_CLOSE:
        RequestExit();
        return 0;
    case WM_DESTROY:
        KillTimer(window, TIMER_PROGRESS);
        KillTimer(window, TIMER_NOTICE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static void ReleaseApplication(void)
{
    int index;

    if (app.poolStarted)
        PoolStop(&app.pool);
    if (app.companionsLoaded)
        CompanionsFree(&app.companions);
    ErrorLogFree(&app.errors);
    SurfaceDestroy(&app.surface);
    ThemeDestroy(&app.theme);
    for (index = 0; index < SLAB_COUNT; index++)
    {
        MemoryRelease(app.subline[index]);
        MemoryRelease(app.notice[index]);
    }
    MemoryRelease(app.pendingSource);
    MemoryRelease(app.gameRoot);
    memset(&app, 0, sizeof(app));
}

int RunGui(HINSTANCE instance, int show)
{
    GdiplusStartupInput input = {1, NULL, FALSE, FALSE};
    ULONG_PTR token = 0;
    WNDCLASSEXW windowClass;
    RECT work;
    MSG message;
    float scale;
    int width;
    int height;
    int index;
    HRESULT com;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (GdiplusStartup(&token, &input, NULL) != Ok)
    {
        MessageBoxW(NULL, L"GDI+ couldnt be started.", L"Sotek's Spite", MB_ICONERROR | MB_OK);
        if (SUCCEEDED(com))
            CoUninitialize();
        return 1;
    }
    memset(&app, 0, sizeof(app));
    app.instance = instance;
    app.hovered = HIT_NONE;
    app.pressed = HIT_NONE;
    app.busySlab = -1;
    scale = (float)GetDpiForSystem() / 96.0f;
    ErrorLogInitialize(&app.errors);
    app.gameRoot = ExecutableDirectory();
    app.poolStarted = PoolStart(&app.pool, DefaultWorkerCount());
    if (!ThemeCreate(&app.theme, scale) || !app.poolStarted)
    {
        MessageBoxW(NULL, L"Sotek's Spite couldnt set up its drawing resources or worker threads.", L"Sotek's Spite", MB_ICONERROR | MB_OK);
        ReleaseApplication();
        GdiplusShutdown(token);
        if (SUCCEEDED(com))
            CoUninitialize();
        return 1;
    }
    width = (int)(900 * scale);
    height = (int)(690 * scale);
    app.pyramid.centerX = width / 2.0f;
    app.pyramid.apexY = 30 * scale;
    app.pyramid.baseY = 650 * scale;
    app.pyramid.baseHalf = 425 * scale;
    app.titleBottom = BaseEdges[0] * scale;
    for (index = 0; index <= SLAB_COUNT; index++)
        app.edges[index] = BaseEdges[index] * scale;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = MainProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    windowClass.lpszClassName = MAIN_CLASS;
    RegisterClassExW(&windowClass);
    TriangleRegister(instance);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (!SurfaceCreate(&app.surface, width, height) ||
        !(app.window = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, MAIN_CLASS, L"Sotek's Spite", WS_POPUP,
                                       (work.left + work.right - width) / 2, (work.top + work.bottom - height) / 2, width, height, NULL, NULL, instance, NULL)))
    {
        MessageBoxW(NULL, L"The Sotek's Spite window couldnt be created.", L"Sotek's Spite", MB_ICONERROR | MB_OK);
        ReleaseApplication();
        GdiplusShutdown(token);
        if (SUCCEEDED(com))
            CoUninitialize();
        return 1;
    }
    RenderAll();
    ShowWindow(app.window, show == SW_HIDE ? SW_SHOW : show);
    SetForegroundWindow(app.window);
    while (GetMessageW(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (app.thread)
    {
        WaitForSingleObject(app.thread, INFINITE);
        CloseHandle(app.thread);
    }
    ReleaseApplication();
    GdiplusShutdown(token);
    if (SUCCEEDED(com))
        CoUninitialize();
    return 0;
}
