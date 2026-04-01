#include "BluetoothController.h"
#include "../utils/Logger.h"
#include <initguid.h>
#include <setupapi.h>

#pragma comment(lib, "setupapi.lib")

DEFINE_GUID(GUID_DEVCLASS_BLUETOOTH, 0xe0cbf06c, 0xcd8b, 0x4647, 0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74);

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

bool BluetoothController::Match(const std::string& cmd) {
    return cmd.find(GBK("打开蓝牙")) != std::string::npos || cmd.find(GBK("关闭蓝牙")) != std::string::npos;
}

void BluetoothController::Execute(const std::string& cmd) {
    if (cmd.find(GBK("打开蓝牙")) != std::string::npos) {
        LOG("Match: bluetooth on\n");
        ToggleBluetooth(true);
    } else if (cmd.find(GBK("关闭蓝牙")) != std::string::npos) {
        LOG("Match: bluetooth off\n");
        ToggleBluetooth(false);
    }
}

void BluetoothController::ToggleBluetooth(bool enable) {
    HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_BLUETOOTH, NULL, NULL, DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA devInfoData;
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); i++) {
        SP_PROPCHANGE_PARAMS params;
        params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        params.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
        params.Scope = DICS_FLAG_GLOBAL;
        params.HwProfile = 0;

        if (SetupDiSetClassInstallParams(hDevInfo, &devInfoData, &params.ClassInstallHeader, sizeof(params))) {
            SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hDevInfo, &devInfoData);
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
}
