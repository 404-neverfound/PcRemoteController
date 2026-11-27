#pragma once
#include "BaseController.h"

class KeyboardController : public BaseController {
public:
    bool Match(const std::string& cmd) override;
    void Execute(const std::string& cmd) override;
};
