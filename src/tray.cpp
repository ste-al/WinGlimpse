// Glimpse native build: tray icon and menu, toasts.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"

namespace glimpse {

using namespace Gdiplus;

// ================================================================== tray

static const wchar_t* kTrayClass = L"GlimpseTray";
static const UINT kTrayMsg = WM_APP + 1;
enum { CmdXRay = 1, CmdArrange, CmdUndo, CmdPause15, CmdPause60, CmdPauseTomorrow, CmdResume, CmdSettings, CmdExit };

TrayUi::TrayUi(GlimpseApp* a) : app(a) {
    static bool registered = false;
    if (!registered) { Ui::RegisterClass(kTrayClass, Proc); registered = true; }
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, L"Glimpse tray", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, Ui::Instance(), this);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    Update();
    Add();
}

TrayUi::~TrayUi() {
    if (added) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof nid;
        nid.hWnd = hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    if (hwnd) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); DestroyWindow(hwnd); }
    if (hIcon) DestroyIcon(hIcon);
}

void TrayUi::Add() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = hIcon;
    wcsncpy(nid.szTip, tip.c_str(), 127);
    added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (added) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        if (addFailed) Log::Info(L"tray icon added");
        addFailed = false;
    } else if (!addFailed) {
        // started before the taskbar (a logon script): TaskbarCreated or the next status update retries
        Log::Warn(L"tray icon could not be added (no notification area yet), will retry");
        addFailed = true;
    }
}

void TrayUi::Rebuild() { Update(); }

wstr TrayUi::StatusText(Color& dot) const {
    dot = Theme::Green();
    if (!app->sessionActive) { dot = Color(128, 128, 128); return Texts::T(L"status.idle"); }
    if (app->Paused()) {
        dot = Color(128, 128, 128);
        if (app->PausedUntilTomorrow()) return Texts::T(L"status.pausedTomorrow");
        return Texts::F(L"status.paused", { { L"time", app->PausedUntilText() } });
    }
    if (app->Suspended()) { dot = Theme::Red(); return Texts::T(L"status.suspended"); }
    wstr s = Texts::P(L"status.guarding", app->guardedCount);
    if (app->hiddenCount == 0) s += L" \u00b7 " + Texts::T(L"status.allInReach");
    else { s += L" \u00b7 " + Texts::P(L"status.hidden", app->hiddenCount); dot = Theme::Amber(); }
    return s;
}

std::vector<wstr> TrayUi::Notes() const {
    std::vector<wstr> n;
    if (app->S().mode == FixMode::Guide) n.push_back(Texts::T(L"status.guide"));
    if (app->overLimit > 0) n.push_back(Texts::F(L"status.overLimit", { { L"count", Util::Inv(app->S().maxWindows) } }));
    if (app->store.usingDefaults) n.push_back(Texts::T(L"status.iniDefaults"));
    else if (!app->S().problems.empty()) n.push_back(Texts::T(L"status.invalidPattern"));
    if (app->hotkeys && !app->hotkeys->problem.empty()) n.push_back(Texts::F(L"status.hotkeyTaken", { { L"hotkey", app->hotkeys->problem } }));
    return n;
}

void TrayUi::Update() {
    if (!added && addFailed) Add();
    Color dot;
    wstr status = StatusText(dot);
    bool warn = app->Suspended() || app->store.usingDefaults || !app->S().problems.empty() || (app->hotkeys && !app->hotkeys->problem.empty());
    wstr t = L"Glimpse \u00b7 " + status;
    if (t.size() > 127) t = t.substr(0, 126) + L"\u2026";
    bool tipChanged = t != tip;
    tip = t;
    int state = !app->sessionActive || app->Paused() ? 1 : (warn ? 3 : (app->hiddenCount > 0 ? 2 : 0));
    wstr before = iconKey;
    SetIcon(state, app->hiddenCount);
    if (added && (tipChanged || before != iconKey)) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof nid;
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.hIcon = hIcon;
        wcsncpy(nid.szTip, tip.c_str(), 127);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void TrayUi::SetIcon(int state, int hidden) {
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size <= 0) size = 16;
    wstr key = Util::Inv(state) + L"|" + Util::Inv(hidden) + L"|" + Util::Inv(size);
    if (key == iconKey) return;
    iconKey = key;
    std::unique_ptr<Gdiplus::Bitmap> bmp(RenderIcon(size, state, hidden));
    HICON h = Draw::MakeIcon(*bmp);
    if (hIcon) DestroyIcon(hIcon);
    hIcon = h;
}

