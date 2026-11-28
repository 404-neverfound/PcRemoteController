#include "AgentServer.h"
#include "ClipboardImage.h"
#include "DesktopNotify.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <wincrypt.h>
#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "Crypt32.lib")

// The agent runs once per interactive session. The session id is part of the
// pipe name so a client always reaches the agent of the desktop session it
// targets, even when several users/sessions are logged on at the same time.
static const wchar_t* kPipePrefix = L"\\\\.\\pipe\\PcRemoteController.Agent.";
static const wchar_t* kMutexPrefix = L"Local\\PcRemoteController.Agent.";
// Text input is base64-encoded on the wire. Image import (phone screenshots)
// also arrives as a single Base64 request, so the logical message limit is
// much larger than a text command; the named-pipe buffers themselves stay
// small and the request is streamed through them.
static const size_t kMaxImageBytes = 24 * 1024 * 1024;
static const size_t kMaxBase64Chars = ((kMaxImageBytes + 2) / 3) * 4;
static const size_t kMaxMessageBytes = kMaxBase64Chars + 256;
static const DWORD kPipeBufferBytes = 64 * 1024;
static const DWORD kRequestTimeoutMs = 10000;
static const size_t kMaxInputUnits = 20000;

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

static bool Base64Decode(const char* input, size_t length, std::string& output) {
    output.clear();
    if (length == 0) {
        return true;
    }
    if (length > 0xFFFFFFFFull) {
        return false;
    }

    DWORD size = 0;
    if (!CryptStringToBinaryA(input, (DWORD)length, CRYPT_STRING_BASE64,
                              NULL, &size, NULL, NULL)) {
        return false;
    }

    output.resize(size);
    if (size == 0) {
        return true;
    }
    DWORD actual = size;
    if (!CryptStringToBinaryA(input, (DWORD)length, CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(&output[0]), &actual, NULL, NULL)) {
        output.clear();
        return false;
    }
    output.resize(actual);
    return true;
}

static bool Base64Decode(const std::string& input, std::string& output) {
    return Base64Decode(input.data(), input.size(), output);
}

static bool Utf8ToWide(const std::string& text, std::wstring& result) {
    result.clear();
    if (text.empty()) {
        return true;
    }
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                  text.data(), (int)text.size(), NULL, 0);
    if (len <= 0) {
        return false;
    }
    result.resize(len);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               text.data(), (int)text.size(),
                               &result[0], len) == len;
}

// Types text into the currently focused window. Newlines become Enter and
// tabs become Tab; every other UTF-16 unit is sent as a Unicode key event.
static bool SendUnicodeText(const std::wstring& text) {
    static const size_t kChunkEvents = 128;
    std::vector<INPUT> events;
    events.reserve(kChunkEvents);

    auto flush = [&events]() -> bool {
        if (events.empty()) {
            return true;
        }
        UINT sent = SendInput((UINT)events.size(), events.data(), sizeof(INPUT));
        if (sent != events.size()) {
            DWORD error = GetLastError();
            LOG("Agent: SendInput text failed: sent=%u/%u error=%lu\n",
                sent, (UINT)events.size(), error);
            events.clear();
            return false;
        }
        events.clear();
        return true;
    };

    for (wchar_t ch : text) {
        INPUT down = {};
        INPUT up = {};
        down.type = INPUT_KEYBOARD;
        up.type = INPUT_KEYBOARD;

        if (ch == L'\r' || ch == L'\n') {
            down.ki.wVk = VK_RETURN;
            up.ki.wVk = VK_RETURN;
            up.ki.dwFlags = KEYEVENTF_KEYUP;
        } else if (ch == L'\t') {
            down.ki.wVk = VK_TAB;
            up.ki.wVk = VK_TAB;
            up.ki.dwFlags = KEYEVENTF_KEYUP;
        } else {
            down.ki.wScan = ch;
            down.ki.dwFlags = KEYEVENTF_UNICODE;
            up.ki.wScan = ch;
            up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        }

        events.push_back(down);
        events.push_back(up);
        if (events.size() >= kChunkEvents) {
            if (!flush()) {
                return false;
            }
        }
    }
    return flush();
}

