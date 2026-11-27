#include "KeyboardDevices.h"
#include "../utils/Logger.h"
#include <initguid.h>
#include <setupapi.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <fstream>
#include <string>
#include <vector>
#include <cwchar>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "Cfgmgr32.lib")

// Keyboard setup class {4D36E96B-E325-11CE-BFC1-08002BE10318}
DEFINE_GUID(GUID_DEVCLASS_KEYBOARD_LOCAL,
    0x4D36E96B, 0xE325, 0x11CE, 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18);

#ifndef CM_PROB_DISABLED
#define CM_PROB_DISABLED 22
#endif

static std::string WideToAcp(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    int len = WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    if (len <= 0) {
        return std::string();
    }
    std::string result(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), &result[0], len, NULL, NULL);
    return result;
}

static std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0);
    if (len > 0) {
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &result[0], len);
        return result;
    }
    // Fall back to the ANSI code page for files saved as ANSI.
    len = MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), NULL, 0);
    if (len <= 0) {
        return std::wstring();
    }
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), &result[0], len);
    return result;
}

static std::wstring Trim(const std::wstring& text) {
    size_t start = text.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) {
        return std::wstring();
    }
    size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(start, end - start + 1);
}

static bool StartsWithNoCase(const std::wstring& value, const std::wstring& prefix) {
    if (prefix.empty() || value.size() < prefix.size()) {
        return false;
    }
    return _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) == 0;
}

