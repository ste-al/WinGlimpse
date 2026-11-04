// Glimpse native build: theme, fonts, drawing helpers (GDI+, the same calls as System.Drawing in
// the C# build) and owner-drawn popup menus.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"

namespace glimpse {

using namespace Gdiplus;

// ================================================================== theme

namespace Theme {
bool dark = false;
int forced = -1;

void Refresh() {
    if (forced >= 0) { dark = forced == 1; return; }
    DWORD v = 1, size = sizeof v;
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    dark = st == ERROR_SUCCESS && v == 0;
}

Color Accent() { return Color(91, 79, 224); }
Color AccentHover() { return Color(76, 64, 205); }
Color Green() { return Color(22, 163, 74); }
Color GreenSoft() { return Color(220, 252, 231); }
Color GreenLine() { return Color(34, 197, 94); }
Color Amber() { return Color(245, 158, 11); }
Color AmberSoft() { return Color(254, 243, 199); }
Color Red() { return Color(220, 38, 38); }
Color RedSoft() { return Color(254, 226, 226); }
Color GuideC() { return Color(124, 108, 242); }
Color Ink() { return Color(27, 27, 34); }
Color Chip() { return Color(30, 32, 40); }
Color Page() { return dark ? Color(32, 32, 38) : Color(244, 244, 247); }
Color Card() { return dark ? Color(43, 43, 50) : Color(255, 255, 255); }
Color Border() { return dark ? Color(62, 62, 72) : Color(226, 226, 233); }
Color Text() { return dark ? Color(242, 242, 245) : Color(27, 27, 34); }
Color Sub() { return dark ? Color(166, 166, 178) : Color(104, 104, 118); }
Color Hover() { return dark ? Color(55, 55, 64) : Color(240, 240, 246); }
Color Input() { return dark ? Color(32, 32, 38) : Color(246, 246, 249); }
Color AccentSoft() { return dark ? Color(49, 45, 92) : Color(237, 235, 255); }
Color AccentText() { return dark ? Color(168, 160, 255) : Accent(); }
Color Off() { return dark ? Color(90, 90, 102) : Color(203, 203, 212); }
COLORREF Ref(const Color& c) { return RGB(c.GetR(), c.GetG(), c.GetB()); }
}

// ================================================================== fonts and text

namespace Draw {

namespace {
std::map<wstr, Gdiplus::Font*>& Fonts() { static std::map<wstr, Gdiplus::Font*> f; return f; }
std::map<wstr, HFONT>& GdiFonts() { static std::map<wstr, HFONT> f; return f; }
Gdiplus::FontFamily* family = nullptr;
Gdiplus::FontFamily* semibold = nullptr;
Gdiplus::FontFamily* monoFamily = nullptr;
bool familiesDone = false;
wstr familyName = L"Segoe UI";
bool haveSemibold = false;
Gdiplus::Bitmap* measureBitmap = nullptr;
Gdiplus::Graphics* measurer = nullptr;
Gdiplus::StringFormat* typo = nullptr;

void Families() {
    if (familiesDone) return;
    familiesDone = true;
    family = new FontFamily(L"Segoe UI");
    if (family->GetLastStatus() != Ok || !family->IsAvailable()) {
        delete family;
        NONCLIENTMETRICSW ncm = {};
        ncm.cbSize = sizeof ncm;
        familyName = L"Tahoma";
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0) && ncm.lfMessageFont.lfFaceName[0]) familyName = ncm.lfMessageFont.lfFaceName;
        family = new FontFamily(familyName.c_str());
        if (family->GetLastStatus() != Ok || !family->IsAvailable()) {
            delete family;
            family = FontFamily::GenericSansSerif()->Clone();
            wchar_t name[LF_FACESIZE];
            family->GetFamilyName(name);
            familyName = name;
        }
    }
    semibold = new FontFamily(L"Segoe UI Semibold");
    haveSemibold = semibold->GetLastStatus() == Ok && semibold->IsAvailable();
    if (!haveSemibold) { delete semibold; semibold = nullptr; }
    monoFamily = new FontFamily(L"Consolas");
    if (monoFamily->GetLastStatus() != Ok || !monoFamily->IsAvailable()) {
        delete monoFamily;
        monoFamily = FontFamily::GenericMonospace()->Clone();
    }
}

wstr Key(float px, const wchar_t* tag) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f", (double)px);
    return wstr(buf) + tag;
}

