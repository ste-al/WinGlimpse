// Glimpse: simulation of crowded desktops through the real resolver (the one in Glimpse.cs).
//
// A 1920x1080 screen (work area 1920x1040) holds 8 windows of random size and 8 windows of 1600x800
// that each mostly cover one of them. For Guide, Assist and Strict it runs Arrange now, then user and
// app moves and resizes and a new window, each one covering a visible window's glimpse, and checks the
// rules on every outcome (see Validate). It writes a report, pictures of one scenario and the outcomes
// as test cases, which the native build must reproduce exactly (Glimpse.exe /selftest:<cases>).
//
// build.sh compiles and runs it; by hand:
//   mcs -langversion:5 -main:GlimpseSim.Program -r:System.Windows.Forms.dll -r:System.Drawing.dll -r:System.Core.dll
//       -r:Accessibility.dll -out:simulate.exe Glimpse.cs tests/simulate.cs
//   mono simulate.exe <outdir> [seeds=300] [showcase seed=1] [seeds exported as cases=0] [small=8 big=8]
//
// SPDX-License-Identifier: GPL-3.0-or-later
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Text;
using Glimpse;

namespace GlimpseSim
{
    sealed class Win
    {
        public string Id;
        public Rc Frame;
        public Win(string id, Rc f) { Id = id; Frame = f; }
    }

    /// <summary>One action in one mode: the layout handed to the resolver and what came out.</summary>
    sealed class Run
    {
        public string Name, Action, Target, Victim, Edges = "";
        public FixMode Mode;
        public Cause Cause;
        public Rc From;
        public List<Win> Input;
        public Outcome Before, After;
        public Outcome Pre;             // the layout before the action happened (for actions, not for Arrange)
        public List<Win> PreInput;
        public List<string> Bad = new List<string>();

        /// <summary>Windows that had a glimpse before the action and lost it through the action.</summary>
        public List<string> HiddenByAction()
        {
            var r = new List<string>();
            if (Pre == null) return r;
            for (int i = 0; i < PreInput.Count; i++)
            {
                if (Pre.Zones[i] == null) continue;
                for (int j = 0; j < Input.Count; j++)
                    if (Input[j].Id == PreInput[i].Id && Before.Zones[j] == null) r.Add(Input[j].Id);
            }
            return r;
        }
    }

    static class Program
    {
        static readonly Rc Screen = new Rc(0, 0, 1920, 1080), Work = new Rc(0, 0, 1920, 1040);
        static readonly FixMode[] Modes = { FixMode.Guide, FixMode.Assist, FixMode.Strict };

        static int Clamp(int v, int lo, int hi) { return Math.Max(lo, Math.Min(hi, v)); }
        static string M(FixMode m) { return m.ToString(); }

        // ---------------------------------------------------------------- scenario

        /// <summary>8 windows of random size, and 8 windows of 1600x800 that each mostly cover one of them.
        /// Most recently used first: the large windows were used last (they are on top).</summary>
        static int SmallCount = 8, BigCount = 8;

        static List<Win> Scenario(int seed)
        {
            var rnd = new Random(seed);
            var small = new List<Win>();
            for (int i = 1; i <= SmallCount; i++)
            {
                int w = rnd.Next(300, 1201), h = rnd.Next(220, 801);
                int x = rnd.Next(0, Work.W - w + 1), y = rnd.Next(0, Work.H - h + 1);
                small.Add(new Win("S" + i, Rc.XYWH(x, y, w, h)));
            }
            var all = new List<Win>();
            for (int i = 1; i <= BigCount; i++)
            {
                Rc s = small[(i - 1) % small.Count].Frame;
                int x = Clamp(s.CX - 800 + rnd.Next(-120, 121), 0, Work.W - 1600);
                int y = Clamp(s.CY - 400 + rnd.Next(-80, 81), 0, Work.H - 800);
                all.Add(new Win("B" + i, Rc.XYWH(x, y, 1600, 800)));
            }
            all.AddRange(small);
            return all;
        }

        static double Covered(List<Win> wins, int i)
        {
            // share of window i's area covered by the windows above it (earlier in the list)
            Rc f = wins[i].Frame;
            long total = f.Area, covered = 0;
            // sample on a 10 px grid: exact enough for a report
            for (int y = f.T; y < f.B; y += 10)
                for (int x = f.L; x < f.R; x += 10)
                {
                    for (int j = 0; j < i; j++) if (wins[j].Frame.Contains(x, y)) { covered++; break; }
                }
            long samples = (long)((f.W + 9) / 10) * ((f.H + 9) / 10);
            return samples == 0 ? 0 : covered / (double)samples;
        }

