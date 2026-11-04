// Glimpse native build: geometry engine and resolver. A line-by-line port of the C# build, so both
// make the same decisions (tests/geometry-cases.txt, docs/algorithm.md).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "glimpse.h"
#include <climits>

namespace glimpse {

const wchar_t* EdgeName(Edge e) {
    switch (e) {
        case Edge::Top: return L"top";
        case Edge::Bottom: return L"bottom";
        case Edge::Left: return L"left";
        default: return L"right";
    }
}

// ================================================================== geometry engine

namespace Geometry {

int GlimpsePx(int glimpseSize, int dpi) {
    return std::max(8, Util::RoundEven(glimpseSize * (dpi <= 0 ? 96 : dpi) / 96.0));
}

struct Run { int a, b, q; };

// quality ranges along the top edge as absolute [start, end) runs
static std::vector<Run> TopRanges(const GeoWindow* w, const Rc& f) {
    std::vector<Run> r;
    if (w && w->topMap && !w->topMap->starts.empty()) {
        const TopEdgeMap& m = *w->topMap;
        for (size_t i = 0; i < m.starts.size(); i++) {
            int a = f.L + std::max(0, m.starts[i]);
            int b = i + 1 < m.starts.size() ? f.L + m.starts[i + 1] : f.R;
            if (b > f.R) b = f.R;
            if (b > a) r.push_back(Run{ a, b, m.qualities[i] });
        }
        return r;
    }
    int dpi = w ? w->dpi : 96;
    int icon = w && w->iconW >= 0 ? w->iconW : Util::Scale(32, dpi);
    int caption = w && w->captionW >= 0 ? w->captionW : Util::Scale(138, dpi);
    int iconEnd = std::min(f.R, f.L + icon);
    int capStart = std::max(iconEnd, f.R - caption);
    if (iconEnd > f.L) r.push_back(Run{ f.L, iconEnd, 1 });
    if (capStart > iconEnd) r.push_back(Run{ iconEnd, capStart, 3 });
    if (f.R > capStart) r.push_back(Run{ capStart, f.R, 0 });
    return r;
}

static int MinQuality(const std::vector<Run>& ranges, int x0, int x1) {
    int q = 3;
    for (const Run& r : ranges) if (r.a < x1 && r.b > x0) q = std::min(q, r.q);
    return q;
}

// square start positions s where [s, s+P) lies in runs of at least quality q
static IntervalSet GoodStarts(const std::vector<Run>& ranges, int q, int P) {
    IntervalSet set;
    int runStart = INT_MIN, runEnd = INT_MIN;
    for (const Run& r : ranges) {
        if (r.q >= q) {
            if (runStart != INT_MIN && r.a == runEnd) runEnd = r.b;
            else {
                if (runStart != INT_MIN) set.Add(runStart, runEnd - P);
                runStart = r.a;
                runEnd = r.b;
            }
        } else if (runStart != INT_MIN) {
            set.Add(runStart, runEnd - P);
            runStart = INT_MIN;
        }
    }
    if (runStart != INT_MIN) set.Add(runStart, runEnd - P);
    return set;
}

ZonePtr BestZone(const GeoWindow* w, const Rc& f, int P, const std::vector<Rc>& others, const std::vector<GeoMonitor>& mons) {
    ZonePtr best;
    for (int e = 0; e < 4; e++) {
        Edge edge = (Edge)e;
        bool horiz = edge == Edge::Top || edge == Edge::Bottom;
        int len = horiz ? f.W() : f.H(), depth = horiz ? f.H() : f.W();
        if (len < P || depth < P) continue;
        int c0 = edge == Edge::Top ? f.T : edge == Edge::Bottom ? f.B - P : edge == Edge::Left ? f.L : f.R - P;
        int c1 = c0 + P;
        int s0 = horiz ? f.L : f.T, s1 = (horiz ? f.R : f.B) - P;

        IntervalSet allowed;
        for (const GeoMonitor& m : mons) {
            int mc0 = horiz ? m.work.T : m.work.L, mc1 = horiz ? m.work.B : m.work.R;
            if (mc0 > c0 || mc1 < c1) continue;
            int ms0 = horiz ? m.work.L : m.work.T, ms1 = (horiz ? m.work.R : m.work.B) - P;
            allowed.Add(std::max(s0, ms0), std::min(s1, ms1));
        }
        if (allowed.IsEmpty()) continue;
        for (const Rc& o : others) {
            int oc0 = horiz ? o.T : o.L, oc1 = horiz ? o.B : o.R;
            if (oc1 <= c0 || oc0 >= c1) continue;
            int os0 = horiz ? o.L : o.T, os1 = horiz ? o.R : o.B;
            if (os1 <= os0) continue;
            allowed.Subtract(os0 - P + 1, os1 - 1);
            if (allowed.IsEmpty()) break;
        }
        if (allowed.IsEmpty()) continue;

        int quality, pos;
        if (edge == Edge::Top) {
            std::vector<Run> ranges = TopRanges(w, f);
            quality = 0;
            pos = 0;
            for (int q = 3; q >= 1; q--) {
                IntervalSet hit = allowed.Intersect(GoodStarts(ranges, q, P));
                if (!hit.IsEmpty()) { quality = q; pos = hit.First(); break; }
            }
            if (quality == 0) continue;   // only caption buttons are free: never counts
        } else if (edge == Edge::Bottom) {
            quality = 2;
            pos = allowed.First();
        } else {
            // a side square reaching into the title-bar band inherits what a click there does:
            // never on caption buttons, only as a last resort on the window-menu icon
            int band = w && w->captionH >= 0 ? w->captionH : Util::Scale(32, w ? w->dpi : 96);
            int qBand = MinQuality(TopRanges(w, f), c0, c1);
            if (qBand == 0) allowed.Subtract(f.T - P + 1, f.T + band - 1);
            if (allowed.IsEmpty()) continue;
            quality = 2;
            pos = allowed.First();
            if (qBand == 1) {
                IntervalSet below = IntervalSet::Of(f.T + band, INT_MAX - 1).Intersect(allowed);
                if (below.IsEmpty()) quality = 1; else pos = below.First();
            }
        }

        if (best && best->quality >= quality) continue;
        Rc z;
        switch (edge) {
            case Edge::Top: z = Rc(pos, f.T, pos + P, f.T + P); break;
            case Edge::Bottom: z = Rc(pos, f.B - P, pos + P, f.B); break;
            case Edge::Left: z = Rc(f.L, pos, f.L + P, pos + P); break;
            default: z = Rc(f.R - P, pos, f.R, pos + P); break;
        }
        best = std::make_shared<Zone>();
        best->edge = edge;
        best->rect = z;
        best->quality = quality;
        if (quality == 3) break;
    }
    return best;
}

int MonitorOf(const Rc& r, const std::vector<GeoMonitor>& mons) {
    int best = -1;
    long long bestArea = 0;
    for (size_t i = 0; i < mons.size(); i++) {
        long long a = r.Intersect(mons[i].bounds).Area();
        if (a > bestArea) { bestArea = a; best = (int)i; }
    }
    if (best >= 0) return best;
    double bestDist = 1.7976931348623157e308;
    for (size_t i = 0; i < mons.size(); i++) {
        double d = mons[i].work.DistanceTo(r.CX(), r.CY());
        if (d < bestDist) { bestDist = d; best = (int)i; }
    }
    return std::max(0, best);
}

Rc ClampInto(const Rc& r, const Rc& work) {
    int dx = 0, dy = 0;
    if (r.R > work.R) dx = work.R - r.R;
    if (r.L + dx < work.L) dx = work.L - r.L;
    if (r.B > work.B) dy = work.B - r.B;
    if (r.T + dy < work.T) dy = work.T - r.T;
    return r.Offset(dx, dy);
}

Rc GrowToMin(const Rc& f, int minW, int minH, const wstr& edges, const Rc& work) {
    Rc r = f;
    if (minW > 0 && r.W() < minW) {
        if (work.W() <= minW) { r.L = work.L; r.R = work.R; }
        else if (edges.find(L'L') != wstr::npos && edges.find(L'R') == wstr::npos) r.L = r.R - minW;
        else r.R = r.L + minW;
    }
    if (minH > 0 && r.H() < minH) {
        if (work.H() <= minH) { r.T = work.T; r.B = work.B; }
        else if (edges.find(L'T') != wstr::npos && edges.find(L'B') == wstr::npos) r.T = r.B - minH;
        else r.B = r.T + minH;
    }
    if (r.Same(f)) return f;
    // shift inward only by what growing pushed out
    int over = r.R - std::max(work.R, f.R);
    if (over > 0) r = r.Offset(-over, 0);
    over = std::min(work.L, f.L) - r.L;
    if (over > 0) r = r.Offset(over, 0);
    over = r.B - std::max(work.B, f.B);
    if (over > 0) r = r.Offset(0, -over);
    over = std::min(work.T, f.T) - r.T;
    if (over > 0) r = r.Offset(0, over);
    return r;
}

bool Reachable(const Rc& r, int P, const std::vector<GeoMonitor>& mons) {
    for (const GeoMonitor& m : mons) {
        Rc i = r.Intersect(m.work);
        if (i.W() >= P && i.H() >= P) return true;
    }
    return false;
}

Rc Rescue(const Rc& f, const Rc& prevWork, bool resizable, const std::vector<GeoMonitor>& mons) {
    int mi = 0;
    double bestDist = 1.7976931348623157e308;
    for (size_t i = 0; i < mons.size(); i++) {
        double d = mons[i].work.DistanceTo(f.CX(), f.CY());
        if (d < bestDist) { bestDist = d; mi = (int)i; }
    }
    Rc work = mons[mi].work;
    Rc old = prevWork.IsEmpty() ? work : prevWork;
    int w = f.W(), h = f.H();
    if (resizable) { w = std::min(w, work.W()); h = std::min(h, work.H()); }
    double rx = old.W() > f.W() ? (f.L - old.L) / (double)(old.W() - f.W()) : 0;
    double ry = old.H() > f.H() ? (f.T - old.T) / (double)(old.H() - f.H()) : 0;
    rx = std::max(0.0, std::min(1.0, rx));
    ry = std::max(0.0, std::min(1.0, ry));
    int x = work.L + Util::RoundEven(rx * std::max(0, work.W() - w));
    int y = work.T + Util::RoundEven(ry * std::max(0, work.H() - h));
    return Rc(x, y, x + w, y + h);
}

} // namespace Geometry

// ================================================================== resolver

Request Request::For(Cause cause, FixMode mode, int target, bool placeNewWindows) {
    Request r;
    r.cause = cause;
    r.target = target;
    r.allowShrink = mode == FixMode::Strict;
    r.fix = mode != FixMode::Guide || cause == Cause::GiveGlimpse;
    switch (cause) {
        case Cause::Drop: r.victims = true; break;
        case Cause::New:
        case Cause::Restore: r.minSizeTarget = true; r.intruder = placeNewWindows; break;
        case Cause::AppMove: r.intruder = mode == FixMode::Strict; break;
        case Cause::AppResize: r.minSizeTarget = true; r.intruder = mode == FixMode::Strict; break;
        case Cause::Display: r.rescue = true; r.minSizeAll = true; r.victims = true; break;
        case Cause::Arrange: r.minSizeAll = true; r.victims = true; break;
        case Cause::GiveGlimpse: r.intruder = true; break;
        default: break;
    }
    return r;
}

const Move* Outcome::MoveOf(int index) const {
    for (const Move& m : moves) if (m.index == index) return &m;
    return nullptr;
}

void Layout::Begin() {
    frames.assign(windows.size(), Rc());
    for (size_t i = 0; i < windows.size(); i++) { frames[i] = windows[i].frame; windows[i].order = (long long)i; }
}

std::vector<Rc> Layout::Others(int i, int moved, const Rc& movedTo, bool skipMoved) const {
    std::vector<Rc> list;
    list.reserve(windows.size());
    for (int j = 0; j < (int)windows.size(); j++) {
        if (j == i || !windows[j].blocker) continue;
        if (j == moved) { if (!skipMoved) list.push_back(movedTo); continue; }
        list.push_back(frames[j]);
    }
    return list;
}

ZonePtr Layout::ZoneOf(int i) const { return Geometry::BestZone(&windows[i], frames[i], P(i), Others(i, -1, Rc(), false), monitors); }

ZonePtr Layout::ZoneAt(int i, const Rc& frame) const { return Geometry::BestZone(&windows[i], frame, P(i), Others(i, -1, Rc(), false), monitors); }

ZonePtr Layout::ZoneWith(int j, int moved, const Rc& movedTo, bool absent) const {
    return Geometry::BestZone(&windows[j], frames[j], P(j), Others(j, moved, movedTo, absent), monitors);
}

namespace Resolver {

static const int MonitorPenalty = 100000;

static Move* FindMove(Outcome& o, int index) {
    for (Move& m : o.moves) if (m.index == index) return &m;
    return nullptr;
}

static void Record(Layout& L, Outcome& o, int i, const Rc& to, const wstr& why) {
    Rc from = L.frames[i];
    if (from.Same(to)) return;
    L.frames[i] = to;
    Move* m = FindMove(o, i);
    if (!m) {
        Move mv;
        mv.index = i;
        mv.w = &L.windows[i];
        mv.from = from;
        mv.to = to;
        mv.why = why;
        o.moves.push_back(mv);
    } else {
        m->to = to;
        if (m->why == L"minsize" && why != L"minsize") m->why = why;
    }
}

static void RescuePass(Layout& L, Outcome& o) {
    for (int i = 0; i < (int)L.windows.size(); i++) {
        const GeoWindow& w = L.windows[i];
        if (!w.movable) continue;
        if (Geometry::Reachable(L.frames[i], L.P(i), L.monitors)) continue;
        Record(L, o, i, Geometry::Rescue(L.frames[i], w.prevWork, w.resizable, L.monitors), L"rescue");
    }
}

static void MinSizePass(Layout& L, const Request& q, Outcome& o) {
    for (int i = 0; i < (int)L.windows.size(); i++) {
        bool inScope = q.minSizeAll || (q.minSizeTarget && i == q.target) || (q.cause == Cause::Drop && i == q.target && !q.draggedEdges.empty());
        if (!inScope) continue;
        const GeoWindow& w = L.windows[i];
        if (!w.resizable || !w.movable || !w.subject || (w.minW <= 0 && w.minH <= 0)) continue;
        Rc to = Geometry::GrowToMin(L.frames[i], w.minW, w.minH, i == q.target ? q.draggedEdges : wstr(), L.WorkOf(L.frames[i]));
        Record(L, o, i, to, L"minsize");
    }
}

// windows that lost their glimpse yield, smallest first, by the cheapest nudge
static void VictimPass(Layout& L, const Request& q, Outcome& o, std::vector<wstr>& reasons) {
    int n = (int)L.windows.size();
    std::vector<int> victims;
    Rc anchorEnd = q.target >= 0 ? L.frames[q.target] : Rc();
    for (int i = 0; i < n; i++) {
        const GeoWindow& w = L.windows[i];
        if (!w.subject || i == q.target) continue;
        if (q.cause == Cause::Drop && q.target >= 0) {
            // only windows the drop can have affected
            bool touched = L.frames[i].Intersects(anchorEnd) || (!q.anchorStart.IsEmpty() && L.frames[i].Intersects(q.anchorStart));
            if (!touched) continue;
        }
        if (!L.ZoneOf(i)) victims.push_back(i);
    }
    std::sort(victims.begin(), victims.end(), [&](int a, int b) {
        long long aa = L.frames[a].Area(), ab = L.frames[b].Area();
        if (aa != ab) return aa < ab;
        return a < b;
    });
    for (int i : victims) {
        const GeoWindow& w = L.windows[i];
        if (L.ZoneOf(i)) continue;   // an earlier move freed it
        if (!w.movable) { reasons[i] = w.fixedReason.empty() ? L"noSpace" : w.fixedReason; continue; }
        if (q.alreadyMoved.count(w.id)) { reasons[i] = L"noSpace"; continue; }
        Rc to;
        if (BestNudge(L, q, i, to)) Record(L, o, i, to, L"glimpse");
        else reasons[i] = L"noSpace";
    }
}

struct Cand {
    Rc to;
    int cost = 0, dx = 0, dy = 0, seq = 0;
};

static std::vector<bool> SubjectsWithZone(const Layout& L, int except) {
    std::vector<bool> has(L.windows.size(), false);
    for (int j = 0; j < (int)L.windows.size(); j++)
        if (j != except && L.windows[j].subject) has[j] = (bool)L.ZoneOf(j);
    return has;
}

// window i at frame to has a glimpse, and every window in mustKeep keeps its own
static bool Valid(const Layout& L, int i, const Rc& to, const std::vector<bool>& mustKeep, ZonePtr& zone) {
    zone = L.ZoneAt(i, to);
    if (!zone) return false;
    if (!L.windows[i].blocker) return true;
    for (int j = 0; j < (int)L.windows.size(); j++) {
        if (j == i || !mustKeep[j]) continue;
        if (!L.frames[j].Intersects(to)) continue;
        if (!L.ZoneWith(j, i, to, false)) return false;
    }
    return true;
}

static void AddUnique(std::vector<int>& list, int v) {
    if (std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
}

static void TryShrink(std::vector<Cand>& cands, const Rc& to, const Rc& f, int minW, int minH, int& seq) {
    if (to.W() < minW || to.H() < minH || to.Same(f)) return;
    if (to.W() < f.W() * 2 / 3 || to.H() < f.H() * 2 / 3) return;   // never collapse a window
    int shrunk = (f.W() - to.W()) + (f.H() - to.H());
    Cand c;
    c.to = to;
    c.cost = 3 * shrunk;
    c.seq = seq++;
    cands.push_back(c);
}

bool BestNudge(Layout& L, const Request& q, int i, Rc& best) {
    best = Rc();
    const GeoWindow& w = L.windows[i];
    Rc f = L.frames[i];
    int P = L.P(i);
    int mon = Geometry::MonitorOf(f, L.monitors);
    Rc work = L.monitors[mon].work;
    int maxNudge = std::min(std::min(f.W(), f.H()) / 4, Util::Scale(240, w.dpi));

    std::vector<int> dxs(1, 0), dys(1, 0);
    std::vector<Rc> coverers;
    for (int j = 0; j < (int)L.windows.size(); j++) {
        if (j == i || !L.windows[j].blocker) continue;
        Rc c = L.frames[j];
        if (!c.Intersects(f)) continue;
        coverers.push_back(c);
        AddUnique(dys, c.T - P - f.T); AddUnique(dys, c.B + P - f.B); AddUnique(dys, c.B - f.T); AddUnique(dys, c.T - f.B);
        AddUnique(dxs, c.L - P - f.L); AddUnique(dxs, c.R + P - f.R); AddUnique(dxs, c.R - f.L); AddUnique(dxs, c.L - f.R);
    }
    // the work area's edges matter when the glimpse lies under the taskbar or off-screen
    AddUnique(dys, work.T - f.T); AddUnique(dys, work.B - f.B);
    AddUnique(dxs, work.L - f.L); AddUnique(dxs, work.R - f.R);

    std::vector<Cand> cands;
    std::set<std::pair<int, int>> seen;
    int seq = 0;
    for (int dx : dxs)
        for (int dy : dys) {
            if (dx == 0 && dy == 0) continue;
            Rc to = Geometry::ClampInto(f.Offset(dx, dy), work);
            int ax = to.L - f.L, ay = to.T - f.T;
            if (ax == 0 && ay == 0) continue;
            if (std::sqrt((double)ax * ax + (double)ay * ay) > maxNudge) continue;
            if (!seen.insert(std::make_pair(ax, ay)).second) continue;
            int cost = std::abs(ax) + std::abs(ay);
            if (Geometry::MonitorOf(to, L.monitors) != mon) cost += MonitorPenalty;
            Cand c;
            c.to = to;
            c.cost = cost;
            c.dx = ax;
            c.dy = ay;
            c.seq = seq++;
            cands.push_back(c);
        }

    if (q.allowShrink && w.resizable) {
        int minW = std::max(w.minW, 2 * P), minH = std::max(w.minH, 2 * P);
        for (const Rc& c : coverers) {
            TryShrink(cands, Rc(f.L, f.T, c.L, f.B), f, minW, minH, seq);
            TryShrink(cands, Rc(c.R, f.T, f.R, f.B), f, minW, minH, seq);
            TryShrink(cands, Rc(f.L, f.T, f.R, c.T), f, minW, minH, seq);
            TryShrink(cands, Rc(f.L, c.B, f.R, f.B), f, minW, minH, seq);
        }
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.cost != b.cost) return a.cost < b.cost;
        return a.seq < b.seq;
    });

