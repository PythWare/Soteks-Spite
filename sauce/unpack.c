#include "operations.h"
#include "big.h"

#define UNPACK_CHUNK_BYTES (32ull << 20)
#define UNPACK_FULL_DEPTH 8

typedef struct UnpackRun
{
    OperationContext *context;
    const wchar_t *sourceRoot;
    const wchar_t *outputRoot;
    int maxDepth;
    volatile LONG64 archives;
    volatile LONG64 entries;
} UnpackRun;

typedef struct UnpackArchive
{
    UnpackRun *run;
    MappedFile file;
    BigView view;
    wchar_t *sourcePath;
    volatile LONG references;
} UnpackArchive;

typedef struct UnpackItem
{
    BigRecord record;
    wchar_t *path;
} UnpackItem;

typedef struct UnpackChunk
{
    UnpackArchive *archive;
    UnpackItem *items;
    size_t count;
    size_t capacity;
} UnpackChunk;

typedef struct UnpackPlanJob
{
    UnpackRun *run;
    wchar_t *relative;
} UnpackPlanJob;

static void ArchiveRelease(UnpackArchive *archive)
{
    if (InterlockedDecrement(&archive->references) != 0)
        return;
    InterlockedIncrement64(&archive->run->context->progress.done);
    MappedFileClose(&archive->file);
    MemoryRelease(archive->sourcePath);
    MemoryRelease(archive);
}

static void UnpackTree(UnpackRun *run, const unsigned char *data, size_t size, const wchar_t *directory, int level, const wchar_t *origin)
{
    BigView view;
    NameSet used = {0};
    ByteBuffer manifest = {0};
    ByteBuffer fileName = {0};
    uint32_t index;

    if (!BigOpenView(&view, data, size))
    {
        ErrorLogAdd(run->context->errors, L"Couldnt read nested archive %ls", PathDisplay(origin));
        return;
    }
    if (!CreateDirectoryTree(directory))
    {
        ErrorLogAdd(run->context->errors, L"Couldnt create folder %ls", PathDisplay(directory));
        return;
    }
    ManifestBegin(&manifest);
    for (index = 0; index < view.header.count && !OperationCancelled(run->context); index++)
    {
        BigRecord record;
        const unsigned char *name;
        size_t nameLength;
        const unsigned char *entry;
        size_t entrySize;
        unsigned char *owned;
        wchar_t *wideName;
        wchar_t *path;

        if (!BigGetRecord(&view, index, &record) || !BigEntryName(&view, &record, &name, &nameLength))
        {
            ErrorLogAdd(run->context->errors, L"Damaged entry %u in %ls", index, PathDisplay(origin));
            continue;
        }
        BigFileName(name, nameLength, index, &used, &fileName);
        ManifestAppend(&manifest, record.alignment, record.compression, (const char *)fileName.data, fileName.size, name, nameLength);
        wideName = Latin1ToWide(fileName.data, fileName.size);
        path = PathJoin(directory, wideName);
        if (wcschr(wideName, L'\\'))
            CreateParentDirectories(path);
        if (!BigEntryData(&view, &record, &entry, &entrySize, &owned))
            ErrorLogAdd(run->context->errors, L"Couldnt extract %ls from %ls", wideName, PathDisplay(origin));
        else
        {
            if (WriteWholeFile(path, entry, entrySize))
                InterlockedIncrement64(&run->context->progress.files);
            else
                ErrorLogAdd(run->context->errors, L"Couldnt write %ls", PathDisplay(path));
            if (level < run->maxDepth && BigIsComplete(entry, entrySize))
            {
                wchar_t *nested = WideFormat(L"%ls.unpacked", path);

                UnpackTree(run, entry, entrySize, nested, level + 1, path);
                MemoryRelease(nested);
            }
            MemoryRelease(owned);
        }
        MemoryRelease(path);
        MemoryRelease(wideName);
    }
    if (!ManifestWrite(directory, &manifest))
        ErrorLogAdd(run->context->errors, L"Couldnt write the manifest in %ls", PathDisplay(directory));
    ByteBufferFree(&manifest);
    ByteBufferFree(&fileName);
    NameSetFree(&used);
}

