#include "TextController.h"
#include "../agent/AgentClient.h"
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

    std::wstring wide = AnsiToWide(text);
    if (wide.empty()) {
        Console::WriteLine("文字转换失败");
        return false;
    }
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
    LOG("Text: mode=%s chars=%d\n", ModeName(mode), (int)wide.size());

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
