#include <iostream>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <string>
#include "agent/AgentClient.h"
#include "agent/AgentInstall.h"
#include "agent/AgentServer.h"
#include "controller/BaseController.h"
#include "controller/VolumeController.h"
#include "controller/MediaController.h"
#include "controller/BluetoothController.h"
#include "controller/SleepController.h"
#include "utils/Logger.h"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

bool g_DebugMode = true;

static bool HasArg(const wchar_t* arg) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], arg) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdLine, int iCmdShow) {
    try {
        if (HasArg(L"--agent")) {
            return AgentServer::Run();
        }
        if (HasArg(L"--install-agent")) {
            return AgentInstall::Install() ? 0 : 1;
        }
        if (HasArg(L"--uninstall-agent")) {
            return AgentInstall::Uninstall() ? 0 : 1;
        }
        if (HasArg(L"--agent-ping")) {
            std::wstring response;
            bool ok = AgentClient::SendCommand(L"ping", response);
            LOG("Agent ping: %s (%ls)\n", ok ? "ok" : "failed", response.c_str());
            return ok ? 0 : 1;
        }

        int helperExitCode = 0;
        if (SleepController::HandleHelperMode(helperExitCode)) {
            return helperExitCode;
        }

        std::string cmd(szCmdLine);

        if (cmd.find("--debug") != std::string::npos) {
            g_DebugMode = true;
            FILE* log = fopen("D:\\debug.txt", "w");
            if (log) fclose(log);
        }

        LOG("Raw input: %s\n", cmd.c_str());
        LOG("Input length: %d\n", (int)cmd.length());

        std::vector<BaseController*> controllers = {
            new VolumeController(),
            new BluetoothController(),
            new SleepController(),
            new MediaController()
        };

        for (auto* controller : controllers) {
            if (controller->Match(cmd)) {
                controller->Execute(cmd);
                break;
            }
        }

        for (auto* controller : controllers) {
            delete controller;
        }

        return 0;
    } catch(...) {
        return 1;
    }
}