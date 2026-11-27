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
#include "controller/KeyboardController.h"
#include "controller/SleepController.h"
#include "device/KeyboardDevices.h"
#include "device/KeyboardGuard.h"
#include "utils/Console.h"
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

static void PrintHelp() {
    Console::WriteLine("PcRemoteController - 远程控制辅助工具");
    Console::WriteLine("");
    Console::WriteLine("用法: PcRemoteController.exe <命令>");
    Console::WriteLine("");
    Console::WriteLine("音量:");
    Console::WriteLine("  音量增加 [数值]       默认 5");
    Console::WriteLine("  音量减少 [数值]       默认 5");
    Console::WriteLine("  设置音量 <0-100>      默认 50");
    Console::WriteLine("媒体:");
    Console::WriteLine("  暂停 / 播放");
    Console::WriteLine("键盘（需要管理员权限）:");
    Console::WriteLine("  关闭键盘 / 打开键盘");
    Console::WriteLine("电源:");
    Console::WriteLine("  休眠");
    Console::WriteLine("诊断:");
    Console::WriteLine("  --help, --list-commands");
    Console::WriteLine("  --agent-ping");
    Console::WriteLine("  --list-keyboards");
    Console::WriteLine("  --install-agent, --uninstall-agent");
    Console::WriteLine("  --install-keyboard-guard, --uninstall-keyboard-guard");
    Console::WriteLine("  --debug                 清空 D:\\debug.txt");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdLine, int iCmdShow) {
    try {
        if (HasArg(L"--agent")) {
            // The agent is started by Task Scheduler and has no console; do not
            // attach one.
            return AgentServer::Run();
        }

        Console::EnsureAttached();

        if (HasArg(L"--help") || HasArg(L"--list-commands")) {
            PrintHelp();
            return 0;
        }
        if (HasArg(L"--install-agent")) {
            bool ok = AgentInstall::Install();
            Console::WriteLine(ok ? "Agent 安装成功" : "Agent 安装失败，详见 D:\\debug.txt");
            return ok ? 0 : 1;
        }
        if (HasArg(L"--uninstall-agent")) {
            bool ok = AgentInstall::Uninstall();
            Console::WriteLine(ok ? "Agent 卸载成功" : "Agent 卸载失败，详见 D:\\debug.txt");
            return ok ? 0 : 1;
        }
        if (HasArg(L"--agent-ping")) {
            std::wstring response;
            bool ok = AgentClient::SendCommand(L"ping", response);
            LOG("Agent ping: %s (%ls)\n", ok ? "ok" : "failed", response.c_str());
            if (ok) {
                Console::WriteLine("Agent 可用");
            } else {
                std::string detail = Console::Utf8FromWide(response);
                Console::WriteLine(detail.empty() ? "Agent 不可用"
                                                  : "Agent 不可用: " + detail);
            }
            return ok ? 0 : 1;
        }
        if (HasArg(L"--list-keyboards")) {
            KeyboardDevices::ListDevices();
            Console::WriteLine("键盘设备列表已写入 D:\\debug.txt");
            return 0;
        }
        if (HasArg(L"--enable-keyboard")) {
            bool ok = KeyboardGuard::RunBootEnable();
            Console::WriteLine(ok ? "键盘恢复完成" : "键盘恢复失败，详见 D:\\debug.txt");
            return ok ? 0 : 1;
        }
        if (HasArg(L"--install-keyboard-guard")) {
            if (!KeyboardDevices::IsElevated()) {
                LOG("KeyboardGuard: administrator rights required\n");
                Console::WriteLine("需要管理员权限");
                return 1;
            }
            bool ok = KeyboardGuard::EnsureTask();
            Console::WriteLine(ok ? "键盘恢复任务已安装"
                                  : "键盘恢复任务安装失败，详见 D:\\debug.txt");
            return ok ? 0 : 1;
        }
        if (HasArg(L"--uninstall-keyboard-guard")) {
            if (!KeyboardDevices::IsElevated()) {
                LOG("KeyboardGuard: administrator rights required\n");
                Console::WriteLine("需要管理员权限");
                return 1;
            }
            bool ok = KeyboardGuard::RemoveTask();
            Console::WriteLine(ok ? "键盘恢复任务已卸载"
                                  : "键盘恢复任务卸载失败，详见 D:\\debug.txt");
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
            new KeyboardController(),
            new SleepController(),
            new MediaController()
        };

        bool matched = false;
        bool executed = false;
        for (auto* controller : controllers) {
            if (controller->Match(cmd)) {
                matched = true;
                executed = controller->Execute(cmd);
                break;
            }
        }

        for (auto* controller : controllers) {
            delete controller;
        }

        if (!matched) {
            LOG("No controller matched the command\n");
            Console::WriteErrorLine("未知命令，使用 --help 查看支持的命令");
            return 2;
        }
        return executed ? 0 : 1;
    } catch(...) {
        return 1;
    }
}