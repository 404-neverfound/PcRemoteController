#include "ImageClipboardController.h"
#include "../agent/AgentClient.h"
#include "../utils/Console.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <wincrypt.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "Crypt32.lib")

namespace {
    // Keep in sync with kMaxImageBytes in AgentServer.cpp / ClipboardImage.cpp.
    const size_t kMaxImageBytes = 24 * 1024 * 1024;
    const size_t kMaxBase64Chars = ((kMaxImageBytes + 2) / 3) * 4;
    // Allow line breaks inserted by Shortcuts or other producers.
    const size_t kMaxStdinBytes = kMaxBase64Chars + 4 * 1024 * 1024;
    const DWORD kReadIdleTimeoutMs = 60000;
    const DWORD kAgentTimeoutMs = 60000;

    std::string GBK(const char* utf8) {
        int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
        if (len <= 0) {
            return std::string();
        }
        std::wstring wide(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], len);

        len = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, NULL, 0, NULL, NULL);
        if (len <= 0) {
            return std::string();
        }
        std::string result(len, '\0');
        WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, &result[0], len, NULL, NULL);
        if (!result.empty() && result.back() == '\0') {
            result.pop_back();
        }
        return result;
    }

    std::string Trim(const std::string& text) {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                               text[begin] == '\r' || text[begin] == '\n')) {
            ++begin;
        }
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                               text[end - 1] == '\r' || text[end - 1] == '\n')) {
            --end;
        }
        return text.substr(begin, end - begin);
    }

    std::string StripQuotes(const std::string& text) {
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            return text.substr(1, text.size() - 2);
        }
        return text;
    }

    std::string StripWhitespace(const std::string& text) {
        std::string result;
        result.reserve(text.size());
        for (char ch : text) {
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                result.push_back(ch);
            }
        }
        return result;
    }

    std::wstring AnsiToWide(const std::string& text) {
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

    bool ResolvePath(const std::string& ansiPath, std::wstring& widePath) {
        std::wstring input = AnsiToWide(ansiPath);
        if (input.empty()) {
            return false;
        }

        wchar_t buffer[1024];
        DWORD length = GetFullPathNameW(input.c_str(), (DWORD)(sizeof(buffer) / sizeof(buffer[0])),
                                        buffer, NULL);
        if (length == 0 || length >= sizeof(buffer) / sizeof(buffer[0])) {
            widePath = input;
        } else {
            widePath.assign(buffer, length);
        }
        return !widePath.empty();
    }

    bool IsImageExtension(const std::wstring& name) {
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos) {
            return false;
        }
        std::wstring extension = name.substr(dot);
        static const wchar_t* kExtensions[] = {
            L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff"
        };
        for (const wchar_t* candidate : kExtensions) {
            if (_wcsicmp(extension.c_str(), candidate) == 0) {
                return true;
            }
        }
        return false;
    }

    bool Base64Encode(const std::string& input, std::string& output) {
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
        output.resize(chars);
        while (!output.empty() && output.back() == '\0') {
            output.pop_back();
        }
        return true;
    }

    // Reads stdin until EOF (or a long idle period when the peer keeps the
    // pipe open). Returns false when the data exceeds maxBytes or the handle
    // is a console instead of a pipe/redirected file.
    bool ReadStdin(std::string& data, size_t maxBytes) {
        HANDLE input = Console::OriginalStdin();
        if (input == NULL || input == INVALID_HANDLE_VALUE) {
            input = GetStdHandle(STD_INPUT_HANDLE);
        }
        if (input == NULL || input == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD type = GetFileType(input);
        if (type == FILE_TYPE_CHAR) {
            return false;
        }
        if (type == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) {
            return false;
        }

        char buffer[64 * 1024];
        ULONGLONG lastData = GetTickCount64();
        for (;;) {
            DWORD available = 0;
            if (type == FILE_TYPE_PIPE) {
                if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL)) {
                    if (GetLastError() == ERROR_BROKEN_PIPE) {
                        break;
                    }
                    return false;
                }
                if (available == 0) {
                    if (GetTickCount64() - lastData >= kReadIdleTimeoutMs) {
                        // Some SSH clients keep stdin open after sending the
                        // payload. Treat a long silence as the end of input.
                        break;
                    }
                    Sleep(10);
                    continue;
                }
            }

            size_t remaining = maxBytes + 1 - data.size();
            DWORD toRead = (DWORD)(remaining < sizeof(buffer) ? remaining : sizeof(buffer));
            if (toRead == 0) {
                return false;
            }

            DWORD read = 0;
            if (!ReadFile(input, buffer, toRead, &read, NULL)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) {
                    break;
                }
                return false;
            }
            if (read == 0) {
                break;
            }
            data.append(buffer, read);
            lastData = GetTickCount64();
            if (data.size() > maxBytes) {
                return false;
            }
        }
        return true;
    }

    void PrintFailure(const std::string& detail) {
        if (detail.find("ERR 413") == 0) {
            Console::WriteLine("图片复制失败: 图片过大");
        } else if (detail.find("ERR 415") == 0) {
            Console::WriteLine("图片复制失败: 不支持的图片格式");
        } else if (detail.find("ERR 400") == 0) {
            Console::WriteLine("图片复制失败: Base64 数据无效");
        } else if (detail.find("ERR 404") == 0) {
            Console::WriteLine("图片复制失败: Agent 版本过旧，请重启 Agent");
        } else if (detail.find("ERR 500") == 0) {
            Console::WriteLine("图片复制失败: 剪贴板写入失败或 Agent 内部错误");
        } else if (detail.empty()) {
            Console::WriteLine("图片复制失败");
        } else {
            Console::WriteLine("图片复制失败: " + detail);
        }
    }

    bool SendBase64(const std::string& base64) {
        if (base64.empty()) {
            Console::WriteLine("没有从 stdin 读取到图片数据");
            return false;
        }
        if (base64.size() > kMaxBase64Chars) {
            Console::WriteLine("图片数据过大（解码后最多 24 MB）");
            return false;
        }

        // Never log the payload itself.
        LOG("ClipboardImage: Base64 payload %d bytes\n", (int)base64.size());

        std::string command = "clipboard.image ";
        command += base64;

        std::wstring response;
        bool ok = AgentClient::SendRawCommand(command, response, kAgentTimeoutMs);
        if (ok) {
            int width = 0;
            int height = 0;
            if (swscanf_s(response.c_str(), L"OK %dx%d", &width, &height) == 2 &&
                width > 0 && height > 0) {
                Console::WriteLine("图片已复制到剪贴板（" + std::to_string(width) + " × " +
                                   std::to_string(height) + "）");
            } else {
                Console::WriteLine("图片已复制到剪贴板");
            }
            return true;
        }

        PrintFailure(Console::Utf8FromWide(response));
        return false;
    }

    bool ImportFile(const std::wstring& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            LOG("ClipboardImage: open file failed: %lu\n", error);
            Console::WriteLine("无法打开图片文件，请检查路径");
            return false;
        }

        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
            DWORD error = GetLastError();
            CloseHandle(file);
            LOG("ClipboardImage: file size failed: %lu\n", error);
            Console::WriteLine("图片文件为空或无法读取");
            return false;
        }
        if (size.QuadPart > (LONGLONG)kMaxImageBytes) {
            CloseHandle(file);
            Console::WriteLine("图片文件过大（最多 24 MB）");
            return false;
        }

        std::string data((size_t)size.QuadPart, '\0');
        size_t total = 0;
        while (total < data.size()) {
            DWORD read = 0;
            size_t remaining = data.size() - total;
            DWORD toRead = (DWORD)(remaining < 1024 * 1024 ? remaining : 1024 * 1024);
            if (!ReadFile(file, &data[total], toRead, &read, NULL) || read == 0) {
                DWORD error = GetLastError();
                CloseHandle(file);
                LOG("ClipboardImage: file read failed: %lu\n", error);
                Console::WriteLine("读取图片文件失败");
                return false;
            }
            total += read;
        }
        CloseHandle(file);

        // Never log the file path or contents; the size is enough for diagnostics.
        LOG("ClipboardImage: file import (%d bytes)\n", (int)data.size());

        std::string base64;
        if (!Base64Encode(data, base64)) {
            Console::WriteLine("图片编码失败");
            return false;
        }
        return SendBase64(base64);
    }

    bool ImportFromStdin() {
        LOG("ClipboardImage: reading Base64 from stdin\n");
        std::string raw;
        if (!ReadStdin(raw, kMaxStdinBytes)) {
            Console::WriteLine("读取 stdin 失败（请通过管道传入 Base64 图片，且不要直接读控制台）");
            return false;
        }

        std::string base64 = StripWhitespace(raw);
        std::string().swap(raw);
        return SendBase64(base64);
    }

    void ConsiderFile(const std::wstring& path, std::wstring& bestPath, FILETIME& bestTime) {
        WIN32_FILE_ATTRIBUTE_DATA info = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
            return;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            return;
        }
        if (!IsImageExtension(path)) {
            return;
        }
        if (CompareFileTime(&info.ftLastWriteTime, &bestTime) > 0) {
            bestTime = info.ftLastWriteTime;
            bestPath = path;
        }
    }

    void CollectNewestInDir(const std::wstring& directory, std::wstring& bestPath,
                            FILETIME& bestTime) {
        std::wstring pattern = directory + L"\\*";
        WIN32_FIND_DATAW entry = {};
        HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) {
            return;
        }

        do {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            if (!IsImageExtension(entry.cFileName)) {
                continue;
            }
            if (CompareFileTime(&entry.ftLastWriteTime, &bestTime) > 0) {
                bestTime = entry.ftLastWriteTime;
                bestPath = directory + L"\\" + entry.cFileName;
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }

    std::wstring UserProfile() {
        wchar_t buffer[MAX_PATH];
        DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            return std::wstring(buffer, length);
        }
        return std::wstring();
    }

    bool ImportLatest(const std::wstring& directory) {
        std::wstring bestPath;
        FILETIME bestTime = {};

        if (!directory.empty()) {
            CollectNewestInDir(directory, bestPath, bestTime);
        } else {
            std::wstring profile = UserProfile();
            if (!profile.empty()) {
                // The a-Shell/scp workflow commonly uploads latest.png to the
                // user's home directory; also check the dedicated folder.
                static const wchar_t* kLatestNames[] = {
                    L"latest.png", L"latest.jpg", L"latest.jpeg", L"latest.bmp",
                    L"latest.gif", L"latest.tif", L"latest.tiff"
                };
                for (const wchar_t* name : kLatestNames) {
                    ConsiderFile(profile + L"\\" + name, bestPath, bestTime);
                }
                CollectNewestInDir(profile + L"\\Pictures\\PhoneScreenshots",
                                   bestPath, bestTime);
            }
        }

        if (bestPath.empty()) {
            Console::WriteLine("没有找到截图文件（可上传到用户目录的 latest.png "
                               "或 Pictures\\PhoneScreenshots）");
            return false;
        }

        LOG("ClipboardImage: latest file selected (%d chars)\n", (int)bestPath.size());
        return ImportFile(bestPath);
    }

    bool ImportPath(const std::string& ansiPath, bool fromLatest) {
        std::wstring widePath;
        if (!ResolvePath(ansiPath, widePath)) {
            Console::WriteLine("图片路径无效");
            return false;
        }

        DWORD attributes = GetFileAttributesW(widePath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            Console::WriteLine(fromLatest ? "找不到指定的文件或目录" : "找不到图片文件");
            return false;
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!fromLatest) {
                Console::WriteLine("复制图片需要一个文件路径；要扫描目录请用 最新截图");
                return false;
            }
            return ImportLatest(widePath);
        }
        return ImportFile(widePath);
    }
}

