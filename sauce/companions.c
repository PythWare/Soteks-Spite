#include "operations.h"
#include "big.h"

#define COMPANIONS_MAGIC "SOTEKCOMPANIONS 2"
#define PROBE_BATCH 64
#define MAX_UPDATE_PASSES 64

static wchar_t *AsciiFieldToWide(const unsigned char *text, size_t length)
{
    return Latin1ToWide(text, length);
}

static CompanionGroup *FindGroupSlot(const Companions *companions, const wchar_t *archive, uint64_t hash)
{
    size_t slot;

    if (!companions->capacity)
        return NULL;
    for (slot = (size_t)hash & (companions->capacity - 1); companions->groups[slot].archive; slot = (slot + 1) & (companions->capacity - 1))
        if (companions->groups[slot].hash == hash && WideEqualInsensitive(companions->groups[slot].archive, archive))
            return &companions->groups[slot];
    return &companions->groups[slot];
}

static void GrowCompanions(Companions *companions)
{
    Companions grown;
    size_t index;

    grown.capacity = companions->capacity ? companions->capacity * 2 : 1024;
    grown.groups = MemoryAllocateZero(grown.capacity, sizeof(CompanionGroup));
    grown.count = companions->count;
    grown.records = companions->records;
    for (index = 0; index < companions->capacity; index++)
    {
        CompanionGroup *group = &companions->groups[index];
        size_t slot;

        if (!group->archive)
            continue;
        for (slot = (size_t)group->hash & (grown.capacity - 1); grown.groups[slot].archive; slot = (slot + 1) & (grown.capacity - 1))
            ;
        grown.groups[slot] = *group;
    }
    MemoryRelease(companions->groups);
    *companions = grown;
}

const CompanionGroup *CompanionsFind(const Companions *companions, const wchar_t *relative)
{
    CompanionGroup *group = FindGroupSlot(companions, relative, WideHashInsensitive(relative, wcslen(relative)));

    return group && group->archive ? group : NULL;
}

static int ParseSteps(const unsigned char *text, size_t length, CompanionRecord *record)
{
    size_t index;
    size_t capacity = 4;
    int atStepStart = 1;

    record->steps = MemoryAllocate(capacity * sizeof(uint32_t));
    record->stepCount = 0;
    for (index = 0; index < length; index++)
    {
        if (atStepStart)
        {
            uint32_t value = 0;
            int digits = 0;

            while (index < length && text[index] >= '0' && text[index] <= '9')
            {
                value = value * 10 + (uint32_t)(text[index] - '0');
                index++;
                digits++;
            }
            if (!digits || index >= length || text[index] != ':')
                return 0;
            if (record->stepCount == capacity)
            {
                capacity *= 2;
                record->steps = MemoryResize(record->steps, capacity * sizeof(uint32_t));
            }
            record->steps[record->stepCount++] = value;
            atStepStart = 0;
        }
        else if (text[index] == '/')
            atStepStart = 1;
    }
    return record->stepCount > 0;
}

