// Glimpse native build: X-ray, every window's outline and name, through whatever covers it.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include <climits>

namespace glimpse {

using namespace Gdiplus;

// ------------------------------------------------------------------ matching: ignores case and diacritics

static wstr Fold(const wstr& s) {
    if (s.empty()) return s;
    wstr d;
    int n = NormalizeString(NormalizationKD, s.c_str(), (int)s.size(), nullptr, 0);
    if (n > 0) {
        d.resize(n);
        n = NormalizeString(NormalizationKD, s.c_str(), (int)s.size(), &d[0], n);
        if (n > 0) d.resize(n); else d = s;
    } else d = s;
    wstr out;
    out.reserve(d.size());
    for (wchar_t c : d) {
        if (c >= 0x0300 && c <= 0x036F) continue;   // combining diacritical marks
        WORD t = 0;
        if (c > 0x7F && GetStringTypeW(CT_CTYPE3, &c, 1, &t) && (t & C3_NONSPACING) && !(t & C3_ALPHA)) continue;
        out += c;
    }
    return Util::Lower(out);
}

bool XRayView::Matches(const wstr& title, const wstr& q) {
    if (q.empty()) return true;
    return Fold(title).find(Fold(q)) != wstr::npos;
}

// ------------------------------------------------------------------ view

XRayView::XRayView(GlimpseApp* a) : app(a), digitTimer(700) {
    digitTimer.tick = [this]() { digitTimer.Stop(); PickNumber(true); };
}

XRayView::~XRayView() {
    digitTimer.Stop();
    // detach first, as in Close: a surface that is destroyed may send WM_ACTIVATE back to the view
    std::vector<std::unique_ptr<XRaySurface>> closing;
    closing.swap(surfaces);
    main = nullptr;
    closing.clear();
}

std::vector<XRayEntry> XRayView::Collect(const WindowRegistry& reg, bool showMinimized) {
    std::vector<XRayEntry> list;
    HWND fg = GetForegroundWindow();
    int n = 0;
    std::vector<WinPtr> all = reg.AllByMru();
    for (const WinPtr& w : all) {
        if (w->minimized) continue;
        if (w->handle == fg && (w->maximized || w->fullScreen)) continue;   // it is the backdrop
        XRayEntry e;
        e.w = w;
        e.frame = w->frame;
        e.title = w->ShortTitle();
        e.number = ++n;
        list.push_back(e);
    }
    if (showMinimized)
        for (const WinPtr& w : all) {
            if (!w->minimized) continue;
            XRayEntry e;
            e.w = w;
            e.title = w->ShortTitle();
            e.number = ++n;
            e.minimized = true;
            list.push_back(e);
        }
    return list;
}

void XRayView::Show(bool fromKeyboard) {
    Close();
    entries = Collect(app->reg, app->S().showMinimized);
    query.clear();
    digits.clear();
    selected = entries.empty() ? -1 : 0;
    hover = -1;
    POINT p = Native::CursorPos();
    const MonitorInfo* mainMon = Monitors::AtPoint(app->mons, p.x, p.y);
    if (!mainMon && !app->mons.empty()) mainMon = &app->mons[0];
    for (const MonitorInfo& m : app->mons) {
        try {
            auto s = std::unique_ptr<XRaySurface>(new XRaySurface(this, m, app->msg ? app->msg->handle : nullptr));
            if (&m == mainMon) main = s.get();
            surfaces.push_back(std::move(s));
        } catch (const std::exception& ex) { Log::Error(L"x-ray: " + Util::Widen(ex.what())); }
    }
    RenderAll();
    for (auto& s : surfaces) s->ShowNow();
    const wchar_t* fg = main ? Native::ForceForeground(main->hwnd) : L"no";
    Log::Debug(L"x-ray opened with " + Util::Inv((long long)entries.size()) + L" windows" + (fromKeyboard ? L" (hotkey)" : L"") + L", foreground: " + fg);
}

void XRayView::Close() {
    digitTimer.Stop();
    // detach the surfaces first: hiding the active one sends WM_ACTIVATE (and more) into the view right away,
    // and the view must already be closed then
    std::vector<std::unique_ptr<XRaySurface>> closing;
    closing.swap(surfaces);
    main = nullptr;
    // the windows may be closing from inside their own message: hide now, destroy a moment later
    for (auto& s : closing) {
        XRaySurface* raw = s.release();
        ShowWindow(raw->hwnd, SW_HIDE);
        Ui::Post([raw]() { delete raw; });
    }
}

bool XRayView::Owns(HWND h) const {
    for (const auto& s : surfaces) if (s && s->hwnd == h) return true;
    return false;
}

void XRayView::RenderAll() {
    for (auto& s : surfaces) {
        XRaySurface* sp = s.get();
        bool isMain = sp == main;
        s->Render([this, sp, isMain](Graphics& g) { PaintScene(g, sp->mon, isMain, entries, query, selected, hover); });
    }
}

std::vector<int> XRayView::MatchList() const {
    std::vector<int> r;
    for (int i = 0; i < (int)entries.size(); i++) {
        const XRayEntry& e = entries[i];
        if (Matches(e.title, query) || (e.w && Matches(e.w->process, query))) r.push_back(i);
    }
    return r;
}

void XRayView::OnKey(UINT vk, bool shift) {
    std::vector<int> m = MatchList();
    switch (vk) {
        case VK_ESCAPE: Close(); return;
        case VK_RETURN:
            if (selected >= 0 && selected < (int)entries.size() && (query.empty() || std::find(m.begin(), m.end(), selected) != m.end())) Activate(entries[selected], shift);
            else if (!m.empty()) Activate(entries[m[0]], shift);
            return;
        case VK_BACK:
            if (!query.empty()) { query.pop_back(); SelectFirstMatch(); RenderAll(); }
            return;
        case VK_DOWN: case VK_RIGHT: case VK_TAB:
            Step(m, vk == VK_TAB && shift ? -1 : 1);
            return;
        case VK_UP: case VK_LEFT:
            Step(m, -1);
            return;
    }
}

void XRayView::Step(const std::vector<int>& m, int dir) {
    if (m.empty()) return;
    auto it = std::find(m.begin(), m.end(), selected);
    int pos = it == m.end() ? -1 : (int)(it - m.begin());
    pos = pos < 0 ? 0 : (pos + dir + (int)m.size()) % (int)m.size();
    selected = m[pos];
    RenderAll();
}

void XRayView::OnChar(wchar_t c) {
    if (c < 0x20 || c == 0x7F) return;
    if (query.empty() && c >= L'0' && c <= L'9') {
        digits += c;
        int count = (int)entries.size();
        if (count < 10 || digits.size() >= Util::Inv(count).size()) PickNumber(false);
        else digitTimer.Start();
        return;
    }
    query += c;
    SelectFirstMatch();
    RenderAll();
}

void XRayView::PickNumber(bool) {
    digitTimer.Stop();
    wstr d = digits;
    digits.clear();
    int n;
    if (!Util::TryParseInt(d, n)) return;
    for (const XRayEntry& e : entries) if (e.number == n) { XRayEntry copy = e; Activate(copy, Native::KeyDown(VK_SHIFT)); return; }
}

void XRayView::SelectFirstMatch() {
    std::vector<int> m = MatchList();
    selected = m.empty() ? -1 : m[0];
}

void XRayView::OnClick(int x, int y, bool shift) {
    int i = HitTest(x, y);
    if (i < 0) { Close(); return; }
    XRayEntry copy = entries[i];
    Activate(copy, shift);
}

void XRayView::OnMove(int x, int y) {
    int i = HitTest(x, y);
    if (i != hover) { hover = i; RenderAll(); }
}

int XRayView::HitTest(int x, int y) const {
    for (int i = 0; i < (int)entries.size(); i++) if (entries[i].label.Contains(x, y)) return i;
    int best = -1;
    long long bestArea = LLONG_MAX;
    for (int i = 0; i < (int)entries.size(); i++) {
        const XRayEntry& e = entries[i];
        if (e.minimized || !e.frame.Contains(x, y)) continue;
        if (e.frame.Area() < bestArea) { bestArea = e.frame.Area(); best = i; }
    }
    return best;
}

void XRayView::Activate(const XRayEntry& e, bool shift) {
    WinPtr w = e.w;
    // bring it forward while X-ray still holds the foreground, then close
    app->BringForward(w, shift);
    Close();
}

void XRayView::OnDeactivated(HWND other) {
    if (!IsOpen() || Owns(other)) return;
    // later, not inside the surface's own message; the view may be gone by then
    Ui::Post([]() {
        GlimpseApp* a = GlimpseApp::current;
        if (a && a->xray && a->xray->IsOpen() && !a->xray->Owns(GetForegroundWindow())) a->xray->Close();
    });
}

// ------------------------------------------------------------------ painting

static float LabelWidth(const XRayEntry& e, float k) {
    Gdiplus::Font* f = Draw::Font(16 * k, true);
    return 10 * k + 26 * k + 10 * k + 20 * k + 8 * k + std::min(Draw::Measure(e.title, f).Width, 420 * k) + 16 * k;
}

static Rc LabelAt(Graphics& g, XRayEntry& e, int x, int y, float k, bool selected, int alpha) {
    Gdiplus::Font* f = Draw::Font(16 * k, true);
    float w = LabelWidth(e, k), h = 38 * k;
    RectF r((REAL)x, (REAL)y, w, h);
    Draw::Fill(g, r, h / 2, selected ? Theme::Accent() : Color((BYTE)std::min(alpha, 230), 44, 44, 56));
    RectF num(x + 8 * k, y + (h - 26 * k) / 2, 26 * k, 26 * k);
    Draw::Fill(g, num, 13 * k, selected ? Color(255, 255, 255) : Color((BYTE)alpha, 208, 208, 216));
    Gdiplus::Font* fn = Draw::Font(14 * k, true);
    wstr ns = Util::Inv(e.number);
    SizeF nsz = Draw::Measure(ns, fn);
    Draw::Text(g, ns, fn, selected ? Theme::Accent() : Color((BYTE)alpha, 40, 40, 50), num.X + (num.Width - nsz.Width) / 2, num.Y + (num.Height - nsz.Height) / 2);
    RectF badge(num.GetRight() + 10 * k, y + (h - 20 * k) / 2, 20 * k, 20 * k);
    Gdiplus::Bitmap* icon = e.w ? WindowRegistry::LoadIcon(*e.w, (int)(20 * k)) : nullptr;
    Draw::AppBadge(g, badge, e.title, icon);
    float fh = Draw::Height(f);
    Draw::TextIn(g, e.title, f, Color((BYTE)alpha, 255, 255, 255), RectF(badge.GetRight() + 8 * k, y + (h - fh) / 2, std::min(Draw::Measure(e.title, f).Width, 420 * k) + 2, fh + 2));
    return Rc(x, y, x + (int)w, y + (int)h);
}

void XRayView::PaintScene(Graphics& g, const MonitorInfo& mon, bool isMain, std::vector<XRayEntry>& entries, const wstr& query, int selected, int hover) {
    float k = mon.dpi / 96.0f;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    {
        SolidBrush b(Color(160, 22, 22, 30));
        g.FillRectangle(&b, mon.bounds.L, mon.bounds.T, mon.bounds.W(), mon.bounds.H());
    }
    bool filtering = !query.empty();
    auto hitOf = [&](const XRayEntry& e) { return !filtering || Matches(e.title, query) || (e.w && Matches(e.w->process, query)); };

    // outlines, bottom of the stack first so the top window's outline stays on top
    for (int i = (int)entries.size() - 1; i >= 0; i--) {
        XRayEntry& e = entries[i];
        if (e.minimized) continue;
        bool sel = i == selected || i == hover;
        if (sel) continue;
        RectF r((REAL)e.frame.L, (REAL)e.frame.T, (REAL)e.frame.W(), (REAL)e.frame.H());
        Draw::Stroke(g, r, 6 * k, Color((BYTE)(hitOf(e) ? 210 : 70), 214, 214, 224), 1.6f * k, false);
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = (int)entries.size() - 1; i >= 0; i--) {
            XRayEntry& e = entries[i];
            if (e.minimized) continue;
            bool sel = i == selected || i == hover;
            if ((pass == 1) != sel) continue;
            bool hit = hitOf(e);
            if (sel) {
                RectF r((REAL)e.frame.L, (REAL)e.frame.T, (REAL)e.frame.W(), (REAL)e.frame.H());
                Draw::Fill(g, r, 8 * k, Color(225, 45, 38, 112));
                RectF outer(r.X - 3 * k, r.Y - 3 * k, r.Width + 6 * k, r.Height + 6 * k);
                Draw::Stroke(g, outer, 10 * k, Color(120, 124, 108, 242), 6 * k, false);
                Draw::Stroke(g, r, 8 * k, Color(160, 150, 255), 3 * k, false);
            }
            e.label = LabelAt(g, e, e.frame.L + (int)(18 * k), e.frame.T + (int)(16 * k), k, sel, hit ? 255 : 90);
        }
    if (!isMain) return;

    Rc work = mon.work;
    // search box
    float bw = std::min(660 * k, work.W() - 40 * k), bh = 64 * k;
    RectF box(work.CX() - bw / 2, work.T + 40 * k, bw, bh);
    Draw::Fill(g, RectF(box.X, box.Y + 3 * k, box.Width, box.Height), 16 * k, Color(60, 0, 0, 0));
    Draw::Fill(g, box, 16 * k, Color(255, 255, 255));
    Draw::Glyph(g, L"search", RectF(box.X + 22 * k, box.Y + (bh - 24 * k) / 2, 24 * k, 24 * k), Color(70, 70, 82));
    Gdiplus::Font* fq = Draw::Font(24 * k, false);
    float fqh = Draw::Height(fq);
    float tx = box.X + 64 * k, ty = box.Y + (bh - fqh) / 2;
    float qw = Draw::Measure(query, fq).Width;
    Draw::Text(g, query, fq, Theme::Ink(), tx, ty);
    {
        Pen caret(Theme::Accent(), 2.5f * k);
        g.DrawLine(&caret, tx + qw + 2 * k, ty + 4 * k, tx + qw + 2 * k, ty + fqh - 4 * k);
    }
    Gdiplus::Font* fp = Draw::Font(20 * k, false);
    Draw::Text(g, Texts::T(L"xray.search"), fp, Color(160, 160, 172), tx + qw + 14 * k, box.Y + (bh - Draw::Height(fp)) / 2);
    int matches = 0;
    for (const XRayEntry& e : entries) if (hitOf(e)) matches++;
    Gdiplus::Font* fk = Draw::Font(15 * k, true);
    wstr enter = Texts::T(L"xray.enter");
    float ew = Draw::Measure(enter, fk).Width + 20 * k;
    RectF keyR(box.GetRight() - ew - 22 * k, box.Y + (bh - 30 * k) / 2, ew, 30 * k);
    Draw::Fill(g, keyR, 6 * k, Color(246, 246, 249));
    Draw::Stroke(g, keyR, 6 * k, Color(205, 205, 214), 1.2f * k, false);
    Draw::Text(g, enter, fk, Theme::Ink(), keyR.X + 10 * k, keyR.Y + (keyR.Height - Draw::Height(fk)) / 2);
    if (filtering) {
        Gdiplus::Font* fm = Draw::Font(16 * k, false);
        wstr mt = matches == 0 ? Texts::T(L"xray.noMatch") : Texts::P(L"xray.matches", matches);
        float mw = Draw::Measure(mt, fm).Width;
        Draw::Text(g, mt, fm, Color(110, 110, 124), keyR.X - mw - 16 * k, box.Y + (bh - Draw::Height(fm)) / 2);
    }

    // minimized windows in a row along the bottom
    std::vector<int> mins;
    for (int i = 0; i < (int)entries.size(); i++) if (entries[i].minimized) mins.push_back(i);
    if (!mins.empty()) {
        Gdiplus::Font* fl = Draw::Font(14 * k, true);
        wstr label = Texts::T(L"xray.minimized");
        if (!label.empty()) CharUpperBuffW(&label[0], (DWORD)label.size());
        float total = Draw::Measure(label, fl).Width + 18 * k;
        std::vector<float> widths;
        for (int i : mins) { float w = LabelWidth(entries[i], k); widths.push_back(w); total += w + 12 * k; }
        float x = std::max(work.L + 20 * k, work.CX() - total / 2);
        float y = work.B - 150 * k;
        Draw::Text(g, label, fl, Color(220, 220, 228), x, y + (34 * k - Draw::Height(fl)) / 2);
        x += Draw::Measure(label, fl).Width + 18 * k;
        for (size_t j = 0; j < mins.size(); j++) {
            int i = mins[j];
            XRayEntry& e = entries[i];
            e.label = LabelAt(g, e, (int)x, (int)y, k, i == selected || i == hover, hitOf(e) ? 255 : 90);
            x += widths[j] + 12 * k;
            if (x > work.R - 40 * k) break;
        }
    }
    if (entries.empty()) {
        Gdiplus::Font* fe = Draw::Font(20 * k, false);
        wstr t = Texts::T(L"xray.empty");
        SizeF sz = Draw::Measure(t, fe);
        Draw::Text(g, t, fe, Color(255, 255, 255), work.CX() - sz.Width / 2, work.CY() - sz.Height / 2);
    }
    // hint line
    wstr hint = Texts::T(L"xray.hint");
    float hw = Draw::RichText(g, hint, 17 * k, Color(225, 225, 232), Color(255, 255, 255), 0, 0, false);
    Draw::RichText(g, hint, 17 * k, Color(225, 225, 232), Color(255, 255, 255), work.CX() - hw / 2, work.B - 64 * k, true);
}

// ------------------------------------------------------------------ surface: a focusable full-monitor layered window

static const wchar_t* kXRayClass = L"GlimpseXRay";

XRaySurface::XRaySurface(XRayView* v, const MonitorInfo& m, HWND owner) : mon(m), view(v) {
    static bool registered = false;
    if (!registered) { Ui::RegisterClass(kXRayClass, Proc); registered = true; }
    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kXRayClass, L"Glimpse X-ray", WS_POPUP,
                           mon.bounds.L, mon.bounds.T, mon.bounds.W(), mon.bounds.H(), owner, nullptr, Ui::Instance(), this);
    if (!hwnd) throw std::runtime_error("cannot create the x-ray window");
}

