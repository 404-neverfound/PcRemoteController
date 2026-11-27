#include "KeyboardGuard.h"
#include "KeyboardDevices.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <string>
#include <vector>

static const wchar_t* kTaskName = L"PcRemoteController_KeyboardRestore";
static const wchar_t* kMarkerName = L"keyboard_disabled.flag";
static const DWORD kRestoreTimeoutMs = 60000;

static std::wstring SchtasksPath() {
    wchar_t dir[MAX_PATH];
    UINT len = GetSystemDirectoryW(dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"schtasks.exe";
    }
    return std::wstring(dir) + L"\\schtasks.exe";
}

static std::wstring ExePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD len = GetModuleFileNameW(NULL, buffer.data(), (DWORD)buffer.size());
        if (len == 0) {
            return std::wstring();
        }
        if (len < buffer.size()) {
            return std::wstring(buffer.data(), len);
        }
        buffer.resize(buffer.size() * 2);
    }
}

static bool RunAndWait(const std::wstring& cmdLine, DWORD timeoutMs, DWORD* exitCode) {
    std::wstring mutableCmd = cmdLine;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, &mutableCmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        // Capture the error before LOG(): its fopen() call may overwrite it.
        DWORD error = GetLastError();
        LOG("KeyboardGuard: CreateProcess failed: %lu\n", error);
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

static std::wstring MarkerPath() {
    return KeyboardDevices::ExeDirectory() + L"\\" + kMarkerName;
}

bool KeyboardGuard::EnsureTask() {
    if (TaskExists()) {
        LOG("KeyboardGuard: restore task already exists\n");
        return true;
    }

    std::wstring exePath = ExePath();
    if (exePath.empty()) {
        LOG("KeyboardGuard: cannot determine executable path\n");
        return false;
    }

    // Action stored in the task: "C:\path\PcRemoteController.exe" --enable-keyboard
    std::wstring actionArg = L"\"";
    actionArg += L"\\\"";
    actionArg += exePath;
    actionArg += L"\\\" --enable-keyboard";
    actionArg += L"\"";

    std::wstring createCmd = L"\"" + SchtasksPath() + L"\" /create /f /tn \"" + kTaskName +
                             L"\" /tr " + actionArg + L" /sc onstart /ru SYSTEM";
    DWORD exitCode = 1;
    if (!RunAndWait(createCmd, 15000, &exitCode) || exitCode != 0) {
        LOG("KeyboardGuard: restore task create failed: exit %lu\n", exitCode);
        LOG("KeyboardGuard: command was: %s\n", KeyboardDevices::ToGbk(createCmd).c_str());
        return false;
    }

    LOG("KeyboardGuard: restore task created\n");
    return true;
}

bool KeyboardGuard::RemoveTask() {
    if (!TaskExists()) {
        LOG("KeyboardGuard: restore task not found\n");
        return true;
    }

    std::wstring endCmd = L"\"" + SchtasksPath() + L"\" /end /tn \"" + kTaskName + L"\"";
    DWORD endExit = 1;
    RunAndWait(endCmd, 5000, &endExit);

    std::wstring deleteCmd = L"\"" + SchtasksPath() + L"\" /delete /f /tn \"" + kTaskName + L"\"";
    DWORD exitCode = 1;
    if (!RunAndWait(deleteCmd, 15000, &exitCode) || exitCode != 0) {
        LOG("KeyboardGuard: restore task delete failed: exit %lu\n", exitCode);
        return false;
    }

    LOG("KeyboardGuard: restore task deleted\n");
    return true;
}

bool KeyboardGuard::SetMarker() {
    std::wstring path = MarkerPath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        LOG("KeyboardGuard: create marker failed: %lu\n", error);
        return false;
    }
    CloseHandle(file);
    LOG("KeyboardGuard: marker set\n");
    return true;
}

bool KeyboardGuard::HasMarker() {
    DWORD attributes = GetFileAttributesW(MarkerPath().c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

void KeyboardGuard::ClearMarker() {
    if (DeleteFileW(MarkerPath().c_str())) {
        LOG("KeyboardGuard: marker cleared\n");
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        DWORD error = GetLastError();
        LOG("KeyboardGuard: clearing marker failed: %lu\n", error);
    }
}

bool KeyboardGuard::RunBootEnable() {
    LOG("KeyboardGuard: boot enable start (elevated=%d)\n", KeyboardDevices::IsElevated() ? 1 : 0);

    if (!HasMarker()) {
        LOG("KeyboardGuard: no marker, nothing to restore\n");
        return true;
    }

    std::vector<KeyboardDevices::Target> targets;
    std::wstring error;
    if (!KeyboardDevices::LoadTargets(targets, error)) {
        LOG("KeyboardGuard: %s\n", KeyboardDevices::ToGbk(error).c_str());
        LOG("KeyboardGuard: marker kept for the next boot\n");
        return false;
    }

    DWORD64 deadline = GetTickCount64() + kRestoreTimeoutMs;
    for (;;) {
        std::vector<KeyboardDevices::DeviceInfo> devices = KeyboardDevices::Snapshot(targets);
        int matched = 0;
        int disabled = 0;
        for (const auto& device : devices) {
            if (device.matched) {
                matched++;
                if (device.disabled) {
                    disabled++;
                }
            }
        }

        if (matched > 0 && disabled == 0) {
            ClearMarker();
            LOG("KeyboardGuard: keyboard restored\n");
            return true;
        }

        if (matched > 0 && disabled > 0) {
            LOG("KeyboardGuard: enabling %d disabled keyboard device(s)\n", disabled);
            KeyboardDevices::SetState(true, targets);
        } else {
            LOG("KeyboardGuard: waiting for the target keyboard to appear\n");
        }

        if (GetTickCount64() >= deadline) {
            LOG("KeyboardGuard: restore timed out, marker kept for the next boot\n");
            return false;
        }
        Sleep(2000);
    }
}
