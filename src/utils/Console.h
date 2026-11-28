#pragma once
#include <windows.h>
#include <string>

// Console output for this GUI-subsystem executable. When the program is
// launched from SSH or cmd it inherits or attaches to the parent console; when
// it is launched by Task Scheduler there is none and every write is silently
// ignored. Diagnostics still go to D:\debug.txt, so console output is
// best-effort only.
namespace Console {
    inline std::wstring Utf8ToWide(const std::string& text) {
        if (text.empty()) {
            return std::wstring();
        }
        int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), NULL, 0);
        if (len <= 0) {
            return std::wstring();
        }
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &result[0], len);
        return result;
    }

    inline std::string Utf8FromWide(const std::wstring& text) {
        if (text.empty()) {
            return std::string();
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                                      NULL, 0, NULL, NULL);
        if (len <= 0) {
            return std::string();
        }
        std::string result(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                            &result[0], len, NULL, NULL);
        return result;
    }

    inline HANDLE& OutHandle() {
        static HANDLE handle = INVALID_HANDLE_VALUE;
        return handle;
    }

    inline HANDLE& ErrHandle() {
        static HANDLE handle = INVALID_HANDLE_VALUE;
        return handle;
    }

    // The stdin handle as inherited from the caller. Saved before
    // AttachConsole(): attaching updates the standard handles of a
    // GUI-subsystem process, but the original pipe or redirected file must
    // stay readable for commands such as 复制图片 -base64.
    inline HANDLE& OriginalStdin() {
        static HANDLE handle = INVALID_HANDLE_VALUE;
        return handle;
    }

    inline void EnsureAttached() {
        static bool initialized = false;
        if (initialized) {
            return;
        }
        initialized = true;

        OriginalStdin() = GetStdHandle(STD_INPUT_HANDLE);

        // A GUI-subsystem process normally has no console. Attach to the
        // parent's console when there is one; handles inherited from SSH or a
        // redirection stay usable regardless.
        AttachConsole(ATTACH_PARENT_PROCESS);

        OutHandle() = GetStdHandle(STD_OUTPUT_HANDLE);
        if (OutHandle() == NULL || OutHandle() == INVALID_HANDLE_VALUE) {
            OutHandle() = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING, 0, NULL);
        }

        ErrHandle() = GetStdHandle(STD_ERROR_HANDLE);
        if (ErrHandle() == NULL || ErrHandle() == INVALID_HANDLE_VALUE) {
            ErrHandle() = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING, 0, NULL);
        }
    }

    inline void WriteTo(HANDLE handle, const std::string& text) {
        if (handle == NULL || handle == INVALID_HANDLE_VALUE || text.empty()) {
            return;
        }

        DWORD mode = 0;
        if (GetConsoleMode(handle, &mode)) {
            // Real console: write UTF-16 so the console's code page does not
            // garble the Chinese text.
            std::wstring wide = Utf8ToWide(text);
            if (!wide.empty()) {
                DWORD written = 0;
                WriteConsoleW(handle, wide.data(), (DWORD)wide.size(), &written, NULL);
            }
        } else {
            // Pipe or redirected file: write UTF-8 bytes unchanged.
            DWORD written = 0;
            WriteFile(handle, text.data(), (DWORD)text.size(), &written, NULL);
        }
    }

    inline void Write(const std::string& text) {
        EnsureAttached();
        WriteTo(OutHandle(), text);
    }

    inline void WriteLine(const std::string& text) {
        Write(text + "\n");
    }

    inline void WriteErrorLine(const std::string& text) {
        EnsureAttached();
        WriteTo(ErrHandle(), text + "\n");
    }
}
