#include "KeyboardController.h"
#include "../device/KeyboardDevices.h"
#include "../device/KeyboardGuard.h"
#include "../utils/Console.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <vector>

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

bool KeyboardController::Match(const std::string& cmd) {
    return cmd.find(GBK("关闭键盘")) != std::string::npos ||
           cmd.find(GBK("打开键盘")) != std::string::npos;
}

bool KeyboardController::Execute(const std::string& cmd) {
    bool disable = cmd.find(GBK("关闭键盘")) != std::string::npos;
    bool enable = cmd.find(GBK("打开键盘")) != std::string::npos;
    if (!disable && !enable) {
        return false;
    }

    LOG("Match: keyboard %s\n", disable ? "off" : "on");

    if (!KeyboardDevices::IsElevated()) {
        LOG("Keyboard: administrator rights required\n");
        Console::WriteLine("键盘操作需要管理员权限");
        return false;
    }

    std::vector<KeyboardDevices::Target> targets;
    std::wstring error;
    if (!KeyboardDevices::LoadTargets(targets, error)) {
        LOG("Keyboard: %s\n", KeyboardDevices::ToGbk(error).c_str());
        Console::WriteLine("键盘目标配置加载失败，详见 D:\\debug.txt");
        return false;
    }

    if (disable) {
        // Refuse to disable anything unless the reboot restore path is ready.
        std::vector<KeyboardDevices::DeviceInfo> devices = KeyboardDevices::Snapshot(targets);
        bool anyMatched = false;
        for (const auto& device : devices) {
            if (device.matched) {
                anyMatched = true;
                break;
            }
        }
        if (!anyMatched) {
            LOG("Keyboard: no matching keyboard is present\n");
            Console::WriteLine("未找到匹配的键盘设备，已取消");
            return false;
        }

        if (!KeyboardGuard::EnsureTask()) {
            LOG("Keyboard: cannot create restore task, refusing to disable\n");
            Console::WriteLine("无法创建开机恢复任务，已取消");
            return false;
        }
        if (!KeyboardGuard::SetMarker()) {
            LOG("Keyboard: cannot write marker, refusing to disable\n");
            Console::WriteLine("无法写入状态标记，已取消");
            return false;
        }

        KeyboardDevices::Counts counts = KeyboardDevices::SetState(false, targets);
        bool ok = counts.matched > 0 && counts.failed == 0 &&
                  counts.disabledAfter == counts.matched;
        if (ok) {
            Console::WriteLine("键盘已关闭");
        } else {
            Console::WriteLine("键盘关闭未完全成功，详见 D:\\debug.txt");
        }
        return ok;
    }

    KeyboardDevices::Counts counts = KeyboardDevices::SetState(true, targets);
    if (counts.matched == 0) {
        LOG("Keyboard: no matching keyboard is present\n");
        Console::WriteLine("未找到匹配的键盘设备");
        return false;
    }
    if (counts.disabledAfter == 0) {
        KeyboardGuard::ClearMarker();
        Console::WriteLine("键盘已打开");
        return true;
    }

    LOG("Keyboard: some devices are still disabled\n");
    Console::WriteLine("键盘打开未完全成功，详见 D:\\debug.txt");
    return false;
}
