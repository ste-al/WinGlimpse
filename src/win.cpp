// Glimpse native build: Win32 helpers, UI-thread timers, monitors, window registry, event intake,
// actuator and Undo, drag state, hotkey, hot corner, Start with Windows, size and layout memory,
// first-click mode, the hidden message window and the single-instance signal.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include <climits>

namespace glimpse {

// ================================================================== Win32 helpers

namespace Native {

void SetPerMonitorV2() {
    typedef DPI_AWARENESS_CONTEXT(WINAPI * Fn)(DPI_AWARENESS_CONTEXT);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    Fn fn = u ? (Fn)(void*)GetProcAddress(u, "SetThreadDpiAwarenessContext") : nullptr;
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

int DpiForMonitor(HMONITOR hmon) {
    if (!hmon) return 96;
    UINT x = 0, y = 0;
    if (GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &x, &y) == S_OK && x > 0) return (int)x;
    return 96;
}

int DpiForWindow(HWND hwnd) {
    if (!hwnd) return 96;
    UINT d = GetDpiForWindow(hwnd);
    if (d > 0) return (int)d;
    return DpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
}

bool DragFullWindows() {
    BOOL v = TRUE;
    return !SystemParametersInfoW(SPI_GETDRAGFULLWINDOWS, 0, &v, 0) || v != 0;
}

bool TryFrame(HWND hwnd, Rc& out) {
    RECT r;
    if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r) == S_OK && r.right > r.left && r.bottom > r.top) {
        out = Rc::From(r);
        return true;
    }
    if (GetWindowRect(hwnd, &r)) { out = Rc::From(r); return true; }
    return false;
}

bool IsCloaked(HWND hwnd) {
    int v = 0;
    return DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &v, sizeof v) == S_OK && v != 0;
}

void SetDarkTitleBar(HWND hwnd, bool dark) {
    BOOL v = dark ? TRUE : FALSE;
    if (DwmSetWindowAttribute(hwnd, 20, &v, sizeof v) != S_OK) DwmSetWindowAttribute(hwnd, 19, &v, sizeof v);
}

void SetRoundCorners(HWND hwnd, bool smallRadius) {
    int v = smallRadius ? 3 : 2;
    DwmSetWindowAttribute(hwnd, 33, &v, sizeof v);
}

int IntegrityLevel(DWORD pid) {
    HANDLE proc = pid == 0 ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return -1;
    int result = -1;
    HANDLE token = nullptr;
    if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        DWORD len = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &len);
        if (len > 0) {
            std::vector<BYTE> buf(len);
            if (GetTokenInformation(token, TokenIntegrityLevel, buf.data(), len, &len)) {
                PSID sid = ((TOKEN_MANDATORY_LABEL*)buf.data())->Label.Sid;
                UCHAR count = *GetSidSubAuthorityCount(sid);
                if (count > 0) result = (int)*GetSidSubAuthority(sid, count - 1);
            }
        }
        CloseHandle(token);
    }
    if (pid != 0) CloseHandle(proc);
    return result;
}

