#include "operations.h"
#include "big.h"

#define REBUILD_CHUNK_BYTES (16ull << 20)

typedef struct RebuildRun
{
    OperationContext *context;
    volatile LONG64 archivesWritten;
    volatile LONG64 entries;
    volatile LONG64 added;
    volatile LONG64 removed;
} RebuildRun;

typedef struct BuildSpec
{
    unsigned char *name;
    size_t nameLength;
    uint32_t alignment;
    uint32_t compression;
    wchar_t *path;
    uint64_t fileSize;
    ByteBuffer stored;
    uint32_t size;
} BuildSpec;

typedef struct BuildPlan
{
    RebuildRun *run;
    wchar_t *folder;
    wchar_t *target;
    BuildSpec *specs;
    size_t specCount;
    volatile LONG pending;
    volatile LONG failed;
} BuildPlan;

typedef struct CompressChunk
{
    BuildPlan *plan;
    size_t first;
    size_t last;
} CompressChunk;

typedef struct NestedFolder
{
    int depth;
    wchar_t *folder;
    wchar_t *entryPath;
} NestedFolder;

typedef struct NestedList
{
    NestedFolder *items;
    size_t count;
    size_t capacity;
} NestedList;

typedef struct ExtraCollector
{
    const WideSet *listed;
    WideList files;
} ExtraCollector;

static void ReleasePlanContents(BuildPlan *plan)
{
    size_t index;

    for (index = 0; index < plan->specCount; index++)
    {
        MemoryRelease(plan->specs[index].name);
        MemoryRelease(plan->specs[index].path);
        ByteBufferFree(&plan->specs[index].stored);
    }
    MemoryRelease(plan->specs);
    plan->specs = NULL;
    plan->specCount = 0;
}

static void AssemblePlan(BuildPlan *plan)
{
    RebuildRun *run = plan->run;
    BigEntry *entries;
    size_t index;
    int succeeded = !plan->failed;

    entries = MemoryAllocateZero(plan->specCount ? plan->specCount : 1, sizeof(BigEntry));
    for (index = 0; index < plan->specCount && succeeded; index++)
    {
        BuildSpec *spec = &plan->specs[index];

        entries[index].name = spec->name;
        entries[index].nameLength = spec->nameLength;
        entries[index].alignment = spec->alignment;
        entries[index].compression = spec->compression;
        if (spec->compression == 1)
        {
            entries[index].stored = spec->stored.data;
            entries[index].storedSize = spec->stored.size;
            entries[index].size = spec->size;
        }
        else
        {
            if (spec->fileSize > 0xFFFFFFFFull)
            {
                ErrorLogAdd(run->context->errors, L"%ls is larger than 4 GB and cant be stored in a BIG archive", PathDisplay(spec->path));
                succeeded = 0;
            }
            entries[index].sourcePath = spec->path;
            entries[index].storedSize = (size_t)spec->fileSize;
            entries[index].size = (uint32_t)spec->fileSize;
        }
    }
    if (succeeded && !OperationCancelled(run->context))
    {
        if (!CreateParentDirectories(plan->target) || !BigWriteFile(plan->target, entries, plan->specCount))
            ErrorLogAdd(run->context->errors, L"Couldnt write %ls", PathDisplay(plan->target));
        else
        {
            InterlockedIncrement64(&run->archivesWritten);
            InterlockedAdd64(&run->entries, (LONG64)plan->specCount);
            InterlockedIncrement64(&run->context->progress.files);
        }
    }
    MemoryRelease(entries);
    ReleasePlanContents(plan);
    InterlockedIncrement64(&run->context->progress.done);
}

static void AssembleTask(void *argument)
{
    AssemblePlan(argument);
}

