#include "TextController.h"
#include "../agent/AgentClient.h"
#include "../utils/AuditLog.h"
#include "../utils/Console.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <wincrypt.h>
#include <string>

#pragma comment(lib, "Crypt32.lib")

// The command line is limited to about 32K UTF-16 units; keep a safety margin
// and cap the agent side as well.
static const size_t kMaxTextUnits = 20000;
static const DWORD kTextCommandTimeoutMs = 15000;
// stdin text is capped by bytes before decoding. UTF-8 needs at most 4 bytes
// per UTF-16 unit (astral characters are 4 bytes for 2 units), so more than
// kMaxStdinBytes always means more than kMaxTextUnits characters.
static const size_t kMaxStdinBytes = kMaxTextUnits * 4;
// Some SSH clients keep stdin open after sending the payload; treat a short
// silence as end of input so the command does not hang.
static const DWORD kStdinFirstByteTimeoutMs = 15000;
static const DWORD kStdinIdleTimeoutMs = 5000;

enum class TextMode {
    Input,
    Paste,
    Copy
};

static const char* ModeName(TextMode mode) {
    switch (mode) {
    case TextMode::Paste:
        return "paste";
    case TextMode::Copy:
        return "copy";
    default:
        return "input";
    }
}

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

// Text commands are matched as a prefix (after optional leading whitespace)
// instead of as a substring: the text after the keyword may itself contain
// words such as "播放" or "音量增加" that other controllers would otherwise
// claim first.
static size_t PrefixOffset(const std::string& cmd, const std::string& keyword) {
    size_t pos = 0;
    // Allow the global --debug flag before the text keyword.
    if (cmd.compare(0, 7, "--debug") == 0 &&
        (cmd.size() == 7 || cmd[7] == ' ' || cmd[7] == '\t')) {
        pos = 7;
    }
    while (pos < cmd.size() && (cmd[pos] == ' ' || cmd[pos] == '\t')) {
        ++pos;
    }
    if (cmd.compare(pos, keyword.size(), keyword) == 0) {
        return pos;
    }
    return std::string::npos;
}

static std::wstring AnsiToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    int len = MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), NULL, 0);
    if (len <= 0) {
        return std::wstring();
    }
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), &result[0], len);
    return result;
}

// "复制文字 -stdin" reads the text from stdin instead of the command line.
// Windows OpenSSH runs the remote command through cmd.exe, which silently
// truncates the command line at the first newline; stdin is the only reliable
// way to pass multi-line text.
static bool IsStdinToken(const std::string& text) {
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                       text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    std::string token = text.substr(0, end);
    return _stricmp(token.c_str(), "-stdin") == 0;
}

enum class StdinReadResult {
    Ok,
    TooLong,
    Failed
};

// Reads stdin until EOF (or a short idle period). Mirrors the stdin handling
// used by 复制图片 -base64: a console or an unavailable handle is rejected, and
// the data is capped before it is decoded.
static StdinReadResult ReadStdinText(std::string& data, size_t maxBytes) {
    data.clear();

    HANDLE input = Console::OriginalStdin();
    if (input == NULL || input == INVALID_HANDLE_VALUE) {
        input = GetStdHandle(STD_INPUT_HANDLE);
    }
    if (input == NULL || input == INVALID_HANDLE_VALUE) {
        return StdinReadResult::Failed;
    }

    DWORD type = GetFileType(input);
    if (type == FILE_TYPE_CHAR) {
        return StdinReadResult::Failed;
    }
    if (type == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) {
        return StdinReadResult::Failed;
    }

    char buffer[64 * 1024];
    ULONGLONG start = GetTickCount64();
    ULONGLONG lastData = start;
    bool received = false;
    for (;;) {
        DWORD available = 0;
        if (type == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) {
                    break;
                }
                return StdinReadResult::Failed;
            }
            if (available == 0) {
                ULONGLONG now = GetTickCount64();
                DWORD timeout = received ? kStdinIdleTimeoutMs : kStdinFirstByteTimeoutMs;
                if (now - (received ? lastData : start) >= timeout) {
                    break;
                }
                Sleep(10);
                continue;
            }
        }

        size_t remaining = maxBytes + 1 - data.size();
        DWORD toRead = (DWORD)(remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (toRead == 0) {
            return StdinReadResult::TooLong;
        }

        DWORD read = 0;
        if (!ReadFile(input, buffer, toRead, &read, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                break;
            }
            return StdinReadResult::Failed;
        }
        if (read == 0) {
            break;
        }
        data.append(buffer, read);
        received = true;
        lastData = GetTickCount64();
        if (data.size() > maxBytes) {
            return StdinReadResult::TooLong;
        }
    }
    return StdinReadResult::Ok;
}

