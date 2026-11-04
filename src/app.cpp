// Glimpse native build: the coordinator (one Glimpse process per session) and the drag scene.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"

namespace glimpse {

GlimpseApp* GlimpseApp::current = nullptr;

enum { PShown = 1, PRestored = 2, PMoved = 4, PTitle = 8, PGone = 16, PMinimized = 32, PCloak = 64 };

static const unsigned long long kMinuteTicks = 600000000ULL, kDayTicks = 864000000000ULL;

static unsigned long long Ticks(const SYSTEMTIME& st) {
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static unsigned long long NowLocal() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Ticks(st);
}

static unsigned long long TodayMidnight() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    return Ticks(st);
}

static const wchar_t* CauseName(Cause c) {
    switch (c) {
        case Cause::Check: return L"Check";
        case Cause::Drop: return L"Drop";
        case Cause::New: return L"New";
        case Cause::Restore: return L"Restore";
        case Cause::AppMove: return L"AppMove";
        case Cause::AppResize: return L"AppResize";
        case Cause::Display: return L"Display";
        case Cause::Arrange: return L"Arrange";
        default: return L"GiveGlimpse";
    }
}

static const wchar_t* ModeName(FixMode m) { return m == FixMode::Guide ? L"Guide" : (m == FixMode::Strict ? L"Strict" : L"Assist"); }

static const wchar_t* CornerName(Corner c) {
    switch (c) {
        case Corner::TopLeft: return L"TopLeft";
        case Corner::TopRight: return L"TopRight";
        case Corner::BottomLeft: return L"BottomLeft";
        case Corner::BottomRight: return L"BottomRight";
        default: return L"Off";
    }
}

static wstr Hex(HWND h) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%llX", (unsigned long long)(uintptr_t)h);
    return buf;
}

// ================================================================== construction

GlimpseApp::GlimpseApp()
    : debounce(150), settle(1500), safety(10000), readback(250), dragTimer(33), dropTimer(120), iniPoll(2000) {
    current = this;
    wchar_t tv[8];
    trace = GetEnvironmentVariableW(L"GLIMPSE_TRACE", tv, 8) == 1 && tv[0] == L'1';
    store.Load(Program::opts.iniArgument);
    Log::SetDebug(S().debugLog);
    Log::Info(wstr(L"Glimpse ") + kVersion + L" (native), " + Util::Inv((long long)sizeof(void*) * 8) + L"-bit, settings " + store.filePath +
              (store.writable ? L"" : L" (read-only)") + (store.usingDefaults ? L" (built-in defaults)" : L""));
    Texts::Load(S().language);
    msg.reset(new MessageWindow([this](UINT m, WPARAM wp, LPARAM lp, LRESULT& r) { return HandleMessage(m, wp, lp, r); }));
    mons = Monitors::Read();
    layoutSig = Monitors::Signature(mons);
    overlays.reset(new OverlayManager(this));
    xray.reset(new XRayView(this));
    hotkeys.reset(new HotkeyManager(msg->handle));
    hotkeys->Apply(S().hotkey);
    corner.reset(new HotCorner([this]() { ShowXRay(false); }));
    corner->Set(S().hotCorner);
    tray.reset(new TrayUi(this));
    sizes.Load();
    if (!WTSRegisterSessionNotification(msg->handle, NOTIFY_FOR_THIS_SESSION)) Log::Warn(L"session notifications are not available");
    reg.Refresh(S(), mons);
    for (auto& kv : reg.all) { kv.second->placed = true; kv.second->settledFrame = kv.second->frame; }
    intake.reset(new EventIntake([this](DWORD evt, HWND h) { OnWinEvent(evt, h); }));
    debounce.tick = [this]() { ProcessPending(); };
    settle.tick = [this]() { OnDisplaySettled(); };
    safety.tick = [this]() { OnSafetyTick(); };
    readback.tick = [this]() { OnReadback(); };
    dragTimer.tick = [this]() { OnDragTick(); };
    dropTimer.tick = [this]() { OnDropSettled(); };
    iniPoll.tick = [this]() { if (!disposed) store.Poll(); };
    safety.Start();
    iniPoll.Start();
    store.changed = [this]() { ApplySettings(true); };
    StartWithWindows::Sync(S().startWithWindows);
    ApplyFirstClick();
    UpdateStatus();
    layouts.Save(layoutSig, reg.all);
    Log::Info(Util::Inv((long long)mons.size()) + L" monitor(s), " + Util::Inv(guardedCount) + L" window(s) guarded, mode " + ModeName(S().mode) +
              L", hotkey " + (hotkeys->registered ? hotkeys->current.ToString() : wstr(L"none")) + L", hot corner " + CornerName(S().hotCorner));
    if (Program::opts.showSettings) ShowSettings();
}

GlimpseApp::~GlimpseApp() {
    Shutdown();
    if (current == this) current = nullptr;
}

// ================================================================== state

bool GlimpseApp::Paused() const { return pausedUntil != 0 && NowLocal() < pausedUntil; }

bool GlimpseApp::PausedUntilTomorrow() const { return pausedUntil >= TodayMidnight() + kDayTicks; }

