#pragma once
#include "BaseController.h"

class KeyboardController : public BaseController {
public:
    bool Match(const std::string& cmd) override;
    bool Execute(const std::string& cmd) override;
};
