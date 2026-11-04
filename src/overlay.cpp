// Glimpse native build: drag feedback and flashes on click-through layered windows.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"

namespace glimpse {

using namespace Gdiplus;

namespace OverlayElements {

namespace {

float K(int dpi) { return dpi / 96.0f; }

wstr Argb(const Color& c) { return Util::Inv((long long)c.GetValue()); }

wstr P(int x, int y) { return Util::Inv(x) + L"," + Util::Inv(y); }

OverlayElement Make(const Rc& bounds, const wstr& key, std::function<void(Graphics&)> draw) {
    OverlayElement e;
    e.bounds = bounds;
    e.key = key;
    e.draw = std::move(draw);
    return e;
}

OverlayElement Box(const Rc& r, int dpi, const wstr& text, Color fill, Color border, Color ink) {
    float k = K(dpi);
    wstr key = L"box|" + r.ToString() + L"|" + text + L"|" + Argb(fill) + L"|" + Util::Inv(dpi);
    return Make(r.Inflate(3), key, [=](Graphics& g) {
        RectF rf((REAL)r.L, (REAL)r.T, (REAL)r.W(), (REAL)r.H());
        Draw::Fill(g, rf, 6 * k, fill);
        Draw::Stroke(g, rf, 6 * k, border, 2 * k, false);
        Gdiplus::Font* f = Draw::Font(14 * k, true);
        SizeF sz = Draw::Measure(text, f);
        float fh = Draw::Height(f);
        float tx = r.L + std::max(6 * k, (r.W() - sz.Width) / 2);
        Draw::TextIn(g, text, f, ink, RectF(tx, r.T + (r.H() - fh) / 2, r.W() - 12 * k, fh + 2));
    });
}

OverlayElement ArrowLine(PointF a, PointF b, int dpi, Color c) {
    float k = K(dpi);
    Rc bounds((int)std::min(a.X, b.X) - 12, (int)std::min(a.Y, b.Y) - 12, (int)std::max(a.X, b.X) + 12, (int)std::max(a.Y, b.Y) + 12);
    wchar_t buf[96];
    swprintf(buf, 96, L"arrow|%.1f,%.1f|%.1f,%.1f|", a.X, a.Y, b.X, b.Y);
    return Make(bounds, buf + Argb(c), [=](Graphics& g) { Draw::Arrow(g, a, b, c, 2.5f * k); });
}

// dark summary chip near the cursor: coloured dots and counts
OverlayElement Chip(int x, int y, int dpi, const std::vector<std::pair<Color, wstr>>& items, const Rc& keepIn) {
    float k = K(dpi);
    Gdiplus::Font* f = Draw::Font(14 * k, false);
    float h = 32 * k, pad = 12 * k, dot = 8 * k, gap = 14 * k;
    float w = pad;
    wstr sb;
    for (const auto& it : items) { w += dot + 6 * k + Draw::Measure(it.second, f).Width + gap; sb += it.second + L"|"; }
    w += pad - gap;
    if (!keepIn.IsEmpty()) {
        // flip to the left of the cursor or above it near the screen edge
        if (x + w > keepIn.R - 4) x = std::max(keepIn.L + 4, x - (int)w - (int)(56 * k));
        if (y + h > keepIn.B - 4) y = std::max(keepIn.T + 4, y - (int)h - (int)(52 * k));
    }
    wstr key = L"chip|" + P(x, y) + L"|" + sb + Util::Inv(dpi);
    return Make(Rc(x - 2, y - 2, x + (int)w + 3, y + (int)h + 3), key, [=](Graphics& g) {
        Draw::Fill(g, RectF((REAL)x, (REAL)y, w, h), 8 * k, Draw::Alpha(Theme::Chip(), 240));
        float cx = x + pad;
        float fh = Draw::Height(f);
        for (const auto& it : items) {
            SolidBrush b(it.first);
            g.FillEllipse(&b, cx, y + (h - dot) / 2, dot, dot);
            cx += dot + 6 * k;
            Draw::Text(g, it.second, f, Color(255, 255, 255), cx, y + (h - fh) / 2);
            cx += Draw::Measure(it.second, f).Width + gap;
        }
    });
}

OverlayElement GuideLine(const Rc& line, int dpi, const wstr& label, bool horizontal) {
    float k = K(dpi);
    Gdiplus::Font* f = Draw::Font(13 * k, true);
    SizeF sz = Draw::Measure(label, f);
    float lw = sz.Width + 20 * k, lh = 26 * k;
    float lx = horizontal ? line.R - lw : line.L + 6 * k;
    float ly = horizontal ? line.T + 6 * k : line.B - lh;
    Rc bounds = line.Inflate(4).Union(Rc((int)lx - 2, (int)ly - 2, (int)(lx + lw) + 3, (int)(ly + lh) + 3));
    wstr key = L"guide|" + line.ToString() + L"|" + label + L"|" + Util::Inv(dpi);
    return Make(bounds, key, [=](Graphics& g) {
        {
            Pen pen(Theme::GuideC(), 2 * k);
            REAL dash[] = { 4.0f, 3.0f };
            pen.SetDashPattern(dash, 2);
            if (horizontal) g.DrawLine(&pen, (REAL)line.L, (REAL)line.T, (REAL)line.R, (REAL)line.T);
            else g.DrawLine(&pen, (REAL)line.L, (REAL)line.T, (REAL)line.L, (REAL)line.B);
        }
        Draw::Fill(g, RectF(lx, ly, lw, lh), 6 * k, Theme::GuideC());
        Draw::Text(g, label, f, Color(255, 255, 255), lx + 10 * k, ly + (lh - Draw::Height(f)) / 2);
    });
}

OverlayElement Callout(int x, int y, int dpi, const wstr& title, const wstr& text, const Rc& keepIn) {
    float k = K(dpi);
    Gdiplus::Font* ft = Draw::Font(15 * k, true);
    Gdiplus::Font* fs = Draw::Font(13 * k, false);
    float w = std::max(Draw::Measure(title, ft).Width, Draw::Measure(text, fs).Width) + 32 * k;
    float h = 62 * k;
    if (!keepIn.IsEmpty() && x + w > keepIn.R - 4) x = std::max(keepIn.L + 4, keepIn.R - 4 - (int)std::ceil(w));
    wstr key = L"callout|" + P(x, y) + L"|" + title + L"|" + text + L"|" + Util::Inv(dpi);
    return Make(Rc(x - 3, y - 3, x + (int)w + 4, y + (int)h + 4), key, [=](Graphics& g) {
        RectF r((REAL)x, (REAL)y, w, h);
        Draw::Fill(g, r, 8 * k, Color(255, 255, 255));
        Draw::Stroke(g, r, 8 * k, Color(230, 230, 236), 1, false);
        SizeF st = Draw::Measure(title, ft);
        Draw::Text(g, title, ft, Theme::Red(), x + (w - st.Width) / 2, y + 10 * k);
        SizeF ss = Draw::Measure(text, fs);
        Draw::Text(g, text, fs, Color(75, 75, 88), x + (w - ss.Width) / 2, y + 34 * k);
    });
}

OverlayElement DirChip(int x, int y, int dpi, const wstr& dir, const wstr& text, const Rc& box, const Rc& keepIn) {
    float k = K(dpi);
    Gdiplus::Font* f = Draw::Font(14 * k, true);
    float h = 32 * k, w = 14 * k + 18 * k + 8 * k + Draw::Measure(text, f).Width + 16 * k;
    if (!keepIn.IsEmpty() && x + w > keepIn.R - 4) {
        // no room right of the box: put it under the box, or above when that leaves the screen
        x = std::max(keepIn.L + 4, std::min(box.L, keepIn.R - 4 - (int)w));
        y = box.B + (int)(10 * k);
        if (y + h > keepIn.B - 4) y = box.T - (int)h - (int)(10 * k);
    }
    wstr key = L"dir|" + P(x, y) + L"|" + dir + L"|" + text + L"|" + Util::Inv(dpi);
    return Make(Rc(x - 2, y - 2, x + (int)w + 3, y + (int)h + 3), key, [=](Graphics& g) {
        Draw::Fill(g, RectF((REAL)x, (REAL)y, w, h), h / 2, Color(251, 191, 36));
        Draw::Glyph(g, dir, RectF(x + 12 * k, y + (h - 18 * k) / 2, 18 * k, 18 * k), Theme::Ink());
        Draw::Text(g, text, f, Theme::Ink(), x + 38 * k, y + (h - Draw::Height(f)) / 2);
    });
}

BitmapPtr IconOf(const WinPtr& w, const BitmapPtr& fallback, int size) {
    if (!w || !w->handle) return fallback;
    WindowRegistry::LoadIcon(*w, size);
    return w->icon;
}

} // namespace

OverlayElement Pill(int x, int y, int dpi, const wstr& text, BitmapPtr icon, bool badgeOn, const wstr& badgeName, Color fill, Color border, Color ink, int trailing, Color trailColor, float maxWidth) {
    float k = K(dpi);
    Gdiplus::Font* f = Draw::Font(14 * k, true);
    float h = 28 * k, pad = 9 * k, badge = icon || badgeOn ? 18 * k : 0;
    float tw = std::min(Draw::Measure(text, f).Width, maxWidth > 0 ? maxWidth * k : 380 * k);
    float w = pad + (badge > 0 ? badge + 7 * k : 0) + tw + pad;
    float total = w + (trailing != 0 ? 8 * k + 22 * k : 0);
    Rc bounds(x - 2, y - 2, x + (int)std::ceil(total) + 3, y + (int)std::ceil(h) + 3);
    wstr key = L"pill|" + P(x, y) + L"|" + text + L"|" + Argb(fill) + L"|" + Util::Inv(trailing) + L"|" + Util::Inv(dpi);
    wstr name = badgeOn ? badgeName : text;
    return Make(bounds, key, [=](Graphics& g) {
        RectF r((REAL)x, (REAL)y, w, h);
        Draw::Fill(g, r, 7 * k, fill);
        if (border.GetA() > 0) Draw::Stroke(g, r, 7 * k, border, 2 * k, false);
        float cx = x + pad;
        if (badge > 0) {
            Draw::AppBadge(g, RectF(cx, y + (h - badge) / 2, badge, badge), name, icon.get());
            cx += badge + 7 * k;
        }
        float fh = Draw::Height(f);
        Draw::TextIn(g, text, f, ink, RectF(cx, y + (h - fh) / 2 - 1, tw + 2, fh + 2));
        if (trailing != 0) {
            RectF t(x + w + 8 * k, y + (h - 22 * k) / 2, 22 * k, 22 * k);
            if (trailing == 1) Draw::CheckBadge(g, t, trailColor); else Draw::CrossBadge(g, t, Color(255, 255, 255), fill);
        }
    });
}

OverlayElement Outline(const Rc& r, int dpi, Color c, bool dashed, int fillAlpha, float width) {
    float k = K(dpi);
    int pad = (int)std::ceil(width * k) + 2;
    wchar_t buf[48];
    swprintf(buf, 48, L"|%d|%d|%.2f|", dashed ? 1 : 0, fillAlpha, (double)width);
    wstr key = L"outline|" + r.ToString() + L"|" + Argb(c) + buf + Util::Inv(dpi);
    return Make(r.Inflate(pad), key, [=](Graphics& g) {
        RectF rf(r.L + width * k / 2, r.T + width * k / 2, r.W() - width * k, r.H() - width * k);
        if (fillAlpha > 0) Draw::Fill(g, rf, 6 * k, Draw::Alpha(c, fillAlpha));
        auto p = Draw::Round(rf, 6 * k);
        Pen pen(c, width * k);
        if (dashed) { REAL dash[] = { 3.0f, 2.0f }; pen.SetDashPattern(dash, 2); }
        g.DrawPath(&pen, p.get());
    });
}

wstr ReasonText(const wstr& reason) {
    if (reason == L"elevated") return Texts::T(L"drag.reason.elevated");
    if (reason == L"stubborn") return Texts::T(L"drag.reason.stubborn");
    if (reason == L"excluded") return Texts::T(L"drag.reason.excluded");
    if (reason == L"hung") return Texts::T(L"drag.reason.hung");
    if (reason == L"topmost") return Texts::T(L"drag.reason.topmost");
    if (reason == L"guide") return Texts::T(L"drag.reason.guide");
    if (reason == L"paused") return Texts::T(L"drag.reason.paused");
    if (reason == L"limit") return Texts::T(L"drag.reason.limit");
    if (reason == L"shift") return Texts::T(L"drag.reason.shift");
    return Texts::T(L"drag.reason.noSpace");
}

Elements ForDrag(const DragScene& s, std::function<int(const Rc&)> dpiAt, std::function<Rc(const Rc&)> screenAt) {
    Elements els;
    int cursorDpi = dpiAt(Rc(s.cursor.x, s.cursor.y, s.cursor.x + 1, s.cursor.y + 1));
    const Color none(0, 0, 0, 0);
    for (const SceneWindow& w : s.others) {
        int dpi = dpiAt(w.frame);
        float k = K(dpi);
        BitmapPtr icon = IconOf(w.w, w.icon, (int)(18 * k));
        if (w.state == 0 && w.zone) {
            els.push_back(Pill(w.zone->rect.L + (int)(4 * k), w.zone->rect.T + (int)(4 * k), dpi, w.title, icon, true, w.title,
                               Theme::GreenSoft(), Theme::GreenLine(), Theme::Ink(), 1, Theme::Green(), 300));
        } else if (w.state == 1) {
            els.push_back(Outline(w.frame, dpi, Theme::Amber(), true, 40, 2.5f));
            Rc at = w.zone ? w.zone->rect : w.frame;
            els.push_back(Pill(at.L + (int)(4 * k), at.T + (int)(4 * k), dpi, w.title, icon, true, w.title, Color(251, 191, 36), none, Theme::Ink(), 0, none, 300));
            if (w.newZone) {
                Rc z = w.newZone->rect;
                wstr peek = Texts::F(L"drag.peek", { { L"title", Util::Shorten(w.title, 30) } });
                Gdiplus::Font* f = Draw::Font(14 * k, true);
                int bw = std::max(z.W(), (int)(Draw::Measure(peek, f).Width + 40 * k));
                int bh = std::max((int)(36 * k), std::min(z.H(), (int)(48 * k)));
                Rc box;
                switch (w.newZone->edge) {
                    case Edge::Bottom: box = Rc(w.newFrame.L, w.newFrame.B - bh, w.newFrame.L + std::max(bw, std::min(w.newFrame.W(), bw)), w.newFrame.B); break;
                    case Edge::Top: box = Rc(w.newFrame.L, w.newFrame.T, w.newFrame.L + bw, w.newFrame.T + bh); break;
                    default: box = Rc(z.L, z.T, z.L + bw, z.T + bh); break;
                }
                els.push_back(Box(box, dpi, peek, Theme::AmberSoft(), Theme::Amber(), Color(120, 53, 15)));
                int dx = w.newFrame.L - w.frame.L, dy = w.newFrame.T - w.frame.T;
                int px = Util::RoundEven(std::sqrt((double)dx * dx + (double)dy * dy) * 96.0 / dpi);
                PointF from((REAL)(box.CX() - dx), (REAL)(box.CY() - dy - (dy > 0 ? bh : 0)));
                PointF to((REAL)box.CX(), (REAL)(dy > 0 ? box.T - 4 : box.CY()));
                if (std::abs(dx) > std::abs(dy)) {
                    from = PointF((REAL)(box.CX() - dx), (REAL)box.CY());
                    to = PointF((REAL)(box.CX() + (dx > 0 ? -bw / 2 : bw / 2)), (REAL)box.CY());
                }
                if (std::abs(to.X - from.X) + std::abs(to.Y - from.Y) > 12) els.push_back(ArrowLine(from, to, dpi, Theme::Amber()));
                wstr dir = std::abs(dx) > std::abs(dy) ? (dx > 0 ? L"right" : L"left") : (dy > 0 ? L"down" : L"up");
                els.push_back(DirChip(box.R + (int)(16 * k), box.CY() - (int)(16 * k), dpi, dir, Texts::F(L"drag.movesPx", { { L"px", Util::Inv(px) } }), box, screenAt(box)));
            }
        } else if (w.state == 2) {
            els.push_back(Outline(w.frame, dpi, Theme::Red(), true, 36, 2.5f));
            els.push_back(Pill(w.frame.L + (int)(12 * k), w.frame.T + (int)(12 * k), dpi, w.title, icon, true, w.title, Color(239, 68, 68), none,
                               Color(255, 255, 255), 2, Color(255, 255, 255, 255), 260));
            els.push_back(Callout(w.frame.L + (int)(12 * k), w.frame.T + (int)(56 * k), dpi, Texts::T(L"drag.wouldBeHidden"), ReasonText(w.reason), screenAt(w.frame)));
        }
    }
    // the dragged window's own status
    {
        int dpi = dpiAt(s.dragged);
        float k = K(dpi);
        if (s.draggedZone) {
            BitmapPtr icon = s.draggedWindow && s.draggedWindow->handle ? IconOf(s.draggedWindow, BitmapPtr(), (int)(18 * k)) : BitmapPtr();
            els.push_back(Pill(s.dragged.L + (int)(40 * k), s.dragged.T - (int)(40 * k), dpi, Texts::T(L"drag.keeps"), icon, true,
                               s.draggedWindow ? s.draggedWindow->ShortTitle() : wstr(L"?"), Theme::GreenSoft(), Theme::GreenLine(), Theme::Ink(), 1, Theme::Green(), 360));
        } else {
            els.push_back(Outline(s.dragged, dpi, Theme::Red(), false, 0, 3));
            els.push_back(Pill(s.dragged.L + (int)(40 * k), s.dragged.T - (int)(40 * k), dpi, Texts::T(L"drag.noGlimpse"), BitmapPtr(), false, wstr(),
                               Theme::Red(), none, Color(255, 255, 255), 2, Color(255, 255, 255, 255), 360));
        }
    }
    if (!s.minOutline.IsEmpty()) {
        int dpi = dpiAt(s.minOutline);
        float k = K(dpi);
        els.push_back(Outline(s.minOutline, dpi, Theme::Accent(), true, 0, 2.5f));
        if (!s.minText.empty())
            els.push_back(Pill(s.minOutline.L + (int)(8 * k), s.minOutline.B - (int)(36 * k), dpi, s.minText, BitmapPtr(), false, wstr(),
                               Theme::Accent(), none, Color(255, 255, 255), 0, none, 300));
    }
    int shown = 0;
    for (const Guide& gd : s.guides) {
        if (shown++ >= 2) break;
        int dpi = dpiAt(gd.snapped);
        bool horiz = gd.edge == Edge::Top || gd.edge == Edge::Bottom;
        Rc line = horiz ? Rc(std::min(gd.snapped.L, gd.zone.L), gd.coord, std::max(gd.snapped.R, gd.zone.R), gd.coord + 1)
                        : Rc(gd.coord, std::min(gd.snapped.T, gd.zone.T), gd.coord + 1, std::max(gd.snapped.B, gd.zone.B));
        els.push_back(GuideLine(line, dpi, Texts::T(L"drag.guide"), horiz));
    }
    // summary chip by the cursor
    std::vector<std::pair<Color, wstr>> items;
    if (s.stay > 0) items.push_back(std::make_pair(Theme::GreenLine(), Texts::P(L"drag.stay", s.stay)));
    if (s.moves > 0) items.push_back(std::make_pair(Theme::Amber(), Texts::P(L"drag.move", s.moves)));
    if (s.hidden > 0) items.push_back(std::make_pair(Color(248, 113, 113), Texts::P(L"drag.hide", s.hidden)));
    if (!items.empty()) {
        float k = K(cursorDpi);
        Rc cursorRect(s.cursor.x, s.cursor.y, s.cursor.x + 1, s.cursor.y + 1);
        els.push_back(Chip(s.cursor.x + (int)(28 * k), s.cursor.y + (int)(26 * k), cursorDpi, items, screenAt(cursorRect)));
    }
    return els;
}

Elements ForFlash(const std::vector<Rc>& frames, std::function<int(const Rc&)> dpiAt) {
    Elements els;
    for (const Rc& r : frames) els.push_back(Outline(r.Inflate(2), dpiAt(r), Theme::Accent(), false, 0, 3.5f));
    return els;
}

} // namespace OverlayElements

// ================================================================== surface: one layered window per monitor

static const wchar_t* kOverlayClass = L"GlimpseOverlay";

OverlaySurface::OverlaySurface(const MonitorInfo& m, std::function<void(const wstr&)> dpi, HWND owner) : mon(m), onDpi(std::move(dpi)) {
    static bool registered = false;
    if (!registered) { Ui::RegisterClass(kOverlayClass, Proc); registered = true; }
    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kOverlayClass, L"Glimpse overlay",
                           WS_POPUP, mon.bounds.L, mon.bounds.T, 1, 1, owner, nullptr, Ui::Instance(), this);
    if (!hwnd) throw std::runtime_error("cannot create the overlay window");
    Idle();
}

