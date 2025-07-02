#include "VolumeController.h"

float ChangeVolume(float volume) {
    HRESULT hr;

    // 初始化 COM
    hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        std::cout << "COM 初始化失败\n";
        return -1;
    }

    IMMDeviceEnumerator* pDeviceEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pDeviceEnumerator);
    if (FAILED(hr)) {
        std::cout << "创建设备枚举器失败\n";
        CoUninitialize();
        return -1;
    }

    IMMDevice* pDefaultDevice = nullptr;
    hr = pDeviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDevice);
    if (FAILED(hr)) {
        std::cout << "获取默认音频设备失败\n";
        pDeviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }

    IAudioEndpointVolume* pEndpointVolume = nullptr;
    hr = pDefaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
        NULL, (void**)&pEndpointVolume);
    if (FAILED(hr)) {
        std::cout << "激活音频端点失败\n";
        pDefaultDevice->Release();
        pDeviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }

    // 设置音量
    float volumeLevel = volume; // 范围为 0.0 ~ 1.0
    pEndpointVolume->SetMasterVolumeLevelScalar(volumeLevel, NULL);

    // 清理资源
    pEndpointVolume->Release();
    pDefaultDevice->Release();
    pDeviceEnumerator->Release();
    CoUninitialize();
    return 0;
}
