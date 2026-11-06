// Glimpse native build: preview images, Glimpse.exe /render:<folder>. Renders the drag overlay, X-ray,
// settings, toast and tray menu with fake windows, using the same drawing code as the real thing.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"

namespace glimpse {

using namespace Gdiplus;

namespace {

const Rc Screen(0, 0, 1600, 1000);
const Rc Work(0, 0, 1600, 952);

bool PngEncoder(CLSID& clsid) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, codecs);
    for (UINT i = 0; i < n; i++)
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) { clsid = codecs[i].Clsid; return true; }
    return false;
}

void Save(Gdiplus::Bitmap* b, const wstr& folder, const wstr& name) {
    std::unique_ptr<Gdiplus::Bitmap> owned(b);
    if (!b) return;
    CLSID png;
    if (!PngEncoder(png)) throw std::runtime_error("no PNG encoder");
    wstr path = Util::Combine(folder, name);
    if (b->Save(path.c_str(), &png, nullptr) != Ok) throw std::runtime_error("cannot save " + Util::Narrow(path));
    ConsoleOut::Line(L"  " + name);
}

WinPtr Fake(const wstr& title, const Rc& frame) {
    auto w = std::make_shared<TrackedWindow>();
    w->title = title;
    w->frame = frame;
    w->windowRect = frame;
    w->counted = true;
    w->resizable = true;
    w->hasCaption = true;
    w->process = L"app.exe";
    return w;
}

void Desktop(Graphics& g) {
    Gdiplus::Rect r(Screen.L, Screen.T, Screen.W(), Screen.H());
    LinearGradientBrush b(r, Color(28, 52, 82), Color(70, 120, 150), 30.0f);
    g.FillRectangle(&b, r);
    SolidBrush bar(Color(32, 32, 38));
    g.FillRectangle(&bar, 0, Work.B, Screen.W(), Screen.B - Work.B);
}

// a plain window: title bar with badge and caption buttons, skeleton content
void FakeWindow(Graphics& g, const Rc& f, const wstr& title) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    RectF r((REAL)f.L, (REAL)f.T, (REAL)f.W(), (REAL)f.H());
    Draw::Fill(g, RectF(r.X + 2, r.Y + 4, r.Width, r.Height), 8, Color(50, 0, 0, 0));
    Draw::Fill(g, r, 8, Color(250, 250, 252));
    Draw::Stroke(g, r, 8, Color(205, 205, 214), 1, false);
    {
        SolidBrush b(Color(240, 240, 244));
        g.FillRectangle(&b, r.X + 1, r.Y + 1, r.Width - 2, 38.0f);
    }
    Draw::AppBadge(g, RectF(r.X + 12, r.Y + 11, 18, 18), title, nullptr);
    bool buttons = r.Width >= 300;
    Draw::TextIn(g, title, Draw::Font(14, false), Color(40, 40, 48), RectF(r.X + 38, r.Y + 11, std::max(10.0f, r.Width - (buttons ? 190 : 50)), 20));
    if (buttons) {
        Pen pen(Color(60, 60, 70), 1.2f);
        float y = r.Y + 20;
        g.DrawLine(&pen, r.GetRight() - 128, y, r.GetRight() - 118, y);
        g.DrawRectangle(&pen, r.GetRight() - 80, y - 5, 10.0f, 10.0f);
        g.DrawLine(&pen, r.GetRight() - 32, y - 5, r.GetRight() - 22, y + 5);
        g.DrawLine(&pen, r.GetRight() - 22, y - 5, r.GetRight() - 32, y + 5);
    }
    SolidBrush b(Color(228, 228, 234));
    for (float y = r.Y + 62; y < r.GetBottom() - 20; y += 34) {
        g.FillRectangle(&b, r.X + 20, y, std::min(90.0f, r.Width / 5), 10.0f);
        if (r.Width > 260) g.FillRectangle(&b, r.X + 30 + std::min(90.0f, r.Width / 5), y, std::min(200.0f, r.Width / 3), 10.0f);
        if (r.Width > 520) g.FillRectangle(&b, r.X + r.Width * 0.6f, y, std::min(140.0f, r.Width / 5), 10.0f);
    }
}

Layout MakeLayout(const std::vector<WinPtr>& list, int dpi) {
    Layout L;
    L.glimpseSize = 50;
    L.monitors.push_back(GeoMonitor(Screen, Work, dpi));
    for (const WinPtr& w : list) {
        GeoWindow g;
        g.id = w->title;
        g.frame = w->frame;
        g.dpi = dpi;
        g.tag = w.get();
        g.resizable = true;
        if (w->elevated) { g.movable = false; g.fixedReason = L"elevated"; }
        L.windows.push_back(g);
    }
    L.Begin();
    return L;
}

