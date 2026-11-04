// Glimpse - keep every window within reach. Native build (Win32, GDI+, DWM).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The same design as Glimpse.cs, reimplemented in C++ for a smaller footprint. The geometry engine
// and resolver must make exactly the decisions of the C# build: both run tests/geometry-cases.txt
// (Glimpse.exe /selftest), and docs/algorithm.md describes them. Keep this source ASCII.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <wtsapi32.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include <cmath>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace glimpse {

typedef std::wstring wstr;
typedef std::vector<std::pair<wstr, wstr>> Args;   // named placeholders: {L"count", L"3"}

extern const wchar_t* const kVersion;

// ------------------------------------------------------------------ program, options

struct Options {
    wstr iniArgument;      // as given, environment variables not expanded
    bool selfTest = false;
    wstr casesPath;
    bool checkLang = false;
    wstr dumpLang, dumpIni;
    wstr render, renderLang;
    bool showSettings = false, exit = false, background = false;
    wstr error;
    static Options Parse(const std::vector<wstr>& args);
};

namespace Program {
extern wstr programDir;     // folder of Glimpse.exe; Glimpse.ini and lang\ live here by default
extern wstr launchPath;     // path of Glimpse.exe (Start with Windows)
extern Options opts;
wstr StartCommandLine();
}

// ------------------------------------------------------------------ utilities

namespace Util {
wstr Widen(const std::string& utf8);
std::string Narrow(const wstr& w);
wstr Inv(long long v);
wstr Trim(const wstr& s);
wstr Lower(const wstr& s);                     // Unicode-aware (CharLowerBuff)
bool IEquals(const wstr& a, const wstr& b);
bool StartsWithI(const wstr& s, const wstr& prefix);
bool EndsWithI(const wstr& s, const wstr& suffix);
wstr Replace(wstr s, const wstr& from, const wstr& to);
std::vector<wstr> Split(const wstr& s, wchar_t sep);
bool ParseBool(const wstr* s, bool fallback);
int ParseInt(const wstr* s, int fallback, int lo, int hi);
bool TryParseInt(const wstr& s, int& v);
int Clamp(int v, int lo, int hi);
int Scale(int px, int dpi);                    // round-half-to-even like .NET Math.Round
int RoundEven(double v);
wstr TitleShape(const wstr& title);
wstr Shorten(const wstr& s, size_t max);
wstr FileName(const wstr& path);
wstr DirName(const wstr& path);
wstr Combine(const wstr& a, const wstr& b);
wstr FullPath(const wstr& p);
wstr ExpandEnv(const wstr& s);
bool FileExists(const wstr& p);
bool DirExists(const wstr& p);
void CreateDirs(const wstr& p);
bool ReadFileBytes(const wstr& p, std::string& out);
bool WriteFileBytes(const wstr& p, const std::string& data);
bool WriteFileAtomic(const wstr& p, const std::string& data);   // old or new content, never half
bool AppendFileBytes(const wstr& p, const std::string& data);
bool FileStamp(const wstr& p, unsigned long long& writeTime, unsigned long long& size);
wstr LocalAppData();
std::string Sha1Hex(const std::string& data);
std::string ResourceText(int id);              // RCDATA resource as bytes
}

namespace Clock { long long Ms(); }

namespace Log {
void Init(bool debug);
void SetDebug(bool on);
bool DebugEnabled();
wstr Folder();
void Info(const wstr& m);
void Warn(const wstr& m);
void Debug(const wstr& m);
void Error(const wstr& m);
}

// ------------------------------------------------------------------ rectangles and intervals