// Writes the text to the clipboard. The previous clipboard contents are not
// restored. Returns false when the clipboard is locked or allocation fails.
static bool SetClipboardText(const std::wstring& text) {
    bool opened = false;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(NULL)) {
            opened = true;
            break;
        }
        Sleep(50);
    }
    if (!opened) {
        DWORD error = GetLastError();
        LOG("Agent: OpenClipboard failed: %lu\n", error);
        return false;
    }

    bool copied = false;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) {
                memory = NULL; // now owned by the clipboard
                copied = true;
            }
        }
        if (memory) {
            GlobalFree(memory);
        }
    }
    if (!copied) {
        DWORD error = GetLastError();
        LOG("Agent: clipboard write failed: %lu\n", error);
    }
    CloseClipboard();
    return copied;
}

// Writes the text to the clipboard and sends Ctrl+V.
static bool PasteText(const std::wstring& text) {
    if (!SetClipboardText(text)) {
        return false;
    }

    Sleep(50);
    INPUT input[4] = {};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_CONTROL;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = 'V';
    input[2].type = INPUT_KEYBOARD;
    input[2].ki.wVk = 'V';
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].type = INPUT_KEYBOARD;
    input[3].ki.wVk = VK_CONTROL;
    input[3].ki.dwFlags = KEYEVENTF_KEYUP;

    UINT sent = SendInput(4, input, sizeof(INPUT));
    if (sent != 4) {
        DWORD error = GetLastError();
        LOG("Agent: SendInput Ctrl+V failed: sent=%u error=%lu\n", sent, error);
        return false;
    }
    return true;
}

enum class InputMode {
    Text,
    Paste,
    Copy
};

static std::string HandleTextCommand(const std::string& command,
                                     const std::string& prefix, InputMode mode) {
    std::string encoded = command.substr(prefix.size());
    std::string utf8;
    if (!Base64Decode(encoded, utf8)) {
        return "ERR 400 invalid base64\n";
    }

    std::wstring text;
    if (!Utf8ToWide(utf8, text) || text.empty()) {
        return "ERR 400 empty text\n";
    }
    if (text.size() > kMaxInputUnits) {
        return "ERR 413 text too long\n";
    }

    bool ok = false;
    if (mode == InputMode::Text) {
        ok = SendUnicodeText(text);
    } else if (mode == InputMode::Paste) {
        ok = PasteText(text);
    } else {
        ok = SetClipboardText(text);
        if (ok) {
            wchar_t message[128];
            swprintf_s(message, L"文字已复制到剪贴板（%d 个字符）", (int)text.size());
            DesktopNotify::Show(message);
        }
    }
    return ok ? "OK\n" : "ERR 500 input failed\n";
}

// Imports a Base64-encoded image that is already in memory on the CLI side.
// The image is never written to disk and its bytes are not logged.
static std::string HandleImageCommand(const std::string& request,
                                      size_t payloadOffset) {
    try {
        size_t payloadLength = request.size() - payloadOffset;
        std::string bytes;
        if (!Base64Decode(request.data() + payloadOffset, payloadLength, bytes)) {
            return "ERR 400 invalid base64\n";
        }
        if (bytes.empty()) {
            return "ERR 400 empty image\n";
        }
        if (bytes.size() > kMaxImageBytes) {
            return "ERR 413 image too large\n";
        }

        ClipboardImage::Result result = ClipboardImage::CopyToClipboard(bytes);
        if (!result.ok) {
            LOG("Agent: clipboard image failed: %s\n", result.error.c_str());
            if (result.error == "image too large") {
                return "ERR 413 image too large\n";
            }
            if (result.error == "invalid image") {
                return "ERR 415 unsupported image\n";
            }
            if (result.error == "clipboard busy") {
                return "ERR 500 clipboard busy\n";
            }
            return "ERR 500 image failed\n";
        }

        LOG("Agent: clipboard image copied: %dx%d\n", result.width, result.height);
        wchar_t message[128];
        swprintf_s(message, L"图片已复制到剪贴板（%d × %d）",
                   result.width, result.height);
        DesktopNotify::Show(message);

        char reply[64];
        sprintf_s(reply, "OK %dx%d\n", result.width, result.height);
        return reply;
    } catch (...) {
        LOG("Agent: clipboard image threw an exception\n");
        return "ERR 500 image failed\n";
    }
}