wstr GlimpseApp::PausedUntilText() const {
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(pausedUntil & 0xFFFFFFFF);
    ft.dwHighDateTime = (DWORD)(pausedUntil >> 32);
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    wchar_t buf[64];
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, 64) > 0) return buf;
    swprintf(buf, 64, L"%02d:%02d", st.wHour, st.wMinute);
    return buf;
}

void GlimpseApp::Pause(int minutes, bool untilTomorrow) {
    pausedUntil = untilTomorrow ? TodayMidnight() + kDayTicks : NowLocal() + (unsigned long long)minutes * kMinuteTicks;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(pausedUntil & 0xFFFFFFFF);
    ft.dwHighDateTime = (DWORD)(pausedUntil >> 32);
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    Log::Info(wstr(L"paused until ") + buf);
    overlays->HideDrag();
    tray->Update();
}

void GlimpseApp::Resume() {
    pausedUntil = 0;
    suspendedUntil = 0;
    recentMoves.clear();
    Log::Info(L"resumed");
    tray->Update();
}

// ================================================================== messages

bool GlimpseApp::HandleMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT& result) {
    switch (m) {
        case WM_DISPLAYCHANGE:
            DisplaySignal(L"display change");
            return false;
        case WM_SETTINGCHANGE:
            DisplaySignal(wp == SPI_SETWORKAREA ? L"work area" : L"setting");
            Theme::Refresh();
            return false;
        case WM_WTSSESSION_CHANGE:
            OnSession((int)wp);
            result = 0;
            return true;
        case WM_HOTKEY:
            if ((int)wp == HotkeyManager::XRayId) OnHotkey();
            result = 0;
            return true;
    }
    if (m == Ipc::Message()) {
        int cmd = (int)wp;
        if (cmd == Ipc::CmdSettings) ShowSettings();
        else if (cmd == Ipc::CmdExit) ExitApp();
        else if (cmd == Ipc::CmdXRay) ShowXRay(true);
        result = 0;
        return true;
    }
    return false;
}

void GlimpseApp::OnHotkey() {
    if (hotkeys->testArrived) {
        std::function<void()> test = hotkeys->testArrived;
        hotkeys->testArrived = nullptr;
        test();
        return;
    }
    if (xray->IsOpen()) xray->Close(); else ShowXRay(true);
}

void GlimpseApp::OnSession(int code) {
    switch (code) {
        case WTS_SESSION_LOCK:
        case WTS_REMOTE_DISCONNECT:
        case WTS_CONSOLE_DISCONNECT:
            if (!sessionActive) return;
            sessionActive = false;
            Log::Info(wstr(L"session ") + (code == WTS_SESSION_LOCK ? L"locked" : L"disconnected") + L": idle");
            CancelDrag();
            xray->Close();
            overlays->HideAll();
            corner->Suspend(true);
            tray->Update();
            break;
        case WTS_SESSION_UNLOCK:
        case WTS_REMOTE_CONNECT:
        case WTS_CONSOLE_CONNECT:
            if (sessionActive && code == WTS_SESSION_UNLOCK) return;
            sessionActive = true;
            Log::Info(wstr(L"session ") + (code == WTS_SESSION_UNLOCK ? L"unlocked" : L"connected"));
            corner->Suspend(false);
            DisplaySignal(L"session");
            tray->Update();
            break;
    }
}

// ================================================================== window events

void GlimpseApp::OnWinEvent(DWORD evt, HWND h) {
    if (trace) {
        WinPtr tw = reg.Get(h);
        wchar_t buf[64];
        swprintf(buf, 64, L"event 0x%04X ", (unsigned)evt);
        Log::Info(buf + Hex(h) + (tw ? L" " + tw->process : wstr()) + L" lbutton=" + (Native::KeyDown(VK_LBUTTON) ? L"True" : L"False") +
                  L" fg=" + (GetForegroundWindow() == h ? L"True" : L"False"));
    }
    if (disposed || !sessionActive) return;
    switch (evt) {
        case EVENT_SYSTEM_FOREGROUND:
            reg.Touch(h);
            return;
        case EVENT_SYSTEM_MOVESIZESTART:
            BeginDrag(h, false);
            return;
        case EVENT_SYSTEM_MOVESIZEEND:
            EndDrag(h);
            return;
        case EVENT_OBJECT_LOCATIONCHANGE: {
            WinPtr w = reg.Get(h);
            if (!w) return;
            if (drag && drag->handle == h) return;          // the drag timer reads it
            if (Clock::Ms() < w->suppressUntil) return;     // our own move
            // apps with their own drag code (no system move loop, no MOVESIZESTART):
            // the foreground window moving while the left button is down is a user drag
            if (!drag && !dropping && !w->maximized && Native::KeyDown(VK_LBUTTON) && GetForegroundWindow() == h) {
                BeginDrag(h, true);
                return;
            }
            AddPending(h, PMoved);
            return;
        }
        case EVENT_OBJECT_SHOW:
            if (GetAncestor(h, GA_ROOT) == h) AddPending(h, PShown);
            return;
        case EVENT_OBJECT_UNCLOAKED:
        case EVENT_OBJECT_CLOAKED:
            if (GetAncestor(h, GA_ROOT) == h) AddPending(h, PCloak);
            return;
        case EVENT_SYSTEM_MINIMIZEEND:
            AddPending(h, PRestored);
            return;
        case EVENT_SYSTEM_MINIMIZESTART:
            if (reg.Get(h)) AddPending(h, PMinimized);
            return;
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_DESTROY:
            if (reg.Get(h)) AddPending(h, PGone);
            return;
        case EVENT_OBJECT_NAMECHANGE:
            if (reg.Get(h)) AddPending(h, PTitle);
            return;
    }
}

