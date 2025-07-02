#include "VolumeController.h"

float ChangeVolume(float volume) {
    HRESULT hr;

    // ��ʼ�� COM
    hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        std::cout << "COM ��ʼ��ʧ��\n";
        return -1;
    }

    IMMDeviceEnumerator* pDeviceEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pDeviceEnumerator);
    if (FAILED(hr)) {
        std::cout << "�����豸ö����ʧ��\n";
        CoUninitialize();
        return -1;
    }

    IMMDevice* pDefaultDevice = nullptr;
    hr = pDeviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDevice);
    if (FAILED(hr)) {
        std::cout << "��ȡĬ����Ƶ�豸ʧ��\n";
        pDeviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }

    IAudioEndpointVolume* pEndpointVolume = nullptr;
    hr = pDefaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
        NULL, (void**)&pEndpointVolume);
    if (FAILED(hr)) {
        std::cout << "������Ƶ�˵�ʧ��\n";
        pDefaultDevice->Release();
        pDeviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }

    // ��������
    float volumeLevel = volume; // ��ΧΪ 0.0 ~ 1.0
    pEndpointVolume->SetMasterVolumeLevelScalar(volumeLevel, NULL);

    // ������Դ
    pEndpointVolume->Release();
    pDefaultDevice->Release();
    pDeviceEnumerator->Release();
    CoUninitialize();
    return 0;
}
