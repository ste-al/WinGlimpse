// Glimpse native build: the settings window, one page of custom-painted controls.
// Every control is a small child window (focus, tab order, mouse capture come from Windows); text boxes are
// real EDIT controls. The layout is the same as in the C# build.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "resource.h"
#include <climits>

namespace glimpse {

using namespace Gdiplus;

namespace {

const wchar_t* kPageClass = L"GlimpseSettings";
const wchar_t* kCtlClass = L"GlimpseSettingsControl";
const int kWidth100 = 740;

struct Page;

// ------------------------------------------------------------------ control base

struct Ctl {
    HWND hwnd = nullptr;
    Page* page = nullptr;
    float K = 1;
    Color back = Color(0, 0, 0, 0);       // unset: the card colour
    int x = 0, y = 0, w = 0, h = 0;      // page coordinates (not scrolled)
    bool hot = false, tracking = false, pressed = false;
    bool tabStop = false, hand = false;
    wstr tip;
    virtual ~Ctl() {}
    virtual void Paint(Graphics& g) {}
    virtual void PaintGdi(HDC dc, bool snapshot) {}
    virtual void OnClick() {}
    virtual void OnMouseDown(int, int) {}
    virtual void OnMouseUp(int, int) {}
    virtual void OnMouseMove(int, int) {}
    virtual bool OnKey(UINT) { return false; }
    virtual UINT DlgCode(const MSG*) { return 0; }
    virtual LRESULT Extra(UINT, WPARAM, LPARAM, bool& handled) { handled = false; return 0; }
    bool Enabled() const { return hwnd && IsWindowEnabled(hwnd); }
    bool Focused() const { return hwnd && GetFocus() == hwnd; }
    bool FocusCues() const { return hwnd && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS); }
    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    void FocusRing(Graphics& g, const RectF& r, float radius) {
        if (Focused() && FocusCues()) Draw::Stroke(g, RectF(r.X + 1, r.Y + 1, r.Width - 2, r.Height - 2), radius, Theme::Accent(), 1.5f * K, true);
    }
    void Fill(Graphics& g) {
        SolidBrush b(back);
        g.FillRectangle(&b, 0, 0, w, h);
    }
};

// ------------------------------------------------------------------ controls (as in the C# build)

struct Label : Ctl {
    wstr text;
    HFONT font = nullptr;
    Color ink;
    bool wrap = false;
    void Paint(Graphics& g) override { Fill(g); }
    void PaintGdi(HDC dc, bool) override {
        RECT r = { 0, 0, w, h };
        UINT flags = wrap ? DT_WORDBREAK : (DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
        Draw::TextGdi(dc, text, font, Theme::Ref(Enabled() ? ink : Theme::Sub()), r, flags);
    }
};

struct Toggle : Ctl {
    bool on = false;
    std::function<void()> changed;
    Toggle() { tabStop = true; hand = true; }
    void OnClick() override {
        if (!Enabled()) return;
        on = !on;
        SetFocus(hwnd);
        Invalidate();
        if (changed) changed();
    }
    bool OnKey(UINT vk) override { if (vk == VK_SPACE) { OnClick(); return true; } return false; }
    void Paint(Graphics& g) override {
        Fill(g);
        float hh = 26 * K, ww = 50 * K;
        RectF r(w - ww - 2, (h - hh) / 2, ww, hh);
        Draw::Fill(g, r, hh / 2, !Enabled() ? Theme::Off() : on ? (hot ? Theme::AccentHover() : Theme::Accent()) : Theme::Off());
        float d = hh - 6 * K;
        float xx = on ? r.GetRight() - d - 3 * K : r.X + 3 * K;
        Draw::Fill(g, RectF(xx, r.Y + 3 * K, d, d), d / 2, Color(255, 255, 255));
        FocusRing(g, RectF(r.X - 3 * K, r.Y - 3 * K, r.Width + 6 * K, r.Height + 6 * K), hh / 2 + 3 * K);
    }
};

struct Segmented : Ctl {
    std::vector<wstr> items;
    int selected = 0;
    std::function<void()> changed;
    Segmented() { tabStop = true; hand = true; }
    int PreferredWidth() const {
        float ww = 0;
        for (const wstr& s : items) ww += Draw::Measure(s, Draw::Font(15 * K, true)).Width + 44 * K;
        return (int)ww + 2;
    }
    std::vector<float> Edges() const {
        std::vector<float> e(items.size() + 1), widths(items.size());
        float total = 0;
        for (size_t i = 0; i < items.size(); i++) { widths[i] = Draw::Measure(items[i], Draw::Font(15 * K, true)).Width + 44 * K; total += widths[i]; }
        float scale = total > 0 ? (w - 2) / total : 1;
        e[0] = 1;
        for (size_t i = 0; i < items.size(); i++) e[i + 1] = e[i] + widths[i] * scale;
        return e;
    }
    void Paint(Graphics& g) override {
        Fill(g);
        RectF outer(1, 1, (REAL)w - 3, (REAL)h - 3);
        Draw::Fill(g, outer, 8 * K, Theme::Card());
        std::vector<float> edges = Edges();
        for (size_t i = 0; i < items.size(); i++) {
            RectF r(edges[i], 1, edges[i + 1] - edges[i], (REAL)h - 3);
            bool sel = (int)i == selected;
            if (sel) {
                auto clip = Draw::Round(outer, 8 * K);
                Region old;
                g.GetClip(&old);
                g.SetClip(clip.get(), CombineModeIntersect);
                SolidBrush b(Enabled() ? Theme::Accent() : Theme::Off());
                g.FillRectangle(&b, r);
                g.SetClip(&old);
            } else if (i > 0 && (int)i - 1 != selected) {
                Pen pen(Theme::Border(), 1);
                g.DrawLine(&pen, r.X, r.Y + 1, r.X, r.GetBottom() - 1);
            }
            Gdiplus::Font* f = Draw::Font(15 * K, sel);
            SizeF sz = Draw::Measure(items[i], f);
            Draw::Text(g, items[i], f, sel ? Color(255, 255, 255) : Theme::Text(), r.X + (r.Width - sz.Width) / 2, r.Y + (r.Height - sz.Height) / 2);
        }
        Draw::Stroke(g, outer, 8 * K, Theme::Border(), 1, false);
        FocusRing(g, RectF(outer.X - 2, outer.Y - 2, outer.Width + 4, outer.Height + 4), 9 * K);
    }
    void OnMouseUp(int mx, int) override {
        if (!Enabled()) return;
        std::vector<float> edges = Edges();
        for (size_t i = 0; i < items.size(); i++) if (mx >= edges[i] && mx < edges[i + 1]) { Pick((int)i); break; }
    }
    bool OnKey(UINT vk) override {
        if (vk == VK_LEFT && selected > 0) { Pick(selected - 1); return true; }
        if (vk == VK_RIGHT && selected < (int)items.size() - 1) { Pick(selected + 1); return true; }
        return false;
    }
    UINT DlgCode(const MSG*) override { return DLGC_WANTARROWS; }
    void Pick(int i) {
        if (i == selected) return;
        selected = i;
        Invalidate();
        if (changed) changed();
    }
};

bool BadgeBelow(const wstr& title, const wstr& badge, int width, float k) {
    if (badge.empty()) return false;
    float bx = 18 * k + Draw::Measure(title, Draw::Font(16 * k, true)).Width + 8 * k;
    float bw = Draw::Measure(badge, Draw::Font(11.5f * k, true)).Width + 12 * k;
    return bx + bw > width - 46 * k;
}

struct ModeCard : Ctl {
    wstr title, badge, desc;
    bool checked = false;
    std::function<void()> picked;
    ModeCard() { tabStop = true; hand = true; }
    static int HeightFor(const wstr& title, const wstr& badge, const wstr& desc, int width, float k) {
        int top = (int)(46 * k) + (BadgeBelow(title, badge, width, k) ? (int)(28 * k) : 0);
        return top + Draw::MeasureGdi(desc, Draw::GdiFont(14 * k, false), width - (int)(36 * k), true).cy + (int)(16 * k);
    }
    int Top() const { return (int)(46 * K) + (BadgeBelow(title, badge, w, K) ? (int)(28 * K) : 0); }
    void Paint(Graphics& g) override {
        Fill(g);
        RectF r(1.5f * K, 1.5f * K, w - 3 * K, h - 3 * K);
        Draw::Fill(g, r, 10 * K, checked ? Theme::AccentSoft() : (hot ? Theme::Hover() : Theme::Card()));
        Draw::Stroke(g, r, 10 * K, checked ? Theme::Accent() : Theme::Border(), checked ? 2 * K : 1, false);
        Gdiplus::Font* ft = Draw::Font(16 * K, true);
        Draw::Text(g, title, ft, Theme::Text(), 18 * K, 16 * K);
        bool below = BadgeBelow(title, badge, w, K);
        if (!badge.empty()) {
            Gdiplus::Font* fb = Draw::Font(11.5f * K, true);
            float ftH = Draw::Height(ft);
            float bx = below ? 18 * K : 18 * K + Draw::Measure(title, ft).Width + 8 * K;
            float by = below ? 16 * K + ftH + 6 * K : 16 * K + (ftH - 22 * K) / 2;
            float bw = Draw::Measure(badge, fb).Width + 12 * K;
            RectF br(bx, by, bw, 22 * K);
            Draw::Fill(g, br, 11 * K, Theme::dark ? Color(70, 62, 140) : Color(255, 255, 255));
            Draw::Stroke(g, br, 11 * K, Theme::dark ? Color(120, 110, 220) : Color(205, 200, 250), 1, false);
            Draw::Text(g, badge, fb, Theme::AccentText(), br.X + 6 * K, br.Y + (br.Height - Draw::Height(fb)) / 2);
        }
        float d = 20 * K;
        RectF rr(w - d - 16 * K, 18 * K, d, d);
        {
            Pen pen(checked ? Theme::Accent() : Theme::Off(), 1.6f * K);
            g.DrawEllipse(&pen, rr);
        }
        if (checked) Draw::Fill(g, RectF(rr.X + 5 * K, rr.Y + 5 * K, rr.Width - 10 * K, rr.Height - 10 * K), d, Theme::Accent());
        FocusRing(g, r, 10 * K);
    }
    void PaintGdi(HDC dc, bool) override {
        int top = Top();
        RECT tr = { (int)(18 * K), top, w - (int)(18 * K), h - (int)(4 * K) };
        Draw::TextGdi(dc, desc, Draw::GdiFont(14 * K, false), Theme::Ref(Theme::Sub()), tr, DT_WORDBREAK);
    }
    void OnClick() override {
        if (!Enabled()) return;
        SetFocus(hwnd);
        if (picked) picked();
    }
    bool OnKey(UINT vk) override { if (vk == VK_SPACE || vk == VK_RETURN) { OnClick(); return true; } return false; }
    UINT DlgCode(const MSG* m) override { return m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN ? DLGC_WANTMESSAGE : 0; }
};

struct LinkText : Ctl {
    wstr label;
    bool bold = true;
    std::function<void()> clicked;
    LinkText() { tabStop = true; hand = true; }
    int PreferredWidth() const { return (int)Draw::Measure(label, Draw::Font(15 * K, bold)).Width + (int)(6 * K); }
    void Paint(Graphics& g) override {
        Fill(g);
        Gdiplus::Font* f = Draw::Font(15 * K, bold);
        SizeF sz = Draw::Measure(label, f);
        Color c = Enabled() ? Theme::AccentText() : Theme::Sub();
        Draw::Text(g, label, f, c, 2, (h - sz.Height) / 2);
        if ((hot || Focused()) && Enabled()) {
            Pen pen(c, 1);
            float yy = (h + sz.Height) / 2;
            g.DrawLine(&pen, 2.0f, yy, 2 + sz.Width, yy);
        }
    }
    void OnClick() override { if (Enabled() && clicked) clicked(); }
    bool OnKey(UINT vk) override { if (vk == VK_SPACE || vk == VK_RETURN) { OnClick(); return true; } return false; }
    UINT DlgCode(const MSG* m) override { return m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN ? DLGC_WANTMESSAGE : 0; }
};

struct KeyCaps : Ctl {
    std::vector<wstr> keys;
    int PreferredWidth() const {
        if (keys.empty()) return (int)Draw::Measure(Texts::T(L"settings.xray.none"), Draw::Font(15 * K, false)).Width + 4;
        float ww = 0;
        for (const wstr& s : keys) ww += Draw::Measure(s, Draw::Font(15 * K, true)).Width + 26 * K;
        return (int)(ww + (keys.size() - 1) * 24 * K) + 2;
    }
    void Paint(Graphics& g) override {
        Fill(g);
        Gdiplus::Font* f = Draw::Font(15 * K, true);
        float fh = Draw::Height(f);
        if (keys.empty()) {
            Draw::Text(g, Texts::T(L"settings.xray.none"), Draw::Font(15 * K, false), Theme::Sub(), 0, (h - fh) / 2);
            return;
        }
        float xx = 0, hh = 34 * K, yy = (h - hh) / 2;
        for (size_t i = 0; i < keys.size(); i++) {
            if (i > 0) {
                Gdiplus::Font* plus = Draw::Font(14 * K, false);
                Draw::Text(g, L"+", plus, Theme::Sub(), xx + 6 * K, (h - Draw::Height(plus)) / 2);
                xx += 24 * K;
            }
            float ww = Draw::Measure(keys[i], f).Width + 26 * K;
            RectF r(xx, yy, ww, hh);
            Draw::Fill(g, r, 7 * K, Theme::Input());
            Draw::Stroke(g, r, 7 * K, Theme::Border(), 1.2f * K, false);
            Pen pen(Theme::Border(), 2 * K);
            g.DrawLine(&pen, r.X + 4 * K, r.GetBottom() - 1, r.GetRight() - 4 * K, r.GetBottom() - 1);
            Draw::Text(g, keys[i], f, Theme::Text(), r.X + 13 * K, r.Y + (hh - fh) / 2);
            xx += ww;
        }
    }
};

struct CornerPicker : Ctl {
    Corner value = Corner::TopLeft;
    std::function<void()> changed;
    CornerPicker() { tabStop = true; hand = true; }
    RectF Box() const { return RectF(1, 1, (REAL)w - 3, (REAL)h - 3); }
    void Paint(Graphics& g) override {
        Fill(g);
        RectF b = Box();
        Draw::Fill(g, b, 6 * K, Theme::Input());
        Draw::Stroke(g, b, 6 * K, Theme::Border(), 1.2f * K, false);
        float s = 11 * K, m = 6 * K;
        const Corner corners[] = { Corner::TopLeft, Corner::TopRight, Corner::BottomLeft, Corner::BottomRight };
        for (Corner c : corners) {
            float cx = c == Corner::TopLeft || c == Corner::BottomLeft ? b.X + m : b.GetRight() - m - s;
            float cy = c == Corner::TopLeft || c == Corner::TopRight ? b.Y + m : b.GetBottom() - m - s;
            Draw::Fill(g, RectF(cx, cy, s, s), 2.5f * K, c == value && Enabled() ? Theme::Accent() : Theme::Off());
        }
        FocusRing(g, b, 6 * K);
    }
    void OnMouseUp(int mx, int my) override {
        if (!Enabled()) return;
        RectF b = Box();
        bool left = mx < b.X + b.Width / 2, top = my < b.Y + b.Height / 2;
        Corner c = top ? (left ? Corner::TopLeft : Corner::TopRight) : (left ? Corner::BottomLeft : Corner::BottomRight);
        value = c == value ? Corner::Off : c;
        Invalidate();
        if (changed) changed();
    }
    bool OnKey(UINT vk) override {
        // keyboard: Space cycles through the corners and Off
        if (vk != VK_SPACE || !Enabled()) return false;
        const Corner order[] = { Corner::TopLeft, Corner::TopRight, Corner::BottomRight, Corner::BottomLeft, Corner::Off };
        int i = 0;
        while (i < 5 && order[i] != value) i++;
        value = order[(i + 1) % 5];
        Invalidate();
        if (changed) changed();
        return true;
    }
};

struct Chip : Ctl {
    wstr label, badgeName;
    BitmapPtr icon;
    RectF xr;
    std::function<void()> removed;
    Chip() { tabStop = true; }
    int PreferredWidth() const { return (int)(12 * K + 22 * K + 8 * K + Draw::Measure(label, Draw::Font(15 * K, false)).Width + 10 * K + 18 * K + 10 * K); }
    void Paint(Graphics& g) override {
        Fill(g);
        RectF r(1, 1, (REAL)w - 3, (REAL)h - 3);
        Draw::Fill(g, r, r.Height / 2, Theme::Input());
        Draw::Stroke(g, r, r.Height / 2, Theme::Border(), 1.2f * K, false);
        Draw::AppBadge(g, RectF(12 * K, (h - 22 * K) / 2, 22 * K, 22 * K), badgeName, icon.get());
        Gdiplus::Font* f = Draw::Font(15 * K, false);
        Draw::Text(g, label, f, Theme::Text(), 42 * K, (h - Draw::Height(f)) / 2);
        xr = RectF(w - 30 * K, (h - 18 * K) / 2, 18 * K, 18 * K);
        Pen pen(hot ? Theme::Text() : Theme::Sub(), 1.5f * K);
        float a = 5 * K;
        g.DrawLine(&pen, xr.X + a, xr.Y + a, xr.GetRight() - a, xr.GetBottom() - a);
        g.DrawLine(&pen, xr.GetRight() - a, xr.Y + a, xr.X + a, xr.GetBottom() - a);
        FocusRing(g, r, r.Height / 2);
    }
    void OnMouseUp(int mx, int my) override { if (Enabled() && xr.Contains((REAL)mx, (REAL)my) && removed) removed(); }
    bool OnKey(UINT vk) override { if (Enabled() && (vk == VK_DELETE || vk == VK_BACK) && removed) { removed(); return true; } return false; }
};

// "Pick a window": press, drag the target cursor onto a window, release. Uses mouse capture, no hook.
struct TargetButton : Ctl {
    wstr label;
    bool picking = false;
    std::function<void(POINT)> hovering, pickedAt;
    TargetButton() { tabStop = true; hand = true; }
    int PreferredWidth() const { return (int)(16 * K + 22 * K + 10 * K + Draw::Measure(label, Draw::Font(15 * K, true)).Width + 18 * K); }
    void Paint(Graphics& g) override {
        Fill(g);
        RectF r(1, 1, (REAL)w - 3, (REAL)h - 3);
        Draw::Fill(g, r, r.Height / 2, picking || hot ? Theme::AccentSoft() : Theme::Card());
        Draw::Stroke(g, r, r.Height / 2, Theme::Accent(), 1.4f * K, true);
        Draw::Glyph(g, L"target", RectF(16 * K, (h - 22 * K) / 2, 22 * K, 22 * K), Theme::AccentText());
        Gdiplus::Font* f = Draw::Font(15 * K, true);
        Draw::Text(g, label, f, Theme::AccentText(), 48 * K, (h - Draw::Height(f)) / 2);
        FocusRing(g, r, r.Height / 2);
    }
    void OnMouseDown(int, int) override {
        if (!Enabled()) return;
        picking = true;
        SetCapture(hwnd);
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        Invalidate();
    }
    void OnMouseMove(int, int) override { if (picking && hovering) hovering(Native::CursorPos()); }
    void OnMouseUp(int, int) override {
        if (!picking) return;
        picking = false;
        ReleaseCapture();
        Invalidate();
        if (pickedAt) pickedAt(Native::CursorPos());
    }
};

struct PrimaryButton : Ctl {
    wstr label;
    std::function<void()> clicked;
    PrimaryButton() { tabStop = true; hand = true; }
    void Paint(Graphics& g) override {
        Fill(g);
        RectF r(1, 1, (REAL)w - 3, (REAL)h - 3);
        Draw::Fill(g, r, 8 * K, hot ? Theme::AccentHover() : Theme::Accent());
        Gdiplus::Font* f = Draw::Font(16 * K, true);
        SizeF sz = Draw::Measure(label, f);
        Draw::Text(g, label, f, Color(255, 255, 255), (w - sz.Width) / 2, (h - sz.Height) / 2);
        if (Focused() && FocusCues()) Draw::Stroke(g, RectF(r.X + 3 * K, r.Y + 3 * K, r.Width - 6 * K, r.Height - 6 * K), 6 * K, Color(255, 255, 255), 1.2f * K, true);
    }
    void OnClick() override { if (clicked) clicked(); }
    bool OnKey(UINT vk) override { if (vk == VK_SPACE || vk == VK_RETURN) { OnClick(); return true; } return false; }
    UINT DlgCode(const MSG* m) override { return m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN ? DLGC_WANTMESSAGE : 0; }
};

// a text box in a rounded frame; the frame turns red when the content is not valid
struct InputBox : Ctl {
    HWND edit = nullptr;
    HFONT font = nullptr;
    bool invalid = false;
    HBRUSH brush = nullptr;
    std::function<void()> textChanged;
    std::function<bool(UINT)> key;      // Enter, Escape
    ~InputBox() override { if (brush) DeleteObject(brush); }
    wstr Text() const { return edit ? Native::WindowText(edit) : wstr(); }
    void SetInvalid(bool v) { if (invalid != v) { invalid = v; Invalidate(); } }
    void Paint(Graphics& g) override {
        Fill(g);
        RectF r(1, 1, (REAL)w - 3, (REAL)h - 3);
        bool focused = edit && GetFocus() == edit;
        Draw::Fill(g, r, 7 * K, Theme::Input());
        Draw::Stroke(g, r, 7 * K, invalid ? Theme::Red() : (focused ? Theme::Accent() : Theme::Border()), invalid || focused ? 1.6f * K : 1.2f * K, false);
    }
    void PaintGdi(HDC dc, bool snapshot) override {
        if (!snapshot || !edit) return;
        RECT er;
        GetWindowRect(edit, &er);
        MapWindowPoints(nullptr, hwnd, (POINT*)&er, 2);
        HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, Theme::Ref(Theme::Text()));
        SetBkMode(dc, TRANSPARENT);
        wstr t = Text();
        DrawTextW(dc, t.c_str(), (int)t.size(), &er, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT);
        SelectObject(dc, old);
    }
    LRESULT Extra(UINT m, WPARAM wp, LPARAM lp, bool& handled) override {
        handled = false;
        if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORSTATIC) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, Theme::Ref(Theme::Text()));
            SetBkColor(dc, Theme::Ref(Theme::Input()));
            if (!brush) brush = CreateSolidBrush(Theme::Ref(Theme::Input()));
            handled = true;
            return (LRESULT)brush;
        }
        if (m == WM_COMMAND && (HWND)lp == edit) {
            if (HIWORD(wp) == EN_CHANGE && textChanged) textChanged();
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) Invalidate();
            handled = true;
            return 0;
        }
        return 0;
    }
};

