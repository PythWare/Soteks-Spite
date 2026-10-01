#ifndef SOTEK_OPERATIONS_H
#define SOTEK_OPERATIONS_H

#include "common.h"
#include "pool.h"

enum OperationStage
{
    STAGE_IDLE = 0,
    STAGE_SCANNING,
    STAGE_UNPACKING,
    STAGE_REBUILDING_NESTED,
    STAGE_REBUILDING,
    STAGE_UPDATING
};

typedef struct OperationProgress
{
    volatile LONG64 done;
    volatile LONG64 total;
    volatile LONG64 files;
    volatile LONG stage;
} OperationProgress;

typedef struct OperationContext
{
    WorkerPool *pool;
    ErrorLog *errors;
    OperationProgress progress;
    const wchar_t *gameRoot;
    volatile LONG cancelled;
} OperationContext;

typedef struct WideSet
{
    wchar_t **items;
    uint64_t *hashes;
    size_t capacity;
    size_t count;
} WideSet;

typedef struct UnpackSummary
{
    LONG64 archives;
    LONG64 entries;
    LONG64 files;
    double seconds;
} UnpackSummary;

typedef struct RebuildSummary
{
    LONG64 archives;
    LONG64 archivesWritten;
    LONG64 nestedRebuilt;
    LONG64 entries;
    LONG64 added;
    LONG64 removed;
    double seconds;
} RebuildSummary;

typedef struct CompanionRecord
{
    wchar_t *container;
    uint32_t *steps;
    uint32_t stepCount;
    int fullCopy;
} CompanionRecord;

typedef struct CompanionGroup
{
    wchar_t *archive;
    uint64_t hash;
    CompanionRecord *records;
    size_t count;
    size_t capacity;
} CompanionGroup;

typedef struct Companions
{
    CompanionGroup *groups;
    size_t capacity;
    size_t count;
    size_t records;
} Companions;

typedef struct CompanionSource
{
    wchar_t *relative;
    wchar_t *outputRoot;
    wchar_t *path;
    ByteBuffer prefix;
} CompanionSource;

typedef struct BatchSummary
{
    LONG64 filesScanned;
    LONG64 gameArchives;
    LONG64 outsideLayout;
    LONG64 withCompanions;
    LONG64 copiesUpdated;
    LONG64 containersWritten;
    double seconds;
} BatchSummary;

double SecondsNow(void);
int OperationCancelled(OperationContext *context);
void ProgressReset(OperationContext *context, LONG stage);

void WideSetInsert(WideSet *set, const wchar_t *text);
int WideSetContains(const WideSet *set, const wchar_t *text);
void WideSetFree(WideSet *set);

wchar_t *GameRelativePath(const wchar_t *absolutePath, const wchar_t *gameRoot);

int UnpackFolder(OperationContext *context, const wchar_t *sourceRoot, const wchar_t *outputRoot, int mode, UnpackSummary *summary);
int RebuildFolder(OperationContext *context, const wchar_t *inputRoot, const wchar_t *outputRoot, RebuildSummary *summary);

int CompanionsLoad(const wchar_t *path, Companions *companions, ErrorLog *errors);
const CompanionGroup *CompanionsFind(const Companions *companions, const wchar_t *relative);
void CompanionsFree(Companions *companions);
int BatchUpdate(OperationContext *context, const wchar_t *folder, const Companions *companions, BatchSummary *summary);
int CompanionsUpdateSources(OperationContext *context, const Companions *companions, CompanionSource *sources, size_t count, const wchar_t *overlayRoot, LONG64 *copiesUpdated, LONG64 *containersWritten);

#endif
