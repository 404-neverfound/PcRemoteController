#pragma once
#include <string>

namespace DesktopNotify {
    // Shows a small topmost notification overlay in the current interactive
    // session. The call is asynchronous and best-effort: it returns after the
    // worker thread has been started, and failure never affects the command
    // that triggered it.
    bool Show(const std::wstring& message);
}
