// Glimpse native build: runtime declarations (registry, intake, actuator, coordinator, UI).
// The same components as the C# build, one to one; see Glimpse.cs for the reasoning behind each.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "glimpse.h"
#include <commctrl.h>

namespace glimpse {

typedef Gdiplus::Color Color;
typedef Gdiplus::RectF RectF;
typedef Gdiplus::PointF PointF;
typedef Gdiplus::SizeF SizeF;
typedef Gdiplus::Graphics Graphics;
typedef std::shared_ptr<Gdiplus::Bitmap> BitmapPtr;

// ------------------------------------------------------------------ Win32 helpers

namespace Native {
void SetPerMonitorV2();
int DpiForMonitor(HMONITOR hmon);
int DpiForWindow(HWND hwnd);
bool DragFullWindows();
bool TryFrame(HWND hwnd, Rc& r);
bool IsCloaked(HWND hwnd);
void SetDarkTitleBar(HWND hwnd, bool dark);
void SetRoundCorners(HWND hwnd, bool smallRadius);
int IntegrityLevel(DWORD pid);   // 0: this process; -1 when it cannot be read
wstr ProcessFileName(DWORD pid);
bool KeyDown(int vk);
void ForceForeground(HWND hwnd);
wstr WindowText(HWND hwnd);
wstr ClassName(HWND hwnd);
POINT CursorPos();
}

// ------------------------------------------------------------------ UI thread: timers, posted calls

class Timer {
public:
    explicit Timer(int intervalMs = 100);
    ~Timer();
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    int interval;
    std::function<void()> tick;
    void Start();                       // (re)starts the interval, like a WinForms timer
    void Stop();
    bool Enabled() const { return enabled; }
private:
    UINT_PTR id;
    bool enabled = false;
};

namespace Ui {
void Init();
HINSTANCE Instance();
HWND Host();                                         // hidden message-only window: timers, posts, menus
void Post(std::function<void()> fn);                 // runs later on the UI thread
void Later(int ms, std::function<void()> fn);        // one-shot timer
ATOM RegisterClass(const wchar_t* name, WNDPROC proc, UINT style = 0, HCURSOR cursor = nullptr);
void StartGdiplus();
void StopGdiplus();
int RunLoop();                                       // message loop until Quit
void Quit(int code);
void AddDialog(HWND h);                              // windows that get IsDialogMessage
void RemoveDialog(HWND h);
}

// ------------------------------------------------------------------ monitors

struct MonitorInfo {
    HMONITOR handle = nullptr;
    Rc bounds, work;
    int dpi = 96;
    bool primary = false;
    wstr device;
};

namespace Monitors {
std::vector<MonitorInfo> Read();
wstr Signature(const std::vector<MonitorInfo>& list);
const MonitorInfo* At(const std::vector<MonitorInfo>& list, const Rc& r);
const MonitorInfo* AtPoint(const std::vector<MonitorInfo>& list, int x, int y);
}

// ------------------------------------------------------------------ window registry

struct TrackedWindow {
    HWND handle = nullptr;
    DWORD pid = 0;
    wstr process, cls, title;
    Rc frame;                   // visible bounds (DWM extended frame), physical px
    Rc windowRect;              // GetWindowRect, includes the invisible resize border
    bool counted = false;       // visible app window, not minimized, not cloaked
    bool minimized = false, maximized = false, fullScreen = false;
    bool topmost = false, elevated = false, hung = false, resizable = false, hasCaption = false, excluded = false, stubborn = false;
    int dpi = 96;
    long long lastActive = 0;   // MRU: last time it was the foreground window
    long long zOrder = 0;       // position in the last EnumWindows pass (0 = top)
    bool placed = false;        // new-window placement already ran (or it existed at start)
    bool wasMaximized = false;
    Rc settledFrame;            // frame after the last processed change
    long long suppressUntil = 0;
    Rc fixedFrom, fixedTo;
    long long fixedAt = 0;
    std::vector<long long> reverts;
    bool hasRuleKey = false;    // the minimum-size rule last applied (title changes)
    wstr ruleKey;
    std::shared_ptr<TopEdgeMap> topMap;
    int topMapWidth = -1;
    int captionW = -1, captionH = -1, iconW = -1;
    Rc prevWork;
    bool hidden = false;        // no free glimpse: reachable through X-ray
    wstr hiddenReason;
    BitmapPtr icon;
    bool iconTried = false;
    long long userPlacedAt = 0;
    ZonePtr zone;               // its free glimpse at the last check
    wstr ShortTitle() const { return Util::Shorten(!title.empty() ? title : process, 60); }
};
typedef std::shared_ptr<TrackedWindow> WinPtr;

class WindowRegistry {
public:
    std::map<HWND, WinPtr> all;
    WindowRegistry();
    WinPtr Get(HWND h) const;
    bool IsAppWindow(HWND h, wstr& cls) const;
    void Refresh(const Settings& s, const std::vector<MonitorInfo>& mons);
    void Update(TrackedWindow& w, const Settings* s, const std::vector<MonitorInfo>& mons);
    void Touch(HWND h);
    std::vector<WinPtr> CountedByMru() const;
    std::vector<WinPtr> AllByMru() const;
    static Gdiplus::Bitmap* LoadIcon(TrackedWindow& w, int size);
private:
    std::map<DWORD, wstr> processNames;
    std::map<DWORD, int> integrity;
    DWORD ownPid;
    int ownIntegrity;
    long long probeBudgetMs = 0;
    wstr ProcessName(DWORD pid);
    bool IsElevated(DWORD pid);
    void ProbeTopEdge(TrackedWindow& w);
};

// ------------------------------------------------------------------ events, actuator, undo, drag

class EventIntake {
public:
    explicit EventIntake(std::function<void(DWORD, HWND)> handler);
    ~EventIntake();
private:
    std::vector<HWINEVENTHOOK> hooks;
};

struct UndoItem {
    HWND handle = nullptr;
    Rc before, after;   // window rectangles
    wstr title;
};

struct UndoGroup {
    std::vector<UndoItem> items;
    wstr reason;
    long long time = 0;
};
typedef std::shared_ptr<UndoGroup> UndoPtr;

class UndoStack {
public:
    UndoPtr Last() const { return groups.empty() ? UndoPtr() : groups.back(); }
    void Push(const UndoPtr& g);
    bool Remove(const UndoPtr& g);
    bool Contains(const UndoPtr& g) const;
private:
    std::vector<UndoPtr> groups;
};

namespace Actuator {
const int OwnMoveQuietMs = 500;
bool MoveFrame(TrackedWindow& w, const Rc& frame, UndoGroup* undo, bool& accessDenied);
bool SetRect(TrackedWindow* w, HWND h, const Rc& current, const Rc& target, bool& accessDenied);
int UndoGroupNow(const UndoGroup& g, WindowRegistry& reg);
}

struct DragState {
    HWND handle = nullptr;
    WinPtr w;
    Rc startFrame;
    POINT startCursor = { 0, 0 };
    bool fullDrag = false;
    int hit = HTCAPTION;          // what was grabbed: caption or an edge
    Rc current;
    bool shiftAtEnd = false, lastShift = false, computed = false;
    bool byButton = false;        // detected from the mouse button (no MOVESIZESTART from the app)
    std::vector<Guide> guides;
    bool Resizing() const { return hit != HTCAPTION; }
    wstr Edges() const;
    Rc Predict(POINT c) const;
    static wstr ChangedEdges(const Rc& a, const Rc& b);
};

// ------------------------------------------------------------------ hotkey, hot corner, memory, first click

class HotkeyManager {
public:
    static const int XRayId = 1, TestId = 2;
    explicit HotkeyManager(HWND owner) : owner(owner) {}
    HotkeySpec current;
    bool registered = false;
    wstr problem;                       // why the X-ray hotkey is not active
    std::function<void()> testArrived;  // set while "Press it now" waits
    void Apply(const wstr& text);
    void Unregister();
    bool CanRegister(const HotkeySpec& spec);
private:
    HWND owner;
};

class HotCorner {
public:
    explicit HotCorner(std::function<void()> fire);
    Corner which = Corner::Off;
    void Set(Corner c);
    void Suspend(bool off);
private:
    Timer timer;
    std::function<void()> fire;
    long long since = -1;
    bool fired = false;
    void Tick();
    bool InCorner(POINT p) const;
};

namespace StartWithWindows {
void Sync(bool enabled);
}

class SizeMemory {
public:
    void Load();
    static wstr KindOf(const TrackedWindow& w);
    void Remember(const TrackedWindow& w, const Rc& frame);
    bool TryGet(const TrackedWindow& w, int& width, int& height);
    void SaveIfDirty();
private:
    wstr Path() const;
    IniFile ini;
    bool loaded = false, dirty = false;
};

class LayoutMemory {
public:
    void Save(const wstr& signature, const std::map<HWND, WinPtr>& windows);
    const std::map<HWND, Rc>* Get(const wstr& signature) const;
private:
    std::map<wstr, std::map<HWND, Rc>> bySignature;
};

class FirstClickHook {
public:
    FirstClickHook(std::function<HWND(int, int)> zoneOwnerAt, std::function<void(HWND)> activate);
    ~FirstClickHook();
private:
    static LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp);
    HHOOK hook = nullptr;
    std::function<HWND(int, int)> zoneOwnerAt;
    std::function<void(HWND)> activate;
    bool swallowUp = false;
};