void GlimpseApp::AddPending(HWND h, int flag) {
    bool found = false;
    for (auto& p : pending) if (p.first == h) { p.second |= flag; found = true; break; }
    if (!found) pending.push_back(std::make_pair(h, flag));
    debounce.Start();
}

void GlimpseApp::RefreshRegistry() { reg.Refresh(S(), mons); }

void GlimpseApp::ProcessPending() {
    debounce.Stop();
    if (disposed || !sessionActive || drag || dropping) return;
    if (pending.empty()) return;
    std::vector<std::pair<HWND, int>> items;
    items.swap(pending);
    std::set<HWND> knownBefore;
    for (const auto& kv : reg.all) knownBefore.insert(kv.first);
    RefreshRegistry();
    for (const auto& kv : items) {
        WinPtr w = reg.Get(kv.first);
        if (!w) continue;
        int f = kv.second;
        if (!knownBefore.count(kv.first)) {
            // shown now: a new window (or one shown again); uncloaked ones only switched desktops
            w->placed = (f & PShown) == 0;
            w->settledFrame = w->frame;
        }
        if (!w->counted) { w->settledFrame = w->frame; continue; }
        try {
            if (!w->placed) {
                w->placed = true;
                NewWindow(w);
            } else if ((f & PRestored) != 0 || (w->wasMaximized && !w->maximized && (f & PMoved) != 0)) {
                RunAndApply(Cause::Restore, w, L"", Rc(), nullptr, UndoPtr());
            } else if ((f & PMoved) != 0 && !w->frame.Same(w->settledFrame) && !w->maximized && !w->fullScreen) {
                if (!DetectRevert(*w)) {
                    bool resized = w->frame.W() != w->settledFrame.W() || w->frame.H() != w->settledFrame.H();
                    RunAndApply(resized ? Cause::AppResize : Cause::AppMove, w, L"", Rc(), nullptr, UndoPtr());
                }
            } else if ((f & PTitle) != 0) {
                const MinSizeRule* rule = S().RuleFor(w->title);
                wstr key = rule ? rule->pattern : wstr();
                if (w->hasRuleKey && key != w->ruleKey) RunAndApply(Cause::AppResize, w, L"", Rc(), nullptr, UndoPtr());
                w->ruleKey = key;
                w->hasRuleKey = true;
            }
        } catch (const std::exception& ex) { Log::Error(L"processing " + w->process + L": " + Util::Widen(ex.what())); }
        w->settledFrame = w->frame;
        if (!w->hasRuleKey) {
            const MinSizeRule* r = S().RuleFor(w->title);
            w->ruleKey = r ? r->pattern : wstr();
            w->hasRuleKey = true;
        }
    }
    UpdateStatus();
}

// a window that moves back twice within 10 seconds after a fix is stubborn for the session
bool GlimpseApp::DetectRevert(TrackedWindow& w) {
    if (w.fixedAt == 0 || Clock::Ms() - w.fixedAt > 10000) return false;
    if (!w.frame.Near(w.fixedFrom, 4)) return false;
    return CountRevert(w, L"moved back");
}

bool GlimpseApp::CountRevert(TrackedWindow& w, const wstr& what) {
    long long now = Clock::Ms();
    w.reverts.erase(std::remove_if(w.reverts.begin(), w.reverts.end(), [now](long long t) { return now - t > 10000; }), w.reverts.end());
    w.reverts.push_back(now);
    if (w.reverts.size() >= 2 && !w.stubborn) {
        w.stubborn = true;
        Log::Info(w.process + L" " + what + L" twice within 10 s: left alone for this session");
    }
    return true;
}

void GlimpseApp::NewWindow(const WinPtr& w) {
    Rc start = w->frame;
    Rc sized;
    bool haveSized = false;
    int sw, sh;
    if (S().rememberSizes && w->resizable && !w->maximized && sizes.TryGet(*w, sw, sh) && (std::abs(sw - start.W()) > 2 || std::abs(sh - start.H()) > 2)) {
        const MonitorInfo* mon = Monitors::At(mons, start);
        if (mon) {
            Rc r = Rc::XYWH(start.L, start.T, std::min(sw, mon->work.W()), std::min(sh, mon->work.H()));
            sized = Geometry::ClampInto(r, mon->work);
            haveSized = true;
        }
    }
    RunAndApply(Cause::New, w, L"", Rc(), haveSized ? &sized : nullptr, UndoPtr());
}

// ================================================================== resolver glue

