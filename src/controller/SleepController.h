#pragma once
#include "BaseController.h"
#include <windows.h>

class SleepController : public BaseController {
public:
    bool Match(const std::string& cmd) override;
    void Execute(const std::string& cmd) override;
};