wstr ProcessFileName(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return wstr();
    wchar_t buf[1024];
    DWORD size = 1024;
    wstr r;
    if (QueryFullProcessImageNameW(proc, 0, buf, &size)) r = Util::FileName(wstr(buf, size));
    CloseHandle(proc);
    return r;
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void ForceForeground(HWND hwnd) {
    if (!hwnd) return;
    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return;
    HWND fg = GetForegroundWindow();
    DWORD pid;
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
    DWORD me = GetCurrentThreadId();
    bool attached = fgThread != 0 && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

wstr WindowText(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return wstr();
    std::vector<wchar_t> buf(std::min(len + 1, 1024));
    int n = GetWindowTextW(h, buf.data(), (int)buf.size());
    return wstr(buf.data(), n > 0 ? n : 0);
}

wstr ClassName(HWND h) {
    wchar_t buf[256];
    int n = GetClassNameW(h, buf, 256);
    return wstr(buf, n > 0 ? n : 0);
}

POINT CursorPos() {
    POINT p = { 0, 0 };
    GetCursorPos(&p);
    return p;
}

} // namespace Native

// ================================================================== UI thread

namespace {
const UINT kPostMsg = WM_APP + 0x100;
HWND host = nullptr;
UINT_PTR nextTimerId = 100;
std::map<UINT_PTR, Timer*>& TimerMap() { static std::map<UINT_PTR, Timer*> m; return m; }
std::map<UINT_PTR, std::function<void()>>& OnceMap() { static std::map<UINT_PTR, std::function<void()>> m; return m; }
std::vector<HWND>& Dialogs() { static std::vector<HWND> d; return d; }
ULONG_PTR gdiplusToken = 0;

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    LRESULT r = 0;
    if (PopupMenu::OwnerDraw(m, wp, lp, r)) return r;
    if (m == WM_TIMER) {
        auto it = TimerMap().find(wp);
        if (it != TimerMap().end()) {
            std::function<void()> fn = it->second->tick;   // the timer may be destroyed by its own tick
            try { if (fn) fn(); } catch (const std::exception& ex) { Log::Error(L"timer: " + Util::Widen(ex.what())); }
            return 0;
        }
        auto once = OnceMap().find(wp);
        if (once != OnceMap().end()) {
            KillTimer(h, wp);
            std::function<void()> fn = std::move(once->second);
            OnceMap().erase(once);
            try { if (fn) fn(); } catch (const std::exception& ex) { Log::Error(L"later: " + Util::Widen(ex.what())); }
            return 0;
        }
        KillTimer(h, wp);
        return 0;
    }
    if (m == kPostMsg) {
        std::unique_ptr<std::function<void()>> fn((std::function<void()>*)lp);
        try { if (fn && *fn) (*fn)(); } catch (const std::exception& ex) { Log::Error(L"posted: " + Util::Widen(ex.what())); }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
} // namespace

Timer::Timer(int intervalMs) : interval(intervalMs), id(nextTimerId++) {}

Timer::~Timer() { Stop(); }

void Timer::Start() {
    Stop();
    if (!host) return;
    TimerMap()[id] = this;
    SetTimer(host, id, (UINT)std::max(1, interval), nullptr);
    enabled = true;
}

void Timer::Stop() {
    if (!enabled) return;
    if (host) KillTimer(host, id);
    TimerMap().erase(id);
    enabled = false;
}

namespace Ui {

HINSTANCE Instance() { return GetModuleHandleW(nullptr); }
HWND Host() { return host; }

ATOM RegisterClass(const wchar_t* name, WNDPROC proc, UINT style, HCURSOR cursor) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = Instance();
    wc.hCursor = cursor ? cursor : LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name;
    wc.hIcon = LoadIconW(Instance(), MAKEINTRESOURCEW(1));
    ATOM a = RegisterClassExW(&wc);
    return a;
}

void Init() {
    if (host) return;
    RegisterClass(L"GlimpseHost", HostProc);
    host = CreateWindowExW(0, L"GlimpseHost", L"Glimpse host", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, Instance(), nullptr);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);
}

void Post(std::function<void()> fn) {
    if (!host) { if (fn) fn(); return; }
    auto* p = new std::function<void()>(std::move(fn));
    if (!PostMessageW(host, kPostMsg, 0, (LPARAM)p)) delete p;
}

void Later(int ms, std::function<void()> fn) {
    if (!host) return;
    UINT_PTR id = nextTimerId++;
    OnceMap()[id] = std::move(fn);
    SetTimer(host, id, (UINT)std::max(1, ms), nullptr);
}

void StartGdiplus() {
    if (gdiplusToken) return;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&gdiplusToken, &in, nullptr);
}

void StopGdiplus() {
    if (!gdiplusToken) return;
    Draw::Shutdown();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    gdiplusToken = 0;
}

int RunLoop() {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        bool handled = false;
        for (HWND d : Dialogs()) {
            if (IsWindow(d) && (m.hwnd == d || IsChild(d, m.hwnd)) && IsDialogMessageW(d, &m)) { handled = true; break; }
        }
        if (handled) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}

void Quit(int code) { PostQuitMessage(code); }

void AddDialog(HWND h) { Dialogs().push_back(h); }

void RemoveDialog(HWND h) {
    auto& d = Dialogs();
    d.erase(std::remove(d.begin(), d.end(), h), d.end());
}

} // namespace Ui

// ================================================================== monitors

namespace Monitors {

static BOOL CALLBACK EnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM data) {
    auto* list = (std::vector<MonitorInfo>*)data;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(hmon, &mi)) {
        MonitorInfo m;
        m.handle = hmon;
        m.bounds = Rc::From(mi.rcMonitor);
        m.work = Rc::From(mi.rcWork);
        m.dpi = Native::DpiForMonitor(hmon);
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        m.device = mi.szDevice;
        list->push_back(m);
    }
    return TRUE;
}

std::vector<MonitorInfo> Read() {
    std::vector<MonitorInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, EnumProc, (LPARAM)&list);
    if (list.empty()) {
        MonitorInfo m;
        m.bounds = m.work = Rc(0, 0, 1024, 768);
        m.primary = true;
        list.push_back(m);
    }
    std::stable_sort(list.begin(), list.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.bounds.L != b.bounds.L) return a.bounds.L < b.bounds.L;
        return a.bounds.T < b.bounds.T;
    });
    return list;
}

