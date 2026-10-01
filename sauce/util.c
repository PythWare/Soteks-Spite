#include "common.h"

static volatile LONG64 outstandingAllocations;

static void FatalOutOfMemory(void)
{
    MessageBoxW(NULL, L"Sotek's Spite ran out of memory and has to close.", L"Sotek's Spite", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

void *MemoryAllocate(size_t size)
{
    void *block = malloc(size ? size : 1);

    if (!block)
        FatalOutOfMemory();
    InterlockedIncrement64(&outstandingAllocations);
    return block;
}

void *MemoryAllocateZero(size_t count, size_t size)
{
    void *block = calloc(count ? count : 1, size ? size : 1);

    if (!block)
        FatalOutOfMemory();
    InterlockedIncrement64(&outstandingAllocations);
    return block;
}

void *MemoryResize(void *block, size_t size)
{
    void *resized;

    if (!block)
        return MemoryAllocate(size);
    resized = realloc(block, size ? size : 1);
    if (!resized)
        FatalOutOfMemory();
    return resized;
}

void MemoryRelease(void *block)
{
    if (!block)
        return;
    free(block);
    InterlockedDecrement64(&outstandingAllocations);
}

LONG64 MemoryOutstanding(void)
{
    return InterlockedCompareExchange64(&outstandingAllocations, 0, 0);
}

void ByteBufferReserve(ByteBuffer *buffer, size_t capacity)
{
    size_t grown;

    if (capacity <= buffer->capacity)
        return;
    grown = buffer->capacity ? buffer->capacity : 64;
    while (grown < capacity)
        grown = grown > ((size_t)-1) / 2 ? capacity : grown * 2;
    buffer->data = MemoryResize(buffer->data, grown);
    buffer->capacity = grown;
}

void ByteBufferAppend(ByteBuffer *buffer, const void *data, size_t size)
{
    if (!size)
        return;
    ByteBufferReserve(buffer, buffer->size + size);
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
}

void ByteBufferAppendByte(ByteBuffer *buffer, unsigned char value)
{
    ByteBufferReserve(buffer, buffer->size + 1);
    buffer->data[buffer->size++] = value;
}

void ByteBufferAppendZeros(ByteBuffer *buffer, size_t count)
{
    if (!count)
        return;
    ByteBufferReserve(buffer, buffer->size + count);
    memset(buffer->data + buffer->size, 0, count);
    buffer->size += count;
}

void ByteBufferAppendText(ByteBuffer *buffer, const char *text)
{
    ByteBufferAppend(buffer, text, strlen(text));
}

void ByteBufferFree(ByteBuffer *buffer)
{
    MemoryRelease(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

wchar_t *WideDuplicateLength(const wchar_t *text, size_t length)
{
    wchar_t *copy = MemoryAllocate((length + 1) * sizeof(wchar_t));

    memcpy(copy, text, length * sizeof(wchar_t));
    copy[length] = 0;
    return copy;
}

wchar_t *WideDuplicate(const wchar_t *text)
{
    return WideDuplicateLength(text, wcslen(text));
}

wchar_t *WideFormatList(const wchar_t *format, va_list arguments)
{
    size_t capacity = 256;

    for (;;)
    {
        wchar_t *text = MemoryAllocate(capacity * sizeof(wchar_t));
        va_list copy;
        int written;

        va_copy(copy, arguments);
        written = vswprintf(text, capacity, format, copy);
        va_end(copy);
        if (written >= 0 && (size_t)written < capacity)
            return text;
        MemoryRelease(text);
        if (capacity > 16 * 1024 * 1024)
            return WideDuplicate(L"(message too long)");
        capacity *= 4;
    }
}

wchar_t *WideFormat(const wchar_t *format, ...)
{
    va_list arguments;
    wchar_t *text;

    va_start(arguments, format);
    text = WideFormatList(format, arguments);
    va_end(arguments);
    return text;
}

wchar_t WideLower(wchar_t character)
{
    if (character >= L'A' && character <= L'Z')
        return (wchar_t)(character + 32);
    if (character < 0xC0)
        return character;
    if (character <= 0xDE)
        return character == 0xD7 ? character : (wchar_t)(character + 32);
    return (wchar_t)towlower(character);
}

int WideEqualInsensitive(const wchar_t *left, const wchar_t *right)
{
    return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

int WideEndsWithInsensitive(const wchar_t *text, const wchar_t *suffix)
{
    size_t textLength = wcslen(text);
    size_t suffixLength = wcslen(suffix);

    if (suffixLength > textLength)
        return 0;
    return CompareStringOrdinal(text + textLength - suffixLength, (int)suffixLength, suffix, (int)suffixLength, TRUE) == CSTR_EQUAL;
}

uint64_t WideHashInsensitive(const wchar_t *text, size_t length)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    size_t index;

    for (index = 0; index < length; index++)
    {
        hash ^= (uint64_t)WideLower(text[index]);
        hash *= 0x100000001B3ull;
    }
    return hash ? hash : 1;
}

void WideListAppend(WideList *list, wchar_t *owned)
{
    if (list->count == list->capacity)
    {
        list->capacity = list->capacity ? list->capacity * 2 : 64;
        list->items = MemoryResize(list->items, list->capacity * sizeof(wchar_t *));
    }
    list->items[list->count++] = owned;
}

void WideListFree(WideList *list)
{
    size_t index;

    for (index = 0; index < list->count; index++)
        MemoryRelease(list->items[index]);
    MemoryRelease(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

wchar_t *Latin1ToWide(const unsigned char *text, size_t length)
{
    wchar_t *wide = MemoryAllocate((length + 1) * sizeof(wchar_t));
    size_t index;

    for (index = 0; index < length; index++)
        wide[index] = text[index];
    wide[length] = 0;
    return wide;
}

int WideToLatin1(const wchar_t *text, ByteBuffer *output)
{
    int exact = 1;

    for (; *text; text++)
    {
        if (*text > 0xFF)
        {
            exact = 0;
            ByteBufferAppendByte(output, '?');
        }
        else
            ByteBufferAppendByte(output, (unsigned char)*text);
    }
    return exact;
}

unsigned char Latin1Lower(unsigned char character)
{
    if (character >= 'A' && character <= 'Z')
        return (unsigned char)(character + 32);
    if (character >= 0xC0 && character <= 0xDE && character != 0xD7)
        return (unsigned char)(character + 32);
    return character;
}

wchar_t *PathJoin(const wchar_t *left, const wchar_t *right)
{
    size_t leftLength = wcslen(left);
    size_t rightLength = wcslen(right);
    int separator = leftLength && left[leftLength - 1] != L'\\';
    wchar_t *joined = MemoryAllocate((leftLength + rightLength + 2) * sizeof(wchar_t));

    memcpy(joined, left, leftLength * sizeof(wchar_t));
    if (separator)
        joined[leftLength++] = L'\\';
    memcpy(joined + leftLength, right, (rightLength + 1) * sizeof(wchar_t));
    return joined;
}

wchar_t *PathParent(const wchar_t *path)
{
    const wchar_t *separator = wcsrchr(path, L'\\');

    if (!separator)
        return WideDuplicate(L"");
    return WideDuplicateLength(path, (size_t)(separator - path));
}

const wchar_t *PathName(const wchar_t *path)
{
    const wchar_t *separator = wcsrchr(path, L'\\');

    return separator ? separator + 1 : path;
}

wchar_t *PathAbsolute(const wchar_t *path)
{
    DWORD needed;
    wchar_t *full;
    wchar_t *result;
    size_t length;

    if (wcsncmp(path, L"\\\\?\\", 4) == 0)
        return WideDuplicate(path);
    needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (!needed)
        return WideDuplicate(path);
    full = MemoryAllocate((needed + 1) * sizeof(wchar_t));
    GetFullPathNameW(path, needed + 1, full, NULL);
    length = wcslen(full);
    while (length > 3 && full[length - 1] == L'\\')
        full[--length] = 0;
    if (full[0] == L'\\' && full[1] == L'\\')
        result = WideFormat(L"\\\\?\\UNC\\%ls", full + 2);
    else
        result = WideFormat(L"\\\\?\\%ls", full);
    MemoryRelease(full);
    return result;
}

const wchar_t *PathDisplay(const wchar_t *path)
{
    return wcsncmp(path, L"\\\\?\\", 4) == 0 ? path + 4 : path;
}

int PathIsFile(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);

    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int PathIsDirectory(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);

    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int CreateDirectoryTree(const wchar_t *path)
{
    wchar_t *copy;
    size_t index;
    size_t start = 0;

    if (PathIsDirectory(path))
        return 1;
    copy = WideDuplicate(path);
    if (wcsncmp(copy, L"\\\\?\\UNC\\", 8) == 0)
        start = 8;
    else if (wcsncmp(copy, L"\\\\?\\", 4) == 0)
        start = 4;
    for (index = start + 1; copy[index]; index++)
    {
        if (copy[index] != L'\\')
            continue;
        if (index > start && copy[index - 1] == L':')
            continue;
        copy[index] = 0;
        CreateDirectoryW(copy, NULL);
        copy[index] = L'\\';
    }
    CreateDirectoryW(copy, NULL);
    MemoryRelease(copy);
    return PathIsDirectory(path);
}

int CreateParentDirectories(const wchar_t *path)
{
    wchar_t *parent = PathParent(path);
    int created = parent[0] ? CreateDirectoryTree(parent) : 1;

    MemoryRelease(parent);
    return created;
}

wchar_t *ExecutableDirectory(void)
{
    DWORD capacity = MAX_PATH;

    for (;;)
    {
        wchar_t *path = MemoryAllocate(capacity * sizeof(wchar_t));
        DWORD length = GetModuleFileNameW(NULL, path, capacity);

        if (length && length < capacity)
        {
            wchar_t *parent = PathParent(path);
            wchar_t *absolute = PathAbsolute(parent);

            MemoryRelease(parent);
            MemoryRelease(path);
            return absolute;
        }
        MemoryRelease(path);
        if (!length || capacity > 32768)
            return PathAbsolute(L".");
        capacity *= 2;
    }
}

static HANDLE OpenForRead(const wchar_t *path)
{
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
}

int ReadFilePrefix(const wchar_t *path, size_t count, ByteBuffer *output)
{
    HANDLE file = OpenForRead(path);
    size_t total = 0;

    output->size = 0;
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    ByteBufferReserve(output, count);
    while (total < count)
    {
        DWORD chunk = (DWORD)((count - total) > (1u << 30) ? (1u << 30) : (count - total));
        DWORD got = 0;

        if (!ReadFile(file, output->data + total, chunk, &got, NULL))
        {
            CloseHandle(file);
            return 0;
        }
        if (!got)
            break;
        total += got;
    }
    output->size = total;
    CloseHandle(file);
    return 1;
}

int ReadWholeFile(const wchar_t *path, ByteBuffer *output)
{
    HANDLE file = OpenForRead(path);
    LARGE_INTEGER size;
    size_t total = 0;

    output->size = 0;
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    if (!GetFileSizeEx(file, &size) || (uint64_t)size.QuadPart > (uint64_t)SIZE_MAX)
    {
        CloseHandle(file);
        return 0;
    }
    ByteBufferReserve(output, (size_t)size.QuadPart);
    while (total < (size_t)size.QuadPart)
    {
        size_t remaining = (size_t)size.QuadPart - total;
        DWORD chunk = (DWORD)(remaining > (1u << 30) ? (1u << 30) : remaining);
        DWORD got = 0;

        if (!ReadFile(file, output->data + total, chunk, &got, NULL) || !got)
        {
            CloseHandle(file);
            return 0;
        }
        total += got;
    }
    output->size = total;
    CloseHandle(file);
    return 1;
}

int WriteWholeFile(const wchar_t *path, const void *data, size_t size)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    const unsigned char *cursor = data;
    size_t total = 0;

    if (file == INVALID_HANDLE_VALUE)
        return 0;
    while (total < size)
    {
        size_t remaining = size - total;
        DWORD chunk = (DWORD)(remaining > (1u << 30) ? (1u << 30) : remaining);
        DWORD written = 0;

        if (!WriteFile(file, cursor + total, chunk, &written, NULL) || written != chunk)
        {
            CloseHandle(file);
            return 0;
        }
        total += written;
    }
    return CloseHandle(file) != 0;
}

int FileWriteTime(const wchar_t *path, uint64_t *time)
{
    WIN32_FILE_ATTRIBUTE_DATA data;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data))
        return 0;
    *time = ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
    return 1;
}

int FileSize(const wchar_t *path, uint64_t *size)
{
    WIN32_FILE_ATTRIBUTE_DATA data;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data))
        return 0;
    *size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return 1;
}

int MappedFileOpen(MappedFile *mapped, const wchar_t *path)
{
    LARGE_INTEGER size;

    memset(mapped, 0, sizeof(*mapped));
    mapped->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (mapped->file == INVALID_HANDLE_VALUE)
    {
        mapped->file = NULL;
        return 0;
    }
    if (!GetFileSizeEx(mapped->file, &size) || (uint64_t)size.QuadPart > (uint64_t)SIZE_MAX)
    {
        MappedFileClose(mapped);
        return 0;
    }
    mapped->size = (size_t)size.QuadPart;
    if (!mapped->size)
        return 1;
    mapped->mapping = CreateFileMappingW(mapped->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapped->mapping)
    {
        MappedFileClose(mapped);
        return 0;
    }
    mapped->view = MapViewOfFile(mapped->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!mapped->view)
    {
        MappedFileClose(mapped);
        return 0;
    }
    return 1;
}

void MappedFileClose(MappedFile *mapped)
{
    if (mapped->view)
        UnmapViewOfFile(mapped->view);
    if (mapped->mapping)
        CloseHandle(mapped->mapping);
    if (mapped->file)
        CloseHandle(mapped->file);
    memset(mapped, 0, sizeof(*mapped));
}

typedef struct WalkState
{
    wchar_t *buffer;
    size_t capacity;
    size_t rootLength;
    FileVisitor visitor;
    void *context;
} WalkState;

static void WalkReserve(WalkState *state, size_t length)
{
    if (length + 4 <= state->capacity)
        return;
    while (state->capacity < length + 4)
        state->capacity *= 2;
    state->buffer = MemoryResize(state->buffer, state->capacity * sizeof(wchar_t));
}

static int WalkDirectory(WalkState *state, size_t length)
{
    WIN32_FIND_DATAW data;
    HANDLE search;
    int keepGoing = 1;

    WalkReserve(state, length + 2);
    state->buffer[length] = L'\\';
    state->buffer[length + 1] = L'*';
    state->buffer[length + 2] = 0;
    search = FindFirstFileExW(state->buffer, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE)
        return 1;
    do
    {
        size_t nameLength;
        int result;

        if (data.cFileName[0] == L'.' && (!data.cFileName[1] || (data.cFileName[1] == L'.' && !data.cFileName[2])))
            continue;
        nameLength = wcslen(data.cFileName);
        WalkReserve(state, length + 1 + nameLength);
        state->buffer[length] = L'\\';
        memcpy(state->buffer + length + 1, data.cFileName, (nameLength + 1) * sizeof(wchar_t));
        result = state->visitor(state->context, state->buffer + state->rootLength + 1, &data);
        if (result == WALK_STOP)
        {
            keepGoing = 0;
            break;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && result == WALK_CONTINUE)
        {
            if (!WalkDirectory(state, length + 1 + nameLength))
            {
                keepGoing = 0;
                break;
            }
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return keepGoing;
}

int WalkFiles(const wchar_t *root, FileVisitor visitor, void *context)
{
    WalkState state;
    size_t rootLength = wcslen(root);
    int finished;

    while (rootLength > 3 && root[rootLength - 1] == L'\\')
        rootLength--;
    state.capacity = 1024;
    while (state.capacity < rootLength + 8)
        state.capacity *= 2;
    state.buffer = MemoryAllocate(state.capacity * sizeof(wchar_t));
    memcpy(state.buffer, root, rootLength * sizeof(wchar_t));
    state.buffer[rootLength] = 0;
    state.rootLength = rootLength;
    state.visitor = visitor;
    state.context = context;
    finished = WalkDirectory(&state, rootLength);
    MemoryRelease(state.buffer);
    return finished;
}

void ErrorLogInitialize(ErrorLog *log)
{
    memset(log, 0, sizeof(*log));
    InitializeSRWLock(&log->lock);
}

static void ErrorLogAppendLine(ErrorLog *log, const wchar_t *line, size_t length)
{
    if (log->length + length + 1 > log->capacity)
    {
        size_t capacity = log->capacity ? log->capacity : 4096;

        while (capacity < log->length + length + 1)
            capacity *= 2;
        log->text = MemoryResize(log->text, capacity * sizeof(wchar_t));
        log->capacity = capacity;
    }
    if (log->lineCount == log->lineCapacity)
    {
        log->lineCapacity = log->lineCapacity ? log->lineCapacity * 2 : 256;
        log->lineStarts = MemoryResize(log->lineStarts, log->lineCapacity * sizeof(size_t));
        log->lineLengths = MemoryResize(log->lineLengths, log->lineCapacity * sizeof(size_t));
    }
    memcpy(log->text + log->length, line, length * sizeof(wchar_t));
    log->lineStarts[log->lineCount] = log->length;
    log->lineLengths[log->lineCount] = length;
    log->lineCount++;
    log->length += length;
    log->text[log->length] = 0;
}

void ErrorLogAdd(ErrorLog *log, const wchar_t *format, ...)
{
    va_list arguments;
    wchar_t *message;
    const wchar_t *start;
    const wchar_t *cursor;

    va_start(arguments, format);
    message = WideFormatList(format, arguments);
    va_end(arguments);
    AcquireSRWLockExclusive(&log->lock);
    for (start = cursor = message;; cursor++)
    {
        if (*cursor == L'\n' || !*cursor)
        {
            size_t length = (size_t)(cursor - start);

            if (length && start[length - 1] == L'\r')
                length--;
            ErrorLogAppendLine(log, start, length);
            if (!*cursor)
                break;
            start = cursor + 1;
        }
    }
    InterlockedIncrement(&log->version);
    ReleaseSRWLockExclusive(&log->lock);
    MemoryRelease(message);
}

void ErrorLogClear(ErrorLog *log)
{
    AcquireSRWLockExclusive(&log->lock);
    log->length = 0;
    log->lineCount = 0;
    InterlockedIncrement(&log->version);
    ReleaseSRWLockExclusive(&log->lock);
}

size_t ErrorLogCount(ErrorLog *log)
{
    size_t count;

    AcquireSRWLockShared(&log->lock);
    count = log->lineCount;
    ReleaseSRWLockShared(&log->lock);
    return count;
}

wchar_t *ErrorLogJoined(ErrorLog *log)
{
    wchar_t *joined;
    size_t index;
    size_t position = 0;

    AcquireSRWLockShared(&log->lock);
    joined = MemoryAllocate((log->length + log->lineCount * 2 + 1) * sizeof(wchar_t));
    for (index = 0; index < log->lineCount; index++)
    {
        memcpy(joined + position, log->text + log->lineStarts[index], log->lineLengths[index] * sizeof(wchar_t));
        position += log->lineLengths[index];
        joined[position++] = L'\r';
        joined[position++] = L'\n';
    }
    joined[position] = 0;
    ReleaseSRWLockShared(&log->lock);
    return joined;
}

void ErrorLogFree(ErrorLog *log)
{
    MemoryRelease(log->text);
    MemoryRelease(log->lineStarts);
    MemoryRelease(log->lineLengths);
    memset(log, 0, sizeof(*log));
}