OverlaySurface::~OverlaySurface() {
    FreeBuffer();
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
    }
}

LRESULT CALLBACK OverlaySurface::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* self = (OverlaySurface*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_DPICHANGED:
            if (self && self->onDpi) { auto cb = self->onDpi; Ui::Post([cb]() { cb(L"dpi"); }); }
            return 0;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void OverlaySurface::Idle() {
    FreeBuffer();
    shown.clear();
    big = false;
    // 1x1 transparent pixel: stays "visible" to get DPI notifications, costs nothing
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 1;
    bi.bmiHeader.biHeight = -1;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* px = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &px, nullptr, 0);
    if (hb) {
        *(uint32_t*)px = 0;
        HGDIOBJ old = SelectObject(dc, hb);
        POINT dst = { mon.bounds.L, mon.bounds.T }, src = { 0, 0 };
        SIZE size = { 1, 1 };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd, nullptr, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
        DeleteObject(hb);
    }
    DeleteDC(dc);
    ShowWindow(hwnd, SW_SHOWNA);
}

void OverlaySurface::EnsureBuffer() {
    int w = mon.bounds.W(), h = mon.bounds.H();
    if (dib && bw == w && bh == h) return;
    FreeBuffer();
    bw = w;
    bh = h;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    memDC = CreateCompatibleDC(nullptr);
    dib = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(memDC); memDC = nullptr; throw std::runtime_error("CreateDIBSection failed"); }
    oldObj = SelectObject(memDC, dib);
}