// the picture next to the glimpse size: a back window peeking out from behind a front one
struct SizeIllustration : Ctl {
    int px = 50;
    void Paint(Graphics& g) override {
        Fill(g);
        float ww = (REAL)w - 2, hh = (REAL)h - 2;
        float strip = std::max(6 * K, std::min(hh * 0.32f, px / 72.0f * hh * 0.32f));
        RectF backR(1, 1, ww * 0.62f, hh * 0.72f);
        Draw::Fill(g, backR, 6 * K, Theme::Card());
        Draw::Stroke(g, backR, 6 * K, Theme::Border(), 1.2f * K, false);
        RectF title(backR.X + 4 * K, backR.Y + 4 * K, backR.Width * 0.4f, strip * 0.6f);
        Draw::Fill(g, title, 4 * K, Theme::AccentSoft());
        Draw::Stroke(g, title, 4 * K, Theme::Accent(), 1.4f * K, false);
        RectF glimpse(backR.X + 4 * K, backR.GetBottom() - strip - 4 * K, strip * 1.3f, strip);
        Draw::Fill(g, glimpse, 4 * K, Theme::AccentSoft());
        Draw::Stroke(g, glimpse, 4 * K, Theme::Accent(), 1.4f * K, false);
        RectF front(ww * 0.3f, hh * 0.22f, ww * 0.7f, hh * 0.78f);
        Draw::Fill(g, front, 6 * K, Theme::Card());
        Draw::Stroke(g, front, 6 * K, Theme::Border(), 1.2f * K, false);
        SolidBrush b(Theme::Hover());
        g.FillRectangle(&b, front.X + 1, front.Y + 1, front.Width - 2, 10 * K);
    }
};

