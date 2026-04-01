#include "VolumeController.h"
#include "../utils/Logger.h"
#include <cstdlib>

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

static int ExtractNumber(const std::string& str, int defaultValue) {
    size_t pos = str.find_first_of("0123456789");
    return (pos != std::string::npos) ? std::atoi(str.c_str() + pos) : defaultValue;
}

static IAudioEndpointVolume* GetVolumeControl() {
    CoInitialize(NULL);
    IMMDeviceEnumerator* pEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (!pEnum) return nullptr;
    IMMDevice* pDevice = nullptr;
    pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    pEnum->Release();
    if (!pDevice) return nullptr;
    IAudioEndpointVolume* pVolume = nullptr;
    pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pVolume);
    pDevice->Release();
    return pVolume;
}

bool VolumeController::Match(const std::string& cmd) {
    return cmd.find(GBK("音量增加")) != std::string::npos ||
           cmd.find(GBK("提高音量")) != std::string::npos ||
           cmd.find(GBK("增加音量")) != std::string::npos ||
           cmd.find(GBK("音量减少")) != std::string::npos ||
           cmd.find(GBK("降低音量")) != std::string::npos ||
           cmd.find(GBK("减少音量")) != std::string::npos ||
           cmd.find(GBK("设置音量")) != std::string::npos;
}

void VolumeController::Execute(const std::string& cmd) {
    if (cmd.find(GBK("音量增加")) != std::string::npos || cmd.find(GBK("提高音量")) != std::string::npos || cmd.find(GBK("增加音量")) != std::string::npos) {
        LOG("Match: volume up\n");
        AdjustVolume(ExtractNumber(cmd, 5));
    } else if (cmd.find(GBK("音量减少")) != std::string::npos || cmd.find(GBK("降低音量")) != std::string::npos || cmd.find(GBK("减少音量")) != std::string::npos) {
        LOG("Match: volume down\n");
        AdjustVolume(-ExtractNumber(cmd, 5));
    } else if (cmd.find(GBK("设置音量")) != std::string::npos) {
        LOG("Match: set volume\n");
        SetVolume(ExtractNumber(cmd, 50));
    }
}

void VolumeController::SetVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    IAudioEndpointVolume* pVolume = GetVolumeControl();
    if (pVolume) {
        pVolume->SetMasterVolumeLevelScalar((float)volume / 100.0f, NULL);
        pVolume->Release();
    }
    CoUninitialize();
}

void VolumeController::AdjustVolume(int delta) {
    IAudioEndpointVolume* pVolume = GetVolumeControl();
    if (pVolume) {
        float current = 0.0f;
        pVolume->GetMasterVolumeLevelScalar(&current);
        int newVolume = (int)(current * 100) + delta;
        if (newVolume < 0) newVolume = 0;
        if (newVolume > 100) newVolume = 100;
        pVolume->SetMasterVolumeLevelScalar((float)newVolume / 100.0f, NULL);
        pVolume->Release();
    }
    CoUninitialize();
}