struct Rc {
    int L = 0, T = 0, R = 0, B = 0;
    Rc() {}
    Rc(int l, int t, int r, int b) : L(l), T(t), R(r), B(b) {}
    static Rc XYWH(int x, int y, int w, int h) { return Rc(x, y, x + w, y + h); }
    int W() const { return R - L; }
    int H() const { return B - T; }
    bool IsEmpty() const { return R <= L || B <= T; }
    long long Area() const { return IsEmpty() ? 0 : (long long)W() * H(); }
    int CX() const { return L + W() / 2; }
    int CY() const { return T + H() / 2; }
    bool Intersects(const Rc& o) const { return L < o.R && o.L < R && T < o.B && o.T < B; }
    Rc Intersect(const Rc& o) const;
    Rc Union(const Rc& o) const;
    bool Contains(const Rc& o) const { return o.L >= L && o.T >= T && o.R <= R && o.B <= B; }
    bool Contains(int x, int y) const { return x >= L && x < R && y >= T && y < B; }
    Rc Offset(int dx, int dy) const { return Rc(L + dx, T + dy, R + dx, B + dy); }
    Rc Inflate(int d) const { return Rc(L - d, T - d, R + d, B + d); }
    bool Same(const Rc& o) const { return L == o.L && T == o.T && R == o.R && B == o.B; }
    bool Near(const Rc& o, int tol) const;
    double DistanceTo(int x, int y) const;
    static Rc From(const RECT& r) { return Rc(r.left, r.top, r.right, r.bottom); }
    RECT ToRECT() const { RECT r = { L, T, R, B }; return r; }
    wstr ToString() const;
};

class IntervalSet {
public:
    static IntervalSet Of(int lo, int hi);
    bool IsEmpty() const { return v.empty(); }
    int First() const { return v[0]; }
    void Add(int lo, int hi);
    void Subtract(int lo, int hi);
    IntervalSet Intersect(const IntervalSet& o) const;
    bool Contains(int x) const;
    wstr ToString() const;
private:
    std::vector<int> v;   // lo0, hi0, lo1, hi1, ...
};

// ------------------------------------------------------------------ settings

class IniFile {
public:
    std::vector<wstr> lines;
    int enc = 0;            // 0 UTF-8, 1 UTF-16 LE, 2 UTF-16 BE, 3 ANSI
    bool bom = false;
    wstr newline = L"\r\n";

    static IniFile Parse(const std::string& data);
    static IniFile FromText(const wstr& text);
    wstr Text() const;
    std::string ToBytes() const;
    bool HasSection(const wstr& section) const;
    bool Get(const wstr& section, const wstr& key, wstr& value) const;
    const wstr* GetPtr(const wstr& section, const wstr& key) const;   // valid until the next change
    std::vector<wstr> Sections() const;
    std::vector<std::pair<wstr, wstr>> Entries(const wstr& section) const;
    void Set(const wstr& section, const wstr& key, const wstr& value);
    void RemoveKey(const wstr& section, const wstr& key);
    int RemoveSection(const wstr& section);
    void InsertSection(int at, const wstr& section, const std::vector<std::pair<wstr, wstr>>& entries);
private:
    void SetText(const wstr& text);
    int FindSection(const wstr& section) const;
    int SectionEnd(int header) const;
    int LastKeyLine(int header) const;
    int FindKey(const wstr& section, const wstr& key) const;
    mutable wstr scratch;
};

enum class FixMode { Guide, Assist, Strict };
enum class Corner { Off, TopLeft, TopRight, BottomLeft, BottomRight };

// ECMAScript pattern, case-insensitive (both sides lowered, Unicode-aware)
struct Pattern {
    wstr text;
    std::shared_ptr<std::wregex> re;
    wstr error;
    bool Compile(const wstr& pattern);
    bool Matches(const wstr& s) const;
};

struct MinSizeRule {
    wstr pattern = L".*";
    int minWidth = 200, minHeight = 200;
    wstr note;
    Pattern re;
    bool Compile() { return re.Compile(pattern); }
};

struct Exclusion {
    bool isProcess = false;
    wstr value;
    Pattern re;
    static Exclusion Process(const wstr& name);
    static Exclusion Title(const wstr& pattern);
    bool Matches(const wstr& process, const wstr& title) const;
};