void OverlaySurface::FreeBuffer() {
    if (memDC) {
        if (oldObj) SelectObject(memDC, oldObj);
        DeleteDC(memDC);
        memDC = nullptr;
    }
    oldObj = nullptr;
    if (dib) { DeleteObject(dib); dib = nullptr; }
    bits = nullptr;
}

void OverlaySurface::Render(const Elements& all) {
    std::vector<const OverlayElement*> mine;
    for (const OverlayElement& e : all) if (e.bounds.Intersects(mon.bounds)) mine.push_back(&e);
    if (mine.empty()) { if (big) Idle(); return; }
    std::map<wstr, Rc> now;
    for (const OverlayElement* e : mine) now[e->key] = e->bounds;
    Rc dirty;
    for (const auto& kv : now) if (!shown.count(kv.first)) dirty = dirty.Union(kv.second);
    for (const auto& kv : shown) if (!now.count(kv.first)) dirty = dirty.Union(kv.second);
    if (!big) dirty = mon.bounds;
    if (dirty.IsEmpty()) return;
    dirty = dirty.Intersect(mon.bounds);
    if (dirty.IsEmpty()) { shown = now; return; }
    EnsureBuffer();
    {
        Bitmap bmp(bw, bh, bw * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.TranslateTransform((REAL)-mon.bounds.L, (REAL)-mon.bounds.T);
        Gdiplus::Rect clip(dirty.L, dirty.T, dirty.W(), dirty.H());
        g.SetClip(clip);
        g.SetCompositingMode(CompositingModeSourceCopy);
        SolidBrush clear(Color(0, 0, 0, 0));
        g.FillRectangle(&clear, clip);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        for (const OverlayElement* e : mine) {
            if (!e->bounds.Intersects(dirty)) continue;
            try { e->draw(g); } catch (const std::exception& ex) { Log::Error(L"overlay draw: " + Util::Widen(ex.what())); }
        }
        g.Flush(FlushIntentionSync);
    }
    PushDib(big ? &dirty : nullptr);
    if (!big) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    big = true;
    shown = now;
}

void OverlaySurface::Clear() { if (big) Idle(); }

void OverlaySurface::PushDib(const Rc* dirtyScreen) {
    POINT dst = { mon.bounds.L, mon.bounds.T }, src = { 0, 0 };
    SIZE size = { bw, bh };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (!dirtyScreen) {
        UpdateLayeredWindow(hwnd, nullptr, &dst, &size, memDC, &src, 0, &blend, ULW_ALPHA);
        return;
    }
    Rc d = dirtyScreen->Offset(-mon.bounds.L, -mon.bounds.T);
    RECT dr = d.ToRECT();
    UPDATELAYEREDWINDOWINFO info = {};
    info.cbSize = sizeof info;
    info.pptDst = &dst;
    info.psize = &size;
    info.hdcSrc = memDC;
    info.pptSrc = &src;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = &dr;
    if (!UpdateLayeredWindowIndirect(hwnd, &info)) UpdateLayeredWindow(hwnd, nullptr, &dst, &size, memDC, &src, 0, &blend, ULW_ALPHA);
}

// ================================================================== manager

OverlayManager::OverlayManager(GlimpseApp* a) : app(a), flashTimer(500) {
    flashTimer.tick = [this]() { flashTimer.Stop(); flash.clear(); Compose(); };
    Theme::Refresh();
    OnMonitorsChanged();
}

OverlayManager::~OverlayManager() {
    flashTimer.Stop();
    surfaces.clear();
}

void OverlayManager::OnMonitorsChanged() {
    surfaces.clear();
    for (const MonitorInfo& m : app->mons) {
        try {
            GlimpseApp* a = app;
            surfaces.push_back(std::unique_ptr<OverlaySurface>(new OverlaySurface(m, [a](const wstr& why) { a->DisplaySignal(why); }, a->msg ? a->msg->handle : nullptr)));
        } catch (const std::exception& ex) { Log::Error(L"overlay: " + Util::Widen(ex.what())); }
    }
}

int OverlayManager::DpiAt(const Rc& r) const {
    const MonitorInfo* m = Monitors::At(app->mons, r);
    return m ? m->dpi : 96;
}

Rc OverlayManager::ScreenAt(const Rc& r) const {
    const MonitorInfo* m = Monitors::At(app->mons, r);
    return m ? m->bounds : Rc();
}

void OverlayManager::ShowDrag(const DragScene& scene) {
    // at most 30 redraws per second
    if (Clock::Ms() - lastRender < 30 && !drag.empty()) return;
    drag = OverlayElements::ForDrag(scene, [this](const Rc& r) { return DpiAt(r); }, [this](const Rc& r) { return ScreenAt(r); });
    Compose();
}

void OverlayManager::HideDrag() {
    if (drag.empty()) return;
    drag.clear();
    Compose();
}

void OverlayManager::Flash(const std::vector<Rc>& frames) {
    flash = OverlayElements::ForFlash(frames, [this](const Rc& r) { return DpiAt(r); });
    flashTimer.Start();
    Compose();
}

void OverlayManager::ShowPick(const Rc& frame) {
    pick.clear();
    pick.push_back(OverlayElements::Outline(frame.Inflate(2), DpiAt(frame), Theme::Accent(), false, 50, 4));
    Compose();
}

void OverlayManager::HidePick() {
    if (pick.empty()) return;
    pick.clear();
    Compose();
}

void OverlayManager::HideAll() {
    drag.clear();
    flash.clear();
    pick.clear();
    Compose();
}

void OverlayManager::Compose() {
    lastRender = Clock::Ms();
    Elements all;
    all.insert(all.end(), drag.begin(), drag.end());
    all.insert(all.end(), flash.begin(), flash.end());
    all.insert(all.end(), pick.begin(), pick.end());
    for (auto& s : surfaces) {
        try { if (all.empty()) s->Clear(); else s->Render(all); }
        catch (const std::exception& ex) { Log::Error(L"overlay render: " + Util::Widen(ex.what())); }
    }
}

} // namespace glimpse
