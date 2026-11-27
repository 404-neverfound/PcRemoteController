#pragma once
#include "BaseController.h"
#include <windows.h>

class SleepController : public BaseController {
public:
    bool Match(const std::string& cmd) override;
    void Execute(const std::string& cmd) override;

    // Handles the internal "--delayed-suspend" helper invocation. Returns true
    // and sets exitCode when this process was started as the helper, so WinMain
    // must exit without dispatching a normal command.
    static bool HandleHelperMode(int& exitCode);
};