Gdiplus::Bitmap* TrayUi::RenderIcon(int size, int state, int hidden) {
    auto* bmp = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
    Graphics g(bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    Color front = state == 1 ? Color(140, 140, 150) : Theme::Accent();
    Draw::Logo(g, RectF(0, 0, (REAL)size, (REAL)size), front, front, Color(255, 255, 255));
    float d = size * 0.5f;
    RectF badge(size - d, size - d, d, d);
    switch (state) {
        case 0: {
            float s = size * 0.36f;
            Draw::Fill(g, RectF(size - s - 0.5f, size - s - 0.5f, s, s), s / 2, Color(255, 255, 255));
            Draw::Fill(g, RectF(size - s + size * 0.06f - 0.5f, size - s + size * 0.06f - 0.5f, s - size * 0.12f, s - size * 0.12f), s / 2, Theme::GreenLine());
            break;
        }
        case 1: {
            Draw::Fill(g, badge, d / 2, Color(90, 90, 100));
            SolidBrush b(Color(255, 255, 255));
            g.FillRectangle(&b, badge.X + d * 0.3f, badge.Y + d * 0.25f, d * 0.14f, d * 0.5f);
            g.FillRectangle(&b, badge.X + d * 0.56f, badge.Y + d * 0.25f, d * 0.14f, d * 0.5f);
            break;
        }
        case 2:
        case 3: {
            Draw::Fill(g, badge, d / 2, state == 2 ? Theme::Amber() : Theme::Red());
            wstr t = state == 3 ? L"!" : (hidden > 9 ? L"9+" : Util::Inv(hidden));
            Gdiplus::Font* f = Draw::Font(d * (t.size() > 1 ? 0.62f : 0.82f), true);
            SizeF sz = Draw::Measure(t, f);
            Draw::Text(g, t, f, state == 2 ? Theme::Ink() : Color(255, 255, 255), badge.X + (d - sz.Width) / 2, badge.Y + (d - sz.Height) / 2);
            break;
        }
    }
    return bmp;
}

SIZE TrayUi::HeaderSize(float k, const wstr& status, const std::vector<wstr>& notes) {
    Gdiplus::Font* fs = Draw::Font(14.5f * k, false);
    float w = Draw::Measure(status, fs).Width;
    Gdiplus::Font* fn = Draw::Font(13 * k, false);
    for (const wstr& n : notes) w = std::max(w, Draw::Measure(n, fn).Width);
    SIZE s = { (LONG)(w + 90 * k), (LONG)((62 + notes.size() * 20) * k) };
    return s;
}

void TrayUi::PaintHeader(Graphics& g, const RectF& r, float k, const wstr& status, Color dot, const std::vector<wstr>& notes) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    {
        SolidBrush b(Theme::Card());
        g.FillRectangle(&b, r);
    }
    float x = r.X, y0 = r.Y;
    Draw::Logo(g, RectF(x + 14 * k, y0 + 12 * k, 36 * k, 36 * k), Theme::Accent(), Theme::Accent(), Theme::Card());
    Draw::Text(g, L"Glimpse", Draw::Font(17 * k, true), Theme::Text(), x + 64 * k, y0 + 10 * k);
    {
        SolidBrush b(dot);
        g.FillEllipse(&b, x + 65 * k, y0 + 41 * k, 9 * k, 9 * k);
    }
    Draw::Text(g, status, Draw::Font(14.5f * k, false), Theme::Text(), x + 80 * k, y0 + 35 * k);
    float y = y0 + 58 * k;
    for (const wstr& n : notes) {
        Draw::Text(g, n, Draw::Font(13 * k, false), Theme::Sub(), x + 64 * k, y);
        y += 20 * k;
    }
}

