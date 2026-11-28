# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Windows-only remote-control helper (`PcRemoteController.exe`, described in README as “用于辅助远程控制电脑”). It is normally invoked over Windows OpenSSH with one Chinese command, e.g. `PcRemoteController.exe 关闭键盘`. It is a GUI-subsystem executable (`WinMain`). Detailed diagnostics are appended to `D:\debug.txt` (GBK); command results are also written to stdout/console when the caller provides one. Exit codes: `0` success, `1` execution failure, `2` unknown command. `README.md` itself is GBK-encoded and may be rejected by the Read/Edit tools; use `iconv -f GBK -t UTF-8` when needed.

## Build and run

Run from the repository root. In Git Bash use dash-style MSBuild switches (MSYS rewrites `/p:...`):

```bash
"/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" \
  PcRemoteController.vcxproj -p:Configuration=Release -p:Platform=x64 -m -v:minimal
```

Output: `x64\Release\PcRemoteController.exe`. Use `-p:Configuration=Debug` for Debug.

The interactive agent lives in the same exe and keeps it open. A rebuild fails with `LNK1104` while the agent runs:

```bash
taskkill //IM PcRemoteController.exe //F
MSYS_NO_PATHCONV=1 schtasks /run /tn "PcRemoteController_Agent"
```

There is no automated test suite (`src/test_main.cpp` is not in the vcxproj). Validate with the built-in diagnostics and the log:

```bash
./x64/Release/PcRemoteController.exe --help
./x64/Release/PcRemoteController.exe --agent-ping
./x64/Release/PcRemoteController.exe --list-keyboards
tail -40 /d/debug.txt | iconv -f GBK -t UTF-8
```

All configurations compile with `/utf-8`; keep source files UTF-8 (no BOM). Do not remove the flag: the Chinese console result strings depend on it.

Console output requires a caller that waits for the GUI process (direct invocation or PowerShell `Start-Process -Wait`). `cmd /c` and PowerShell's `&` may return before the process exits, losing stdout and the exit code; over SSH this depends on how sshd waits for the process tree.

Do not disable the keyboard for a casual test unless you can SSH back in to re-enable it or are prepared to reboot (the guard task restores it at boot). `--debug` truncates `D:\debug.txt`.

Do not run text commands as a casual test: `输入文字` / `粘贴文字` inject keystrokes into the current foreground window, and `粘贴文字` / `复制文字` overwrite the clipboard. `复制文字` also shows a desktop overlay.

## Entry point and command dispatch

`src/cmd_contorller.cpp:WinMain`:

1. Handles ASCII internal modes via `HasArg`: `--help` / `--list-commands`, `--agent`, `--install-agent`, `--uninstall-agent`, `--agent-ping`, `--list-keyboards`, `--enable-keyboard`, `--install-keyboard-guard`, `--uninstall-keyboard-guard`. `--agent` returns before console setup; every other mode calls `Console::EnsureAttached()` first and prints a short result.
2. `SleepController::HandleHelperMode` handles the internal sleep helper (`--delayed-suspend <ms> [--scheduled]`).
3. Otherwise the ANSI command line (`szCmdLine`, GBK) is matched by controllers; **first match wins**. Order: Text → ImageClipboard → Volume → Keyboard → Sleep → Media. `TextController` uses prefix matching (after optional leading whitespace and an optional leading `--debug`) and must stay first: the typed text after its keyword may contain other controllers' keywords. `Execute` returns `bool`; `WinMain` maps it to exit code 0/1 and returns 2 when no controller matched.

Chinese literals are UTF-8 in source; each controller has a local `GBK()` helper that converts them to the ANSI code page before comparing with `szCmdLine`. Keep that pattern when adding Chinese keywords. Console result strings stay UTF-8 and go through `src/utils/Console.h`. Register new controllers in `cmd_contorller.cpp` (order matters) and in `PcRemoteController.vcxproj`.

## Architecture

### Controller model
`src/controller/BaseController.h` defines `Match(const std::string&)` / `Execute(const std::string&)`; `Execute` returns `true` on success and prints a short Chinese result line.