class MessageWindow {
public:
    explicit MessageWindow(std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> handler);
    ~MessageWindow();
    HWND handle = nullptr;
private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp);
    std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> handler;
};

namespace Ipc {
const int CmdSettings = 1, CmdExit = 2, CmdXRay = 3;
extern const wchar_t* const WindowTitle;
UINT Message();
void SignalRunningInstance(int cmd);
}

// ------------------------------------------------------------------ look: theme, fonts, drawing

namespace Theme {
extern bool dark;
extern int forced;     // -1 follow Windows, 0 light, 1 dark (preview images)
void Refresh();
Color Accent(); Color AccentHover(); Color Green(); Color GreenSoft(); Color GreenLine();
Color Amber(); Color AmberSoft(); Color Red(); Color RedSoft(); Color GuideC(); Color Ink(); Color Chip();
Color Page(); Color Card(); Color Border(); Color Text(); Color Sub(); Color Hover(); Color Input();
Color AccentSoft(); Color AccentText(); Color Off();
COLORREF Ref(const Color& c);
}

namespace Draw {
Gdiplus::Font* Font(float px, bool bold);
Gdiplus::Font* Mono(float px);
HFONT GdiFont(float px, bool bold, bool mono = false);   // same faces for GDI text and edit boxes
float Height(Gdiplus::Font* f);                          // like .NET Font.Height
SizeF Measure(const wstr& s, Gdiplus::Font* f);
const Gdiplus::StringFormat* Typo();
void Text(Graphics& g, const wstr& s, Gdiplus::Font* f, const Color& c, float x, float y);
void TextIn(Graphics& g, const wstr& s, Gdiplus::Font* f, const Color& c, const RectF& r);
std::unique_ptr<Gdiplus::GraphicsPath> Round(const RectF& r, float radius);
void Fill(Graphics& g, const RectF& r, float radius, const Color& c);
void Stroke(Graphics& g, const RectF& r, float radius, const Color& c, float width, bool dashed);
Color Alpha(const Color& c, int a);
Color BadgeColor(const wstr& s);
void AppBadge(Graphics& g, const RectF& r, const wstr& name, Gdiplus::Bitmap* icon);
void Logo(Graphics& g, const RectF& r, const Color& back, const Color& front, const Color& rim);
void CheckBadge(Graphics& g, const RectF& r, const Color& fill);
void CrossBadge(Graphics& g, const RectF& r, const Color& fill, const Color& mark);
void Arrow(Graphics& g, PointF from, PointF to, const Color& c, float width);
void Glyph(Graphics& g, const wstr& name, const RectF& r, const Color& c);
float RichText(Graphics& g, const wstr& s, float px, const Color& normal, const Color& bold, float x, float y, bool draw);
// GDI text (like WinForms TextRenderer): single line with ellipsis, or word-wrapped
SIZE MeasureGdi(const wstr& s, HFONT f, int width, bool wrap);
void TextGdi(HDC dc, const wstr& s, HFONT f, COLORREF c, const RECT& r, UINT flags);
void Shutdown();
BitmapPtr IconBitmap(HICON icon, int size);
HICON MakeIcon(Gdiplus::Bitmap& bmp);
}