int CompanionsLoad(const wchar_t *path, Companions *companions, ErrorLog *errors)
{
    ByteBuffer file = {0};
    size_t position = 0;
    size_t lineNumber = 0;

    memset(companions, 0, sizeof(*companions));
    if (!ReadWholeFile(path, &file))
    {
        ErrorLogAdd(errors, L"Couldnt read %ls", PathDisplay(path));
        ByteBufferFree(&file);
        return 0;
    }
    while (position < file.size)
    {
        unsigned char *line = file.data + position;
        unsigned char *end = memchr(line, '\n', file.size - position);
        size_t length = end ? (size_t)(end - line) : file.size - position;
        unsigned char *fields[4];
        size_t fieldLengths[4];
        size_t field = 0;
        size_t start = 0;
        size_t index;

        position += length + 1;
        lineNumber++;
        if (length && line[length - 1] == '\r')
            length--;
        if (lineNumber == 1)
        {
            if (length != sizeof(COMPANIONS_MAGIC) - 1 || memcmp(line, COMPANIONS_MAGIC, length) != 0)
            {
                ErrorLogAdd(errors, L"%ls isnt a companion list. Replace it with the vanilla_companions.txt that comes with Sotek's Spite.", PathDisplay(path));
                ByteBufferFree(&file);
                return 0;
            }
            continue;
        }
        if (lineNumber == 2 || !length)
            continue;
        for (index = 0; index <= length && field < 4; index++)
        {
            if (index == length || line[index] == '\t')
            {
                fields[field] = line + start;
                fieldLengths[field] = index - start;
                field++;
                start = index + 1;
            }
        }
        if (field == 4)
        {
            CompanionRecord record;
            wchar_t *archive = AsciiFieldToWide(fields[0], fieldLengths[0]);
            uint64_t hash = WideHashInsensitive(archive, wcslen(archive));
            CompanionGroup *group;

            memset(&record, 0, sizeof(record));
            if (!ParseSteps(fields[2], fieldLengths[2], &record))
            {
                MemoryRelease(record.steps);
                MemoryRelease(archive);
                continue;
            }
            record.container = AsciiFieldToWide(fields[1], fieldLengths[1]);
            record.fullCopy = fieldLengths[3] == 9 && memcmp(fields[3], "full-copy", 9) == 0;
            if ((companions->count + 1) * 2 > companions->capacity)
                GrowCompanions(companions);
            group = FindGroupSlot(companions, archive, hash);
            if (!group->archive)
            {
                group->archive = archive;
                group->hash = hash;
                companions->count++;
            }
            else
                MemoryRelease(archive);
            if (group->count == group->capacity)
            {
                group->capacity = group->capacity ? group->capacity * 2 : 2;
                group->records = MemoryResize(group->records, group->capacity * sizeof(CompanionRecord));
            }
            group->records[group->count++] = record;
            companions->records++;
        }
    }
    ByteBufferFree(&file);
    return 1;
}

void CompanionsFree(Companions *companions)
{
    size_t index;

    for (index = 0; index < companions->capacity; index++)
    {
        CompanionGroup *group = &companions->groups[index];
        size_t record;

        if (!group->archive)
            continue;
        for (record = 0; record < group->count; record++)
        {
            MemoryRelease(group->records[record].container);
            MemoryRelease(group->records[record].steps);
        }
        MemoryRelease(group->records);
        MemoryRelease(group->archive);
    }
    MemoryRelease(companions->groups);
    memset(companions, 0, sizeof(*companions));
}

typedef struct SourceList
{
    SRWLOCK lock;
    CompanionSource *items;
    size_t count;
    size_t capacity;
} SourceList;

typedef struct ProbeShared
{
    OperationContext *context;
    const Companions *companions;
    const wchar_t *folder;
    SourceList *sources;
    volatile LONG64 gameArchives;
    volatile LONG64 outsideLayout;
    volatile LONG64 withCompanions;
} ProbeShared;

typedef struct ProbeBatch
{
    ProbeShared *shared;
    wchar_t **relatives;
    size_t count;
} ProbeBatch;

static void SourceListAppend(SourceList *list, CompanionSource *source)
{
    AcquireSRWLockExclusive(&list->lock);
    if (list->count == list->capacity)
    {
        list->capacity = list->capacity ? list->capacity * 2 : 64;
        list->items = MemoryResize(list->items, list->capacity * sizeof(CompanionSource));
    }
    list->items[list->count++] = *source;
    ReleaseSRWLockExclusive(&list->lock);
}