- **TextController** — 输入文字 / 粘贴文字 / 复制文字; prefix-matched and registered first; sends base64 UTF-8 to the agent, which types it via Unicode `SendInput`, pastes from the clipboard, or only writes the clipboard.
- **ImageClipboardController** — `复制图片 -base64` reads a Base64 image from stdin (the iOS Shortcuts `Run Script Over SSH` action pipes it there); `复制图片 <路径>` reads a local image file; `最新截图 [目录]` picks the newest image from a directory, defaulting to `%USERPROFILE%\latest.*` and `%USERPROFILE%\Pictures\PhoneScreenshots`. All forms send `clipboard.image <base64>` to the agent. The CLI (the elevated SSH process) reads local files itself and forwards the bytes, so the non-elevated agent never opens uploaded files (which may be high-integrity). The agent decodes the image in memory with GDI+ and writes `CF_DIB` (plus the registered `"PNG"` format for PNG sources) to the interactive clipboard. The image is never written to disk by the agent and never logged.
- **VolumeController** — Core Audio `IAudioEndpointVolume`; keywords 音量增加/提高音量/增加音量, 音量减少/降低音量/减少音量, 设置音量, with an optional number (defaults 5 / 50).
- **KeyboardController** — 关闭键盘 / 打开键盘; delegates to `src/device/KeyboardDevices.*` and `src/device/KeyboardGuard.*`.
- **SleepController** — 休眠; waits ~1 s, then `SetSuspendState`.
- **MediaController** — 暂停 / 播放; sends `media.toggle` to the agent.

The old Bluetooth feature (`BluetoothController`, 关闭蓝牙/打开蓝牙, `蓝牙启用教程.md`) was deliberately removed; do not restore it from old commits.

### Media: session-0 SSH → interactive-session agent
SSH commands run in session 0, where `SendInput` cannot reach the desktop. `src/agent/` bridges this:

- The agent runs as the logged-on user in the interactive session, started by the logon task `PcRemoteController_Agent` (`--agent`).
- Pipe name: `\\.\pipe\PcRemoteController.Agent.<sessionId>`; the session id routes the CLI to the correct desktop when multiple sessions exist.
- Protocol: newline-terminated `PCR1 <command>`; implemented commands are `ping`, `media.toggle`, `input.text <base64>`, `input.paste <base64>`, `input.copy <base64>` and `clipboard.image <base64>`; replies are `OK` / `ERR <code> <msg>`.
- Text commands carry base64-encoded UTF-8. A single text input is capped at 20000 UTF-16 units. `input.text` uses Unicode `SendInput` (Enter/Tab for `\n`/`\t`); `input.paste` writes `CF_UNICODETEXT` and sends Ctrl+V; `input.copy` writes `CF_UNICODETEXT` only. Clipboard writes overwrite the previous contents. `input.copy` also shows a best-effort desktop overlay containing only the character count. Typing/pasting targets the current foreground window and is subject to UIPI: the non-elevated agent cannot inject into elevated windows.
- Image import (`clipboard.image`) carries a Base64 image, capped at 24 MB after decoding (30 MP, 10000 px per side). The logical request limit is ~32 MB; the named-pipe buffers stay at 64 KB and the request is streamed. The agent decodes with GDI+ in memory, writes `CF_DIB` (24bpp) and, for PNG input, the registered `"PNG"` format, then shows the desktop overlay with the dimensions.
- `AgentClient` finds the explorer session, connects, and if the pipe is missing runs `schtasks /run /tn PcRemoteController_Agent`, then retries.
- The pipe DACL grants the current user and SYSTEM. `PIPE_REJECT_REMOTE_CLIENTS` belongs in the **pipe-mode** argument of `CreateNamedPipeW`, not the open mode.
- The agent is not elevated: it must not perform device/admin operations. Those stay in the CLI (the SSH shell is elevated).

### Keyboard control (requires elevation)
- Targets are read from `keyboard_targets.txt` in the exe directory. `config/keyboard_targets.txt` is the source-controlled template; the MSBuild target `CopyKeyboardTargets` copies it to the output directory only when the runtime file does not exist. Edit the runtime file (or delete it and rebuild) to change targets — rebuilding alone does not overwrite an existing runtime copy.
- Rules are case-insensitive prefix matches: a plain line matches device instance id or hardware id; `parent:` matches the parent device instance id. Multiple lines = multiple devices. Current default targets the ROG receiver's keyboard collection `HID\VID_0B05&PID_1ACE&MI_00&COL01`.
- Enumeration uses SetupAPI (`GUID_DEVCLASS_KEYBOARD`, present devices) for discovery/verification; state changes use `pnputil /disable-device "<instance>" /force` and `/enable-device`. **kbdclass.sys marks keyboards non-disableable**, so `SetupDiCallClassInstaller(DIF_PROPERTYCHANGE/DICS_DISABLE)` fails with `ERROR_NOT_DISABLEABLE` (0xE0000231). Do not “simplify” back to SetupAPI-only.
- pnputil returns 3010 (pending reboot) for the disable; `KeyboardDevices::SetState` then restarts the parent (`pnputil /restart-device`) to apply it immediately and verifies through `CM_Get_DevNode_Status` (`CM_PROB_DISABLED`/problem 22).
- `关闭键盘` refuses to proceed unless elevated, targets are loaded, a matching device is present, the guard task exists, and the marker was written. `打开键盘` enables targets and clears the marker.
- Reboot recovery: `KeyboardGuard` creates `PcRemoteController_KeyboardRestore`, an ONSTART task running as SYSTEM with `--enable-keyboard`. It waits/retries for the device, enables it, and clears `keyboard_disabled.flag` next to the exe. `--install-keyboard-guard` / `--uninstall-keyboard-guard` manage the task.
- On this machine both a ROG keyboard (via receiver/Bluetooth) and a Logitech keyboard exist; only configured targets are touched.

