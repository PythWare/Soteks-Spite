#ifndef SOTEK_LOOSE_H
#define SOTEK_LOOSE_H

#include <stddef.h>
#include <wchar.h>

#define LOOSE_CACHE_FOLDER L"SotekCache"

typedef void (*LooseLogFunction)(const char *category, const char *message);

typedef struct LooseResult
{
    char **paths;
    size_t count;
} LooseResult;

int LooseIsArchiveFolder(const wchar_t *gameRoot, const wchar_t *relativeDirectory);
int LoosePrepare(const wchar_t *gameRoot, const wchar_t *modsRoot, LooseLogFunction log, LooseResult *result);
void LooseResultFree(LooseResult *result);

#endif