static void SourceListFree(SourceList *list)
{
    size_t index;

    for (index = 0; index < list->count; index++)
    {
        MemoryRelease(list->items[index].relative);
        MemoryRelease(list->items[index].outputRoot);
        MemoryRelease(list->items[index].path);
        ByteBufferFree(&list->items[index].prefix);
    }
    MemoryRelease(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void ProbeTask(void *argument)
{
    ProbeBatch *batch = argument;
    ProbeShared *shared = batch->shared;
    ByteBuffer head = {0};
    size_t index;

    for (index = 0; index < batch->count; index++)
    {
        wchar_t *path = PathJoin(shared->folder, batch->relatives[index]);
        BigHeader header;
        wchar_t *relative;

        if (!OperationCancelled(shared->context) && ReadFilePrefix(path, BIG_HEADER_SIZE, &head) && BigReadHeader(head.data, head.size, &header))
        {
            relative = GameRelativePath(path, shared->context->gameRoot);
            if (!relative)
                InterlockedIncrement64(&shared->outsideLayout);
            else
            {
                InterlockedIncrement64(&shared->gameArchives);
                if (CompanionsFind(shared->companions, relative))
                {
                    CompanionSource source;
                    size_t pathLength = wcslen(path);
                    size_t relativeLength = wcslen(relative);

                    memset(&source, 0, sizeof(source));
                    if (ReadFilePrefix(path, header.dataOffset, &source.prefix) && source.prefix.size == header.dataOffset && pathLength > relativeLength)
                    {
                        source.relative = relative;
                        source.outputRoot = WideDuplicateLength(path, pathLength - relativeLength - 1);
                        source.path = path;
                        path = NULL;
                        relative = NULL;
                        SourceListAppend(shared->sources, &source);
                        InterlockedIncrement64(&shared->withCompanions);
                    }
                    else
                    {
                        ErrorLogAdd(shared->context->errors, L"Couldnt read the header of %ls", PathDisplay(path));
                        ByteBufferFree(&source.prefix);
                    }
                }
                MemoryRelease(relative);
            }
        }
        MemoryRelease(path);
        MemoryRelease(batch->relatives[index]);
        InterlockedIncrement64(&shared->context->progress.done);
    }
    ByteBufferFree(&head);
    MemoryRelease(batch->relatives);
    MemoryRelease(batch);
}

typedef struct FileListCollector
{
    WideList files;
} FileListCollector;

static int CollectAnyFile(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data)
{
    FileListCollector *collector = context;

    if (!(data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (data->nFileSizeHigh || data->nFileSizeLow >= BIG_HEADER_SIZE))
        WideListAppend(&collector->files, WideDuplicate(relativePath));
    return WALK_CONTINUE;
}

typedef struct DecodedLevel
{
    uint32_t *steps;
    uint32_t stepCount;
    const unsigned char *data;
    size_t size;
    unsigned char *owned;
} DecodedLevel;

typedef struct NestedCache
{
    const unsigned char *root;
    size_t rootSize;
    DecodedLevel *levels;
    size_t count;
    size_t capacity;
} NestedCache;

static int StepsEqual(const uint32_t *left, const uint32_t *right, uint32_t count)
{
    return memcmp(left, right, count * sizeof(uint32_t)) == 0;
}

static int NestedGet(NestedCache *cache, const uint32_t *steps, uint32_t stepCount, const unsigned char **data, size_t *size)
{
    const unsigned char *current = cache->root;
    size_t currentSize = cache->rootSize;
    uint32_t depth;

    for (depth = 1; depth <= stepCount; depth++)
    {
        size_t index;
        int found = 0;
        BigView view;
        BigRecord record;
        const unsigned char *entry;
        size_t entrySize;
        unsigned char *owned;

        for (index = 0; index < cache->count; index++)
        {
            if (cache->levels[index].stepCount == depth && StepsEqual(cache->levels[index].steps, steps, depth))
            {
                current = cache->levels[index].data;
                currentSize = cache->levels[index].size;
                found = 1;
                break;
            }
        }
        if (found)
            continue;
        if (!BigOpenView(&view, current, currentSize) || !BigGetRecord(&view, steps[depth - 1], &record) || !BigEntryData(&view, &record, &entry, &entrySize, &owned))
            return 0;
        if (cache->count == cache->capacity)
        {
            cache->capacity = cache->capacity ? cache->capacity * 2 : 16;
            cache->levels = MemoryResize(cache->levels, cache->capacity * sizeof(DecodedLevel));
        }
        cache->levels[cache->count].steps = MemoryAllocate(depth * sizeof(uint32_t));
        memcpy(cache->levels[cache->count].steps, steps, depth * sizeof(uint32_t));
        cache->levels[cache->count].stepCount = depth;
        cache->levels[cache->count].data = entry;
        cache->levels[cache->count].size = entrySize;
        cache->levels[cache->count].owned = owned;
        cache->count++;
        current = entry;
        currentSize = entrySize;
    }
    *data = current;
    *size = currentSize;
    return 1;
}

static void NestedCacheFree(NestedCache *cache)
{
    size_t index;

    for (index = cache->count; index > 0; index--)
    {
        MemoryRelease(cache->levels[index - 1].steps);
        MemoryRelease(cache->levels[index - 1].owned);
    }
    MemoryRelease(cache->levels);
    memset(cache, 0, sizeof(*cache));
}

typedef struct Replacement
{
    const uint32_t *steps;
    uint32_t stepCount;
    const unsigned char *payload;
    size_t payloadSize;
} Replacement;

typedef struct IndexedReplacement
{
    uint32_t index;
    const Replacement *replacement;
} IndexedReplacement;

static int CompareIndexed(const void *left, const void *right)
{
    const IndexedReplacement *a = left;
    const IndexedReplacement *b = right;

    if (a->index != b->index)
        return a->index < b->index ? -1 : 1;
    return a->replacement < b->replacement ? -1 : (a->replacement > b->replacement ? 1 : 0);
}

static int ApplyReplacements(const unsigned char *data, size_t size, const Replacement *const *items, size_t count, uint32_t depth, ByteBuffer *output)
{
    BigView view;
    BigEntry *entries;
    ByteBuffer *ownedStored;
    IndexedReplacement *ordered;
    uint32_t index;
    size_t cursor;
    int succeeded = 1;

    if (!BigOpenView(&view, data, size))
        return 0;
    entries = MemoryAllocateZero(view.header.count ? view.header.count : 1, sizeof(BigEntry));
    ownedStored = MemoryAllocateZero(view.header.count ? view.header.count : 1, sizeof(ByteBuffer));
    for (index = 0; index < view.header.count && succeeded; index++)
    {
        BigRecord record;
        const unsigned char *name;
        size_t nameLength;
        const unsigned char *stored;

        if (!BigGetRecord(&view, index, &record) || !BigEntryName(&view, &record, &name, &nameLength) || !BigEntryStored(&view, &record, &stored))
        {
            succeeded = 0;
            break;
        }
        entries[index].name = name;
        entries[index].nameLength = nameLength;
        entries[index].alignment = record.alignment;
        entries[index].compression = record.compression;
        entries[index].size = record.size;
        entries[index].stored = stored;
        entries[index].storedSize = record.storedSize;
    }
    ordered = MemoryAllocate((count ? count : 1) * sizeof(IndexedReplacement));
    for (cursor = 0; cursor < count; cursor++)
    {
        ordered[cursor].index = items[cursor]->steps[depth];
        ordered[cursor].replacement = items[cursor];
    }
    qsort(ordered, count, sizeof(IndexedReplacement), CompareIndexed);
    for (cursor = 0; cursor < count && succeeded;)
    {
        uint32_t target = ordered[cursor].index;
        size_t groupStart = cursor;
        const Replacement *direct = NULL;
        size_t deeperCount = 0;
        const Replacement **deeper;
        ByteBuffer newData = {0};
        const unsigned char *payload;
        size_t payloadSize;

        while (cursor < count && ordered[cursor].index == target)
        {
            if (ordered[cursor].replacement->stepCount == depth + 1)
                direct = ordered[cursor].replacement;
            else
                deeperCount++;
            cursor++;
        }
        if (target >= view.header.count)
        {
            succeeded = 0;
            break;
        }
        if (direct)
        {
            payload = direct->payload;
            payloadSize = direct->payloadSize;
        }
        else
        {
            const unsigned char *inner;
            size_t innerSize;
            unsigned char *innerOwned = NULL;
            BigRecord record;
            size_t scan;
            size_t used = 0;

            deeper = MemoryAllocate(deeperCount * sizeof(Replacement *));
            for (scan = groupStart; scan < cursor; scan++)
                deeper[used++] = ordered[scan].replacement;
            if (!BigGetRecord(&view, target, &record) || !BigEntryData(&view, &record, &inner, &innerSize, &innerOwned) || !ApplyReplacements(inner, innerSize, deeper, deeperCount, depth + 1, &newData))
                succeeded = 0;
            MemoryRelease(innerOwned);
            MemoryRelease(deeper);
            payload = newData.data;
            payloadSize = newData.size;
        }
        if (succeeded)
        {
            if (entries[target].compression == 1)
            {
                if (!BigCompress(payload, payloadSize, &ownedStored[target]))
                    succeeded = 0;
                entries[target].stored = ownedStored[target].data;
                entries[target].storedSize = ownedStored[target].size;
            }
            else
            {
                ByteBufferAppend(&ownedStored[target], payload, payloadSize);
                entries[target].stored = ownedStored[target].data;
                entries[target].storedSize = ownedStored[target].size;
            }
            entries[target].size = (uint32_t)payloadSize;
        }
        ByteBufferFree(&newData);
    }
    if (succeeded)
        succeeded = BigBuildBuffer(entries, view.header.count, output);
    for (index = 0; index < view.header.count; index++)
        ByteBufferFree(&ownedStored[index]);
    MemoryRelease(ordered);
    MemoryRelease(ownedStored);
    MemoryRelease(entries);
    return succeeded;
}

typedef struct GroupItem
{
    const CompanionRecord *record;
    const CompanionSource *source;
} GroupItem;

typedef struct ContainerGroup
{
    const wchar_t *modsRoot;
    const wchar_t *container;
    GroupItem *items;
    size_t count;
    size_t capacity;
} ContainerGroup;

static ContainerGroup *FindContainerGroup(ContainerGroup **groups, size_t *count, size_t *capacity, const wchar_t *modsRoot, const wchar_t *container)
{
    size_t index;

    for (index = 0; index < *count; index++)
        if (WideEqualInsensitive((*groups)[index].modsRoot, modsRoot) && WideEqualInsensitive((*groups)[index].container, container))
            return &(*groups)[index];
    if (*count == *capacity)
    {
        *capacity = *capacity ? *capacity * 2 : 16;
        *groups = MemoryResize(*groups, *capacity * sizeof(ContainerGroup));
    }
    memset(&(*groups)[*count], 0, sizeof(ContainerGroup));
    (*groups)[*count].modsRoot = modsRoot;
    (*groups)[*count].container = container;
    return &(*groups)[(*count)++];
}

static int UpdateContainer(OperationContext *context, ContainerGroup *group, SourceList *next, LONG64 *copiesUpdated, const wchar_t *overlayRoot)
{
    wchar_t *outputPath = PathJoin(group->modsRoot, group->container);
    wchar_t *vanillaPath = PathJoin(context->gameRoot, group->container);
    wchar_t *overlayPath = overlayRoot ? PathJoin(overlayRoot, group->container) : NULL;
    const wchar_t *inputPath = PathIsFile(outputPath) ? outputPath : (overlayPath && PathIsFile(overlayPath) ? overlayPath : vanillaPath);
    ByteBuffer blob = {0};
    ByteBuffer rebuilt = {0};
    NestedCache cache;
    Replacement *replacements = MemoryAllocateZero(group->count, sizeof(Replacement));
    const Replacement **stale = MemoryAllocate(group->count * sizeof(Replacement *));
    ByteBuffer *payloadBuffers = MemoryAllocateZero(group->count, sizeof(ByteBuffer));
    size_t staleCount = 0;
    size_t index;
    int succeeded = 1;

    memset(&cache, 0, sizeof(cache));
    if (!ReadWholeFile(inputPath, &blob))
    {
        ErrorLogAdd(context->errors, L"Couldnt read companion container %ls", group->container);
        succeeded = 0;
        goto done;
    }
    cache.root = blob.data;
    cache.rootSize = blob.size;
    for (index = 0; index < group->count; index++)
    {
        const GroupItem *item = &group->items[index];
        const unsigned char *payload;
        size_t payloadSize;
        const unsigned char *current;
        size_t currentSize;

        if (item->record->fullCopy)
        {
            if (!ReadWholeFile(item->source->path, &payloadBuffers[index]))
            {
                ErrorLogAdd(context->errors, L"Couldnt read %ls", PathDisplay(item->source->path));
                continue;
            }
            payload = payloadBuffers[index].data;
            payloadSize = payloadBuffers[index].size;
        }
        else
        {
            payload = item->source->prefix.data;
            payloadSize = item->source->prefix.size;
        }
        if (!NestedGet(&cache, item->record->steps, item->record->stepCount, &current, &currentSize))
        {
            ErrorLogAdd(context->errors, L"The companion entry for %ls in %ls is missing or damaged", item->source->relative, group->container);
            continue;
        }
        if (currentSize == payloadSize && memcmp(current, payload, payloadSize) == 0)
            continue;
        replacements[index].steps = item->record->steps;
        replacements[index].stepCount = item->record->stepCount;
        replacements[index].payload = payload;
        replacements[index].payloadSize = payloadSize;
        stale[staleCount++] = &replacements[index];
    }
    if (!staleCount)
        goto done;
    if (!ApplyReplacements(blob.data, blob.size, stale, staleCount, 0, &rebuilt))
    {
        ErrorLogAdd(context->errors, L"Couldnt rebuild companion container %ls", group->container);
        succeeded = 0;
        goto done;
    }
    if (!CreateParentDirectories(outputPath) || !WriteWholeFile(outputPath, rebuilt.data, rebuilt.size))
    {
        ErrorLogAdd(context->errors, L"Couldntt write %ls", PathDisplay(outputPath));
        succeeded = 0;
        goto done;
    }
    *copiesUpdated += (LONG64)staleCount;
    {
        CompanionSource source;
        BigHeader header;

        memset(&source, 0, sizeof(source));
        BigReadHeader(rebuilt.data, rebuilt.size, &header);
        source.relative = WideDuplicate(group->container);
        source.outputRoot = WideDuplicate(group->modsRoot);
        source.path = outputPath;
        outputPath = NULL;
        ByteBufferAppend(&source.prefix, rebuilt.data, header.dataOffset <= rebuilt.size ? header.dataOffset : rebuilt.size);
        SourceListAppend(next, &source);
    }
done:
    for (index = 0; index < group->count; index++)
        ByteBufferFree(&payloadBuffers[index]);
    MemoryRelease(payloadBuffers);
    MemoryRelease(stale);
    MemoryRelease(replacements);
    NestedCacheFree(&cache);
    ByteBufferFree(&blob);
    ByteBufferFree(&rebuilt);
    MemoryRelease(outputPath);
    MemoryRelease(vanillaPath);
    MemoryRelease(overlayPath);
    return succeeded;
}

static void RunUpdatePasses(OperationContext *context, const Companions *companions, SourceList *pending, const wchar_t *overlayRoot, LONG64 *copiesUpdated, LONG64 *containersWritten)
{
    size_t index;
    int pass;

    for (pass = 0; pass < MAX_UPDATE_PASSES && pending->count && !OperationCancelled(context); pass++)
    {
        ContainerGroup *groups = NULL;
        size_t groupCount = 0;
        size_t groupCapacity = 0;
        SourceList next;
        size_t source;

        memset(&next, 0, sizeof(next));
        InitializeSRWLock(&next.lock);
        for (source = 0; source < pending->count; source++)
        {
            const CompanionGroup *companion = CompanionsFind(companions, pending->items[source].relative);
            size_t record;

            if (!companion)
                continue;
            for (record = 0; record < companion->count; record++)
            {
                ContainerGroup *group = FindContainerGroup(&groups, &groupCount, &groupCapacity, pending->items[source].outputRoot, companion->records[record].container);

                if (group->count == group->capacity)
                {
                    group->capacity = group->capacity ? group->capacity * 2 : 8;
                    group->items = MemoryResize(group->items, group->capacity * sizeof(GroupItem));
                }
                group->items[group->count].record = &companion->records[record];
                group->items[group->count].source = &pending->items[source];
                group->count++;
            }
        }
        InterlockedExchange64(&context->progress.total, (LONG64)groupCount);
        for (index = 0; index < groupCount && !OperationCancelled(context); index++)
        {
            LONG64 before = *copiesUpdated;

            UpdateContainer(context, &groups[index], &next, copiesUpdated, overlayRoot);
            if (*copiesUpdated != before)
                (*containersWritten)++;
            InterlockedIncrement64(&context->progress.done);
        }
        for (index = 0; index < groupCount; index++)
            MemoryRelease(groups[index].items);
        MemoryRelease(groups);
        SourceListFree(pending);
        *pending = next;
    }
    SourceListFree(pending);
}

int CompanionsUpdateSources(OperationContext *context, const Companions *companions, CompanionSource *sources, size_t count, const wchar_t *overlayRoot, LONG64 *copiesUpdated, LONG64 *containersWritten)
{
    SourceList pending;

    memset(&pending, 0, sizeof(pending));
    InitializeSRWLock(&pending.lock);
    pending.items = sources;
    pending.count = count;
    pending.capacity = count;
    *copiesUpdated = 0;
    *containersWritten = 0;
    ProgressReset(context, STAGE_UPDATING);
    RunUpdatePasses(context, companions, &pending, overlayRoot, copiesUpdated, containersWritten);
    InterlockedExchange(&context->progress.stage, STAGE_IDLE);
    return !OperationCancelled(context);
}

int BatchUpdate(OperationContext *context, const wchar_t *folder, const Companions *companions, BatchSummary *summary)
{
    FileListCollector collector;
    ProbeShared shared;
    SourceList pending;
    double started = SecondsNow();
    size_t index;

    memset(summary, 0, sizeof(*summary));
    memset(&collector, 0, sizeof(collector));
    memset(&shared, 0, sizeof(shared));
    memset(&pending, 0, sizeof(pending));
    InitializeSRWLock(&pending.lock);
    ProgressReset(context, STAGE_SCANNING);
    if (!PathIsDirectory(folder))
    {
        ErrorLogAdd(context->errors, L"The folder %ls doesnnt exist.", PathDisplay(folder));
        return 0;
    }
    WalkFiles(folder, CollectAnyFile, &collector);
    InterlockedExchange64(&context->progress.total, (LONG64)collector.files.count);
    shared.context = context;
    shared.companions = companions;
    shared.folder = folder;
    shared.sources = &pending;
    for (index = 0; index < collector.files.count; index += PROBE_BATCH)
    {
        ProbeBatch *batch = MemoryAllocate(sizeof(ProbeBatch));
        size_t take = collector.files.count - index < PROBE_BATCH ? collector.files.count - index : PROBE_BATCH;

        batch->shared = &shared;
        batch->count = take;
        batch->relatives = MemoryAllocate(take * sizeof(wchar_t *));
        memcpy(batch->relatives, collector.files.items + index, take * sizeof(wchar_t *));
        PoolSubmit(context->pool, ProbeTask, batch);
    }
    PoolWaitIdle(context->pool);
    MemoryRelease(collector.files.items);
    summary->filesScanned = (LONG64)collector.files.count;
    summary->gameArchives = shared.gameArchives;
    summary->outsideLayout = shared.outsideLayout;
    summary->withCompanions = shared.withCompanions;
    ProgressReset(context, STAGE_UPDATING);
    RunUpdatePasses(context, companions, &pending, NULL, &summary->copiesUpdated, &summary->containersWritten);
    summary->seconds = SecondsNow() - started;
    InterlockedExchange(&context->progress.stage, STAGE_IDLE);
    return !OperationCancelled(context);
}
