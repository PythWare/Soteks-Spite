#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "loose.h"

#define GAME_SIZE_OF_IMAGE 0x1CD0000u
#define GAME_TIMESTAMP 0x55DE1122u
#define GAME_RVA_RESOLVE_PATH 0xE020B0u
#define GAME_RVA_RESOLVE_PATH_VTABLE_SLOT 0x1467498u
#define GAME_RVA_LOAD_FROM_MOUNTED_BIGS 0xDE94F0u
#define GAME_RVA_BINK_OPEN_IMPORT_SLOT 0x10A07F0u
#define BINK_FROM_MEMORY_FLAG 0x04000000u
#define MOUNTED_BIGS_PROLOGUE_SIZE 15
#define ABSOLUTE_JUMP_SIZE 14
#define PATH_CAPACITY 1024
#define RELATIVE_MODS_PREFIX "Mods\\"

typedef HRESULT (WINAPI *DirectInput8CreateFunction)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
typedef HRESULT (WINAPI *DllCanUnloadNowFunction)(void);
typedef HRESULT (WINAPI *DllGetClassObjectFunction)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *DllRegistrationFunction)(void);
typedef LPCVOID (WINAPI *GetdfDIJoystickFunction)(void);
typedef char *(*ResolvePathFunction)(void *, const char *, char *, size_t);
typedef void *(*LoadFromMountedBigsFunction)(const char *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, int *, uintptr_t);
typedef void *(*BinkOpenFunction)(const char *, unsigned int);

typedef struct ModFile
{
    uint64_t hash;
    char *path;
    volatile LONG logged;
} ModFile;

typedef struct PathIndex
{
    ModFile *files;
    size_t count;
    size_t capacity;
    ModFile **table;
    size_t mask;
} PathIndex;

static const BYTE resolvePathSignature[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57
};

static const BYTE loadFromMountedBigsSignature[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41
};

static HMODULE realDinput8;
static INIT_ONCE realDinput8Once = INIT_ONCE_STATIC_INIT;

static PathIndex modIndex;
static PathIndex cacheIndex;
static INIT_ONCE preparedOnce = INIT_ONCE_STATIC_INIT;
static WCHAR gameDirectory[PATH_CAPACITY];
static WCHAR modsRoot[PATH_CAPACITY];
static char modsPrefix[PATH_CAPACITY];

static HANDLE logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION logLock;
static ULONGLONG logStartTick;
static BOOL logAllPaths;
static uint64_t *seenHashes;
static size_t seenMask;
static size_t seenCount;

static ResolvePathFunction originalResolvePath;
static LoadFromMountedBigsFunction originalLoadFromMountedBigs;
static BinkOpenFunction originalBinkOpen;

static BOOL CALLBACK LoadRealDinput8(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    WCHAR path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);

    (void)once;
    (void)parameter;
    (void)context;
    if (length == 0 || length + 14 >= MAX_PATH)
        return FALSE;
    wcscat(path, L"\\dinput8.dll");
    realDinput8 = LoadLibraryW(path);
    return realDinput8 != NULL;
}

static FARPROC RealExport(const char *name)
{
    InitOnceExecuteOnce(&realDinput8Once, LoadRealDinput8, NULL, NULL);
    return realDinput8 ? GetProcAddress(realDinput8, name) : NULL;
}

HRESULT WINAPI ProxyDirectInput8Create(HINSTANCE instance, DWORD version, REFIID interfaceId, LPVOID *created, void *outer)
{
    DirectInput8CreateFunction real = (DirectInput8CreateFunction)(void *)RealExport("DirectInput8Create");
    return real ? real(instance, version, interfaceId, created, outer) : E_FAIL;
}

HRESULT WINAPI ProxyDllCanUnloadNow(void)
{
    DllCanUnloadNowFunction real = (DllCanUnloadNowFunction)(void *)RealExport("DllCanUnloadNow");
    return real ? real() : S_FALSE;
}

