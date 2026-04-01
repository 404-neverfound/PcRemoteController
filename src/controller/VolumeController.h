#pragma once
#include "BaseController.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#pragma comment(lib, "Ole32.lib")

class VolumeController : public BaseController {
public:
    bool Match(const std::string& cmd) override;
    void Execute(const std::string& cmd) override;
private:
    void SetVolume(int volume);
    void AdjustVolume(int delta);
};