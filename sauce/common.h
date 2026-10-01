#ifndef SOTEK_COMMON_H
#define SOTEK_COMMON_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum WalkResult
{
    WALK_STOP = 0,
    WALK_CONTINUE = 1,
    WALK_SKIP = 2
};

typedef struct ByteBuffer
{
    unsigned char *data;
    size_t size;
    size_t capacity;
} ByteBuffer;

typedef struct WideList
{
    wchar_t **items;
    size_t count;
    size_t capacity;
} WideList;

typedef struct MappedFile
{
    HANDLE file;
    HANDLE mapping;
    const unsigned char *view;
    size_t size;
} MappedFile;

typedef struct ErrorLog
{
    SRWLOCK lock;
    wchar_t *text;
    size_t length;
    size_t capacity;
    size_t *lineStarts;
    size_t *lineLengths;
    size_t lineCount;
    size_t lineCapacity;
    volatile LONG version;
} ErrorLog;

typedef int (*FileVisitor)(void *context, const wchar_t *relativePath, const WIN32_FIND_DATAW *data);

void *MemoryAllocate(size_t size);
void *MemoryAllocateZero(size_t count, size_t size);
void *MemoryResize(void *block, size_t size);
void MemoryRelease(void *block);
LONG64 MemoryOutstanding(void);

void ByteBufferReserve(ByteBuffer *buffer, size_t capacity);
void ByteBufferAppend(ByteBuffer *buffer, const void *data, size_t size);
void ByteBufferAppendByte(ByteBuffer *buffer, unsigned char value);
void ByteBufferAppendZeros(ByteBuffer *buffer, size_t count);
void ByteBufferAppendText(ByteBuffer *buffer, const char *text);
void ByteBufferFree(ByteBuffer *buffer);

wchar_t *WideDuplicate(const wchar_t *text);
wchar_t *WideDuplicateLength(const wchar_t *text, size_t length);
wchar_t *WideFormat(const wchar_t *format, ...);
wchar_t *WideFormatList(const wchar_t *format, va_list arguments);
wchar_t WideLower(wchar_t character);
int WideEqualInsensitive(const wchar_t *left, const wchar_t *right);
int WideEndsWithInsensitive(const wchar_t *text, const wchar_t *suffix);
uint64_t WideHashInsensitive(const wchar_t *text, size_t length);

void WideListAppend(WideList *list, wchar_t *owned);
void WideListFree(WideList *list);

wchar_t *Latin1ToWide(const unsigned char *text, size_t length);
int WideToLatin1(const wchar_t *text, ByteBuffer *output);
unsigned char Latin1Lower(unsigned char character);

wchar_t *PathJoin(const wchar_t *left, const wchar_t *right);
wchar_t *PathParent(const wchar_t *path);
const wchar_t *PathName(const wchar_t *path);
wchar_t *PathAbsolute(const wchar_t *path);
const wchar_t *PathDisplay(const wchar_t *path);
int PathIsFile(const wchar_t *path);
int PathIsDirectory(const wchar_t *path);
int CreateDirectoryTree(const wchar_t *path);
int CreateParentDirectories(const wchar_t *path);
wchar_t *ExecutableDirectory(void);

int ReadWholeFile(const wchar_t *path, ByteBuffer *output);
int ReadFilePrefix(const wchar_t *path, size_t count, ByteBuffer *output);
int WriteWholeFile(const wchar_t *path, const void *data, size_t size);
int FileWriteTime(const wchar_t *path, uint64_t *time);
int FileSize(const wchar_t *path, uint64_t *size);
int MappedFileOpen(MappedFile *mapped, const wchar_t *path);
void MappedFileClose(MappedFile *mapped);
int WalkFiles(const wchar_t *root, FileVisitor visitor, void *context);

void ErrorLogInitialize(ErrorLog *log);
void ErrorLogAdd(ErrorLog *log, const wchar_t *format, ...);
void ErrorLogClear(ErrorLog *log);
size_t ErrorLogCount(ErrorLog *log);
wchar_t *ErrorLogJoined(ErrorLog *log);
void ErrorLogFree(ErrorLog *log);

#endif
