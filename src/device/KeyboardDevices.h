#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace KeyboardDevices {
    struct Target {
        bool parent;          // true: match the parent device instance id
        std::wstring prefix;  // case-insensitive prefix
    };

    struct DeviceInfo {
        std::wstring instanceId;
        std::wstring friendlyName;
        std::wstring parentId;
        std::vector<std::wstring> hardwareIds;
        DWORD status;
        ULONG problem;
        bool disabled;
        bool matched;
    };

    struct Counts {
        int matched;
        int changed;
        int already;
        int failed;
        int disabledAfter;
    };

    bool IsElevated();
    std::wstring ExeDirectory();
    std::wstring ConfigPath();
    std::string ToGbk(const std::wstring& text);

    // Loads keyboard_targets.txt from the executable directory.
    bool LoadTargets(std::vector<Target>& targets, std::wstring& error);

    // Returns all present keyboard-class devices, with the matched flag set.
    std::vector<DeviceInfo> Snapshot(const std::vector<Target>& targets);

    // Logs the present keyboard devices and the configured targets.
    void ListDevices();

    // Disables or enables every matched present keyboard device.
    Counts SetState(bool enable, const std::vector<Target>& targets);
}
