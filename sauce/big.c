#include "big.h"
#include <zlib.h>

static uint32_t ReadLittle32(const unsigned char *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void WriteLittle32(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8);
    data[2] = (unsigned char)(value >> 16);
    data[3] = (unsigned char)(value >> 24);
}

int BigReadHeader(const unsigned char *data, size_t size, BigHeader *header)
{
    if (size < BIG_HEADER_SIZE)
        return 0;
    header->magic = ReadLittle32(data);
    header->dataOffset = ReadLittle32(data + 4);
    header->declaredSize = ReadLittle32(data + 8);
    header->count = ReadLittle32(data + 12);
    header->tableOffset = ReadLittle32(data + 16);
    header->stringsOffset = ReadLittle32(data + 20);
    return header->magic == BIG_MAGIC;
}

int BigOpenView(BigView *view, const unsigned char *data, size_t size)
{
    memset(view, 0, sizeof(*view));
    if (!BigReadHeader(data, size, &view->header))
        return 0;
    if ((uint64_t)view->header.tableOffset + (uint64_t)view->header.count * BIG_RECORD_SIZE > (uint64_t)size)
        return 0;
    view->data = data;
    view->size = size;
    return 1;
}

int BigGetRecord(const BigView *view, uint32_t index, BigRecord *record)
{
    const unsigned char *cursor;

    if (index >= view->header.count)
        return 0;
    cursor = view->data + view->header.tableOffset + (size_t)index * BIG_RECORD_SIZE;
    record->nameOffset = ReadLittle32(cursor);
    record->nameHash = ReadLittle32(cursor + 4);
    record->storedSize = ReadLittle32(cursor + 8);
    record->size = ReadLittle32(cursor + 12);
    record->offset = ReadLittle32(cursor + 16);
    record->alignment = ReadLittle32(cursor + 20);
    record->compression = ReadLittle32(cursor + 24);
    return 1;
}

int BigEntryName(const BigView *view, const BigRecord *record, const unsigned char **name, size_t *length)
{
    const unsigned char *end;

    if (record->nameOffset >= view->size)
        return 0;
    end = memchr(view->data + record->nameOffset, 0, view->size - record->nameOffset);
    if (!end)
        return 0;
    *name = view->data + record->nameOffset;
    *length = (size_t)(end - *name);
    return 1;
}

int BigEntryStored(const BigView *view, const BigRecord *record, const unsigned char **stored)
{
    if ((uint64_t)record->offset + record->storedSize > (uint64_t)view->size)
        return 0;
    *stored = view->data + record->offset;
    return 1;
}

int BigEntryData(const BigView *view, const BigRecord *record, const unsigned char **data, size_t *size, unsigned char **owned)
{
    const unsigned char *stored;

    *owned = NULL;
    if (!BigEntryStored(view, record, &stored))
        return 0;
    if (record->compression == 0)
    {
        *data = stored;
        *size = record->storedSize;
        return 1;
    }
    if (record->compression != 1 || !BigDecompress(stored, record->storedSize, record->size, owned))
        return 0;
    *data = *owned;
    *size = record->size;
    return 1;
}

int BigIsComplete(const unsigned char *data, size_t size)
{
    BigHeader header;

    if (!BigReadHeader(data, size, &header))
        return 0;
    return header.declaredSize == size && header.dataOffset < size && header.count > 0;
}

uint32_t BigHash(const unsigned char *name, size_t length)
{
    uint32_t value = 0;
    size_t index;

    for (index = 0; index < length; index++)
    {
        unsigned char character = name[index];

        if (character >= 'A' && character <= 'Z')
            character = (unsigned char)(character + 32);
        value = value * 0x1003Fu + character;
    }
    return value;
}

int BigDecompress(const unsigned char *stored, size_t storedSize, uint32_t size, unsigned char **output)
{
    z_stream stream;
    unsigned char *buffer;
    int status;

    *output = NULL;
    if (storedSize < 4 || storedSize - 4 > 0xFFFFFFFFu)
        return 0;
    buffer = MemoryAllocate(size ? size : 1);
    memset(&stream, 0, sizeof(stream));
    if (inflateInit(&stream) != Z_OK)
    {
        MemoryRelease(buffer);
        return 0;
    }
    stream.next_in = (Bytef *)(stored + 4);
    stream.avail_in = (uInt)(storedSize - 4);
    stream.next_out = buffer;
    stream.avail_out = size;
    status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (status != Z_STREAM_END || stream.total_out != size)
    {
        MemoryRelease(buffer);
        return 0;
    }
    *output = buffer;
    return 1;
}

