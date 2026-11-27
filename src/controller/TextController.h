#pragma once
#include "BaseController.h"

class TextController : public BaseController {
public:
    bool Match(const std::string& cmd) override;
    bool Execute(const std::string& cmd) override;

    // True when cmd begins with a text-input keyword (optionally after
    // --debug). WinMain uses this to keep the text out of the log.
    static bool LooksLikeTextCommand(const std::string& cmd);
};
