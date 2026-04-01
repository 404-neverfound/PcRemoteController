#include "MediaController.h"
#include "../utils/Logger.h"
#include <tlhelp32.h>

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

static DWORD GetActiveSessionId() {
    DWORD sessionId = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe = { sizeof(pe) };
        if (Process32First(hSnap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, "explorer.exe") == 0) {
                    ProcessIdToSessionId(pe.th32ProcessID, &sessionId);
                    break;
                }
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    return sessionId;
}

bool MediaController::Match(const std::string& cmd) {
    return cmd.find(GBK("暂停")) != std::string::npos || cmd.find(GBK("播放")) != std::string::npos;
}

void MediaController::Execute(const std::string& cmd) {
    LOG("Match: media\n");

    DWORD currentSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &currentSession);
    DWORD activeSession = GetActiveSessionId();

    LOG("Current session: %d, Active session: %d\n", currentSession, activeSession);

    if (currentSession != activeSession && activeSession != 0) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char cmdLine[512];
        sprintf_s(cmdLine, "cmd /c \"D:\\Code\\PcRemoteController\\PsExec.exe\" -accepteula -s -i %d \"%s\" \"%s\" >> D:\\debug.txt 2>&1",
                  activeSession, exePath, cmd.c_str());
        LOG("Executing: %s\n", cmdLine);
        system(cmdLine);
    } else {
        LOG("Sending input directly\n");
        INPUT input[2] = {};
        input[0].type = INPUT_KEYBOARD;
        input[0].ki.wVk = VK_MEDIA_PLAY_PAUSE;
        input[1].type = INPUT_KEYBOARD;
        input[1].ki.wVk = VK_MEDIA_PLAY_PAUSE;
        input[1].ki.dwFlags = KEYEVENTF_KEYUP;
        UINT result = SendInput(2, input, sizeof(INPUT));
        LOG("SendInput result: %d\n", result);
    }
}