int BigCompress(const unsigned char *data, size_t size, ByteBuffer *output)
{
    z_stream stream;
    uLong bound;
    int status;

    output->size = 0;
    if (size > 0xFFFFFFFFu)
        return 0;
    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, 9, Z_DEFLATED, 12, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0;
    bound = deflateBound(&stream, (uLong)size);
    ByteBufferReserve(output, (size_t)bound + 4);
    output->data[0] = (unsigned char)(size >> 24);
    output->data[1] = (unsigned char)(size >> 16);
    output->data[2] = (unsigned char)(size >> 8);
    output->data[3] = (unsigned char)size;
    stream.next_in = (Bytef *)data;
    stream.avail_in = (uInt)size;
    stream.next_out = output->data + 4;
    stream.avail_out = (uInt)bound;
    status = deflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END)
    {
        deflateEnd(&stream);
        return 0;
    }
    output->size = 4 + stream.total_out;
    deflateEnd(&stream);
    return 1;
}

typedef struct BigLayout
{
    uint32_t dataOffset;
    uint32_t total;
    uint32_t *offsets;
    uint32_t *nameOffsets;
} BigLayout;

static int ComputeLayout(const BigEntry *entries, size_t count, BigLayout *layout)
{
    uint64_t position;
    size_t index;

    memset(layout, 0, sizeof(*layout));
    if (count > 0xFFFFFFFFu / BIG_RECORD_SIZE)
        return 0;
    layout->offsets = MemoryAllocate(count * sizeof(uint32_t));
    layout->nameOffsets = MemoryAllocate(count * sizeof(uint32_t));
    position = BIG_HEADER_SIZE + (uint64_t)count * BIG_RECORD_SIZE;
    for (index = 0; index < count; index++)
    {
        layout->nameOffsets[index] = (uint32_t)position;
        position += entries[index].nameLength + 1;
        if (position > 0xFFFFFFFFull)
            goto overflow;
    }
    layout->dataOffset = (uint32_t)position;
    for (index = 0; index < count; index++)
    {
        uint32_t alignment = entries[index].alignment;

        if (alignment > 1)
            position = (position + alignment - 1) / alignment * alignment;
        layout->offsets[index] = (uint32_t)position;
        position += entries[index].storedSize;
        if (position > 0xFFFFFFFFull)
            goto overflow;
    }
    layout->total = (uint32_t)position;
    return 1;
overflow:
    MemoryRelease(layout->offsets);
    MemoryRelease(layout->nameOffsets);
    memset(layout, 0, sizeof(*layout));
    return 0;
}

static void FreeLayout(BigLayout *layout)
{
    MemoryRelease(layout->offsets);
    MemoryRelease(layout->nameOffsets);
}

static void WriteHeaderAndTable(unsigned char *target, const BigEntry *entries, size_t count, const BigLayout *layout)
{
    size_t index;

    WriteLittle32(target, BIG_MAGIC);
    WriteLittle32(target + 4, layout->dataOffset);
    WriteLittle32(target + 8, layout->total);
    WriteLittle32(target + 12, (uint32_t)count);
    WriteLittle32(target + 16, BIG_HEADER_SIZE);
    WriteLittle32(target + 20, BIG_HEADER_SIZE + (uint32_t)count * BIG_RECORD_SIZE);
    for (index = 0; index < count; index++)
    {
        unsigned char *record = target + BIG_HEADER_SIZE + index * BIG_RECORD_SIZE;

        WriteLittle32(record, layout->nameOffsets[index]);
        WriteLittle32(record + 4, BigHash(entries[index].name, entries[index].nameLength));
        WriteLittle32(record + 8, (uint32_t)entries[index].storedSize);
        WriteLittle32(record + 12, entries[index].size);
        WriteLittle32(record + 16, layout->offsets[index]);
        WriteLittle32(record + 20, entries[index].alignment);
        WriteLittle32(record + 24, entries[index].compression);
        memcpy(target + layout->nameOffsets[index], entries[index].name, entries[index].nameLength);
        target[layout->nameOffsets[index] + entries[index].nameLength] = 0;
    }
}

static int ReadSourceInto(const wchar_t *path, unsigned char *target, size_t size)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    size_t total = 0;
    DWORD extra = 0;
    unsigned char probe;

    if (file == INVALID_HANDLE_VALUE)
        return 0;
    while (total < size)
    {
        size_t remaining = size - total;
        DWORD chunk = (DWORD)(remaining > (1u << 30) ? (1u << 30) : remaining);
        DWORD got = 0;

        if (!ReadFile(file, target + total, chunk, &got, NULL) || !got)
        {
            CloseHandle(file);
            return 0;
        }
        total += got;
    }
    ReadFile(file, &probe, 1, &extra, NULL);
    CloseHandle(file);
    return extra == 0;
}

