// Glimpse native build: utilities, log, Glimpse.ini, settings, hotkeys, texts.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "glimpse.h"
#include "resource.h"
#include <wincrypt.h>
#include <cstdio>

namespace glimpse {

const wchar_t* const kVersion = L"1.0.0";

// ================================================================== utilities

namespace Util {

wstr Widen(const std::string& s) {
    if (s.empty()) return wstr();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstr w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const wstr& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

wstr Inv(long long v) { return std::to_wstring(v); }

// the whitespace set of .NET's char.IsWhiteSpace, so Trim matches the C# build
static bool IsWhite(wchar_t c) {
    return c == L' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

wstr Trim(const wstr& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsWhite(s[a])) a++;
    while (b > a && IsWhite(s[b - 1])) b--;
    return s.substr(a, b - a);
}

wstr Lower(const wstr& s) {
    wstr r = s;
    if (!r.empty()) CharLowerBuffW(&r[0], (DWORD)r.size());
    return r;
}

bool IEquals(const wstr& a, const wstr& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool StartsWithI(const wstr& s, const wstr& p) { return s.size() >= p.size() && IEquals(s.substr(0, p.size()), p); }
bool EndsWithI(const wstr& s, const wstr& p) { return s.size() >= p.size() && IEquals(s.substr(s.size() - p.size()), p); }

wstr Replace(wstr s, const wstr& from, const wstr& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != wstr::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}

std::vector<wstr> Split(const wstr& s, wchar_t sep) {
    std::vector<wstr> r;
    size_t a = 0;
    for (size_t i = 0; i <= s.size(); i++)
        if (i == s.size() || s[i] == sep) { r.push_back(s.substr(a, i - a)); a = i + 1; }
    return r;
}

bool ParseBool(const wstr* s, bool fallback) {
    if (!s) return fallback;
    wstr v = Lower(Trim(*s));
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") return true;
    if (v == L"0" || v == L"false" || v == L"no" || v == L"off") return false;
    return fallback;
}

bool TryParseInt(const wstr& s, int& v) {
    wstr t = Trim(s);
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (t[0] == L'-' || t[0] == L'+') { neg = t[0] == L'-'; i = 1; }
    if (i >= t.size()) return false;
    long long acc = 0;
    for (; i < t.size(); i++) {
        if (t[i] < L'0' || t[i] > L'9') return false;
        acc = acc * 10 + (t[i] - L'0');
        if (acc > 2147483648LL) return false;
    }
    if (neg) acc = -acc;
    if (acc > 2147483647LL || acc < -2147483648LL) return false;
    v = (int)acc;
    return true;
}

int ParseInt(const wstr* s, int fallback, int lo, int hi) {
    int v;
    if (!s || !TryParseInt(*s, v)) return fallback;
    return std::max(lo, std::min(hi, v));
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int RoundEven(double v) { return (int)std::nearbyint(v); }
int Scale(int px, int dpi) { return RoundEven(px * dpi / 96.0); }

wstr TitleShape(const wstr& title) {
    wstr r;
    bool inDigits = false;
    for (wchar_t c : title) {
        bool digit = c >= L'0' && c <= L'9';
        if (!digit) { WORD t = 0; GetStringTypeW(CT_CTYPE1, &c, 1, &t); digit = (t & C1_DIGIT) != 0; }
        if (digit) { if (!inDigits) r += L'#'; inDigits = true; }
        else { r += c; inDigits = false; }
    }
    return r.size() > 80 ? r.substr(0, 80) : r;
}

wstr Shorten(const wstr& s, size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, max > 0 ? max - 1 : 0) + L"\u2026";
}

wstr FileName(const wstr& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == wstr::npos ? p : p.substr(i + 1);
}

wstr DirName(const wstr& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == wstr::npos ? wstr() : p.substr(0, i);
}

wstr Combine(const wstr& a, const wstr& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

wstr FullPath(const wstr& p) {
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(p.c_str(), 4096, buf, nullptr);
    return n > 0 && n < 4096 ? wstr(buf, n) : p;
}

wstr ExpandEnv(const wstr& s) {
    wchar_t buf[8192];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 8192);
    return n > 0 && n <= 8192 ? wstr(buf) : s;
}

bool FileExists(const wstr& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const wstr& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void CreateDirs(const wstr& p) {
    if (p.empty() || DirExists(p)) return;
    CreateDirs(DirName(p));
    CreateDirectoryW(p.c_str(), nullptr);
}

bool ReadFileBytes(const wstr& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1LL << 30);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ok = size.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)size.QuadPart, &read, nullptr) && read == (DWORD)size.QuadPart);
    }
    CloseHandle(h);
    return ok;
}

bool WriteFileBytes(const wstr& p, const std::string& data) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size());
    CloseHandle(h);
    return ok;
}

bool AppendFileBytes(const wstr& p, const std::string& data) {
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

bool FileStamp(const wstr& p, unsigned long long& writeTime, unsigned long long& size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
    writeTime = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return true;
}

wstr LocalAppData() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) return wstr(buf);
    n = GetTempPathW(MAX_PATH * 2, buf);
    return wstr(buf, n);
}

std::string Sha1Hex(const std::string& data) {
    std::string out;
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
            CryptHashData(hash, (const BYTE*)data.data(), (DWORD)data.size(), 0);
            BYTE h[20];
            DWORD len = 20;
            if (CryptGetHashParam(hash, HP_HASHVAL, h, &len, 0)) {
                static const char* hex = "0123456789abcdef";
                for (DWORD i = 0; i < len; i++) { out += hex[h[i] >> 4]; out += hex[h[i] & 15]; }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
    }
    if (out.empty()) {
        // fallback: FNV-1a, enough to notice a change
        unsigned long long x = 1469598103934665603ULL;
        for (unsigned char c : data) { x ^= c; x *= 1099511628211ULL; }
        char buf[32];
        snprintf(buf, sizeof buf, "%016llx", x);
        out = buf;
    }
    return out;
}

std::string ResourceText(int id) {
    HMODULE m = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!r) return std::string();
    HGLOBAL g = LoadResource(m, r);
    DWORD size = SizeofResource(m, r);
    const char* p = g ? (const char*)LockResource(g) : nullptr;
    if (!p) return std::string();
    std::string s(p, size);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s = s.substr(3);
    return s;
}

} // namespace Util

namespace Clock {
long long Ms() {
    static LARGE_INTEGER freq = {};
    static LARGE_INTEGER start = {};
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - start.QuadPart) * 1000 / freq.QuadPart;
}
}

// ================================================================== log

namespace Log {
static wstr path;
static bool debug = false;
static wstr session = L"?";
static CRITICAL_SECTION gate;
static bool gateInit = false;

wstr Folder() { return Util::Combine(Util::LocalAppData(), L"Glimpse"); }
bool DebugEnabled() { return debug; }
void SetDebug(bool on) { debug = on; }

void Init(bool debugLevel) {
    if (!gateInit) { InitializeCriticalSection(&gate); gateInit = true; }
    debug = debugLevel;
    wstr dir = Folder();
    Util::CreateDirs(dir);
    path = Util::Combine(dir, L"Glimpse.log");
    DWORD s = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &s)) session = Util::Inv(s);
}