static bool Matches(const KeyboardDevices::DeviceInfo& device, const std::vector<KeyboardDevices::Target>& targets) {
    for (const auto& target : targets) {
        if (target.parent) {
            if (StartsWithNoCase(device.parentId, target.prefix)) {
                return true;
            }
        } else {
            if (StartsWithNoCase(device.instanceId, target.prefix)) {
                return true;
            }
            for (const auto& hardwareId : device.hardwareIds) {
                if (StartsWithNoCase(hardwareId, target.prefix)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static void ReadDeviceInfo(HDEVINFO devInfoSet, const SP_DEVINFO_DATA& devInfo,
                           const std::vector<KeyboardDevices::Target>& targets,
                           KeyboardDevices::DeviceInfo& device) {
    wchar_t buffer[MAX_PATH] = {};
    if (!SetupDiGetDeviceInstanceIdW(devInfoSet, const_cast<SP_DEVINFO_DATA*>(&devInfo),
                                     buffer, MAX_PATH, NULL)) {
        return;
    }
    device.instanceId = buffer;

    wchar_t name[512] = {};
    DWORD type = 0;
    DWORD required = 0;
    if (SetupDiGetDeviceRegistryPropertyW(devInfoSet, const_cast<SP_DEVINFO_DATA*>(&devInfo),
                                          SPDRP_FRIENDLYNAME, &type, (PBYTE)name, sizeof(name), &required)) {
        device.friendlyName = name;
    } else if (SetupDiGetDeviceRegistryPropertyW(devInfoSet, const_cast<SP_DEVINFO_DATA*>(&devInfo),
                                                 SPDRP_DEVICEDESC, &type, (PBYTE)name, sizeof(name), &required)) {
        device.friendlyName = name;
    }

    // Hardware ids are stored as a REG_MULTI_SZ.
    wchar_t hardwareIds[1024] = {};
    if (SetupDiGetDeviceRegistryPropertyW(devInfoSet, const_cast<SP_DEVINFO_DATA*>(&devInfo),
                                          SPDRP_HARDWAREID, &type, (PBYTE)hardwareIds,
                                          sizeof(hardwareIds), &required)) {
        const wchar_t* current = hardwareIds;
        while (*current) {
            device.hardwareIds.push_back(current);
            current += wcslen(current) + 1;
        }
    }

    wchar_t parent[MAX_PATH] = {};
    DEVPROPTYPE propertyType = 0;
    if (SetupDiGetDevicePropertyW(devInfoSet, const_cast<SP_DEVINFO_DATA*>(&devInfo),
                                  &DEVPKEY_Device_Parent, &propertyType, (PBYTE)parent,
                                  sizeof(parent), NULL, 0)) {
        device.parentId = parent;
    }

    ULONG status = 0;
    ULONG problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, devInfo.DevInst, 0) == CR_SUCCESS) {
        device.status = status;
        device.problem = problem;
    }
    device.disabled = (device.problem == CM_PROB_DISABLED);
    device.matched = Matches(device, targets);
}

static std::wstring PnputilPath() {
    wchar_t systemDir[MAX_PATH];
    UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::wstring path = std::wstring(systemDir) + L"\\pnputil.exe";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    // 32-bit process on 64-bit Windows: System32 is redirected to SysWOW64,
    // which does not contain pnputil; Sysnative points at the real System32.
    wchar_t windowsDir[MAX_PATH];
    len = GetWindowsDirectoryW(windowsDir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(windowsDir) + L"\\Sysnative\\pnputil.exe";
    }
    return L"pnputil.exe";
}

static bool RunAndWait(const std::wstring& cmdLine, DWORD timeoutMs, DWORD& exitCode) {
    std::wstring mutableCmd = cmdLine;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, &mutableCmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        // Capture the error before LOG(): its fopen() call may overwrite it.
        DWORD error = GetLastError();
        LOG("Keyboard: CreateProcess failed: %lu\n", error);
        return false;
    }

    bool finished = (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0);
    if (finished) {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        exitCode = code;
    } else {
        TerminateProcess(pi.hProcess, 1);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished;
}

// kbdclass.sys marks every keyboard as non-disableable, so the normal
// DIF_PROPERTYCHANGE path returns ERROR_NOT_DISABLEABLE. pnputil /force is
// the supported way to override that protection.
static bool SetStateViaPnpUtil(const std::wstring& instanceId, bool enable, DWORD& exitCode) {
    std::wstring cmd = L"\"" + PnputilPath() + L"\" ";
    cmd += enable ? L"/enable-device " : L"/disable-device ";
    cmd += L"\"" + instanceId + L"\"";
    if (!enable) {
        cmd += L" /force";
    }
    LOG("Keyboard: running %s\n", KeyboardDevices::ToGbk(cmd).c_str());
    return RunAndWait(cmd, 30000, exitCode);
}

static bool RestartDevice(const std::wstring& instanceId, DWORD& exitCode) {
    std::wstring cmd = L"\"" + PnputilPath() + L"\" /restart-device \"" + instanceId + L"\"";
    LOG("Keyboard: running %s\n", KeyboardDevices::ToGbk(cmd).c_str());
    return RunAndWait(cmd, 30000, exitCode);
}

static bool QueryDeviceState(const std::wstring& instanceId, DEVINST fallback, bool& disabled, ULONG& problem) {
    DEVINST devInst = 0;
    CONFIGRET result = CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(instanceId.c_str()),
                                          CM_LOCATE_DEVNODE_NORMAL);
    if (result != CR_SUCCESS) {
        devInst = fallback;
    }

    ULONG status = 0;
    problem = 0;
    result = CM_Get_DevNode_Status(&status, &problem, devInst, 0);
    if (result != CR_SUCCESS) {
        return false;
    }
    disabled = (problem == CM_PROB_DISABLED);
    return true;
}

static bool WaitForTargetState(const std::wstring& instanceId, DEVINST fallback, bool enable,
                               DWORD timeoutMs, bool& disabled, ULONG& problem) {
    DWORD64 deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (!QueryDeviceState(instanceId, fallback, disabled, problem)) {
            return false;
        }
        if (disabled == !enable) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(200);
    }
}

static bool SetDeviceState(HDEVINFO devInfoSet, SP_DEVINFO_DATA& devInfo, bool enable, DWORD& error) {
    SP_PROPCHANGE_PARAMS params = {};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
    params.Scope = DICS_FLAG_GLOBAL;
    params.HwProfile = 0;

    if (!SetupDiSetClassInstallParams(devInfoSet, &devInfo, &params.ClassInstallHeader, sizeof(params))) {
        error = GetLastError();
        return false;
    }
    if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfoSet, &devInfo)) {
        error = GetLastError();
        return false;
    }
    return true;
}

static const char* StatusText(const KeyboardDevices::DeviceInfo& device) {
    if (device.disabled) {
        return "disabled";
    }
    if (device.status & DN_STARTED) {
        return "started";
    }
    return "stopped";
}

