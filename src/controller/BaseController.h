#pragma once
#include <string>

class BaseController {
public:
    virtual ~BaseController() = default;
    virtual bool Match(const std::string& cmd) = 0;
    // Returns true when the command was carried out successfully. WinMain uses
    // this value as the process exit code so SSH callers can branch on it.
    virtual bool Execute(const std::string& cmd) = 0;
};
