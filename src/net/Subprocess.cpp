//
// Subprocess.cpp — see Subprocess.h. POSIX fork/exec + winsock-free CreateProcess.
//
#include "Subprocess.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <filesystem>

namespace tb::net {

#ifdef _WIN32

bool runCapture(const std::string& exe, const std::vector<std::string>& args, std::string& out) {
    out.clear();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0); // parent's read end stays private

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    // Command line: "exe" arg1 arg2 …  (our args carry no spaces, so this is enough).
    std::string cmd = "\"" + exe + "\"";
    for (const std::string& a : args) cmd += " " + a;
    std::vector<char> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back('\0');

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr); // child owns the write end now; drop ours so ReadFile sees EOF
    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void openUrl(const std::string& url) {
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool openTailscaleApp() {
    // The Windows tray app; present alongside the CLI in a normal install.
    for (const char* p : {"C:\\Program Files\\Tailscale\\tailscale-ipn.exe",
                          "C:\\Program Files (x86)\\Tailscale\\tailscale-ipn.exe"}) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            ShellExecuteA(nullptr, "open", p, nullptr, nullptr, SW_SHOWNORMAL);
            return true;
        }
    }
    return false;
}

#else // POSIX

bool runCapture(const std::string& exe, const std::vector<std::string>& args, std::string& out) {
    out.clear();

    int fds[2];
    if (::pipe(fds) != 0) return false;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (pid == 0) { // child
        ::dup2(fds[1], STDOUT_FILENO);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) ::dup2(devnull, STDERR_FILENO); // silence CLI errors
        ::close(fds[0]);
        ::close(fds[1]);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execvp(exe.c_str(), argv.data());
        ::_exit(127); // exec failed (binary vanished between discovery and here)
    }

    // parent
    ::close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, static_cast<std::size_t>(n));
    ::close(fds[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    return !(WIFEXITED(status) && WEXITSTATUS(status) == 127);
}

namespace {
// fork/exec `opener url`, reaping the middle child so no zombie is left and the
// browser (grandchild) is reparented to init — never blocks the caller.
void launchDetached(const char* opener, const char* arg) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        const pid_t grandchild = ::fork();
        if (grandchild == 0) {
            ::setsid();
            ::execlp(opener, opener, arg, static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::_exit(0);
    }
    if (pid > 0) ::waitpid(pid, nullptr, 0);
}
} // namespace

void openUrl(const std::string& url) {
#ifdef __APPLE__
    launchDetached("open", url.c_str());
#else
    launchDetached("xdg-open", url.c_str());
#endif
}

bool openTailscaleApp() {
#ifdef __APPLE__
    // macOS: `open -a Tailscale` — two args, so exec it directly (not launchDetached).
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        ::execlp("open", "open", "-a", "Tailscale", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    if (pid > 0) ::waitpid(pid, nullptr, 0);
    return true;
#else
    // Linux: Tailscale is a daemon, not an app to launch — the flight check shows
    // the `systemctl` / `tailscale up` command instead.
    return false;
#endif
}

#endif

} // namespace tb::net