static void CompressTask(void *argument)
{
    CompressChunk *chunk = argument;
    BuildPlan *plan = chunk->plan;
    RebuildRun *run = plan->run;
    ByteBuffer data = {0};
    size_t index;

    for (index = chunk->first; index < chunk->last && !plan->failed && !OperationCancelled(run->context); index++)
    {
        BuildSpec *spec = &plan->specs[index];

        if (spec->compression != 1)
            continue;
        if (!ReadWholeFile(spec->path, &data))
        {
            ErrorLogAdd(run->context->errors, L"Couldnt read %ls", PathDisplay(spec->path));
            InterlockedExchange(&plan->failed, 1);
            break;
        }
        if (data.size > 0xFFFFFFFFull || !BigCompress(data.data, data.size, &spec->stored))
        {
            ErrorLogAdd(run->context->errors, L"Couldntt compress %ls", PathDisplay(spec->path));
            InterlockedExchange(&plan->failed, 1);
            break;
        }
        spec->size = (uint32_t)data.size;
    }
    ByteBufferFree(&data);
    MemoryRelease(chunk);
    if (InterlockedDecrement(&plan->pending) == 0)
        AssemblePlan(plan);
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

static int SameExtension(const unsigned char *left, size_t leftLength, const unsigned char *right, size_t rightLength)
{
    size_t index;

    if (leftLength != rightLength)
        return 0;
    for (index = 0; index < leftLength; index++)
        if (Latin1Lower(left[index]) != Latin1Lower(right[index]))
            return 0;
    return 1;
}

static uint32_t GuessAlignment(const BuildSpec *specs, size_t manifestCount, const unsigned char *name, size_t nameLength)
{
    size_t extensionLength;
    const unsigned char *extension = ExtensionOf(name, nameLength, &extensionLength);
    uint32_t best = 4;
    size_t bestVotes = 0;
    size_t index;

    for (index = 0; index < manifestCount; index++)
    {
        size_t otherLength;
        const unsigned char *other = ExtensionOf(specs[index].name, specs[index].nameLength, &otherLength);
        size_t votes = 0;
        size_t scan;

        if (!SameExtension(extension, extensionLength, other, otherLength))
            continue;
        for (scan = 0; scan < manifestCount; scan++)
        {
            size_t scanLength;
            const unsigned char *scanExtension = ExtensionOf(specs[scan].name, specs[scan].nameLength, &scanLength);

            if (specs[scan].alignment == specs[index].alignment && SameExtension(extension, extensionLength, scanExtension, scanLength))
                votes++;
        }
        if (votes > bestVotes)
        {
            bestVotes = votes;
            best = specs[index].alignment;
        }
    }
    return best;
}

static int CollectExtra(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data)
{
    ExtraCollector *collector = context;

    if (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        if (WideEndsWithInsensitive(relativePath, L".unpacked"))
        {
            size_t length = wcslen(relativePath) - 9;
            wchar_t *owner = WideDuplicateLength(relativePath, length);
            int skip = !wcschr(relativePath, L'\\') || WideSetContains(collector->listed, owner);

            MemoryRelease(owner);
            if (skip)
                return WALK_SKIP;
        }
        return WALK_CONTINUE;
    }
    if (WideEqualInsensitive(relativePath, BIG_MANIFEST_NAME) || WideSetContains(collector->listed, relativePath))
        return WALK_CONTINUE;
    WideListAppend(&collector->files, WideDuplicate(relativePath));
    return WALK_CONTINUE;
}

static int PlanFolder(BuildPlan *plan)
{
    RebuildRun *run = plan->run;
    Manifest manifest;
    WideSet listed = {0};
    ExtraCollector collector;
    size_t manifestCount = 0;
    size_t index;
    size_t capacity;

    if (!ManifestRead(plan->folder, &manifest))
    {
        ErrorLogAdd(run->context->errors, L"Couldnt read the manifest in %ls", PathDisplay(plan->folder));
        return 0;
    }
    capacity = manifest.count + 16;
    plan->specs = MemoryAllocateZero(capacity, sizeof(BuildSpec));
    for (index = 0; index < manifest.count; index++)
    {
        ManifestRow *row = &manifest.rows[index];
        wchar_t *wideName = Latin1ToWide((const unsigned char *)row->fileName, row->fileNameLength);
        wchar_t *path = PathJoin(plan->folder, wideName);
        uint64_t size;

        WideSetInsert(&listed, wideName);
        MemoryRelease(wideName);
        if (!FileSize(path, &size) || PathIsDirectory(path))
        {
            MemoryRelease(path);
            InterlockedIncrement64(&run->removed);
            continue;
        }
        plan->specs[plan->specCount].name = MemoryAllocate(row->nameLength + 1);
        memcpy(plan->specs[plan->specCount].name, row->name, row->nameLength);
        plan->specs[plan->specCount].nameLength = row->nameLength;
        plan->specs[plan->specCount].alignment = row->alignment;
        plan->specs[plan->specCount].compression = row->compression;
        plan->specs[plan->specCount].path = path;
        plan->specs[plan->specCount].fileSize = size;
        plan->specCount++;
    }
    manifestCount = plan->specCount;
    ManifestFree(&manifest);
    memset(&collector, 0, sizeof(collector));
    collector.listed = &listed;
    WalkFiles(plan->folder, CollectExtra, &collector);
    for (index = 0; index < collector.files.count; index++)
    {
        ByteBuffer name = {0};
        wchar_t *path;
        uint64_t size;

        if (!WideToLatin1(collector.files.items[index], &name))
        {
            ErrorLogAdd(run->context->errors, L"Skipped %ls, its name cant be stored in a BIG archive", collector.files.items[index]);
            ByteBufferFree(&name);
            continue;
        }
        path = PathJoin(plan->folder, collector.files.items[index]);
        if (!FileSize(path, &size))
        {
            MemoryRelease(path);
            ByteBufferFree(&name);
            continue;
        }
        if (plan->specCount == capacity)
        {
            size_t grown = capacity * 2;

            plan->specs = MemoryResize(plan->specs, grown * sizeof(BuildSpec));
            memset(plan->specs + capacity, 0, (grown - capacity) * sizeof(BuildSpec));
            capacity = grown;
        }
        plan->specs[plan->specCount].alignment = GuessAlignment(plan->specs, manifestCount, name.data, name.size);
        plan->specs[plan->specCount].name = name.data;
        plan->specs[plan->specCount].nameLength = name.size;
        plan->specs[plan->specCount].compression = 0;
        plan->specs[plan->specCount].path = path;
        plan->specs[plan->specCount].fileSize = size;
        plan->specCount++;
        InterlockedIncrement64(&run->added);
    }
    WideListFree(&collector.files);
    WideSetFree(&listed);
    return 1;
}

static void BuildFolders(RebuildRun *run, wchar_t **folders, wchar_t **targets, size_t count)
{
    BuildPlan *plans = MemoryAllocateZero(count ? count : 1, sizeof(BuildPlan));
    size_t index;

    for (index = 0; index < count && !OperationCancelled(run->context); index++)
    {
        BuildPlan *plan = &plans[index];
        size_t spec;
        size_t first = 0;
        uint64_t bytes = 0;
        size_t chunks = 0;

        plan->run = run;
        plan->folder = folders[index];
        plan->target = targets[index];
        if (!PlanFolder(plan))
        {
            InterlockedIncrement64(&run->context->progress.done);
            continue;
        }
        for (spec = 0; spec < plan->specCount; spec++)
        {
            if (plan->specs[spec].compression != 1)
                continue;
            bytes += plan->specs[spec].fileSize;
            if (bytes >= REBUILD_CHUNK_BYTES)
            {
                chunks++;
                bytes = 0;
            }
        }
        if (bytes)
            chunks++;
        if (!chunks)
        {
            PoolSubmit(run->context->pool, AssembleTask, plan);
            continue;
        }
        plan->pending = (LONG)chunks;
        bytes = 0;
        for (spec = 0; spec < plan->specCount; spec++)
        {
            if (plan->specs[spec].compression != 1)
                continue;
            bytes += plan->specs[spec].fileSize;
            if (bytes >= REBUILD_CHUNK_BYTES)
            {
                CompressChunk *chunk = MemoryAllocate(sizeof(CompressChunk));

                chunk->plan = plan;
                chunk->first = first;
                chunk->last = spec + 1;
                first = spec + 1;
                bytes = 0;
                PoolSubmit(run->context->pool, CompressTask, chunk);
            }
        }
        if (bytes)
        {
            CompressChunk *chunk = MemoryAllocate(sizeof(CompressChunk));

            chunk->plan = plan;
            chunk->first = first;
            chunk->last = plan->specCount;
            PoolSubmit(run->context->pool, CompressTask, chunk);
        }
    }
    PoolWaitIdle(run->context->pool);
    for (index = 0; index < count; index++)
        ReleasePlanContents(&plans[index]);
    MemoryRelease(plans);
}

typedef struct TopCollector
{
    const wchar_t *root;
    WideList folders;
} TopCollector;

static int CollectTop(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data)
{
    TopCollector *collector = context;
    wchar_t *folder;
    wchar_t *manifest;
    int isArchive;

    if (!(data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return WALK_CONTINUE;
    folder = PathJoin(collector->root, relativePath);
    manifest = PathJoin(folder, BIG_MANIFEST_NAME);
    isArchive = PathIsFile(manifest);
    MemoryRelease(manifest);
    if (!isArchive)
    {
        MemoryRelease(folder);
        return WALK_CONTINUE;
    }
    WideListAppend(&collector->folders, folder);
    return WALK_SKIP;
}

static void NestedAppend(NestedList *list, int depth, wchar_t *folder, wchar_t *entryPath)
{
    if (list->count == list->capacity)
    {
        list->capacity = list->capacity ? list->capacity * 2 : 64;
        list->items = MemoryResize(list->items, list->capacity * sizeof(NestedFolder));
    }
    list->items[list->count].depth = depth;
    list->items[list->count].folder = folder;
    list->items[list->count].entryPath = entryPath;
    list->count++;
}

static void CollectNested(const wchar_t *folder, int depth, NestedList *list)
{
    Manifest manifest;
    size_t index;

    if (!ManifestRead(folder, &manifest))
        return;
    for (index = 0; index < manifest.count; index++)
    {
        wchar_t *wideName = Latin1ToWide((const unsigned char *)manifest.rows[index].fileName, manifest.rows[index].fileNameLength);
        wchar_t *entryPath = PathJoin(folder, wideName);
        wchar_t *nested = WideFormat(L"%ls.unpacked", entryPath);
        wchar_t *nestedManifest = PathJoin(nested, BIG_MANIFEST_NAME);

        MemoryRelease(wideName);
        if (PathIsFile(nestedManifest))
        {
            NestedAppend(list, depth, nested, entryPath);
            CollectNested(nested, depth + 1, list);
        }
        else
        {
            MemoryRelease(nested);
            MemoryRelease(entryPath);
        }
        MemoryRelease(nestedManifest);
    }
    ManifestFree(&manifest);
}

typedef struct NewestCollector
{
    uint64_t newest;
} NewestCollector;

static int CollectNewest(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data)
{
    NewestCollector *collector = context;
    uint64_t time = ((uint64_t)data->ftLastWriteTime.dwHighDateTime << 32) | data->ftLastWriteTime.dwLowDateTime;

    (void)relativePath;
    if (time > collector->newest)
        collector->newest = time;
    return WALK_CONTINUE;
}

static uint64_t NewestTime(const wchar_t *folder)
{
    NewestCollector collector = {0};

    FileWriteTime(folder, &collector.newest);
    WalkFiles(folder, CollectNewest, &collector);
    return collector.newest;
}

int RebuildFolder(OperationContext *context, const wchar_t *inputRoot, const wchar_t *outputRoot, RebuildSummary *summary)
{
    RebuildRun run;
    TopCollector tops;
    NestedList nested = {0};
    WideSet dirtyParents = {0};
    double started = SecondsNow();
    wchar_t *rootManifest;
    int maxDepth = 0;
    int depth;
    size_t index;

    memset(summary, 0, sizeof(*summary));
    memset(&run, 0, sizeof(run));
    memset(&tops, 0, sizeof(tops));
    run.context = context;
    ProgressReset(context, STAGE_SCANNING);
    if (!PathIsDirectory(inputRoot))
    {
        ErrorLogAdd(context->errors, L"The folder %ls doesnt exist.", PathDisplay(inputRoot));
        return 0;
    }
    tops.root = inputRoot;
    rootManifest = PathJoin(inputRoot, BIG_MANIFEST_NAME);
    if (PathIsFile(rootManifest))
        WideListAppend(&tops.folders, WideDuplicate(inputRoot));
    else
        WalkFiles(inputRoot, CollectTop, &tops);
    MemoryRelease(rootManifest);
    for (index = 0; index < tops.folders.count; index++)
        CollectNested(tops.folders.items[index], 1, &nested);
    for (index = 0; index < nested.count; index++)
        if (nested.items[index].depth > maxDepth)
            maxDepth = nested.items[index].depth;
    for (depth = maxDepth; depth >= 1 && !OperationCancelled(context); depth--)
    {
        wchar_t **folders = MemoryAllocateZero(nested.count ? nested.count : 1, sizeof(wchar_t *));
        wchar_t **targets = MemoryAllocateZero(nested.count ? nested.count : 1, sizeof(wchar_t *));
        size_t dirty = 0;

        for (index = 0; index < nested.count; index++)
        {
            NestedFolder *item = &nested.items[index];
            uint64_t entryTime = 0;

            if (item->depth != depth)
                continue;
            if (WideSetContains(&dirtyParents, item->folder) || !FileWriteTime(item->entryPath, &entryTime) || PathIsDirectory(item->entryPath) || NewestTime(item->folder) > entryTime)
            {
                folders[dirty] = item->folder;
                targets[dirty] = item->entryPath;
                dirty++;
            }
        }
        if (dirty)
        {
            ProgressReset(context, STAGE_REBUILDING_NESTED);
            InterlockedExchange64(&context->progress.total, (LONG64)dirty);
            BuildFolders(&run, folders, targets, dirty);
            summary->nestedRebuilt += (LONG64)dirty;
            for (index = 0; index < dirty; index++)
            {
                wchar_t *parent = PathParent(targets[index]);

                WideSetInsert(&dirtyParents, parent);
                MemoryRelease(parent);
            }
        }
        MemoryRelease(folders);
        MemoryRelease(targets);
    }
    if (!OperationCancelled(context))
    {
        wchar_t **targets = MemoryAllocateZero(tops.folders.count ? tops.folders.count : 1, sizeof(wchar_t *));
        LONG64 nestedWritten = run.archivesWritten;
        LONG64 nestedEntries = run.entries;

        for (index = 0; index < tops.folders.count; index++)
        {
            const wchar_t *folder = tops.folders.items[index];
            wchar_t *relative = GameRelativePath(folder, context->gameRoot);

            if (!relative)
            {
                size_t rootLength = wcslen(inputRoot);

                if (wcslen(folder) > rootLength && folder[rootLength] == L'\\')
                    relative = WideDuplicate(folder + rootLength + 1);
                else
                    relative = WideDuplicate(PathName(folder));
            }
            targets[index] = PathJoin(outputRoot, relative);
            MemoryRelease(relative);
        }
        ProgressReset(context, STAGE_REBUILDING);
        InterlockedExchange64(&context->progress.total, (LONG64)tops.folders.count);
        BuildFolders(&run, tops.folders.items, targets, tops.folders.count);
        summary->archivesWritten = run.archivesWritten - nestedWritten;
        summary->entries = run.entries - nestedEntries;
        for (index = 0; index < tops.folders.count; index++)
            MemoryRelease(targets[index]);
        MemoryRelease(targets);
    }
    summary->archives = (LONG64)tops.folders.count;
    summary->added = run.added;
    summary->removed = run.removed;
    summary->seconds = SecondsNow() - started;
    for (index = 0; index < nested.count; index++)
    {
        MemoryRelease(nested.items[index].folder);
        MemoryRelease(nested.items[index].entryPath);
    }
    MemoryRelease(nested.items);
    WideSetFree(&dirtyParents);
    WideListFree(&tops.folders);
    InterlockedExchange(&context->progress.stage, STAGE_IDLE);
    return !OperationCancelled(context);
}