bool KeyboardDevices::IsElevated() {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring KeyboardDevices::ExeDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD len = GetModuleFileNameW(NULL, buffer.data(), (DWORD)buffer.size());
        if (len == 0) {
            return std::wstring(L".");
        }
        if (len < buffer.size()) {
            std::wstring path(buffer.data(), len);
            size_t pos = path.find_last_of(L"\\/");
            return pos == std::wstring::npos ? std::wstring(L".") : path.substr(0, pos);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring KeyboardDevices::ConfigPath() {
    return ExeDirectory() + L"\\keyboard_targets.txt";
}

std::string KeyboardDevices::ToGbk(const std::wstring& text) {
    return WideToAcp(text);
}

bool KeyboardDevices::LoadTargets(std::vector<Target>& targets, std::wstring& error) {
    targets.clear();

    std::wstring path = ConfigPath();
    std::ifstream file(WideToAcp(path).c_str());
    if (!file) {
        error = L"cannot open config: " + path;
        return false;
    }

    std::string line;
    bool firstLine = true;
    while (std::getline(file, line)) {
        if (firstLine) {
            firstLine = false;
            if (line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                line.erase(0, 3);
            }
        }

        std::wstring rule = Trim(Utf8ToWide(line));
        if (rule.empty() || rule[0] == L'#') {
            continue;
        }

        Target target = {};
        target.parent = false;
        if (rule.size() > 7 && _wcsnicmp(rule.c_str(), L"parent:", 7) == 0) {
            target.parent = true;
            rule = Trim(rule.substr(7));
        }
        if (rule.empty()) {
            continue;
        }
        target.prefix = rule;
        targets.push_back(target);
    }

    if (targets.empty()) {
        error = L"no targets configured in " + path;
        return false;
    }
    return true;
}

std::vector<KeyboardDevices::DeviceInfo> KeyboardDevices::Snapshot(const std::vector<Target>& targets) {
    std::vector<DeviceInfo> result;

    HDEVINFO devInfoSet = SetupDiGetClassDevsW(&GUID_DEVCLASS_KEYBOARD_LOCAL, NULL, NULL, DIGCF_PRESENT);
    if (devInfoSet == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        LOG("Keyboard: SetupDiGetClassDevs failed: %lu\n", error);
        return result;
    }

    SP_DEVINFO_DATA devInfo = {};
    devInfo.cbSize = sizeof(devInfo);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devInfoSet, index, &devInfo); ++index) {
        DeviceInfo device = {};
        ReadDeviceInfo(devInfoSet, devInfo, targets, device);
        if (!device.instanceId.empty()) {
            result.push_back(device);
        }
    }

    SetupDiDestroyDeviceInfoList(devInfoSet);
    return result;
}

void KeyboardDevices::ListDevices() {
    LOG("Keyboard: elevated=%d\n", IsElevated() ? 1 : 0);
    std::wstring configPath = ConfigPath();
    LOG("Keyboard: config=%s\n", ToGbk(configPath).c_str());
    LOG("Keyboard: pnputil=%s\n", ToGbk(PnputilPath()).c_str());

    std::vector<Target> targets;
    std::wstring error;
    bool haveTargets = LoadTargets(targets, error);
    if (haveTargets) {
        for (const auto& target : targets) {
            LOG("Keyboard: target %s%s\n", target.parent ? "parent:" : "", ToGbk(target.prefix).c_str());
        }
    } else {
        LOG("Keyboard: %s\n", ToGbk(error).c_str());
    }

    std::vector<DeviceInfo> devices = Snapshot(haveTargets ? targets : std::vector<Target>());
    LOG("Keyboard: found %d present keyboard device(s)\n", (int)devices.size());
    for (const auto& device : devices) {
        LOG("Keyboard: %s instance=%s name=%s parent=%s status=%s problem=%lu\n",
            device.matched ? "[match]" : "[     ]",
            ToGbk(device.instanceId).c_str(),
            ToGbk(device.friendlyName).c_str(),
            ToGbk(device.parentId).c_str(),
            StatusText(device),
            device.problem);
    }
}