static void UnpackChunkTask(void *argument)
{
    UnpackChunk *chunk = argument;
    UnpackArchive *archive = chunk->archive;
    UnpackRun *run = archive->run;
    size_t index;

    for (index = 0; index < chunk->count; index++)
    {
        UnpackItem *item = &chunk->items[index];
        const unsigned char *entry;
        size_t entrySize;
        unsigned char *owned;

        if (!OperationCancelled(run->context))
        {
            if (!BigEntryData(&archive->view, &item->record, &entry, &entrySize, &owned))
                ErrorLogAdd(run->context->errors, L"Couldnt extract %ls from %ls", PathName(item->path), PathDisplay(archive->sourcePath));
            else
            {
                if (WriteWholeFile(item->path, entry, entrySize))
                    InterlockedIncrement64(&run->context->progress.files);
                else
                    ErrorLogAdd(run->context->errors, L"Couldnt write %ls", PathDisplay(item->path));
                if (run->maxDepth > 0 && BigIsComplete(entry, entrySize))
                {
                    wchar_t *nested = WideFormat(L"%ls.unpacked", item->path);

                    UnpackTree(run, entry, entrySize, nested, 1, item->path);
                    MemoryRelease(nested);
                }
                MemoryRelease(owned);
            }
        }
        MemoryRelease(item->path);
    }
    MemoryRelease(chunk->items);
    MemoryRelease(chunk);
    ArchiveRelease(archive);
}

static UnpackChunk *NewChunk(UnpackArchive *archive)
{
    UnpackChunk *chunk = MemoryAllocateZero(1, sizeof(UnpackChunk));

    chunk->archive = archive;
    return chunk;
}

static void SubmitChunk(UnpackArchive *archive, UnpackChunk *chunk)
{
    InterlockedIncrement(&archive->references);
    PoolSubmit(archive->run->context->pool, UnpackChunkTask, chunk);
}

static void UnpackPlanTask(void *argument)
{
    UnpackPlanJob *job = argument;
    UnpackRun *run = job->run;
    ByteBuffer head = {0};
    ByteBuffer manifest = {0};
    ByteBuffer fileName = {0};
    NameSet used = {0};
    UnpackArchive *archive = NULL;
    UnpackChunk *chunk = NULL;
    uint64_t chunkBytes = 0;
    wchar_t *sourcePath = PathJoin(run->sourceRoot, job->relative);
    wchar_t *directory = NULL;
    BigHeader header;
    uint32_t index;

    if (OperationCancelled(run->context) || !ReadFilePrefix(sourcePath, BIG_HEADER_SIZE, &head) || !BigReadHeader(head.data, head.size, &header))
    {
        InterlockedDecrement64(&run->context->progress.total);
        goto done;
    }
    InterlockedIncrement64(&run->archives);
    archive = MemoryAllocateZero(1, sizeof(UnpackArchive));
    archive->run = run;
    archive->references = 1;
    archive->sourcePath = sourcePath;
    sourcePath = NULL;
    if (!MappedFileOpen(&archive->file, archive->sourcePath) || !BigOpenView(&archive->view, archive->file.view, archive->file.size))
    {
        ErrorLogAdd(run->context->errors, L"Couldnt open archive %ls", PathDisplay(archive->sourcePath));
        goto done;
    }
    directory = PathJoin(run->outputRoot, job->relative);
    if (!CreateDirectoryTree(directory))
    {
        ErrorLogAdd(run->context->errors, L"Couldnt create folder %ls", PathDisplay(directory));
        goto done;
    }
    ManifestBegin(&manifest);
    chunk = NewChunk(archive);
    for (index = 0; index < archive->view.header.count; index++)
    {
        BigRecord record;
        const unsigned char *name;
        size_t nameLength;
        wchar_t *wideName;
        UnpackItem *item;

        if (!BigGetRecord(&archive->view, index, &record) || !BigEntryName(&archive->view, &record, &name, &nameLength))
        {
            ErrorLogAdd(run->context->errors, L"Damaged entry %u in %ls", index, PathDisplay(archive->sourcePath));
            continue;
        }
        BigFileName(name, nameLength, index, &used, &fileName);
        ManifestAppend(&manifest, record.alignment, record.compression, (const char *)fileName.data, fileName.size, name, nameLength);
        if (chunk->count == chunk->capacity)
        {
            chunk->capacity = chunk->capacity ? chunk->capacity * 2 : 64;
            chunk->items = MemoryResize(chunk->items, chunk->capacity * sizeof(UnpackItem));
        }
        wideName = Latin1ToWide(fileName.data, fileName.size);
        item = &chunk->items[chunk->count++];
        item->record = record;
        item->path = PathJoin(directory, wideName);
        if (wcschr(wideName, L'\\'))
            CreateParentDirectories(item->path);
        MemoryRelease(wideName);
        InterlockedIncrement64(&run->entries);
        chunkBytes += record.storedSize > record.size ? record.storedSize : record.size;
        if (chunkBytes >= UNPACK_CHUNK_BYTES)
        {
            SubmitChunk(archive, chunk);
            chunk = NewChunk(archive);
            chunkBytes = 0;
        }
    }
    if (!ManifestWrite(directory, &manifest))
        ErrorLogAdd(run->context->errors, L"Couldnt write the manifest in %ls", PathDisplay(directory));
    if (chunk->count)
        SubmitChunk(archive, chunk);
    else
    {
        MemoryRelease(chunk->items);
        MemoryRelease(chunk);
    }
    chunk = NULL;
done:
    if (archive)
        ArchiveRelease(archive);
    MemoryRelease(sourcePath);
    MemoryRelease(directory);
    ByteBufferFree(&head);
    ByteBufferFree(&manifest);
    ByteBufferFree(&fileName);
    NameSetFree(&used);
    MemoryRelease(job->relative);
    MemoryRelease(job);
}

