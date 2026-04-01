#pragma once
#include <cstdio>

extern bool g_DebugMode;

#define LOG(fmt, ...) do { if (g_DebugMode) { FILE* log = fopen("D:\\debug.txt", "a"); if (log) { fprintf(log, fmt, ##__VA_ARGS__); fclose(log); } } } while(0)