// Decodes stdin as UTF-8 (a leading BOM is ignored) and falls back to the
// console code page for older Windows tools that do not emit UTF-8.
static bool DecodeStdinText(const std::string& bytes, std::wstring& text) {
    text.clear();
    size_t offset = 0;
    if (bytes.size() >= 3 &&
        (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB &&
        (unsigned char)bytes[2] == 0xBF) {
        offset = 3;
    }
    if (offset >= bytes.size()) {
        return true;
    }

    const char* data = bytes.data() + offset;
    int size = (int)(bytes.size() - offset);
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, NULL, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (len <= 0) {
        codePage = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(CP_ACP, 0, data, size, NULL, 0);
    }
    if (len <= 0) {
        return false;
    }
    text.resize(len);
    return MultiByteToWideChar(codePage, flags, data, size, &text[0], len) == len;
}

// CF_UNICODETEXT conventionally uses CRLF. Typing, however, maps every
// newline to a single Enter, so input mode must not see CRLF (it would press
// Enter twice). Normalize both forms here, in the CLI, before the agent sees
// the text.
static std::wstring NormalizeLineEndings(const std::wstring& text, bool crlf) {
    std::wstring result;
    result.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t ch = text[i];
        if (ch == L'\r' || ch == L'\n') {
            if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
                ++i;
            }
            if (crlf) {
                result.push_back(L'\r');
            }
            result.push_back(L'\n');
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

static bool Base64Encode(const std::string& input, std::string& output) {
    DWORD chars = 0;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(input.data()), (DWORD)input.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &chars)) {
        return false;
    }

    output.resize(chars);
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(input.data()), (DWORD)input.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              &output[0], &chars)) {
        output.clear();
        return false;
    }

    // The reported length may include the terminating null.
    output.resize(chars);
    while (!output.empty() && output.back() == '\0') {
        output.pop_back();
    }
    return true;
}

bool TextController::LooksLikeTextCommand(const std::string& cmd) {
    return PrefixOffset(cmd, GBK("输入文字")) != std::string::npos ||
           PrefixOffset(cmd, GBK("粘贴文字")) != std::string::npos ||
           PrefixOffset(cmd, GBK("复制文字")) != std::string::npos;
}

bool TextController::Match(const std::string& cmd) {
    return LooksLikeTextCommand(cmd);
}

bool TextController::Execute(const std::string& cmd) {
    std::string inputKeyword = GBK("输入文字");
    std::string pasteKeyword = GBK("粘贴文字");
    std::string copyKeyword = GBK("复制文字");

    TextMode mode = TextMode::Input;
    std::string keyword = inputKeyword;
    size_t keywordPos = PrefixOffset(cmd, keyword);
    if (keywordPos == std::string::npos) {
        mode = TextMode::Paste;
        keyword = pasteKeyword;
        keywordPos = PrefixOffset(cmd, keyword);
    }
    if (keywordPos == std::string::npos) {
        mode = TextMode::Copy;
        keyword = copyKeyword;
        keywordPos = PrefixOffset(cmd, keyword);
    }
    if (keywordPos == std::string::npos) {
        return false;
    }

    size_t pos = keywordPos + keyword.size();
    while (pos < cmd.size() && (cmd[pos] == ' ' || cmd[pos] == '\t')) {
        ++pos;
    }

    std::string text = cmd.substr(pos);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }

    bool fromStdin = IsStdinToken(text);
    // The audit log records the matched keyword and the input source only,
    // never the text itself.
    AuditLog::SetCommand(keyword);
    AuditLog::SetSource(fromStdin ? "stdin" : "arg");

    std::wstring wide;
    if (fromStdin) {
        std::string bytes;
        StdinReadResult result = ReadStdinText(bytes, kMaxStdinBytes);
        if (result == StdinReadResult::TooLong) {
            Console::WriteLine("文字过长（最多 20000 个字符）");
            return false;
        }
        if (result != StdinReadResult::Ok || bytes.empty()) {
            Console::WriteLine("没有从 stdin 读取到文字");
            return false;
        }
        if (!DecodeStdinText(bytes, wide) || wide.empty()) {
            Console::WriteLine("stdin 文字编码无效（请使用 UTF-8）");
            return false;
        }
    } else {
        // Strip one layer of surrounding quotes so "hello world" works as expected.
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            text = text.substr(1, text.size() - 2);
        }

        if (text.empty()) {
            if (mode == TextMode::Copy) {
                Console::WriteLine("请输入要复制的文字");
            } else if (mode == TextMode::Paste) {
                Console::WriteLine("请输入要粘贴的文字");
            } else {
                Console::WriteLine("请输入要输入的文字");
            }
            return false;
        }

        wide = AnsiToWide(text);
        if (wide.empty()) {
            Console::WriteLine("文字转换失败");
            return false;
        }
    }

    wide = NormalizeLineEndings(wide, mode != TextMode::Input);
    if (wide.size() > kMaxTextUnits) {
        Console::WriteLine("文字过长（最多 20000 个字符）");
        return false;
    }

    std::string encoded;
    if (!Base64Encode(Console::Utf8FromWide(wide), encoded)) {
        Console::WriteLine("文字编码失败");
        return false;
    }

    std::wstring command;
    if (mode == TextMode::Paste) {
        command = L"input.paste ";
    } else if (mode == TextMode::Copy) {
        command = L"input.copy ";
    } else {
        command = L"input.text ";
    }
    command.append(encoded.begin(), encoded.end());

    // Never log the text itself: it may contain passwords or other secrets.
    LOG("Text: mode=%s source=%s chars=%d\n", ModeName(mode),
        fromStdin ? "stdin" : "arg", (int)wide.size());

    std::wstring response;
    bool ok = AgentClient::SendCommand(command, response, kTextCommandTimeoutMs);
    if (ok) {
        if (mode == TextMode::Copy) {
            Console::WriteLine("文字已复制到剪贴板（" + std::to_string(wide.size()) + " 个字符）");
        } else if (mode == TextMode::Paste) {
            Console::WriteLine("文字已粘贴");
        } else {
            Console::WriteLine("文字已输入");
        }
    } else {
        const char* action = "文字输入失败";
        if (mode == TextMode::Paste) {
            action = "文字粘贴失败";
        } else if (mode == TextMode::Copy) {
            action = "文字复制失败";
        }
        std::string detail = Console::Utf8FromWide(response);
        Console::WriteLine(detail.empty() ? action : std::string(action) + ": " + detail);
    }
    return ok;
}