float RoundPx(float px) {
    double v = std::max(6.0, (double)px) * 10.0;
    return (float)(std::nearbyint(v) / 10.0);
}
}

Gdiplus::Font* Font(float px, bool bold) {
    px = RoundPx(px);
    wstr key = Key(px, bold ? L"b" : L"");
    auto it = Fonts().find(key);
    if (it != Fonts().end()) return it->second;
    Families();
    Gdiplus::Font* f;
    if (bold && haveSemibold) f = new Gdiplus::Font(semibold, px, FontStyleRegular, UnitPixel);
    else f = new Gdiplus::Font(family, px, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    Fonts()[key] = f;
    return f;
}

Gdiplus::Font* Mono(float px) {
    wstr key = Key(px, L"mono");
    auto it = Fonts().find(key);
    if (it != Fonts().end()) return it->second;
    Families();
    auto* f = new Gdiplus::Font(monoFamily, px, FontStyleRegular, UnitPixel);
    Fonts()[key] = f;
    return f;
}

HFONT GdiFont(float px, bool bold, bool mono) {
    px = RoundPx(px);
    wstr key = Key(px, mono ? L"m" : (bold ? L"b" : L""));
    auto it = GdiFonts().find(key);
    if (it != GdiFonts().end()) return it->second;
    Families();
    wstr face = mono ? L"Consolas" : familyName;
    int weight = bold ? (haveSemibold ? FW_SEMIBOLD : FW_BOLD) : FW_NORMAL;
    HFONT f = CreateFontW(-(int)std::lround(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
    GdiFonts()[key] = f;
    return f;
}

float Height(Gdiplus::Font* f) {
    FontFamily ff;
    f->GetFamily(&ff);
    INT style = f->GetStyle();
    UINT16 em = ff.GetEmHeight(style), line = ff.GetLineSpacing(style);
    if (em == 0) return std::ceil(f->GetSize() * 1.2f);
    return std::ceil(f->GetSize() * line / em);
}

const StringFormat* Typo() {
    if (!typo) {
        typo = StringFormat::GenericTypographic()->Clone();
        typo->SetFormatFlags(typo->GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
        typo->SetTrimming(StringTrimmingEllipsisCharacter);
    }
    return typo;
}

SizeF Measure(const wstr& s, Gdiplus::Font* f) {
    if (s.empty()) return SizeF(0, Height(f));
    if (!measurer) {
        measureBitmap = new Bitmap(1, 1, PixelFormat32bppPARGB);
        measurer = new Graphics(measureBitmap);
        measurer->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    }
    RectF layout(0, 0, 2147483647.0f, 999999.0f), box;
    measurer->MeasureString(s.c_str(), (INT)s.size(), f, layout, Typo(), &box);
    return SizeF(box.Width, box.Height);
}

void Text(Graphics& g, const wstr& s, Gdiplus::Font* f, const Color& c, float x, float y) {
    if (s.empty()) return;
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, PointF(x, y), Typo(), &b);
}

void TextIn(Graphics& g, const wstr& s, Gdiplus::Font* f, const Color& c, const RectF& r) {
    if (s.empty()) return;
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, r, Typo(), &b);
}

std::unique_ptr<GraphicsPath> Round(const RectF& r, float radius) {
    std::unique_ptr<GraphicsPath> p(new GraphicsPath());
    float d = std::min(radius * 2, std::min(r.Width, r.Height));
    if (d <= 0.5f) { p->AddRectangle(r); return p; }
    p->AddArc(r.X, r.Y, d, d, 180, 90);
    p->AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p->AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p->CloseFigure();
    return p;
}

void Fill(Graphics& g, const RectF& r, float radius, const Color& c) {
    auto p = Round(r, radius);
    SolidBrush b(c);
    g.FillPath(&b, p.get());
}

void Stroke(Graphics& g, const RectF& r, float radius, const Color& c, float width, bool dashed) {
    auto p = Round(r, radius);
    Pen pen(c, width);
    if (dashed) { REAL dash[] = { 4.0f, 3.0f }; pen.SetDashPattern(dash, 2); }
    g.DrawPath(&pen, p.get());
}

Color Alpha(const Color& c, int a) { return Color((BYTE)a, c.GetR(), c.GetG(), c.GetB()); }

Color BadgeColor(const wstr& s) {
    static const Color palette[] = { Color(91, 79, 224), Color(14, 116, 204), Color(22, 128, 61), Color(217, 119, 6),
                                     Color(107, 114, 128), Color(190, 24, 93), Color(13, 148, 136) };
    uint32_t h = 0;
    for (wchar_t c : s) h = h * 31u + (uint32_t)c;
    return palette[(h & 0x7fffffffu) % 7];
}

void AppBadge(Graphics& g, const RectF& r, const wstr& name, Gdiplus::Bitmap* icon) {
    if (icon) {
        InterpolationMode old = g.GetInterpolationMode();
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(icon, r);
        g.SetInterpolationMode(old);
        return;
    }
    Fill(g, r, r.Width * 0.22f, BadgeColor(name));
    wstr t = Util::Trim(name);
    wstr letter = t.empty() ? L"?" : t.substr(0, 1);
    CharUpperBuffW(&letter[0], 1);
    Gdiplus::Font* f = Font(r.Height * 0.62f, true);
    SizeF sz = Measure(letter, f);
    Text(g, letter, f, Color(255, 255, 255), r.X + (r.Width - sz.Width) / 2, r.Y + (r.Height - sz.Height) / 2);
}

void Logo(Graphics& g, const RectF& r, const Color& back, const Color& front, const Color& rim) {
    float u = r.Width / 16.0f;
    RectF b(r.X + 0.75f * u, r.Y + 0.75f * u, 10.5f * u, 9.5f * u);
    Fill(g, b, 2.2f * u, Alpha(back, 70));
    Stroke(g, b, 2.2f * u, back, std::max(1.0f, 1.25f * u), false);
    RectF f(r.X + 5 * u, r.Y + 5 * u, 10.25f * u, 10.25f * u);
    Fill(g, RectF(f.X - 0.9f * u, f.Y - 0.9f * u, f.Width + 1.3f * u, f.Height + 1.3f * u), 3.1f * u, rim);
    Fill(g, f, 2.2f * u, front);
}

void CheckBadge(Graphics& g, const RectF& r, const Color& fill) {
    Fill(g, r, r.Width / 2, fill);
    Pen pen(Color(255, 255, 255), std::max(1.5f, r.Width / 9.0f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    PointF pts[] = { PointF(r.X + r.Width * 0.28f, r.Y + r.Height * 0.52f), PointF(r.X + r.Width * 0.44f, r.Y + r.Height * 0.68f),
                     PointF(r.X + r.Width * 0.73f, r.Y + r.Height * 0.36f) };
    g.DrawLines(&pen, pts, 3);
}

void CrossBadge(Graphics& g, const RectF& r, const Color& fill, const Color& mark) {
    Fill(g, r, r.Width / 2, fill);
    Pen pen(mark, std::max(1.5f, r.Width / 9.0f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    float a = r.Width * 0.32f;
    g.DrawLine(&pen, r.X + a, r.Y + a, r.GetRight() - a, r.GetBottom() - a);
    g.DrawLine(&pen, r.GetRight() - a, r.Y + a, r.X + a, r.GetBottom() - a);
}

void Arrow(Graphics& g, PointF from, PointF to, const Color& c, float width) {
    Pen pen(c, width);
    AdjustableArrowCap cap(3.2f, 3.2f, TRUE);
    pen.SetCustomEndCap(&cap);
    g.DrawLine(&pen, from, to);
}

void Glyph(Graphics& g, const wstr& name, const RectF& r, const Color& c) {
    float u = r.Width / 16.0f;
    Pen pen(c, std::max(1.2f, 1.4f * u));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    float x = r.X, y = r.Y;
    if (name == L"xray") {
        g.DrawPath(&pen, Round(RectF(x + 1.5f * u, y + 2 * u, 8.5f * u, 7.5f * u), 1.5f * u).get());
        g.DrawPath(&pen, Round(RectF(x + 6 * u, y + 6.5f * u, 8.5f * u, 7.5f * u), 1.5f * u).get());
    } else if (name == L"arrange") {
        g.DrawPath(&pen, Round(RectF(x + 1.5f * u, y + 2.5f * u, 13 * u, 11 * u), 1.5f * u).get());
        g.DrawLine(&pen, x + 1.5f * u, y + 5.5f * u, x + 14.5f * u, y + 5.5f * u);
        SolidBrush b(c);
        g.FillRectangle(&b, x + 3 * u, y + 7.5f * u, 4 * u, 4.5f * u);
    } else if (name == L"undo") {
        g.DrawArc(&pen, x + 3 * u, y + 3.5f * u, 10 * u, 9 * u, 200, 250);
        PointF pts[] = { PointF(x + 2.5f * u, y + 3 * u), PointF(x + 3.2f * u, y + 7 * u), PointF(x + 7 * u, y + 6.2f * u) };
        g.DrawLines(&pen, pts, 3);
    } else if (name == L"pause") {
        g.DrawLine(&pen, x + 5.5f * u, y + 3 * u, x + 5.5f * u, y + 13 * u);
        g.DrawLine(&pen, x + 10.5f * u, y + 3 * u, x + 10.5f * u, y + 13 * u);
    } else if (name == L"play") {
        PointF pts[] = { PointF(x + 5 * u, y + 3 * u), PointF(x + 13 * u, y + 8 * u), PointF(x + 5 * u, y + 13 * u) };
        g.DrawPolygon(&pen, pts, 3);
    } else if (name == L"settings") {
        SolidBrush b(c);
        for (int i = 0; i < 3; i++) {
            float yy = y + (4 + i * 4) * u;
            g.DrawLine(&pen, x + 2 * u, yy, x + 14 * u, yy);
            float xx = x + (i == 1 ? 10 : 5.5f) * u;
            g.FillEllipse(&b, xx - 1.6f * u, yy - 1.6f * u, 3.2f * u, 3.2f * u);
        }
    } else if (name == L"exit") {
        g.DrawArc(&pen, x + 3 * u, y + 3.5f * u, 10 * u, 10 * u, -60, 300);
        g.DrawLine(&pen, x + 8 * u, y + 2 * u, x + 8 * u, y + 8 * u);
    } else if (name == L"search") {
        g.DrawEllipse(&pen, x + 2 * u, y + 2 * u, 9 * u, 9 * u);
        g.DrawLine(&pen, x + 10 * u, y + 10 * u, x + 14 * u, y + 14 * u);
    } else if (name == L"target") {
        g.DrawEllipse(&pen, x + 2.5f * u, y + 2.5f * u, 11 * u, 11 * u);
        g.DrawLine(&pen, x + 8 * u, y + 0.5f * u, x + 8 * u, y + 5 * u);
        g.DrawLine(&pen, x + 8 * u, y + 11 * u, x + 8 * u, y + 15.5f * u);
        g.DrawLine(&pen, x + 0.5f * u, y + 8 * u, x + 5 * u, y + 8 * u);
        g.DrawLine(&pen, x + 11 * u, y + 8 * u, x + 15.5f * u, y + 8 * u);
    } else if (name == L"check") {
        PointF pts[] = { PointF(x + 3 * u, y + 8.5f * u), PointF(x + 6.5f * u, y + 12 * u), PointF(x + 13 * u, y + 4.5f * u) };
        g.DrawLines(&pen, pts, 3);
    } else if (name == L"chevdown") {
        PointF pts[] = { PointF(x + 3.5f * u, y + 6 * u), PointF(x + 8 * u, y + 10.5f * u), PointF(x + 12.5f * u, y + 6 * u) };
        g.DrawLines(&pen, pts, 3);
    } else if (name == L"chevron") {
        PointF pts[] = { PointF(x + 6 * u, y + 3.5f * u), PointF(x + 10.5f * u, y + 8 * u), PointF(x + 6 * u, y + 12.5f * u) };
        g.DrawLines(&pen, pts, 3);
    } else if (name == L"down") Arrow(g, PointF(x + 8 * u, y + 2 * u), PointF(x + 8 * u, y + 13 * u), c, pen.GetWidth());
    else if (name == L"up") Arrow(g, PointF(x + 8 * u, y + 14 * u), PointF(x + 8 * u, y + 3 * u), c, pen.GetWidth());
    else if (name == L"left") Arrow(g, PointF(x + 14 * u, y + 8 * u), PointF(x + 3 * u, y + 8 * u), c, pen.GetWidth());
    else if (name == L"right") Arrow(g, PointF(x + 2 * u, y + 8 * u), PointF(x + 13 * u, y + 8 * u), c, pen.GetWidth());
}

float RichText(Graphics& g, const wstr& s, float px, const Color& normal, const Color& bold, float x, float y, bool draw) {
    float cx = x;
    bool b = false;
    size_t pos = 0;
    while (true) {
        size_t next = s.find(L"**", pos);
        wstr part = s.substr(pos, next == wstr::npos ? wstr::npos : next - pos);
        if (!part.empty()) {
            Gdiplus::Font* f = Font(px, b);
            if (draw) Text(g, part, f, b ? bold : normal, cx, y);
            cx += Measure(part, f).Width;
        }
        b = !b;
        if (next == wstr::npos) break;
        pos = next + 2;
    }
    return cx - x;
}

// ---- GDI text, like WinForms TextRenderer (glyph overhang padding on both sides)

static int Padding(HDC dc) {
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    return (int)std::ceil(tm.tmHeight / 6.0);
}

SIZE MeasureGdi(const wstr& s, HFONT f, int width, bool wrap) {
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    int pad = Padding(dc);
    RECT r = { 0, 0, wrap ? std::max(1, width) : 1000000, 1000000 };
    DRAWTEXTPARAMS p = { sizeof p, 0, pad, pad, 0 };
    UINT flags = DT_CALCRECT | DT_NOPREFIX | (wrap ? DT_WORDBREAK : DT_SINGLELINE);
    wstr text = s.empty() ? wstr(L" ") : s;
    DrawTextExW(dc, &text[0], (int)text.size(), &r, flags, &p);
    SIZE sz = { r.right - r.left, r.bottom - r.top };
    if (s.empty()) sz.cx = 2 * pad;
    SelectObject(dc, old);
    DeleteDC(dc);
    sz.cx = std::min(width, (int)sz.cx + 2);
    return sz;
}

void TextGdi(HDC dc, const wstr& s, HFONT f, COLORREF c, const RECT& r, UINT flags) {
    if (s.empty()) return;
    HGDIOBJ old = SelectObject(dc, f);
    int pad = Padding(dc);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    RECT rr = r;
    DRAWTEXTPARAMS p = { sizeof p, 0, pad, pad, 0 };
    wstr text = s;
    DrawTextExW(dc, &text[0], (int)text.size(), &rr, flags | DT_NOPREFIX, &p);
    SelectObject(dc, old);
}

void Shutdown() {
    for (auto& kv : Fonts()) delete kv.second;
    Fonts().clear();
    for (auto& kv : GdiFonts()) DeleteObject(kv.second);
    GdiFonts().clear();
    delete measurer; measurer = nullptr;
    delete measureBitmap; measureBitmap = nullptr;
    delete typo; typo = nullptr;
    delete family; family = nullptr;
    delete semibold; semibold = nullptr;
    delete monoFamily; monoFamily = nullptr;
    familiesDone = false;
}

// ---- icons

static bool ReadBits(HBITMAP hb, int w, int h, std::vector<uint32_t>& out) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign((size_t)w * h, 0);
    HDC dc = GetDC(nullptr);
    int n = GetDIBits(dc, hb, 0, h, out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return n == h;
}

BitmapPtr IconBitmap(HICON icon, int size) {
    if (!icon || size <= 0) return BitmapPtr();
    ICONINFO ii = {};
    if (!GetIconInfo(icon, &ii)) return BitmapPtr();
    BitmapPtr result;
    BITMAP bm = {};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        int w = bm.bmWidth, h = bm.bmHeight;
        std::vector<uint32_t> color, mask;
        if (ReadBits(ii.hbmColor, w, h, color)) {
            bool alpha = false;
            for (uint32_t p : color) if (p >> 24) { alpha = true; break; }
            if (!alpha) {
                bool haveMask = ii.hbmMask && ReadBits(ii.hbmMask, w, h, mask);
                for (size_t i = 0; i < color.size(); i++) {
                    bool transparent = haveMask && (mask[i] & 0xFFFFFF) != 0;
                    color[i] = transparent ? 0 : (color[i] | 0xFF000000u);
                }
            }
            Bitmap src(w, h, PixelFormat32bppARGB);
            BitmapData data;
            Gdiplus::Rect all(0, 0, w, h);
            if (src.LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &data) == Ok) {
                for (int y = 0; y < h; y++) memcpy((BYTE*)data.Scan0 + (size_t)y * data.Stride, &color[(size_t)y * w], (size_t)w * 4);
                src.UnlockBits(&data);
                auto* dst = new Bitmap(size, size, PixelFormat32bppPARGB);
                {
                    Graphics g(dst);
                    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                    g.SetPixelOffsetMode(PixelOffsetModeHalf);
                    g.DrawImage(&src, RectF(0, 0, (REAL)size, (REAL)size));
                }
                result.reset(dst);
            }
        }
    }
    if (!result) {
        // monochrome icon: let GDI draw it on white and on black and work out the alpha
        auto* dst = new Bitmap(size, size, PixelFormat32bppARGB);
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP hb = CreateCompatibleBitmap(screen, size, size);
        HGDIOBJ old = SelectObject(dc, hb);
        RECT r = { 0, 0, size, size };
        FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        std::vector<uint32_t> bits;
        if (ReadBits(hb, size, size, bits)) {
            BitmapData data;
            Gdiplus::Rect all(0, 0, size, size);
            if (dst->LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &data) == Ok) {
                for (int y = 0; y < size; y++)
                    for (int x = 0; x < size; x++) {
                        uint32_t p = bits[(size_t)y * size + x] & 0xFFFFFF;
                        ((uint32_t*)((BYTE*)data.Scan0 + (size_t)y * data.Stride))[x] = p == 0xFFFFFF ? 0 : (p | 0xFF000000u);
                    }
                dst->UnlockBits(&data);
            }
        }
        DeleteObject(hb);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        result.reset(dst);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return result;
}

HICON MakeIcon(Gdiplus::Bitmap& bmp) {
    HICON h = nullptr;
    bmp.GetHICON(&h);
    return h;
}

} // namespace Draw

// ================================================================== owner-drawn popup menus

namespace PopupMenu {

namespace {
struct ItemData {
    const MenuItem* item;
    bool margin;      // the menu shows a glyph or check column
};

struct Context {
    float k = 1;
    std::function<SIZE(float)> headerSize;
    std::function<void(Graphics&, const RectF&, float)> headerPaint;
    std::vector<std::unique_ptr<ItemData>> data;
    bool active = false;
};
Context ctx;

bool HasMargin(const std::vector<MenuItem>& items) {
    for (const MenuItem& m : items) if (!m.glyph.empty() || m.checked) return true;
    return false;
}

bool HasCheck(const std::vector<MenuItem>& items) {
    for (const MenuItem& m : items) if (m.checked) return true;
    return false;
}

HMENU Build(const std::vector<MenuItem>& items, HBRUSH back) {
    HMENU menu = CreatePopupMenu();
    bool margin = HasMargin(items) || HasCheck(items);
    for (const MenuItem& m : items) {
        auto d = std::unique_ptr<ItemData>(new ItemData{ &m, margin });
        UINT flags = MF_OWNERDRAW;
        UINT_PTR id = (UINT_PTR)m.id;
        if (m.separator) flags |= MF_SEPARATOR | MF_DISABLED;
        if (!m.enabled || m.header) flags |= MF_GRAYED | MF_DISABLED;
        if (!m.sub.empty()) { flags |= MF_POPUP; id = (UINT_PTR)Build(m.sub, back); }
        AppendMenuW(menu, flags, id, (LPCWSTR)d.get());
        ctx.data.push_back(std::move(d));
    }
    MENUINFO mi = {};
    mi.cbSize = sizeof mi;
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = back;
    SetMenuInfo(menu, &mi);
    return menu;
}

bool Ours(ULONG_PTR p) {
    for (const auto& d : ctx.data) if ((ULONG_PTR)d.get() == p) return true;
    return false;
}

float TextLeft(const ItemData& d, float k) { return d.margin ? 40 * k : 16 * k; }

SIZE MeasureItem(const ItemData& d, float k) {
    const MenuItem& it = *d.item;
    SIZE s = { 0, 0 };
    if (it.header && ctx.headerSize) return ctx.headerSize(k);
    if (it.separator) { s.cx = (LONG)(20 * k); s.cy = (LONG)(9 * k); return s; }
    Gdiplus::Font* f = Draw::Font(14.5f * k, false);
    float w = TextLeft(d, k) + Draw::Measure(it.text, f).Width + 24 * k;
    if (!it.hint.empty()) w += 32 * k + Draw::Measure(it.hint, f).Width;
    if (!it.sub.empty()) w += 20 * k;
    s.cx = (LONG)std::ceil(w);
    s.cy = (LONG)std::ceil(Draw::Height(f) + 14 * k);
    return s;
}

void DrawItem(Graphics& g, const ItemData& d, int w, int h, bool selected, float k) {
    const MenuItem& it = *d.item;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    SolidBrush back(Theme::Card());
    g.FillRectangle(&back, 0, 0, w, h);
    if (it.header) {
        if (ctx.headerPaint) ctx.headerPaint(g, RectF(0, 0, (REAL)w, (REAL)h), k);
        return;
    }
    if (it.separator) {
        Pen pen(Theme::Border(), 1);
        float y = (float)(h / 2);
        g.DrawLine(&pen, TextLeft(d, k) - 4 * k, y, (float)w - 4 * k, y);
        return;
    }
    selected = selected && it.enabled;
    if (selected) Draw::Fill(g, RectF(4, 1, (REAL)w - 8, (REAL)h - 2), 5, Theme::Hover());
    Color ink = it.enabled ? Theme::Text() : Theme::Sub();
    float gs = 18 * k;
    if (it.checked) Draw::Glyph(g, L"check", RectF(12 * k, (h - gs) / 2, gs, gs), Theme::AccentText());
    else if (!it.glyph.empty()) Draw::Glyph(g, it.glyph, RectF(12 * k, (h - gs) / 2, gs, gs), ink);
    Gdiplus::Font* f = Draw::Font(14.5f * k, false);
    float fh = Draw::Height(f);
    Draw::Text(g, it.text, f, ink, TextLeft(d, k), (h - fh) / 2);
    float right = (float)w - (it.sub.empty() ? 16 * k : 30 * k);
    if (!it.hint.empty()) {
        float hw = Draw::Measure(it.hint, f).Width;
        Draw::Text(g, it.hint, f, Theme::Sub(), right - hw, (h - fh) / 2);
    }
    if (!it.sub.empty()) Draw::Glyph(g, L"chevron", RectF((float)w - 24 * k, (h - 14 * k) / 2, 14 * k, 14 * k), Theme::Sub());
}
} // namespace

bool OwnerDraw(UINT m, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (!ctx.active) return false;
    if (m == WM_MEASUREITEM) {
        auto* mis = (MEASUREITEMSTRUCT*)lp;
        if (mis->CtlType != ODT_MENU || !Ours(mis->itemData)) return false;
        SIZE s = MeasureItem(*(ItemData*)mis->itemData, ctx.k);
        mis->itemWidth = s.cx;
        mis->itemHeight = s.cy;
        result = TRUE;
        return true;
    }
    if (m == WM_DRAWITEM) {
        auto* dis = (DRAWITEMSTRUCT*)lp;
        if (dis->CtlType != ODT_MENU || !Ours(dis->itemData)) return false;
        RECT rc = dis->rcItem;
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            Bitmap buf(w, h, PixelFormat32bppPARGB);
            {
                Graphics g(&buf);
                DrawItem(g, *(ItemData*)dis->itemData, w, h, (dis->itemState & ODS_SELECTED) != 0, ctx.k);
            }
            Graphics out(dis->hDC);
            out.DrawImage(&buf, rc.left, rc.top, w, h);
        }
        // the system would draw its own (black) submenu arrow after this: keep it out
        ExcludeClipRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
        result = TRUE;
        return true;
    }
    return false;
}

Gdiplus::Bitmap* Render(const std::vector<MenuItem>& items, float k, std::function<SIZE(float)> headerSize,
                        std::function<void(Graphics&, const RectF&, float)> headerPaint, int selected) {
    ctx = Context();
    ctx.k = k;
    ctx.headerSize = std::move(headerSize);
    ctx.headerPaint = std::move(headerPaint);
    bool margin = HasMargin(items) || HasCheck(items);
    std::vector<ItemData> data;
    for (const MenuItem& m : items) data.push_back(ItemData{ &m, margin });
    int border = 1, padY = (int)(3 * k), w = 0, h = 0;
    std::vector<SIZE> sizes;
    for (const ItemData& d : data) { SIZE s = MeasureItem(d, k); sizes.push_back(s); w = std::max(w, (int)s.cx); h += s.cy; }
    w += GetSystemMetrics(SM_CXMENUCHECK);   // Windows adds this to owner-drawn items
    auto* bmp = new Bitmap(w + 2 * border, h + 2 * border + 2 * padY, PixelFormat32bppARGB);
    {
        Graphics g(bmp);
        SolidBrush back(Theme::Card());
        g.FillRectangle(&back, 0, 0, (INT)bmp->GetWidth(), (INT)bmp->GetHeight());
        Pen pen(Theme::Border(), 1);
        g.DrawRectangle(&pen, 0, 0, (INT)bmp->GetWidth() - 1, (INT)bmp->GetHeight() - 1);
        int y = border + padY;
        for (size_t i = 0; i < data.size(); i++) {
            Bitmap item(w, sizes[i].cy, PixelFormat32bppPARGB);
            {
                Graphics ig(&item);
                DrawItem(ig, data[i], w, sizes[i].cy, (int)i == selected, k);
            }
            g.DrawImage(&item, border, y, w, sizes[i].cy);
            y += sizes[i].cy;
        }
    }
    ctx = Context();
    return bmp;
}

int Show(HWND owner, const std::vector<MenuItem>& items, int x, int y, float k, UINT align,
         std::function<SIZE(float)> headerSize, std::function<void(Graphics&, const RectF&, float)> headerPaint) {
    if (ctx.active) return 0;
    Theme::Refresh();
    ctx = Context();
    ctx.k = k;
    ctx.headerSize = std::move(headerSize);
    ctx.headerPaint = std::move(headerPaint);
    ctx.active = true;
    HBRUSH back = CreateSolidBrush(Theme::Ref(Theme::Card()));
    HMENU menu = Build(items, back);
    int id = (int)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | align, x, y, owner, nullptr);
    DestroyMenu(menu);
    DeleteObject(back);
    ctx = Context();
    return id;
}

} // namespace PopupMenu

} // namespace glimpse