Gdiplus::Bitmap* Scene(const std::vector<WinPtr>& list, DragScene& scene, bool reversed) {
    auto* bmp = new Gdiplus::Bitmap(Screen.W(), Screen.H(), PixelFormat32bppPARGB);
    Graphics g(bmp);
    Desktop(g);
    if (reversed) for (int i = (int)list.size() - 1; i >= 1; i--) FakeWindow(g, list[i]->frame, list[i]->title);
    else for (size_t i = 1; i < list.size(); i++) FakeWindow(g, list[i]->frame, list[i]->title);
    FakeWindow(g, list[0]->frame, list[0]->title);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    Elements els = OverlayElements::ForDrag(scene, [](const Rc&) { return 96; }, [](const Rc&) { return Screen; });
    for (const OverlayElement& e : els) e.draw(g);
    return bmp;
}

Gdiplus::Bitmap* DragPreview() {
    Settings s = Settings::Defaults();
    std::vector<WinPtr> list = {
        Fake(L"Order #4711 \u2014 ERP", Rc(820, 180, 1520, 790)),
        Fake(L"Invoices \u2014 ERP", Rc(40, 40, 860, 600)),
        Fake(L"Inbox \u2014 Outlook", Rc(40, 640, 760, 940)),
        Fake(L"Q4 budget.xlsx \u2014 Excel", Rc(880, 330, 1300, 760)),
        Fake(L"Services (admin)", Rc(1250, 420, 1560, 700)),
    };
    list[4]->elevated = true;
    DragState d;
    d.w = list[0];
    d.startFrame = Rc(900, 120, 1600, 730);
    d.current = list[0]->frame;
    Layout L = MakeLayout(list, 96);
    DragScene scene = DragScene::Compute(L, list, 0, d, list[0]->frame, false, true, s, false);
    scene.cursor.x = 1010;
    scene.cursor.y = 200;
    return Scene(list, scene, true);
}

Gdiplus::Bitmap* ResizePreview() {
    Settings s = Settings::Defaults();
    std::vector<WinPtr> list = { Fake(L"Order #4711 \u2014 ERP", Rc(700, 200, 860, 520)), Fake(L"Invoices \u2014 ERP", Rc(80, 80, 900, 640)) };
    Layout L = MakeLayout(list, 96);
    L.windows[0].minW = 480;
    L.windows[0].minH = 360;
    MinSizeRule rule;
    rule.pattern = L"^Order #\\d+";
    rule.minWidth = 480;
    rule.minHeight = 360;
    rule.Compile();
    s.minSizes.insert(s.minSizes.begin(), rule);
    DragState d;
    d.w = list[0];
    d.startFrame = Rc(700, 200, 1300, 620);
    d.hit = HTRIGHT;
    d.current = list[0]->frame;
    DragScene scene = DragScene::Compute(L, list, 0, d, list[0]->frame, false, true, s, false);
    scene.cursor.x = 860;
    scene.cursor.y = 380;
    return Scene(list, scene, false);
}

Gdiplus::Bitmap* XRayPreview() {
    auto entry = [](const wstr& title, const Rc& frame, int number, bool minimized) {
        XRayEntry e;
        e.title = title;
        e.frame = frame;
        e.number = number;
        e.minimized = minimized;
        return e;
    };
    std::vector<XRayEntry> entries = {
        entry(L"Inbox \u2014 Outlook", Rc(70, 120, 690, 530), 1, false),
        entry(L"Q4 budget.xlsx \u2014 Excel", Rc(500, 310, 1150, 740), 2, false),
        entry(L"Order #4711 \u2014 ERP", Rc(880, 140, 1520, 590), 3, false),
        entry(L"Services (admin)", Rc(1200, 540, 1550, 800), 4, false),
        entry(L"Teams \u2014 Chat", Rc(), 5, true),
        entry(L"Calculator", Rc(), 6, true),
    };
    MonitorInfo mon;
    mon.bounds = Screen;
    mon.work = Work;
    mon.dpi = 96;
    mon.primary = true;
    auto* bmp = new Gdiplus::Bitmap(Screen.W(), Screen.H(), PixelFormat32bppPARGB);
    Graphics g(bmp);
    Desktop(g);
    FakeWindow(g, Work, L"Invoices \u2014 ERP");
    XRayView::PaintScene(g, mon, true, entries, L"bud", 1, -1);
    return bmp;
}

