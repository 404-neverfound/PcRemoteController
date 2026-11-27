#pragma once
#include "BaseController.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#pragma comment(lib, "Ole32.lib")

class VolumeController : public BaseController {
public:
    bool Match(const std::string& cmd) override;
    bool Execute(const std::string& cmd) override;
private:
    bool SetVolume(int volume);
    bool AdjustVolume(int delta);
};