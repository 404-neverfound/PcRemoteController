#include "AgentInstall.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <string>

static const wchar_t* kTaskName = L"PcRemoteController_Agent";

static std::wstring SchtasksPath() {
    wchar_t dir[MAX_PATH];
    UINT len = GetSystemDirectoryW(dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"schtasks.exe";
    }
    return std::wstring(dir) + L"\\schtasks.exe";
}

static bool RunAndWait(const std::wstring& cmdLine, DWORD timeoutMs, DWORD* exitCode) {
    std::wstring mutableCmd = cmdLine;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, &mutableCmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        // Capture the error before LOG(): its fopen() call may overwrite it.
        DWORD error = GetLastError();
        LOG("AgentInstall: CreateProcess failed: %lu\n", error);
        return false;
    }

    bool finished = (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0);
    if (finished && exitCode) {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        *exitCode = code;
    } else if (!finished) {
        TerminateProcess(pi.hProcess, 1);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished;
}

static bool TaskExists() {
    std::wstring cmd = L"\"" + SchtasksPath() + L"\" /query /tn \"" + kTaskName + L"\"";
    DWORD exitCode = 1;
    return RunAndWait(cmd, 15000, &exitCode) && exitCode == 0;
}

bool AgentInstall::Install() {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
        DWORD error = GetLastError();
        LOG("AgentInstall: GetModuleFileName failed: %lu\n", error);
        return false;
    }

    // The action stored in the task:
    //   "C:\path\PcRemoteController.exe" --agent
    // It has to be passed to schtasks /tr as one quoted argument, so the
    // quotes belonging to the action itself are escaped.
    std::wstring actionArg = L"\"";
    actionArg += L"\\\"";
    actionArg += exePath;
    actionArg += L"\\\" --agent";
    actionArg += L"\"";

    // /it runs the task in the logged-on user's interactive session, which is
    // where SendInput has to be called. A past schedule time is irrelevant
    // here: the onlogon trigger is the logon event and /run starts it on demand.
    std::wstring createCmd = L"\"" + SchtasksPath() + L"\" /create /f /tn \"" + kTaskName +
                             L"\" /tr " + actionArg + L" /sc onlogon /it";
    DWORD exitCode = 1;
    if (!RunAndWait(createCmd, 15000, &exitCode) || exitCode != 0) {
        LOG("AgentInstall: task create failed: exit %lu\n", exitCode);
        LOG("AgentInstall: command was: %ls\n", createCmd.c_str());
        LOG("AgentInstall: creating the logon task may require an elevated shell\n");
        return false;
    }
    LOG("AgentInstall: logon task created\n");

    std::wstring runCmd = L"\"" + SchtasksPath() + L"\" /run /tn \"" + kTaskName + L"\"";
    DWORD runExit = 1;
    if (!RunAndWait(runCmd, 15000, &runExit) || runExit != 0) {
        LOG("AgentInstall: task created but immediate start failed: exit %lu\n", runExit);
    } else {
        LOG("AgentInstall: agent started\n");
    }
    return true;
}

bool AgentInstall::Uninstall() {
    if (!TaskExists()) {
        LOG("AgentInstall: task not found, nothing to delete\n");
        return true;
    }

    std::wstring endCmd = L"\"" + SchtasksPath() + L"\" /end /tn \"" + kTaskName + L"\"";
    DWORD endExit = 1;
    RunAndWait(endCmd, 5000, &endExit);

    std::wstring deleteCmd = L"\"" + SchtasksPath() + L"\" /delete /f /tn \"" + kTaskName + L"\"";
    DWORD exitCode = 1;
    if (!RunAndWait(deleteCmd, 15000, &exitCode) || exitCode != 0) {
        LOG("AgentInstall: task delete failed: exit %lu\n", exitCode);
        return false;
    }
    LOG("AgentInstall: task deleted\n");
    return true;
}
