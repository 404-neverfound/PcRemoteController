#include "SleepController.h"
#include "../utils/Console.h"
#include "../utils/Logger.h"
#include <powrprof.h>
#include <shellapi.h>
#include <string>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "Shell32.lib")

// The suspend must happen *after* the SSH session has reported the result.
// A thread inside this process does not work: returning from WinMain calls
// ExitProcess, which kills every thread before it can suspend. A normally
// spawned child process does not work either: Windows OpenSSH puts everything
// it starts into a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so the
// child dies the moment the foreground process exits.
//
// The helper therefore has to run outside that job:
//   1. Start a copy of ourselves with CREATE_BREAKAWAY_FROM_JOB. This works
//      when we are not in a job (local run) or when the job allows breakaway.
//   2. Otherwise register a one-shot Task Scheduler task. It is launched by
//      the Task Scheduler service, which is outside the SSH session's job.
// The helper waits a moment and then suspends (or hibernates) the machine.
static const DWORD kSuspendDelayMs = 1000;
static const wchar_t* kTaskName = L"PcRemoteController_Sleep";
static const wchar_t* kHelperArg = L"--delayed-suspend";
static const wchar_t* kScheduledArg = L"--scheduled";
#ifdef _DEBUG
static const wchar_t* kNoSuspendArg = L"--no-suspend";
#endif

static std::string GBK(const char* utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t* wstr = new wchar_t[len];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr, len);
    len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, NULL, 0, NULL, NULL);
    char* gbk = new char[len];
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, gbk, len, NULL, NULL);
    std::string result(gbk);
    delete[] wstr;
    delete[] gbk;
    return result;
}

static std::wstring SchtasksPath() {
    wchar_t dir[MAX_PATH];
    UINT len = GetSystemDirectoryW(dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"schtasks.exe";
    }
    return std::wstring(dir) + L"\\schtasks.exe";
}

// Runs a command line without a window and waits for it to finish.
static bool RunAndWait(const std::wstring& cmdLine, DWORD timeoutMs, DWORD* exitCode) {
    std::wstring mutableCmd = cmdLine;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, &mutableCmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        // Capture the error before LOG(): its fopen() call may overwrite it.
        DWORD error = GetLastError();
        LOG("CreateProcess failed for command: error %lu\n", error);
        return false;
    }

    bool finished = (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0);
    if (finished) {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        if (exitCode) *exitCode = code;
    } else {
        LOG("Command timed out after %lu ms\n", timeoutMs);
        TerminateProcess(pi.hProcess, 1);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished;
}

// Arguments for the helper copy of this executable.
static std::wstring BuildHelperArgs() {
    std::wstring args = std::wstring(L" ") + kHelperArg + L" " + std::to_wstring(kSuspendDelayMs);
#ifdef _DEBUG
    // Test hook: never suspend the machine while running automated tests.
    if (GetEnvironmentVariableW(L"PCMRC_TEST_NO_SUSPEND", NULL, 0) > 0) {
        args += std::wstring(L" ") + kNoSuspendArg;
    }
#endif
    return args;
}

static bool SpawnHelperDirectly(const std::wstring& helperArgs) {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
        return false;
    }

    std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\"" + helperArgs;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    DWORD flags = CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
    if (!CreateProcessW(exePath, &cmdLine[0], NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi)) {
        // Capture the error before LOG(): its fopen() call may overwrite it.
        // ERROR_ACCESS_DENIED means the current job does not allow breakaway
        // (this is the case for a Windows OpenSSH session).
        DWORD error = GetLastError();
        LOG("Direct helper start failed: error %lu\n", error);
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool SpawnHelperViaScheduler(const std::wstring& helperArgs) {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
        return false;
    }

    // The action stored in the task:
    //   "C:\path\PcRemoteController.exe" --delayed-suspend 1000 --scheduled
    // It has to be passed to schtasks /tr as a single, quoted argument, so the
    // quotes belonging to the action itself are escaped. --scheduled tells the
    // helper to remove the one-shot task before suspending.
    std::wstring actionArgs = helperArgs + L" " + kScheduledArg;
    std::wstring actionArg = L"\"";
    actionArg += L"\\\"";
    actionArg += exePath;
    actionArg += L"\\\"";
    actionArg += actionArgs;
    actionArg += L"\"";

    std::wstring schtasks = L"\"" + SchtasksPath() + L"\"";
    std::wstring taskName = kTaskName;

    // The trigger time is irrelevant because the task is started with /run and
    // deleted by the helper. A past time (00:00) guarantees the leftover task
    // can never fire on its own. The task must be able to run on demand only.
    std::wstring createCmd = schtasks + L" /create /f /tn \"" + taskName + L"\" /tr " + actionArg +
                             L" /sc once /st 00:00 /it";
    DWORD exitCode = 1;
    if (!RunAndWait(createCmd, 15000, &exitCode) || exitCode != 0) {
        LOG("schtasks /create failed: exit code %lu\n", exitCode);
        return false;
    }

    std::wstring runCmd = schtasks + L" /run /tn \"" + taskName + L"\"";
    if (!RunAndWait(runCmd, 15000, &exitCode) || exitCode != 0) {
        LOG("schtasks /run failed: exit code %lu\n", exitCode);
        std::wstring deleteCmd = schtasks + L" /delete /f /tn \"" + taskName + L"\"";
        RunAndWait(deleteCmd, 5000, NULL);
        return false;
    }

    return true;
}