// a closed drop-down list; opens an owner-drawn menu
struct DropButton : Ctl {
    std::vector<wstr> items;
    int selected = 0;
    std::function<void(int)> picked;
    DropButton() { tabStop = true; hand = true; }
    void Paint(Graphics& g) override {
        Fill(g);
        RectF r(0.5f, 0.5f, (REAL)w - 1.5f, (REAL)h - 1.5f);
        Draw::Fill(g, r, 4 * K, Theme::Input());
        Draw::Stroke(g, r, 4 * K, Focused() ? Theme::Accent() : Theme::Border(), 1.2f * K, false);
        Draw::Glyph(g, L"chevdown", RectF(w - 26 * K, (h - 16 * K) / 2, 16 * K, 16 * K), Theme::Sub());
        FocusRing(g, r, 4 * K);
    }
    void PaintGdi(HDC dc, bool) override {
        if (selected < 0 || selected >= (int)items.size()) return;
        RECT r = { (int)(6 * K), 0, w - (int)(30 * K), h };
        Draw::TextGdi(dc, items[selected], Draw::GdiFont(15 * K, false), Theme::Ref(Enabled() ? Theme::Text() : Theme::Sub()), r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    void OnClick() override;
    bool OnKey(UINT vk) override { if (vk == VK_SPACE || vk == VK_RETURN || vk == VK_DOWN || vk == VK_F4) { OnClick(); return true; } return false; }
    UINT DlgCode(const MSG* m) override {
        if (m && m->message == WM_KEYDOWN && (m->wParam == VK_RETURN || m->wParam == VK_DOWN)) return DLGC_WANTMESSAGE;
        return 0;
    }
};

// ------------------------------------------------------------------ the page

std::set<const void*>& AlivePages() { static std::set<const void*> s; return s; }
bool Alive(const void* p) { return AlivePages().count(p) != 0; }

struct Page {
    GlimpseApp* app = nullptr;          // null in preview images
    Settings edit;
    float k = 1;
    HWND hwnd = nullptr, tips = nullptr;
    std::vector<std::unique_ptr<Ctl>> controls, graveyard;
    std::vector<RectF> cards;
    wstr hkStatus;
    Color hkStatusColor;
    bool recording = false, addingApp = false;
    Timer testTimer, saveTimer;
    int M = 0, CW = 0, P = 0, X0 = 0, IW = 0, cardTop = 0;
    int contentH = 0, clientH = 0, scrollY = 0;
    InputBox* focusAfterBuild = nullptr;
    bool closing = false, building = false;
    SettingsWindow* owner = nullptr;

    Page() : testTimer(10000), saveTimer(600) { AlivePages().insert(this); }
    ~Page() { AlivePages().erase(this); }

    void MoveCtl(Ctl* c) {
        if (c->hwnd) SetWindowPos(c->hwnd, nullptr, c->x, c->y - scrollY, c->w, c->h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    int S(float v) const { return Util::RoundEven(v * k); }
    bool ReadOnly() const { return app && (!app->store.writable || app->store.usingDefaults); }
    static wstr T(const wchar_t* key) { return Texts::T(key); }

    // ---- controls

    template <class C>
    C* Add(C* c, int x, int y, int w, int h) {
        c->x = x; c->y = y; c->w = w; c->h = h;
        c->K = k;
        c->page = this;
        if (c->back.GetValue() == 0) c->back = Theme::Card();
        controls.push_back(std::unique_ptr<Ctl>(c));
        c->hwnd = CreateWindowExW(0, kCtlClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | (c->tabStop ? WS_TABSTOP : 0),
                                  x, y - scrollY, w, h, hwnd, nullptr, Ui::Instance(), c);
        return c;
    }

    Label* MakeLabel(const wstr& text, float px, bool bold, Color ink, int x, int y, int w, bool wrap, Color back) {
        auto* l = new Label();
        l->text = text;
        l->font = Draw::GdiFont(px * k, bold);
        l->ink = ink;
        l->wrap = wrap;
        l->back = back;
        SIZE sz = Draw::MeasureGdi(text, l->font, w, wrap);
        return Add(l, x, y, wrap ? w : std::min(w, (int)sz.cx + S(4)), sz.cy);
    }

    InputBox* MakeInput(HFONT font, int x, int y, int w, int h, const wstr& text) {
        auto* b = new InputBox();
        b->font = font;
        Add(b, x, y, w, h);
        int padX = S(10);
        HDC dc = GetDC(b->hwnd);
        HGDIOBJ old = SelectObject(dc, font);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(b->hwnd, dc);
        int eh = tm.tmHeight + 2;
        b->edit = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  padX, std::max(1, (h - eh) / 2), std::max(10, w - 2 * padX), eh, b->hwnd, nullptr, Ui::Instance(), nullptr);
        SendMessageW(b->edit, WM_SETFONT, (WPARAM)font, FALSE);
        SendMessageW(b->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SetWindowSubclass(b->edit, EditProc, 1, (DWORD_PTR)b);
        return b;
    }

    static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto* box = (InputBox*)data;
        if (m == WM_GETDLGCODE && lp) {
            const MSG* msg = (const MSG*)lp;
            if (msg->message == WM_KEYDOWN && (msg->wParam == VK_RETURN || msg->wParam == VK_ESCAPE) && box->key)
                return DefSubclassProc(h, m, wp, lp) | DLGC_WANTMESSAGE;
        }
        if (m == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE) && box->key) {
            if (box->key((UINT)wp)) return 0;
        }
        if (m == WM_CHAR && (wp == L'\r' || wp == 27) && box->key) return 0;
        if (m == WM_NCDESTROY) RemoveWindowSubclass(h, EditProc, 1);
        return DefSubclassProc(h, m, wp, lp);
    }

    void Tip(HWND target, const wstr& text) {
        if (!tips || !target) return;
        TOOLINFOW ti = {};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd;
        ti.uId = (UINT_PTR)target;
        wstr t = text;
        ti.lpszText = &t[0];
        if (!SendMessageW(tips, TTM_GETTOOLINFOW, 0, (LPARAM)&ti)) {
            ti.lpszText = &t[0];
            SendMessageW(tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        } else {
            ti.lpszText = &t[0];
            SendMessageW(tips, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        }
    }

    void BeginCard(int& y) { cardTop = y; y += S(20); }

    void EndCard(int& y) {
        y += S(20);
        cards.push_back(RectF((REAL)M, (REAL)cardTop, (REAL)CW, (REAL)(y - cardTop)));
        y += S(12);
    }

    // ---- layout (the same page as the C# build)

    void Build();
    int ToggleRow(int y, const wstr& text, bool value, std::function<void(bool)> set);
    void ApplySize();
    void ScrollTo(int pos);

    wstr CornerText(Corner c) const {
        if (c == Corner::Off) return T(L"settings.xray.cornerOff");
        const wchar_t* name = c == Corner::TopLeft ? L"settings.corner.topLeft" : c == Corner::TopRight ? L"settings.corner.topRight"
                            : c == Corner::BottomLeft ? L"settings.corner.bottomLeft" : L"settings.corner.bottomRight";
        return Texts::F(L"settings.xray.cornerDesc", { { L"corner", T(name) } });
    }

    static wstr AutoLanguageName(const std::vector<std::pair<wstr, wstr>>& langs) {
        wstr ui = Texts::AutoLanguageCode();
        size_t dash = ui.find(L'-');
        wstr two = dash == wstr::npos ? ui : ui.substr(0, dash);
        for (const auto& kv : langs) if (Util::IEquals(kv.first, ui) || Util::IEquals(kv.first, two)) return kv.second;
        return L"English";
    }

    wstr FooterText() const {
        if (!app) return Texts::F(L"settings.saved", { { L"program", L"Glimpse.exe" } });
        if (app->store.usingDefaults) return T(L"settings.defaults");
        if (!app->store.writable) return Texts::F(L"settings.readOnly", { { L"path", app->store.filePath } });
        if (app->store.IsNextToProgram()) return Texts::F(L"settings.saved", { { L"program", Util::FileName(Program::launchPath) } });
        return Texts::F(L"settings.savedPath", { { L"path", app->store.filePath } });
    }

    BitmapPtr IconForProcess(const wstr& name) {
        if (!app) return BitmapPtr();
        for (auto& kv : app->reg.all) {
            TrackedWindow& w = *kv.second;
            if (Util::IEquals(w.process, name) || Util::IEquals(w.process, name + L".exe")) {
                WindowRegistry::LoadIcon(w, S(22));
                return w.icon;
            }
        }
        return BitmapPtr();
    }

    void ScheduleSave() { saveTimer.Start(); }

    void SaveNow() {
        saveTimer.Stop();
        if (!app) return;
        if (!app->SaveSettings(edit)) Rebuild();
    }

    void Rebuild() { Ui::Post([this]() { if (Alive(this) && !closing) Build(); }); }

    // ---- exclusions: pick a window or type an app name

    WinPtr WindowAt(POINT p) {
        if (!app) return WinPtr();
        HWND h = GetAncestor(WindowFromPoint(p), GA_ROOT);
        if (h == hwnd) return WinPtr();
        WinPtr w = app->reg.Get(h);
        if (!w) {
            app->reg.Refresh(app->S(), app->mons);
            w = app->reg.Get(h);
        }
        return w;
    }

    void HoverPick(POINT p) {
        if (!app) return;
        WinPtr w = WindowAt(p);
        if (w) app->overlays->ShowPick(w->frame); else app->overlays->HidePick();
    }

    void Pick(POINT p) {
        if (app) app->overlays->HidePick();
        WinPtr w = WindowAt(p);
        if (!w || w->process.empty()) return;
        AddApp(w->process);
    }

    void AddApp(wstr name) {
        name = Util::Trim(name);
        while (!name.empty() && name.front() == L'"') name.erase(name.begin());
        while (!name.empty() && name.back() == L'"') name.pop_back();
        addingApp = false;
        if (!name.empty()) {
            bool have = false;
            wstr exe = Util::EndsWithI(name, L".exe") ? name : name + L".exe";
            for (const Exclusion& e : edit.exclusions) if (e.isProcess && e.Matches(exe, L"")) have = true;
            if (!have) edit.exclusions.push_back(Exclusion::Process(name));
            SaveNow();
        }
        Rebuild();
    }

    // ---- hotkey: offered list, recording, the three checks

    void ShowHotkeyMenu(Ctl* anchor) {
        std::vector<MenuItem> items;
        std::vector<HotkeySpec> specs;
        int id = 1;
        for (const wstr& o : HotkeySpec::Offered()) {
            HotkeySpec s;
            HotkeySpec::TryParse(o, s);
            MenuItem m;
            m.id = id++;
            m.text = s.DisplayText();
            m.checked = Util::IEquals(o, edit.hotkey);
            items.push_back(m);
            specs.push_back(s);
        }
        MenuItem sep;
        sep.separator = true;
        items.push_back(sep);
        MenuItem rec;
        rec.id = 100;
        rec.text = T(L"settings.xray.record");
        items.push_back(rec);
        MenuItem none;
        none.id = 101;
        none.text = T(L"settings.xray.noHotkey");
        none.checked = edit.hotkey.empty();
        items.push_back(none);
        RECT r;
        GetWindowRect(anchor->hwnd, &r);
        int chosen = PopupMenu::Show(hwnd, items, r.left, r.bottom, k, TPM_LEFTALIGN | TPM_TOPALIGN);
        if (chosen >= 1 && chosen <= (int)specs.size()) TryHotkey(specs[chosen - 1]);
        else if (chosen == 100) StartRecording();
        else if (chosen == 101) { edit.hotkey.clear(); hkStatus.clear(); SaveNow(); Rebuild(); }
    }

    void StartRecording() {
        if (!app) return;
        recording = true;
        app->hotkeys->Unregister();
        hkStatus = T(L"settings.xray.recordNow");
        hkStatusColor = Theme::AccentText();
        Build();
        Ui::RemoveDialog(hwnd);   // every key goes to the page while recording
        SetFocus(hwnd);
    }

    void StopRecording(const HotkeySpec* spec) {
        recording = false;
        Ui::RemoveDialog(hwnd);
        Ui::AddDialog(hwnd);
        if (!spec || !spec->IsValid()) {
            hkStatus.clear();
            if (app) app->hotkeys->Apply(edit.hotkey);
            Rebuild();
            return;
        }
        TryHotkey(*spec);
    }

    // check 1: Windows accepts it; check 2: it types no character on any installed layout;
    // then check 3, "Press it now", confirms it reaches this session through the remote client
    void TryHotkey(const HotkeySpec& spec) {
        if (!app) return;
        wstr shown = spec.DisplayText();
        app->hotkeys->Unregister();
        bool ok = app->hotkeys->CanRegister(spec);
        wstr ch, layout;
        bool types = ok && spec.TypesCharacter(ch, layout);
        if (!ok) {
            hkStatus = Texts::F(L"settings.xray.taken", { { L"hotkey", shown } });
            hkStatusColor = Theme::Red();
            app->hotkeys->Apply(edit.hotkey);
            Rebuild();
            return;
        }
        if (types) {
            hkStatus = Texts::F(L"settings.xray.types", { { L"hotkey", shown }, { L"char", ch }, { L"layout", layout } });
            hkStatusColor = Theme::Red();
            app->hotkeys->Apply(edit.hotkey);
            Rebuild();
            return;
        }
        edit.hotkey = spec.ToString();
        SaveNow();
        StartTest();
    }

    void StartTest() {
        if (!app || edit.hotkey.empty()) return;
        HotkeySpec spec;
        if (!HotkeySpec::TryParse(edit.hotkey, spec)) return;
        if (!app->hotkeys->registered) app->hotkeys->Apply(edit.hotkey);
        hkStatus = Texts::F(L"settings.xray.pressNow", { { L"hotkey", spec.DisplayText() } });
        hkStatusColor = Theme::AccentText();
        wstr shown = spec.DisplayText();
        app->hotkeys->testArrived = [this, shown]() {
            testTimer.Stop();
            hkStatus = L"\u2713 " + Texts::F(L"settings.xray.arrived", { { L"hotkey", shown } });
            hkStatusColor = Theme::Green();
            if (!closing) Rebuild();
        };
        testTimer.Start();
        Rebuild();
    }

    void TestTimeout() {
        testTimer.Stop();
        if (!app) return;
        app->hotkeys->testArrived = nullptr;
        HotkeySpec spec;
        if (!HotkeySpec::TryParse(edit.hotkey, spec)) return;
        // suggest the next offered combination
        const std::vector<wstr>& offered = HotkeySpec::Offered();
        int i = -1;
        for (int j = 0; j < (int)offered.size(); j++) if (Util::IEquals(offered[j], edit.hotkey)) i = j;
        wstr next = offered[(i + 1 + offered.size()) % offered.size()];
        HotkeySpec ns;
        HotkeySpec::TryParse(next, ns);
        hkStatus = Texts::F(L"settings.xray.silent", { { L"hotkey", spec.DisplayText() }, { L"next", ns.DisplayText() } });
        hkStatusColor = Theme::Red();
        Rebuild();
    }

    void OnKeyWhileRecording(UINT vk) {
        if (vk == VK_ESCAPE) { StopRecording(nullptr); return; }
        if (HotkeySpec::IsModifierVk(vk) || vk == VK_LWIN || vk == VK_RWIN) return;
        HotkeySpec spec = HotkeySpec::FromKey(vk, Native::KeyDown(VK_CONTROL), Native::KeyDown(VK_SHIFT), Native::KeyDown(VK_MENU));
        StopRecording(&spec);
    }

    void Closed() {
        if (closing) return;
        closing = true;
        if (saveTimer.Enabled()) SaveNow();
        testTimer.Stop();
        if (app) {
            app->hotkeys->testArrived = nullptr;
            if (recording || !app->hotkeys->registered) app->hotkeys->Apply(app->S().hotkey);
            app->overlays->HidePick();
        }
    }

    void PaintBackground(Graphics& g, int offsetY) {
        SolidBrush b(Theme::Page());
        g.FillRectangle(&b, 0, 0, 100000, 100000);
        g.TranslateTransform(0, (REAL)-offsetY);
        for (const RectF& r : cards) {
            Draw::Fill(g, r, 12 * k, Theme::Card());
            Draw::Stroke(g, r, 12 * k, Theme::Border(), 1, false);
        }
        g.ResetTransform();
    }
};

void DropButton::OnClick() {
    if (!Enabled() || items.empty()) return;
    SetFocus(hwnd);
    std::vector<MenuItem> menu;
    for (size_t i = 0; i < items.size(); i++) {
        MenuItem m;
        m.id = (int)i + 1;
        m.text = items[i];
        m.checked = (int)i == selected;
        menu.push_back(m);
    }
    RECT r;
    GetWindowRect(hwnd, &r);
    int chosen = PopupMenu::Show(page->hwnd, menu, r.left, r.bottom, K, TPM_LEFTALIGN | TPM_TOPALIGN);
    if (chosen >= 1 && chosen - 1 != selected) {
        selected = chosen - 1;
        Invalidate();
        if (picked) picked(selected);
    }
}

// ------------------------------------------------------------------ control window procedure

LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* c = (Ctl*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!c) return DefWindowProcW(h, m, wp, lp);
    bool handled = false;
    LRESULT extra = c->Extra(m, wp, lp, handled);
    if (handled) return extra;
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            int w = rc.right, hh = rc.bottom;
            if (w > 0 && hh > 0) {
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, w, hh);
                HGDIOBJ old = SelectObject(mem, bmp);
                {
                    Graphics g(mem);
                    g.SetSmoothingMode(SmoothingModeAntiAlias);
                    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
                    try { c->Paint(g); } catch (const std::exception& ex) { Log::Error(L"settings paint: " + Util::Widen(ex.what())); }
                }
                c->PaintGdi(mem, false);
                BitBlt(dc, 0, 0, w, hh, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!c->tracking) {
                TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                c->tracking = true;
                c->hot = true;
                c->Invalidate();
            }
            c->OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            c->tracking = false;
            c->hot = false;
            c->Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
            if (c->tabStop && c->Enabled()) SetFocus(h);
            c->pressed = true;
            c->OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (GetCapture() != h) SetCapture(h);
            return 0;
        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            bool click = c->pressed && x >= 0 && y >= 0 && x < c->w && y < c->h;
            c->pressed = false;
            if (GetCapture() == h) ReleaseCapture();
            c->OnMouseUp(x, y);
            if (click && IsWindow(h)) c->OnClick();
            return 0;
        }
        case WM_CAPTURECHANGED:
            c->pressed = false;
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                auto* tb = dynamic_cast<TargetButton*>(c);
                SetCursor(LoadCursorW(nullptr, tb && tb->picking ? IDC_CROSS : (c->hand && c->Enabled() ? IDC_HAND : IDC_ARROW)));
                return TRUE;
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
        case WM_UPDATEUISTATE:
            c->Invalidate();
            break;
        case WM_KEYDOWN:
            if (c->OnKey((UINT)wp)) return 0;
            break;
        case WM_GETDLGCODE:
            return c->DlgCode((const MSG*)lp);
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ------------------------------------------------------------------ layout

int Page::ToggleRow(int y, const wstr& text, bool value, std::function<void(bool)> set) {
    int rowH = S(34);
    auto* tg = new Toggle();
    tg->on = value;
    Add(tg, X0 + IW - S(54), y + (rowH - S(30)) / 2, S(54), S(30));
    Label* l = MakeLabel(text, 15.5f, false, Theme::Text(), X0, y, IW - S(70), true, Theme::Card());
    l->y = y + std::max(0, (rowH - l->h) / 2);
    MoveCtl(l);
    tg->changed = [this, tg, set]() { set(tg->on); SaveNow(); };
    return y + std::max(rowH, l->h) + S(6);
}

void Page::Build() {
    if (!hwnd || building) return;
    building = true;
    Theme::Refresh();
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    HWND focused = GetFocus();
    bool hadFocus = focused && (focused == hwnd || IsChild(hwnd, focused));
    for (auto& c : controls) {
        if (c->hwnd) { ShowWindow(c->hwnd, SW_HIDE); }
        graveyard.push_back(std::move(c));
    }
    controls.clear();
    // old controls go away a moment later: the one that started this build may still be in its message
    Ui::Post([this]() {
        if (!Alive(this)) return;
        for (auto& c : graveyard) if (c->hwnd) { SetWindowLongPtrW(c->hwnd, GWLP_USERDATA, 0); DestroyWindow(c->hwnd); }
        graveyard.clear();
    });
    if (tips) { DestroyWindow(tips); tips = nullptr; }
    tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, hwnd, nullptr, Ui::Instance(), nullptr);
    if (tips) SendMessageW(tips, TTM_SETMAXTIPWIDTH, 0, S(420));
    cards.clear();
    SetWindowTextW(hwnd, T(L"settings.title").c_str());
    Native::SetDarkTitleBar(hwnd, Theme::dark);
    Color card = Theme::Card();
    int oldScroll = scrollY;
    scrollY = 0;

    M = S(18); CW = S(kWidth100) - 2 * M; P = S(22); X0 = M + P; IW = CW - 2 * P;
    int y = S(16);

    // ---- glimpse size
    BeginCard(y);
    {
        int illW = S(150), illH = S(86), textW = IW - illW - S(20);
        Label* t = MakeLabel(T(L"settings.size.title"), 17, true, Theme::Text(), X0, y, textW, false, card);
        int yy = y + t->h + S(2);
        Label* sub = MakeLabel(T(L"settings.size.subtitle"), 14.5f, false, Theme::Sub(), X0, yy, textW, true, card);
        yy += sub->h + S(14);
        auto* seg = new Segmented();
        seg->K = k;
        const int sizes[] = { Settings::SmallSize, Settings::MediumSize, Settings::LargeSize };
        seg->items.push_back(Texts::F(L"settings.size.small", { { L"px", Util::Inv(sizes[0]) } }));
        seg->items.push_back(Texts::F(L"settings.size.medium", { { L"px", Util::Inv(sizes[1]) } }));
        seg->items.push_back(Texts::F(L"settings.size.large", { { L"px", Util::Inv(sizes[2]) } }));
        int sel = -1;
        for (int i = 0; i < 3; i++) if (sizes[i] == edit.glimpseSize) sel = i;
        if (sel < 0) { seg->items.push_back(Texts::F(L"settings.size.custom", { { L"px", Util::Inv(edit.glimpseSize) } })); sel = 3; }
        seg->selected = sel;
        auto* ill = new SizeIllustration();
        ill->px = edit.glimpseSize;
        Add(ill, X0 + IW - illW, y, illW, illH);
        Add(seg, X0, yy, std::min(textW + S(8), seg->PreferredWidth()), S(42));
        int custom = edit.glimpseSize;
        seg->changed = [this, seg, ill, custom]() {
            const int sz[] = { Settings::SmallSize, Settings::MediumSize, Settings::LargeSize };
            edit.glimpseSize = seg->selected < 3 ? sz[seg->selected] : custom;
            ill->px = edit.glimpseSize;
            ill->Invalidate();
            SaveNow();
        };
        y = std::max(yy + S(42), y + illH);
    }
    EndCard(y);

    // ---- mode
    BeginCard(y);
    {
        Label* t = MakeLabel(T(L"settings.mode.title"), 17, true, Theme::Text(), X0, y, IW, false, card);
        y += t->h + S(14);
        int gap = S(12), cw = (IW - 2 * gap) / 3;
        const FixMode modes[] = { FixMode::Guide, FixMode::Assist, FixMode::Strict };
        const wchar_t* names[] = { L"guide", L"assist", L"strict" };
        int ch = 0;
        for (int i = 0; i < 3; i++) {
            wstr n = names[i];
            ch = std::max(ch, ModeCard::HeightFor(Texts::T(L"settings.mode." + n), i == 1 ? T(L"settings.recommended") : wstr(), Texts::T(L"settings.mode." + n + L".desc"), cw, k));
        }
        auto cards3 = std::make_shared<std::vector<ModeCard*>>();
        for (int i = 0; i < 3; i++) {
            wstr n = names[i];
            auto* mc = new ModeCard();
            mc->title = Texts::T(L"settings.mode." + n);
            mc->desc = Texts::T(L"settings.mode." + n + L".desc");
            mc->badge = i == 1 ? T(L"settings.recommended") : wstr();
            mc->checked = edit.mode == modes[i];
            Add(mc, X0 + i * (cw + gap), y, cw, ch);
            cards3->push_back(mc);
            FixMode m = modes[i];
            mc->picked = [this, mc, m, cards3]() {
                edit.mode = m;
                for (ModeCard* c : *cards3) { c->checked = c == mc; c->Invalidate(); }
                SaveNow();
            };
        }
        y += ch;
    }
    EndCard(y);

    // ---- new windows
    BeginCard(y);
    {
        Label* t = MakeLabel(T(L"settings.new.title"), 17, true, Theme::Text(), X0, y, IW, false, card);
        y += t->h + S(10);
        y = ToggleRow(y, T(L"settings.new.place"), edit.placeNewWindows, [this](bool v) { edit.placeNewWindows = v; });
        y = ToggleRow(y + S(4), T(L"settings.new.remember"), edit.rememberSizes, [this](bool v) { edit.rememberSizes = v; });
        y -= S(6);
    }
    EndCard(y);

    // ---- minimum size rules
    BeginCard(y);
    {
        auto* add = new LinkText();
        add->label = T(L"settings.min.add");
        add->K = k;
        add->back = card;
        int aw = add->PreferredWidth();
        Label* t = MakeLabel(T(L"settings.min.title"), 17, true, Theme::Text(), X0, y, IW - aw - S(12), false, card);
        Add(add, X0 + IW - aw, y, aw, t->h);
        add->clicked = [this]() {
            MinSizeRule r;
            r.pattern = L"^";
            r.minWidth = 400;
            r.minHeight = 300;
            r.Compile();
            size_t at = edit.minSizes.size();
            if (at > 0 && edit.minSizes[at - 1].pattern == L".*") at--;
            edit.minSizes.insert(edit.minSizes.begin() + at, r);
            SaveNow();
            Rebuild();
        };
        y += t->h + S(2);
        Label* sub = MakeLabel(T(L"settings.min.subtitle"), 14.5f, false, Theme::Sub(), X0, y, IW, true, card);
        y += sub->h + S(12);
        HFONT mono = Draw::GdiFont(15 * k, false, true);
        HFONT normal = Draw::GdiFont(15 * k, false);
        int rowH = S(38);
        for (size_t i = 0; i < edit.minSizes.size(); i++) {
            MinSizeRule& rule = edit.minSizes[i];
            int x = X0;
            InputBox* pat = MakeInput(mono, x, y, S(210), rowH, rule.pattern);
            pat->SetInvalid(!rule.re.re);
            Tip(pat->hwnd, T(L"settings.min.pattern"));
            Tip(pat->edit, rule.re.error.empty() ? T(L"settings.min.pattern") : Texts::F(L"settings.min.invalid", { { L"error", rule.re.error } }));
            x += S(210) + S(14);
            InputBox* wBox = MakeInput(normal, x, y, S(68), rowH, Util::Inv(rule.minWidth));
            x += S(68);
            MakeLabel(L"\u00d7", 15, false, Theme::Sub(), x + S(4), y + (rowH - S(20)) / 2, S(22), false, card);
            x += S(26);
            InputBox* hBox = MakeInput(normal, x, y, S(68), rowH, Util::Inv(rule.minHeight));
            x += S(68);
            MakeLabel(L"px", 15, false, Theme::Sub(), x + S(6), y + (rowH - S(20)) / 2, S(34), false, card);
            x += S(40);
            auto* rm = new LinkText();
            rm->label = L"\u00d7";
            rm->K = k;
            rm->back = card;
            rm->bold = false;
            int rmW = S(26);
            InputBox* note = MakeInput(normal, x, y, X0 + IW - rmW - S(10) - x, rowH, rule.note);
            Tip(note->edit, T(L"settings.min.note"));
            Add(rm, X0 + IW - rmW, y, rmW, rowH);
            Tip(rm->hwnd, T(L"settings.min.remove"));
            size_t index = i;
            pat->textChanged = [this, pat, index]() {
                if (index >= edit.minSizes.size()) return;
                MinSizeRule& r = edit.minSizes[index];
                r.pattern = pat->Text();
                bool ok = r.Compile();
                pat->SetInvalid(!ok);
                Tip(pat->edit, ok ? T(L"settings.min.pattern") : Texts::F(L"settings.min.invalid", { { L"error", r.re.error } }));
                if (ok) ScheduleSave();
            };
            auto sizeChanged = [this, wBox, hBox, index]() {
                if (index >= edit.minSizes.size()) return;
                MinSizeRule& r = edit.minSizes[index];
                int wv = 0, hv = 0;
                bool okW = Util::TryParseInt(wBox->Text(), wv) && wv >= 0 && wv <= 10000;
                bool okH = Util::TryParseInt(hBox->Text(), hv) && hv >= 0 && hv <= 10000;
                wBox->SetInvalid(!okW);
                hBox->SetInvalid(!okH);
                if (okW) r.minWidth = wv;
                if (okH) r.minHeight = hv;
                if (okW && okH) ScheduleSave();
            };
            wBox->textChanged = sizeChanged;
            hBox->textChanged = sizeChanged;
            note->textChanged = [this, note, index]() {
                if (index >= edit.minSizes.size()) return;
                edit.minSizes[index].note = note->Text();
                ScheduleSave();
            };
            rm->clicked = [this, index]() {
                if (index < edit.minSizes.size()) edit.minSizes.erase(edit.minSizes.begin() + index);
                SaveNow();
                Rebuild();
            };
            if (rule.pattern == L"^" && !focusAfterBuild) focusAfterBuild = pat;
            y += rowH + S(8);
        }
        y -= S(8);
    }
    EndCard(y);

    // ---- X-ray
    BeginCard(y);
    {
        Label* t = MakeLabel(T(L"settings.xray.title"), 17, true, Theme::Text(), X0, y, IW, false, card);
        y += t->h + S(12);
        int rowH = S(40);
        MakeLabel(T(L"settings.xray.hotkey"), 15.5f, false, Theme::Text(), X0, y + (rowH - S(22)) / 2, S(200), false, card);
        auto* change = new LinkText();
        change->label = T(L"settings.xray.change");
        change->K = k;
        change->back = card;
        auto* test = new LinkText();
        test->label = T(L"settings.xray.test");
        test->K = k;
        test->back = card;
        auto* caps = new KeyCaps();
        caps->K = k;
        caps->back = card;
        HotkeySpec spec;
        if (!recording && HotkeySpec::TryParse(edit.hotkey, spec)) caps->keys = spec.DisplayParts();
        int cw = change->PreferredWidth(), tw = test->PreferredWidth(), kw = caps->PreferredWidth();
        int xr = X0 + IW;
        Add(change, xr - cw, y, cw, rowH);
        xr -= cw + S(18);
        Add(test, xr - tw, y, tw, rowH);
        xr -= tw + S(18);
        Add(caps, xr - kw, y, kw, rowH);
        EnableWindow(test->hwnd, !edit.hotkey.empty() && !recording);
        test->clicked = [this]() { StartTest(); };
        change->clicked = [this, change]() { ShowHotkeyMenu(change); };
        y += rowH;
        if (!hkStatus.empty()) {
            Label* st = MakeLabel(hkStatus, 14, false, hkStatusColor, X0, y + S(6), IW, true, card);
            y += st->h + S(6);
        }
        y += S(12);
        MakeLabel(T(L"settings.xray.corner"), 15.5f, false, Theme::Text(), X0, y + (rowH - S(22)) / 2, S(130), false, card);
        auto* picker = new CornerPicker();
        picker->value = edit.hotCorner;
        picker->back = card;
        Add(picker, X0 + S(132), y, S(56), rowH);
        Label* desc = MakeLabel(CornerText(edit.hotCorner), 15, false, Theme::Sub(), X0 + S(132) + S(56) + S(14), y + (rowH - S(22)) / 2, S(220), false, card);
        picker->changed = [this, picker, desc]() {
            edit.hotCorner = picker->value;
            desc->text = CornerText(edit.hotCorner);
            desc->w = Draw::MeasureGdi(desc->text, desc->font, S(220), false).cx + S(4);
            MoveCtl(desc);
            desc->Invalidate();
            SaveNow();
        };
        wstr smText = T(L"settings.xray.minimized");
        int smW = Draw::MeasureGdi(smText, Draw::GdiFont(15.5f * k, false), IW - S(70), false).cx;
        int leftEnd = X0 + S(132) + S(56) + S(14) + Draw::MeasureGdi(desc->text, desc->font, S(220), false).cx;
        if (leftEnd + S(24) > X0 + IW - S(54) - S(14) - smW) y += rowH + S(4);   // no room: its own line
        auto* showMin = new Toggle();
        showMin->on = edit.showMinimized;
        showMin->back = card;
        Add(showMin, X0 + IW - S(54), y + (rowH - S(30)) / 2, S(54), S(30));
        MakeLabel(smText, 15.5f, false, Theme::Text(), X0 + IW - S(54) - S(14) - smW, y + (rowH - S(22)) / 2, smW + S(4), false, card);
        showMin->changed = [this, showMin]() { edit.showMinimized = showMin->on; SaveNow(); };
        y += rowH;
    }
    EndCard(y);

    // ---- exclusions
    BeginCard(y);
    {
        int leftW = S(230);
        Label* t = MakeLabel(T(L"settings.exclude.title"), 17, true, Theme::Text(), X0, y, leftW, true, card);
        Label* sub = MakeLabel(T(L"settings.exclude.subtitle"), 14.5f, false, Theme::Sub(), X0, y + t->h + S(2), leftW, true, card);
        int leftBottom = y + t->h + S(2) + sub->h;
        int fx0 = X0 + leftW + S(16), fx1 = X0 + IW, cx = fx0, cy = y, chipH = S(40), gap = S(10);
        auto place = [&](Ctl* c, int w) {
            if (cx > fx0 && cx + w > fx1) { cx = fx0; cy += chipH + gap; }
            Add(c, cx, cy, std::min(w, fx1 - fx0), chipH);
            cx += w + gap;
        };
        for (size_t i = 0; i < edit.exclusions.size(); i++) {
            const Exclusion& ex = edit.exclusions[i];
            wstr label = ex.isProcess ? ex.value : FriendlyPattern(ex.value);
            auto* chip = new Chip();
            chip->K = k;
            chip->back = card;
            chip->label = label;
            chip->badgeName = label;
            if (ex.isProcess) chip->icon = IconForProcess(ex.value);
            size_t index = i;
            chip->removed = [this, index]() {
                if (index < edit.exclusions.size()) edit.exclusions.erase(edit.exclusions.begin() + index);
                SaveNow();
                Rebuild();
            };
            place(chip, chip->PreferredWidth());
            if (!ex.isProcess) Tip(chip->hwnd, Texts::F(L"settings.exclude.titleTip", { { L"pattern", ex.value } }));
            else Tip(chip->hwnd, T(L"settings.exclude.remove") + L" (Delete)");
        }
        auto* pick = new TargetButton();
        pick->K = k;
        pick->back = card;
        pick->label = T(L"settings.exclude.pick");
        pick->hovering = [this](POINT p) { HoverPick(p); };
        pick->pickedAt = [this](POINT p) { Pick(p); };
        place(pick, pick->PreferredWidth());
        Tip(pick->hwnd, T(L"settings.exclude.pickHint"));
        if (addingApp) {
            int bw = S(200);
            if (cx > fx0 && cx + bw > fx1) { cx = fx0; cy += chipH + gap; }
            InputBox* box = MakeInput(Draw::GdiFont(15 * k, false), cx, cy, std::min(bw, fx1 - fx0), chipH, L"");
            cx += bw + gap;
            Tip(box->edit, T(L"settings.exclude.prompt"));
            box->key = [this, box](UINT vk) {
                if (vk == VK_RETURN) { AddApp(box->Text()); return true; }
                if (vk == VK_ESCAPE) { addingApp = false; Rebuild(); return true; }
                return false;
            };
            focusAfterBuild = box;
        } else {
            auto* addLink = new LinkText();
            addLink->K = k;
            addLink->back = card;
            addLink->label = T(L"settings.exclude.add");
            addLink->clicked = [this]() { addingApp = true; Rebuild(); };
            place(addLink, addLink->PreferredWidth());
        }
        y = std::max(leftBottom, cy + chipH);
    }
    EndCard(y);

    // ---- start with Windows, language
    {
        Color page = Theme::Page();
        int rowH = S(40);
        auto* start = new Toggle();
        start->on = edit.startWithWindows;
        start->back = page;
        Add(start, M + S(2), y + (rowH - S(30)) / 2, S(54), S(30));
        start->changed = [this, start]() { edit.startWithWindows = start->on; SaveNow(); };
        MakeLabel(T(L"settings.start"), 15.5f, false, Theme::Text(), M + S(66), y + (rowH - S(22)) / 2, S(260), false, page);
        auto* combo = new DropButton();
        combo->back = page;
        std::vector<std::pair<wstr, wstr>> langs = Texts::Available();
        auto codes = std::make_shared<std::vector<wstr>>();
        combo->items.push_back(Texts::F(L"settings.language.auto", { { L"language", AutoLanguageName(langs) } }));
        codes->push_back(L"auto");
        for (const auto& kv : langs) { combo->items.push_back(kv.second); codes->push_back(kv.first); }
        int idx = 0;
        for (size_t i = 0; i < codes->size(); i++) if (Util::IEquals((*codes)[i], edit.language)) { idx = (int)i; break; }
        combo->selected = idx;
        int comboW = S(250), comboH = S(34);
        Add(combo, M + CW - comboW, y + (rowH - comboH) / 2, comboW, comboH);
        wstr lt = T(L"settings.language");
        int lw = Draw::MeasureGdi(lt, Draw::GdiFont(15.5f * k, false), S(200), false).cx;
        MakeLabel(lt, 15.5f, false, Theme::Text(), M + CW - comboW - S(14) - lw, y + (rowH - S(22)) / 2, lw + S(4), false, page);
        combo->picked = [this, codes](int i) {
            if (i < 0 || i >= (int)codes->size()) return;
            edit.language = (*codes)[i];
            SaveNow();
            if (!app) Texts::Load(edit.language);
            Rebuild();
        };
        y += rowH + S(16);
    }

    // ---- footer
    {
        Color page = Theme::Page();
        int btnW = S(120), btnH = S(44);
        auto* done = new PrimaryButton();
        done->K = k;
        done->back = page;
        done->label = T(L"settings.done");
        Add(done, M + CW - btnW, y, btnW, btnH);
        done->clicked = [this]() { if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0); };
        Label* f = MakeLabel(FooterText(), 14.5f, false, ReadOnly() ? Theme::Red() : Theme::Sub(), M + S(2), y, CW - btnW - S(20), true, page);
        f->y = y + std::max(0, (btnH - f->h) / 2);
        MoveCtl(f);
        y += std::max(btnH, f->h) + S(18);
        if (ReadOnly())
            for (auto& c : controls)
                if (c.get() != done && !dynamic_cast<Label*>(c.get())) {
                    EnableWindow(c->hwnd, FALSE);
                    if (auto* ib = dynamic_cast<InputBox*>(c.get())) EnableWindow(ib->edit, FALSE);
                }
    }

    contentH = y;
    ApplySize();
    ScrollTo(oldScroll);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
    if (focusAfterBuild) {
        InputBox* fb = focusAfterBuild;
        focusAfterBuild = nullptr;
        SetFocus(fb->edit);
        SendMessageW(fb->edit, EM_SETSEL, 0, -1);
    } else if (hadFocus && !recording) {
        // keep keyboard users in the page: focus the first control that takes it
        for (auto& c : controls) if (c->tabStop && IsWindowEnabled(c->hwnd)) { SetFocus(c->hwnd); break; }
    }
    building = false;
}

void Page::ApplySize() {
    int maxH = INT_MAX;
    if (app) {
        RECT wr;
        GetWindowRect(hwnd, &wr);
        const MonitorInfo* mon = Monitors::At(app->mons, Rc::From(wr));
        if (mon) maxH = mon->work.H() - S(70);
    }
    clientH = std::min(contentH, maxH);
    bool scroll = contentH > clientH;
    int dpi = (int)std::lround(k * 96);
    int sb = scroll ? GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) : 0;
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE) & ~WS_VSCROLL;
    RECT r = { 0, 0, S(kWidth100) + sb, clientH };
    AdjustWindowRectExForDpi(&r, style, FALSE, (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE), dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SCROLLINFO si = {};
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, contentH - 1);
    si.nPage = (UINT)clientH;
    si.nPos = scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    ShowScrollBar(hwnd, SB_VERT, scroll);
}