bool ImageClipboardController::Match(const std::string& cmd) {
    std::string copyKeyword = GBK("复制图片");
    std::string latestKeyword = GBK("最新截图");
    return (!copyKeyword.empty() && cmd.find(copyKeyword) != std::string::npos) ||
           (!latestKeyword.empty() && cmd.find(latestKeyword) != std::string::npos);
}

bool ImageClipboardController::Execute(const std::string& cmd) {
    std::string copyKeyword = GBK("复制图片");
    std::string latestKeyword = GBK("最新截图");
    size_t copyPos = cmd.find(copyKeyword);
    size_t latestPos = cmd.find(latestKeyword);

    if (copyPos == std::string::npos && latestPos == std::string::npos) {
        return false;
    }

    bool useCopy = copyPos != std::string::npos &&
                   (latestPos == std::string::npos || copyPos < latestPos);
    if (useCopy) {
        std::string arg = StripQuotes(Trim(cmd.substr(copyPos + copyKeyword.size())));
        if (_stricmp(arg.c_str(), "-base64") == 0) {
            return ImportFromStdin();
        }
        if (arg.empty()) {
            Console::WriteLine("用法: 复制图片 -base64 或 复制图片 <图片路径>");
            return false;
        }
        return ImportPath(arg, false);
    }

    std::string arg = StripQuotes(Trim(cmd.substr(latestPos + latestKeyword.size())));
    if (arg.empty()) {
        return ImportLatest(std::wstring());
    }
    return ImportPath(arg, true);
}