        // ---------------------------------------------------------------- resolver calls (as GlimpseApp.BuildLayout does)

        static Layout Build(List<Win> wins)
        {
            var L = new Layout { GlimpseSize = 50 };
            L.Monitors.Add(new GeoMonitor(Screen, Work, 96));
            // the default rule in Glimpse.ini: every window at least 200 x 200
            foreach (var w in wins) L.Windows.Add(new GeoWindow { Id = w.Id, Frame = w.Frame, Dpi = 96, MinW = 200, MinH = 200 });
            L.Begin();
            return L;
        }

        static Outcome Check(List<Win> wins) { return Resolver.Resolve(Build(wins), new Request { Fix = false }); }

        static int IndexOf(List<Win> wins, string id) { for (int i = 0; i < wins.Count; i++) if (wins[i].Id == id) return i; return -1; }

        static Run Do(string name, string action, FixMode mode, Cause cause, List<Win> input, string target, string victim, string edges, Rc from)
        {
            var r = new Run { Name = name, Action = action, Mode = mode, Cause = cause, Input = input, Target = target, Victim = victim, Edges = edges ?? "", From = from };
            r.Before = Check(input);
            var q = Request.For(cause, mode, target == null ? -1 : IndexOf(input, target), true);
            q.DraggedEdges = r.Edges;
            q.AnchorStart = from;
            r.After = Resolver.Resolve(Build(input), q);
            Validate(r);
            return r;
        }

        static List<Win> Apply(List<Win> wins, Outcome o)
        {
            var r = new List<Win>();
            for (int i = 0; i < wins.Count; i++) r.Add(new Win(wins[i].Id, o.Frames[i]));
            return r;
        }

        static List<Win> With(List<Win> wins, string id, Rc frame)
        {
            var r = new List<Win>();
            foreach (var w in wins) r.Add(new Win(w.Id, w.Id == id ? frame : w.Frame));
            return r;
        }

        static int Visible(Outcome o) { int n = 0; foreach (var z in o.Zones) if (z != null) n++; return n; }

        // ---------------------------------------------------------------- rules every outcome must keep

        static void Validate(Run r)
        {
            var seen = new HashSet<int>();
            string what = r.Name;
            foreach (var m in r.After.Moves)
            {
                if (!seen.Add(m.Index)) r.Bad.Add(what + ": " + m.W.Id + " moved twice");
                if (!Work.Contains(m.To)) r.Bad.Add(what + ": " + m.W.Id + " left the work area: " + m.To);
                bool resized = m.To.W != m.From.W || m.To.H != m.From.H;
                if (resized && r.Mode != FixMode.Strict && m.Why != "minsize") r.Bad.Add(what + ": " + m.W.Id + " resized in " + r.Mode);
                if (resized && (m.To.W < 200 || m.To.H < 200)) r.Bad.Add(what + ": " + m.W.Id + " below the 200 x 200 minimum: " + m.To);
                if (m.Why == "glimpse" && !resized)
                {
                    int limit = Math.Min(Math.Min(m.From.W, m.From.H) / 4, 240);
                    double d = Math.Sqrt(Math.Pow(m.To.L - m.From.L, 2) + Math.Pow(m.To.T - m.From.T, 2));
                    if (d > limit + 0.5) r.Bad.Add(what + ": " + m.W.Id + " nudged " + d.ToString("0") + " px, limit " + limit);
                }
            }
            if (r.Mode == FixMode.Guide && r.After.Moves.Count > 0) r.Bad.Add(what + ": Guide moved " + r.After.Moves.Count + " windows");
            if (r.Cause == Cause.Drop)
                foreach (var m in r.After.Moves) if (m.W.Id == r.Target && m.Why != "minsize") r.Bad.Add(what + ": the window the user dropped was moved");
            if (r.Mode == FixMode.Assist && (r.Cause == Cause.AppMove || r.Cause == Cause.AppResize))
                foreach (var m in r.After.Moves) if (m.Why != "minsize") r.Bad.Add(what + ": Assist fixed an app's move (" + m.W.Id + ")");
            // a window that has a glimpse keeps it (the resolver only makes moves that keep every glimpse);
            // the one exception by design is the intruder's last resort, a cascade offset, which is taken only
            // when it leaves fewer windows without a glimpse (docs/algorithm.md, Intruder 4)
            bool cascade = false;
            foreach (var m in r.After.Moves) if (m.Why == "cascade") cascade = true;
            if (cascade)
            {
                if (Visible(r.After) <= Visible(r.Before)) r.Bad.Add(what + ": cascade did not leave fewer windows without a glimpse (" + Visible(r.Before) + " -> " + Visible(r.After) + "); " + Moves(r.After));
            }
            else
                for (int j = 0; j < r.Input.Count; j++)
                    if (r.Before.Zones[j] != null && r.After.Zones[j] == null) r.Bad.Add(what + ": " + r.Input[j].Id + " lost its glimpse; " + Moves(r.After));
        }