// ------------------------------------------------------------------ owner-drawn popup menus

struct MenuItem {
    int id = 0;                   // command (0: none)
    wstr text, hint, glyph;
    bool checked = false, enabled = true, separator = false, header = false;
    std::vector<MenuItem> sub;
};

namespace PopupMenu {
// Shows the menu at x, y (screen) and returns the chosen id, or 0. k: DPI scale.
// header: optional painter for a header item (menu.header = true), with its size.
int Show(HWND owner, const std::vector<MenuItem>& items, int x, int y, float k, UINT align,
         std::function<SIZE(float)> headerSize = nullptr, std::function<void(Graphics&, const RectF&, float)> headerPaint = nullptr);
// every window that owns a popup menu passes WM_MEASUREITEM and WM_DRAWITEM here first
bool OwnerDraw(UINT m, WPARAM wp, LPARAM lp, LRESULT& result);
// true while a menu is open (its owner must stay alive until it closes)
bool IsOpen();
// the same menu as a picture (preview images)
Gdiplus::Bitmap* Render(const std::vector<MenuItem>& items, float k, std::function<SIZE(float)> headerSize,
                        std::function<void(Graphics&, const RectF&, float)> headerPaint, int selected);
}

// ------------------------------------------------------------------ drag scene and overlays

struct SceneWindow {
    WinPtr w;
    wstr title;
    BitmapPtr icon;
    Rc frame, newFrame;
    ZonePtr zone, newZone;
    int state = 0;      // 0 keeps a glimpse, 1 moves on drop, 2 would be hidden
    wstr reason;
};

