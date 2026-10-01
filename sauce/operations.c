#include "operations.h"

double SecondsNow(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

int OperationCancelled(OperationContext *context)
{
    return InterlockedCompareExchange(&context->cancelled, 0, 0) != 0;
}

void ProgressReset(OperationContext *context, LONG stage)
{
    InterlockedExchange64(&context->progress.done, 0);
    InterlockedExchange64(&context->progress.total, 0);
    InterlockedExchange64(&context->progress.files, 0);
    InterlockedExchange(&context->progress.stage, stage);
}

static void WideSetGrow(WideSet *set)
{
    size_t capacity = set->capacity ? set->capacity * 2 : 64;
    wchar_t **items = MemoryAllocateZero(capacity, sizeof(wchar_t *));
    uint64_t *hashes = MemoryAllocateZero(capacity, sizeof(uint64_t));
    size_t index;

    for (index = 0; index < set->capacity; index++)
    {
        size_t slot;

        if (!set->items[index])
            continue;
        for (slot = (size_t)set->hashes[index] & (capacity - 1); items[slot]; slot = (slot + 1) & (capacity - 1))
            ;
        items[slot] = set->items[index];
        hashes[slot] = set->hashes[index];
    }
    MemoryRelease(set->items);
    MemoryRelease(set->hashes);
    set->items = items;
    set->hashes = hashes;
    set->capacity = capacity;
}

void WideSetInsert(WideSet *set, const wchar_t *text)
{
    uint64_t hash;
    size_t slot;

    if (WideSetContains(set, text))
        return;
    if ((set->count + 1) * 2 > set->capacity)
        WideSetGrow(set);
    hash = WideHashInsensitive(text, wcslen(text));
    for (slot = (size_t)hash & (set->capacity - 1); set->items[slot]; slot = (slot + 1) & (set->capacity - 1))
        ;
    set->items[slot] = WideDuplicate(text);
    set->hashes[slot] = hash;
    set->count++;
}

int WideSetContains(const WideSet *set, const wchar_t *text)
{
    uint64_t hash;
    size_t slot;

    if (!set->capacity)
        return 0;
    hash = WideHashInsensitive(text, wcslen(text));
    for (slot = (size_t)hash & (set->capacity - 1); set->items[slot]; slot = (slot + 1) & (set->capacity - 1))
        if (set->hashes[slot] == hash && WideEqualInsensitive(set->items[slot], text))
            return 1;
    return 0;
}

void WideSetFree(WideSet *set)
{
    size_t index;

    for (index = 0; index < set->capacity; index++)
        MemoryRelease(set->items[index]);
    MemoryRelease(set->items);
    MemoryRelease(set->hashes);
    memset(set, 0, sizeof(*set));
}

static int ComponentIs(const wchar_t *component, size_t length, const wchar_t *expected)
{
    size_t expectedLength = wcslen(expected);

    return length == expectedLength && CompareStringOrdinal(component, (int)length, expected, (int)length, TRUE) == CSTR_EQUAL;
}

wchar_t *GameRelativePath(const wchar_t *absolutePath, const wchar_t *gameRoot)
{
    const wchar_t *cursor;

    for (cursor = absolutePath; *cursor; cursor++)
    {
        const wchar_t *component;
        const wchar_t *end;
        wchar_t *candidate;

        if (*cursor != L'\\')
            continue;
        component = cursor + 1;
        end = wcschr(component, L'\\');
        if (!end)
            break;
        if (!ComponentIs(component, (size_t)(end - component), L"data") && !ComponentIs(component, (size_t)(end - component), L"dlc"))
            continue;
        candidate = PathJoin(gameRoot, component);
        if (PathIsFile(candidate))
        {
            MemoryRelease(candidate);
            return WideDuplicate(component);
        }
        MemoryRelease(candidate);
    }
    return NULL;
}
