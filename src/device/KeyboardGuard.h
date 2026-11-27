#pragma once

namespace KeyboardGuard {
    // Creates the SYSTEM startup task that restores the keyboard after a
    // reboot. Safe to call when the task already exists.
    bool EnsureTask();

    // Stops and removes the startup task.
    bool RemoveTask();

    // The marker is written before disabling the keyboard and is only
    // removed after the keyboard has been enabled again.
    bool SetMarker();
    bool HasMarker();
    void ClearMarker();

    // Boot-time entry point (--enable-keyboard). Runs as SYSTEM from the
    // startup task: waits for the target devices, enables them, then clears
    // the marker. Returns false and keeps the marker when it could not
    // restore the keyboard, so the next boot tries again.
    bool RunBootEnable();
}