static void Write(const wchar_t* level, const wstr& msg) {
    if (path.empty() || !gateInit) return;
    EnterCriticalSection(&gate);
    unsigned long long t, size;
    if (Util::FileStamp(path, t, size) && size > 1024 * 1024) {
        wstr old = path + L".1";
        DeleteFileW(old.c_str());
        MoveFileW(path.c_str(), old.c_str());
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    wstr line = wstr(stamp) + L" [" + session + L"] " + level + L" " + msg + L"\r\n";
    Util::AppendFileBytes(path, Util::Narrow(line));
    LeaveCriticalSection(&gate);
}

void Info(const wstr& m) { Write(L"INFO ", m); }
void Warn(const wstr& m) { Write(L"WARN ", m); }
void Debug(const wstr& m) { if (debug) Write(L"DEBUG", m); }
void Error(const wstr& m) { Write(L"ERROR", m); }
}

// ================================================================== rectangles, intervals

Rc Rc::Intersect(const Rc& o) const {
    Rc r(std::max(L, o.L), std::max(T, o.T), std::min(R, o.R), std::min(B, o.B));
    return r.IsEmpty() ? Rc(0, 0, 0, 0) : r;
}

Rc Rc::Union(const Rc& o) const {
    if (IsEmpty()) return o;
    if (o.IsEmpty()) return *this;
    return Rc(std::min(L, o.L), std::min(T, o.T), std::max(R, o.R), std::max(B, o.B));
}

bool Rc::Near(const Rc& o, int tol) const {
    return std::abs(L - o.L) <= tol && std::abs(T - o.T) <= tol && std::abs(R - o.R) <= tol && std::abs(B - o.B) <= tol;
}

double Rc::DistanceTo(int x, int y) const {
    int dx = x < L ? L - x : (x >= R ? x - R + 1 : 0);
    int dy = y < T ? T - y : (y >= B ? y - B + 1 : 0);
    return std::sqrt((double)dx * dx + (double)dy * dy);
}

wstr Rc::ToString() const { return Util::Inv(L) + L"," + Util::Inv(T) + L"," + Util::Inv(R) + L"," + Util::Inv(B); }

IntervalSet IntervalSet::Of(int lo, int hi) {
    IntervalSet s;
    if (lo <= hi) { s.v.push_back(lo); s.v.push_back(hi); }
    return s;
}

void IntervalSet::Add(int lo, int hi) {
    if (lo > hi) return;
    std::vector<int> n;
    n.reserve(v.size() + 2);
    bool placed = false;
    for (size_t i = 0; i < v.size(); i += 2) {
        int a = v[i], b = v[i + 1];
        if ((long long)b < (long long)lo - 1) { n.push_back(a); n.push_back(b); }
        else if ((long long)a > (long long)hi + 1) {
            if (!placed) { n.push_back(lo); n.push_back(hi); placed = true; }
            n.push_back(a); n.push_back(b);
        }
        else { lo = std::min(lo, a); hi = std::max(hi, b); }
    }
    if (!placed) { n.push_back(lo); n.push_back(hi); }
    v.swap(n);
}

void IntervalSet::Subtract(int lo, int hi) {
    if (lo > hi || v.empty()) return;
    std::vector<int> n;
    n.reserve(v.size() + 2);
    for (size_t i = 0; i < v.size(); i += 2) {
        int a = v[i], b = v[i + 1];
        if (b < lo || a > hi) { n.push_back(a); n.push_back(b); continue; }
        if (a < lo) { n.push_back(a); n.push_back(lo - 1); }
        if (b > hi) { n.push_back(hi + 1); n.push_back(b); }
    }
    v.swap(n);
}

IntervalSet IntervalSet::Intersect(const IntervalSet& o) const {
    IntervalSet r;
    size_t i = 0, j = 0;
    while (i < v.size() && j < o.v.size()) {
        int lo = std::max(v[i], o.v[j]), hi = std::min(v[i + 1], o.v[j + 1]);
        if (lo <= hi) { r.v.push_back(lo); r.v.push_back(hi); }
        if (v[i + 1] < o.v[j + 1]) i += 2; else j += 2;
    }
    return r;
}

bool IntervalSet::Contains(int x) const {
    for (size_t i = 0; i < v.size(); i += 2) if (x >= v[i] && x <= v[i + 1]) return true;
    return false;
}

wstr IntervalSet::ToString() const {
    wstr s;
    for (size_t i = 0; i < v.size(); i += 2) s += L"[" + Util::Inv(v[i]) + L"," + Util::Inv(v[i + 1]) + L"]";
    return s;
}

// ================================================================== Glimpse.ini

static bool LooksUtf16(const std::string& d, bool bigEndian) {
    if (d.size() < 4 || d.size() % 2 != 0) return false;
    int zeros = 0, n = (int)std::min<size_t>(d.size(), 400);
    for (int i = bigEndian ? 0 : 1; i < n; i += 2) if (d[i] == 0) zeros++;
    return zeros > n / 2 * 0.8;
}

static bool ValidUtf8(const std::string& d) {
    return d.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, d.data(), (int)d.size(), nullptr, 0) > 0;
}

static wstr DecodeUtf16(const std::string& d, size_t skip, bool be) {
    wstr w;
    for (size_t i = skip; i + 1 < d.size(); i += 2) {
        unsigned char a = (unsigned char)d[i], b = (unsigned char)d[i + 1];
        w += (wchar_t)(be ? (a << 8 | b) : (b << 8 | a));
    }
    return w;
}

IniFile IniFile::Parse(const std::string& d) {
    IniFile f;
    wstr text;
    const unsigned char* u = (const unsigned char*)d.data();
    if (d.size() >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) { f.enc = 0; f.bom = true; text = Util::Widen(d.substr(3)); }
    else if (d.size() >= 2 && u[0] == 0xFF && u[1] == 0xFE) { f.enc = 1; f.bom = true; text = DecodeUtf16(d, 2, false); }
    else if (d.size() >= 2 && u[0] == 0xFE && u[1] == 0xFF) { f.enc = 2; f.bom = true; text = DecodeUtf16(d, 2, true); }
    else if (LooksUtf16(d, false)) { f.enc = 1; text = DecodeUtf16(d, 0, false); }
    else if (LooksUtf16(d, true)) { f.enc = 2; text = DecodeUtf16(d, 0, true); }
    else if (ValidUtf8(d)) { f.enc = 0; text = Util::Widen(d); }
    else {
        f.enc = 3;
        int n = MultiByteToWideChar(CP_ACP, 0, d.data(), (int)d.size(), nullptr, 0);
        text.resize(n);
        if (n) MultiByteToWideChar(CP_ACP, 0, d.data(), (int)d.size(), &text[0], n);
    }
    f.SetText(text);
    return f;
}

IniFile IniFile::FromText(const wstr& text) {
    IniFile f;
    f.SetText(text);
    return f;
}

void IniFile::SetText(const wstr& text) {
    lines.clear();
    newline = text.find(L"\r\n") != wstr::npos || text.find(L'\n') == wstr::npos ? L"\r\n" : L"\n";
    std::vector<wstr> parts = Util::Split(Util::Replace(text, L"\r\n", L"\n"), L'\n');
    size_t count = parts.size();
    if (count > 0 && parts[count - 1].empty()) count--;
    for (size_t i = 0; i < count; i++) {
        wstr l = parts[i];
        while (!l.empty() && l.back() == L'\r') l.pop_back();
        lines.push_back(l);
    }
}

wstr IniFile::Text() const {
    wstr s;
    for (const wstr& l : lines) { s += l; s += newline; }
    return s;
}

