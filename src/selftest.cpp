// Glimpse native build: /selftest (unit tests and the shared geometry cases) and /checklang.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "glimpse.h"

namespace glimpse {

namespace {

struct FormatError {
    wstr message;
    explicit FormatError(const wstr& m) : message(m) {}
};

int pass = 0, fail = 0;

void Check(bool ok, const wstr& name, const wstr& detail) {
    if (ok) { pass++; ConsoleOut::Line(L"ok - " + name); }
    else { fail++; ConsoleOut::Line(L"not ok - " + name + (detail.empty() ? L"" : L": " + detail)); }
}

void Eq(const wstr& expected, const wstr& actual, const wstr& name) {
    bool ok = expected == actual;
    Check(ok, name, ok ? L"" : L"expected <" + expected + L"> got <" + actual + L">");
}

void Eq(long long expected, long long actual, const wstr& name) {
    bool ok = expected == actual;
    Check(ok, name, ok ? L"" : L"expected <" + Util::Inv(expected) + L"> got <" + Util::Inv(actual) + L">");
}

wstr Join(const std::vector<wstr>& v, const wstr& sep) {
    wstr s;
    for (size_t i = 0; i < v.size(); i++) { if (i) s += sep; s += v[i]; }
    return s;
}

bool EndsWith(const wstr& s, const wstr& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

wstr TrimEnd(const wstr& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) e--;
    return s.substr(0, e);
}

wstr Get(const IniFile& ini, const wstr& section, const wstr& key) {
    wstr v;
    return ini.Get(section, key, v) ? v : L"<null>";
}

wstr ModeName(FixMode m) { return m == FixMode::Guide ? L"Guide" : (m == FixMode::Strict ? L"Strict" : L"Assist"); }

wstr CornerName(Corner c) {
    switch (c) {
        case Corner::TopLeft: return L"TopLeft";
        case Corner::TopRight: return L"TopRight";
        case Corner::BottomLeft: return L"BottomLeft";
        case Corner::BottomRight: return L"BottomRight";
        default: return L"Off";
    }
}

std::string Utf16Le(const wstr& s) {
    std::string b;
    for (wchar_t c : s) { b += (char)(c & 0xFF); b += (char)((c >> 8) & 0xFF); }
    return b;
}

void CheckEnglishFile() {
    const wstr places[] = { Util::Combine(Texts::LangFolder(), L"en.txt"), Util::Combine(Program::programDir, L"..\\lang\\en.txt") };
    for (const wstr& p : places) {
        std::string data;
        if (!Util::FileExists(p) || !Util::ReadFileBytes(p, data)) continue;
        if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) data = data.substr(3);
        std::map<wstr, wstr> file = Texts::Parse(Util::Widen(data));
        const std::map<wstr, wstr>& en = Texts::English();
        std::vector<wstr> diff;
        for (const auto& kv : en) {
            auto it = file.find(kv.first);
            if (it == file.end()) diff.push_back(L"missing " + kv.first);
            else if (it->second != kv.second) diff.push_back(L"differs " + kv.first);
        }
        for (const auto& kv : file) if (!en.count(kv.first)) diff.push_back(L"extra " + kv.first);
        Check(diff.empty(), L"texts: lang\\en.txt matches the built-in English", Join(diff, L", "));
        return;
    }
    ConsoleOut::Line(L"# lang\\en.txt not found next to the program; skipped");
}

void UnitTests() {
    // intervals
    IntervalSet s = IntervalSet::Of(0, 100);
    s.Subtract(10, 20);
    s.Subtract(50, 200);
    Eq(L"[0,9][21,49]", s.ToString(), L"intervals: subtract");
    s.Add(5, 30);
    Eq(L"[0,49]", s.ToString(), L"intervals: add merges");
    Eq(L"[10,20]", IntervalSet::Of(0, 20).Intersect(IntervalSet::Of(10, 40)).ToString(), L"intervals: intersect");

    // INI: encodings, comments, round trip
    const wstr pattern = L"^Objedn\u00e1vka \u010d\\.\\d+";
    wstr text = L"; comment\r\n[Glimpse]\r\nMode=Strict\r\n# other comment\r\nGlimpseSize = 72\r\n\r\n[MinSize1]\r\nTitleRegex=" + pattern + L"\r\nMinWidth=480\r\nMinHeight=360\r\n";
    IniFile ini = IniFile::Parse("\xEF\xBB\xBF" + Util::Narrow(text));
    Eq(1, ini.bom ? 1 : 0, L"ini: UTF-8 BOM detected");
    Eq(L"Strict", Get(ini, L"glimpse", L"mode"), L"ini: keys and sections ignore case");
    Eq(L"72", Get(ini, L"Glimpse", L"GlimpseSize"), L"ini: spaces around =");
    Eq(pattern, Get(ini, L"MinSize1", L"TitleRegex"), L"ini: UTF-8 text with diacritics");
    ini.Set(L"Glimpse", L"Mode", L"Guide");
    ini.Set(L"Glimpse", L"Language", L"cs");
    ini.Set(L"XRay", L"HotCorner", L"Off");
    wstr round = ini.Text();
    Check(round.find(L"; comment") != wstr::npos && round.find(L"# other comment") != wstr::npos, L"ini: comments survive a save", round);
    Check(round.find(L"Language=cs") < round.find(L"[MinSize1]"), L"ini: new key goes into its section", round);
    Check(EndsWith(TrimEnd(round), L"[XRay]\r\nHotCorner=Off"), L"ini: new section appended", round);
    std::string again = ini.ToBytes();
    Eq(0xEF, again.empty() ? -1 : (unsigned char)again[0], L"ini: BOM written back");
    IniFile utf16 = IniFile::Parse(std::string("\xFF\xFE") + Utf16Le(text));
    Eq(pattern, Get(utf16, L"MinSize1", L"TitleRegex"), L"ini: UTF-16 LE with BOM");
    std::string u16 = utf16.ToBytes();
    Check(u16.size() > 2 && (unsigned char)u16[0] == 0xFF && (unsigned char)u16[1] == 0xFE, L"ini: UTF-16 written back as UTF-16", L"");
    IniFile nobom = IniFile::Parse(Util::Narrow(L"[A]\nKey=\u00e4\n"));
    Eq(L"\u00e4", Get(nobom, L"A", L"Key"), L"ini: UTF-8 without BOM");
    Eq(L"\n", nobom.newline, L"ini: keeps LF line ends");

    // settings model
    Settings def = Settings::Defaults();
    Eq(L"Assist", ModeName(def.mode), L"settings: default mode Assist");
    Eq(50, def.glimpseSize, L"settings: default glimpse 50 px");
    Eq(1, (long long)def.minSizes.size(), L"settings: one default minimum-size rule");
    Eq(200, def.minSizes.empty() ? -1 : def.minSizes[0].minWidth, L"settings: default minimum 200 px");
    Eq(L"Ctrl+Alt+Space", def.hotkey, L"settings: default hotkey");
    Eq(L"TopLeft", CornerName(def.hotCorner), L"settings: default hot corner");
    Eq(0, (long long)def.exclusions.size(), L"settings: no exclusions by default");
    Eq(0, def.startWithWindows ? 1 : 0, L"settings: Start with Windows is off by default");
    Eq(0, Settings::FromIni(IniFile::FromText(L"[Glimpse]\r\n")).startWithWindows ? 1 : 0, L"settings: Start with Windows stays off when the key is missing");
    Eq(0, (long long)def.problems.size(), L"settings: defaults have no problems");

    Settings st = Settings::Defaults();
    st.mode = FixMode::Strict;
    st.glimpseSize = 72;
    st.language = L"sk";
    st.hotCorner = Corner::BottomRight;
    st.hotkey = L"Ctrl+Shift+Alt+Space";
    MinSizeRule order;
    order.pattern = L"^Order #\\d+";
    order.minWidth = 480;
    order.minHeight = 360;
    order.note = L"ERP order forms";
    order.Compile();
    st.minSizes.insert(st.minSizes.begin(), order);
    st.exclusions.push_back(Exclusion::Process(L"teams.exe"));
    st.exclusions.push_back(Exclusion::Title(L"^Calculator$"));
    IniFile ini2 = IniFile::FromText(Settings::DefaultIniText());
    st.WriteTo(ini2);
    Settings back = Settings::FromIni(IniFile::FromText(ini2.Text()));
    Eq(L"Strict", ModeName(back.mode), L"settings: round trip mode");
    Eq(72, back.glimpseSize, L"settings: round trip glimpse size");
    Eq(L"sk", back.language, L"settings: round trip language");
    Eq(L"BottomRight", CornerName(back.hotCorner), L"settings: round trip hot corner");
    Eq(L"Ctrl+Shift+Alt+Space", back.hotkey, L"settings: round trip hotkey");
    Eq(2, (long long)back.minSizes.size(), L"settings: round trip rule count");
    if (back.minSizes.size() == 2) {
        Eq(L"^Order #\\d+", back.minSizes[0].pattern, L"settings: rules keep their order");
        Eq(L".*", back.minSizes[1].pattern, L"settings: catch-all rule stays last");
        Eq(L"ERP order forms", back.minSizes[0].note, L"settings: rule note");
    }
    Eq(2, (long long)back.exclusions.size(), L"settings: round trip exclusions");
    wstr saved = ini2.Text();
    Check(saved.find(L"; minimum sizes: rules are tried") != wstr::npos, L"settings: comments kept on save", L"");
    Check(saved.find(L"[MinSize2]") < saved.find(L"[Experimental]"), L"settings: rules stay before [Experimental]", saved);
    IniFile twice = IniFile::FromText(saved);
    back.WriteTo(twice);
    Eq(saved, twice.Text(), L"settings: saving twice changes nothing");

    const MinSizeRule* r = back.RuleFor(L"order #4711 - ERP");
    Eq(L"^Order #\\d+", r ? r->pattern : L"<none>", L"rules: case-insensitive, first match wins");
    r = back.RuleFor(L"Inbox - Outlook");
    Eq(L".*", r ? r->pattern : L"<none>", L"rules: catch-all");
    Check(back.IsExcluded(L"Teams.exe", L"Chat"), L"exclusions: process name, any case", L"");
    Check(back.IsExcluded(L"calc.exe", L"Calculator"), L"exclusions: title pattern", L"");
    Check(!back.IsExcluded(L"notepad.exe", L"Calculator notes"), L"exclusions: anchored pattern", L"");
    Settings bad = Settings::FromIni(IniFile::FromText(L"[MinSize1]\nTitleRegex=([unclosed\nMinWidth=300\n[MinSize2]\nTitleRegex=.*\nMinWidth=200\nMinHeight=200\n"));
    Check(bad.problems.size() == 1, L"rules: an invalid pattern is reported", Join(bad.problems, L"; "));
    r = bad.RuleFor(L"anything");
    Eq(L".*", r ? r->pattern : L"<none>", L"rules: an invalid pattern disables only its own rule");
    MinSizeRule ecma;
    ecma.pattern = L"^\\w+$";
    ecma.Compile();
    Check(!ecma.re.Matches(L"Objedn\u00e1vka"), L"rules: ECMAScript \\w is ASCII only, as in .NET", L"");
    Pattern ci;
    ci.Compile(L"^\u010cesk\u00fd [A-Z]+$");
    Check(ci.Matches(L"\u010desk\u00fd ABC") && ci.Matches(L"\u010cESK\u00dd abc"), L"rules: ignore case beyond ASCII", L"");

    // hotkeys
    HotkeySpec hk;
    Check(HotkeySpec::TryParse(L"Ctrl+Alt+Space", hk) && hk.mods == (MOD_CONTROL | MOD_ALT) && hk.vk == 0x20, L"hotkey: Ctrl+Alt+Space", L"");
    Check(HotkeySpec::TryParse(L"alt + shift + ctrl + enter", hk) && hk.ToString() == L"Ctrl+Shift+Alt+Enter", L"hotkey: canonical order", hk.ToString());
    Check(HotkeySpec::TryParse(L"Pause", hk) && hk.vk == 0x13 && hk.mods == 0, L"hotkey: Pause alone", L"");
    Check(!HotkeySpec::TryParse(L"Ctrl+Alt", hk), L"hotkey: modifiers only is invalid", L"");
    Check(!HotkeySpec::TryParse(L"Ctrl+Alt+Foo", hk), L"hotkey: unknown key is invalid", L"");
    Check(HotkeySpec::TryParse(L"Ctrl+Alt+F12", hk) && hk.vk == 0x7B, L"hotkey: F-keys", L"");
    Eq(L"Ctrl+Alt+Space", HotkeySpec::FromKey(0x20, true, false, true).ToString(), L"hotkey: from recorded keys");

    // texts and plural rules
    Eq(L"one", Texts::Category(L"cs", 1), L"plural cs 1");
    Eq(L"few", Texts::Category(L"cs", 3), L"plural cs 3");
    Eq(L"other", Texts::Category(L"sk", 5), L"plural sk 5");
    Eq(L"few", Texts::Category(L"pl", 22), L"plural pl 22");
    Eq(L"many", Texts::Category(L"pl", 25), L"plural pl 25");
    Eq(L"one", Texts::Category(L"ru", 21), L"plural ru 21");
    Eq(L"other", Texts::Category(L"de", 0), L"plural de 0");
    Eq(L"one", Texts::Category(L"fr", 0), L"plural fr 0");
    Texts::Load(L"en");
    Eq(L"3 windows", Texts::P(L"windows", 3), L"texts: English plural");
    Eq(L"1 window", Texts::P(L"windows", 1), L"texts: English singular");
    Eq(L"moves 80 px on drop", Texts::F(L"drag.movesPx", { { L"px", Util::Inv(80) } }), L"texts: named placeholder");
    Eq(L"no.such.key", Texts::T(L"no.such.key"), L"texts: unknown key shows the key");
    std::map<wstr, wstr> pseudo = Texts::Pseudo(Texts::English());
    wstr pw = pseudo[L"windows.other"];
    auto ew = Texts::English().find(L"windows.other");
    Check(ew != Texts::English().end() && pw.find(L"{count}") != wstr::npos && pw.size() >= ew->second.size() * 1.3,
          L"texts: pseudo-language keeps placeholders, 40% longer", pw);
    CheckEnglishFile();

    // options
    Options o = Options::Parse({ L"/ini:%APPDATA%\\Glimpse\\Glimpse.ini" });
    Eq(L"%APPDATA%\\Glimpse\\Glimpse.ini", o.iniArgument, L"options: /ini:path");
    o = Options::Parse({ L"-Ini", L"C:\\x y\\G.ini", L"/settings" });
    Check(o.iniArgument == L"C:\\x y\\G.ini" && o.showSettings, L"options: -Ini path /settings", L"");
    Check(!Options::Parse({ L"/bogus" }).error.empty(), L"options: unknown option", L"");
    Check(Options::Parse({ L"/background" }).background, L"options: /background", L"");

    // misc
    Eq(L"Order ## \u2014 ERP", Util::TitleShape(L"Order #4711 \u2014 ERP"), L"size memory: title shape");
    Eq(75, Geometry::GlimpsePx(50, 144), L"glimpse: 50 px at 150% is 75 px");
    Eq(300, Util::Scale(200, 144), L"minimum size: 200 px at 150% is 300 px");
    Eq(2, Util::RoundEven(2.5), L"rounding: half to even like .NET");
    Eq(4, Util::RoundEven(3.5), L"rounding: half to even, odd below");
}

// ------------------------------------------------------------------ geometry cases

struct Case {
    wstr name;
    Layout L;
    std::map<wstr, int> ids;
    std::vector<std::vector<wstr>> windows;
    FixMode mode = FixMode::Assist;
    bool place = true;
    std::vector<wstr> action;
    std::vector<std::vector<wstr>> expects;
    std::vector<wstr> already;
    int line = 0;
};

int ParseIntStrict(const wstr& s) {
    int v;
    if (!Util::TryParseInt(s, v) || Util::Trim(s) != s) throw FormatError(L"not a number: " + s);
    return v;
}

const wstr& At(const std::vector<wstr>& t, size_t i) {
    if (i >= t.size()) throw FormatError(L"missing value");
    return t[i];
}

Rc R(const std::vector<wstr>& t, size_t i) {
    return Rc(ParseIntStrict(At(t, i)), ParseIntStrict(At(t, i + 1)), ParseIntStrict(At(t, i + 2)), ParseIntStrict(At(t, i + 3)));
}

Rc RList(const wstr& s) {
    std::vector<wstr> p = Util::Split(s, L',');
    if (p.size() < 4) throw FormatError(L"need l,t,r,b: " + s);
    return Rc(ParseIntStrict(p[0]), ParseIntStrict(p[1]), ParseIntStrict(p[2]), ParseIntStrict(p[3]));
}

std::vector<wstr> Tokens(const wstr& line) {
    std::vector<wstr> t;
    wstr cur;
    for (wchar_t c : line) {
        if (c == L' ' || c == L'\t') { if (!cur.empty()) { t.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) t.push_back(cur);
    return t;
}

wstr AsciiLower(wstr s) {
    for (wchar_t& c : s) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return s;
}

wstr AsciiUpper(wstr s) {
    for (wchar_t& c : s) if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
    return s;
}

bool Starts(const wstr& s, const wstr& p) { return s.compare(0, p.size(), p) == 0; }

void RunCase(Case& c) {
    if (c.L.monitors.empty()) c.L.monitors.push_back(GeoMonitor(Rc(0, 0, 1920, 1080), Rc(0, 0, 1920, 1040), 96));
    for (const std::vector<wstr>& t : c.windows) {
        GeoWindow w;
        w.id = At(t, 1);
        w.frame = R(t, 2);
        w.dpi = c.L.monitors[Geometry::MonitorOf(w.frame, c.L.monitors)].dpi;
        for (size_t i = 6; i < t.size(); i++) {
            wstr f = AsciiLower(t[i]);
            if (f == L"elevated" || f == L"stubborn" || f == L"excluded" || f == L"hung") { w.movable = false; w.fixedReason = f; }
            else if (f == L"topmost") { w.movable = false; w.subject = false; w.fixedReason = f; }
            else if (f == L"maximized" || f == L"notcounted") { w.subject = false; w.blocker = false; w.movable = false; w.fixedReason = f; }
            else if (f == L"noresize") w.resizable = false;
            else if (Starts(f, L"min=")) {
                std::vector<wstr> wh = Util::Split(f.substr(4), L'x');
                if (wh.size() < 2) throw FormatError(L"min=WxH: " + t[i]);
                w.minW = Util::Scale(ParseIntStrict(wh[0]), w.dpi);
                w.minH = Util::Scale(ParseIntStrict(wh[1]), w.dpi);
            }
            else if (Starts(f, L"caption=")) w.captionW = Util::Scale(ParseIntStrict(f.substr(8)), w.dpi);
            else if (Starts(f, L"icon=")) w.iconW = Util::Scale(ParseIntStrict(f.substr(5)), w.dpi);
            else if (Starts(f, L"prev=")) w.prevWork = RList(f.substr(5));
            else throw FormatError(L"unknown window flag " + t[i]);
        }
        c.ids[w.id] = (int)c.L.windows.size();
        c.L.windows.push_back(w);
    }
    c.L.Begin();

    Request check;
    check.fix = false;
    Outcome before = Resolver::Resolve(c.L, check);
    c.L.Begin();
    Outcome after = before;
    std::vector<Guide> guides;
    bool haveGuides = false;
    std::vector<wstr> a = c.action.empty() ? std::vector<wstr>{ L"action", L"check" } : c.action;
    wstr act = AsciiLower(At(a, 1));
    auto id = [&](const wstr& name) -> int {
        auto it = c.ids.find(name);
        if (it == c.ids.end()) throw FormatError(L"unknown window " + name);
        return it->second;
    };
    if (act == L"guides") {
        int idx = id(At(a, 2));
        guides = Resolver::Guides(c.L, idx, ParseIntStrict(At(a, 3)));
        haveGuides = true;
    } else if (act != L"check") {
        Cause cause;
        if (act == L"drop") cause = Cause::Drop;
        else if (act == L"new") cause = Cause::New;
        else if (act == L"restore") cause = Cause::Restore;
        else if (act == L"appmove") cause = Cause::AppMove;
        else if (act == L"appresize") cause = Cause::AppResize;
        else if (act == L"display") cause = Cause::Display;
        else if (act == L"arrange") cause = Cause::Arrange;
        else if (act == L"glimpse") cause = Cause::GiveGlimpse;
        else throw FormatError(L"unknown action " + act);
        int target = a.size() > 2 && c.ids.count(a[2]) ? c.ids[a[2]] : -1;
        Request q = Request::For(cause, c.mode, target, c.place);
        for (size_t i = 3; i < a.size(); i++) {
            if (Starts(a[i], L"edges=")) q.draggedEdges = AsciiUpper(a[i].substr(6));
            else if (Starts(a[i], L"from=")) q.anchorStart = RList(a[i].substr(5));
        }
        for (const wstr& s : c.already) q.alreadyMoved.insert(s);
        after = Resolver::Resolve(c.L, q);
    }

    std::vector<wstr> problems;
    for (const std::vector<wstr>& e : c.expects) {
        wstr what = AsciiLower(At(e, 1));
        wstr err;
        if (what == L"zone") {
            const Outcome& src = act == L"check" ? before : after;
            ZonePtr z = src.zones[id(At(e, 2))];
            wstr got = z ? EdgeName(z->edge) : L"none";
            if (got != AsciiLower(At(e, 3))) err = L"zone of " + e[2] + L": expected " + e[3] + L" got " + got + (z ? L" " + z->rect.ToString() : L"");
            else if (e.size() > 4 && z && z->quality != ParseIntStrict(e[4])) err = L"zone quality of " + e[2] + L": expected " + e[4] + L" got " + Util::Inv(z->quality);
        } else if (what == L"free") {
            int ix = id(At(e, 2));
            if (!after.zones[ix]) err = e[2] + L" has no free glimpse (" + after.reasons[ix] + L")";
        } else if (what == L"hidden") {
            int ix = id(At(e, 2));
            if (after.zones[ix]) err = e[2] + L" still has a glimpse " + after.zones[ix]->rect.ToString();
            else if (e.size() > 3 && after.reasons[ix] != e[3]) err = e[2] + L" hidden because " + after.reasons[ix] + L", expected " + e[3];
        } else if (what == L"at") {
            Rc want = R(e, 3), got = after.frames[id(At(e, 2))];
            if (!want.Same(got)) err = e[2] + L" at " + got.ToString() + L", expected " + want.ToString();
        } else if (what == L"moved") {
            if (!after.MoveOf(id(At(e, 2)))) err = e[2] + L" was not moved";
        } else if (what == L"unchanged") {
            const Move* m = after.MoveOf(id(At(e, 2)));
            if (m) err = e[2] + L" moved to " + m->to.ToString();
        } else if (what == L"moves") {
            int want = ParseIntStrict(At(e, 2));
            if ((int)after.moves.size() != want) {
                wstr sb;
                for (const Move& m : after.moves) sb += L" " + m.w->id + L"->" + m.to.ToString() + L"(" + m.why + L")";
                err = L"expected " + Util::Inv(want) + L" moves, got " + Util::Inv((long long)after.moves.size()) + L":" + sb;
            }
        } else if (what == L"why") {
            const Move* m = after.MoveOf(id(At(e, 2)));
            if (!m || m->why != At(e, 3)) err = e[2] + L" moved because " + (m ? m->why : L"-") + L", expected " + e[3];
        } else if (what == L"guide") {
            bool found = false;
            int coord = ParseIntStrict(At(e, 3));
            for (const Guide& g : guides) if (EdgeName(g.edge) == AsciiLower(e[2]) && g.coord == coord) found = true;
            if (!found) {
                wstr sb;
                for (const Guide& g : guides) {
                    wstr n = EdgeName(g.edge);
                    n[0] = (wchar_t)(n[0] - L'a' + L'A');
                    sb += L" " + n + L"@" + Util::Inv(g.coord);
                }
                err = L"no " + e[2] + L" guide at " + e[3] + L"; got" + sb;
            }
        } else if (what == L"guides") {
            int want = ParseIntStrict(At(e, 2));
            int got = haveGuides ? (int)guides.size() : 0;
            if (got != want) err = L"expected " + Util::Inv(want) + L" guides, got " + Util::Inv(got);
        } else err = L"unknown expectation " + what;
        if (!err.empty()) problems.push_back(err);
    }
    Check(problems.empty(), L"case " + c.name, Join(problems, L"; "));
}

void RunCases(const wstr& path) {
    ConsoleOut::Line(L"# cases: " + path);
    std::string data;
    if (!Util::ReadFileBytes(path, data)) throw FormatError(L"cannot read " + path);
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) data = data.substr(3);
    std::vector<wstr> lines = Util::Split(Util::Replace(Util::Widen(data), L"\r\n", L"\n"), L'\n');
    std::unique_ptr<Case> c;
    int count = 0;
    for (size_t ln = 0; ln < lines.size(); ln++) {
        wstr line = lines[ln];
        size_t hash = line.find(L'#');
        if (hash != wstr::npos) line = line.substr(0, hash);
        line = Util::Trim(line);
        if (line.empty()) continue;
        std::vector<wstr> t = Tokens(line);
        wstr kw = AsciiLower(t[0]);
        try {
            if (kw == L"case") {
                c.reset(new Case());
                c->name = Util::Trim(line.substr(4));
                c->line = (int)ln + 1;
                continue;
            }
            if (!c) throw FormatError(L"'" + kw + L"' outside a case");
            if (kw == L"monitor") {
                Rc b = R(t, 1), w = b;
                int dpi = 96;
                for (size_t i = 5; i < t.size(); i++) {
                    if (t[i] == L"work") { w = R(t, i + 1); i += 4; }
                    else if (t[i] == L"dpi") { dpi = ParseIntStrict(At(t, i + 1)); i++; }
                }
                c->L.monitors.push_back(GeoMonitor(b, w, dpi));
            } else if (kw == L"glimpse") c->L.glimpseSize = ParseIntStrict(At(t, 1));
            else if (kw == L"mode") {
                wstr m = AsciiLower(At(t, 1));
                if (m == L"guide") c->mode = FixMode::Guide;
                else if (m == L"assist") c->mode = FixMode::Assist;
                else if (m == L"strict") c->mode = FixMode::Strict;
                else throw FormatError(L"unknown mode " + t[1]);
            }
            else if (kw == L"place") c->place = At(t, 1) == L"1" || t[1] == L"on";
            else if (kw == L"window") c->windows.push_back(t);
            else if (kw == L"already") c->already.push_back(At(t, 1));
            else if (kw == L"action") c->action = t;
            else if (kw == L"expect") c->expects.push_back(t);
            else if (kw == L"end") {
                RunCase(*c);
                count++;
                c.reset();
            } else throw FormatError(L"unknown keyword '" + kw + L"'");
        } catch (const FormatError& ex) {
            Check(false, L"cases line " + Util::Inv((long long)ln + 1), ex.message);
            c.reset();
        } catch (const std::exception& ex) {
            Check(false, L"cases line " + Util::Inv((long long)ln + 1), Util::Widen(ex.what()));
            c.reset();
        }
    }
    Check(count > 0, L"geometry cases found", Util::Inv(count) + L" cases");
}

wstr FindCases() {
    const wchar_t* rel[] = { L"tests\\geometry-cases.txt", L"geometry-cases.txt", L"..\\tests\\geometry-cases.txt",
                             L"..\\..\\tests\\geometry-cases.txt", L"..\\..\\..\\tests\\geometry-cases.txt" };
    for (const wchar_t* r : rel) {
        wstr p = Util::Combine(Program::programDir, r);
        if (Util::FileExists(p)) return Util::FullPath(p);
    }
    return wstr();
}

const wchar_t* Arch() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return L"ARM64";
#elif defined(_M_X64) || defined(__x86_64__)
    return L"x64";
#elif defined(_M_IX86) || defined(__i386__)
    return L"x86";
#else
    return L"unknown";
#endif
}

} // namespace

