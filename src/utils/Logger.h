#pragma once
#include <windows.h>
#include <cstdio>

extern bool g_DebugMode;

#define LOG(fmt, ...) do { \
    if (g_DebugMode) { \
        SYSTEMTIME logTime; \
        GetLocalTime(&logTime); \
        FILE* log = fopen("D:\\debug.txt", "a"); \
        if (log) { \
            fprintf(log, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", \
                    (unsigned)logTime.wYear, (unsigned)logTime.wMonth, \
                    (unsigned)logTime.wDay, (unsigned)logTime.wHour, \
                    (unsigned)logTime.wMinute, (unsigned)logTime.wSecond, \
                    (unsigned)logTime.wMilliseconds); \
            fprintf(log, fmt, ##__VA_ARGS__); \
            fclose(log); \
        } \
    } \
} while(0)