HRESULT WINAPI ProxyDllGetClassObject(REFCLSID classId, REFIID interfaceId, LPVOID *created)
{
    DllGetClassObjectFunction real = (DllGetClassObjectFunction)(void *)RealExport("DllGetClassObject");
    return real ? real(classId, interfaceId, created) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI ProxyDllRegisterServer(void)
{
    DllRegistrationFunction real = (DllRegistrationFunction)(void *)RealExport("DllRegisterServer");
    return real ? real() : E_FAIL;
}

HRESULT WINAPI ProxyDllUnregisterServer(void)
{
    DllRegistrationFunction real = (DllRegistrationFunction)(void *)RealExport("DllUnregisterServer");
    return real ? real() : E_FAIL;
}

LPCVOID WINAPI ProxyGetdfDIJoystick(void)
{
    GetdfDIJoystickFunction real = (GetdfDIJoystickFunction)(void *)RealExport("GetdfDIJoystick");
    return real ? real() : NULL;
}

static uint64_t HashPath(const char *path)
{
    uint64_t hash = 0xCBF29CE484222325ull;

    while (*path)
    {
        hash ^= (unsigned char)*path++;
        hash *= 0x100000001B3ull;
    }
    return hash ? hash : 1;
}

static BOOL NormalizeGamePath(const char *path, char *normalized, size_t capacity)
{
    size_t length = 0;

    if (!path || !*path)
        return FALSE;
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\'))
        path += 2;
    while (*path == '/' || *path == '\\')
        path++;
    for (; *path; path++)
    {
        char character = *path;

        if (character == ':')
            return FALSE;
        if (character == '/')
            character = '\\';
        if (character == '\\' && length > 0 && normalized[length - 1] == '\\')
            continue;
        if (character >= 'A' && character <= 'Z')
            character += 'a' - 'A';
        if (length + 1 >= capacity)
            return FALSE;
        normalized[length++] = character;
    }
    normalized[length] = 0;
    return length > 0;
}

static void LogFormat(const char *category, const char *format, ...)
{
    char message[PATH_CAPACITY * 2];
    char line[PATH_CAPACITY * 2 + 64];
    va_list arguments;
    DWORD written;
    int length;

    if (logFile == INVALID_HANDLE_VALUE)
        return;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    length = snprintf(line, sizeof line, "%10.3f  %-9s %s\r\n", (double)(GetTickCount64() - logStartTick) / 1000.0, category, message);
    if (length <= 0)
        return;
    if (length >= (int)sizeof line)
        length = (int)sizeof line - 1;
    EnterCriticalSection(&logLock);
    WriteFile(logFile, line, (DWORD)length, &written, NULL);
    LeaveCriticalSection(&logLock);
}

static BOOL GrowSeenHashes(void)
{
    size_t size = seenHashes ? (seenMask + 1) * 2 : 4096;
    uint64_t *table = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof *table);
    size_t index;

    if (!table)
        return FALSE;
    if (seenHashes)
    {
        for (index = 0; index <= seenMask; index++)
        {
            size_t slot;

            if (!seenHashes[index])
                continue;
            slot = (size_t)seenHashes[index] & (size - 1);
            while (table[slot])
                slot = (slot + 1) & (size - 1);
            table[slot] = seenHashes[index];
        }
        HeapFree(GetProcessHeap(), 0, seenHashes);
    }
    seenHashes = table;
    seenMask = size - 1;
    return TRUE;
}

static void LogUniquePath(const char *category, const char *path)
{
    uint64_t hash = HashPath(path) ^ ((uint64_t)(unsigned char)category[0] * 0x9E3779B97F4A7C15ull);
    size_t slot;

    if (!hash)
        hash = 1;
    EnterCriticalSection(&logLock);
    if ((!seenHashes || (seenCount + 1) * 2 > seenMask + 1) && !GrowSeenHashes())
    {
        LeaveCriticalSection(&logLock);
        return;
    }
    for (slot = (size_t)hash & seenMask; seenHashes[slot]; slot = (slot + 1) & seenMask)
    {
        if (seenHashes[slot] == hash)
        {
            LeaveCriticalSection(&logLock);
            return;
        }
    }
    seenHashes[slot] = hash;
    seenCount++;
    LogFormat(category, "%s", path);
    LeaveCriticalSection(&logLock);
}