wstr Signature(const std::vector<MonitorInfo>& list) {
    wstr s;
    for (const MonitorInfo& m : list) s += m.bounds.ToString() + L"|" + m.work.ToString() + L"|" + Util::Inv(m.dpi) + L";";
    return s;
}

const MonitorInfo* At(const std::vector<MonitorInfo>& list, const Rc& r) {
    if (list.empty()) return nullptr;
    std::vector<GeoMonitor> geo;
    for (const MonitorInfo& m : list) geo.push_back(GeoMonitor(m.bounds, m.work, m.dpi));
    return &list[Geometry::MonitorOf(r, geo)];
}

const MonitorInfo* AtPoint(const std::vector<MonitorInfo>& list, int x, int y) {
    for (const MonitorInfo& m : list) if (m.bounds.Contains(x, y)) return &m;
    return list.empty() ? nullptr : At(list, Rc(x, y, x + 1, y + 1));
}

} // namespace Monitors

// ================================================================== window registry

static const std::set<wstr>& ShellClasses() {
    static const std::set<wstr> s = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW", L"NotifyIconOverflowWindow",
        L"TopLevelWindowForOverflowXamlIsland", L"XamlExplorerHostIslandWindow", L"Windows.UI.Core.CoreWindow",
        L"MultitaskingViewFrame", L"ForegroundStaging", L"Shell_InputSwitchTopLevelWindow",
        L"ApplicationManager_ImmersiveShellWindow", L"EdgeUiInputTopWndClass", L"Internet Explorer_Hidden",
        L"#32768", L"#32770_hidden", L"tooltips_class32", L"SysShadow", L"ThumbnailDeviceHelperWnd",
        L"Windows.Internal.Shell.TabProxyWindow", L"DummyDWMListenerWindow", L"IME", L"MSCTFIME UI",
        L"Shell_LightDismissOverlay", L"Windows.UI.Input.InputSite.WindowClass", L"SnapAssistFlyout",
        L"TaskListThumbnailWnd", L"TaskListOverlayWnd", L"CEF-OSC-WIDGET", L"PseudoConsoleWindow"
    };
    return s;
}

WindowRegistry::WindowRegistry() : ownPid(GetCurrentProcessId()), ownIntegrity(Native::IntegrityLevel(0)) {}

WinPtr WindowRegistry::Get(HWND h) const {
    auto it = all.find(h);
    return it == all.end() ? WinPtr() : it->second;
}

bool WindowRegistry::IsAppWindow(HWND h, wstr& cls) const {
    cls.clear();
    if (!IsWindowVisible(h)) return false;
    if (GetAncestor(h, GA_ROOT) != h) return false;
    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (style & WS_CHILD) return false;
    bool app = (ex & WS_EX_APPWINDOW) != 0;
    if (!app) {
        if (ex & WS_EX_TOOLWINDOW) return false;
        if (ex & WS_EX_NOACTIVATE) return false;
        if (GetWindow(h, GW_OWNER)) return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == ownPid) return false;
    cls = Native::ClassName(h);
    if (ShellClasses().count(cls)) return false;
    if (!app && GetWindowTextLengthW(h) == 0) return false;
    if (Native::IsCloaked(h)) return false;
    return true;
}

wstr WindowRegistry::ProcessName(DWORD pid) {
    auto it = processNames.find(pid);
    if (it != processNames.end()) return it->second;
    wstr n = Native::ProcessFileName(pid);
    processNames[pid] = n;
    return n;
}

bool WindowRegistry::IsElevated(DWORD pid) {
    int il;
    auto it = integrity.find(pid);
    if (it == integrity.end()) { il = Native::IntegrityLevel(pid); integrity[pid] = il; }
    else il = it->second;
    if (ownIntegrity <= 0) return false;
    return il < 0 || il > ownIntegrity;
}

namespace {
struct EnumCtx {
    WindowRegistry* reg;
    const Settings* s;
    const std::vector<MonitorInfo>* mons;
    std::set<HWND> seen;
    long long z = 0;
    std::vector<HWND> order;
};
BOOL CALLBACK EnumWindowsCb(HWND h, LPARAM lp) {
    ((EnumCtx*)lp)->order.push_back(h);
    return TRUE;
}
}