// Reads one newline-terminated request with a timeout so a stalled client
// cannot block the agent forever.
static bool ReadRequest(HANDLE pipe, std::string& request) {
    std::string data;
    DWORD64 deadline = GetTickCount64() + kRequestTimeoutMs;
    char buffer[64 * 1024];

    while (GetTickCount64() < deadline && data.size() <= kMaxMessageBytes) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
            return false;
        }
        if (available == 0) {
            Sleep(10);
            continue;
        }

        size_t remaining = kMaxMessageBytes + 1 - data.size();
        DWORD toRead = (DWORD)(remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (toRead == 0) {
            return false;
        }

        DWORD read = 0;
        if (!ReadFile(pipe, buffer, toRead, &read, NULL) || read == 0) {
            return false;
        }

        size_t searchFrom = data.size();
        data.append(buffer, read);
        if (data.find('\n', searchFrom) != std::string::npos) {
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

    request.swap(data);
    return true;
}

static std::string HandleRequest(const std::string& request) {
    static const std::string kPrefix = "PCR1 ";
    if (request.compare(0, kPrefix.size(), kPrefix) != 0) {
        LOG("Agent: bad request (%d bytes)\n", (int)request.size());
        if (request.compare(0, 3, "PCR") == 0) {
            return "ERR 426 version mismatch\n";
        }
        return "ERR 400 bad request\n";
    }

    // Image imports are large; handle them before copying the command out of
    // the request and never write the payload to the log.
    static const std::string kImagePrefix = "clipboard.image ";
    if (request.compare(kPrefix.size(), kImagePrefix.size(), kImagePrefix) == 0) {
        LOG("Agent: request: image input (%d bytes)\n", (int)request.size());
        return HandleImageCommand(request, kPrefix.size() + kImagePrefix.size());
    }

    std::string command = request.substr(kPrefix.size());

    static const std::string kTextPrefix = "input.text ";
    static const std::string kPastePrefix = "input.paste ";
    static const std::string kCopyPrefix = "input.copy ";
    static const std::string kInputPrefix = "input.";
    bool isTextInput = command.compare(0, kInputPrefix.size(), kInputPrefix) == 0;
    if (isTextInput) {
        // Do not log the text payload.
        LOG("Agent: request: text input (%d bytes)\n", (int)request.size());
    } else if (request.size() > 16 * 1024) {
        // Never dump a large payload, even if it uses an unknown command.
        LOG("Agent: request: large command (%d bytes)\n", (int)request.size());
    } else {
        LOG("Agent: request: %s\n", request.c_str());
    }

    if (command == "ping") {
        return "OK\n";
    }
    if (command == "media.toggle") {
        return SendMediaToggleKey() ? "OK\n" : "ERR 500 send failed\n";
    }
    if (command.compare(0, kTextPrefix.size(), kTextPrefix) == 0) {
        return HandleTextCommand(command, kTextPrefix, InputMode::Text);
    }
    if (command.compare(0, kPastePrefix.size(), kPastePrefix) == 0) {
        return HandleTextCommand(command, kPastePrefix, InputMode::Paste);
    }
    if (command.compare(0, kCopyPrefix.size(), kCopyPrefix) == 0) {
        return HandleTextCommand(command, kCopyPrefix, InputMode::Copy);
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
        kPipeBufferBytes,
        kPipeBufferBytes,
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