static void IndexAppend(PathIndex *index, const char *utf8Path)
{
    char normalized[PATH_CAPACITY];
    size_t length;
    char *copy;

    if (!NormalizeGamePath(utf8Path, normalized, sizeof normalized))
        return;
    if (index->count == index->capacity)
    {
        size_t capacity = index->capacity ? index->capacity * 2 : 256;
        ModFile *grown = index->files
            ? HeapReAlloc(GetProcessHeap(), 0, index->files, capacity * sizeof *grown)
            : HeapAlloc(GetProcessHeap(), 0, capacity * sizeof *grown);

        if (!grown)
            return;
        index->files = grown;
        index->capacity = capacity;
    }
    length = strlen(normalized);
    copy = HeapAlloc(GetProcessHeap(), 0, length + 1);
    if (!copy)
        return;
    memcpy(copy, normalized, length + 1);
    index->files[index->count].hash = HashPath(normalized);
    index->files[index->count].path = copy;
    index->files[index->count].logged = 0;
    index->count++;
}

static void AppendModFile(const WCHAR *relativePath)
{
    char utf8[PATH_CAPACITY];

    if (WideCharToMultiByte(CP_UTF8, 0, relativePath, -1, utf8, sizeof utf8, NULL, NULL))
        IndexAppend(&modIndex, utf8);
}

static BOOL IsToolkitFile(const WCHAR *name)
{
    return lstrcmpiW(name, L"sotek_spite.log") == 0 || lstrcmpiW(name, L"SotekSpite.ini") == 0;
}