void WindowRegistry::Refresh(const Settings& s, const std::vector<MonitorInfo>& mons) {
    EnumCtx ctx;
    ctx.reg = this;
    ctx.s = &s;
    ctx.mons = &mons;
    probeBudgetMs = Clock::Ms() + 120;
    EnumWindows(EnumWindowsCb, (LPARAM)&ctx);
    for (HWND h : ctx.order) {
        wstr cls;
        if (!IsAppWindow(h, cls)) continue;
        ctx.seen.insert(h);
        WinPtr w = Get(h);
        if (!w) {
            w = std::make_shared<TrackedWindow>();
            w->handle = h;
            w->cls = cls;
            GetWindowThreadProcessId(h, &w->pid);
            w->process = ProcessName(w->pid);
            w->elevated = IsElevated(w->pid);
            all[h] = w;
        }
        w->zOrder = ctx.z++;
        Update(*w, &s, mons);
    }
    std::vector<HWND> gone;
    for (const auto& kv : all) if (!ctx.seen.count(kv.first)) gone.push_back(kv.first);
    for (HWND h : gone) all.erase(h);
    if (!gone.empty()) {
        std::set<DWORD> live;
        for (const auto& kv : all) live.insert(kv.second->pid);
        std::vector<DWORD> pids;
        for (const auto& kv : processNames) pids.push_back(kv.first);
        for (DWORD pid : pids) if (!live.count(pid)) { processNames.erase(pid); integrity.erase(pid); }
    }
}

void WindowRegistry::Update(TrackedWindow& w, const Settings* s, const std::vector<MonitorInfo>& mons) {
    HWND h = w.handle;
    w.title = Native::WindowText(h);
    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    w.minimized = IsIconic(h) != FALSE;
    w.wasMaximized = w.maximized;
    w.maximized = IsZoomed(h) != FALSE;
    w.topmost = (ex & WS_EX_TOPMOST) != 0;
    w.resizable = (style & WS_THICKFRAME) != 0;
    w.hasCaption = (style & WS_CAPTION) == WS_CAPTION;
    w.hung = IsHungAppWindow(h) != FALSE;
    w.excluded = s && s->IsExcluded(w.process, w.title);
    RECT r;
    if (GetWindowRect(h, &r)) w.windowRect = Rc::From(r);
    Rc f;
    if (Native::TryFrame(h, f)) w.frame = f;
    const MonitorInfo* mon = Monitors::At(mons, w.frame);
    w.dpi = mon ? mon->dpi : 96;
    w.fullScreen = !w.maximized && mon && w.frame.Contains(mon->bounds) && !w.hasCaption;
    w.counted = !w.minimized;
    if (w.counted && !w.hung && w.frame.W() != w.topMapWidth && Clock::Ms() < probeBudgetMs) ProbeTopEdge(w);
}

void WindowRegistry::Touch(HWND h) {
    WinPtr w = Get(h);
    if (w) w->lastActive = Clock::Ms();
}

static bool CompareMru(const WinPtr& a, const WinPtr& b) {
    if (a->lastActive != b->lastActive) return a->lastActive > b->lastActive;
    return a->zOrder < b->zOrder;
}

std::vector<WinPtr> WindowRegistry::CountedByMru() const {
    std::vector<WinPtr> list;
    for (const auto& kv : all) if (kv.second->counted) list.push_back(kv.second);
    std::stable_sort(list.begin(), list.end(), CompareMru);
    return list;
}

std::vector<WinPtr> WindowRegistry::AllByMru() const {
    std::vector<WinPtr> list;
    for (const auto& kv : all) list.push_back(kv.second);
    std::stable_sort(list.begin(), list.end(), CompareMru);
    return list;
}

static int HitQuality(int ht) {
    switch (ht) {
        case HTCAPTION: case HTTOP: case HTTOPLEFT: case HTTOPRIGHT: case HTBORDER: return 3;
        case HTSYSMENU: return 1;
        case HTMINBUTTON: case HTMAXBUTTON: case HTCLOSE: case HTHELP: case HTMENU: return 0;
        default: return 2;   // client area, nowhere, transparent
    }
}

static LPARAM PointParam(POINT pt) { return MAKELPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y); }

void WindowRegistry::ProbeTopEdge(TrackedWindow& w) {
    Rc f = w.frame;
    w.topMapWidth = f.W();
    w.topMap.reset();
    if (f.W() < 40 || f.H() < 20) return;
    auto map = std::make_shared<TopEdgeMap>();
    int y = f.T + Util::Scale(12, w.dpi);
    int step = std::max(Util::Scale(16, w.dpi), f.W() / 24);
    int capL = INT_MAX, capR = INT_MIN;
    RECT cb;
    if (DwmGetWindowAttribute(w.handle, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof cb) == S_OK && cb.right > cb.left) {
        // relative to the window rectangle
        capL = w.windowRect.L + cb.left;
        capR = w.windowRect.L + cb.right;
        w.captionH = std::max(Util::Scale(24, w.dpi), (int)(cb.bottom - cb.top));
    }
    int prevQ = -1;
    for (int x = f.L + 2; x < f.R; x += step) {
        int q;
        if (x >= capL && x < capR) q = 0;
        else {
            POINT pt = { x, y };
            PhysicalToLogicalPointForPerMonitorDPI(w.handle, &pt);
            DWORD_PTR res = 0;
            if (!SendMessageTimeoutW(w.handle, WM_NCHITTEST, 0, PointParam(pt), SMTO_ABORTIFHUNG, 40, &res)) { w.topMap.reset(); return; }
            q = HitQuality((int)(LRESULT)res);
        }
        if (q != prevQ) { map->Add(std::max(0, x - f.L - (prevQ < 0 ? 2 : step / 2)), q); prevQ = q; }
        if (Clock::Ms() > probeBudgetMs + 200) break;
    }
    if (!map->starts.empty()) {
        map->starts[0] = 0;
        w.topMap = map;
    }
}

