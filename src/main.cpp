// Glimpse native build: entry point, command line, single instance.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "glimpse.h"
#include "app.h"

namespace glimpse {

namespace Program {
wstr programDir;
wstr launchPath;
Options opts;

wstr StartCommandLine() {
    wstr line = L"\"" + launchPath + L"\" /background";
    if (!opts.iniArgument.empty()) line += L" \"/ini:" + opts.iniArgument + L"\"";
    return line;
}
}

static const wchar_t* kMutexName = L"Local\\Glimpse.SingleInstance";

static int RunApp() {
    Native::SetPerMonitorV2();
    Log::Init(false);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    bool createdNew = mutex != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
    if (!mutex) createdNew = true;
    int code = 0;
    if (!createdNew) {
        // Another Glimpse runs in this session: ask it to show Settings (or to exit).
        // Started at logon (/background): stay quiet.
        if (!(Program::opts.background && !Program::opts.showSettings && !Program::opts.exit))
            Ipc::SignalRunningInstance(Program::opts.exit ? Ipc::CmdExit : Ipc::CmdSettings);
    } else if (!Program::opts.exit) {
        code = App::Run();
    }
    if (mutex) {
        if (createdNew) ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code;
}

static int Run(const std::vector<wstr>& args) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    Program::launchPath = wstr(buf, n);
    Program::programDir = Util::DirName(Program::launchPath);
    Program::opts = Options::Parse(args);
    const Options& o = Program::opts;
    if (!o.error.empty()) {
        ConsoleOut::Attach();
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        std::string m = Util::Narrow(o.error + L"\r\n");
        DWORD written;
        if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, m.data(), (DWORD)m.size(), &written, nullptr);
        else ConsoleOut::Line(o.error);
        return 2;
    }
    if (o.selfTest) { ConsoleOut::Attach(); return SelfTest::Run(o.casesPath); }
    if (o.checkLang) { ConsoleOut::Attach(); return LangCheck::Run(); }
    if (!o.dumpLang.empty()) { ConsoleOut::Attach(); return Texts::DumpBuiltIn(o.dumpLang); }
    if (!o.dumpIni.empty()) {
        ConsoleOut::Attach();
        Util::WriteFileBytes(o.dumpIni, "\xEF\xBB\xBF" + Util::Narrow(Settings::DefaultIniText()));
        ConsoleOut::Line(L"wrote " + o.dumpIni);
        return 0;
    }
    if (!o.render.empty()) { ConsoleOut::Attach(); return Previews::Render(o.render, o.renderLang); }
    return RunApp();
}

} // namespace glimpse

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<glimpse::wstr> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    if (argv) LocalFree(argv);
    return glimpse::Run(args);
}
