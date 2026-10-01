#include "loose.h"
#include "operations.h"
#include "big.h"

#define LOOSE_MANIFEST L"sotek_cache.txt"
#define LOOSE_MANIFEST_MAGIC "SOTEKCACHE 1"
#define LOOSE_MAX_DEPTH 8

typedef struct LooseFolder
{
    wchar_t *gamePath;
    wchar_t *folder;
} LooseFolder;

typedef struct LooseFolderList
{
    LooseFolder *items;
    size_t count;
    size_t capacity;
} LooseFolderList;

typedef struct ScanState
{
    const wchar_t *gameRoot;
    const wchar_t *modsRoot;
    LooseFolderList folders;
    WideList packed;
} ScanState;

typedef struct NameIndex
{
    unsigned char **keys;
    size_t *lengths;
    uint64_t *hashes;
    uint32_t *values;
    size_t capacity;
    size_t count;
} NameIndex;

typedef struct MergeStats
{
    volatile LONG64 replaced;
    volatile LONG64 added;
    volatile LONG64 nested;
} MergeStats;

typedef struct LooseCollector
{
    OperationContext *context;
    const BigView *view;
    const NameIndex *names;
    const wchar_t *root;
    int depth;
    wchar_t **replacements;
    wchar_t **nestedFolders;
    WideList addedRelatives;
} LooseCollector;

typedef struct MergeJob
{
    OperationContext *context;
    const LooseFolder *folder;
    const wchar_t *gameRoot;
    const wchar_t *modsRoot;
    wchar_t *output;
    MergeStats *stats;
    int succeeded;
} MergeJob;

static const wchar_t *const ArchiveSuffixes[] = {L"_big", L"_tex", L"_hrt"};

static int HasArchiveSuffix(const wchar_t *name)
{
    size_t index;

    for (index = 0; index < sizeof(ArchiveSuffixes) / sizeof(ArchiveSuffixes[0]); index++)
        if (wcslen(name) > 4 && WideEndsWithInsensitive(name, ArchiveSuffixes[index]))
            return 1;
    return 0;
}

static wchar_t *SuffixToExtension(const wchar_t *relative)
{
    wchar_t *mapped = WideDuplicate(relative);
    wchar_t *underscore = wcsrchr(mapped, L'_');

    if (underscore)
        *underscore = L'.';
    return mapped;
}

int LooseIsArchiveFolder(const wchar_t *gameRoot, const wchar_t *relativeDirectory)
{
    wchar_t *mapped;
    wchar_t *vanilla;
    int exists;

    if (!HasArchiveSuffix(PathName(relativeDirectory)))
        return 0;
    mapped = SuffixToExtension(relativeDirectory);
    vanilla = PathJoin(gameRoot, mapped);
    exists = PathIsFile(vanilla);
    MemoryRelease(vanilla);
    MemoryRelease(mapped);
    return exists;
}

static uint64_t LowerHash(const unsigned char *text, size_t length)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    size_t index;

    for (index = 0; index < length; index++)
    {
        hash ^= Latin1Lower(text[index]);
        hash *= 0x100000001B3ull;
    }
    return hash ? hash : 1;
}

static int LowerEqual(const unsigned char *left, size_t leftLength, const unsigned char *right, size_t rightLength)
{
    size_t index;

    if (leftLength != rightLength)
        return 0;
    for (index = 0; index < leftLength; index++)
        if (Latin1Lower(left[index]) != Latin1Lower(right[index]))
            return 0;
    return 1;
}

static void NameIndexCreate(NameIndex *index, size_t expected)
{
    memset(index, 0, sizeof(*index));
    index->capacity = 16;
    while (index->capacity < expected * 2 + 2)
        index->capacity *= 2;
    index->keys = MemoryAllocateZero(index->capacity, sizeof(unsigned char *));
    index->lengths = MemoryAllocateZero(index->capacity, sizeof(size_t));
    index->hashes = MemoryAllocateZero(index->capacity, sizeof(uint64_t));
    index->values = MemoryAllocateZero(index->capacity, sizeof(uint32_t));
}

static void NameIndexInsert(NameIndex *index, const unsigned char *key, size_t length, uint32_t value)
{
    uint64_t hash = LowerHash(key, length);
    size_t slot;

    for (slot = (size_t)hash & (index->capacity - 1); index->keys[slot]; slot = (slot + 1) & (index->capacity - 1))
        if (index->hashes[slot] == hash && LowerEqual(index->keys[slot], index->lengths[slot], key, length))
            return;
    index->keys[slot] = MemoryAllocate(length + 1);
    memcpy(index->keys[slot], key, length);
    index->keys[slot][length] = 0;
    index->lengths[slot] = length;
    index->hashes[slot] = hash;
    index->values[slot] = value;
    index->count++;
}