// The 30 (MaxWindows) most recently used windows count; the rest stay reachable through X-ray.
Layout GlimpseApp::BuildLayout(std::vector<WinPtr>& list, const WinPtr& target, const Rc* targetFrame) {
    Layout L;
    L.glimpseSize = S().glimpseSize;
    for (const MonitorInfo& m : mons) L.monitors.push_back(GeoMonitor(m.bounds, m.work, m.dpi));
    list = reg.CountedByMru();
    if (target && std::find(list.begin(), list.end(), target) == list.end() && target->counted) list.insert(list.begin(), target);
    int k = 0;
    for (const WinPtr& wp : list) {
        const TrackedWindow& w = *wp;
        Rc frame = wp == target && targetFrame ? *targetFrame : w.frame;
        GeoWindow g;
        g.id = Hex(w.handle);
        g.frame = frame;
        g.dpi = w.dpi;
        g.tag = wp.get();
        g.resizable = w.resizable && !w.maximized;
        g.captionW = w.captionW;
        g.captionH = w.captionH;
        g.iconW = w.iconW;
        g.topMap = w.topMap;
        g.prevWork = w.prevWork;
        int P = Geometry::GlimpsePx(S().glimpseSize, w.dpi);
        if (w.maximized || w.fullScreen) { g.subject = g.blocker = g.movable = false; g.fixedReason = L"maximized"; }
        else if (frame.W() < 2 * P || frame.H() < 2 * P) { g.subject = g.blocker = g.movable = false; g.fixedReason = L"small"; }
        else if (++k > S().maxWindows && wp != target) { g.subject = g.blocker = g.movable = false; g.fixedReason = L"limit"; }
        else if (w.topmost) { g.subject = false; g.movable = false; g.fixedReason = L"topmost"; }
        else if (w.elevated) { g.movable = false; g.fixedReason = L"elevated"; }
        else if (w.hung) { g.movable = false; g.fixedReason = L"hung"; }
        else if (w.stubborn) { g.movable = false; g.fixedReason = L"stubborn"; }
        else if (w.excluded) { g.movable = false; g.fixedReason = L"excluded"; }
        const MinSizeRule* rule = w.resizable ? S().RuleFor(w.title) : nullptr;
        if (rule) { g.minW = Util::Scale(rule->minWidth, w.dpi); g.minH = Util::Scale(rule->minHeight, w.dpi); }
        L.windows.push_back(g);
    }
    L.Begin();
    return L;
}

Outcome GlimpseApp::RunAndApply(Cause cause, const WinPtr& target, const wstr& edges, const Rc& anchorStart, const Rc* targetFrame, const UndoPtr& group) {
    std::vector<WinPtr> list;
    Layout L = BuildLayout(list, target, targetFrame);
    int ti = -1;
    if (target) {
        auto it = std::find(list.begin(), list.end(), target);
        if (it != list.end()) ti = (int)(it - list.begin());
    }
    Request q = Request::For(cause, S().mode, ti, S().placeNewWindows);
    q.draggedEdges = edges;
    q.anchorStart = anchorStart;
    if (!(sessionActive && !Paused() && !Suspended())) q.fix = false;
    Outcome o = Resolver::Resolve(L, q);
    if (!q.fix) return o;
    UndoPtr g = group;
    if (!g) { g = std::make_shared<UndoGroup>(); g->reason = CauseName(cause); }
    Apply(list, o, cause, *g);
    if (!group) undo.Push(g);
    return o;
}

void GlimpseApp::Apply(const std::vector<WinPtr>& list, const Outcome& o, Cause cause, UndoGroup& g) {
    std::vector<Rc> flashes;
    for (size_t i = 0; i < list.size(); i++) {
        TrackedWindow& w = *list[i];
        Rc to = o.frames[i];
        if (to.Same(w.frame)) continue;
        if (!RateOk()) { SuspendAutoFix(); break; }
        Rc from = w.frame;
        bool denied;
        if (Actuator::MoveFrame(w, to, &g, denied)) {
            recentMoves.push_back(Clock::Ms());
            w.fixedFrom = from;
            w.fixedTo = to;
            w.fixedAt = Clock::Ms();
            w.settledFrame = to;
            flashes.push_back(to);
            Log::Info(wstr(CauseName(cause)) + L": " + w.process + (Log::DebugEnabled() ? L" \"" + w.title + L"\"" : wstr()) + L" " + from.ToString() + L" -> " + to.ToString());
        } else if (denied) {
            w.elevated = true;
            Log::Info(L"cannot move " + w.process + L": treated as an admin window");
        }
    }
    if (!flashes.empty()) {
        overlays->Flash(flashes);
        for (const UndoItem& it : g.items) toVerify.push_back(it);
        readback.Start();
    }
}

bool GlimpseApp::RateOk() {
    long long now = Clock::Ms();
    recentMoves.erase(std::remove_if(recentMoves.begin(), recentMoves.end(), [now](long long t) { return now - t > 10000; }), recentMoves.end());
    return recentMoves.size() < 10;
}

void GlimpseApp::SuspendAutoFix() {
    if (Suspended()) return;
    suspendedUntil = Clock::Ms() + 5 * 60 * 1000;
    Log::Warn(L"more than 10 moves within 10 seconds: automatic fixes paused for 5 minutes");
    tray->Update();
}

// reads moved windows back: an app that refuses twice within 10 s is stubborn
void GlimpseApp::OnReadback() {
    readback.Stop();
    for (const UndoItem& it : toVerify) {
        WinPtr w = reg.Get(it.handle);
        if (!w || !IsWindow(it.handle)) continue;
        RECT r;
        if (!GetWindowRect(it.handle, &r)) continue;
        Rc now = Rc::From(r);
        if (!now.Near(it.after, 3)) {
            Log::Debug(w->process + L" did not take the new size or position");
            CountRevert(*w, L"refused a fix");
        }
        Rc f;
        if (Native::TryFrame(it.handle, f)) { w->frame = f; w->settledFrame = f; }
        w->windowRect = now;
    }
    toVerify.clear();
}