Gdiplus::Bitmap* WindowRegistry::LoadIcon(TrackedWindow& w, int size) {
    if (w.iconTried || !w.handle) return w.icon.get();
    w.iconTried = true;
    HICON hIcon = nullptr;
    const int kinds[] = { ICON_SMALL2, ICON_SMALL, ICON_BIG };
    for (int kind : kinds) {
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(w.handle, WM_GETICON, kind, 0, SMTO_ABORTIFHUNG, 50, &res) && res) { hIcon = (HICON)res; break; }
    }
    if (!hIcon) hIcon = (HICON)GetClassLongPtrW(w.handle, GCLP_HICONSM);
    if (!hIcon) hIcon = (HICON)GetClassLongPtrW(w.handle, GCLP_HICON);
    if (!hIcon) return nullptr;
    w.icon = Draw::IconBitmap(hIcon, size);
    return w.icon.get();
}

// ================================================================== event intake

namespace {
std::function<void(DWORD, HWND)> eventHandler;

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD evt, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    try { if (eventHandler) eventHandler(evt, hwnd); }
    catch (const std::exception& ex) { Log::Error(L"event: " + Util::Widen(ex.what())); }
}
}

EventIntake::EventIntake(std::function<void(DWORD, HWND)> handler) {
    eventHandler = std::move(handler);
    const DWORD ranges[][2] = {
        { EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
        { EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND },
        { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND },
        { EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE },
        { EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE },
        { EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED },
    };
    for (const auto& r : ranges) {
        HWINEVENTHOOK h = SetWinEventHook(r[0], r[1], nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!h) {
            wchar_t buf[32];
            swprintf(buf, 32, L"0x%04X", (unsigned)r[0]);
            Log::Warn(wstr(L"SetWinEventHook failed for ") + buf);
        } else hooks.push_back(h);
    }
}

EventIntake::~EventIntake() {
    for (HWINEVENTHOOK h : hooks) UnhookWinEvent(h);
    hooks.clear();
    eventHandler = nullptr;
}

// ================================================================== undo, actuator

void UndoStack::Push(const UndoPtr& g) {
    if (!g || g->items.empty()) return;
    g->time = Clock::Ms();
    groups.push_back(g);
    if (groups.size() > 20) groups.erase(groups.begin());
}

bool UndoStack::Remove(const UndoPtr& g) {
    auto it = std::find(groups.begin(), groups.end(), g);
    if (it == groups.end()) return false;
    groups.erase(it);
    return true;
}

bool UndoStack::Contains(const UndoPtr& g) const { return std::find(groups.begin(), groups.end(), g) != groups.end(); }

namespace Actuator {

bool MoveFrame(TrackedWindow& w, const Rc& frame, UndoGroup* undo, bool& accessDenied) {
    accessDenied = false;
    RECT r;
    if (!GetWindowRect(w.handle, &r)) return false;
    Rc wr = Rc::From(r);
    Rc fr;
    if (!Native::TryFrame(w.handle, fr)) fr = wr;
    int dl = fr.L - wr.L, dt = fr.T - wr.T, dr = wr.R - fr.R, db = wr.B - fr.B;
    Rc target(frame.L - dl, frame.T - dt, frame.R + dr, frame.B + db);
    if (!SetRect(&w, w.handle, wr, target, accessDenied)) return false;
    if (undo) {
        // one entry per window: the first "before" and the last "after"
        UndoItem* item = nullptr;
        for (UndoItem& it : undo->items) if (it.handle == w.handle) item = &it;
        if (!item) {
            UndoItem it;
            it.handle = w.handle;
            it.before = wr;
            it.after = target;
            it.title = w.ShortTitle();
            undo->items.push_back(it);
        } else item->after = target;
    }
    w.frame = frame;
    w.windowRect = target;
    return true;
}

bool SetRect(TrackedWindow* w, HWND h, const Rc& current, const Rc& target, bool& accessDenied) {
    accessDenied = false;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
    if (target.W() == current.W() && target.H() == current.H()) flags |= SWP_NOSIZE;
    if (target.L == current.L && target.T == current.T) flags |= SWP_NOMOVE;
    if (w) w->suppressUntil = Clock::Ms() + OwnMoveQuietMs;
    SetLastError(0);
    if (SetWindowPos(h, nullptr, target.L, target.T, target.W(), target.H(), flags)) return true;
    DWORD err = GetLastError();
    accessDenied = err == ERROR_ACCESS_DENIED;
    Log::Warn(L"SetWindowPos failed (" + Util::Inv(err) + L") for " + (w ? w->process : wstr(L"?")));
    return false;
}

int UndoGroupNow(const UndoGroup& g, WindowRegistry& reg) {
    int n = 0;
    for (const UndoItem& it : g.items) {
        if (!IsWindow(it.handle)) continue;
        WinPtr w = reg.Get(it.handle);
        RECT r;
        if (!GetWindowRect(it.handle, &r)) continue;
        bool denied;
        if (w) w->placed = true;
        if (SetRect(w.get(), it.handle, Rc::From(r), it.before, denied)) n++;
    }
    return n;
}

} // namespace Actuator

// ================================================================== drag state

wstr DragState::Edges() const {
    switch (hit) {
        case HTLEFT: return L"L";
        case HTRIGHT: return L"R";
        case HTTOP: return L"T";
        case HTBOTTOM: return L"B";
        case HTTOPLEFT: return L"TL";
        case HTTOPRIGHT: return L"TR";
        case HTBOTTOMLEFT: return L"BL";
        case HTBOTTOMRIGHT: return L"BR";
        default: return L"";
    }
}

Rc DragState::Predict(POINT c) const {
    int dx = c.x - startCursor.x, dy = c.y - startCursor.y;
    Rc f = startFrame;
    if (!Resizing()) return f.Offset(dx, dy);
    wstr e = Edges();
    int min = Util::Scale(80, w ? w->dpi : 96);
    if (e.find(L'L') != wstr::npos) f.L = std::min(f.L + dx, f.R - min);
    if (e.find(L'R') != wstr::npos) f.R = std::max(f.R + dx, f.L + min);
    if (e.find(L'T') != wstr::npos) f.T = std::min(f.T + dy, f.B - min);
    if (e.find(L'B') != wstr::npos) f.B = std::max(f.B + dy, f.T + min);
    return f;
}

wstr DragState::ChangedEdges(const Rc& a, const Rc& b) {
    wstr s;
    if (a.L != b.L) s += L"L";
    if (a.T != b.T) s += L"T";
    if (a.R != b.R) s += L"R";
    if (a.B != b.B) s += L"B";
    return s;
}

// ================================================================== hotkey

void HotkeyManager::Apply(const wstr& text) {
    Unregister();
    problem.clear();
    if (text.empty()) return;
    HotkeySpec spec;
    if (!HotkeySpec::TryParse(text, spec)) { problem = text; return; }
    current = spec;
    registered = RegisterHotKey(owner, XRayId, spec.mods | MOD_NOREPEAT, spec.vk) != FALSE;
    if (!registered) {
        problem = spec.ToString();
        Log::Warn(L"hotkey " + spec.ToString() + L" is taken (error " + Util::Inv(GetLastError()) + L")");
    }
}

void HotkeyManager::Unregister() {
    if (registered) UnregisterHotKey(owner, XRayId);
    registered = false;
}

bool HotkeyManager::CanRegister(const HotkeySpec& spec) {
    if (registered && spec.mods == current.mods && spec.vk == current.vk) return true;
    bool ok = RegisterHotKey(owner, TestId, spec.mods | MOD_NOREPEAT, spec.vk) != FALSE;
    if (ok) UnregisterHotKey(owner, TestId);
    return ok;
}

// ================================================================== hot corner

HotCorner::HotCorner(std::function<void()> f) : timer(100), fire(std::move(f)) {
    timer.tick = [this]() { Tick(); };
}

void HotCorner::Set(Corner c) {
    which = c;
    since = -1;
    fired = false;
    if (c != Corner::Off) timer.Start(); else timer.Stop();
}

void HotCorner::Suspend(bool off) {
    if (!off && which != Corner::Off) timer.Start(); else timer.Stop();
}

void HotCorner::Tick() {
    POINT p;
    if (!GetCursorPos(&p)) return;
    bool inCorner = InCorner(p) && !Native::KeyDown(VK_LBUTTON) && !Native::KeyDown(VK_RBUTTON);
    if (!inCorner) { since = -1; fired = false; return; }
    if (since < 0) since = Clock::Ms();
    if (!fired && Clock::Ms() - since >= 400) {
        fired = true;
        if (fire) fire();
    }
}

bool HotCorner::InCorner(POINT p) const {
    HMONITOR hmon = MonitorFromPoint(p, MONITOR_DEFAULTTONULL);
    if (!hmon) return false;
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(hmon, &mi)) return false;
    Rc b = Rc::From(mi.rcMonitor);
    int tol = 2;
    bool left = p.x <= b.L + tol, right = p.x >= b.R - 1 - tol, top = p.y <= b.T + tol, bottom = p.y >= b.B - 1 - tol;
    int ox, oy;
    switch (which) {
        case Corner::TopLeft: if (!(left && top)) return false; ox = b.L - 1; oy = b.T - 1; break;
        case Corner::TopRight: if (!(right && top)) return false; ox = b.R; oy = b.T - 1; break;
        case Corner::BottomLeft: if (!(left && bottom)) return false; ox = b.L - 1; oy = b.B; break;
        case Corner::BottomRight: if (!(right && bottom)) return false; ox = b.R; oy = b.B; break;
        default: return false;
    }
    // only an outer corner of the desktop, where the cursor stops
    POINT a = { ox, p.y }, c = { p.x, oy };
    if (MonitorFromPoint(a, MONITOR_DEFAULTTONULL)) return false;
    if (MonitorFromPoint(c, MONITOR_DEFAULTTONULL)) return false;
    return true;
}