int BigBuildBuffer(const BigEntry *entries, size_t count, ByteBuffer *output)
{
    BigLayout layout;
    size_t index;

    output->size = 0;
    if (!ComputeLayout(entries, count, &layout))
        return 0;
    ByteBufferReserve(output, layout.total);
    memset(output->data, 0, layout.total);
    WriteHeaderAndTable(output->data, entries, count, &layout);
    for (index = 0; index < count; index++)
    {
        if (entries[index].stored)
            memcpy(output->data + layout.offsets[index], entries[index].stored, entries[index].storedSize);
        else if (entries[index].sourcePath && !ReadSourceInto(entries[index].sourcePath, output->data + layout.offsets[index], entries[index].storedSize))
        {
            FreeLayout(&layout);
            return 0;
        }
    }
    output->size = layout.total;
    FreeLayout(&layout);
    return 1;
}

typedef struct FileWriter
{
    HANDLE file;
    unsigned char *buffer;
    size_t used;
    size_t capacity;
    int failed;
} FileWriter;

static void WriterFlush(FileWriter *writer)
{
    size_t total = 0;

    while (!writer->failed && total < writer->used)
    {
        DWORD written = 0;
        DWORD chunk = (DWORD)(writer->used - total);

        if (!WriteFile(writer->file, writer->buffer + total, chunk, &written, NULL) || !written)
            writer->failed = 1;
        total += written;
    }
    writer->used = 0;
}

static void WriterPut(FileWriter *writer, const unsigned char *data, size_t size)
{
    while (size && !writer->failed)
    {
        size_t room = writer->capacity - writer->used;
        size_t chunk = size < room ? size : room;

        if (data)
            memcpy(writer->buffer + writer->used, data, chunk);
        else
            memset(writer->buffer + writer->used, 0, chunk);
        writer->used += chunk;
        if (data)
            data += chunk;
        size -= chunk;
        if (writer->used == writer->capacity)
            WriterFlush(writer);
    }
}

static int WriterCopyFile(FileWriter *writer, const wchar_t *path, size_t size)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    size_t total = 0;

    if (file == INVALID_HANDLE_VALUE)
        return 0;
    while (total < size && !writer->failed)
    {
        size_t room;
        DWORD chunk;
        DWORD got = 0;

        if (writer->used == writer->capacity)
            WriterFlush(writer);
        room = writer->capacity - writer->used;
        chunk = (DWORD)((size - total) < room ? (size - total) : room);
        if (!ReadFile(file, writer->buffer + writer->used, chunk, &got, NULL) || !got)
        {
            CloseHandle(file);
            return 0;
        }
        writer->used += got;
        total += got;
    }
    CloseHandle(file);
    return total == size;
}

int BigWriteFile(const wchar_t *path, const BigEntry *entries, size_t count)
{
    BigLayout layout;
    FileWriter writer;
    unsigned char *head;
    size_t index;
    uint64_t position;
    int succeeded = 1;

    if (!ComputeLayout(entries, count, &layout))
        return 0;
    memset(&writer, 0, sizeof(writer));
    writer.file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (writer.file == INVALID_HANDLE_VALUE)
    {
        FreeLayout(&layout);
        return 0;
    }
    writer.capacity = 4u << 20;
    writer.buffer = MemoryAllocate(writer.capacity);
    head = MemoryAllocateZero(layout.dataOffset, 1);
    WriteHeaderAndTable(head, entries, count, &layout);
    WriterPut(&writer, head, layout.dataOffset);
    MemoryRelease(head);
    position = layout.dataOffset;
    for (index = 0; index < count && succeeded && !writer.failed; index++)
    {
        WriterPut(&writer, NULL, (size_t)(layout.offsets[index] - position));
        if (entries[index].stored)
            WriterPut(&writer, entries[index].stored, entries[index].storedSize);
        else if (entries[index].sourcePath)
            succeeded = WriterCopyFile(&writer, entries[index].sourcePath, entries[index].storedSize);
        position = (uint64_t)layout.offsets[index] + entries[index].storedSize;
    }
    WriterFlush(&writer);
    if (!CloseHandle(writer.file))
        succeeded = 0;
    if (writer.failed || !succeeded)
    {
        DeleteFileW(path);
        succeeded = 0;
    }
    MemoryRelease(writer.buffer);
    FreeLayout(&layout);
    return succeeded;
}

static uint64_t LowerNameHash(const unsigned char *text, size_t length)
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

