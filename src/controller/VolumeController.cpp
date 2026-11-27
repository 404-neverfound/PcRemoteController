#include "VolumeController.h"
#include "../utils/Console.h"
#include "../utils/Logger.h"
#include <cstdlib>
#include <string>

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

static IAudioEndpointVolume* GetVolumeControl(bool& comInitialized) {
    comInitialized = false;
    if (FAILED(CoInitialize(NULL))) {
        return nullptr;
    }
    comInitialized = true;

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr) || !pEnum) {
        return nullptr;
    }

    IMMDevice* pDevice = nullptr;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    pEnum->Release();
    if (FAILED(hr) || !pDevice) {
        return nullptr;
    }

    IAudioEndpointVolume* pVolume = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pVolume);
    pDevice->Release();
    if (FAILED(hr) || !pVolume) {
        return nullptr;
    }
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

bool VolumeController::Execute(const std::string& cmd) {
    if (cmd.find(GBK("音量增加")) != std::string::npos || cmd.find(GBK("提高音量")) != std::string::npos || cmd.find(GBK("增加音量")) != std::string::npos) {
        LOG("Match: volume up\n");
        int amount = ExtractNumber(cmd, 5);
        bool ok = AdjustVolume(amount);
        if (ok) {
            Console::WriteLine("音量已增加 " + std::to_string(amount));
        } else {
            Console::WriteLine("音量调整失败（未找到音频设备）");
        }
        return ok;
    }
    if (cmd.find(GBK("音量减少")) != std::string::npos || cmd.find(GBK("降低音量")) != std::string::npos || cmd.find(GBK("减少音量")) != std::string::npos) {
        LOG("Match: volume down\n");
        int amount = ExtractNumber(cmd, 5);
        bool ok = AdjustVolume(-amount);
        if (ok) {
            Console::WriteLine("音量已减少 " + std::to_string(amount));
        } else {
            Console::WriteLine("音量调整失败（未找到音频设备）");
        }
        return ok;
    }
    if (cmd.find(GBK("设置音量")) != std::string::npos) {
        LOG("Match: set volume\n");
        int volume = ExtractNumber(cmd, 50);
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        bool ok = SetVolume(volume);
        if (ok) {
            Console::WriteLine("音量已设置为 " + std::to_string(volume));
        } else {
            Console::WriteLine("音量设置失败（未找到音频设备）");
        }
        return ok;
    }
    return false;
}

bool VolumeController::SetVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    bool comInitialized = false;
    IAudioEndpointVolume* pVolume = GetVolumeControl(comInitialized);
    bool ok = false;
    if (pVolume) {
        HRESULT hr = pVolume->SetMasterVolumeLevelScalar((float)volume / 100.0f, NULL);
        ok = SUCCEEDED(hr);
        pVolume->Release();
    }
    if (comInitialized) {
        CoUninitialize();
    }
    return ok;
}

bool VolumeController::AdjustVolume(int delta) {
    bool comInitialized = false;
    IAudioEndpointVolume* pVolume = GetVolumeControl(comInitialized);
    bool ok = false;
    if (pVolume) {
        float current = 0.0f;
        HRESULT hr = pVolume->GetMasterVolumeLevelScalar(&current);
        if (SUCCEEDED(hr)) {
            int newVolume = (int)(current * 100.0f) + delta;
            if (newVolume < 0) newVolume = 0;
            if (newVolume > 100) newVolume = 100;
            hr = pVolume->SetMasterVolumeLevelScalar((float)newVolume / 100.0f, NULL);
            ok = SUCCEEDED(hr);
        }
        pVolume->Release();
    }
    if (comInitialized) {
        CoUninitialize();
    }
    return ok;
}
