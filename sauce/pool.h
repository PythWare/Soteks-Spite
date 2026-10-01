#ifndef SOTEK_POOL_H
#define SOTEK_POOL_H

#include "common.h"

typedef void (*PoolTask)(void *argument);

typedef struct PoolJob
{
    PoolTask task;
    void *argument;
} PoolJob;

typedef struct WorkerPool
{
    HANDLE *threads;
    unsigned threadCount;
    SRWLOCK lock;
    CONDITION_VARIABLE workAvailable;
    CONDITION_VARIABLE workFinished;
    PoolJob *jobs;
    size_t capacity;
    size_t head;
    size_t count;
    size_t running;
    int stopping;
} WorkerPool;

int PoolStart(WorkerPool *pool, unsigned threadCount);
void PoolSubmit(WorkerPool *pool, PoolTask task, void *argument);
void PoolWaitIdle(WorkerPool *pool);
void PoolStop(WorkerPool *pool);
unsigned DefaultWorkerCount(void);

#endif