void Page::ScrollTo(int pos) {
    int maxPos = std::max(0, contentH - clientH);
    pos = std::max(0, std::min(maxPos, pos));
    SetScrollPos(hwnd, SB_VERT, pos, TRUE);
    if (pos == scrollY) return;
    scrollY = pos;
    // move every control (ScrollWindowEx would leave the ones outside the visible part where they are)
    HDWP d = BeginDeferWindowPos((int)controls.size());
    for (auto& c : controls)
        if (c->hwnd && d) d = DeferWindowPos(d, c->hwnd, nullptr, c->x, c->y - scrollY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (d) EndDeferWindowPos(d);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------ page window procedure

LRESULT CALLBACK PageProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    LRESULT r = 0;
    if (PopupMenu::OwnerDraw(m, wp, lp, r)) return r;
    auto* p = (Page*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!p) return DefWindowProcW(h, m, wp, lp);
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            if (rc.right > 0 && rc.bottom > 0) {
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                HGDIOBJ old = SelectObject(mem, bmp);
                {
                    Graphics g(mem);
                    g.SetSmoothingMode(SmoothingModeAntiAlias);
                    p->PaintBackground(g, p->scrollY);
                }
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si = { sizeof si, SIF_ALL };
            GetScrollInfo(h, SB_VERT, &si);
            int pos = p->scrollY, line = p->S(40);
            switch (LOWORD(wp)) {
                case SB_LINEUP: pos -= line; break;
                case SB_LINEDOWN: pos += line; break;
                case SB_PAGEUP: pos -= p->clientH; break;
                case SB_PAGEDOWN: pos += p->clientH; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = p->contentH; break;
            }
            p->ScrollTo(pos);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            p->ScrollTo(p->scrollY - delta * p->S(40) * 3 / WHEEL_DELTA / 2);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (p->recording) { p->OnKeyWhileRecording((UINT)wp); return 0; }
            break;
        case WM_SYSCHAR:
        case WM_CHAR:
            if (p->recording) return 0;
            break;
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL && lp == 0) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
            break;
        case WM_DPICHANGED: {
            p->k = LOWORD(wp) / 96.0f;
            const RECT* sug = (const RECT*)lp;
            SetWindowPos(h, nullptr, sug->left, sug->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            p->Build();
            return 0;
        }
        case WM_SETTINGCHANGE:
            p->Rebuild();
            break;
        case WM_CLOSE:
            p->Closed();
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            Ui::RemoveDialog(h);
            if (p->app) p->app->SettingsClosed();
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            p->hwnd = nullptr;
            break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void RegisterClasses() {
    static bool done = false;
    if (done) return;
    done = true;
    Ui::RegisterClass(kPageClass, PageProc);
    Ui::RegisterClass(kCtlClass, CtlProc);
}

} // namespace

// ================================================================== public face

// ^Calculator$ shows as Calculator; the exact pattern is in the tooltip
wstr FriendlyPattern(const wstr& p) {
    wstr s = p;
    if (!s.empty() && s[0] == L'^') s = s.substr(1);
    if (!s.empty() && s.back() == L'$' && !(s.size() >= 2 && s[s.size() - 2] == L'\\')) s.pop_back();
    wstr out;
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[i + 1];
            bool alnum = (n >= L'a' && n <= L'z') || (n >= L'A' && n <= L'Z') || (n >= L'0' && n <= L'9');
            if (!alnum) { out += n; i++; continue; }
        }
        out += c;
    }
    return out;
}