        // ---------------------------------------------------------------- actions that hide a visible window

        /// <summary>A move of window t (same size) that covers the glimpse zone z completely, inside the work area.</summary>
        static bool CoverBy(Rc t, Rc z, out Rc to)
        {
            to = Geometry.ClampInto(Rc.XYWH(z.CX - t.W / 2, z.CY - t.H / 2, t.W, t.H), Work);
            return to.Contains(z) && !to.Same(t);
        }

        /// <summary>A resize of window t by its right and bottom edges until it covers zone z.</summary>
        static bool GrowOver(Rc t, Rc z, out Rc to)
        {
            to = new Rc(t.L, t.T, Math.Min(Work.R, Math.Max(t.R, z.R + 20)), Math.Min(Work.B, Math.Max(t.B, z.B + 20)));
            return t.L <= z.L && t.T <= z.T && to.Contains(z) && !to.Same(t) && to.Area <= t.Area * 4;
        }

        /// <summary>Picks a visible window (preferring the small ones) and another window that can cover its glimpse.</summary>
        static bool Pick(List<Win> wins, Outcome o, bool resize, bool bigMover, out string victim, out string mover, out Rc to)
        {
            victim = mover = null;
            to = default(Rc);
            var order = new List<int>();
            for (int i = 0; i < wins.Count; i++) if (wins[i].Id[0] == 'S') order.Add(i);
            for (int i = 0; i < wins.Count; i++) if (wins[i].Id[0] == 'B') order.Add(i);
            foreach (int v in order)
            {
                if (o.Zones[v] == null) continue;
                Rc z = o.Zones[v].Rect;
                for (int t = 0; t < wins.Count; t++)
                {
                    if (t == v) continue;
                    bool big = wins[t].Id[0] == 'B';
                    if (!resize && bigMover != big) continue;
                    if (resize && big) continue;
                    if (o.Zones[t] == null) continue;   // the mover is a visible window too
                    Rc nf;
                    if (resize ? GrowOver(wins[t].Frame, z, out nf) : CoverBy(wins[t].Frame, z, out nf))
                    {
                        victim = wins[v].Id;
                        mover = wins[t].Id;
                        to = nf;
                        return true;
                    }
                }
            }
            return false;
        }