void GlimpseApp::UpdateStatus() {
    if (disposed) return;
    std::vector<WinPtr> list;
    Layout L = BuildLayout(list, WinPtr(), nullptr);
    Request check;
    check.fix = false;
    Outcome o = Resolver::Resolve(L, check);
    int hidden = 0, guarded = 0, over = 0;
    for (size_t i = 0; i < list.size(); i++) {
        TrackedWindow& w = *list[i];
        const GeoWindow& g = L.windows[i];
        w.zone = o.zones[i];
        if (g.fixedReason == L"limit") over++;
        if (g.fixedReason == L"maximized") guarded++;
        if (!g.subject) { w.hidden = false; continue; }
        guarded++;
        w.hidden = !o.zones[i];
        w.hiddenReason = o.reasons[i];
        if (w.hidden) hidden++;
    }
    hiddenCount = hidden;
    guardedCount = guarded;
    overLimit = over;
    tray->Update();
}

// ================================================================== drag and drop

static int HitAt(HWND h, POINT p) {
    POINT pt = p;
    PhysicalToLogicalPointForPerMonitorDPI(h, &pt);
    DWORD_PTR res = 0;
    LPARAM lp = MAKELPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y);
    if (!SendMessageTimeoutW(h, WM_NCHITTEST, 0, lp, SMTO_ABORTIFHUNG, 50, &res)) return HTCAPTION;
    int ht = (int)(LRESULT)res;
    return ht >= HTLEFT && ht <= HTBOTTOMRIGHT ? ht : HTCAPTION;
}

static int GuessEdge(const Rc& a, const Rc& b) {
    bool l = a.L != b.L, r = a.R != b.R, t = a.T != b.T, bo = a.B != b.B;
    if (t && l) return HTTOPLEFT;
    if (t && r) return HTTOPRIGHT;
    if (bo && l) return HTBOTTOMLEFT;
    if (bo && r) return HTBOTTOMRIGHT;
    if (l) return HTLEFT;
    if (r) return HTRIGHT;
    if (t) return HTTOP;
    if (bo) return HTBOTTOM;
    return HTCAPTION;
}

void GlimpseApp::BeginDrag(HWND h, bool byButton) {
    CancelDrag();
    WinPtr known = reg.Get(h);
    Rc settled = known ? known->settledFrame : Rc();
    RefreshRegistry();
    WinPtr w = reg.Get(h);
    if (!w || !w->counted || w->maximized) return;
    POINT p = Native::CursorPos();
    Rc start = byButton && !settled.IsEmpty() ? settled : w->frame;
    drag.reset(new DragState());
    drag->handle = h;
    drag->w = w;
    drag->startFrame = start;
    drag->startCursor = p;
    drag->fullDrag = byButton || Native::DragFullWindows();
    drag->hit = byButton ? GuessEdge(start, w->frame) : HitAt(h, p);
    drag->current = w->frame;
    drag->byButton = byButton;
    lastSceneThreatened = false;
    Log::Debug(L"drag start " + w->process + L" " + w->frame.ToString() + (drag->fullDrag ? L" (full window)" : L" (outline)") + L" hit " + Util::Inv(drag->hit));
    if (!Paused() && sessionActive) dragTimer.Start();
}

void GlimpseApp::CancelDrag() {
    dragTimer.Stop();
    if (drag) overlays->HideDrag();
    drag.reset();
}

void GlimpseApp::OnDragTick() {
    DragState* d = drag.get();
    if (!d) { dragTimer.Stop(); return; }
    if (!IsWindow(d->handle)) { CancelDrag(); return; }
    if (d->byButton && !Native::KeyDown(VK_LBUTTON)) { EndDrag(d->handle); return; }
    POINT p = Native::CursorPos();
    Rc cur;
    if (!(d->fullDrag && Native::TryFrame(d->handle, cur))) cur = d->Predict(p);
    if (d->fullDrag && d->hit == HTCAPTION && (cur.W() != d->startFrame.W() || cur.H() != d->startFrame.H())) d->hit = GuessEdge(d->startFrame, cur);
    bool shift = Native::KeyDown(VK_SHIFT);
    if (d->computed && cur.Same(d->current) && shift == d->lastShift) return;
    d->computed = true;
    d->current = cur;
    d->lastShift = shift;
    DragScene scene = BuildScene(*d, cur, shift, p);
    lastSceneThreatened = scene.threatened;
    d->guides = scene.guides;
    if (scene.threatened || !scene.minOutline.IsEmpty()) overlays->ShowDrag(scene);
    else overlays->HideDrag();
}

DragScene GlimpseApp::BuildScene(const DragState& d, const Rc& cur, bool shift, POINT cursor) {
    std::vector<WinPtr> list;
    Layout L = BuildLayout(list, d.w, &cur);
    auto it = std::find(list.begin(), list.end(), d.w);
    if (it == list.end()) {
        DragScene s;
        s.dragged = cur;
        s.draggedWindow = d.w;
        s.cursor = cursor;
        return s;
    }
    int ai = (int)(it - list.begin());
    DragScene scene = DragScene::Compute(L, list, ai, d, cur, shift, AutoFixAllowed(), S(), Paused() || Suspended());
    scene.cursor = cursor;
    return scene;
}

