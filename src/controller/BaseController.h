#pragma once
#include <string>

class BaseController {
public:
    virtual ~BaseController() = default;
    virtual bool Match(const std::string& cmd) = 0;
    virtual void Execute(const std::string& cmd) = 0;
};