static void CollectModFiles(const WCHAR *directory, const WCHAR *relativeDirectory)
{
    WCHAR pattern[PATH_CAPACITY];
    WIN32_FIND_DATAW entry;
    HANDLE search;

    if (swprintf(pattern, PATH_CAPACITY, L"%ls\\*", directory) < 0)
        return;
    search = FindFirstFileExW(pattern, FindExInfoBasic, &entry, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do
    {
        WCHAR childDirectory[PATH_CAPACITY];
        WCHAR childRelative[PATH_CAPACITY];
        BOOL isRoot = relativeDirectory[0] == 0;

        if (entry.cFileName[0] == L'.')
            continue;
        if (swprintf(childDirectory, PATH_CAPACITY, L"%ls\\%ls", directory, entry.cFileName) < 0)
            continue;
        if (swprintf(childRelative, PATH_CAPACITY, isRoot ? L"%ls%ls" : L"%ls\\%ls", relativeDirectory, entry.cFileName) < 0)
            continue;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (isRoot && lstrcmpiW(entry.cFileName, LOOSE_CACHE_FOLDER) == 0)
                continue;
            if (LooseIsArchiveFolder(gameDirectory, childRelative))
                continue;
            CollectModFiles(childDirectory, childRelative);
        }
        else if (!isRoot || !IsToolkitFile(entry.cFileName))
            AppendModFile(childRelative);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

static void IndexBuild(PathIndex *index)
{
    size_t size = 16;
    size_t file;

    while (size < index->count * 2)
        size <<= 1;
    index->table = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof *index->table);
    if (!index->table)
    {
        index->count = 0;
        return;
    }
    index->mask = size - 1;
    for (file = 0; file < index->count; file++)
    {
        size_t slot = (size_t)index->files[file].hash & index->mask;

        while (index->table[slot])
            slot = (slot + 1) & index->mask;
        index->table[slot] = &index->files[file];
    }
}

static ModFile *IndexFind(const PathIndex *index, const char *normalized)
{
    uint64_t hash;
    size_t slot;

    if (!index->table || index->count == 0)
        return NULL;
    hash = HashPath(normalized);
    for (slot = (size_t)hash & index->mask; index->table[slot]; slot = (slot + 1) & index->mask)
    {
        if (index->table[slot]->hash == hash && strcmp(index->table[slot]->path, normalized) == 0)
            return index->table[slot];
    }
    return NULL;
}

static void LooseLogBridge(const char *category, const char *message)
{
    LogFormat(category, "%s", message);
}

static BOOL CALLBACK PrepareLoose(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    LooseResult result;
    size_t index;

    (void)once;
    (void)parameter;
    (void)context;
    if (LoosePrepare(gameDirectory, modsRoot, LooseLogBridge, &result))
    {
        for (index = 0; index < result.count; index++)
            IndexAppend(&cacheIndex, result.paths[index]);
        LooseResultFree(&result);
    }
    IndexBuild(&cacheIndex);
    return TRUE;
}

static void EnsurePrepared(void)
{
    InitOnceExecuteOnce(&preparedOnce, PrepareLoose, NULL, NULL);
}

static BOOL FormatModPath(char *resolved, size_t resolvedSize, const char *normalized)
{
    int written = snprintf(resolved, resolvedSize, "%s%s", modsPrefix, normalized);

    if (written > 0 && (size_t)written < resolvedSize)
        return TRUE;
    written = snprintf(resolved, resolvedSize, "%s%s", RELATIVE_MODS_PREFIX, normalized);
    return written > 0 && (size_t)written < resolvedSize;
}

static BOOL FormatCachePath(char *resolved, size_t resolvedSize, const char *normalized)
{
    int written = snprintf(resolved, resolvedSize, "%sSotekCache\\%s", modsPrefix, normalized);

    if (written > 0 && (size_t)written < resolvedSize)
        return TRUE;
    written = snprintf(resolved, resolvedSize, "%sSotekCache\\%s", RELATIVE_MODS_PREFIX, normalized);
    return written > 0 && (size_t)written < resolvedSize;
}

static BOOL RedirectPath(const char *normalized, char *resolved, size_t resolvedSize)
{
    ModFile *file = IndexFind(&cacheIndex, normalized);

    if (file && FormatCachePath(resolved, resolvedSize, normalized))
    {
        if (!InterlockedExchange(&file->logged, 1))
            LogFormat("cache", "%s", resolved);
        return TRUE;
    }
    file = IndexFind(&modIndex, normalized);
    if (file && FormatModPath(resolved, resolvedSize, normalized))
    {
        if (!InterlockedExchange(&file->logged, 1))
            LogFormat("override", "%s", resolved);
        return TRUE;
    }
    return FALSE;
}

static char *HookedResolvePath(void *fileSystem, const char *path, char *resolved, size_t resolvedSize)
{
    char normalized[PATH_CAPACITY];

    EnsurePrepared();
    if (NormalizeGamePath(path, normalized, sizeof normalized) && RedirectPath(normalized, resolved, resolvedSize))
        return resolved;
    if (logAllPaths && path)
        LogUniquePath("disk", path);
    return originalResolvePath(fileSystem, path, resolved, resolvedSize);
}

static void *HookedLoadFromMountedBigs(const char *path, uintptr_t forwardedArgument2, uintptr_t forwardedArgument3, uintptr_t forwardedArgument4, uintptr_t forwardedArgument5, int *loadedSize, uintptr_t forwardedArgument7)
{
    char normalized[PATH_CAPACITY];
    void *data;

    EnsurePrepared();
    if (NormalizeGamePath(path, normalized, sizeof normalized) && (IndexFind(&cacheIndex, normalized) || IndexFind(&modIndex, normalized)))
    {
        if (loadedSize)
            *loadedSize = 0;
        return NULL;
    }
    data = originalLoadFromMountedBigs(path, forwardedArgument2, forwardedArgument3, forwardedArgument4, forwardedArgument5, loadedSize, forwardedArgument7);
    if (logAllPaths && data && path)
        LogUniquePath("archive", path);
    return data;
}

static void *HookedBinkOpen(const char *path, unsigned int flags)
{
    char normalized[PATH_CAPACITY];
    char modPath[PATH_CAPACITY];

    if (flags & BINK_FROM_MEMORY_FLAG)
        return originalBinkOpen(path, flags);
    EnsurePrepared();
    if (NormalizeGamePath(path, normalized, sizeof normalized) && RedirectPath(normalized, modPath, sizeof modPath))
        return originalBinkOpen(modPath, flags);
    if (logAllPaths && path)
        LogUniquePath("movie", path);
    return originalBinkOpen(path, flags);
}

static BOOL WriteProtectedMemory(void *address, const void *data, size_t size)
{
    DWORD previousProtection;

    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previousProtection))
        return FALSE;
    memcpy(address, data, size);
    VirtualProtect(address, size, previousProtection, &previousProtection);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return TRUE;
}