struct Settings {
    FixMode mode = FixMode::Assist;
    int glimpseSize = 50;
    bool placeNewWindows = true, rememberSizes = true;
    int maxWindows = 30;
    wstr language = L"auto";
    bool startWithWindows = false;   // optional, off by default
    bool debugLog = false;
    wstr hotkey = L"Ctrl+Alt+Space";
    Corner hotCorner = Corner::TopLeft;
    bool showMinimized = true;
    std::vector<Exclusion> exclusions;
    std::vector<MinSizeRule> minSizes;
    bool firstClickActivates = false;
    std::vector<wstr> problems;

    static const int SmallSize = 32, MediumSize = 50, LargeSize = 72;
    static wstr DefaultIniText();
    static Settings Defaults();
    static Settings FromIni(const IniFile& ini);
    static bool TryParseCorner(const wstr& v, Corner& c);
    void WriteTo(IniFile& ini) const;
    const MinSizeRule* RuleFor(const wstr& title) const;
    bool IsExcluded(const wstr& process, const wstr& title) const;
    Settings Clone() const;
};

class SettingsStore {
public:
    wstr filePath;
    bool writable = false, usingDefaults = false;
    wstr loadProblem;
    Settings current;
    std::function<void()> changed;

    static wstr ResolvePath(const wstr& iniArgument);
    bool IsNextToProgram() const;
    void Load(const wstr& iniArgument);
    bool Save(const Settings& s);
    void Poll();
private:
    IniFile ini;
    std::string lastHash;
    unsigned long long lastWrite = 0, lastSize = (unsigned long long)-1;
    void EnsureExists();
    bool ReadNow(bool first);
    bool CheckWritable();
};

// ------------------------------------------------------------------ hotkeys

struct HotkeySpec {
    UINT mods = 0;   // MOD_* flags
    UINT vk = 0;
    bool IsValid() const { return vk != 0; }
    static const std::vector<wstr>& Offered();
    static bool IsModifierVk(UINT vk);
    static bool TryParse(const wstr& text, HotkeySpec& spec);
    static HotkeySpec FromKey(UINT vk, bool ctrl, bool shift, bool alt);
    wstr ToString() const;
    std::vector<wstr> DisplayParts() const;
    wstr DisplayText() const;
    bool TypesCharacter(wstr& ch, wstr& layout) const;
};

// ------------------------------------------------------------------ texts

namespace Texts {
void Load(const wstr& language);
wstr Code();
wstr DisplayName();
wstr LangFolder();
const std::map<wstr, wstr>& English();
std::map<wstr, wstr> Parse(const wstr& text);
wstr Unescape(const wstr& v);
wstr T(const wstr& key);
wstr F(const wstr& key, const Args& args);
wstr Fill(wstr text, const Args& args);
wstr P(const wstr& key, long long count, const Args& args = Args());
wstr Category(const wstr& lang, long long n);
std::map<wstr, wstr> Pseudo(const std::map<wstr, wstr>& src);
std::vector<std::pair<wstr, wstr>> Available();
wstr AutoLanguageCode();     // e.g. de-AT
wstr EnglishSource();        // lang\en.txt as built into the program
int DumpBuiltIn(const wstr& file);
}

// ------------------------------------------------------------------ geometry engine (pure)

enum class Edge { Top = 0, Bottom = 1, Left = 2, Right = 3 };
const wchar_t* EdgeName(Edge e);

struct GeoMonitor {
    Rc bounds, work;
    int dpi = 96;
    GeoMonitor(const Rc& b, const Rc& w, int d) : bounds(b), work(w.IsEmpty() ? b : w), dpi(d <= 0 ? 96 : d) {}
};

struct TopEdgeMap {
    std::vector<int> starts, qualities;
    void Add(int relX, int quality) {
        if (!qualities.empty() && qualities.back() == quality) return;
        starts.push_back(relX);
        qualities.push_back(quality);
    }
};