static int LowerNameEqual(const unsigned char *left, size_t leftLength, const unsigned char *right, size_t rightLength)
{
    size_t index;

    if (leftLength != rightLength)
        return 0;
    for (index = 0; index < leftLength; index++)
        if (Latin1Lower(left[index]) != Latin1Lower(right[index]))
            return 0;
    return 1;
}

static int NameSetContains(const NameSet *set, const unsigned char *text, size_t length)
{
    uint64_t hash;
    size_t slot;

    if (LowerNameEqual(text, length, (const unsigned char *)BIG_MANIFEST_NAME_LATIN1, sizeof(BIG_MANIFEST_NAME_LATIN1) - 1))
        return 1;
    if (!set->capacity)
        return 0;
    hash = LowerNameHash(text, length);
    for (slot = (size_t)hash & (set->capacity - 1); set->items[slot]; slot = (slot + 1) & (set->capacity - 1))
        if (set->hashes[slot] == hash && LowerNameEqual(set->items[slot], set->lengths[slot], text, length))
            return 1;
    return 0;
}

static void NameSetInsert(NameSet *set, const unsigned char *text, size_t length)
{
    uint64_t hash;
    size_t slot;

    if ((set->count + 1) * 2 > set->capacity)
    {
        NameSet grown;
        size_t index;

        grown.capacity = set->capacity ? set->capacity * 2 : 64;
        grown.count = 0;
        grown.items = MemoryAllocateZero(grown.capacity, sizeof(unsigned char *));
        grown.lengths = MemoryAllocateZero(grown.capacity, sizeof(size_t));
        grown.hashes = MemoryAllocateZero(grown.capacity, sizeof(uint64_t));
        for (index = 0; index < set->capacity; index++)
        {
            if (!set->items[index])
                continue;
            for (slot = (size_t)set->hashes[index] & (grown.capacity - 1); grown.items[slot]; slot = (slot + 1) & (grown.capacity - 1))
                ;
            grown.items[slot] = set->items[index];
            grown.lengths[slot] = set->lengths[index];
            grown.hashes[slot] = set->hashes[index];
            grown.count++;
        }
        MemoryRelease(set->items);
        MemoryRelease(set->lengths);
        MemoryRelease(set->hashes);
        *set = grown;
    }
    hash = LowerNameHash(text, length);
    for (slot = (size_t)hash & (set->capacity - 1); set->items[slot]; slot = (slot + 1) & (set->capacity - 1))
        ;
    set->items[slot] = MemoryAllocate(length + 1);
    memcpy(set->items[slot], text, length);
    set->items[slot][length] = 0;
    set->lengths[slot] = length;
    set->hashes[slot] = hash;
    set->count++;
}

void NameSetFree(NameSet *set)
{
    size_t index;

    for (index = 0; index < set->capacity; index++)
        MemoryRelease(set->items[index]);
    MemoryRelease(set->items);
    MemoryRelease(set->lengths);
    MemoryRelease(set->hashes);
    memset(set, 0, sizeof(*set));
}

static int IsReservedCharacter(unsigned char character)
{
    return character < 0x20 || character == '<' || character == '>' || character == ':' || character == '"' || character == '|' || character == '?' || character == '*';
}

void BigFileName(const unsigned char *name, size_t length, uint32_t index, NameSet *used, ByteBuffer *output)
{
    ByteBuffer candidate = {0};
    size_t start = 0;
    size_t cursor;
    size_t baseStart;
    size_t extension;
    unsigned number = 2;

    output->size = 0;
    if (!length)
    {
        char text[32];

        snprintf(text, sizeof(text), "unnamed_%u", index);
        name = (const unsigned char *)text;
        length = strlen(text);
        ByteBufferAppend(&candidate, name, length);
    }
    else
    {
        for (cursor = 0; cursor <= length; cursor++)
        {
            if (cursor == length || name[cursor] == '/' || name[cursor] == '\\')
            {
                size_t partStart = candidate.size;
                size_t position;

                for (position = start; position < cursor; position++)
                    ByteBufferAppendByte(&candidate, IsReservedCharacter(name[position]) ? '_' : name[position]);
                while (candidate.size > partStart && (candidate.data[candidate.size - 1] == ' ' || candidate.data[candidate.size - 1] == '.'))
                    candidate.size--;
                if (candidate.size == partStart)
                    ByteBufferAppendByte(&candidate, '_');
                if (cursor < length)
                    ByteBufferAppendByte(&candidate, '\\');
                start = cursor + 1;
            }
        }
    }
    baseStart = 0;
    for (cursor = 0; cursor < candidate.size; cursor++)
        if (candidate.data[cursor] == '\\')
            baseStart = cursor + 1;
    extension = candidate.size;
    cursor = baseStart;
    while (cursor < candidate.size && candidate.data[cursor] == '.')
        cursor++;
    for (; cursor < candidate.size; cursor++)
        if (candidate.data[cursor] == '.')
            extension = cursor;
    ByteBufferAppend(output, candidate.data, candidate.size);
    while (NameSetContains(used, output->data, output->size))
    {
        char suffix[24];

        output->size = 0;
        ByteBufferAppend(output, candidate.data, extension);
        snprintf(suffix, sizeof(suffix), "~%u", number++);
        ByteBufferAppendText(output, suffix);
        ByteBufferAppend(output, candidate.data + extension, candidate.size - extension);
    }
    NameSetInsert(used, output->data, output->size);
    ByteBufferAppendByte(output, 0);
    output->size--;
    ByteBufferFree(&candidate);
}