static int NameIndexFind(const NameIndex *index, const unsigned char *key, size_t length, uint32_t *value)
{
    uint64_t hash = LowerHash(key, length);
    size_t slot;

    for (slot = (size_t)hash & (index->capacity - 1); index->keys[slot]; slot = (slot + 1) & (index->capacity - 1))
    {
        if (index->hashes[slot] == hash && LowerEqual(index->keys[slot], index->lengths[slot], key, length))
        {
            *value = index->values[slot];
            return 1;
        }
    }
    return 0;
}

static void NameIndexFree(NameIndex *index)
{
    size_t slot;

    for (slot = 0; slot < index->capacity; slot++)
        MemoryRelease(index->keys[slot]);
    MemoryRelease(index->keys);
    MemoryRelease(index->lengths);
    MemoryRelease(index->hashes);
    MemoryRelease(index->values);
    memset(index, 0, sizeof(*index));
}

static int EntryIsArchive(const BigView *view, uint32_t index)
{
    BigRecord record;
    const unsigned char *data;
    size_t size;
    unsigned char *owned;
    int archive;

    if (!BigGetRecord(view, index, &record) || !BigEntryData(view, &record, &data, &size, &owned))
        return 0;
    archive = BigIsComplete(data, size);
    MemoryRelease(owned);
    return archive;
}

static int CollectLoose(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data)
{
    LooseCollector *collector = context;
    ByteBuffer key = {0};
    uint32_t entry;

    if (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        int nested = 0;

        if (HasArchiveSuffix(PathName(relativePath)) && collector->depth < LOOSE_MAX_DEPTH)
        {
            wchar_t *mapped = SuffixToExtension(relativePath);

            if (WideToLatin1(mapped, &key) && NameIndexFind(collector->names, key.data, key.size, &entry) && EntryIsArchive(collector->view, entry))
            {
                MemoryRelease(collector->nestedFolders[entry]);
                collector->nestedFolders[entry] = PathJoin(collector->root, relativePath);
                nested = 1;
            }
            MemoryRelease(mapped);
        }
        ByteBufferFree(&key);
        return nested ? WALK_SKIP : WALK_CONTINUE;
    }
    if (WideEqualInsensitive(relativePath, BIG_MANIFEST_NAME))
        return WALK_CONTINUE;
    if (!WideToLatin1(relativePath, &key))
    {
        ErrorLogAdd(collector->context->errors, L"Skipped %ls, its name cant be stored in a BIG archive", relativePath);
        ByteBufferFree(&key);
        return WALK_CONTINUE;
    }
    if (NameIndexFind(collector->names, key.data, key.size, &entry))
    {
        MemoryRelease(collector->replacements[entry]);
        collector->replacements[entry] = PathJoin(collector->root, relativePath);
    }
    else
        WideListAppend(&collector->addedRelatives, WideDuplicate(relativePath));
    ByteBufferFree(&key);
    return WALK_CONTINUE;
}

static const unsigned char *ExtensionOf(const unsigned char *name, size_t length, size_t *extensionLength)
{
    size_t baseStart = 0;
    size_t cursor;
    size_t dot = length;

    for (cursor = 0; cursor < length; cursor++)
        if (name[cursor] == '\\' || name[cursor] == '/')
            baseStart = cursor + 1;
    cursor = baseStart;
    while (cursor < length && name[cursor] == '.')
        cursor++;
    for (; cursor < length; cursor++)
        if (name[cursor] == '.')
            dot = cursor;
    *extensionLength = length - dot;
    return name + dot;
}

static uint32_t GuessAlignment(const BigEntry *entries, size_t count, const unsigned char *name, size_t nameLength)
{
    size_t extensionLength;
    const unsigned char *extension = ExtensionOf(name, nameLength, &extensionLength);
    uint32_t alignments[16];
    size_t votes[16];
    size_t distinct = 0;
    size_t best = 0;
    size_t index;

    for (index = 0; index < count; index++)
    {
        size_t otherLength;
        const unsigned char *other = ExtensionOf(entries[index].name, entries[index].nameLength, &otherLength);
        size_t slot;

        if (!LowerEqual(extension, extensionLength, other, otherLength))
            continue;
        for (slot = 0; slot < distinct && alignments[slot] != entries[index].alignment; slot++)
            ;
        if (slot == distinct)
        {
            if (distinct == 16)
                continue;
            alignments[distinct] = entries[index].alignment;
            votes[distinct++] = 0;
        }
        votes[slot]++;
    }
    if (!distinct)
        return 4;
    for (index = 1; index < distinct; index++)
        if (votes[index] > votes[best])
            best = index;
    return alignments[best];
}