static void WriteAbsoluteJump(BYTE *at, const void *destination)
{
    at[0] = 0xFF;
    at[1] = 0x25;
    at[2] = 0;
    at[3] = 0;
    at[4] = 0;
    at[5] = 0;
    memcpy(at + 6, &destination, sizeof destination);
}

static BOOL IsExpectedGameImage(const BYTE *gameBase)
{
    const IMAGE_DOS_HEADER *dosHeader = (const IMAGE_DOS_HEADER *)gameBase;
    const IMAGE_NT_HEADERS64 *ntHeaders;

    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return FALSE;
    ntHeaders = (const IMAGE_NT_HEADERS64 *)(gameBase + dosHeader->e_lfanew);
    return ntHeaders->Signature == IMAGE_NT_SIGNATURE
        && ntHeaders->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
        && ntHeaders->OptionalHeader.SizeOfImage == GAME_SIZE_OF_IMAGE
        && ntHeaders->FileHeader.TimeDateStamp == GAME_TIMESTAMP;
}

static BOOL InstallResolvePathHook(BYTE *gameBase)
{
    void **slot = (void **)(gameBase + GAME_RVA_RESOLVE_PATH_VTABLE_SLOT);
    BYTE *expected = gameBase + GAME_RVA_RESOLVE_PATH;
    void *replacement = (void *)HookedResolvePath;

    if (*slot != expected || memcmp(expected, resolvePathSignature, sizeof resolvePathSignature) != 0)
        return FALSE;
    originalResolvePath = (ResolvePathFunction)(void *)expected;
    return WriteProtectedMemory(slot, &replacement, sizeof replacement);
}

static BOOL InstallMountedBigsHook(BYTE *gameBase)
{
    BYTE *target = gameBase + GAME_RVA_LOAD_FROM_MOUNTED_BIGS;
    BYTE patch[MOUNTED_BIGS_PROLOGUE_SIZE];
    DWORD previousProtection;
    BYTE *trampoline;

    if (memcmp(target, loadFromMountedBigsSignature, sizeof loadFromMountedBigsSignature) != 0)
        return FALSE;
    trampoline = VirtualAlloc(NULL, MOUNTED_BIGS_PROLOGUE_SIZE + ABSOLUTE_JUMP_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!trampoline)
        return FALSE;
    memcpy(trampoline, target, MOUNTED_BIGS_PROLOGUE_SIZE);
    WriteAbsoluteJump(trampoline + MOUNTED_BIGS_PROLOGUE_SIZE, target + MOUNTED_BIGS_PROLOGUE_SIZE);
    if (!VirtualProtect(trampoline, MOUNTED_BIGS_PROLOGUE_SIZE + ABSOLUTE_JUMP_SIZE, PAGE_EXECUTE_READ, &previousProtection))
        return FALSE;
    FlushInstructionCache(GetCurrentProcess(), trampoline, MOUNTED_BIGS_PROLOGUE_SIZE + ABSOLUTE_JUMP_SIZE);
    originalLoadFromMountedBigs = (LoadFromMountedBigsFunction)(void *)trampoline;
    WriteAbsoluteJump(patch, (const void *)HookedLoadFromMountedBigs);
    memset(patch + ABSOLUTE_JUMP_SIZE, 0x90, MOUNTED_BIGS_PROLOGUE_SIZE - ABSOLUTE_JUMP_SIZE);
    return WriteProtectedMemory(target, patch, sizeof patch);
}

