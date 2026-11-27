#include "AgentServer.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>

// The agent runs once per interactive session. The session id is part of the
// pipe name so a client always reaches the agent of the desktop session it
// targets, even when several users/sessions are logged on at the same time.
static const wchar_t* kPipePrefix = L"\\\\.\\pipe\\PcRemoteController.Agent.";
static const wchar_t* kMutexPrefix = L"Local\\PcRemoteController.Agent.";
static const DWORD kMaxMessageBytes = 1024;
static const DWORD kRequestTimeoutMs = 5000;

// Grants the current user and SYSTEM access to the pipe. Remote access is
// rejected by PIPE_REJECT_REMOTE_CLIENTS, so SMB clients can never connect.
static bool BuildPipeSecurity(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD error = GetLastError();
        LOG("Agent: OpenProcessToken failed: %lu\n", error);
        return false;
    }

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    std::vector<BYTE> buffer(size);
    BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok || size == 0) {
        DWORD error = GetLastError();
        LOG("Agent: GetTokenInformation failed: %lu\n", error);
        return false;
    }

    TOKEN_USER* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR sidString = NULL;
    if (!ConvertSidToStringSidW(user->User.Sid, &sidString)) {
        DWORD error = GetLastError();
        LOG("Agent: ConvertSidToStringSid failed: %lu\n", error);
        return false;
    }

    std::wstring sddl = L"D:P(A;;GA;;;";
    sddl += sidString;
    sddl += L")(A;;GA;;;SY)";
    LocalFree(sidString);

    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, NULL)) {
        DWORD error = GetLastError();
        LOG("Agent: ConvertStringSecurityDescriptor failed: %lu\n", error);
        return false;
    }

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = descriptor;
    sa.bInheritHandle = FALSE;
    sd = descriptor;
    return true;
}

static bool SendMediaToggleKey() {
    INPUT input[2] = {};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_MEDIA_PLAY_PAUSE;
    input[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    input[1] = input[0];
    input[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;

    UINT sent = SendInput(2, input, sizeof(INPUT));
    if (sent != 2) {
        DWORD error = GetLastError();
        LOG("Agent: SendInput failed: sent=%u error=%lu\n", sent, error);
        return false;
    }
    return true;
}

// Reads one newline-terminated request with a timeout so a stalled client
// cannot block the agent forever.
static bool ReadRequest(HANDLE pipe, std::string& request) {
    std::string data;
    DWORD64 deadline = GetTickCount64() + kRequestTimeoutMs;
    char buffer[256];

    while (GetTickCount64() < deadline && data.size() <= kMaxMessageBytes) {
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

    if (data.empty() || data.size() > kMaxMessageBytes) {
        return false;
    }

    size_t newline = data.find('\n');
    if (newline != std::string::npos) {
        data.resize(newline);
    }
    if (!data.empty() && data.back() == '\r') {
        data.pop_back();
    }

    request = data;
    return true;
}

static std::string HandleRequest(const std::string& request) {
    LOG("Agent: request: %s\n", request.c_str());

    static const std::string kPrefix = "PCR1 ";
    if (request.compare(0, kPrefix.size(), kPrefix) != 0) {
        if (request.compare(0, 3, "PCR") == 0) {
            return "ERR 426 version mismatch\n";
        }
        return "ERR 400 bad request\n";
    }

    std::string command = request.substr(kPrefix.size());
    if (command == "ping") {
        return "OK\n";
    }
    if (command == "media.toggle") {
        return SendMediaToggleKey() ? "OK\n" : "ERR 500 send failed\n";
    }
    return "ERR 404 unknown command\n";
}

static HANDLE CreateServerPipe(const std::wstring& pipeName, LPSECURITY_ATTRIBUTES sa) {
    // PIPE_REJECT_REMOTE_CLIENTS belongs to the pipe mode, not the open mode.
    return CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        kMaxMessageBytes,
        kMaxMessageBytes,
        0,
        sa);
}

int AgentServer::Run() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);

    std::wstring suffix = std::to_wstring(sessionId);
    std::wstring pipeName = std::wstring(kPipePrefix) + suffix;
    std::wstring mutexName = std::wstring(kMutexPrefix) + suffix;

    HANDLE mutex = CreateMutexW(NULL, TRUE, mutexName.c_str());
    if (!mutex) {
        LOG("Agent: CreateMutex failed: %lu\n", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG("Agent: already running in session %lu\n", sessionId);
        CloseHandle(mutex);
        return 0;
    }

    SECURITY_ATTRIBUTES sa = {};
    PSECURITY_DESCRIPTOR descriptor = NULL;
    LPSECURITY_ATTRIBUTES psa = NULL;
    if (BuildPipeSecurity(sa, descriptor)) {
        psa = &sa;
    } else {
        LOG("Agent: falling back to default pipe security\n");
    }

    LOG("Agent: listening in session %lu on %ls\n", sessionId, pipeName.c_str());

    for (;;) {
        HANDLE pipe = CreateServerPipe(pipeName, psa);
        if (pipe == INVALID_HANDLE_VALUE) {
            // Capture the error before LOG(): its fopen() call may overwrite it.
            DWORD error = GetLastError();
            LOG("Agent: CreateNamedPipe failed: %lu\n", error);
            if (descriptor) LocalFree(descriptor);
            CloseHandle(mutex);
            return 1;
        }

        BOOL connected = ConnectNamedPipe(pipe, NULL);
        DWORD connectError = connected ? 0 : GetLastError();
        if (!connected && connectError != ERROR_PIPE_CONNECTED) {
            // ERROR_NO_DATA means the client went away before sending anything.
            if (connectError != ERROR_NO_DATA) {
                LOG("Agent: ConnectNamedPipe failed: %lu\n", connectError);
                Sleep(100);
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }

        std::string request;
        if (ReadRequest(pipe, request)) {
            std::string response = HandleRequest(request);
            DWORD written = 0;
            WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, NULL);
            FlushFileBuffers(pipe);
        } else {
            LOG("Agent: no request received\n");
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}
