#pragma once
#include <windows.h>
#include <cstdio>
#include <string>

// Append-only invocation log (D:\calls.log). Every regular command appends one
// line with the local time, the command keyword, the text source and the
// result. Text payloads must never be written here.
namespace AuditLog {
    inline std::string& Command() {
        static std::string command;
        return command;
    }

    inline const char*& Source() {
        static const char* source = "arg";
        return source;
    }

    inline void SetCommand(const std::string& command) {
        Command() = command;
    }

    inline void SetSource(const char* source) {
        Source() = source ? source : "arg";
    }

    inline void Write(bool matched, bool ok, int exitCode) {
        SYSTEMTIME now;
        GetLocalTime(&now);

        FILE* log = fopen("D:\\calls.log", "a");
        if (!log) {
            return;
        }

        const std::string& command = Command();
        fprintf(log,
                "%04u-%02u-%02u %02u:%02u:%02u.%03u command=%s source=%s result=%s exit=%d\n",
                (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
                (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond,
                (unsigned)now.wMilliseconds,
                matched && !command.empty() ? command.c_str() : "unknown",
                Source(),
                matched ? (ok ? "OK" : "FAIL") : "UNKNOWN",
                exitCode);
        fclose(log);
    }
}