typedef struct FileCollector
{
    WideList files;
} FileCollector;

static int CollectFile(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data)
{
    FileCollector *collector = context;

    if (!(data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (data->nFileSizeHigh || data->nFileSizeLow >= BIG_HEADER_SIZE))
        WideListAppend(&collector->files, WideDuplicate(relativePath));
    return WALK_CONTINUE;
}

int UnpackFolder(OperationContext *context, const wchar_t *sourceRoot, const wchar_t *outputRoot, int mode, UnpackSummary *summary)
{
    UnpackRun run;
    FileCollector collector;
    double started = SecondsNow();
    size_t index;

    memset(summary, 0, sizeof(*summary));
    memset(&run, 0, sizeof(run));
    memset(&collector, 0, sizeof(collector));
    run.context = context;
    run.sourceRoot = sourceRoot;
    run.outputRoot = outputRoot;
    run.maxDepth = mode <= 0 ? 0 : (mode == 1 ? 1 : UNPACK_FULL_DEPTH);
    ProgressReset(context, STAGE_SCANNING);
    if (!PathIsDirectory(sourceRoot))
    {
        ErrorLogAdd(context->errors, L"The folder %ls doesnt exist.", PathDisplay(sourceRoot));
        return 0;
    }
    WalkFiles(sourceRoot, CollectFile, &collector);
    InterlockedExchange64(&context->progress.total, (LONG64)collector.files.count);
    InterlockedExchange(&context->progress.stage, STAGE_UNPACKING);
    for (index = 0; index < collector.files.count; index++)
    {
        UnpackPlanJob *job = MemoryAllocate(sizeof(UnpackPlanJob));

        job->run = &run;
        job->relative = collector.files.items[index];
        collector.files.items[index] = NULL;
        PoolSubmit(context->pool, UnpackPlanTask, job);
    }
    PoolWaitIdle(context->pool);
    WideListFree(&collector.files);
    summary->archives = run.archives;
    summary->entries = run.entries;
    summary->files = context->progress.files;
    summary->seconds = SecondsNow() - started;
    InterlockedExchange(&context->progress.stage, STAGE_IDLE);
    return !OperationCancelled(context);
}