        /// <summary>Base layout for the move and resize tests: the start layout after Arrange in Strict mode.</summary>
        static List<Run> Actions(List<Win> start, int seed, out List<Win> organized)
        {
            var runs = new List<Run>();
            var arranged = Resolver.Resolve(Build(start), Request.For(Cause.Arrange, FixMode.Strict, -1, true));
            organized = Apply(start, arranged);
            var baseCheck = Check(organized);
            foreach (FixMode mode in Modes)
            {
                string v, t;
                Rc to;
                int first = runs.Count;
                if (Pick(organized, baseCheck, false, true, out v, out t, out to))
                {
                    runs.Add(Do("seed " + seed + " " + M(mode) + " user move", "drop", mode, Cause.Drop, With(organized, t, to), t, v, "", organized[IndexOf(organized, t)].Frame));
                    runs.Add(Do("seed " + seed + " " + M(mode) + " app move", "appmove", mode, Cause.AppMove, With(organized, t, to), t, v, "", default(Rc)));
                }
                if (Pick(organized, baseCheck, true, false, out v, out t, out to))
                {
                    runs.Add(Do("seed " + seed + " " + M(mode) + " user resize", "drop", mode, Cause.Drop, With(organized, t, to), t, v, "RB", organized[IndexOf(organized, t)].Frame));
                    runs.Add(Do("seed " + seed + " " + M(mode) + " app resize", "appresize", mode, Cause.AppResize, With(organized, t, to), t, v, "", default(Rc)));
                }
                // a new 1600 x 800 window opens over the glimpse of a visible window
                for (int i = 0; i < organized.Count; i++)
                {
                    if (baseCheck.Zones[i] == null || organized[i].Id[0] != 'S') continue;
                    Rc z = baseCheck.Zones[i].Rect;
                    Rc nf = Geometry.ClampInto(Rc.XYWH(z.CX - 800, z.CY - 400, 1600, 800), Work);
                    if (!nf.Contains(z)) continue;
                    var input = new List<Win> { new Win("N1", nf) };
                    input.AddRange(organized);
                    runs.Add(Do("seed " + seed + " " + M(mode) + " new window", "new", mode, Cause.New, input, "N1", organized[i].Id, "", default(Rc)));
                    break;
                }
                for (int k = first; k < runs.Count; k++) { runs[k].Pre = baseCheck; runs[k].PreInput = organized; }
            }
            return runs;
        }

        // ---------------------------------------------------------------- report helpers

        static string Moves(Outcome o)
        {
            if (o.Moves.Count == 0) return "no moves";
            var sb = new StringBuilder();
            foreach (var m in o.Moves)
            {
                if (sb.Length > 0) sb.Append("; ");
                bool resized = m.To.W != m.From.W || m.To.H != m.From.H;
                sb.Append(m.W.Id + " " + m.Why + " " + (m.To.L - m.From.L).ToString("+0;-0;0") + "," + (m.To.T - m.From.T).ToString("+0;-0;0"));
                if (resized) sb.Append(" size " + m.From.W + "x" + m.From.H + "->" + m.To.W + "x" + m.To.H);
            }
            return o.Moves.Count + " moved: " + sb;
        }

        static string HiddenList(List<Win> wins, Outcome o)
        {
            var sb = new StringBuilder();
            foreach (int i in o.Hidden) sb.Append((sb.Length > 0 ? " " : "") + wins[i].Id);
            return sb.Length == 0 ? "-" : sb.ToString();
        }

        // ---------------------------------------------------------------- pictures

        static readonly Color Desk = Color.FromArgb(32, 36, 44), Bar = Color.FromArgb(18, 20, 26);

