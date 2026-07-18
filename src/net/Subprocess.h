#pragma once
//
// Subprocess.h — tiny, shell-free process helpers for the online flight check.
//
// The flight check shells out to the Tailscale CLI to read the machine's tailnet
// state (see TailscaleProbe) and opens a browser / the Tailscale app to guide the
// user through the steps the game itself cannot perform (install + sign-in). Kept
// in tb_transport next to Socket — the portable engine never links this.
//
#include <string>
#include <vector>

namespace tb::net {

// Run `exe` with `args`, capturing its stdout into `out` (stderr is discarded).
// Blocking. No shell is invoked (fork/execvp on POSIX, CreateProcess with
// CREATE_NO_WINDOW on Windows — so no console window flashes in the GUI). Returns
// false only if the process could not be launched (e.g. a bad path); a non-zero
// exit still returns true, with whatever the process wrote in `out`.
bool runCapture(const std::string& exe, const std::vector<std::string>& args, std::string& out);

// Open a URL in the user's default browser. Fire-and-forget, non-blocking.
void openUrl(const std::string& url);

// Best-effort launch of the Tailscale GUI/tray app so the user can start it or
// sign in. Returns false where there is no such app to launch — notably Linux,
// where Tailscale is a system daemon (the flight check then shows the CLI command
// instead). True only means "launch attempted", not "app is now up".
bool openTailscaleApp();

} // namespace tb::net