struct SettingsWindow::Impl {
    Page page;
};

SettingsWindow::SettingsWindow(GlimpseApp* app, const Settings& s, int dpi) : impl(new Impl()) {
    RegisterClasses();
    Page& p = impl->page;
    p.app = app;
    p.edit = s;
    p.owner = this;
    Theme::Refresh();
    p.testTimer.tick = [&p]() { p.TestTimeout(); };
    p.saveTimer.tick = [&p]() { p.saveTimer.Stop(); p.SaveNow(); };
    const MonitorInfo* mon = nullptr;
    if (app) { POINT c = Native::CursorPos(); mon = Monitors::AtPoint(app->mons, c.x, c.y); }
    p.k = (dpi > 0 ? dpi : (mon ? mon->dpi : 96)) / 96.0f;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    int x = mon ? mon->work.L : 0, y = mon ? mon->work.T : 0;
    hwnd = CreateWindowExW(WS_EX_CONTROLPARENT | (app ? 0 : WS_EX_TOOLWINDOW), kPageClass, L"Glimpse settings", style,
                           app ? x : -20000, app ? y : -20000, p.S(kWidth100), p.S(600), nullptr, nullptr, Ui::Instance(), &p);
    p.hwnd = hwnd;
    if (!hwnd) throw std::runtime_error("cannot create the settings window (error " + std::to_string(GetLastError()) + ")");
    HICON big = (HICON)LoadImageW(Ui::Instance(), MAKEINTRESOURCEW(IDI_GLIMPSE), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON smallIcon = (HICON)LoadImageW(Ui::Instance(), MAKEINTRESOURCEW(IDI_GLIMPSE), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (smallIcon) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    p.Build();
    if (mon) {
        RECT wr;
        GetWindowRect(hwnd, &wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        Rc work = mon->work;
        SetWindowPos(hwnd, nullptr, std::max(work.L, work.CX() - w / 2), std::max(work.T + p.S(12), work.CY() - h / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (app) Ui::AddDialog(hwnd);
}

SettingsWindow::~SettingsWindow() {
    Page& p = impl->page;
    p.Closed();
    p.testTimer.Stop();
    p.saveTimer.Stop();
    if (p.hwnd) {
        HWND h = p.hwnd;
        Ui::RemoveDialog(h);
        p.app = nullptr;   // closing from the app: do not report back
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
    for (auto& c : p.controls) if (c->hwnd) SetWindowLongPtrW(c->hwnd, GWLP_USERDATA, 0);
    for (auto& c : p.graveyard) if (c->hwnd) SetWindowLongPtrW(c->hwnd, GWLP_USERDATA, 0);
}

void SettingsWindow::Show() {
    if (hwnd) ShowWindow(hwnd, SW_SHOW);
}

void SettingsWindow::ReloadFromApp() {
    Page& p = impl->page;
    if (!p.app) return;
    p.edit = p.app->S().Clone();
    p.Build();
}

void SettingsWindow::Build() { impl->page.Build(); }

Gdiplus::Bitmap* SettingsWindow::Snapshot() {
    Page& p = impl->page;
    int w = p.S(kWidth100), h = std::max(1, p.contentH);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(dc); return nullptr; }
    HGDIOBJ old = SelectObject(dc, dib);
    {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        p.PaintBackground(g, 0);
    }
    for (auto& c : p.controls) {
        if (!c->hwnd || !IsWindowVisible(c->hwnd)) continue;
        {
            Graphics g(dc);
            g.TranslateTransform((REAL)c->x, (REAL)c->y);
            g.SetClip(RectF(0, 0, (REAL)c->w, (REAL)c->h));
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            c->Paint(g);
        }
        int saved = SaveDC(dc);
        SetViewportOrgEx(dc, c->x, c->y, nullptr);
        IntersectClipRect(dc, 0, 0, c->w, c->h);
        c->PaintGdi(dc, true);
        RestoreDC(dc, saved);
    }
    GdiFlush();
    // the page is opaque; GDI text leaves the alpha byte at zero
    uint32_t* px = (uint32_t*)bits;
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] |= 0xFF000000u;
    auto* out = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
    BitmapData data;
    Gdiplus::Rect all(0, 0, w, h);
    if (out->LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &data) == Ok) {
        for (int y = 0; y < h; y++) memcpy((BYTE*)data.Scan0 + (size_t)y * data.Stride, px + (size_t)y * w, (size_t)w * 4);
        out->UnlockBits(&data);
    }
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    return out;
}

} // namespace glimpse