static int MergeArchive(OperationContext *context, const unsigned char *data, size_t size, const wchar_t *folder, int depth, const wchar_t *origin, const wchar_t *writePath, ByteBuffer *output, MergeStats *stats)
{
    BigView view;
    NameIndex names;
    NameSet used = {0};
    ByteBuffer fileName = {0};
    LooseCollector collector;
    BigEntry *entries = NULL;
    ByteBuffer *owned = NULL;
    unsigned char **addedNames = NULL;
    uint64_t *addedSizes = NULL;
    wchar_t **addedPaths = NULL;
    size_t total;
    size_t index;
    uint32_t entry;
    int succeeded = 1;

    if (!BigOpenView(&view, data, size))
    {
        ErrorLogAdd(context->errors, L"Couldnt read archive %ls", PathDisplay(origin));
        return 0;
    }
    NameIndexCreate(&names, view.header.count);
    for (entry = 0; entry < view.header.count; entry++)
    {
        BigRecord record;
        const unsigned char *name;
        size_t nameLength;

        if (!BigGetRecord(&view, entry, &record) || !BigEntryName(&view, &record, &name, &nameLength))
        {
            ErrorLogAdd(context->errors, L"Damaged entry %u in %ls", entry, PathDisplay(origin));
            succeeded = 0;
            break;
        }
        BigFileName(name, nameLength, entry, &used, &fileName);
        NameIndexInsert(&names, fileName.data, fileName.size, entry);
    }
    NameSetFree(&used);
    ByteBufferFree(&fileName);
    memset(&collector, 0, sizeof(collector));
    collector.context = context;
    collector.view = &view;
    collector.names = &names;
    collector.root = folder;
    collector.depth = depth;
    collector.replacements = MemoryAllocateZero(view.header.count ? view.header.count : 1, sizeof(wchar_t *));
    collector.nestedFolders = MemoryAllocateZero(view.header.count ? view.header.count : 1, sizeof(wchar_t *));
    if (succeeded)
        WalkFiles(folder, CollectLoose, &collector);
    total = view.header.count + collector.addedRelatives.count;
    entries = MemoryAllocateZero(total ? total : 1, sizeof(BigEntry));
    owned = MemoryAllocateZero(total ? total : 1, sizeof(ByteBuffer));
    addedNames = MemoryAllocateZero(collector.addedRelatives.count ? collector.addedRelatives.count : 1, sizeof(unsigned char *));
    addedSizes = MemoryAllocateZero(collector.addedRelatives.count ? collector.addedRelatives.count : 1, sizeof(uint64_t));
    addedPaths = MemoryAllocateZero(collector.addedRelatives.count ? collector.addedRelatives.count : 1, sizeof(wchar_t *));
    for (entry = 0; entry < view.header.count && succeeded && !OperationCancelled(context); entry++)
    {
        BigRecord record;
        const unsigned char *stored;
        ByteBuffer payload = {0};

        BigGetRecord(&view, entry, &record);
        BigEntryName(&view, &record, &entries[entry].name, &entries[entry].nameLength);
        entries[entry].alignment = record.alignment;
        entries[entry].compression = record.compression;
        entries[entry].size = record.size;
        if (!BigEntryStored(&view, &record, &stored))
        {
            ErrorLogAdd(context->errors, L"Damaged entry %u in %ls", entry, PathDisplay(origin));
            succeeded = 0;
            break;
        }
        entries[entry].stored = stored;
        entries[entry].storedSize = record.storedSize;
        if (!collector.replacements[entry] && !collector.nestedFolders[entry])
            continue;
        if (collector.replacements[entry] && !collector.nestedFolders[entry] && record.compression == 0)
        {
            uint64_t fileSize;

            if (!FileSize(collector.replacements[entry], &fileSize) || fileSize > 0xFFFFFFFFull)
            {
                ErrorLogAdd(context->errors, L"Couldnt read %ls", PathDisplay(collector.replacements[entry]));
                succeeded = 0;
                break;
            }
            entries[entry].stored = NULL;
            entries[entry].sourcePath = collector.replacements[entry];
            entries[entry].storedSize = (size_t)fileSize;
            entries[entry].size = (uint32_t)fileSize;
            InterlockedIncrement64(&stats->replaced);
            continue;
        }
        if (collector.replacements[entry])
        {
            if (!ReadWholeFile(collector.replacements[entry], &payload))
            {
                ErrorLogAdd(context->errors, L"Couldnt read %ls", PathDisplay(collector.replacements[entry]));
                ByteBufferFree(&payload);
                succeeded = 0;
                break;
            }
            InterlockedIncrement64(&stats->replaced);
        }
        else
        {
            const unsigned char *entryData;
            size_t entrySize;
            unsigned char *entryOwned;

            if (!BigEntryData(&view, &record, &entryData, &entrySize, &entryOwned))
            {
                ErrorLogAdd(context->errors, L"Couldnt extract entry %u from %ls", entry, PathDisplay(origin));
                succeeded = 0;
                break;
            }
            ByteBufferAppend(&payload, entryData, entrySize);
            MemoryRelease(entryOwned);
        }
        if (collector.nestedFolders[entry])
        {
            ByteBuffer merged = {0};

            if (!MergeArchive(context, payload.data, payload.size, collector.nestedFolders[entry], depth + 1, collector.nestedFolders[entry], NULL, &merged, stats))
            {
                ByteBufferFree(&merged);
                ByteBufferFree(&payload);
                succeeded = 0;
                break;
            }
            ByteBufferFree(&payload);
            payload = merged;
            InterlockedIncrement64(&stats->nested);
        }
        if (payload.size > 0xFFFFFFFFull)
        {
            ErrorLogAdd(context->errors, L"An entry of %ls grew larger than 4 GB", PathDisplay(origin));
            ByteBufferFree(&payload);
            succeeded = 0;
            break;
        }
        entries[entry].size = (uint32_t)payload.size;
        if (record.compression == 1)
        {
            if (!BigCompress(payload.data, payload.size, &owned[entry]))
            {
                ErrorLogAdd(context->errors, L"Couldnt compress an entry of %ls", PathDisplay(origin));
                ByteBufferFree(&payload);
                succeeded = 0;
                break;
            }
            ByteBufferFree(&payload);
        }
        else
            owned[entry] = payload;
        entries[entry].stored = owned[entry].data;
        entries[entry].storedSize = owned[entry].size;
    }
    for (index = 0; index < collector.addedRelatives.count && succeeded; index++)
    {
        ByteBuffer name = {0};
        BigEntry *added = &entries[view.header.count + index];

        WideToLatin1(collector.addedRelatives.items[index], &name);
        addedPaths[index] = PathJoin(folder, collector.addedRelatives.items[index]);
        if (!FileSize(addedPaths[index], &addedSizes[index]) || addedSizes[index] > 0xFFFFFFFFull)
        {
            ErrorLogAdd(context->errors, L"Couldnt read %ls", PathDisplay(addedPaths[index]));
            ByteBufferFree(&name);
            succeeded = 0;
            break;
        }
        addedNames[index] = name.data;
        added->name = name.data;
        added->nameLength = name.size;
        added->alignment = GuessAlignment(entries, view.header.count, name.data, name.size);
        added->compression = 0;
        added->sourcePath = addedPaths[index];
        added->storedSize = (size_t)addedSizes[index];
        added->size = (uint32_t)addedSizes[index];
        InterlockedIncrement64(&stats->added);
    }
    if (succeeded && !OperationCancelled(context))
    {
        if (writePath)
        {
            if (!CreateParentDirectories(writePath) || !BigWriteFile(writePath, entries, total))
            {
                ErrorLogAdd(context->errors, L"Couldnt write %ls", PathDisplay(writePath));
                succeeded = 0;
            }
        }
        else if (!BigBuildBuffer(entries, total, output))
        {
            ErrorLogAdd(context->errors, L"Couldnt build the merged archive for %ls", PathDisplay(origin));
            succeeded = 0;
        }
    }
    for (index = 0; index < total; index++)
        ByteBufferFree(&owned[index]);
    for (index = 0; index < collector.addedRelatives.count; index++)
    {
        MemoryRelease(addedNames[index]);
        MemoryRelease(addedPaths[index]);
    }
    for (index = 0; index < view.header.count; index++)
    {
        MemoryRelease(collector.replacements[index]);
        MemoryRelease(collector.nestedFolders[index]);
    }
    MemoryRelease(collector.replacements);
    MemoryRelease(collector.nestedFolders);
    WideListFree(&collector.addedRelatives);
    MemoryRelease(addedNames);
    MemoryRelease(addedSizes);
    MemoryRelease(addedPaths);
    MemoryRelease(owned);
    MemoryRelease(entries);
    NameIndexFree(&names);
    return succeeded && !OperationCancelled(context);
}