struct DragScene {
    Rc dragged;
    WinPtr draggedWindow;
    ZonePtr draggedZone;
    std::vector<SceneWindow> others;
    std::vector<Guide> guides;
    POINT cursor = { 0, 0 };
    int stay = 0, moves = 0, hidden = 0;
    Rc minOutline;
    wstr minText;
    bool shift = false, paused = false, threatened = false;
    FixMode mode = FixMode::Assist;
    static DragScene Compute(Layout& L, const std::vector<WinPtr>& list, int ai, const DragState& d, const Rc& cur, bool shift, bool fix, const Settings& S, bool paused);
};

struct OverlayElement {
    Rc bounds;
    wstr key;
    std::function<void(Graphics&)> draw;
};
typedef std::vector<OverlayElement> Elements;

namespace OverlayElements {
OverlayElement Pill(int x, int y, int dpi, const wstr& text, BitmapPtr icon, bool badge, const wstr& badgeName, Color fill, Color border, Color ink, int trailing, Color trailColor, float maxWidth);
OverlayElement Outline(const Rc& r, int dpi, Color c, bool dashed, int fillAlpha, float width);
Elements ForDrag(const DragScene& s, std::function<int(const Rc&)> dpiAt, std::function<Rc(const Rc&)> screenAt);
Elements ForFlash(const std::vector<Rc>& frames, std::function<int(const Rc&)> dpiAt);
wstr ReasonText(const wstr& reason);
}

class GlimpseApp;

class OverlaySurface {
public:
    OverlaySurface(const MonitorInfo& mon, std::function<void(const wstr&)> onDpi, HWND owner);
    ~OverlaySurface();
    MonitorInfo mon;
    void Render(const Elements& all);
    void Clear();
private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp);
    void Idle();
    void EnsureBuffer();
    void FreeBuffer();
    void PushDib(const Rc* dirtyScreen);
    HWND hwnd = nullptr;
    std::function<void(const wstr&)> onDpi;
    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldObj = nullptr;
    void* bits = nullptr;
    int bw = 0, bh = 0;
    std::map<wstr, Rc> shown;
    bool big = false;
};

