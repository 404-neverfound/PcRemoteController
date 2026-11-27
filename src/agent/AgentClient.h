#pragma once
#include <string>

namespace AgentClient {
    // Sends "PCR1 <command>" to the agent running in the interactive desktop
    // session. Starts the agent task on demand if it is not running yet.
    // Returns true only when the agent replied OK.
    bool SendCommand(const std::wstring& command, std::wstring& response);

    bool MediaToggle();
}