std::string IniFile::ToBytes() const {
    wstr t = Text();
    std::string body;
    if (enc == 1 || enc == 2) {
        if (bom) { body += enc == 1 ? "\xFF\xFE" : "\xFE\xFF"; }
        for (wchar_t c : t) {
            char lo = (char)(c & 0xFF), hi = (char)((c >> 8) & 0xFF);
            if (enc == 1) { body += lo; body += hi; } else { body += hi; body += lo; }
        }
        return body;
    }
    if (enc == 3) {
        int n = WideCharToMultiByte(CP_ACP, 0, t.data(), (int)t.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        if (n) WideCharToMultiByte(CP_ACP, 0, t.data(), (int)t.size(), &s[0], n, nullptr, nullptr);
        return s;
    }
    if (bom) body = "\xEF\xBB\xBF";
    return body + Util::Narrow(t);
}

static bool IsComment(const wstr& t) { return t.empty() || t[0] == L';' || t[0] == L'#'; }

static bool SectionName(const wstr& line, wstr& name) {
    wstr t = Util::Trim(line);
    if (t.size() < 2 || t[0] != L'[') return false;
    size_t e = t.find(L']');
    if (e == wstr::npos || e == 0) return false;
    name = Util::Trim(t.substr(1, e - 1));
    return true;
}

static bool SplitKey(const wstr& line, wstr& key, wstr& value) {
    wstr t = Util::Trim(line);
    if (IsComment(t) || t[0] == L'[') return false;
    size_t eq = t.find(L'=');
    if (eq == wstr::npos || eq == 0) return false;
    key = Util::Trim(t.substr(0, eq));
    value = Util::Trim(t.substr(eq + 1));
    return !key.empty();
}

int IniFile::FindSection(const wstr& section) const {
    wstr s;
    for (size_t i = 0; i < lines.size(); i++) if (SectionName(lines[i], s) && Util::IEquals(s, section)) return (int)i;
    return -1;
}

int IniFile::SectionEnd(int header) const {
    wstr s;
    for (size_t i = header + 1; i < lines.size(); i++) if (SectionName(lines[i], s)) return (int)i;
    return (int)lines.size();
}

int IniFile::LastKeyLine(int header) const {
    int end = SectionEnd(header), last = header;
    wstr k, v;
    for (int i = header + 1; i < end; i++) if (SplitKey(lines[i], k, v)) last = i;
    return last;
}

int IniFile::FindKey(const wstr& section, const wstr& key) const {
    int h = FindSection(section);
    if (h < 0) return -1;
    int end = SectionEnd(h);
    wstr k, v;
    for (int i = h + 1; i < end; i++) if (SplitKey(lines[i], k, v) && Util::IEquals(k, key)) return i;
    return -1;
}

bool IniFile::HasSection(const wstr& section) const { return FindSection(section) >= 0; }

bool IniFile::Get(const wstr& section, const wstr& key, wstr& value) const {
    int i = FindKey(section, key);
    if (i < 0) return false;
    wstr k;
    SplitKey(lines[i], k, value);
    return true;
}

const wstr* IniFile::GetPtr(const wstr& section, const wstr& key) const {
    return Get(section, key, scratch) ? &scratch : nullptr;
}

std::vector<wstr> IniFile::Sections() const {
    std::vector<wstr> r;
    wstr s;
    for (const wstr& l : lines) if (SectionName(l, s)) r.push_back(s);
    return r;
}

std::vector<std::pair<wstr, wstr>> IniFile::Entries(const wstr& section) const {
    std::vector<std::pair<wstr, wstr>> r;
    int h = FindSection(section);
    if (h < 0) return r;
    int end = SectionEnd(h);
    wstr k, v;
    for (int i = h + 1; i < end; i++) if (SplitKey(lines[i], k, v)) r.push_back(std::make_pair(k, v));
    return r;
}

void IniFile::Set(const wstr& section, const wstr& key, const wstr& value) {
    int i = FindKey(section, key);
    if (i >= 0) {
        wstr k, v;
        SplitKey(lines[i], k, v);
        lines[i] = k + L"=" + value;
        return;
    }
    int h = FindSection(section);
    if (h < 0) {
        if (!lines.empty() && !Util::Trim(lines.back()).empty()) lines.push_back(L"");
        lines.push_back(L"[" + section + L"]");
        lines.push_back(key + L"=" + value);
        return;
    }
    lines.insert(lines.begin() + LastKeyLine(h) + 1, key + L"=" + value);
}

void IniFile::RemoveKey(const wstr& section, const wstr& key) {
    int i = FindKey(section, key);
    if (i >= 0) lines.erase(lines.begin() + i);
}

int IniFile::RemoveSection(const wstr& section) {
    int h = FindSection(section);
    if (h < 0) return -1;
    int last = LastKeyLine(h);
    lines.erase(lines.begin() + h, lines.begin() + last + 1);
    if (h < (int)lines.size() && h > 0 && Util::Trim(lines[h]).empty() && Util::Trim(lines[h - 1]).empty()) lines.erase(lines.begin() + h);
    return h;
}

void IniFile::InsertSection(int at, const wstr& section, const std::vector<std::pair<wstr, wstr>>& entries) {
    std::vector<wstr> block;
    block.push_back(L"[" + section + L"]");
    for (const auto& kv : entries) block.push_back(kv.first + L"=" + kv.second);
    if (at < 0 || at >= (int)lines.size()) {
        if (!lines.empty() && !Util::Trim(lines.back()).empty()) lines.push_back(L"");
        lines.insert(lines.end(), block.begin(), block.end());
        return;
    }
    if (!Util::Trim(lines[at]).empty()) block.push_back(L"");
    lines.insert(lines.begin() + at, block.begin(), block.end());
}

// ================================================================== patterns

// Case-insensitive like .NET (RegexOptions.ECMAScript | IgnoreCase): lower the literal characters of
// the pattern and the text with the same Unicode-aware function. ECMAScript character classes are
// ASCII-only in .NET; std::regex would follow the locale, so \w \d \s are spelled out.
static wstr PreparePattern(const wstr& p) {
    wstr out;
    bool inClass = false;
    for (size_t i = 0; i < p.size(); i++) {
        wchar_t c = p[i];
        if (c == L'\\' && i + 1 < p.size()) {
            wchar_t n = p[i + 1];
            const wchar_t* inside = nullptr;
            const wchar_t* outside = nullptr;
            switch (n) {
                case L'w': inside = L"a-zA-Z0-9_"; outside = L"[a-zA-Z0-9_]"; break;
                case L'd': inside = L"0-9"; outside = L"[0-9]"; break;
                case L's': inside = L" \\f\\n\\r\\t\\v"; outside = L"[ \\f\\n\\r\\t\\v]"; break;
                case L'W': outside = L"[^a-zA-Z0-9_]"; break;
                case L'D': outside = L"[^0-9]"; break;
                case L'S': outside = L"[^ \\f\\n\\r\\t\\v]"; break;
            }
            if (inClass && inside) { out += inside; i++; continue; }
            if (!inClass && outside) { out += outside; i++; continue; }
            out += c;
            out += n;
            i++;
            continue;
        }
        if (c == L'[' && !inClass) inClass = true;
        else if (c == L']' && inClass) inClass = false;
        wchar_t lc = c;
        CharLowerBuffW(&lc, 1);
        out += lc;
    }
    return out;
}

bool Pattern::Compile(const wstr& pattern) {
    text = pattern;
    re.reset();
    error.clear();
    try {
        re = std::make_shared<std::wregex>(PreparePattern(pattern), std::regex_constants::ECMAScript);
        return true;
    } catch (const std::regex_error& e) {
        error = Util::Widen(e.what());
        if (error.empty()) error = L"invalid pattern";
        return false;
    }
}

bool Pattern::Matches(const wstr& s) const {
    if (!re) return false;
    try { return std::regex_search(Util::Lower(s), *re); }
    catch (...) { return false; }
}

Exclusion Exclusion::Process(const wstr& name) {
    Exclusion e;
    e.isProcess = true;
    e.value = Util::Trim(name);
    return e;
}

Exclusion Exclusion::Title(const wstr& pattern) {
    Exclusion e;
    e.isProcess = false;
    e.value = pattern;
    e.re.Compile(pattern);
    return e;
}

bool Exclusion::Matches(const wstr& process, const wstr& title) const {
    if (isProcess) {
        if (process.empty()) return false;
        wstr v = Util::EndsWithI(value, L".exe") ? value : value + L".exe";
        return Util::IEquals(process, v);
    }
    return re.Matches(title);
}

// ================================================================== settings model

wstr Settings::DefaultIniText() {
    static wstr text;
    if (text.empty()) {
        text = Util::Widen(Util::ResourceText(IDR_DEFAULT_INI));
        // the resource is the repository's Glimpse.ini; normalise line ends to CRLF
        text = Util::Replace(Util::Replace(text, L"\r\n", L"\n"), L"\n", L"\r\n");
    }
    return text;
}

Settings Settings::Defaults() { return FromIni(IniFile::FromText(DefaultIniText())); }

bool Settings::TryParseCorner(const wstr& v, Corner& c) {
    wstr s = Util::Lower(Util::Trim(v));
    if (s == L"topleft") { c = Corner::TopLeft; return true; }
    if (s == L"topright") { c = Corner::TopRight; return true; }
    if (s == L"bottomleft") { c = Corner::BottomLeft; return true; }
    if (s == L"bottomright") { c = Corner::BottomRight; return true; }
    if (s == L"off" || s.empty() || s == L"none") { c = Corner::Off; return true; }
    c = Corner::Off;
    return false;
}

static const wchar_t* CornerKey(Corner c) {
    switch (c) {
        case Corner::TopLeft: return L"TopLeft";
        case Corner::TopRight: return L"TopRight";
        case Corner::BottomLeft: return L"BottomLeft";
        case Corner::BottomRight: return L"BottomRight";
        default: return L"Off";
    }
}

static const wchar_t* ModeKey(FixMode m) {
    return m == FixMode::Guide ? L"Guide" : (m == FixMode::Strict ? L"Strict" : L"Assist");
}

Settings Settings::FromIni(const IniFile& ini) {
    Settings s;
    wstr v;
    if (ini.Get(L"Glimpse", L"Mode", v)) {
        wstr m = Util::Lower(Util::Trim(v));
        if (m == L"guide") s.mode = FixMode::Guide;
        else if (m == L"assist") s.mode = FixMode::Assist;
        else if (m == L"strict") s.mode = FixMode::Strict;
        else s.problems.push_back(L"Mode=" + v);
    }
    s.glimpseSize = Util::ParseInt(ini.GetPtr(L"Glimpse", L"GlimpseSize"), 50, 16, 400);
    s.placeNewWindows = Util::ParseBool(ini.GetPtr(L"Glimpse", L"PlaceNewWindows"), true);
    s.rememberSizes = Util::ParseBool(ini.GetPtr(L"Glimpse", L"RememberSizes"), true);
    s.maxWindows = Util::ParseInt(ini.GetPtr(L"Glimpse", L"MaxWindows"), 30, 1, 500);
    s.language = ini.Get(L"Glimpse", L"Language", v) && !v.empty() ? Util::Trim(v) : L"auto";
    s.startWithWindows = Util::ParseBool(ini.GetPtr(L"Glimpse", L"StartWithWindows"), false);
    s.debugLog = ini.Get(L"Glimpse", L"LogLevel", v) && Util::IEquals(Util::Trim(v), L"debug");

    if (ini.Get(L"XRay", L"Hotkey", v)) {
        HotkeySpec hk;
        wstr t = Util::Trim(v);
        if (t.empty() || Util::IEquals(t, L"off")) s.hotkey = L"";
        else if (HotkeySpec::TryParse(v, hk)) s.hotkey = hk.ToString();
        else s.problems.push_back(L"Hotkey=" + v);
    }
    if (ini.Get(L"XRay", L"HotCorner", v)) {
        Corner c;
        if (TryParseCorner(v, c)) s.hotCorner = c; else s.problems.push_back(L"HotCorner=" + v);
    }
    s.showMinimized = Util::ParseBool(ini.GetPtr(L"XRay", L"ShowMinimized"), true);

    for (const auto& kv : ini.Entries(L"Exclude")) {
        if (kv.second.empty()) continue;
        if (Util::StartsWithI(kv.first, L"Process")) s.exclusions.push_back(Exclusion::Process(kv.second));
        else if (Util::StartsWithI(kv.first, L"Title")) {
            Exclusion e = Exclusion::Title(kv.second);
            if (!e.re.error.empty()) s.problems.push_back(L"[Exclude] " + kv.first + L": " + e.re.error);
            s.exclusions.push_back(e);
        }
    }

    std::map<int, wstr> numbered;
    for (const wstr& sec : ini.Sections()) {
        if (!Util::StartsWithI(sec, L"MinSize")) continue;
        int n;
        if (Util::TryParseInt(sec.substr(7), n) && sec.substr(7) == Util::Inv(n) && !numbered.count(n)) numbered[n] = sec;
    }
    for (const auto& kv : numbered) {
        MinSizeRule r;
        r.pattern = ini.Get(kv.second, L"TitleRegex", v) ? v : L".*";
        r.minWidth = Util::ParseInt(ini.GetPtr(kv.second, L"MinWidth"), 0, 0, 10000);
        r.minHeight = Util::ParseInt(ini.GetPtr(kv.second, L"MinHeight"), 0, 0, 10000);
        r.note = ini.Get(kv.second, L"Note", v) ? v : L"";
        if (!r.Compile()) s.problems.push_back(L"[" + kv.second + L"] TitleRegex: " + r.re.error);
        s.minSizes.push_back(r);
    }
    s.firstClickActivates = Util::ParseBool(ini.GetPtr(L"Experimental", L"FirstClickActivates"), false);
    return s;
}

static std::vector<std::pair<wstr, wstr>> RuleEntries(const MinSizeRule& r) {
    std::vector<std::pair<wstr, wstr>> e;
    e.push_back(std::make_pair(L"TitleRegex", r.pattern));
    e.push_back(std::make_pair(L"MinWidth", Util::Inv(r.minWidth)));
    e.push_back(std::make_pair(L"MinHeight", Util::Inv(r.minHeight)));
    if (!r.note.empty()) e.push_back(std::make_pair(L"Note", r.note));
    return e;
}

static int IndexOfSectionWithComments(const IniFile& ini, const wstr& section) {
    for (size_t i = 0; i < ini.lines.size(); i++) {
        wstr t = Util::Trim(ini.lines[i]);
        if (t.size() >= 2 && t[0] == L'[' && t.back() == L']' && Util::IEquals(Util::Trim(t.substr(1, t.size() - 2)), section)) {
            int j = (int)i;
            while (j > 0) {
                wstr p = Util::Trim(ini.lines[j - 1]);
                if (!p.empty() && (p[0] == L';' || p[0] == L'#')) j--; else break;
            }
            return j;
        }
    }
    return -1;
}

void Settings::WriteTo(IniFile& ini) const {
    ini.Set(L"Glimpse", L"Mode", ModeKey(mode));
    ini.Set(L"Glimpse", L"GlimpseSize", Util::Inv(glimpseSize));
    ini.Set(L"Glimpse", L"PlaceNewWindows", placeNewWindows ? L"1" : L"0");
    ini.Set(L"Glimpse", L"RememberSizes", rememberSizes ? L"1" : L"0");
    ini.Set(L"Glimpse", L"MaxWindows", Util::Inv(maxWindows));
    ini.Set(L"Glimpse", L"Language", language);
    ini.Set(L"Glimpse", L"StartWithWindows", startWithWindows ? L"1" : L"0");
    ini.Set(L"Glimpse", L"LogLevel", debugLog ? L"Debug" : L"Info");
    ini.Set(L"XRay", L"Hotkey", hotkey.empty() ? L"Off" : hotkey);
    ini.Set(L"XRay", L"HotCorner", CornerKey(hotCorner));
    ini.Set(L"XRay", L"ShowMinimized", showMinimized ? L"1" : L"0");

    for (const auto& kv : ini.Entries(L"Exclude")) ini.RemoveKey(L"Exclude", kv.first);
    int np = 0, nt = 0;
    for (const Exclusion& e : exclusions) {
        if (e.isProcess) ini.Set(L"Exclude", L"Process" + Util::Inv(++np), e.value);
        else ini.Set(L"Exclude", L"Title" + Util::Inv(++nt), e.value);
    }

    int at = -1;
    for (const wstr& sec : ini.Sections()) {
        if (!Util::StartsWithI(sec, L"MinSize")) continue;
        int n;
        if (!Util::TryParseInt(sec.substr(7), n)) continue;
        int pos = ini.RemoveSection(sec);
        if (at < 0 || pos < at) at = pos;
    }
    if (at < 0) at = IndexOfSectionWithComments(ini, L"Experimental");
    if (at >= 0) {
        for (int i = (int)minSizes.size() - 1; i >= 0; i--) ini.InsertSection(at, L"MinSize" + Util::Inv(i + 1), RuleEntries(minSizes[i]));
    } else {
        for (size_t i = 0; i < minSizes.size(); i++) ini.InsertSection(-1, L"MinSize" + Util::Inv(i + 1), RuleEntries(minSizes[i]));
    }
    ini.Set(L"Experimental", L"FirstClickActivates", firstClickActivates ? L"1" : L"0");
}

const MinSizeRule* Settings::RuleFor(const wstr& title) const {
    for (const MinSizeRule& r : minSizes) if (r.re.Matches(title)) return &r;
    return nullptr;
}

bool Settings::IsExcluded(const wstr& process, const wstr& title) const {
    for (const Exclusion& e : exclusions) if (e.Matches(process, title)) return true;
    return false;
}

Settings Settings::Clone() const {
    IniFile ini = IniFile::FromText(DefaultIniText());
    WriteTo(ini);
    return FromIni(ini);
}

// ================================================================== settings store

wstr SettingsStore::ResolvePath(const wstr& arg) {
    if (arg.empty()) return Util::Combine(Program::programDir, L"Glimpse.ini");
    wstr p = Util::ExpandEnv(Util::Trim(arg));
    if (!p.empty() && p.front() == L'"') p = p.substr(1);
    if (!p.empty() && p.back() == L'"') p.pop_back();
    return Util::FullPath(p);
}

bool SettingsStore::IsNextToProgram() const {
    return Util::IEquals(Util::FullPath(Util::DirName(filePath)), Util::FullPath(Program::programDir));
}

void SettingsStore::Load(const wstr& arg) {
    filePath = ResolvePath(arg);
    EnsureExists();
    ReadNow(true);
}

void SettingsStore::EnsureExists() {
    if (Util::FileExists(filePath)) return;
    wstr dir = Util::DirName(filePath);
    if (!dir.empty()) Util::CreateDirs(dir);
    wstr tmpl = Util::Combine(Program::programDir, L"Glimpse.ini");
    std::string data;
    bool ok;
    if (Util::FileExists(tmpl) && !Util::IEquals(Util::FullPath(tmpl), Util::FullPath(filePath)) && Util::ReadFileBytes(tmpl, data))
        ok = Util::WriteFileBytes(filePath, data);
    else
        ok = Util::WriteFileBytes(filePath, "\xEF\xBB\xBF" + Util::Narrow(Settings::DefaultIniText()));
    if (ok) Log::Info(L"created " + filePath); else Log::Warn(L"cannot create " + filePath);
}

bool SettingsStore::CheckWritable() {
    HANDLE h = CreateFileW(filePath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool SettingsStore::ReadNow(bool first) {
    std::string data;
    bool have = false;
    unsigned long long t, size;
    if (Util::FileStamp(filePath, t, size)) {
        lastWrite = t;
        lastSize = size;
        have = Util::ReadFileBytes(filePath, data);
        if (!have) loadProblem = L"cannot read";
    } else loadProblem = L"not found";
    if (!have) {
        if (!first && !usingDefaults) return false;
        ini = IniFile::FromText(Settings::DefaultIniText());
        current = Settings::Defaults();
        usingDefaults = true;
        writable = false;
        Log::Warn(L"cannot read " + filePath + L" (" + loadProblem + L"), using built-in defaults");
        return true;
    }
    std::string hash = Util::Sha1Hex(data);
    if (!first && hash == lastHash) return false;
    lastHash = hash;
    ini = IniFile::Parse(data);
    current = Settings::FromIni(ini);
    usingDefaults = false;
    loadProblem.clear();
    writable = CheckWritable();
    for (const wstr& p : current.problems) Log::Warn(L"Glimpse.ini: " + p);
    return true;
}

bool SettingsStore::Save(const Settings& s) {
    if (!writable) return false;
    s.WriteTo(ini);
    std::string data = ini.ToBytes();
    if (!Util::WriteFileBytes(filePath, data)) {
        Log::Warn(L"cannot write " + filePath);
        writable = false;
        return false;
    }
    lastHash = Util::Sha1Hex(data);
    Util::FileStamp(filePath, lastWrite, lastSize);
    current = Settings::FromIni(ini);
    return true;
}

void SettingsStore::Poll() {
    unsigned long long t, size;
    bool exists = Util::FileStamp(filePath, t, size);
    if (exists && t == lastWrite && size == lastSize && !usingDefaults) return;
    if (!exists && usingDefaults) return;
    if (ReadNow(false)) {
        Log::Info(L"Glimpse.ini changed, settings reloaded");
        if (changed) changed();
    }
}

// ================================================================== hotkeys

static std::map<wstr, UINT>& KeyNames() {
    static std::map<wstr, UINT> n;
    return n;
}
static std::map<UINT, wstr>& CanonicalNames() {
    static std::map<UINT, wstr> c;
    return c;
}

static void InitKeys() {
    if (!KeyNames().empty()) return;
    auto& n = KeyNames();
    auto& c = CanonicalNames();
    auto add = [&](const wstr& name, UINT vk) { n[Util::Lower(name)] = vk; if (!c.count(vk)) c[vk] = name; };
    auto alias = [&](const wstr& name, UINT vk) { n[Util::Lower(name)] = vk; };
    add(L"Space", 0x20); add(L"Enter", 0x0D); alias(L"Return", 0x0D); add(L"Pause", 0x13); alias(L"Break", 0x13);
    add(L"Tab", 0x09); add(L"Esc", 0x1B); alias(L"Escape", 0x1B); add(L"Backspace", 0x08);
    add(L"Insert", 0x2D); alias(L"Ins", 0x2D); add(L"Delete", 0x2E); alias(L"Del", 0x2E); add(L"Home", 0x24); add(L"End", 0x23);
    add(L"PageUp", 0x21); alias(L"PgUp", 0x21); add(L"PageDown", 0x22); alias(L"PgDn", 0x22);
    add(L"Left", 0x25); add(L"Up", 0x26); add(L"Right", 0x27); add(L"Down", 0x28);
    add(L"PrintScreen", 0x2C); add(L"ScrollLock", 0x91); add(L"Apps", 0x5D);
    for (UINT i = 0; i < 24; i++) add(L"F" + Util::Inv(i + 1), 0x70 + i);
    for (wchar_t ch = L'A'; ch <= L'Z'; ch++) add(wstr(1, ch), ch);
    for (wchar_t ch = L'0'; ch <= L'9'; ch++) add(wstr(1, ch), ch);
    for (UINT i = 0; i < 10; i++) add(L"NumPad" + Util::Inv(i), 0x60 + i);
    add(L"Multiply", 0x6A); add(L"Add", 0x6B); alias(L"Plus", 0x6B); add(L"Subtract", 0x6D); alias(L"Minus", 0x6D);
    add(L"Decimal", 0x6E); add(L"Divide", 0x6F);
    add(L"Oem1", 0xBA); add(L"OemPlus", 0xBB); add(L"OemComma", 0xBC); add(L"OemMinus", 0xBD); add(L"OemPeriod", 0xBE);
    add(L"Oem2", 0xBF); add(L"Oem3", 0xC0); add(L"Oem4", 0xDB); add(L"Oem5", 0xDC); add(L"Oem6", 0xDD); add(L"Oem7", 0xDE);
    add(L"Oem102", 0xE2);
}

const std::vector<wstr>& HotkeySpec::Offered() {
    static std::vector<wstr> o = { L"Ctrl+Alt+Space", L"Ctrl+Shift+Alt+Space", L"Ctrl+Alt+Enter", L"Pause" };
    return o;
}

bool HotkeySpec::IsModifierVk(UINT vk) {
    return vk == 0x10 || vk == 0x11 || vk == 0x12 || (vk >= 0xA0 && vk <= 0xA5) || vk == 0x5B || vk == 0x5C;
}

bool HotkeySpec::TryParse(const wstr& text, HotkeySpec& spec) {
    InitKeys();
    spec = HotkeySpec();
    if (text.empty()) return false;
    std::vector<wstr> parts = Util::Split(text, L'+');
    for (size_t i = 0; i < parts.size(); i++) {
        wstr p = Util::Trim(parts[i]);
        if (p.empty()) {
            if (i == parts.size() - 1 && spec.vk == 0) { spec.vk = 0xBB; continue; }
            continue;
        }
        wstr l = Util::Lower(p);
        if (l == L"ctrl" || l == L"control" || l == L"strg") { spec.mods |= MOD_CONTROL; continue; }
        if (l == L"alt") { spec.mods |= MOD_ALT; continue; }
        if (l == L"shift") { spec.mods |= MOD_SHIFT; continue; }
        if (l == L"win" || l == L"windows") { spec.mods |= MOD_WIN; continue; }
        if (spec.vk != 0) return false;
        auto it = KeyNames().find(l);
        if (it != KeyNames().end()) { spec.vk = it->second; continue; }
        if (Util::StartsWithI(p, L"VK")) {
            wstr hex = p.substr(2);
            while (!hex.empty() && hex[0] == L'_') hex = hex.substr(1);
            unsigned v = 0;
            bool ok = !hex.empty();
            for (wchar_t ch : hex) {
                int d = ch >= L'0' && ch <= L'9' ? ch - L'0' : (ch >= L'a' && ch <= L'f' ? ch - L'a' + 10 : (ch >= L'A' && ch <= L'F' ? ch - L'A' + 10 : -1));
                if (d < 0 || v > 0xFFF) { ok = false; break; }
                v = v * 16 + d;
            }
            if (ok && v > 0 && v < 0xFF) { spec.vk = v; continue; }
        }
        return false;
    }
    return spec.vk != 0 && !IsModifierVk(spec.vk);
}

HotkeySpec HotkeySpec::FromKey(UINT vk, bool ctrl, bool shift, bool alt) {
    InitKeys();
    HotkeySpec s;
    if (ctrl) s.mods |= MOD_CONTROL;
    if (shift) s.mods |= MOD_SHIFT;
    if (alt) s.mods |= MOD_ALT;
    if (!IsModifierVk(vk)) s.vk = vk;
    return s;
}

static wstr CanonicalKeyName(UINT vk) {
    InitKeys();
    auto it = CanonicalNames().find(vk);
    if (it != CanonicalNames().end()) return it->second;
    wchar_t buf[16];
    swprintf(buf, 16, L"VK%02X", vk);
    return buf;
}

wstr HotkeySpec::ToString() const {
    if (vk == 0) return L"";
    wstr s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_WIN) s += L"Win+";
    return s + CanonicalKeyName(vk);
}

static wstr WindowsKeyName(UINT vk, const wstr& fallback) {
    UINT scan;
    bool extended = false;
    if (vk == 0x13) scan = 0x45;
    else {
        scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, GetKeyboardLayout(0));
        switch (vk) {
            case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
            case 0x2D: case 0x2E: case 0x6F: case 0x5D: extended = true; break;
        }
    }
    if (scan == 0) return fallback;
    LONG lp = (LONG)(scan << 16) | (extended ? (1 << 24) : 0);
    wchar_t buf[64];
    int len = GetKeyNameTextW(lp, buf, 64);
    if (len <= 0) return fallback;
    wstr s(buf, len);
    wstr upper = s;
    CharUpperBuffW(&upper[0], (DWORD)upper.size());
    if (s.size() > 1 && upper == s) s = s.substr(0, 1) + Util::Lower(s.substr(1));
    return s;
}

std::vector<wstr> HotkeySpec::DisplayParts() const {
    std::vector<wstr> r;
    if (vk == 0) return r;
    if (mods & MOD_CONTROL) r.push_back(WindowsKeyName(0x11, L"Ctrl"));
    if (mods & MOD_SHIFT) r.push_back(WindowsKeyName(0x10, L"Shift"));
    if (mods & MOD_ALT) r.push_back(WindowsKeyName(0x12, L"Alt"));
    if (mods & MOD_WIN) r.push_back(L"Win");
    r.push_back(WindowsKeyName(vk, CanonicalKeyName(vk)));
    return r;
}

wstr HotkeySpec::DisplayText() const {
    wstr s;
    for (const wstr& p : DisplayParts()) { if (!s.empty()) s += L"+"; s += p; }
    return s;
}

bool HotkeySpec::TypesCharacter(wstr& ch, wstr& layout) const {
    if (vk == 0) return false;
    int n = GetKeyboardLayoutList(0, nullptr);
    if (n <= 0) return false;
    std::vector<HKL> list(n);
    GetKeyboardLayoutList(n, list.data());
    for (HKL hkl : list) {
        BYTE state[256] = { 0 };
        if (mods & MOD_CONTROL) { state[0x11] = 0x80; state[0xA2] = 0x80; }
        if (mods & MOD_ALT) { state[0x12] = 0x80; state[0xA4] = 0x80; }
        if (mods & MOD_SHIFT) { state[0x10] = 0x80; state[0xA0] = 0x80; }
        UINT scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl);
        wchar_t buf[8] = { 0 };
        int r = ToUnicodeEx(vk, scan, state, buf, 8, 0x4, hkl);
        if (r > 0 && buf[0] >= 0x20 && buf[0] != 0x7F) {
            ch = wstr(buf, r);
            LCID lcid = MAKELCID(LOWORD((ULONG_PTR)hkl), SORT_DEFAULT);
            wchar_t name[LOCALE_NAME_MAX_LENGTH];
            wchar_t display[128];
            if (LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0) > 0 && GetLocaleInfoEx(name, LOCALE_SLOCALIZEDDISPLAYNAME, display, 128) > 0) layout = display;
            else { wchar_t hex[16]; swprintf(hex, 16, L"0x%04X", (unsigned)LOWORD((ULONG_PTR)hkl)); layout = hex; }
            return true;
        }
    }
    return false;
}