Gdiplus::Bitmap* ToastPreview() {
    struct T { SIZE s; wstr t, u, a; };
    std::vector<T> toasts = {
        { Toast::Measure(Texts::P(L"toast.moved", 2), L"Q4 budget.xlsx, Inbox", Texts::T(L"toast.undo"), 1), Texts::P(L"toast.moved", 2), L"Q4 budget.xlsx, Inbox", Texts::T(L"toast.undo") },
        { Toast::Measure(Texts::P(L"toast.display", 3), L"", Texts::T(L"toast.undo"), 1), Texts::P(L"toast.display", 3), L"", Texts::T(L"toast.undo") },
        { Toast::Measure(Texts::T(L"toast.layout"), Texts::T(L"toast.layoutHint"), Texts::T(L"toast.restore"), 1), Texts::T(L"toast.layout"), Texts::T(L"toast.layoutHint"), Texts::T(L"toast.restore") },
    };
    int w = 0, h = 80;
    for (const T& t : toasts) { w = std::max(w, (int)t.s.cx); h += t.s.cy; }
    w += 40;
    auto* bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
    Graphics g(bmp);
    g.Clear(Color(70, 110, 140));
    int y = 20;
    for (const T& t : toasts) {
        GraphicsState state = g.Save();
        g.TranslateTransform(20, (REAL)y);
        Toast::Paint(g, t.s, t.t, t.u, t.a, false, 1);
        g.Restore(state);
        y += t.s.cy + 20;
    }
    return bmp;
}

Gdiplus::Bitmap* TrayIcons() {
    const int sizes[] = { 16, 20, 24, 32, 48 };
    auto* bmp = new Gdiplus::Bitmap(4 * 70 + 20, 5 * 60 + 20, PixelFormat32bppARGB);
    Graphics g(bmp);
    g.Clear(Color(32, 32, 38));
    for (int i = 0; i < 5; i++)
        for (int st = 0; st < 4; st++) {
            std::unique_ptr<Gdiplus::Bitmap> ic(TrayUi::RenderIcon(sizes[i], st, 2));
            g.DrawImage(ic.get(), 20 + st * 70, 20 + i * 60);
        }
    return bmp;
}

Gdiplus::Bitmap* TrayMenuPreview() {
    wstr status = Texts::P(L"status.guarding", 12) + L" \u00b7 " + Texts::T(L"status.allInReach");
    std::vector<wstr> notes;
    Color dot = Theme::Green();
    std::vector<MenuItem> items = TrayUi::MenuItems(L"Ctrl+Alt+Space", Texts::P(L"windows", 2), true, false, false);
    return PopupMenu::Render(items, 1, [&](float k) { return TrayUi::HeaderSize(k, status, notes); },
                             [&](Graphics& g, const RectF& r, float k) { TrayUi::PaintHeader(g, r, k, status, dot, notes); }, 3);
}

Gdiplus::Bitmap* SettingsPreview(int dpi) {
    Settings s = Settings::Defaults();
    MinSizeRule rule;
    rule.pattern = L"^Order #\\d+";
    rule.minWidth = 480;
    rule.minHeight = 360;
    rule.note = L"ERP order forms";
    rule.Compile();
    s.minSizes.insert(s.minSizes.begin(), rule);
    s.exclusions.push_back(Exclusion::Process(L"teams.exe"));
    s.exclusions.push_back(Exclusion::Title(L"^Calculator$"));
    SettingsWindow w(nullptr, s, dpi);
    return w.Snapshot();
}

} // namespace

namespace Previews {
int Render(const wstr& folder, const wstr& language) {
    Ui::Init();
    Ui::StartGdiplus();
    int code = 0;
    try {
        Util::CreateDirs(Util::FullPath(folder));
        Texts::Load(language.empty() ? L"en" : language);
        Theme::forced = 0;
        Theme::dark = false;
        Save(DragPreview(), folder, L"drag.png");
        Save(ResizePreview(), folder, L"resize.png");
        Save(XRayPreview(), folder, L"xray.png");
        Save(ToastPreview(), folder, L"toast.png");
        Save(TrayIcons(), folder, L"tray-icons.png");
        Save(TrayMenuPreview(), folder, L"tray-menu.png");
        Save(SettingsPreview(96), folder, L"settings.png");
        Save(SettingsPreview(144), folder, L"settings-150.png");
        Theme::forced = 1;
        Theme::dark = true;
        Save(SettingsPreview(96), folder, L"settings-dark.png");
        Save(TrayMenuPreview(), folder, L"tray-menu-dark.png");
        Save(ToastPreview(), folder, L"toast-dark.png");
        Theme::forced = -1;
        Theme::dark = false;
        ConsoleOut::Line(L"previews written to " + Util::FullPath(folder));
    } catch (const std::exception& ex) {
        ConsoleOut::Line(L"render failed: " + Util::Widen(ex.what()));
        code = 1;
    }
    Ui::StopGdiplus();
    return code;
}
}

} // namespace glimpse