struct GeoWindow {
    wstr id;
    Rc frame;
    bool subject = true, blocker = true, movable = true, resizable = true;
    wstr fixedReason;              // elevated, stubborn, excluded, topmost, hung, limit, maximized, small
    int minW = 0, minH = 0;
    int dpi = 96;
    int captionW = -1, iconW = -1, captionH = -1;
    std::shared_ptr<TopEdgeMap> topMap;
    Rc prevWork;
    void* tag = nullptr;
    long long order = 0;
};

struct Zone {
    Edge edge = Edge::Top;
    Rc rect;
    int quality = 0;
};
typedef std::shared_ptr<Zone> ZonePtr;

namespace Geometry {
int GlimpsePx(int glimpseSize, int dpi);
ZonePtr BestZone(const GeoWindow* w, const Rc& f, int P, const std::vector<Rc>& others, const std::vector<GeoMonitor>& mons);
int MonitorOf(const Rc& r, const std::vector<GeoMonitor>& mons);
Rc ClampInto(const Rc& r, const Rc& work);
Rc GrowToMin(const Rc& f, int minW, int minH, const wstr& draggedEdges, const Rc& work);
bool Reachable(const Rc& r, int P, const std::vector<GeoMonitor>& mons);
Rc Rescue(const Rc& f, const Rc& prevWork, bool resizable, const std::vector<GeoMonitor>& mons);
}

enum class Cause { Check, Drop, New, Restore, AppMove, AppResize, Display, Arrange, GiveGlimpse };

struct Request {
    Cause cause = Cause::Check;
    int target = -1;
    bool fix = true, allowShrink = false, rescue = false, minSizeAll = false, minSizeTarget = false;
    wstr draggedEdges;
    bool victims = false, intruder = false;
    Rc anchorStart;
    std::set<wstr> alreadyMoved;
    static Request For(Cause cause, FixMode mode, int target, bool placeNewWindows);
};

struct Move {
    int index = -1;
    GeoWindow* w = nullptr;
    Rc from, to;
    wstr why;   // minsize, glimpse, place, shrink, cascade, rescue
};

struct Outcome {
    std::vector<Move> moves;
    std::vector<ZonePtr> zones;
    std::vector<int> hidden;
    std::vector<wstr> reasons;
    std::vector<Rc> frames;
    const Move* MoveOf(int index) const;
};

struct Layout {
    std::vector<GeoWindow> windows;
    std::vector<GeoMonitor> monitors;
    int glimpseSize = 50;
    std::vector<Rc> frames;
    void Begin();
    int P(int i) const { return Geometry::GlimpsePx(glimpseSize, windows[i].dpi); }
    Rc WorkOf(const Rc& r) const { return monitors[Geometry::MonitorOf(r, monitors)].work; }
    std::vector<Rc> Others(int i, int moved, const Rc& movedTo, bool skipMoved) const;
    ZonePtr ZoneOf(int i) const;
    ZonePtr ZoneAt(int i, const Rc& frame) const;
    ZonePtr ZoneWith(int j, int moved, const Rc& movedTo, bool absent) const;
};

struct Guide {
    Edge edge = Edge::Top;
    int coord = 0;
    Rc snapped, zone;
    int distance = 0;
};

namespace Resolver {
Outcome Resolve(Layout& L, const Request& q);
bool BestNudge(Layout& L, const Request& q, int i, Rc& best);
std::vector<Guide> Guides(Layout& L, int anchor, int range);
}

// ------------------------------------------------------------------ self-test, language check, previews

namespace SelfTest { int Run(const wstr& casesPath); }
namespace LangCheck { int Run(); }
namespace Previews { int Render(const wstr& folder, const wstr& language); }

// ------------------------------------------------------------------ console output for /selftest

namespace ConsoleOut {
void Attach();
void Write(const wstr& s);
void Line(const wstr& s);
}

} // namespace glimpse