// ================================================================== texts

namespace Texts {
static std::map<wstr, wstr> english;
static bool englishLoaded = false;
static std::map<wstr, wstr> current;
static bool hasCurrent = false;
static wstr code = L"en";
static wstr displayName = L"English";

wstr Code() { return code; }
wstr DisplayName() { return displayName; }
wstr LangFolder() { return Util::Combine(Program::programDir, L"lang"); }

wstr EnglishSource() { return Util::Widen(Util::ResourceText(IDR_EN_TXT)); }

const std::map<wstr, wstr>& English() {
    if (!englishLoaded) { english = Parse(EnglishSource()); englishLoaded = true; }
    return english;
}

wstr Unescape(const wstr& v) {
    if (v.find(L'\\') == wstr::npos) return v;
    wstr r;
    for (size_t i = 0; i < v.size(); i++) {
        wchar_t c = v[i];
        if (c == L'\\' && i + 1 < v.size()) {
            wchar_t n = v[i + 1];
            if (n == L'n') { r += L'\n'; i++; continue; }
            if (n == L'u' && i + 5 < v.size()) {
                unsigned code = 0;
                bool ok = true;
                for (int k = 2; k < 6; k++) {
                    wchar_t h = v[i + k];
                    int d = h >= L'0' && h <= L'9' ? h - L'0' : (h >= L'a' && h <= L'f' ? h - L'a' + 10 : (h >= L'A' && h <= L'F' ? h - L'A' + 10 : -1));
                    if (d < 0) { ok = false; break; }
                    code = code * 16 + d;
                }
                if (ok) { r += (wchar_t)code; i += 5; continue; }
            }
        }
        r += c;
    }
    return r;
}

std::map<wstr, wstr> Parse(const wstr& text) {
    std::map<wstr, wstr> d;
    for (const wstr& raw : Util::Split(Util::Replace(text, L"\r\n", L"\n"), L'\n')) {
        wstr line = Util::Trim(raw);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        size_t eq = line.find(L'=');
        if (eq == wstr::npos || eq == 0) continue;
        d[Util::Trim(line.substr(0, eq))] = Unescape(Util::Trim(line.substr(eq + 1)));
    }
    return d;
}

wstr AutoLanguageCode() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    LANGID id = GetUserDefaultUILanguage();
    if (LCIDToLocaleName(MAKELCID(id, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) return name;
    return L"en";
}

static wstr NameOf(const wstr& c, const std::map<wstr, wstr>* d) {
    if (d) {
        auto it = d->find(L"_name");
        if (it != d->end() && !it->second.empty()) return it->second;
    }
    wchar_t buf[128];
    if (GetLocaleInfoEx(c.c_str(), LOCALE_SNATIVELANGUAGENAME, buf, 128) > 0) return buf;
    return c;
}

void Load(const wstr& language) {
    current.clear();
    hasCurrent = false;
    code = L"en";
    displayName = L"English";
    wstr want = language.empty() ? L"auto" : Util::Trim(language);
    std::vector<wstr> candidates;
    if (Util::IEquals(want, L"auto")) {
        wstr ui = AutoLanguageCode();
        candidates.push_back(ui);
        size_t dash = ui.find(L'-');
        if (dash != wstr::npos) candidates.push_back(ui.substr(0, dash));
    } else candidates.push_back(want);
    for (const wstr& c : candidates) {
        if (c.empty()) continue;
        if (Util::IEquals(c, L"qps")) {
            current = Pseudo(English());
            hasCurrent = true;
            code = L"qps";
            displayName = L"Pseudo";
            return;
        }
        if (Util::IEquals(c, L"en") || Util::StartsWithI(c, L"en-")) break;
        wstr file = Util::Combine(LangFolder(), c + L".txt");
        std::string data;
        if (!Util::FileExists(file)) continue;
        if (!Util::ReadFileBytes(file, data)) { Log::Warn(L"cannot read " + file); continue; }
        if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) data = data.substr(3);
        current = Parse(Util::Widen(data));
        hasCurrent = true;
        code = c;
        displayName = NameOf(c, &current);
        return;
    }
}

std::vector<std::pair<wstr, wstr>> Available() {
    std::vector<std::pair<wstr, wstr>> r;
    auto en = English().find(L"_name");
    r.push_back(std::make_pair(L"en", en == English().end() ? L"English" : en->second));
    std::vector<wstr> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Util::Combine(LangFolder(), L"*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end(), [](const wstr& a, const wstr& b) { return Util::Lower(a) < Util::Lower(b); });
    for (const wstr& f : files) {
        wstr c = f.substr(0, f.size() - 4);
        if (Util::IEquals(c, L"en")) continue;
        std::string data;
        std::map<wstr, wstr> d;
        if (Util::ReadFileBytes(Util::Combine(LangFolder(), f), data)) {
            if (data.size() >= 3 && (unsigned char)data[0] == 0xEF) data = data.substr(3);
            d = Parse(Util::Widen(data));
        }
        r.push_back(std::make_pair(c, NameOf(c, &d)));
    }
    return r;
}

wstr T(const wstr& key) {
    if (hasCurrent) { auto it = current.find(key); if (it != current.end()) return it->second; }
    auto it = English().find(key);
    if (it != English().end()) return it->second;
    return key;
}

wstr Fill(wstr text, const Args& args) {
    for (const auto& a : args) text = Util::Replace(text, L"{" + a.first + L"}", a.second);
    return text;
}

wstr F(const wstr& key, const Args& args) { return Fill(T(key), args); }

// numbers fill placeholders like .NET's ToString(null, CurrentCulture) for integers: plain digits
static wstr FormatCount(long long n) { return Util::Inv(n); }

wstr P(const wstr& key, long long count, const Args& args) {
    wstr cat = Category(code == L"qps" ? L"en" : code, count);
    wstr text;
    bool found = false;
    if (hasCurrent) {
        auto it = current.find(key + L"." + cat);
        if (it == current.end()) it = current.find(key + L".other");
        if (it != current.end()) { text = it->second; found = true; }
    }
    if (!found) {
        wstr enCat = Category(L"en", count);
        auto it = English().find(key + L"." + enCat);
        if (it == English().end()) it = English().find(key + L".other");
        text = it != English().end() ? it->second : key;
    }
    Args all;
    all.push_back(std::make_pair(L"count", FormatCount(count)));
    all.insert(all.end(), args.begin(), args.end());
    return Fill(text, all);
}

wstr Category(const wstr& lang, long long n) {
    wstr l = Util::Lower(lang.empty() ? L"en" : lang);
    size_t dash = l.find(L'-');
    if (dash != wstr::npos) l = l.substr(0, dash);
    long long a = n < 0 ? -n : n, m10 = a % 10, m100 = a % 100;
    if (l == L"cs" || l == L"sk") { if (a == 1) return L"one"; if (a >= 2 && a <= 4) return L"few"; return L"other"; }
    if (l == L"pl") { if (a == 1) return L"one"; if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return L"few"; return L"many"; }
    if (l == L"ru" || l == L"uk" || l == L"be") {
        if (m10 == 1 && m100 != 11) return L"one";
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return L"few";
        return L"many";
    }
    if (l == L"hr" || l == L"sr" || l == L"bs") {
        if (m10 == 1 && m100 != 11) return L"one";
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return L"few";
        return L"other";
    }
    if (l == L"sl") { if (m100 == 1) return L"one"; if (m100 == 2) return L"two"; if (m100 == 3 || m100 == 4) return L"few"; return L"other"; }
    if (l == L"lt") { if (m10 == 1 && (m100 < 11 || m100 > 19)) return L"one"; if (m10 >= 2 && (m100 < 11 || m100 > 19)) return L"few"; return L"other"; }
    if (l == L"lv") { if (m10 == 0 || (m100 >= 11 && m100 <= 19)) return L"zero"; if (m10 == 1 && m100 != 11) return L"one"; return L"other"; }
    if (l == L"ro") { if (a == 1) return L"one"; if (a == 0 || (m100 >= 2 && m100 <= 19)) return L"few"; return L"other"; }
    if (l == L"fr" || l == L"pt") return a <= 1 ? L"one" : L"other";
    if (l == L"ar") {
        if (a == 0) return L"zero"; if (a == 1) return L"one"; if (a == 2) return L"two";
        if (m100 >= 3 && m100 <= 10) return L"few"; if (m100 >= 11) return L"many"; return L"other";
    }
    if (l == L"he") { if (a == 1) return L"one"; if (a == 2) return L"two"; return L"other"; }
    if (l == L"ja" || l == L"zh" || l == L"ko" || l == L"vi" || l == L"th" || l == L"id" || l == L"ms") return L"other";
    return a == 1 ? L"one" : L"other";
}

std::map<wstr, wstr> Pseudo(const std::map<wstr, wstr>& src) {
    static const wchar_t* plain = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const wchar_t* fancy = L"\u00e5\u0180\u00e7\u00f0\u00e9\u0192\u011d\u0125\u00ee\u0135\u0137\u013c\u0271\u00f1\u00f6\u00fe\u01eb\u0155\u0161\u0163\u00fb\u1e7d\u0175\u1e8b\u00fd\u017e\u00c5\u0181\u00c7\u00d0\u00c9\u0191\u011c\u0124\u00ce\u0134\u0136\u013b\u1e3e\u00d1\u00d6\u00de\u01ea\u0154\u0160\u0162\u00db\u1e7c\u0174\u1e8a\u00dd\u017d";
    std::map<wstr, wstr> d;
    for (const auto& kv : src) {
        if (!kv.first.empty() && kv.first[0] == L'_') { d[kv.first] = kv.second; continue; }
        wstr sb;
        bool inPlaceholder = false;
        for (wchar_t c : kv.second) {
            if (c == L'{') inPlaceholder = true;
            if (!inPlaceholder) {
                const wchar_t* p = wcschr(plain, c);
                sb += (p && c) ? fancy[p - plain] : c;
            } else sb += c;
            if (c == L'}') inPlaceholder = false;
        }
        int pad = std::max(1, Util::RoundEven(kv.second.size() * 0.4));
        d[kv.first] = L"[" + sb + L" " + wstr(pad, L'\u00b7') + L"]";
    }
    d[L"_name"] = L"Pseudo";
    return d;
}

int DumpBuiltIn(const wstr& file) {
    wstr t = Util::Replace(Util::Replace(EnglishSource(), L"\r\n", L"\n"), L"\n", L"\r\n");
    Util::WriteFileBytes(file, Util::Narrow(t));
    ConsoleOut::Line(L"wrote " + file);
    return 0;
}
}

// ================================================================== options

Options Options::Parse(const std::vector<wstr>& args) {
    Options o;
    for (size_t i = 0; i < args.size(); i++) {
        wstr a = Util::Trim(args[i]);
        if (a.empty()) continue;
        if (a[0] != L'/' && a[0] != L'-') { o.error = L"Unknown argument: " + a; return o; }
        size_t start = 0;
        while (start < a.size() && (a[start] == L'/' || a[start] == L'-')) start++;
        wstr body = a.substr(start);
        wstr name = body, value;
        bool hasValue = false;
        size_t sep = body.find_first_of(L":=");
        if (sep != wstr::npos && sep > 0) { name = body.substr(0, sep); value = body.substr(sep + 1); hasValue = true; }
        auto unquote = [](wstr v) { while (!v.empty() && v.front() == L'"') v.erase(v.begin()); while (!v.empty() && v.back() == L'"') v.pop_back(); return v; };
        name = Util::Lower(name);
        if (name == L"ini") {
            if (!hasValue && i + 1 < args.size()) { value = args[++i]; hasValue = true; }
            if (value.empty()) { o.error = L"/ini needs a path, for example /ini:%APPDATA%\\Glimpse\\Glimpse.ini"; return o; }
            o.iniArgument = unquote(value);
        } else if (name == L"selftest") {
            o.selfTest = true;
            if (hasValue && !value.empty()) o.casesPath = unquote(value);
            else if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != L'/' && args[i + 1][0] != L'-') o.casesPath = args[++i];
        } else if (name == L"checklang") o.checkLang = true;
        else if (name == L"dumplang") o.dumpLang = value.empty() ? L"en.txt" : unquote(value);
        else if (name == L"dumpini") o.dumpIni = value.empty() ? L"Glimpse.ini" : unquote(value);
        else if (name == L"render") {
            if (!hasValue && i + 1 < args.size()) value = args[++i];
            o.render = value.empty() ? L"previews" : unquote(value);
        } else if (name == L"renderlang") o.renderLang = value;
        else if (name == L"settings") o.showSettings = true;
        else if (name == L"exit") o.exit = true;
        else if (name == L"background") o.background = true;
        else { o.error = L"Unknown option: " + a; return o; }
    }
    return o;
}

// ================================================================== console output

namespace ConsoleOut {
static HANDLE out = nullptr;
static bool isConsole = false;

void Attach() {
    if (out) return;
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    DWORD mode;
    isConsole = out && out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode);
}

void Write(const wstr& s) {
    Attach();
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD written;
    if (isConsole) WriteConsoleW(out, s.c_str(), (DWORD)s.size(), &written, nullptr);
    else { std::string u = Util::Narrow(s); WriteFile(out, u.data(), (DWORD)u.size(), &written, nullptr); }
}

void Line(const wstr& s) { Write(s + L"\r\n"); }
}

} // namespace glimpse