namespace SelfTest {
int Run(const wstr& casesPath) {
    ConsoleOut::Line(wstr(L"Glimpse ") + kVersion + L" self-test (native, " + Util::Inv((long long)sizeof(void*) * 8) + L"-bit, " + Arch() + L")");
    try { UnitTests(); }
    catch (const FormatError& ex) { Check(false, L"unit tests crashed", ex.message); }
    catch (const std::exception& ex) { Check(false, L"unit tests crashed", Util::Widen(ex.what())); }
    wstr path = casesPath.empty() ? FindCases() : casesPath;
    if (path.empty()) Check(false, L"geometry cases", L"tests\\geometry-cases.txt not found; pass /selftest:<file>");
    else {
        try { RunCases(path); }
        catch (const FormatError& ex) { Check(false, L"geometry cases crashed", ex.message); }
        catch (const std::exception& ex) { Check(false, L"geometry cases crashed", Util::Widen(ex.what())); }
    }
    ConsoleOut::Line(wstr(fail == 0 ? L"PASS" : L"FAIL") + L": " + Util::Inv(pass) + L" passed, " + Util::Inv(fail) + L" failed");
    return fail == 0 ? 0 : 1;
}
}

// ------------------------------------------------------------------ /checklang

namespace LangCheck {

static const wchar_t* Forms[] = { L".zero", L".one", L".two", L".few", L".many", L".other" };

static bool PluralBase(const wstr& key, wstr& base) {
    for (const wchar_t* f : Forms) {
        wstr s(f);
        if (EndsWith(key, s)) { base = key.substr(0, key.size() - s.size()); return true; }
    }
    return false;
}

static bool HasAnyForm(const std::map<wstr, wstr>& d, const wstr& base) {
    for (const wchar_t* f : Forms) if (d.count(base + f)) return true;
    return false;
}

static wstr Placeholders(const wstr& s) {
    std::vector<wstr> list;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'{') continue;
        size_t j = i + 1;
        while (j < s.size() && s[j] >= L'a' && s[j] <= L'z') j++;
        if (j < s.size() && s[j] == L'}' && j > i + 1) {
            wstr m = s.substr(i, j - i + 1);
            if (std::find(list.begin(), list.end(), m) == list.end()) list.push_back(m);
        }
    }
    std::sort(list.begin(), list.end());
    return Join(list, L"");
}