### Sleep: escaping the SSH job object
The suspend must happen after the SSH call returns, but Windows OpenSSH puts children into a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.

- `SleepController::Execute` spawns a helper copy with `CREATE_BREAKAWAY_FROM_JOB`; if denied, it registers the one-shot task `PcRemoteController_Sleep` (`--delayed-suspend 1000 --scheduled`) so the Task Scheduler service launches it outside the job.
- Delay is `kSuspendDelayMs = 1000`; the helper deletes the one-shot task before/after suspending and calls `SetSuspendState(TRUE, FALSE, FALSE)`.

### Console output
`src/utils/Console.h` provides best-effort stdout/stderr for the GUI-subsystem exe. `Console::EnsureAttached()` calls `AttachConsole(ATTACH_PARENT_PROCESS)` and falls back to `CONOUT$`; real consoles receive UTF-16 through `WriteConsoleW`, pipes and redirected files receive UTF-8 bytes through `WriteFile`. There is deliberately no `AllocConsole`, so the agent and scheduled helpers never pop a window; when no console or pipe exists, writes are no-ops. `EnsureAttached()` stores the inherited stdin handle in `Console::OriginalStdin()` before attaching; `复制图片 -base64` reads that handle because attaching to a console can replace the process's standard handles. Detailed diagnostics stay in the log.

### Desktop notification
`src/agent/DesktopNotify.*` shows a self-drawn notification overlay instead of a `Shell_NotifyIcon` balloon. A short-lived `_beginthreadex` worker creates a layered, topmost, no-activate window at the bottom-right of the primary work area, draws an antialiased success check icon (GDI+) and a single message line (GDI), fades in/out, and destroys itself after ~3.5 s. It is used by `input.copy` after a successful clipboard write. The overlay is DPI-aware and needs no AppUserModelID or Start Menu shortcut; it is not added to the Action Center. The call is asynchronous and best-effort, failure is logged, and the message contains only the character count.

### Logging
`src/utils/Logger.h`: `LOG` appends to `D:\debug.txt` via `fopen(..., "a")`; `g_DebugMode` defaults to true. **Capture `GetLastError()` before calling `LOG`** — the `fopen` inside the macro overwrites it. Existing code follows this; keep the pattern in new error paths. Text and image payloads must never be logged: `WinMain`, `TextController`, `ImageClipboardController`, `AgentClient` and `AgentServer` log only the mode and size.

## Operational notes

- Exit codes: `0` success, `1` execution failure, `2` unknown command (no controller matched). Internal diagnostic modes keep their 0/1 convention.
- Keyboard/agent installation and keyboard state changes require an elevated shell. The user's SSH session runs at High Mandatory Level; a local non-elevated shell logs `elevated=0` and refuses keyboard operations.
- Scheduled tasks: `PcRemoteController_Agent` (logon, interactive), `PcRemoteController_KeyboardRestore` (onstart, SYSTEM), `PcRemoteController_Sleep` (one-shot, only during a sleep command).
- `src/api_server.cpp` is empty but listed in the project; `src/test_main.cpp` is not built.
- Build intermediates under `PcRemote.89D10E8E/**` and `x64/Release/*.pdb` are tracked in git, while `*.exe` is ignored. Commit only source/config changes unless explicitly asked; runtime files `x64/Release/keyboard_targets.txt` and `x64/Release/keyboard_disabled.flag` are generated and untracked.
- Git remote uses SSH (`git@github.com:404-neverfound/PcRemoteController.git`), branch `init_project`.
