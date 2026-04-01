#include "SleepController.h"
#include "../utils/Logger.h"
#include <powrprof.h>

#pragma comment(lib, "PowrProf.lib")

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

static DWORD WINAPI SleepThread(LPVOID) {
    Sleep(100);
    SetSuspendState(true, FALSE, FALSE);
    return 0;
}

bool SleepController::Match(const std::string& cmd) {
    return cmd.find(GBK("休眠")) != std::string::npos;
}

void SleepController::Execute(const std::string& cmd) {
    LOG("Match: sleep\n");
    SetSuspendState(true, FALSE, FALSE);
    //CreateThread(NULL, 0, SleepThread, NULL, 0, NULL);
}