class OverlayManager {
public:
    explicit OverlayManager(GlimpseApp* app);
    ~OverlayManager();
    void OnMonitorsChanged();
    int DpiAt(const Rc& r) const;
    Rc ScreenAt(const Rc& r) const;
    void ShowDrag(const DragScene& scene);
    void HideDrag();
    void Flash(const std::vector<Rc>& frames);
    void ShowPick(const Rc& frame);
    void HidePick();
    void HideAll();
    bool DragShown() const { return !drag.empty(); }
private:
    void Compose();
    GlimpseApp* app;
    std::vector<std::unique_ptr<OverlaySurface>> surfaces;
    Elements drag, flash, pick;
    Timer flashTimer;
    long long lastRender = 0;
};

// ------------------------------------------------------------------ X-ray

struct XRayEntry {
    WinPtr w;
    Rc frame;
    wstr title;
    int number = 0;
    bool minimized = false;
    Rc label;   // screen rectangle of its name label (set while painting)
};

class XRaySurface;

class XRayView {
public:
    explicit XRayView(GlimpseApp* app);
    ~XRayView();
    std::vector<XRayEntry> entries;
    wstr query;
    int selected = -1, hover = -1;
    bool IsOpen() const { return !surfaces.empty(); }
    static std::vector<XRayEntry> Collect(const WindowRegistry& reg, bool showMinimized);
    void Show(bool fromKeyboard);
    void Close();
    bool Owns(HWND h) const;
    void RenderAll();
    std::vector<int> MatchList() const;
    void OnKey(UINT vk, bool shift);
    void OnChar(wchar_t c);
    void OnClick(int x, int y, bool shift);
    void OnMove(int x, int y);
    void OnDeactivated(HWND other);
    static void PaintScene(Graphics& g, const MonitorInfo& mon, bool isMain, std::vector<XRayEntry>& entries, const wstr& query, int selected, int hover);
    static bool Matches(const wstr& title, const wstr& q);
private:
    void Step(const std::vector<int>& m, int dir);
    void PickNumber(bool timeout);
    void SelectFirstMatch();
    int HitTest(int x, int y) const;
    void Activate(const XRayEntry& e, bool shift);
    GlimpseApp* app;
    std::vector<std::unique_ptr<XRaySurface>> surfaces;
    XRaySurface* main = nullptr;
    wstr digits;
    Timer digitTimer;
};

class XRaySurface {
public:
    XRaySurface(XRayView* view, const MonitorInfo& mon, HWND owner);
    ~XRaySurface();
    MonitorInfo mon;
    HWND hwnd = nullptr;
    void ShowNow();
    void Render(std::function<void(Graphics&)> paint);
private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp);
    XRayView* view;
};

// ------------------------------------------------------------------ tray, toast

class TrayUi {
public:
    explicit TrayUi(GlimpseApp* app);
    ~TrayUi();
    void Rebuild();
    void Update();
    wstr StatusText(Color& dot) const;
    std::vector<wstr> Notes() const;
    static Gdiplus::Bitmap* RenderIcon(int size, int state, int hidden);
    static SIZE HeaderSize(float k, const wstr& status, const std::vector<wstr>& notes);
    static void PaintHeader(Graphics& g, const RectF& r, float k, const wstr& status, Color dot, const std::vector<wstr>& notes);
    static std::vector<MenuItem> MenuItems(const wstr& xrayHint, const wstr& undoHint, bool undoEnabled, bool paused, bool suspendedOnly);
    void ShowMenu();
private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp);
    void Add();
    void SetIcon(int state, int hidden);
    GlimpseApp* app;
    HWND hwnd = nullptr;
    UINT taskbarCreated = 0;
    HICON hIcon = nullptr;
    wstr iconKey, tip;
    bool added = false, addFailed = false;
};

namespace Toast {
void Show(GlimpseApp* app, const Rc& anchor, const wstr& title, const wstr& subtitle, const wstr& actionText, std::function<void()> action);
SIZE Measure(const wstr& title, const wstr& subtitle, const wstr& actionText, float k);
RectF Paint(Graphics& g, SIZE size, const wstr& title, const wstr& subtitle, const wstr& actionText, bool hover, float k);
void CloseAll();
}

// ------------------------------------------------------------------ settings window

class SettingsWindow;

// ------------------------------------------------------------------ coordinator

