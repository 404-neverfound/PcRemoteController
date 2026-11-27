#pragma once

namespace AgentServer {
    // Runs the named-pipe server in the current (interactive) session.
    // Blocks for the lifetime of the process.
    int Run();
}