static wchar_t *LowerCopy(const wchar_t *text)
{
    wchar_t *copy = WideDuplicate(text);
    wchar_t *cursor;

    for (cursor = copy; *cursor; cursor++)
        *cursor = WideLower(*cursor);
    return copy;
}

static void MergeTask(void *argument)
{
    MergeJob *job = argument;
    wchar_t *packedPath = PathJoin(job->modsRoot, job->folder->gamePath);
    wchar_t *vanillaPath = PathJoin(job->gameRoot, job->folder->gamePath);
    const wchar_t *basePath = PathIsFile(packedPath) ? packedPath : vanillaPath;
    MappedFile base;

    if (!MappedFileOpen(&base, basePath))
        ErrorLogAdd(job->context->errors, L"Couldnt open %ls", PathDisplay(basePath));
    else
    {
        job->succeeded = MergeArchive(job->context, base.view, base.size, job->folder->folder, 0, basePath, job->output, NULL, job->stats);
        MappedFileClose(&base);
    }
    MemoryRelease(packedPath);
    MemoryRelease(vanillaPath);
}

static int ScanMods(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data)
{
    ScanState *state = context;

    if (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        if (!wcschr(relativePath, L'\\') && WideEqualInsensitive(relativePath, LOOSE_CACHE_FOLDER))
            return WALK_SKIP;
        if (LooseIsArchiveFolder(state->gameRoot, relativePath))
        {
            LooseFolderList *list = &state->folders;

            if (list->count == list->capacity)
            {
                list->capacity = list->capacity ? list->capacity * 2 : 16;
                list->items = MemoryResize(list->items, list->capacity * sizeof(LooseFolder));
            }
            list->items[list->count].gamePath = SuffixToExtension(relativePath);
            list->items[list->count].folder = PathJoin(state->modsRoot, relativePath);
            list->count++;
            return WALK_SKIP;
        }
        return WALK_CONTINUE;
    }
    if (WideEndsWithInsensitive(relativePath, L".big") || WideEndsWithInsensitive(relativePath, L".tex") || WideEndsWithInsensitive(relativePath, L".hrt"))
        WideListAppend(&state->packed, WideDuplicate(relativePath));
    return WALK_CONTINUE;
}