static BOOL InstallBinkOpenHook(BYTE *gameBase)
{
    void **slot = (void **)(gameBase + GAME_RVA_BINK_OPEN_IMPORT_SLOT);
    HMODULE bink = GetModuleHandleW(L"bink2w64.dll");
    void *replacement = (void *)HookedBinkOpen;
    void *resolved;

    if (!bink)
        return FALSE;
    resolved = (void *)GetProcAddress(bink, "BinkOpen");
    if (!resolved || *slot != resolved)
        return FALSE;
    originalBinkOpen = (BinkOpenFunction)resolved;
    return WriteProtectedMemory(slot, &replacement, sizeof replacement);
}

static void BuildModsPrefix(void)
{
    BOOL usedDefaultCharacter = FALSE;
    int length = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, modsRoot, -1, modsPrefix, PATH_CAPACITY - 2, NULL, &usedDefaultCharacter);

    if (length <= 0 || usedDefaultCharacter)
    {
        strcpy(modsPrefix, RELATIVE_MODS_PREFIX);
        return;
    }
    strcat(modsPrefix, "\\");
}

static void Initialize(void)
{
    WCHAR logPath[PATH_CAPACITY];
    WCHAR settingsPath[PATH_CAPACITY];
    BYTE *gameBase = (BYTE *)GetModuleHandleW(NULL);
    DWORD length = GetModuleFileNameW(NULL, gameDirectory, PATH_CAPACITY);
    DWORD attributes;
    WCHAR *lastSeparator;
    BOOL resolvePathHooked;
    BOOL mountedBigsHooked;
    BOOL binkOpenHooked;

    if (length == 0 || length >= PATH_CAPACITY)
        return;
    lastSeparator = wcsrchr(gameDirectory, L'\\');
    if (!lastSeparator)
        return;
    *lastSeparator = 0;
    if (swprintf(modsRoot, PATH_CAPACITY, L"%ls\\Mods", gameDirectory) < 0)
        return;
    attributes = GetFileAttributesW(modsRoot);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return;
    if (swprintf(logPath, PATH_CAPACITY, L"%ls\\sotek_spite.log", modsRoot) < 0)
        return;
    if (swprintf(settingsPath, PATH_CAPACITY, L"%ls\\SotekSpite.ini", modsRoot) < 0)
        return;
    InitializeCriticalSection(&logLock);
    logStartTick = GetTickCount64();
    logFile = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    logAllPaths = GetPrivateProfileIntW(L"Log", L"AllPaths", 0, settingsPath) != 0;
    LogFormat("start", "Sotek's Spite proxy, log all paths: %s", logAllPaths ? "yes" : "no");
    if (!IsExpectedGameImage(gameBase))
    {
        LogFormat("disabled", "unrecognised executable, acting as a plain dinput8 proxy");
        return;
    }
    BuildModsPrefix();
    CollectModFiles(modsRoot, L"");
    IndexBuild(&modIndex);
    LogFormat("mods", "%zu files indexed, override prefix %s", modIndex.count, modsPrefix);
    resolvePathHooked = InstallResolvePathHook(gameBase);
    mountedBigsHooked = resolvePathHooked && InstallMountedBigsHook(gameBase);
    binkOpenHooked = InstallBinkOpenHook(gameBase);
    LogFormat("hooks", "ResolvePath %s, LoadFromMountedBigs %s, BinkOpen %s", resolvePathHooked ? "ok" : "failed", mountedBigsHooked ? "ok" : "failed", binkOpenHooked ? "ok" : "failed");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        Initialize();
    }
    return TRUE;
}
