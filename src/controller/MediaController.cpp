#include "MediaController.h"
#include "../agent/AgentClient.h"
#include "../utils/Logger.h"
#include <windows.h>

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

bool MediaController::Match(const std::string& cmd) {
    return cmd.find(GBK("暂停")) != std::string::npos || cmd.find(GBK("播放")) != std::string::npos;
}

void MediaController::Execute(const std::string& cmd) {
    (void)cmd;
    LOG("Match: media\n");

    // The command arrives from SSH in session 0, where SendInput cannot reach
    // the desktop. The agent performs the key press in the interactive session.
    std::wstring response;
    bool ok = AgentClient::SendCommand(L"media.toggle", response);
    LOG("Media: agent request %s: %ls\n", ok ? "ok" : "failed", response.c_str());
}
