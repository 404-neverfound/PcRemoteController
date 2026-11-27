#pragma once

namespace AgentInstall {
    // Registers a logon task that starts the agent in the interactive session,
    // then starts it immediately for the current logon.
    bool Install();

    // Stops and removes the logon task.
    bool Uninstall();
}
