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

    // Same as SendCommand, but takes an already UTF-8 encoded command. Used
    // for large payloads such as Base64-encoded images so the command is not
    // copied into a wide string. The payload is not written to the log.
    bool SendRawCommand(const std::string& command, std::wstring& response,
                        DWORD timeoutMs = 2000);

    bool MediaToggle();
}