std::vector<MenuItem> TrayUi::MenuItems(const wstr& xrayHint, const wstr& undoHint, bool undoEnabled, bool paused, bool suspendedOnly) {
    auto item = [](int id, const wstr& text, const wstr& glyph, const wstr& hint) {
        MenuItem m;
        m.id = id;
        m.text = text;
        m.glyph = glyph;
        m.hint = hint;
        return m;
    };
    MenuItem sep;
    sep.separator = true;
    MenuItem header;
    header.header = true;
    std::vector<MenuItem> items;
    items.push_back(header);
    items.push_back(sep);
    items.push_back(item(CmdXRay, Texts::T(L"menu.xray"), L"xray", xrayHint));
    items.push_back(item(CmdArrange, Texts::T(L"menu.arrange"), L"arrange", L""));
    MenuItem undo = item(CmdUndo, Texts::T(L"menu.undo"), L"undo", undoHint);
    undo.enabled = undoEnabled;
    items.push_back(undo);
    if (!paused) {
        MenuItem pause = item(0, Texts::T(L"menu.pause"), L"pause", Texts::T(L"menu.pauseHint"));
        pause.sub.push_back(item(CmdPause15, Texts::T(L"menu.pause15"), L"", L""));
        pause.sub.push_back(item(CmdPause60, Texts::T(L"menu.pause60"), L"", L""));
        pause.sub.push_back(item(CmdPauseTomorrow, Texts::T(L"menu.pauseTomorrow"), L"", L""));
        items.push_back(pause);
    } else items.push_back(item(CmdResume, suspendedOnly ? Texts::T(L"menu.resumeFixes") : Texts::T(L"menu.resume"), L"play", L""));
    items.push_back(sep);
    items.push_back(item(CmdSettings, Texts::T(L"menu.settings"), L"settings", L""));
    items.push_back(item(CmdExit, Texts::T(L"menu.exit"), L"exit", L""));
    return items;
}

void TrayUi::ShowMenu() {
    POINT p = Native::CursorPos();
    const MonitorInfo* m = Monitors::AtPoint(app->mons, p.x, p.y);
    float k = (m ? m->dpi : 96) / 96.0f;
    Color dot;
    wstr status = StatusText(dot);
    std::vector<wstr> notes = Notes();
    UndoPtr last = app->undo.Last();
    wstr xrayHint = app->hotkeys && app->hotkeys->registered ? app->hotkeys->current.DisplayText() : wstr();
    bool paused = app->Paused() || app->Suspended();
    std::vector<MenuItem> items = MenuItems(xrayHint, last ? Texts::P(L"windows", (long long)last->items.size()) : wstr(), (bool)last, paused,
                                            app->Suspended() && !app->Paused());
    SetForegroundWindow(hwnd);
    int id = PopupMenu::Show(hwnd, items, p.x, p.y, k, TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                             [&](float kk) { return HeaderSize(kk, status, notes); },
                             [&](Graphics& g, const RectF& r, float kk) { PaintHeader(g, r, kk, status, dot, notes); });
    PostMessageW(hwnd, WM_NULL, 0, 0);
    GlimpseApp* a = app;
    switch (id) {
        case CmdXRay: a->ShowXRay(false); break;
        case CmdArrange: a->Arrange(); break;
        case CmdUndo: a->UndoLast(); break;
        case CmdPause15: a->Pause(15, false); break;
        case CmdPause60: a->Pause(60, false); break;
        case CmdPauseTomorrow: a->Pause(0, true); break;
        case CmdResume: a->Resume(); break;
        case CmdSettings: a->ShowSettings(); break;
        case CmdExit: a->ExitApp(); break;
    }
}