void GlimpseApp::EndDrag(HWND h) {
    if (!drag || drag->handle != h) return;
    dragTimer.Stop();
    overlays->HideDrag();
    drag->shiftAtEnd = Native::KeyDown(VK_SHIFT);
    dropping = std::move(drag);
    dropTimer.Start();
}

void GlimpseApp::OnDropSettled() {
    dropTimer.Stop();
    std::unique_ptr<DragState> d = std::move(dropping);
    if (!d) return;
    try { Drop(*d); }
    catch (const std::exception& ex) { Log::Error(L"drop: " + Util::Widen(ex.what())); }
    ProcessPending();
}

void GlimpseApp::Drop(DragState& d) {
    RefreshRegistry();
    WinPtr w = reg.Get(d.handle);
    if (!w || !w->counted) return;
    Rc end = w->frame;
    w->settledFrame = end;
    Log::Debug(L"drop " + w->process + L" " + d.startFrame.ToString() + L" -> " + end.ToString() + (d.shiftAtEnd ? L" (shift)" : L""));
    if (end.Same(d.startFrame)) { UpdateStatus(); return; }
    bool resized = end.W() != d.startFrame.W() || end.H() != d.startFrame.H();
    wstr edges = resized ? DragState::ChangedEdges(d.startFrame, end) : wstr();
    w->userPlacedAt = Clock::Ms();
    if (resized && S().rememberSizes && !w->maximized) { sizes.Remember(*w, end); sizes.SaveIfDirty(); }
    if (w->maximized || d.shiftAtEnd || !AutoFixAllowed()) { UpdateStatus(); return; }

    UndoPtr g = std::make_shared<UndoGroup>();
    g->reason = L"drop";
    // a drop within 12 px of a snap guide lands on it
    if (!resized && lastSceneThreatened) {
        int snap = Util::Scale(12, w->dpi);
        for (const Guide& gd : d.guides) {
            if (std::abs(gd.snapped.L - end.L) + std::abs(gd.snapped.T - end.T) > snap) continue;
            bool denied;
            if (Actuator::MoveFrame(*w, gd.snapped, g.get(), denied)) { end = gd.snapped; w->settledFrame = end; }
            break;
        }
    }
    RunAndApply(Cause::Drop, w, edges, d.startFrame, &end, g);
    undo.Push(g);
    int others = 0;
    wstr titles;
    for (const UndoItem& it : g->items)
        if (it.handle != w->handle) { others++; if (!titles.empty()) titles += L", "; titles += it.title; }
    if (others > 0) Toast::Show(this, end, Texts::P(L"toast.moved", others), titles, Texts::T(L"toast.undo"), [this, g]() { UndoGroupNow(g); });
    UpdateStatus();
}

// ================================================================== display changes

void GlimpseApp::DisplaySignal(const wstr& why) {
    if (disposed || !sessionActive) return;
    Log::Debug(L"display signal: " + why);
    settle.Start();
}

// 1.5 s after the last display event: rescue, rescale, minimum sizes, resolver, one toast
void GlimpseApp::OnDisplaySettled() {
    settle.Stop();
    if (disposed || !sessionActive) return;
    std::vector<MonitorInfo> next = Monitors::Read();
    wstr sig = Monitors::Signature(next);
    if (sig == layoutSig) return;
    Log::Info(L"display changed: " + Util::Inv((long long)next.size()) + L" monitor(s) " + sig);
    for (auto& kv : reg.all) {
        TrackedWindow& w = *kv.second;
        const MonitorInfo* m = Monitors::At(mons, w.frame);
        w.prevWork = m ? m->work : Rc();
        w.topMapWidth = -1;   // probe again at the new scaling
    }
    layouts.Save(layoutSig, reg.all);
    mons = next;
    layoutSig = sig;
    overlays->OnMonitorsChanged();
    xray->Close();
    RefreshRegistry();
    UndoPtr g = std::make_shared<UndoGroup>();
    g->reason = L"display";
    RunAndApply(Cause::Display, WinPtr(), L"", Rc(), nullptr, g);
    undo.Push(g);
    for (auto& kv : reg.all) kv.second->prevWork = Rc();
    UpdateStatus();
    const std::map<HWND, Rc>* saved = layouts.Get(sig);
    if (saved && !saved->empty()) {
        std::map<HWND, Rc> copy = *saved;
        Toast::Show(this, Rc(), Texts::T(L"toast.layout"), Texts::T(L"toast.layoutHint"), Texts::T(L"toast.restore"), [this, copy]() { RestoreLayout(copy); });
    } else if (!g->items.empty()) {
        Toast::Show(this, Rc(), Texts::P(L"toast.display", (long long)g->items.size()), L"", Texts::T(L"toast.undo"), [this, g]() { UndoGroupNow(g); });
    }
}

void GlimpseApp::RestoreLayout(std::map<HWND, Rc> saved) {
    RefreshRegistry();
    UndoPtr g = std::make_shared<UndoGroup>();
    g->reason = L"restore layout";
    for (const auto& kv : saved) {
        WinPtr w = reg.Get(kv.first);
        if (!w || !w->counted || w->maximized || w->elevated) continue;
        if (w->windowRect.Same(kv.second)) continue;
        bool denied;
        Rc before = w->windowRect;
        if (Actuator::SetRect(w.get(), w->handle, before, kv.second, denied)) {
            UndoItem it;
            it.handle = w->handle;
            it.before = before;
            it.after = kv.second;
            it.title = w->ShortTitle();
            g->items.push_back(it);
        }
    }
    undo.Push(g);
    Log::Info(L"restored " + Util::Inv((long long)g->items.size()) + L" window position(s) for this display setup");
    UpdateStatus();
}