class GlimpseApp {
public:
    GlimpseApp();
    ~GlimpseApp();
    static GlimpseApp* current;
    SettingsStore store;
    const Settings& S() const { return store.current; }
    WindowRegistry reg;
    UndoStack undo;
    std::vector<MonitorInfo> mons;
    wstr layoutSig;
    std::unique_ptr<MessageWindow> msg;
    std::unique_ptr<HotkeyManager> hotkeys;
    std::unique_ptr<OverlayManager> overlays;
    std::unique_ptr<XRayView> xray;
    std::unique_ptr<TrayUi> tray;
    SettingsWindow* settingsWindow = nullptr;
    SizeMemory sizes;
    std::unique_ptr<DragState> drag;
    unsigned long long pausedUntil = 0;   // local time in 100 ns units (FILETIME); 0: not paused
    bool sessionActive = true;
    int hiddenCount = 0, guardedCount = 0, overLimit = 0;

    bool Paused() const;
    bool Suspended() const { return Clock::Ms() < suspendedUntil; }
    bool AutoFixAllowed() const { return sessionActive && !Paused() && !Suspended() && S().mode != FixMode::Guide; }
    bool PausedUntilTomorrow() const;
    wstr PausedUntilText() const;
    void Pause(int minutes, bool untilTomorrow);
    void Resume();
    void ShowXRay(bool fromKeyboard);
    void BringForward(const WinPtr& w, bool giveGlimpse);
    void Arrange();
    void UndoLast();
    void UndoGroupNow(const UndoPtr& g);
    void ShowSettings();
    bool SaveSettings(const Settings& s);
    void ApplySettings(bool fromFile);
    void DisplaySignal(const wstr& why);
    void UpdateStatus();
    void ExitApp();
    void SettingsClosed();
    DragScene BuildScene(const DragState& d, const Rc& cur, bool shift, POINT cursor);
private:
    bool HandleMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT& result);
    void OnHotkey();
    void OnSession(int code);
    void OnWinEvent(DWORD evt, HWND h);
    void AddPending(HWND h, int flag);
    void RefreshRegistry();
    void ProcessPending();
    bool DetectRevert(TrackedWindow& w);
    bool CountRevert(TrackedWindow& w, const wstr& what);
    void NewWindow(const WinPtr& w);
    Layout BuildLayout(std::vector<WinPtr>& list, const WinPtr& target, const Rc* targetFrame);
    Outcome RunAndApply(Cause cause, const WinPtr& target, const wstr& edges, const Rc& anchorStart, const Rc* targetFrame, const UndoPtr& group);
    void Apply(const std::vector<WinPtr>& list, const Outcome& o, Cause cause, UndoGroup& g);
    bool RateOk();
    void SuspendAutoFix();
    void OnReadback();
    void BeginDrag(HWND h, bool byButton);
    void CancelDrag();
    void OnDragTick();
    void EndDrag(HWND h);
    void OnDropSettled();
    void Drop(DragState& d);
    void OnDisplaySettled();
    void RestoreLayout(std::map<HWND, Rc> saved);
    void OnSafetyTick();
    void ApplyFirstClick();
    HWND GlimpseOwnerAt(int x, int y);
    void FinishExit();
    void Shutdown();

    LayoutMemory layouts;
    std::unique_ptr<EventIntake> intake;
    std::unique_ptr<HotCorner> corner;
    std::unique_ptr<FirstClickHook> firstClick;
    std::unique_ptr<DragState> dropping;
    bool lastSceneThreatened = false;
    Timer debounce, settle, safety, readback, dragTimer, dropTimer, iniPoll;
    std::vector<std::pair<HWND, int>> pending;   // in arrival order
    std::vector<long long> recentMoves;
    std::vector<UndoItem> toVerify;
    long long suspendedUntil = 0;
    bool disposed = false, exiting = false;
    bool trace = false;
};

namespace App {
int Run();
}

// ------------------------------------------------------------------ settings window

class SettingsWindow {
public:
    SettingsWindow(GlimpseApp* app, const Settings& s, int dpi);
    ~SettingsWindow();
    HWND hwnd = nullptr;
    void Show();
    void ReloadFromApp();
    void Build();
    // preview images: paints the whole page into a bitmap
    Gdiplus::Bitmap* Snapshot();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

wstr FriendlyPattern(const wstr& p);

} // namespace glimpse