        static void Render(string file, string title, List<Win> wins, Outcome o, Outcome moves, string target, string victim)
        {
            const float s = 0.5f;
            int W = (int)(Screen.W * s), H = (int)(Screen.H * s) + 34;
            using (var bmp = new Bitmap(W, H))
            using (var g = Graphics.FromImage(bmp))
            using (var font = new Font(FontFamily.GenericSansSerif, 9f, FontStyle.Bold, GraphicsUnit.Pixel))
            using (var tfont = new Font(FontFamily.GenericSansSerif, 13f, FontStyle.Bold, GraphicsUnit.Pixel))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAlias;
                g.Clear(Desk);
                using (var b = new SolidBrush(Color.FromArgb(235, 235, 240))) g.DrawString(title, tfont, b, 8, 8);
                g.TranslateTransform(0, 34);
                g.ScaleTransform(s, s);
                using (var b = new SolidBrush(Bar)) g.FillRectangle(b, Work.L, Work.B, Screen.W, Screen.H - Work.B);
                // stacking: the first window is on top, so paint from the last
                for (int i = wins.Count - 1; i >= 0; i--)
                {
                    Rc f = o.Frames[i];
                    bool big = wins[i].Id[0] == 'B', fresh = wins[i].Id[0] == 'N';
                    Color fill = fresh ? Color.FromArgb(120, 84, 150) : big ? Color.FromArgb(58, 92, 140) : Color.FromArgb(150, 118, 70);
                    using (var b = new SolidBrush(fill)) g.FillRectangle(b, f.L, f.T, f.W, f.H);
                    using (var b = new SolidBrush(Color.FromArgb(40, 255, 255, 255))) g.FillRectangle(b, f.L, f.T, f.W, 32);
                    using (var p = new Pen(Color.FromArgb(200, 12, 14, 18), 3)) g.DrawRectangle(p, f.L, f.T, f.W, f.H);
                }
                // where moved windows came from
                if (moves != null)
                    foreach (var m in moves.Moves)
                    {
                        using (var p = new Pen(Color.FromArgb(230, 255, 255, 255), 4) { DashStyle = DashStyle.Dash })
                            g.DrawRectangle(p, m.From.L, m.From.T, m.From.W, m.From.H);
                        using (var p = new Pen(Color.FromArgb(255, 255, 220, 80), 6) { CustomEndCap = new AdjustableArrowCap(4, 4) })
                            g.DrawLine(p, m.From.L + 60, m.From.T + 16, m.To.L + 60, m.To.T + 16);
                    }
                // glimpse zones (always visible by definition), and outlines of the windows without one
                for (int i = 0; i < wins.Count; i++)
                {
                    Rc f = o.Frames[i];
                    if (o.Zones[i] != null)
                    {
                        Rc z = o.Zones[i].Rect;
                        using (var b = new SolidBrush(Color.FromArgb(220, 46, 204, 113))) g.FillRectangle(b, z.L, z.T, z.W, z.H);
                        using (var b = new SolidBrush(Color.White)) g.DrawString(wins[i].Id, new Font(font.FontFamily, 22f, FontStyle.Bold, GraphicsUnit.Pixel), b, z.L + 4, z.T + 10);
                    }
                    else
                    {
                        using (var p = new Pen(Color.FromArgb(240, 231, 76, 60), 5) { DashStyle = DashStyle.Dot }) g.DrawRectangle(p, f.L + 3, f.T + 3, f.W - 6, f.H - 6);
                        using (var b = new SolidBrush(Color.FromArgb(240, 231, 76, 60))) g.FillRectangle(b, f.L + 6, f.T + 6, 64, 30);
                        using (var b = new SolidBrush(Color.White)) g.DrawString(wins[i].Id, new Font(font.FontFamily, 22f, FontStyle.Bold, GraphicsUnit.Pixel), b, f.L + 10, f.T + 8);
                    }
                }
                if (target != null)
                {
                    Rc f = o.Frames[IndexOf(wins, target)];
                    using (var p = new Pen(Color.FromArgb(255, 255, 220, 80), 8)) g.DrawRectangle(p, f.L, f.T, f.W, f.H);
                }
                g.ResetTransform();
                using (var b = new SolidBrush(Color.FromArgb(200, 200, 210)))
                    g.DrawString("green: free glimpse   red: no glimpse (reachable through X-ray)   yellow: the window the action is about   dashed: position before", font, b, 8, 24);
                bmp.Save(file, ImageFormat.Png);
            }
        }

        static void Sheet(string file, List<string> parts, int cols)
        {
            var imgs = new List<Image>();
            foreach (string p in parts) imgs.Add(Image.FromFile(p));
            int w = imgs[0].Width, h = imgs[0].Height, rows = (imgs.Count + cols - 1) / cols;
            using (var bmp = new Bitmap(w * cols + 8 * (cols - 1), h * rows + 8 * (rows - 1)))
            using (var g = Graphics.FromImage(bmp))
            {
                g.Clear(Color.FromArgb(10, 10, 12));
                for (int i = 0; i < imgs.Count; i++) g.DrawImage(imgs[i], (i % cols) * (w + 8), (i / cols) * (h + 8), w, h);
                bmp.Save(file, ImageFormat.Png);
            }
            foreach (var im in imgs) im.Dispose();
        }

        // ---------------------------------------------------------------- shared test cases (both engines must agree)

        static void Case(StringBuilder sb, Run r)
        {
            sb.AppendLine("case " + r.Name);
            sb.AppendLine("mode " + r.Mode.ToString().ToLowerInvariant());
            foreach (var w in r.Input) sb.AppendLine("window " + w.Id + " " + w.Frame.L + " " + w.Frame.T + " " + w.Frame.R + " " + w.Frame.B + " min=200x200");
            string a = "action " + r.Action + (r.Target != null ? " " + r.Target : "");
            if (r.Edges.Length > 0) a += " edges=" + r.Edges;
            if (!r.From.IsEmpty) a += " from=" + r.From;
            sb.AppendLine(a);
            sb.AppendLine("expect moves " + r.After.Moves.Count);
            for (int i = 0; i < r.Input.Count; i++)
            {
                Rc f = r.After.Frames[i];
                sb.AppendLine("expect at " + r.Input[i].Id + " " + f.L + " " + f.T + " " + f.R + " " + f.B);
                sb.AppendLine(r.After.Zones[i] != null ? "expect free " + r.Input[i].Id : "expect hidden " + r.Input[i].Id);
            }
            sb.AppendLine("end");
            sb.AppendLine();
        }