LRESULT CALLBACK TrayUi::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    LRESULT r = 0;
    if (PopupMenu::OwnerDraw(m, wp, lp, r)) return r;
    auto* self = (TrayUi*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self) {
        if (m == kTrayMsg) {
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP: self->app->ShowXRay(false); break;
                case NIN_KEYSELECT: self->app->ShowXRay(true); break;
                case WM_CONTEXTMENU: self->ShowMenu(); break;
            }
            return 0;
        }
        if (self->taskbarCreated && m == self->taskbarCreated) {
            // Explorer restarted: the icon must be added again
            self->added = false;
            self->Add();
            return 0;
        }
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ================================================================== toast

namespace {

struct ToastWindow {
    HWND hwnd = nullptr;
    wstr title, subtitle, actionText;
    std::function<void()> action;
    float k = 1;
    RectF actionRect;
    bool hoverAction = false, tracking = false;
    Timer life;
    ToastWindow() : life(6000) {}
    void Close() { life.Stop(); if (hwnd) DestroyWindow(hwnd); }
};

ToastWindow* current = nullptr;
const wchar_t* kToastClass = L"GlimpseToast";

LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* t = (ToastWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!t) return DefWindowProcW(h, m, wp, lp);
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            SIZE size = { rc.right, rc.bottom };
            if (size.cx > 0 && size.cy > 0) {
                Gdiplus::Bitmap buf(size.cx, size.cy, PixelFormat32bppPARGB);
                {
                    Graphics g(&buf);
                    t->actionRect = Toast::Paint(g, size, t->title, t->subtitle, t->actionText, t->hoverAction, t->k);
                }
                Graphics out(dc);
                out.DrawImage(&buf, 0, 0, size.cx, size.cy);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!t->tracking) {
                TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                t->tracking = true;
                t->life.Stop();   // stays while the mouse is on it
            }
            bool hv = t->actionRect.Contains((REAL)GET_X_LPARAM(lp), (REAL)GET_Y_LPARAM(lp));
            if (hv != t->hoverAction) { t->hoverAction = hv; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_MOUSELEAVE:
            t->tracking = false;
            if (t->hoverAction) { t->hoverAction = false; InvalidateRect(h, nullptr, FALSE); }
            t->life.Start();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, t->hoverAction ? IDC_HAND : IDC_ARROW)); return TRUE; }
            break;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            bool act = t->actionRect.Contains((REAL)GET_X_LPARAM(lp), (REAL)GET_Y_LPARAM(lp)) && t->action;
            std::function<void()> action = t->action;
            t->Close();
            if (act) {
                try { action(); } catch (const std::exception& ex) { Log::Error(L"toast action: " + Util::Widen(ex.what())); }
            }
            return 0;
        }
        case WM_NCDESTROY:
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            t->hwnd = nullptr;
            if (current == t) current = nullptr;
            Ui::Post([t]() { delete t; });
            break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

} // namespace