void GlimpseApp::OnSafetyTick() {
    if (disposed) return;
    store.Poll();
    if (!Paused() && pausedUntil != 0) { pausedUntil = 0; tray->Update(); }
    if (!sessionActive) return;
    wstr sig = Monitors::Signature(Monitors::Read());
    if (sig != layoutSig) DisplaySignal(L"periodic check");
    if (!drag && !dropping && !settle.Enabled()) {
        RefreshRegistry();
        UpdateStatus();
        layouts.Save(layoutSig, reg.all);
    }
    sizes.SaveIfDirty();
}

// ================================================================== commands

void GlimpseApp::ShowXRay(bool fromKeyboard) {
    if (!sessionActive || disposed) return;
    if (xray->IsOpen()) return;
    CancelDrag();
    RefreshRegistry();
    xray->Show(fromKeyboard);
}

// brings a window forward (restoring it if minimized); with giveGlimpse it also moves so it has one
void GlimpseApp::BringForward(const WinPtr& w, bool giveGlimpse) {
    if (!w || !IsWindow(w->handle)) return;
    if (IsIconic(w->handle)) ShowWindowAsync(w->handle, SW_RESTORE);
    Native::ForceForeground(w->handle);
    if (!giveGlimpse) return;
    HWND h = w->handle;
    Ui::Later(350, [this, h]() {
        if (disposed) return;
        RefreshRegistry();
        WinPtr cur = reg.Get(h);
        if (!cur || !cur->counted) return;
        UndoPtr g = std::make_shared<UndoGroup>();
        g->reason = L"x-ray";
        RunAndApply(Cause::GiveGlimpse, cur, L"", Rc(), nullptr, g);
        undo.Push(g);
        UpdateStatus();
    });
}

void GlimpseApp::Arrange() {
    RefreshRegistry();
    UndoPtr g = std::make_shared<UndoGroup>();
    g->reason = L"arrange";
    RunAndApply(Cause::Arrange, WinPtr(), L"", Rc(), nullptr, g);
    undo.Push(g);
    UpdateStatus();
    if (!g->items.empty()) Toast::Show(this, Rc(), Texts::P(L"toast.arranged", (long long)g->items.size()), L"", Texts::T(L"toast.undo"), [this, g]() { UndoGroupNow(g); });
    else Toast::Show(this, Rc(), Texts::T(L"toast.allInReach"), L"", L"", nullptr);
}

void GlimpseApp::UndoLast() {
    UndoPtr g = undo.Last();
    if (g) UndoGroupNow(g);
}

void GlimpseApp::UndoGroupNow(const UndoPtr& g) {
    if (disposed || !undo.Contains(g)) return;
    int n = Actuator::UndoGroupNow(*g, reg);
    undo.Remove(g);
    Log::Info(L"undo: " + Util::Inv(n) + L" window(s) restored");
    for (const UndoItem& it : g->items) {
        WinPtr w = reg.Get(it.handle);
        if (w) { w->fixedAt = 0; w->userPlacedAt = Clock::Ms(); }
    }
    Ui::Later(300, [this]() {
        if (disposed) return;
        RefreshRegistry();
        for (auto& kv : reg.all) kv.second->settledFrame = kv.second->frame;
        UpdateStatus();
    });
}

void GlimpseApp::ShowSettings() {
    if (disposed) return;
    if (!settingsWindow) settingsWindow = new SettingsWindow(this, S().Clone(), 0);
    settingsWindow->Show();
    if (IsIconic(settingsWindow->hwnd)) ShowWindow(settingsWindow->hwnd, SW_RESTORE);
    Native::ForceForeground(settingsWindow->hwnd);
}

void GlimpseApp::SettingsClosed() {
    SettingsWindow* w = settingsWindow;
    settingsWindow = nullptr;
    if (w) Ui::Post([w]() { delete w; });
}

// saves settings from the settings window; false when Glimpse.ini is read-only
bool GlimpseApp::SaveSettings(const Settings& s) {
    if (!store.Save(s)) { ApplySettings(false); return false; }
    ApplySettings(false);
    return true;
}

void GlimpseApp::ApplySettings(bool fromFile) {
    if (disposed) return;
    Log::SetDebug(S().debugLog);
    Texts::Load(S().language);
    hotkeys->Apply(S().hotkey);
    corner->Set(S().hotCorner);
    StartWithWindows::Sync(S().startWithWindows);
    ApplyFirstClick();
    for (auto& kv : reg.all) kv.second->excluded = S().IsExcluded(kv.second->process, kv.second->title);
    tray->Rebuild();
    if (fromFile && settingsWindow) settingsWindow->ReloadFromApp();
    UpdateStatus();
}

