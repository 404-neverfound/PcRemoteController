#pragma once
#include <windows.h>
#include <string>

namespace AgentClient {
    // Sends "PCR1 <command>" to the agent running in the interactive desktop
    // session. Starts the agent task on demand if it is not running yet.
    // Returns true only when the agent replied OK. timeoutMs bounds the wait
    // for the reply; text input needs a longer timeout than media commands.
    bool SendCommand(const std::wstring& command, std::wstring& response,
                     DWORD timeoutMs = 2000);

    bool MediaToggle();
}