void ManifestBegin(ByteBuffer *text)
{
    text->size = 0;
    ByteBufferAppendText(text, BIG_MANIFEST_MAGIC "\n");
}

void ManifestAppend(ByteBuffer *text, uint32_t alignment, uint32_t compression, const char *fileName, size_t fileNameLength, const unsigned char *name, size_t nameLength)
{
    char numbers[40];

    snprintf(numbers, sizeof(numbers), "%x\t%u\t", alignment, compression);
    ByteBufferAppendText(text, numbers);
    ByteBufferAppend(text, fileName, fileNameLength);
    ByteBufferAppendByte(text, '\t');
    ByteBufferAppend(text, name, nameLength);
    ByteBufferAppendByte(text, '\n');
}

int ManifestWrite(const wchar_t *directory, const ByteBuffer *text)
{
    wchar_t *path = PathJoin(directory, BIG_MANIFEST_NAME);
    int written = WriteWholeFile(path, text->data, text->size);

    MemoryRelease(path);
    return written;
}

int ManifestRead(const wchar_t *directory, Manifest *manifest)
{
    wchar_t *path = PathJoin(directory, BIG_MANIFEST_NAME);
    ByteBuffer file = {0};
    size_t position;
    size_t capacity = 0;
    size_t magicLength = sizeof(BIG_MANIFEST_MAGIC) - 1;

    memset(manifest, 0, sizeof(*manifest));
    if (!ReadWholeFile(path, &file))
    {
        MemoryRelease(path);
        ByteBufferFree(&file);
        return 0;
    }
    MemoryRelease(path);
    ByteBufferAppendByte(&file, 0);
    if (file.size < magicLength + 1 || memcmp(file.data, BIG_MANIFEST_MAGIC, magicLength) != 0 || (file.data[magicLength] != '\n' && file.data[magicLength] != '\r'))
    {
        ByteBufferFree(&file);
        return 0;
    }
    manifest->storage = file.data;
    position = magicLength;
    while (position < file.size - 1 && file.data[position] != '\n')
        position++;
    position++;
    while (position < file.size - 1)
    {
        unsigned char *line = file.data + position;
        unsigned char *end = memchr(line, '\n', file.size - 1 - position);
        unsigned char *fields[4];
        size_t lineLength = end ? (size_t)(end - line) : file.size - 1 - position;
        size_t field = 1;
        size_t index;
        ManifestRow *row;

        position += lineLength + 1;
        if (lineLength && line[lineLength - 1] == '\r')
            lineLength--;
        line[lineLength] = 0;
        if (!lineLength)
            continue;
        fields[0] = line;
        for (index = 0; index < lineLength && field < 4; index++)
        {
            if (line[index] == '\t')
            {
                line[index] = 0;
                fields[field++] = line + index + 1;
            }
        }
        if (field < 4)
        {
            ManifestFree(manifest);
            return 0;
        }
        if (manifest->count == capacity)
        {
            capacity = capacity ? capacity * 2 : 64;
            manifest->rows = MemoryResize(manifest->rows, capacity * sizeof(ManifestRow));
        }
        row = &manifest->rows[manifest->count++];
        row->alignment = (uint32_t)strtoul((const char *)fields[0], NULL, 16);
        row->compression = (uint32_t)strtoul((const char *)fields[1], NULL, 10);
        row->fileName = (const char *)fields[2];
        row->fileNameLength = strlen((const char *)fields[2]);
        row->name = fields[3];
        row->nameLength = (size_t)(line + lineLength - fields[3]);
    }
    return 1;
}

void ManifestFree(Manifest *manifest)
{
    MemoryRelease(manifest->rows);
    MemoryRelease(manifest->storage);
    memset(manifest, 0, sizeof(*manifest));
}