    std::vector<bool> mustKeep = SubjectsWithZone(L, i);
    const Cand* chosen = nullptr;
    int chosenQuality = -1;
    for (const Cand& c : cands) {
        if (chosen && c.cost > chosen->cost) break;
        ZonePtr z;
        if (!Valid(L, i, c.to, mustKeep, z)) continue;
        if (!chosen || z->quality > chosenQuality) { chosen = &c; chosenQuality = z->quality; }
    }
    if (!chosen) return false;
    best = chosen->to;
    return true;
}

static int Violations(const Layout& L, int t, const Rc& at, const std::vector<bool>& keep) {
    int v = L.ZoneAt(t, at) ? 0 : 1;
    for (int j = 0; j < (int)L.windows.size(); j++)
        if (keep[j] && !L.ZoneWith(j, t, at, false)) v++;
    return v;
}

static void AddPos(std::vector<int>& list, int v, int lo, int hi) {
    if (hi < lo) v = lo; else v = std::max(lo, std::min(hi, v));
    if (std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
}

static bool NearestSpot(const Layout& L, int t, int w, int h, const Rc& work, const std::vector<bool>& keep, Rc& best) {
    best = Rc();
    Rc f = L.frames[t];
    int xlo = work.L, xhi = work.R - w, ylo = work.T, yhi = work.B - h;
    std::vector<int> xs, ys;
    AddPos(xs, f.L, xlo, xhi); AddPos(xs, work.L, xlo, xhi); AddPos(xs, work.R - w, xlo, xhi);
    AddPos(ys, f.T, ylo, yhi); AddPos(ys, work.T, ylo, yhi); AddPos(ys, work.B - h, ylo, yhi);
    for (int j = 0; j < (int)L.windows.size(); j++) {
        if (j == t || !L.windows[j].blocker) continue;
        Rc c = L.frames[j];
        if (!c.Intersects(work)) continue;
        int Pj = L.P(j);
        AddPos(xs, c.R, xlo, xhi); AddPos(xs, c.L - w, xlo, xhi); AddPos(xs, c.L + Pj, xlo, xhi); AddPos(xs, c.R - Pj - w, xlo, xhi);
        AddPos(ys, c.B, ylo, yhi); AddPos(ys, c.T - h, ylo, yhi); AddPos(ys, c.T + Pj, ylo, yhi); AddPos(ys, c.B - Pj - h, ylo, yhi);
    }
    struct Spot { long long d2, ady, adx, y, x; };
    std::vector<Spot> cands;
    cands.reserve(xs.size() * ys.size());
    for (int x : xs)
        for (int y : ys) {
            long long dx = (long long)x - f.L, dy = (long long)y - f.T;
            cands.push_back(Spot{ dx * dx + dy * dy, dy < 0 ? -dy : dy, dx < 0 ? -dx : dx, y, x });
        }
    std::sort(cands.begin(), cands.end(), [](const Spot& a, const Spot& b) {
        if (a.d2 != b.d2) return a.d2 < b.d2;
        if (a.ady != b.ady) return a.ady < b.ady;
        if (a.adx != b.adx) return a.adx < b.adx;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    size_t limit = std::min<size_t>(cands.size(), 8000);
    for (size_t k = 0; k < limit; k++) {
        Rc to = Rc::XYWH((int)cands[k].x, (int)cands[k].y, w, h);
        ZonePtr z;
        if (Valid(L, t, to, keep, z)) { best = to; return true; }
    }
    return false;
}

// a new, restored or app-moved window yields: the nearest spot on its monitor where every glimpse
// stays, at the same size; Strict mode then tries 70% and 50%; last resort a cascade offset
static void IntruderPass(Layout& L, const Request& q, Outcome& o, std::vector<wstr>& reasons) {
    int t = q.target;
    const GeoWindow& w = L.windows[t];
    if (!w.movable) { reasons[t] = w.fixedReason; return; }
    if (!w.subject) return;
    int n = (int)L.windows.size();

    // windows that have a glimpse when the intruder is not there must keep it
    std::vector<bool> keep(n, false);
    bool violates = false;
    for (int j = 0; j < n; j++) {
        if (j == t || !L.windows[j].subject) continue;
        keep[j] = (bool)L.ZoneWith(j, t, Rc(), true);
        if (keep[j] && !L.ZoneOf(j)) violates = true;
    }
    if (!violates && L.ZoneOf(t)) return;

    Rc f = L.frames[t];
    Rc work = L.WorkOf(f);
    std::vector<std::pair<int, int>> sizes;
    sizes.push_back(std::make_pair(f.W(), f.H()));
    if (q.allowShrink && w.resizable) {
        int P = L.P(t);
        const double ks[] = { 0.7, 0.5 };
        for (double k : ks) {
            int sw = std::max(std::max(w.minW, 2 * P), (int)(f.W() * k));
            int sh = std::max(std::max(w.minH, 2 * P), (int)(f.H() * k));
            if (sw < f.W() || sh < f.H()) sizes.push_back(std::make_pair(std::min(sw, f.W()), std::min(sh, f.H())));
        }
    }
    for (const auto& size : sizes) {
        Rc to;
        if (NearestSpot(L, t, size.first, size.second, work, keep, to)) {
            Record(L, o, t, to, size.first == f.W() && size.second == f.H() ? L"place" : L"shrink");
            return;
        }
    }

    // last resort: cascade offset from the window it covers most
    int c = -1;
    long long bestArea = 0;
    for (int j = 0; j < n; j++) {
        if (j == t || !L.windows[j].subject) continue;
        long long a = L.frames[j].Intersect(f).Area();
        if (a > bestArea) { bestArea = a; c = j; }
    }
    if (c >= 0) {
        int P = L.P(t);
        Rc cf = L.frames[c];
        Rc to = Geometry::ClampInto(Rc::XYWH(cf.L + P, cf.T + P, f.W(), f.H()), work);
        if (Violations(L, t, to, keep) < Violations(L, t, f, keep)) {
            Record(L, o, t, to, L"cascade");
            return;
        }
    }
    reasons[t] = L"noSpace";
}

Outcome Resolve(Layout& L, const Request& q) {
    if (L.frames.size() != L.windows.size()) L.Begin();
    Outcome o;
    int n = (int)L.windows.size();
    std::vector<wstr> reasons(n);

    if (q.fix && !L.monitors.empty()) {
        if (q.rescue) RescuePass(L, o);
        MinSizePass(L, q, o);
        if (q.victims) VictimPass(L, q, o, reasons);
        if (q.intruder && q.target >= 0) IntruderPass(L, q, o, reasons);
    }

    o.zones.assign(n, ZonePtr());
    o.reasons.assign(n, wstr());
    for (int i = 0; i < n; i++) {
        if (!L.windows[i].subject) continue;
        o.zones[i] = L.monitors.empty() ? ZonePtr() : L.ZoneOf(i);
        if (!o.zones[i]) {
            o.hidden.push_back(i);
            const GeoWindow& w = L.windows[i];
            wstr why = reasons[i];
            if (!w.fixedReason.empty() && !w.movable) why = w.fixedReason;
            else if (i == q.target && q.cause == Cause::Drop) why = L"anchor";
            else if (!q.fix) why = L"guide";
            o.reasons[i] = why.empty() ? L"noSpace" : why;
        }
    }
    o.frames = L.frames;
    return o;
}

static void TryGuide(Layout& L, int anchor, const Rc& f, std::vector<Guide>& result, const std::vector<bool>& keep,
                     Edge edge, int coord, int range, const Rc& snapped, const Rc& zone) {
    int dist = std::abs(snapped.L - f.L) + std::abs(snapped.T - f.T);
    if (dist == 0 || dist > range) return;
    // the snapped frame must not cut through the zone it clears
    if (snapped.Intersects(zone)) return;
    for (const Guide& g : result) if (g.edge == edge && g.coord == coord) return;
    Rc work = L.WorkOf(f);
    if (!work.Contains(snapped)) return;
    // every window that has a glimpse without the anchor keeps it, and the anchor has one
    ZonePtr az;
    if (!Valid(L, anchor, snapped, keep, az)) return;
    Guide g;
    g.edge = edge;
    g.coord = coord;
    g.snapped = snapped;
    g.distance = dist;
    g.zone = zone;
    result.push_back(g);
}

std::vector<Guide> Guides(Layout& L, int anchor, int range) {
    std::vector<Guide> result;
    Rc f = L.frames[anchor];
    int n = (int)L.windows.size();
    // natural zones of the neighbours (without the anchor); they must all stay free
    std::vector<ZonePtr> zones(n);
    std::vector<bool> keep(n, false);
    for (int j = 0; j < n; j++) {
        if (j == anchor || !L.windows[j].subject) continue;
        zones[j] = L.ZoneWith(j, anchor, Rc(), true);
        keep[j] = (bool)zones[j];
    }
    for (int j = 0; j < n; j++) {
        if (!zones[j]) continue;
        Rc zr = zones[j]->rect;
        // anchor bottom above the zone, top below it, right edge left of it, left edge right of it
        TryGuide(L, anchor, f, result, keep, Edge::Bottom, zr.T, range, Rc(f.L, zr.T - f.H(), f.R, zr.T), zr);
        TryGuide(L, anchor, f, result, keep, Edge::Top, zr.B, range, Rc(f.L, zr.B, f.R, zr.B + f.H()), zr);
        TryGuide(L, anchor, f, result, keep, Edge::Right, zr.L, range, Rc(zr.L - f.W(), f.T, zr.L, f.B), zr);
        TryGuide(L, anchor, f, result, keep, Edge::Left, zr.R, range, Rc(zr.R, f.T, zr.R + f.W(), f.B), zr);
    }
    // by distance; equal distances keep the order they were found in
    std::stable_sort(result.begin(), result.end(), [](const Guide& a, const Guide& b) { return a.distance < b.distance; });
    return result;
}

} // namespace Resolver
} // namespace glimpse