static void KeyAdd(uint64_t *key, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    size_t index;

    for (index = 0; index < size; index++)
    {
        *key ^= bytes[index];
        *key *= 0x100000001B3ull;
    }
}

static void KeyAddText(uint64_t *key, const wchar_t *text)
{
    for (; *text; text++)
    {
        wchar_t lower = WideLower(*text);

        KeyAdd(key, &lower, sizeof(lower));
    }
    KeyAdd(key, "|", 1);
}

static void KeyAddFile(uint64_t *key, const wchar_t *path)
{
    uint64_t size = 0;
    uint64_t time = 0;

    FileSize(path, &size);
    FileWriteTime(path, &time);
    KeyAdd(key, &size, sizeof(size));
    KeyAdd(key, &time, sizeof(time));
}

typedef struct KeyWalk
{
    uint64_t *key;
    const wchar_t *root;
} KeyWalk;

static int KeyFolderVisitor(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data)
{
    KeyWalk *walk = context;
    wchar_t *path;

    KeyAddText(walk->key, relativePath);
    if (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return WALK_CONTINUE;
    path = PathJoin(walk->root, relativePath);
    KeyAddFile(walk->key, path);
    MemoryRelease(path);
    return WALK_CONTINUE;
}

typedef struct CacheCollector
{
    WideList files;
} CacheCollector;

static int CollectCache(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data)
{
    CacheCollector *collector = context;

    if (!(data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !WideEqualInsensitive(relativePath, LOOSE_MANIFEST))
        WideListAppend(&collector->files, LowerCopy(relativePath));
    return WALK_CONTINUE;
}

static char *WideToUtf8(const wchar_t *text)
{
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *utf8 = MemoryAllocate(bytes > 0 ? (size_t)bytes : 1);

    if (bytes <= 0 || !WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, bytes, NULL, NULL))
        utf8[0] = 0;
    return utf8;
}

static wchar_t *Utf8ToWide(const char *text, size_t length)
{
    int characters = MultiByteToWideChar(CP_UTF8, 0, text, (int)length, NULL, 0);
    wchar_t *wide = MemoryAllocate(((size_t)(characters > 0 ? characters : 0) + 1) * sizeof(wchar_t));

    if (characters > 0)
        MultiByteToWideChar(CP_UTF8, 0, text, (int)length, wide, characters);
    wide[characters > 0 ? characters : 0] = 0;
    return wide;
}

static void Report(LooseLogFunction log, const char *category, const wchar_t *format, ...)
{
    va_list arguments;
    wchar_t *message;
    char *utf8;

    if (!log)
        return;
    va_start(arguments, format);
    message = WideFormatList(format, arguments);
    va_end(arguments);
    utf8 = WideToUtf8(message);
    log(category, utf8);
    MemoryRelease(utf8);
    MemoryRelease(message);
}

static int ReadCacheManifest(const wchar_t *cacheRoot, const char *key, WideList *files)
{
    wchar_t *path = PathJoin(cacheRoot, LOOSE_MANIFEST);
    ByteBuffer text = {0};
    size_t position = 0;
    size_t line = 0;
    int matches = 0;

    if (ReadWholeFile(path, &text))
    {
        while (position < text.size)
        {
            unsigned char *start = text.data + position;
            unsigned char *end = memchr(start, '\n', text.size - position);
            size_t length = end ? (size_t)(end - start) : text.size - position;

            position += length + 1;
            if (length && start[length - 1] == '\r')
                length--;
            line++;
            if (line == 1 && (length != sizeof(LOOSE_MANIFEST_MAGIC) - 1 || memcmp(start, LOOSE_MANIFEST_MAGIC, length) != 0))
                break;
            if (line == 2)
                matches = length == strlen(key) && memcmp(start, key, length) == 0;
            if (line > 2 && length)
                WideListAppend(files, Utf8ToWide((const char *)start, length));
        }
    }
    ByteBufferFree(&text);
    MemoryRelease(path);
    return matches;
}

static void WriteCacheManifest(const wchar_t *cacheRoot, const char *key, const WideList *files)
{
    wchar_t *path = PathJoin(cacheRoot, LOOSE_MANIFEST);
    ByteBuffer text = {0};
    size_t index;

    ByteBufferAppendText(&text, LOOSE_MANIFEST_MAGIC "\n");
    ByteBufferAppendText(&text, key);
    ByteBufferAppendByte(&text, '\n');
    for (index = 0; index < files->count; index++)
    {
        char *utf8 = WideToUtf8(files->items[index]);

        ByteBufferAppendText(&text, utf8);
        ByteBufferAppendByte(&text, '\n');
        MemoryRelease(utf8);
    }
    WriteWholeFile(path, text.data, text.size);
    ByteBufferFree(&text);
    MemoryRelease(path);
}

static int AddSource(CompanionSource **sources, size_t *count, size_t *capacity, const wchar_t *relative, const wchar_t *cacheRoot, const wchar_t *path)
{
    ByteBuffer head = {0};
    BigHeader header;
    CompanionSource *source;

    if (!ReadFilePrefix(path, BIG_HEADER_SIZE, &head) || !BigReadHeader(head.data, head.size, &header))
    {
        ByteBufferFree(&head);
        return 0;
    }
    ByteBufferFree(&head);
    if (*count == *capacity)
    {
        *capacity = *capacity ? *capacity * 2 : 16;
        *sources = MemoryResize(*sources, *capacity * sizeof(CompanionSource));
    }
    source = &(*sources)[*count];
    memset(source, 0, sizeof(*source));
    if (!ReadFilePrefix(path, header.dataOffset, &source->prefix) || source->prefix.size != header.dataOffset)
    {
        ByteBufferFree(&source->prefix);
        return 0;
    }
    source->relative = WideDuplicate(relative);
    source->outputRoot = WideDuplicate(cacheRoot);
    source->path = WideDuplicate(path);
    (*count)++;
    return 1;
}

static void FreeScan(ScanState *scan)
{
    size_t index;

    for (index = 0; index < scan->folders.count; index++)
    {
        MemoryRelease(scan->folders.items[index].gamePath);
        MemoryRelease(scan->folders.items[index].folder);
    }
    MemoryRelease(scan->folders.items);
    WideListFree(&scan->packed);
}

int LoosePrepare(const wchar_t *gameRootIn, const wchar_t *modsRootIn, LooseLogFunction log, LooseResult *result)
{
    wchar_t *gameRoot = PathAbsolute(gameRootIn);
    wchar_t *modsRoot = PathAbsolute(modsRootIn);
    wchar_t *cacheRoot = PathJoin(modsRoot, LOOSE_CACHE_FOLDER);
    wchar_t *companionsPath = PathJoin(gameRoot, L"vanilla_companions.txt");
    ScanState scan;
    Companions companions;
    int companionsLoaded = 0;
    WideList withCompanions = {0};
    WideList previous = {0};
    CacheCollector produced;
    WorkerPool pool;
    ErrorLog errors;
    OperationContext context;
    uint64_t keyValue = 0xCBF29CE484222325ull;
    char key[32];
    double started = SecondsNow();
    size_t index;
    int reused = 0;

    memset(result, 0, sizeof(*result));
    memset(&scan, 0, sizeof(scan));
    memset(&companions, 0, sizeof(companions));
    memset(&produced, 0, sizeof(produced));
    memset(&context, 0, sizeof(context));
    ErrorLogInitialize(&errors);
    scan.gameRoot = gameRoot;
    scan.modsRoot = modsRoot;
    WalkFiles(modsRoot, ScanMods, &scan);
    if (PathIsFile(companionsPath))
        companionsLoaded = CompanionsLoad(companionsPath, &companions, &errors);
    if (companionsLoaded)
        for (index = 0; index < scan.packed.count; index++)
            if (CompanionsFind(&companions, scan.packed.items[index]))
                WideListAppend(&withCompanions, WideDuplicate(scan.packed.items[index]));
    if (!scan.folders.count && !withCompanions.count)
    {
        WideList stale = {0};

        ReadCacheManifest(cacheRoot, "", &stale);
        for (index = 0; index < stale.count; index++)
        {
            wchar_t *path = PathJoin(cacheRoot, stale.items[index]);

            DeleteFileW(path);
            MemoryRelease(path);
        }
        WideListFree(&stale);
        {
            wchar_t *manifest = PathJoin(cacheRoot, LOOSE_MANIFEST);

            DeleteFileW(manifest);
            MemoryRelease(manifest);
        }
        goto finish;
    }
    if (!companionsLoaded)
        Report(log, "warning", L"vanilla_companions.txt wasnt found next to the game, companion copies wont be updated");
    KeyAdd(&keyValue, LOOSE_MANIFEST_MAGIC, sizeof(LOOSE_MANIFEST_MAGIC));
    KeyAddFile(&keyValue, companionsPath);
    for (index = 0; index < scan.folders.count; index++)
    {
        wchar_t *packedPath = PathJoin(modsRoot, scan.folders.items[index].gamePath);
        wchar_t *vanillaPath = PathJoin(gameRoot, scan.folders.items[index].gamePath);

        KeyAddText(&keyValue, scan.folders.items[index].gamePath);
        KeyAddFile(&keyValue, vanillaPath);
        KeyAddFile(&keyValue, packedPath);
        {
            KeyWalk walk;

            walk.key = &keyValue;
            walk.root = scan.folders.items[index].folder;
            WalkFiles(scan.folders.items[index].folder, KeyFolderVisitor, &walk);
        }
        MemoryRelease(packedPath);
        MemoryRelease(vanillaPath);
    }
    for (index = 0; index < withCompanions.count; index++)
    {
        wchar_t *path = PathJoin(modsRoot, withCompanions.items[index]);

        KeyAddText(&keyValue, withCompanions.items[index]);
        KeyAddFile(&keyValue, path);
        MemoryRelease(path);
    }
    snprintf(key, sizeof(key), "%016llx", (unsigned long long)keyValue);
    if (ReadCacheManifest(cacheRoot, key, &previous))
    {
        reused = 1;
        for (index = 0; index < previous.count && reused; index++)
        {
            wchar_t *path = PathJoin(cacheRoot, previous.items[index]);

            reused = PathIsFile(path);
            MemoryRelease(path);
        }
    }
    if (reused)
    {
        for (index = 0; index < previous.count; index++)
            WideListAppend(&produced.files, WideDuplicate(previous.items[index]));
        Report(log, "cache", L"%zu loose folders and %zu packed archives unchanged, reusing %zu cached files", scan.folders.count, withCompanions.count, produced.files.count);
        goto finish;
    }
    for (index = 0; index < previous.count; index++)
    {
        wchar_t *path = PathJoin(cacheRoot, previous.items[index]);

        DeleteFileW(path);
        MemoryRelease(path);
    }
    if (!PoolStart(&pool, DefaultWorkerCount()))
    {
        Report(log, "error", L"Couldnt start worker threads for the loose files");
        goto finish;
    }
    context.pool = &pool;
    context.errors = &errors;
    context.gameRoot = gameRoot;
    {
        MergeStats stats;
        MergeJob *jobs = MemoryAllocateZero(scan.folders.count ? scan.folders.count : 1, sizeof(MergeJob));
        CompanionSource *sources = NULL;
        size_t sourceCount = 0;
        size_t sourceCapacity = 0;
        LONG64 copies = 0;
        LONG64 containers = 0;
        size_t merged = 0;

        memset(&stats, 0, sizeof(stats));
        CreateDirectoryTree(cacheRoot);
        for (index = 0; index < scan.folders.count; index++)
        {
            wchar_t *lower = LowerCopy(scan.folders.items[index].gamePath);

            jobs[index].context = &context;
            jobs[index].folder = &scan.folders.items[index];
            jobs[index].gameRoot = gameRoot;
            jobs[index].modsRoot = modsRoot;
            jobs[index].output = PathJoin(cacheRoot, lower);
            jobs[index].stats = &stats;
            MemoryRelease(lower);
            PoolSubmit(&pool, MergeTask, &jobs[index]);
        }
        PoolWaitIdle(&pool);
        for (index = 0; index < scan.folders.count; index++)
        {
            if (jobs[index].succeeded)
            {
                merged++;
                if (companionsLoaded && CompanionsFind(&companions, scan.folders.items[index].gamePath))
                    AddSource(&sources, &sourceCount, &sourceCapacity, scan.folders.items[index].gamePath, cacheRoot, jobs[index].output);
            }
            MemoryRelease(jobs[index].output);
        }
        MemoryRelease(jobs);
        for (index = 0; index < withCompanions.count; index++)
        {
            wchar_t *path = PathJoin(modsRoot, withCompanions.items[index]);
            size_t folder;
            int superseded = 0;

            for (folder = 0; folder < scan.folders.count; folder++)
                if (WideEqualInsensitive(scan.folders.items[folder].gamePath, withCompanions.items[index]))
                    superseded = 1;
            if (!superseded)
                AddSource(&sources, &sourceCount, &sourceCapacity, withCompanions.items[index], cacheRoot, path);
            MemoryRelease(path);
        }
        if (sourceCount)
            CompanionsUpdateSources(&context, &companions, sources, sourceCount, modsRoot, &copies, &containers);
        else
            MemoryRelease(sources);
        Report(log, "loose", L"merged %zu of %zu loose folders (%lld files replaced, %lld added, %lld nested archives), %lld companion copies updated in %lld containers, %.1f s",
               merged, scan.folders.count, (long long)stats.replaced, (long long)stats.added, (long long)stats.nested, (long long)copies, (long long)containers, SecondsNow() - started);
    }
    PoolStop(&pool);
    if (PathIsDirectory(cacheRoot))
        WalkFiles(cacheRoot, CollectCache, &produced);
    WriteCacheManifest(cacheRoot, key, &produced.files);
finish:
    {
        size_t line;

        AcquireSRWLockShared(&errors.lock);
        for (line = 0; line < errors.lineCount; line++)
        {
            wchar_t *text = WideDuplicateLength(errors.text + errors.lineStarts[line], errors.lineLengths[line]);

            Report(log, "error", L"%ls", text);
            MemoryRelease(text);
        }
        ReleaseSRWLockShared(&errors.lock);
    }
    result->count = produced.files.count;
    result->paths = MemoryAllocateZero(result->count ? result->count : 1, sizeof(char *));
    for (index = 0; index < produced.files.count; index++)
        result->paths[index] = WideToUtf8(produced.files.items[index]);
    WideListFree(&produced.files);
    WideListFree(&previous);
    WideListFree(&withCompanions);
    if (companionsLoaded)
        CompanionsFree(&companions);
    FreeScan(&scan);
    ErrorLogFree(&errors);
    MemoryRelease(companionsPath);
    MemoryRelease(cacheRoot);
    MemoryRelease(modsRoot);
    MemoryRelease(gameRoot);
    return 1;
}

void LooseResultFree(LooseResult *result)
{
    size_t index;

    for (index = 0; index < result->count; index++)
        MemoryRelease(result->paths[index]);
    MemoryRelease(result->paths);
    memset(result, 0, sizeof(*result));
}
