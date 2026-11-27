#include "KeyboardController.h"
#include "../device/KeyboardDevices.h"
#include "../device/KeyboardGuard.h"
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

void KeyboardController::Execute(const std::string& cmd) {
    bool disable = cmd.find(GBK("关闭键盘")) != std::string::npos;
    bool enable = cmd.find(GBK("打开键盘")) != std::string::npos;
    if (!disable && !enable) {
        return;
    }

    LOG("Match: keyboard %s\n", disable ? "off" : "on");

    if (!KeyboardDevices::IsElevated()) {
        LOG("Keyboard: administrator rights required\n");
        return;
    }

    std::vector<KeyboardDevices::Target> targets;
    std::wstring error;
    if (!KeyboardDevices::LoadTargets(targets, error)) {
        LOG("Keyboard: %s\n", KeyboardDevices::ToGbk(error).c_str());
        return;
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
            return;
        }

        if (!KeyboardGuard::EnsureTask()) {
            LOG("Keyboard: cannot create restore task, refusing to disable\n");
            return;
        }
        if (!KeyboardGuard::SetMarker()) {
            LOG("Keyboard: cannot write marker, refusing to disable\n");
            return;
        }

        KeyboardDevices::SetState(false, targets);
    } else {
        KeyboardDevices::Counts counts = KeyboardDevices::SetState(true, targets);
        if (counts.disabledAfter == 0) {
            KeyboardGuard::ClearMarker();
        } else {
            LOG("Keyboard: some devices are still disabled\n");
        }
    }
}