XRaySurface::~XRaySurface() {
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
    }
}

void XRaySurface::ShowNow() { ShowWindow(hwnd, SW_SHOW); }

void XRaySurface::Render(std::function<void(Graphics&)> paint) {
    int w = mon.bounds.W(), h = mon.bounds.H();
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(dc); return; }
    HGDIOBJ old = SelectObject(dc, dib);
    {
        Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.TranslateTransform((REAL)-mon.bounds.L, (REAL)-mon.bounds.T);
        try { paint(g); } catch (const std::exception& ex) { Log::Error(L"x-ray paint: " + Util::Widen(ex.what())); }
        g.Flush(FlushIntentionSync);
    }
    POINT dst = { mon.bounds.L, mon.bounds.T }, src = { 0, 0 };
    SIZE size = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, nullptr, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
}

LRESULT CALLBACK XRaySurface::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* self = (XRaySurface*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self) {
        XRayView* view = self->view;
        int ox = self->mon.bounds.L, oy = self->mon.bounds.T;
        try {
            switch (m) {
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    view->OnKey((UINT)wp, Native::KeyDown(VK_SHIFT));
                    return 0;
                case WM_CLOSE:
                    view->Close();
                    return 0;
                case WM_CHAR:
                    view->OnChar((wchar_t)wp);
                    return 0;
                case WM_LBUTTONUP:
                    view->OnClick(GET_X_LPARAM(lp) + ox, GET_Y_LPARAM(lp) + oy, Native::KeyDown(VK_SHIFT));
                    return 0;
                case WM_RBUTTONUP:
                    view->Close();
                    return 0;
                case WM_MOUSEMOVE:
                    view->OnMove(GET_X_LPARAM(lp) + ox, GET_Y_LPARAM(lp) + oy);
                    break;
                case WM_ACTIVATE:
                    if (LOWORD(wp) == WA_INACTIVE) view->OnDeactivated((HWND)lp);
                    break;
                case WM_DPICHANGED:
                    return 0;
            }
        } catch (const std::exception& ex) { Log::Error(L"x-ray input: " + Util::Widen(ex.what())); }
    }
    return DefWindowProcW(h, m, wp, lp);
}

} // namespace glimpse