namespace Toast {

SIZE Measure(const wstr& title, const wstr& subtitle, const wstr& actionText, float k) {
    float w = std::max(Draw::Measure(title, Draw::Font(16 * k, true)).Width, std::min(Draw::Measure(subtitle, Draw::Font(14 * k, false)).Width, 360 * k));
    float bw = actionText.empty() ? 0 : Draw::Measure(actionText, Draw::Font(15 * k, true)).Width + 28 * k + 16 * k;
    int width = (int)std::max(300 * k, std::min(620 * k, 20 * k + 34 * k + 16 * k + w + 20 * k + bw));
    int height = (int)((subtitle.empty() ? 60 : 76) * k);
    SIZE s = { width, height };
    return s;
}

RectF Paint(Graphics& g, SIZE size, const wstr& title, const wstr& subtitle, const wstr& actionText, bool hover, float k) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    {
        SolidBrush b(Theme::Card());
        g.FillRectangle(&b, 0, 0, (INT)size.cx, (INT)size.cy);
        Pen pen(Theme::Border());
        g.DrawRectangle(&pen, 0, 0, (INT)size.cx - 1, (INT)size.cy - 1);
    }
    float x = 18 * k;
    Draw::Logo(g, RectF(x, (size.cy - 32 * k) / 2, 32 * k, 32 * k), Theme::Accent(), Theme::Accent(), Theme::Card());
    x += 32 * k + 16 * k;
    RectF ar;
    float right = size.cx - 16 * k;
    if (!actionText.empty()) {
        Gdiplus::Font* fa = Draw::Font(15 * k, true);
        float bw = Draw::Measure(actionText, fa).Width + 28 * k, bh = 36 * k;
        ar = RectF(right - bw, (size.cy - bh) / 2, bw, bh);
        Draw::Fill(g, ar, 7 * k, hover ? Theme::Accent() : Theme::AccentSoft());
        Draw::Text(g, actionText, fa, hover ? Color(255, 255, 255) : Theme::AccentText(), ar.X + 14 * k, ar.Y + (bh - Draw::Height(fa)) / 2);
        right = ar.X - 12 * k;
    }
    Gdiplus::Font* ft = Draw::Font(16 * k, true);
    float fth = Draw::Height(ft);
    if (subtitle.empty())
        Draw::TextIn(g, title, ft, Theme::Text(), RectF(x, (size.cy - fth) / 2, right - x, fth + 2));
    else {
        Draw::TextIn(g, title, ft, Theme::Text(), RectF(x, size.cy / 2.0f - fth - 1 * k, right - x, fth + 2));
        Gdiplus::Font* fs = Draw::Font(14 * k, false);
        Draw::TextIn(g, subtitle, fs, Theme::Sub(), RectF(x, size.cy / 2.0f + 3 * k, right - x, Draw::Height(fs) + 2));
    }
    return ar;
}

void Show(GlimpseApp* app, const Rc& anchor, const wstr& title, const wstr& subtitle, const wstr& actionText, std::function<void()> action) {
    try {
        if (current) current->Close();
        static bool registered = false;
        if (!registered) { Ui::RegisterClass(kToastClass, ToastProc, CS_DROPSHADOW); registered = true; }
        Theme::Refresh();
        const MonitorInfo* mon = anchor.IsEmpty() ? nullptr : Monitors::At(app->mons, anchor);
        if (!mon) for (const MonitorInfo& m : app->mons) if (m.primary) mon = &m;
        if (!mon && !app->mons.empty()) mon = &app->mons[0];
        if (!mon) return;
        auto* t = new ToastWindow();
        t->title = title;
        t->subtitle = subtitle;
        t->actionText = actionText;
        t->action = std::move(action);
        t->k = mon->dpi / 96.0f;
        float k = t->k;
        SIZE sz = Measure(title, subtitle, actionText, k);
        Rc work = mon->work;
        int x, y, gap = (int)(12 * k);
        if (anchor.IsEmpty()) { x = work.R - sz.cx - (int)(16 * k); y = work.B - sz.cy - (int)(16 * k); }
        else {
            x = anchor.R - sz.cx;
            y = anchor.B + gap;
            if (y + sz.cy > work.B) y = anchor.T - sz.cy - gap;
            if (y < work.T) y = anchor.B - sz.cy - gap;
        }
        x = Util::Clamp(x, work.L + gap, std::max(work.L + gap, work.R - (int)sz.cx - gap));
        y = Util::Clamp(y, work.T + gap, std::max(work.T + gap, work.B - (int)sz.cy - gap));
        t->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kToastClass, L"Glimpse toast", WS_POPUP,
                                  x, y, sz.cx, sz.cy, app->msg ? app->msg->handle : nullptr, nullptr, Ui::Instance(), t);
        if (!t->hwnd) { delete t; return; }
        Native::SetRoundCorners(t->hwnd, false);
        t->life.tick = [t]() { t->Close(); };
        t->life.Start();
        current = t;
        ShowWindow(t->hwnd, SW_SHOWNOACTIVATE);
    } catch (const std::exception& ex) { Log::Error(L"toast: " + Util::Widen(ex.what())); }
}

void CloseAll() {
    if (current) current->Close();
}

} // namespace Toast

} // namespace glimpse