static bool DeleteScheduledTask() {
    std::wstring deleteCmd = L"\"" + SchtasksPath() + L"\" /delete /f /tn \"" + std::wstring(kTaskName) + L"\"";
    DWORD exitCode = 1;
    return RunAndWait(deleteCmd, 5000, &exitCode) && exitCode == 0;
}

bool SleepController::HandleHelperMode(int& exitCode) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }

    bool isHelper = false;
    bool scheduled = false;
#ifdef _DEBUG
    bool noSuspend = false;
#endif
    DWORD delayMs = kSuspendDelayMs;

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], kHelperArg) == 0) {
            isHelper = true;
            if (i + 1 < argc) {
                wchar_t* end = NULL;
                unsigned long value = wcstoul(argv[i + 1], &end, 10);
                if (end && end != argv[i + 1] && *end == L'\0') {
                    delayMs = static_cast<DWORD>(value);
                    ++i;
                }
            }
        } else if (_wcsicmp(argv[i], kScheduledArg) == 0) {
            scheduled = true;
        }
#ifdef _DEBUG
        else if (_wcsicmp(argv[i], kNoSuspendArg) == 0) {
            noSuspend = true;
        }
#endif
    }
    LocalFree(argv);

    if (!isHelper) {
        return false;
    }
    if (delayMs > 60000) {
        delayMs = 60000;
    }

    LOG("Helper: scheduled=%d, delay=%lu ms\n", scheduled ? 1 : 0, delayMs);

    if (scheduled) {
        // Deleting a running task does not terminate this instance.
        bool deleted = DeleteScheduledTask();
        LOG("Helper: one-shot task delete %s\n", deleted ? "ok" : "failed");
    }

    Sleep(delayMs);

#ifdef _DEBUG
    if (noSuspend) {
        LOG("Helper: test mode, skipping suspend\n");
        exitCode = 0;
        return true;
    }
#endif

    LOG("Helper: calling SetSuspendState\n");
    BOOL ok = SetSuspendState(TRUE, FALSE, FALSE);
    LOG("Helper: SetSuspendState=%d error=%lu\n", ok, ok ? 0UL : GetLastError());

    if (scheduled) {
        // Retry in case the first delete ran before the task was fully started.
        DeleteScheduledTask();
    }

    exitCode = 0;
    return true;
}

bool SleepController::Match(const std::string& cmd) {
    return cmd.find(GBK("休眠")) != std::string::npos;
}

bool SleepController::Execute(const std::string& cmd) {
    (void)cmd;
    LOG("Match: sleep\n");

    std::wstring helperArgs = BuildHelperArgs();

    if (SpawnHelperDirectly(helperArgs)) {
        LOG("Sleep helper started directly\n");
        Console::WriteLine("已安排休眠");
        return true;
    }

    LOG("Trying one-shot scheduled task\n");
    if (SpawnHelperViaScheduler(helperArgs)) {
        LOG("Sleep helper started via Task Scheduler\n");
        Console::WriteLine("已安排休眠");
        return true;
    }

    // Last resort: keep the old behaviour. The SSH call will not return before
    // the machine wakes up, but the machine does go to sleep.
    LOG("Helper could not be started, suspending immediately\n");
    BOOL ok = SetSuspendState(TRUE, FALSE, FALSE);
    if (ok) {
        Console::WriteLine("正在休眠");
    } else {
        Console::WriteLine("休眠失败，详见 D:\\debug.txt");
    }
    return ok != FALSE;
}
