#include "pool.h"

static DWORD WINAPI WorkerMain(void *parameter)
{
    WorkerPool *pool = parameter;

    for (;;)
    {
        PoolJob job;

        AcquireSRWLockExclusive(&pool->lock);
        while (!pool->count && !pool->stopping)
            SleepConditionVariableSRW(&pool->workAvailable, &pool->lock, INFINITE, 0);
        if (!pool->count && pool->stopping)
        {
            ReleaseSRWLockExclusive(&pool->lock);
            return 0;
        }
        job = pool->jobs[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pool->running++;
        ReleaseSRWLockExclusive(&pool->lock);

        job.task(job.argument);

        AcquireSRWLockExclusive(&pool->lock);
        pool->running--;
        if (!pool->count && !pool->running)
            WakeAllConditionVariable(&pool->workFinished);
        ReleaseSRWLockExclusive(&pool->lock);
    }
}

unsigned DefaultWorkerCount(void)
{
    SYSTEM_INFO info;

    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
}

int PoolStart(WorkerPool *pool, unsigned threadCount)
{
    unsigned index;

    memset(pool, 0, sizeof(*pool));
    InitializeSRWLock(&pool->lock);
    InitializeConditionVariable(&pool->workAvailable);
    InitializeConditionVariable(&pool->workFinished);
    pool->capacity = 256;
    pool->jobs = MemoryAllocate(pool->capacity * sizeof(PoolJob));
    pool->threads = MemoryAllocateZero(threadCount ? threadCount : 1, sizeof(HANDLE));
    for (index = 0; index < (threadCount ? threadCount : 1); index++)
    {
        HANDLE thread = CreateThread(NULL, 0, WorkerMain, pool, 0, NULL);

        if (!thread)
            break;
        SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
        pool->threads[pool->threadCount++] = thread;
    }
    if (!pool->threadCount)
    {
        MemoryRelease(pool->threads);
        MemoryRelease(pool->jobs);
        memset(pool, 0, sizeof(*pool));
        return 0;
    }
    return 1;
}

void PoolSubmit(WorkerPool *pool, PoolTask task, void *argument)
{
    AcquireSRWLockExclusive(&pool->lock);
    if (pool->count == pool->capacity)
    {
        size_t capacity = pool->capacity * 2;
        PoolJob *jobs = MemoryAllocate(capacity * sizeof(PoolJob));
        size_t index;

        for (index = 0; index < pool->count; index++)
            jobs[index] = pool->jobs[(pool->head + index) % pool->capacity];
        MemoryRelease(pool->jobs);
        pool->jobs = jobs;
        pool->capacity = capacity;
        pool->head = 0;
    }
    pool->jobs[(pool->head + pool->count) % pool->capacity].task = task;
    pool->jobs[(pool->head + pool->count) % pool->capacity].argument = argument;
    pool->count++;
    WakeConditionVariable(&pool->workAvailable);
    ReleaseSRWLockExclusive(&pool->lock);
}

void PoolWaitIdle(WorkerPool *pool)
{
    AcquireSRWLockExclusive(&pool->lock);
    while (pool->count || pool->running)
        SleepConditionVariableSRW(&pool->workFinished, &pool->lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&pool->lock);
}

void PoolStop(WorkerPool *pool)
{
    unsigned index;

    if (!pool->threads)
        return;
    AcquireSRWLockExclusive(&pool->lock);
    pool->stopping = 1;
    WakeAllConditionVariable(&pool->workAvailable);
    ReleaseSRWLockExclusive(&pool->lock);
    for (index = 0; index < pool->threadCount; index++)
    {
        WaitForSingleObject(pool->threads[index], INFINITE);
        CloseHandle(pool->threads[index]);
    }
    MemoryRelease(pool->threads);
    MemoryRelease(pool->jobs);
    memset(pool, 0, sizeof(*pool));
}