KeyboardDevices::Counts KeyboardDevices::SetState(bool enable, const std::vector<Target>& targets) {
    Counts counts = {};

    HDEVINFO devInfoSet = SetupDiGetClassDevsW(&GUID_DEVCLASS_KEYBOARD_LOCAL, NULL, NULL, DIGCF_PRESENT);
    if (devInfoSet == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        LOG("Keyboard: SetupDiGetClassDevs failed: %lu\n", error);
        return counts;
    }

    SP_DEVINFO_DATA devInfo = {};
    devInfo.cbSize = sizeof(devInfo);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devInfoSet, index, &devInfo); ++index) {
        DeviceInfo device = {};
        ReadDeviceInfo(devInfoSet, devInfo, targets, device);
        if (device.instanceId.empty() || !device.matched) {
            continue;
        }

        counts.matched++;
        std::string instanceId = ToGbk(device.instanceId);
        if (device.disabled == !enable) {
            counts.already++;
            if (device.disabled) {
                counts.disabledAfter++;
            }
            LOG("Keyboard: %s already %s\n", instanceId.c_str(), enable ? "enabled" : "disabled");
            continue;
        }

        DWORD pnpExit = 0;
        bool pnpStarted = SetStateViaPnpUtil(device.instanceId, enable, pnpExit);

        bool disabledNow = device.disabled;
        ULONG problem = device.problem;
        bool targetReached = false;
        if (pnpExit == 0) {
            targetReached = WaitForTargetState(device.instanceId, devInfo.DevInst, enable,
                                               3000, disabledNow, problem);
        } else {
            // 3010 means the change was recorded but is pending; do not waste
            // time waiting, the parent restart below applies it immediately.
            if (QueryDeviceState(device.instanceId, devInfo.DevInst, disabledNow, problem)) {
                targetReached = (disabledNow == !enable);
            }
        }

        DWORD restartExit = 0;
        if (!targetReached) {
            if (!device.parentId.empty()) {
                LOG("Keyboard: pending state not applied, restarting parent %s\n",
                    ToGbk(device.parentId).c_str());
                if (RestartDevice(device.parentId, restartExit)) {
                    targetReached = WaitForTargetState(device.instanceId, devInfo.DevInst, enable,
                                                       5000, disabledNow, problem);
                }
            }
        }

        if (!targetReached) {
            DEVINST parentInst = 0;
            if (CM_Get_Parent(&parentInst, devInfo.DevInst, 0) == CR_SUCCESS) {
                CONFIGRET cr = CM_Reenumerate_DevNode(parentInst, CM_REENUMERATE_SYNCHRONOUS);
                LOG("Keyboard: reenumerate parent result: %lu\n", (unsigned long)cr);
                if (cr == CR_SUCCESS) {
                    targetReached = WaitForTargetState(device.instanceId, devInfo.DevInst, enable,
                                                       5000, disabledNow, problem);
                }
            }
        }

        if (!targetReached) {
            DEVINST parentInst = 0;
            DEVINST grandInst = 0;
            if (CM_Get_Parent(&parentInst, devInfo.DevInst, 0) == CR_SUCCESS &&
                CM_Get_Parent(&grandInst, parentInst, 0) == CR_SUCCESS) {
                CONFIGRET cr = CM_Reenumerate_DevNode(grandInst, CM_REENUMERATE_SYNCHRONOUS);
                LOG("Keyboard: reenumerate grandparent result: %lu\n", (unsigned long)cr);
                if (cr == CR_SUCCESS) {
                    targetReached = WaitForTargetState(device.instanceId, devInfo.DevInst, enable,
                                                       5000, disabledNow, problem);
                }
            }
        }

        DWORD setupError = 0;
        if (!targetReached) {
            LOG("Keyboard: %s pnputil did not reach target (exit=%lu problem=%lu), trying class installer\n",
                instanceId.c_str(), pnpExit, problem);
            if (SetDeviceState(devInfoSet, devInfo, enable, setupError)) {
                targetReached = WaitForTargetState(device.instanceId, devInfo.DevInst, enable,
                                                   2000, disabledNow, problem);
            }
        }

        if (targetReached) {
            counts.changed++;
            if (disabledNow) {
                counts.disabledAfter++;
            }
            LOG("Keyboard: %s %s ok (pnputil exit=%lu)\n", instanceId.c_str(),
                enable ? "enabled" : "disabled", pnpExit);
        } else {
            counts.failed++;
            if (disabledNow) {
                counts.disabledAfter++;
            }
            LOG("Keyboard: %s %s failed: pnputil started=%d exit=%lu setup error=%lu problem=%lu\n",
                instanceId.c_str(), enable ? "enable" : "disable", pnpStarted ? 1 : 0,
                pnpExit, setupError, problem);
        }
    }

    SetupDiDestroyDeviceInfoList(devInfoSet);

    LOG("Keyboard: %s summary: matched=%d changed=%d already=%d failed=%d disabledAfter=%d\n",
        enable ? "enable" : "disable", counts.matched, counts.changed,
        counts.already, counts.failed, counts.disabledAfter);
    return counts;
}