void GlimpseApp::ApplyFirstClick() {
    if (S().firstClickActivates && !firstClick) {
        firstClick.reset(new FirstClickHook([this](int x, int y) { return GlimpseOwnerAt(x, y); }, [](HWND h) { Native::ForceForeground(h); }));
        Log::Info(L"first-click mode on");
    } else if (!S().firstClickActivates && firstClick) firstClick.reset();
}

// the background window whose free glimpse is at x, y (content part only), or none
HWND GlimpseApp::GlimpseOwnerAt(int x, int y) {
    if (!sessionActive || Paused()) return nullptr;
    POINT p = { x, y };
    HWND hit = GetAncestor(WindowFromPoint(p), GA_ROOT);
    WinPtr w = reg.Get(hit);
    if (!w || !w->zone || !w->zone->rect.Contains(x, y)) return nullptr;
    // a click on the title bar only activates anyway, and must stay free to drag the window
    if (y < w->frame.T + Util::Scale(32, w->dpi)) return nullptr;
    return hit;
}

void GlimpseApp::ExitApp() {
    if (disposed) return;
    Log::Info(L"exit");
    // the exit command arrives inside the message window, which Shutdown destroys: shut down a moment
    // later, still inside the message loop (destroying windows after it ended is slow on some systems)
    Ui::Post([this]() {
        Shutdown();
        Ui::Quit(0);
    });
}

void GlimpseApp::Shutdown() {
    if (disposed) return;
    disposed = true;
    Timer* timers[] = { &debounce, &settle, &safety, &readback, &dragTimer, &dropTimer, &iniPoll };
    for (Timer* t : timers) t->Stop();
    intake.reset();
    corner.reset();
    firstClick.reset();
    if (hotkeys) hotkeys->Unregister();
    if (settingsWindow) { delete settingsWindow; settingsWindow = nullptr; }
    Toast::CloseAll();
    if (xray) xray->Close();
    overlays.reset();
    tray.reset();
    sizes.SaveIfDirty();
    if (msg) WTSUnRegisterSessionNotification(msg->handle);
    xray.reset();
    msg.reset();
}

// ================================================================== drag scene

// simulates the drop at cur and sorts the other windows into keeps a glimpse, moves on drop and would be hidden
DragScene DragScene::Compute(Layout& L, const std::vector<WinPtr>& list, int ai, const DragState& d, const Rc& cur, bool shift, bool fix, const Settings& S, bool paused) {
    DragScene scene;
    scene.dragged = cur;
    scene.draggedWindow = list[ai];
    scene.shift = shift;
    scene.mode = S.mode;
    scene.paused = paused;
    std::vector<Rc> before = L.frames;
    if (!d.Resizing()) scene.guides = Resolver::Guides(L, ai, Util::Scale(60, list[ai]->dpi));
    Request q = Request::For(Cause::Drop, S.mode, ai, S.placeNewWindows);
    q.anchorStart = d.startFrame;
    q.draggedEdges = d.Resizing() ? d.Edges() : wstr();
    q.fix = !shift && fix;
    Outcome o = Resolver::Resolve(L, q);
    const GeoWindow& g = L.windows[ai];
    if (d.Resizing() && g.resizable && (cur.W() < g.minW || cur.H() < g.minH)) {
        scene.minOutline = Geometry::GrowToMin(cur, g.minW, g.minH, d.Edges(), L.WorkOf(cur));
        const MinSizeRule* rule = S.RuleFor(list[ai]->title);
        if (rule) scene.minText = Texts::F(L"drag.minSize", { { L"width", Util::Inv(rule->minWidth) }, { L"height", Util::Inv(rule->minHeight) } });
    }
    scene.draggedZone = o.zones[ai];
    for (int i = 0; i < (int)list.size(); i++) {
        if (i == ai || !L.windows[i].subject) continue;
        const WinPtr& w = list[i];
        SceneWindow sw;
        sw.w = w;
        sw.frame = before[i];
        sw.title = w->ShortTitle();
        sw.icon = w->icon;
        const Move* mv = o.MoveOf(i);
        if (mv && mv->why != L"minsize") {
            sw.state = 1;
            sw.newFrame = mv->to;
            sw.newZone = o.zones[i];
            sw.zone = Geometry::BestZone(&L.windows[i], mv->from, L.P(i), std::vector<Rc>(), L.monitors);
            scene.moves++;
        } else if (o.zones[i]) { sw.state = 0; sw.zone = o.zones[i]; scene.stay++; }
        else {
            sw.state = 2;
            sw.reason = shift ? L"shift" : (paused ? L"paused" : o.reasons[i]);
            scene.hidden++;
        }
        scene.others.push_back(sw);
    }
    scene.threatened = scene.moves + scene.hidden > 0 || !scene.draggedZone;
    // windows that were already hidden before the drag are not news
    if (scene.moves == 0 && scene.draggedZone) {
        bool news = false;
        for (const SceneWindow& sw : scene.others) if (sw.state == 2 && sw.w && !sw.w->hidden) news = true;
        scene.threatened = news;
    }
    return scene;
}

// ================================================================== run

namespace App {
int Run() {
    Ui::Init();
    Ui::StartGdiplus();
    int code = 0;
    try {
        GlimpseApp app;
        code = Ui::RunLoop();
    } catch (const std::exception& ex) {
        Log::Error(L"fatal: " + Util::Widen(ex.what()));
        code = 1;
    }
    Ui::StopGdiplus();
    return code;
}
}

} // namespace glimpse
