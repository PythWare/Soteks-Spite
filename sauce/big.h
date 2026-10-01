#ifndef SOTEK_BIG_H
#define SOTEK_BIG_H

#include "common.h"

#define BIG_MAGIC 0x03040506u
#define BIG_HEADER_SIZE 24u
#define BIG_RECORD_SIZE 28u
#define BIG_MANIFEST_NAME L"sotek_manifest.txt"
#define BIG_MANIFEST_NAME_LATIN1 "sotek_manifest.txt"
#define BIG_MANIFEST_MAGIC "SOTEKBIG 1"

typedef struct BigHeader
{
    uint32_t magic;
    uint32_t dataOffset;
    uint32_t declaredSize;
    uint32_t count;
    uint32_t tableOffset;
    uint32_t stringsOffset;
} BigHeader;

typedef struct BigRecord
{
    uint32_t nameOffset;
    uint32_t nameHash;
    uint32_t storedSize;
    uint32_t size;
    uint32_t offset;
    uint32_t alignment;
    uint32_t compression;
} BigRecord;

typedef struct BigView
{
    const unsigned char *data;
    size_t size;
    BigHeader header;
} BigView;

typedef struct BigEntry
{
    const unsigned char *name;
    size_t nameLength;
    uint32_t alignment;
    uint32_t compression;
    uint32_t size;
    const unsigned char *stored;
    size_t storedSize;
    const wchar_t *sourcePath;
} BigEntry;

typedef struct NameSet
{
    unsigned char **items;
    size_t *lengths;
    uint64_t *hashes;
    size_t capacity;
    size_t count;
} NameSet;

typedef struct ManifestRow
{
    uint32_t alignment;
    uint32_t compression;
    const char *fileName;
    size_t fileNameLength;
    const unsigned char *name;
    size_t nameLength;
} ManifestRow;

typedef struct Manifest
{
    ManifestRow *rows;
    size_t count;
    unsigned char *storage;
} Manifest;

int BigReadHeader(const unsigned char *data, size_t size, BigHeader *header);
int BigOpenView(BigView *view, const unsigned char *data, size_t size);
int BigGetRecord(const BigView *view, uint32_t index, BigRecord *record);
int BigEntryName(const BigView *view, const BigRecord *record, const unsigned char **name, size_t *length);
int BigEntryStored(const BigView *view, const BigRecord *record, const unsigned char **stored);
int BigEntryData(const BigView *view, const BigRecord *record, const unsigned char **data, size_t *size, unsigned char **owned);
int BigIsComplete(const unsigned char *data, size_t size);
uint32_t BigHash(const unsigned char *name, size_t length);
int BigDecompress(const unsigned char *stored, size_t storedSize, uint32_t size, unsigned char **output);
int BigCompress(const unsigned char *data, size_t size, ByteBuffer *output);
int BigBuildBuffer(const BigEntry *entries, size_t count, ByteBuffer *output);
int BigWriteFile(const wchar_t *path, const BigEntry *entries, size_t count);

void NameSetFree(NameSet *set);
void BigFileName(const unsigned char *name, size_t length, uint32_t index, NameSet *used, ByteBuffer *output);

void ManifestBegin(ByteBuffer *text);
void ManifestAppend(ByteBuffer *text, uint32_t alignment, uint32_t compression, const char *fileName, size_t fileNameLength, const unsigned char *name, size_t nameLength);
int ManifestWrite(const wchar_t *directory, const ByteBuffer *text);
int ManifestRead(const wchar_t *directory, Manifest *manifest);
void ManifestFree(Manifest *manifest);

#endif
