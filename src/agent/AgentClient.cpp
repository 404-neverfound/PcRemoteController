#include "AgentClient.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <tlhelp32.h>
#include <string>

static const wchar_t* kPipePrefix = L"\\\\.\\pipe\\PcRemoteController.Agent.";
static const wchar_t* kAgentTaskName = L"PcRemoteController_Agent";
static const DWORD kConnectTimeoutMs = 300;
static const DWORD kStartTimeoutMs = 3000;

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
        LOG("AgentClient: CreateProcess failed: %lu\n", error);
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

// Finds the session of the logged-on desktop (explorer.exe), falling back to
// the active console session when no shell process is found.
static DWORD GetInteractiveSessionId() {
    DWORD sessionId = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 entry = { sizeof(entry) };
        if (Process32First(snapshot, &entry)) {
            do {
                if (_stricmp(entry.szExeFile, "explorer.exe") == 0) {
                    DWORD id = 0;
                    if (ProcessIdToSessionId(entry.th32ProcessID, &id) && id != 0) {
                        sessionId = id;
                        break;
                    }
                }
            } while (Process32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    if (sessionId == 0) {
        DWORD console = WTSGetActiveConsoleSessionId();
        if (console != 0xFFFFFFFF) {
            sessionId = console;
        }
    }
    return sessionId;
}

static std::wstring BuildPipeName(DWORD sessionId) {
    return std::wstring(kPipePrefix) + std::to_wstring(sessionId);
}

static HANDLE ConnectOnce(const std::wstring& pipeName) {
    return CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
}

static HANDLE ConnectWithWait(const std::wstring& pipeName, DWORD timeoutMs) {
    DWORD64 deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        HANDLE pipe = ConnectOnce(pipeName);
        if (pipe != INVALID_HANDLE_VALUE) {
            return pipe;
        }

        DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
            LOG("AgentClient: CreateFile failed: %lu\n", error);
            return INVALID_HANDLE_VALUE;
        }
        if (GetTickCount64() >= deadline) {
            return INVALID_HANDLE_VALUE;
        }

        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipeName.c_str(), 200);
        } else {
            Sleep(100);
        }
    }
}

// The agent is registered as an interactive logon task; /run starts it right
// now in the logged-on user's session without waiting for the next logon.
static bool StartAgentTask() {
    std::wstring cmd = L"\"" + SchtasksPath() + L"\" /run /tn \"" + kAgentTaskName + L"\"";
    DWORD exitCode = 1;
    if (!RunAndWait(cmd, 15000, &exitCode) || exitCode != 0) {
        LOG("AgentClient: starting agent task failed (exit %lu); is it installed? Run --install-agent\n", exitCode);
        return false;
    }
    return true;
}

static std::string ToUtf8(const std::wstring& text) {
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, NULL, 0, NULL, NULL);
    if (len <= 1) {
        return std::string();
    }
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &result[0], len, NULL, NULL);
    return result;
}

static std::wstring FromUtf8(const std::string& text) {
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
    if (len <= 1) {
        return std::wstring();
    }
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &result[0], len);
    return result;
}

static bool ReadResponse(HANDLE pipe, std::string& response, DWORD timeoutMs) {
    std::string data;
    DWORD64 deadline = GetTickCount64() + timeoutMs;
    char buffer[256];

    while (GetTickCount64() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
            return false;
        }
        if (available == 0) {
            Sleep(10);
            continue;
        }

        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, NULL) || read == 0) {
            return false;
        }
        data.append(buffer, read);
        if (data.find('\n') != std::string::npos) {
            break;
        }
    }

    if (data.empty()) {
        return false;
    }
    size_t newline = data.find('\n');
    if (newline != std::string::npos) {
        data.resize(newline);
    }
    if (!data.empty() && data.back() == '\r') {
        data.pop_back();
    }
    response = data;
    return true;
}

static bool IsSensitiveCommand(const std::string& command) {
    return command.compare(0, 6, "input.") == 0 ||
           command.compare(0, 10, "clipboard.") == 0;
}

static bool WriteAll(HANDLE pipe, const char* data, size_t size) {
    while (size > 0) {
        DWORD chunk = static_cast<DWORD>(size > 64 * 1024 ? 64 * 1024 : size);
        DWORD written = 0;
        if (!WriteFile(pipe, data, chunk, &written, NULL) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

bool AgentClient::SendRawCommand(const std::string& command, std::wstring& response,
                                 DWORD timeoutMs) {
    response.clear();

    DWORD sessionId = GetInteractiveSessionId();
    if (sessionId == 0) {
        response = L"no interactive session";
        LOG("AgentClient: no interactive session found\n");
        return false;
    }

    std::wstring pipeName = BuildPipeName(sessionId);
    HANDLE pipe = ConnectWithWait(pipeName, kConnectTimeoutMs);
    if (pipe == INVALID_HANDLE_VALUE) {
        LOG("AgentClient: agent not running in session %lu, starting it\n", sessionId);
        if (!StartAgentTask()) {
            response = L"agent unavailable (run --install-agent)";
            return false;
        }
        pipe = ConnectWithWait(pipeName, kStartTimeoutMs);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        response = L"agent unavailable after start";
        LOG("AgentClient: still cannot connect after starting agent\n");
        return false;
    }

    // Write the request in pieces: image payloads can be tens of megabytes and
    // the named pipe streams them through a small buffer.
    bool sent = WriteAll(pipe, "PCR1 ", 5) &&
                WriteAll(pipe, command.data(), command.size()) &&
                WriteAll(pipe, "\n", 1);

    std::string rawResponse;
    bool received = sent && ReadResponse(pipe, rawResponse, timeoutMs);
    CloseHandle(pipe);

    if (!received) {
        response = L"no response from agent";
        LOG("AgentClient: failed to send or receive (%d bytes)\n", (int)command.size());
        return false;
    }

    response = FromUtf8(rawResponse);
    if (IsSensitiveCommand(command)) {
        // Text and image payloads may contain secrets; log only the command
        // name and size.
        std::string name = command.substr(0, command.find(' '));
        std::wstring wideName = FromUtf8(name);
        LOG("AgentClient: sent '%ls' (%d bytes), response '%ls'\n",
            wideName.c_str(), (int)command.size(), response.c_str());
    } else {
        std::wstring wideCommand = FromUtf8(command);
        LOG("AgentClient: sent '%ls', response '%ls'\n",
            wideCommand.c_str(), response.c_str());
    }
    return rawResponse.compare(0, 2, "OK") == 0;
}

bool AgentClient::SendCommand(const std::wstring& command, std::wstring& response,
                              DWORD timeoutMs) {
    return SendRawCommand(ToUtf8(command), response, timeoutMs);
}

bool AgentClient::MediaToggle() {
    std::wstring response;
    return SendCommand(L"media.toggle", response);
}