// ================================================================== Start with Windows

namespace StartWithWindows {
void Sync(bool enabled) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        Log::Warn(L"Start with Windows: cannot open the Run key");
        return;
    }
    wstr want = Program::StartCommandLine();
    wchar_t buf[4096];
    DWORD type = 0, size = sizeof buf;
    bool have = RegQueryValueExW(k, L"Glimpse", nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
    wstr value = have ? wstr(buf, size / sizeof(wchar_t)) : wstr();
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    if (enabled) {
        if (!have || value != want) {
            DWORD t = want.find(L'%') != wstr::npos ? REG_EXPAND_SZ : REG_SZ;
            if (RegSetValueExW(k, L"Glimpse", 0, t, (const BYTE*)want.c_str(), (DWORD)((want.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS)
                Log::Info(L"Start with Windows: " + want);
            else Log::Warn(L"Start with Windows: cannot write the Run key");
        }
    } else if (have && Util::Lower(value).find(Util::Lower(Program::launchPath)) != wstr::npos) {
        RegDeleteValueW(k, L"Glimpse");
        Log::Info(L"Start with Windows removed");
    }
    RegCloseKey(k);
}
}

// ================================================================== size memory

static const size_t kMaxSizes = 300;

wstr SizeMemory::Path() const { return Util::Combine(Log::Folder(), L"Memory.ini"); }

void SizeMemory::Load() {
    std::string data;
    if (Util::FileExists(Path()) && Util::ReadFileBytes(Path(), data)) ini = IniFile::Parse(data);
    else ini = IniFile::FromText(L"; Glimpse size memory\r\n[Sizes]\r\n");
    loaded = true;
}

wstr SizeMemory::KindOf(const TrackedWindow& w) {
    return Util::Lower(w.process + L"|" + w.cls + L"|" + Util::TitleShape(w.title));
}

static wstr SizeKey(const wstr& kind) { return Util::Widen(Util::Sha1Hex(Util::Narrow(kind)).substr(0, 16)); }

void SizeMemory::Remember(const TrackedWindow& w, const Rc& frame) {
    if (!loaded) Load();
    int lw = Util::RoundEven(frame.W() * 96.0 / w.dpi), lh = Util::RoundEven(frame.H() * 96.0 / w.dpi);
    wstr kind = KindOf(w);
    wstr key = SizeKey(kind);
    ini.RemoveKey(L"Sizes", key);   // re-insert last: the file is in least-recently-used order
    ini.Set(L"Sizes", key, Util::Inv(lw) + L"," + Util::Inv(lh) + L";" + Util::Replace(Util::Replace(kind, L"\r", L" "), L"\n", L" "));
    auto entries = ini.Entries(L"Sizes");
    for (size_t i = 0; i + kMaxSizes < entries.size(); i++) ini.RemoveKey(L"Sizes", entries[i].first);
    dirty = true;
}

bool SizeMemory::TryGet(const TrackedWindow& w, int& width, int& height) {
    width = height = 0;
    if (!loaded) Load();
    wstr v;
    if (!ini.Get(L"Sizes", SizeKey(KindOf(w)), v)) return false;
    size_t semi = v.find(L';');
    std::vector<wstr> p = Util::Split(semi != wstr::npos ? v.substr(0, semi) : v, L',');
    if (p.size() != 2) return false;
    int lw = Util::ParseInt(&p[0], 0, 0, 20000), lh = Util::ParseInt(&p[1], 0, 0, 20000);
    if (lw <= 0 || lh <= 0) return false;
    width = Util::Scale(lw, w.dpi);
    height = Util::Scale(lh, w.dpi);
    return true;
}

void SizeMemory::SaveIfDirty() {
    if (!dirty || !loaded) return;
    Util::CreateDirs(Util::DirName(Path()));
    if (Util::WriteFileBytes(Path(), ini.ToBytes())) dirty = false;
    else Log::Warn(L"cannot write " + Path());
}

// ================================================================== layout memory

void LayoutMemory::Save(const wstr& signature, const std::map<HWND, WinPtr>& windows) {
    std::map<HWND, Rc> d;
    for (const auto& kv : windows) {
        const TrackedWindow& w = *kv.second;
        if (w.counted && !w.maximized && !w.fullScreen) d[w.handle] = w.windowRect;
    }
    if (!d.empty()) bySignature[signature] = d;
}

const std::map<HWND, Rc>* LayoutMemory::Get(const wstr& signature) const {
    auto it = bySignature.find(signature);
    return it == bySignature.end() ? nullptr : &it->second;
}

// ================================================================== first-click mode (experimental)

static FirstClickHook* firstClickInstance = nullptr;

FirstClickHook::FirstClickHook(std::function<HWND(int, int)> owner, std::function<void(HWND)> act)
    : zoneOwnerAt(std::move(owner)), activate(std::move(act)) {
    firstClickInstance = this;
    hook = SetWindowsHookExW(WH_MOUSE_LL, Proc, GetModuleHandleW(nullptr), 0);
    if (!hook) Log::Warn(L"first-click mode: mouse hook not available");
}

FirstClickHook::~FirstClickHook() {
    if (hook) UnhookWindowsHookEx(hook);
    hook = nullptr;
    if (firstClickInstance == this) firstClickInstance = nullptr;
}

LRESULT CALLBACK FirstClickHook::Proc(int code, WPARAM wp, LPARAM lp) {
    FirstClickHook* self = firstClickInstance;
    if (self && code >= 0) {
        try {
            if (wp == WM_LBUTTONDOWN) {
                const MSLLHOOKSTRUCT* info = (const MSLLHOOKSTRUCT*)lp;
                HWND target = self->zoneOwnerAt(info->pt.x, info->pt.y);
                if (target && target != GetForegroundWindow()) {
                    self->swallowUp = true;
                    self->activate(target);
                    return 1;
                }
            } else if (wp == WM_LBUTTONUP && self->swallowUp) {
                self->swallowUp = false;
                return 1;
            }
        } catch (...) {}
    }
    return CallNextHookEx(self ? self->hook : nullptr, code, wp, lp);
}

// ================================================================== message window, single instance

MessageWindow::MessageWindow(std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> h) : handler(std::move(h)) {
    static bool registered = false;
    if (!registered) { Ui::RegisterClass(L"GlimpseMessageWindow", Proc); registered = true; }
    // hidden top-level window (not message-only: broadcasts such as WM_DISPLAYCHANGE reach top-level windows only)
    handle = CreateWindowExW(WS_EX_TOOLWINDOW, L"GlimpseMessageWindow", Ipc::WindowTitle, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, Ui::Instance(), this);
}

MessageWindow::~MessageWindow() {
    if (handle) {
        SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
        DestroyWindow(handle);
    }
    handle = nullptr;
}

LRESULT CALLBACK MessageWindow::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    LRESULT r = 0;
    if (PopupMenu::OwnerDraw(m, wp, lp, r)) return r;
    auto* self = (MessageWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self && self->handler) {
        try { if (self->handler(m, wp, lp, r)) return r; }
        catch (const std::exception& ex) { Log::Error(L"message: " + Util::Widen(ex.what())); }
    }
    return DefWindowProcW(h, m, wp, lp);
}

namespace Ipc {
const wchar_t* const WindowTitle = L"Glimpse.MessageWindow.9C1D6A52";

UINT Message() {
    static UINT msg = 0;
    if (!msg) msg = RegisterWindowMessageW(L"Glimpse.Command.9C1D6A52");
    return msg;
}

void SignalRunningInstance(int cmd) {
    for (int attempt = 0; attempt < 20; attempt++) {
        HWND h = FindWindowW(nullptr, WindowTitle);
        if (h) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(h, Message(), (WPARAM)cmd, 0);
            return;
        }
        Sleep(100);   // the other instance may still be starting
    }
}
}

} // namespace glimpse