        // ---------------------------------------------------------------- main

        static int Main(string[] args)
        {
            string outDir = args.Length > 0 ? args[0] : "sim-out";
            int seeds = args.Length > 1 ? int.Parse(args[1], CultureInfo.InvariantCulture) : 300;
            int show = args.Length > 2 ? int.Parse(args[2], CultureInfo.InvariantCulture) : 1;
            int caseSeeds = args.Length > 3 ? int.Parse(args[3], CultureInfo.InvariantCulture) : 0;
            if (args.Length > 5) { SmallCount = int.Parse(args[4], CultureInfo.InvariantCulture); BigCount = int.Parse(args[5], CultureInfo.InvariantCulture); }
            Directory.CreateDirectory(outDir);
            var report = new StringBuilder();
            var cases = new StringBuilder();
            cases.AppendLine("# Generated by Simulate.cs: a 1920x1080 desktop with 8 random windows and 8 of 1600x800 on top.");
            cases.AppendLine();
            int bad = 0;

            // ---- one scenario in detail, with pictures: the first seed from 'show' on with every kind of action
            for (int tries = 0; tries < 1000; tries++, show++)
            {
                List<Win> probe;
                if (Actions(Scenario(show), show, out probe).Count == 15) break;
            }
            {
                var start = Scenario(show);
                var c0 = Check(start);
                report.AppendLine("== showcase: seed " + show + " (1920x1080, work area 1920x1040, glimpse 50 px, every window at least 200x200)");
                for (int i = 0; i < start.Count; i++)
                    report.AppendLine(string.Format(CultureInfo.InvariantCulture, "  {0,-3} {1,-22} {2,4}x{3,-4} covered {4,4:0%}  {5}", start[i].Id, start[i].Frame, start[i].Frame.W, start[i].Frame.H,
                        Covered(start, i), c0.Zones[i] != null ? "glimpse " + c0.Zones[i].Edge.ToString().ToLowerInvariant() : "no glimpse"));
                report.AppendLine("  windows with a free glimpse at the start: " + Visible(c0) + " of " + start.Count + "  (no glimpse: " + HiddenList(start, c0) + ")");
                var pics = new List<string>();
                string p0 = Path.Combine(outDir, "00-start.png");
                Render(p0, "Start (seed " + show + "): " + Visible(c0) + " of 16 windows have a free glimpse", start, c0, null, null, null);
                pics.Add(p0);

                // Arrange now, in every mode, from the start layout; then again until nothing moves
                report.AppendLine();
                report.AppendLine("-- Arrange now (Settings tray menu: Arrange now) from the start layout");
                int pic = 1;
                foreach (FixMode mode in Modes)
                {
                    var r = Do("seed " + show + " " + M(mode) + " arrange", "arrange", mode, Cause.Arrange, start, null, null, "", default(Rc));
                    Case(cases, r);
                    bad += r.Bad.Count;
                    foreach (string b in r.Bad) report.AppendLine("  !! " + b);
                    report.AppendLine("  " + M(mode).PadRight(6) + " " + Visible(r.Before) + " -> " + Visible(r.After) + " with a glimpse; " + Moves(r.After));
                    report.AppendLine("         still without a glimpse (reachable through X-ray): " + HiddenList(start, r.After));
                    var layout = start;
                    var outcome = r.After;
                    var counts = new List<string> { Visible(r.After).ToString(CultureInfo.InvariantCulture) };
                    for (int k = 2; k <= 6 && outcome.Moves.Count > 0; k++)
                    {
                        layout = Apply(layout, outcome);
                        var again = Do("seed " + show + " " + M(mode) + " arrange " + k, "arrange", mode, Cause.Arrange, layout, null, null, "", default(Rc));
                        bad += again.Bad.Count;
                        foreach (string b in again.Bad) report.AppendLine("  !! " + b);
                        outcome = again.After;
                        counts.Add(Visible(outcome) + (outcome.Moves.Count == 0 ? " (no more moves)" : " (" + outcome.Moves.Count + " moved)"));
                    }
                    report.AppendLine("         Arrange now again and again: " + string.Join(" -> ", counts.ToArray()));
                    string pf = Path.Combine(outDir, string.Format("{0:00}-arrange-{1}.png", pic++, mode.ToString().ToLowerInvariant()));
                    Render(pf, M(mode) + ": Arrange now: " + Visible(r.Before) + " -> " + Visible(r.After) + " windows with a glimpse, " + r.After.Moves.Count + " moved", start, r.After, r.After, null, null);
                    pics.Add(pf);
                }

                // moves and resizes, from the layout Arrange left in Strict mode
                List<Win> organized;
                var runs = Actions(start, show, out organized);
                var oc = Check(organized);
                report.AppendLine();
                report.AppendLine("-- moves and resizes, from the layout after Arrange now (Strict): " + Visible(oc) + " of 16 with a glimpse");
                foreach (var r in runs)
                {
                    Case(cases, r);
                    bad += r.Bad.Count;
                    int vi = IndexOf(r.Input, r.Victim), ti = IndexOf(r.Input, r.Target);
                    string verdict = r.After.Zones[vi] != null ? r.Victim + " has a glimpse again" : r.Victim + " stays hidden (X-ray)";
                    report.AppendLine(string.Format("  {0,-6} {1,-12} {2} -> {3}: {4} covers {5}'s glimpse spot; {6}; {7}", M(r.Mode), r.Name.Substring(r.Name.IndexOf(M(r.Mode)) + M(r.Mode).Length + 1),
                        r.Target, r.Input[ti].Frame, r.Target, r.Victim, Moves(r.After), verdict));
                    foreach (string b in r.Bad) report.AppendLine("  !! " + b);
                    if (r.Mode != FixMode.Guide || r.Action == "drop" && r.Edges.Length == 0)
                    {
                        string pf = Path.Combine(outDir, string.Format("{0:00}-{1}-{2}.png", pic++, r.Mode.ToString().ToLowerInvariant(), r.Name.Substring(r.Name.IndexOf(M(r.Mode)) + M(r.Mode).Length + 1).Replace(' ', '-')));
                        string what = r.Name.Substring(r.Name.IndexOf(M(r.Mode)) + M(r.Mode).Length + 1);
                        Render(pf, M(r.Mode) + ": " + what + " of " + r.Target + " over " + r.Victim + ": " + (r.After.Moves.Count == 0 ? "nothing moves" : r.After.Moves.Count + " moved") + ", " + verdict,
                            r.Input, r.After, r.After, r.Target, r.Victim);
                        pics.Add(pf);
                    }
                }
                Sheet(Path.Combine(outDir, "sheet.png"), pics, 3);
            }

            // ---- many scenarios: the rules hold everywhere, and what each mode does
            var stat = new Dictionary<string, int[]>();   // key -> [runs, reacted, victim visible after, invariant failures]
            var whys = new SortedDictionary<string, int>();   // "mode action: why" -> moves
            long arrangeBefore = 0, arrangeAfterAssist = 0, arrangeAfterStrict = 0, arrangeMovesAssist = 0, arrangeMovesStrict = 0;
            int oscillations = 0;
            for (int seed = 1; seed <= seeds; seed++)
            {
                var start = Scenario(seed);
                foreach (FixMode mode in Modes)
                {
                    var r = Do("seed " + seed + " " + M(mode) + " arrange", "arrange", mode, Cause.Arrange, start, null, null, "", default(Rc));
                    bad += r.Bad.Count;
                    foreach (string b in r.Bad) report.AppendLine("  !! " + b);
                    if (seed <= caseSeeds) Case(cases, r);
                    foreach (var m in r.After.Moves) { string wk = M(mode) + " arrange: " + m.Why; int c0; whys.TryGetValue(wk, out c0); whys[wk] = c0 + 1; }
                    if (mode == FixMode.Guide) arrangeBefore += Visible(r.Before);
                    if (mode == FixMode.Assist) { arrangeAfterAssist += Visible(r.After); arrangeMovesAssist += r.After.Moves.Count; }
                    if (mode == FixMode.Strict) { arrangeAfterStrict += Visible(r.After); arrangeMovesStrict += r.After.Moves.Count; }
                    // repeated Arrange must settle
                    var layout = start;
                    var outcome = r.After;
                    int k = 1;
                    while (outcome.Moves.Count > 0 && k < 12)
                    {
                        layout = Apply(layout, outcome);
                        var again = Do("seed " + seed + " " + M(mode) + " arrange again", "arrange", mode, Cause.Arrange, layout, null, null, "", default(Rc));
                        bad += again.Bad.Count;
                        foreach (string b in again.Bad) report.AppendLine("  !! " + b);
                        outcome = again.After;
                        k++;
                    }
                    if (outcome.Moves.Count > 0) { oscillations++; report.AppendLine("  !! seed " + seed + " " + mode + ": Arrange still moves windows after 12 rounds"); }
                }
                List<Win> organized;
                foreach (var r in Actions(start, seed, out organized))
                {
                    if (seed <= caseSeeds) Case(cases, r);
                    foreach (var m in r.After.Moves)
                    {
                        string wk = M(r.Mode) + " " + r.Name.Substring(r.Name.IndexOf(M(r.Mode)) + M(r.Mode).Length + 1) + ": " + m.Why;
                        int c0; whys.TryGetValue(wk, out c0); whys[wk] = c0 + 1;
                    }
                    bad += r.Bad.Count;
                    foreach (string b in r.Bad) report.AppendLine("  !! " + b);
                    string key = M(r.Mode) + " " + r.Name.Substring(r.Name.IndexOf(M(r.Mode)) + M(r.Mode).Length + 1);
                    int[] s;
                    if (!stat.TryGetValue(key, out s)) stat[key] = s = new int[5];
                    var hid = r.HiddenByAction();
                    s[0]++;
                    if (hid.Count > 0)
                    {
                        s[1]++;
                        if (r.After.Moves.Count > 0) s[2]++;
                        bool all = true;
                        foreach (string id in hid) if (r.After.Zones[IndexOf(r.Input, id)] == null) all = false;
                        if (all) s[3]++;
                    }
                    s[4] += r.Bad.Count;
                }
            }
            report.AppendLine();
            report.AppendLine("== " + seeds + " random scenarios (seeds 1-" + seeds + ")");
            report.AppendLine(string.Format(CultureInfo.InvariantCulture, "  Arrange now: windows with a glimpse, average of " + (SmallCount + BigCount) + ": start {0:0.0}, after Assist {1:0.0} ({2:0.0} moved), after Strict {3:0.0} ({4:0.0} moved)",
                arrangeBefore / (double)seeds, arrangeAfterAssist / (double)seeds, arrangeMovesAssist / (double)seeds, arrangeAfterStrict / (double)seeds, arrangeMovesStrict / (double)seeds));
            report.AppendLine("  repeated Arrange now that never settles: " + oscillations);
            report.AppendLine("  action                    runs  hid a visible window  Glimpse moved something  all visible again  rule failures");
            var keys = new List<string>(stat.Keys);
            keys.Sort(delegate(string a, string b)
            {
                int ma = Array.IndexOf(Modes, (FixMode)Enum.Parse(typeof(FixMode), a.Split(' ')[0])), mb = Array.IndexOf(Modes, (FixMode)Enum.Parse(typeof(FixMode), b.Split(' ')[0]));
                return ma != mb ? ma.CompareTo(mb) : string.CompareOrdinal(a, b);
            });
            foreach (string key in keys)
            {
                int[] s = stat[key];
                double n = Math.Max(1, s[1]);
                report.AppendLine(string.Format(CultureInfo.InvariantCulture, "  {0,-24} {1,5}  {2,12}          {3,6} ({4,4:0%})        {5,6} ({6,4:0%})   {7}", key, s[0], s[1], s[2], s[2] / n, s[3], s[3] / n, s[4]));
            }
            report.AppendLine("  moves by kind (glimpse = nudge, place = new spot, shrink = smaller, cascade = last resort):");
            foreach (var kv in whys) report.AppendLine("    " + kv.Key.PadRight(34) + kv.Value);
            report.AppendLine();
            report.AppendLine(bad == 0 ? "RULES: all held" : "RULES: " + bad + " failures");
            File.WriteAllText(Path.Combine(outDir, "report.txt"), report.ToString());
            File.WriteAllText(Path.Combine(outDir, "sim-cases.txt"), cases.ToString());
            Console.Write(report.ToString());
            return bad == 0 ? 0 : 1;
        }
    }
}