int Run() {
    const std::map<wstr, wstr>& en = Texts::English();
    int problems = 0;
    wstr folder = Texts::LangFolder();
    if (!Util::DirExists(folder)) { ConsoleOut::Line(L"no lang folder next to the program: " + folder); return 1; }
    std::vector<wstr> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Util::Combine(folder, L"*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end(), [](const wstr& a, const wstr& b) {
        return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_LESS_THAN;
    });
    for (const wstr& f : files) {
        wstr code = f.substr(0, f.size() - 4);
        std::string data;
        Util::ReadFileBytes(Util::Combine(folder, f), data);
        if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) data = data.substr(3);
        std::map<wstr, wstr> d = Texts::Parse(Util::Widen(data));
        std::vector<wstr> lines;
        for (const auto& kv : en) {
            if (!kv.first.empty() && kv.first[0] == L'_') continue;
            wstr base;
            if (PluralBase(kv.first, base)) {
                // a plural key needs .other and the categories its language uses
                if (!HasAnyForm(d, base)) lines.push_back(L"missing   " + kv.first);
                continue;
            }
            auto it = d.find(kv.first);
            if (it == d.end()) { lines.push_back(L"missing   " + kv.first); continue; }
            wstr pe = Placeholders(kv.second), pv = Placeholders(it->second);
            if (pe != pv) lines.push_back(L"outdated  " + kv.first + L" (placeholders " + pv + L", English has " + pe + L")");
        }
        for (const auto& kv : d) {
            if (!kv.first.empty() && kv.first[0] == L'_') continue;
            wstr base;
            bool known = en.count(kv.first) || (PluralBase(kv.first, base) && HasAnyForm(en, base));
            if (!known) lines.push_back(L"obsolete  " + kv.first);
        }
        // plural categories this language needs
        std::set<wstr> cats;
        for (int n = 0; n < 200; n++) cats.insert(Texts::Category(code, n));
        for (const auto& kv : en) {
            wstr base;
            if (!PluralBase(kv.first, base) || !EndsWith(kv.first, L".other")) continue;
            for (const wstr& cat : cats)
                if (!d.count(base + L"." + cat) && HasAnyForm(d, base)) lines.push_back(L"missing   " + base + L"." + cat + L" (plural form for " + code + L")");
        }
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        ConsoleOut::Line(code + L": " + (lines.empty() ? wstr(L"complete") : Util::Inv((long long)lines.size()) + L" problem(s)"));
        for (const wstr& l : lines) ConsoleOut::Line(L"  " + l);
        if (!Util::IEquals(code, L"en")) problems += (int)lines.size();
    }
    return problems == 0 ? 0 : 1;
}

} // namespace LangCheck
} // namespace glimpse
