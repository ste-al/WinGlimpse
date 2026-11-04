// Glimpse - keep every window within reach.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every open window keeps a "glimpse zone": a corner or edge strip at least P x P pixels
// (50 px at 100% scaling) that no other window overlaps, so any window is one click away.
//
// One source file for two builds:
//   Glimpse.exe  csc.exe from the .NET Framework:  csc /target:winexe /optimize+
//                /win32manifest:Glimpse.manifest /win32icon:Glimpse.ico /out:Glimpse.exe Glimpse.cs
//   Glimpse.ps1  compiles this file with Add-Type and calls Program.RunHosted.
// Keep the language level at C# 5 (the compiler that ships with Windows): no string
// interpolation, no ?. or nameof, no expression-bodied members, no auto-property initializers.
//
// Components (see docs/algorithm.md):
//   Event intake -> Window registry -> Geometry engine -> Resolver -> Actuator
//   plus Overlays (drag feedback, flashes, X-ray), Tray and settings, Settings (Glimpse.ini), Texts.

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;
using WinTimer = System.Windows.Forms.Timer;

[assembly: AssemblyTitle("Glimpse")]
[assembly: AssemblyDescription("Keep every window within reach")]
[assembly: AssemblyProduct("Glimpse")]
[assembly: AssemblyCopyright("Licensed under GPL-3.0-or-later")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]

namespace Glimpse
{
    #region Program: entry points, command line, single instance

    public static class Program
    {
        public const string Version = "1.0.0";
        internal const string MutexName = @"Local\Glimpse.SingleInstance";

        /// <summary>Folder of Glimpse.exe or Glimpse.ps1; Glimpse.ini and lang\ live here by default.</summary>
        internal static string ProgramDir = "";
        /// <summary>Path of Glimpse.exe or Glimpse.ps1, used for Start with Windows.</summary>
        internal static string LaunchPath = "";
        /// <summary>True when Glimpse.ps1 compiled and started this code.</summary>
        internal static bool IsScript;
        internal static Options Opts = new Options();

        [STAThread]
        public static int Main(string[] args)
        {
            string exe = "";
            try { exe = Assembly.GetExecutingAssembly().Location; } catch { }
            if (string.IsNullOrEmpty(exe)) exe = Path.Combine(Environment.CurrentDirectory, "Glimpse.exe");
            return Run(Path.GetDirectoryName(exe), exe, false, args);
        }

        /// <summary>Entry point for Glimpse.ps1. Runs Glimpse on its own STA thread and waits.</summary>
        public static int RunHosted(string scriptDir, string scriptPath, string[] args)
        {
            int code = 0;
            var t = new Thread(delegate() { code = Run(scriptDir, scriptPath, true, args ?? new string[0]); });
            t.SetApartmentState(ApartmentState.STA);
            t.Name = "Glimpse";
            t.Start();
            t.Join();
            return code;
        }

        static int Run(string dir, string launch, bool script, string[] args)
        {
            ProgramDir = dir ?? "";
            LaunchPath = launch ?? "";
            IsScript = script;
            Opts = Options.Parse(args);
            if (Opts.Error != null)
            {
                ConsoleOut.Attach();
                Console.Error.WriteLine(Opts.Error);
                return 2;
            }
            if (Opts.SelfTest) { ConsoleOut.Attach(); return SelfTest.Run(Opts.CasesPath); }
            if (Opts.CheckLang) { ConsoleOut.Attach(); return LangCheck.Run(); }
            if (Opts.DumpLang != null) { ConsoleOut.Attach(); return Texts.DumpBuiltIn(Opts.DumpLang); }
            if (Opts.DumpIni != null)
            {
                ConsoleOut.Attach();
                File.WriteAllText(Opts.DumpIni, Settings.DefaultIniText, new UTF8Encoding(true));
                Console.WriteLine("wrote " + Opts.DumpIni);
                return 0;
            }
            if (Opts.Render != null) { ConsoleOut.Attach(); return Previews.Render(Opts.Render, Opts.RenderLanguage); }
            return RunApp();
        }

        static int RunApp()
        {
            Native.TrySetThreadPerMonitorV2();
            Log.Init(false);
            bool createdNew;
            Mutex mutex;
            try { mutex = new Mutex(true, MutexName, out createdNew); }
            catch (Exception) { createdNew = true; mutex = null; }
            try
            {
                if (!createdNew)
                {
                    // Another Glimpse runs in this session: ask it to show Settings (or to exit).
                    // Started at logon (/background): stay quiet.
                    if (Opts.Background && !Opts.ShowSettings && !Opts.Exit) return 0;
                    Ipc.SignalRunningInstance(Opts.Exit ? Ipc.CmdExit : Ipc.CmdSettings);
                    return 0;
                }
                if (Opts.Exit) return 0;
                try { Application.EnableVisualStyles(); } catch { }
                try { Application.SetCompatibleTextRenderingDefault(false); } catch { }
                Application.ThreadException += delegate(object s, ThreadExceptionEventArgs e) { Log.Error("UI thread", e.Exception); };
                AppDomain.CurrentDomain.UnhandledException += delegate(object s, UnhandledExceptionEventArgs e) { Log.Error("unhandled", e.ExceptionObject as Exception); };
                using (var app = new GlimpseApp())
                {
                    Application.Run(app);
                }
                return 0;
            }
            finally
            {
                if (mutex != null)
                {
                    if (createdNew) { try { mutex.ReleaseMutex(); } catch { } }
                    mutex.Dispose();
                }
            }
        }

        /// <summary>Command line Glimpse would use to start itself (Start with Windows).</summary>
        internal static string StartCommandLine()
        {
            string ini = Opts.IniArgument;
            if (IsScript)
            {
                string cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + LaunchPath + "\" -Background";
                if (!string.IsNullOrEmpty(ini)) cmd += " -Ini \"" + ini + "\"";
                return cmd;
            }
            string line = "\"" + LaunchPath + "\" /background";
            if (!string.IsNullOrEmpty(ini)) line += " \"/ini:" + ini + "\"";
            return line;
        }
    }

    /// <summary>Parsed command line. Accepts /name:value, /name=value, -name value and --name=value.</summary>
    internal sealed class Options
    {
        public string IniArgument;      // as given (environment variables not yet expanded)
        public bool SelfTest;
        public string CasesPath;
        public bool CheckLang;
        public string DumpLang;
        public string DumpIni;
        public string Render;
        public string RenderLanguage;
        public bool ShowSettings;
        public bool Exit;
        public bool Background;     // started at logon: when Glimpse already runs, do nothing
        public string Error;

        public static Options Parse(string[] args)
        {
            var o = new Options();
            if (args == null) return o;
            for (int i = 0; i < args.Length; i++)
            {
                string a = (args[i] ?? "").Trim();
                if (a.Length == 0) continue;
                if (a[0] != '/' && a[0] != '-') { o.Error = "Unknown argument: " + a; return o; }
                string body = a.TrimStart('/', '-');
                string name = body, value = null;
                int sep = body.IndexOfAny(new[] { ':', '=' });
                if (sep > 0) { name = body.Substring(0, sep); value = body.Substring(sep + 1); }
                name = name.ToLowerInvariant();
                switch (name)
                {
                    case "ini":
                        if (value == null && i + 1 < args.Length) value = args[++i];
                        if (string.IsNullOrEmpty(value)) { o.Error = "/ini needs a path, for example /ini:%APPDATA%\\Glimpse\\Glimpse.ini"; return o; }
                        o.IniArgument = value.Trim('"');
                        break;
                    case "selftest":
                        o.SelfTest = true;
                        if (!string.IsNullOrEmpty(value)) o.CasesPath = value.Trim('"');
                        else if (i + 1 < args.Length && args[i + 1].Length > 0 && args[i + 1][0] != '/' && args[i + 1][0] != '-') o.CasesPath = args[++i];
                        break;
                    case "checklang": o.CheckLang = true; break;
                    case "dumplang": o.DumpLang = string.IsNullOrEmpty(value) ? "en.txt" : value.Trim('"'); break;
                    case "dumpini": o.DumpIni = string.IsNullOrEmpty(value) ? "Glimpse.ini" : value.Trim('"'); break;
                    case "render":
                        if (value == null && i + 1 < args.Length) value = args[++i];
                        o.Render = string.IsNullOrEmpty(value) ? "previews" : value.Trim('"');
                        break;
                    case "renderlang": o.RenderLanguage = value; break;
                    case "settings": o.ShowSettings = true; break;
                    case "background": o.Background = true; break;
                    case "exit": o.Exit = true; break;
                    default: o.Error = "Unknown option: " + a; return o;
                }
            }
            return o;
        }
    }

    /// <summary>Talks to the Glimpse that already runs in this session.</summary>
    internal static class Ipc
    {
        public const string WindowTitle = "Glimpse.MessageWindow.9C1D6A52";
        public const int CmdSettings = 1, CmdExit = 2, CmdXRay = 3;
        static uint msg;
        public static uint Message
        {
            get { if (msg == 0 && Native.IsWindows) msg = Native.RegisterWindowMessage("Glimpse.Command.9C1D6A52"); return msg; }
        }

        public static void SignalRunningInstance(int cmd)
        {
            if (!Native.IsWindows) return;
            for (int attempt = 0; attempt < 20; attempt++)
            {
                IntPtr h = Native.FindWindow(null, WindowTitle);
                if (h != IntPtr.Zero)
                {
                    uint pid;
                    Native.GetWindowThreadProcessId(h, out pid);
                    Native.AllowSetForegroundWindow((int)pid);
                    Native.PostMessage(h, Message, new IntPtr(cmd), IntPtr.Zero);
                    return;
                }
                Thread.Sleep(100); // the other instance may still be starting
            }
        }
    }

    /// <summary>Lets a WinExe print to the console it was started from (selftest, checklang).</summary>
    internal static class ConsoleOut
    {
        static bool done;
        public static void Attach()
        {
            if (done) return;
            done = true;
            if (!Native.IsWindows) return;
            try
            {
                IntPtr h = Native.GetStdHandle(-11);
                if (h == IntPtr.Zero || h == new IntPtr(-1)) Native.AttachConsole(-1);
            }
            catch { }
        }
    }

    #endregion

    #region Native: Win32 declarations

    internal static class Native
    {
        public static readonly bool IsWindows = Environment.OSVersion.Platform == PlatformID.Win32NT;

        // ---- messages
        public const int WM_DESTROY = 0x0002, WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008,
            WM_PAINT = 0x000F, WM_CLOSE = 0x0010, WM_ERASEBKGND = 0x0014, WM_SETTINGCHANGE = 0x001A, WM_ACTIVATEAPP = 0x001C,
            WM_SETCURSOR = 0x0020, WM_MOUSEACTIVATE = 0x0021, WM_GETMINMAXINFO = 0x0024, WM_DISPLAYCHANGE = 0x007E,
            WM_GETICON = 0x007F, WM_NCHITTEST = 0x0084, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102,
            WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105, WM_SYSCHAR = 0x0106, WM_TIMER = 0x0113, WM_MOUSEMOVE = 0x0200,
            WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204,
            WM_RBUTTONUP = 0x0205, WM_MOUSEWHEEL = 0x020A, WM_WTSSESSION_CHANGE = 0x02B1, WM_DPICHANGED = 0x02E0,
            WM_HOTKEY = 0x0312, WM_NCLBUTTONDOWN = 0x00A1;
        public const int WA_INACTIVE = 0, MA_NOACTIVATE = 3;

        // ---- hit-test results
        public const int HTERROR = -2, HTTRANSPARENT = -1, HTNOWHERE = 0, HTCLIENT = 1, HTCAPTION = 2, HTSYSMENU = 3,
            HTMENU = 5, HTMINBUTTON = 8, HTMAXBUTTON = 9, HTLEFT = 10, HTRIGHT = 11, HTTOP = 12, HTTOPLEFT = 13,
            HTTOPRIGHT = 14, HTBOTTOM = 15, HTBOTTOMLEFT = 16, HTBOTTOMRIGHT = 17, HTBORDER = 18, HTCLOSE = 20, HTHELP = 21;

        // ---- WinEvents
        public const uint EVENT_SYSTEM_FOREGROUND = 0x0003, EVENT_SYSTEM_MOVESIZESTART = 0x000A, EVENT_SYSTEM_MOVESIZEEND = 0x000B,
            EVENT_SYSTEM_MINIMIZESTART = 0x0016, EVENT_SYSTEM_MINIMIZEEND = 0x0017, EVENT_OBJECT_DESTROY = 0x8001,
            EVENT_OBJECT_SHOW = 0x8002, EVENT_OBJECT_HIDE = 0x8003, EVENT_OBJECT_LOCATIONCHANGE = 0x800B,
            EVENT_OBJECT_NAMECHANGE = 0x800C, EVENT_OBJECT_CLOAKED = 0x8017, EVENT_OBJECT_UNCLOAKED = 0x8018;
        public const uint WINEVENT_OUTOFCONTEXT = 0x0000, WINEVENT_SKIPOWNPROCESS = 0x0002;
        public const int OBJID_WINDOW = 0, CHILDID_SELF = 0;

        public delegate void WinEventProc(IntPtr hook, uint evt, IntPtr hwnd, int idObject, int idChild, uint thread, uint time);
        [DllImport("user32.dll")] public static extern IntPtr SetWinEventHook(uint eventMin, uint eventMax, IntPtr hmod, WinEventProc proc, uint pid, uint tid, uint flags);
        [DllImport("user32.dll")] public static extern bool UnhookWinEvent(IntPtr hook);

        // ---- structures
        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int Left, Top, Right, Bottom; }
        [StructLayout(LayoutKind.Sequential)]
        public struct POINT { public int X, Y; public POINT(int x, int y) { X = x; Y = y; } }
        [StructLayout(LayoutKind.Sequential)]
        public struct SIZE { public int cx, cy; public SIZE(int x, int y) { cx = x; cy = y; } }

        // ---- windows
        public const int GWL_STYLE = -16, GWL_EXSTYLE = -20;
        public const long WS_POPUP = 0x80000000L, WS_CHILD = 0x40000000L, WS_MINIMIZE = 0x20000000L, WS_VISIBLE = 0x10000000L,
            WS_DISABLED = 0x08000000L, WS_MAXIMIZE = 0x01000000L, WS_CAPTION = 0x00C00000L, WS_SYSMENU = 0x00080000L,
            WS_THICKFRAME = 0x00040000L, WS_MINIMIZEBOX = 0x00020000L, WS_MAXIMIZEBOX = 0x00010000L;
        public const long WS_EX_TOPMOST = 0x8L, WS_EX_TRANSPARENT = 0x20L, WS_EX_TOOLWINDOW = 0x80L, WS_EX_APPWINDOW = 0x40000L,
            WS_EX_LAYERED = 0x80000L, WS_EX_NOACTIVATE = 0x08000000L;
        public const uint GA_PARENT = 1, GA_ROOT = 2, GA_ROOTOWNER = 3, GW_OWNER = 4;
        public const uint SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOZORDER = 0x4, SWP_NOACTIVATE = 0x10, SWP_SHOWWINDOW = 0x40,
            SWP_HIDEWINDOW = 0x80, SWP_NOOWNERZORDER = 0x200, SWP_ASYNCWINDOWPOS = 0x4000;
        public static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
        public const int SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_SHOWMINIMIZED = 2, SW_SHOWNOACTIVATE = 4, SW_SHOW = 5, SW_SHOWNA = 8, SW_RESTORE = 9;

        public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
        [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
        [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hwnd, uint cmd);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder sb, int max);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextLength(IntPtr hwnd);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder sb, int max);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll", EntryPoint = "GetWindowLong")] static extern int GetWindowLong32(IntPtr hwnd, int index);
        [DllImport("user32.dll", EntryPoint = "GetWindowLongPtr")] static extern IntPtr GetWindowLongPtr64(IntPtr hwnd, int index);
        [DllImport("user32.dll", EntryPoint = "GetClassLong")] static extern uint GetClassLong32(IntPtr hwnd, int index);
        [DllImport("user32.dll", EntryPoint = "GetClassLongPtr")] static extern IntPtr GetClassLongPtr64(IntPtr hwnd, int index);
        [DllImport("user32.dll", SetLastError = true)] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
        [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hwnd, int cmd);
        [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int pid);
        [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool attach);
        [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
        [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
        [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
        [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
        [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessage(string name);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
        [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr hIcon);
        public const uint SMTO_BLOCK = 0x1, SMTO_ABORTIFHUNG = 0x2;
        public const int ICON_SMALL = 0, ICON_BIG = 1, ICON_SMALL2 = 2, GCLP_HICON = -14, GCLP_HICONSM = -34;

        public static long GetWindowLongPtr(IntPtr hwnd, int index)
        {
            return IntPtr.Size == 8 ? GetWindowLongPtr64(hwnd, index).ToInt64() : (long)(uint)GetWindowLong32(hwnd, index);
        }
        public static IntPtr GetClassLongPtr(IntPtr hwnd, int index)
        {
            return IntPtr.Size == 8 ? GetClassLongPtr64(hwnd, index) : new IntPtr((long)GetClassLong32(hwnd, index));
        }

        // ---- cursor, keyboard
        [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
        [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
        public const int VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
            VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5;
        public static bool KeyDown(int vk) { return IsWindows && (GetAsyncKeyState(vk) & 0x8000) != 0; }

        [DllImport("user32.dll", SetLastError = true)] public static extern bool RegisterHotKey(IntPtr hwnd, int id, uint mods, uint vk);
        [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr hwnd, int id);
        public const uint MOD_ALT = 0x1, MOD_CONTROL = 0x2, MOD_SHIFT = 0x4, MOD_WIN = 0x8, MOD_NOREPEAT = 0x4000;
        [DllImport("user32.dll")] public static extern int GetKeyboardLayoutList(int n, [Out] IntPtr[] list);
        [DllImport("user32.dll")] public static extern IntPtr GetKeyboardLayout(uint thread);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int ToUnicodeEx(uint vk, uint scan, byte[] state, [Out] StringBuilder buf, int cch, uint flags, IntPtr hkl);
        [DllImport("user32.dll")] public static extern uint MapVirtualKeyEx(uint code, uint mapType, IntPtr hkl);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetKeyNameText(int lParam, [Out] StringBuilder sb, int size);
        public const uint MAPVK_VK_TO_VSC = 0;

        // ---- monitors and DPI
        public delegate bool MonitorEnumProc(IntPtr hmon, IntPtr hdc, ref RECT r, IntPtr data);
        [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc cb, IntPtr data);
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct MONITORINFOEX
        {
            public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
        }
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr hmon, ref MONITORINFOEX mi);
        [DllImport("user32.dll")] public static extern IntPtr MonitorFromPoint(POINT p, uint flags);
        [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
        public const uint MONITOR_DEFAULTTONULL = 0, MONITOR_DEFAULTTOPRIMARY = 1, MONITOR_DEFAULTTONEAREST = 2;
        [DllImport("shcore.dll")] public static extern int GetDpiForMonitor(IntPtr hmon, int type, out uint dpiX, out uint dpiY);
        [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
        [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
        [DllImport("user32.dll")] public static extern bool PhysicalToLogicalPointForPerMonitorDPI(IntPtr hwnd, ref POINT p);
        [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint action, uint param, out int value, uint winIni);
        [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
        public const uint SPI_GETDRAGFULLWINDOWS = 0x0026;
        public const int SPI_SETWORKAREA = 0x002F;
        public const int SM_CXSMICON = 49, SM_CYSMICON = 50, SM_CXDOUBLECLK = 36;

        public static void TrySetThreadPerMonitorV2()
        {
            if (!IsWindows) return;
            try { SetThreadDpiAwarenessContext(new IntPtr(-4)); } // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            catch (EntryPointNotFoundException) { }
            catch (DllNotFoundException) { }
        }

        public static int DpiForMonitor(IntPtr hmon)
        {
            if (!IsWindows || hmon == IntPtr.Zero) return 96;
            try
            {
                uint x, y;
                if (GetDpiForMonitor(hmon, 0, out x, out y) == 0 && x > 0) return (int)x;
            }
            catch (EntryPointNotFoundException) { }
            catch (DllNotFoundException) { }
            return 96;
        }

        public static int DpiForWindow(IntPtr hwnd)
        {
            if (!IsWindows || hwnd == IntPtr.Zero) return 96;
            try { uint d = GetDpiForWindow(hwnd); if (d > 0) return (int)d; }
            catch (EntryPointNotFoundException) { }
            return DpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
        }

        public static bool DragFullWindows()
        {
            if (!IsWindows) return true;
            int v;
            return !SystemParametersInfo(SPI_GETDRAGFULLWINDOWS, 0, out v, 0) || v != 0;
        }

        // ---- DWM
        [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out RECT value, int size);
        [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out int value, int size);
        [DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
        public const int DWMWA_CAPTION_BUTTON_BOUNDS = 5, DWMWA_EXTENDED_FRAME_BOUNDS = 9, DWMWA_CLOAKED = 14,
            DWMWA_USE_IMMERSIVE_DARK_MODE_OLD = 19, DWMWA_USE_IMMERSIVE_DARK_MODE = 20, DWMWA_WINDOW_CORNER_PREFERENCE = 33;

        public static bool TryFrame(IntPtr hwnd, out RECT r)
        {
            try
            {
                if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out r, Marshal.SizeOf(typeof(RECT))) == 0
                    && r.Right > r.Left && r.Bottom > r.Top) return true;
            }
            catch (DllNotFoundException) { }
            catch (EntryPointNotFoundException) { }
            return GetWindowRect(hwnd, out r);
        }

        public static bool IsCloaked(IntPtr hwnd)
        {
            try { int v; return DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, out v, 4) == 0 && v != 0; }
            catch (DllNotFoundException) { return false; }
            catch (EntryPointNotFoundException) { return false; }
        }

        public static void SetDarkTitleBar(IntPtr hwnd, bool dark)
        {
            if (!IsWindows) return;
            try
            {
                int v = dark ? 1 : 0;
                if (DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref v, 4) != 0)
                    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, ref v, 4);
            }
            catch { }
        }

        public static void SetRoundCorners(IntPtr hwnd, bool small)
        {
            if (!IsWindows) return;
            try { int v = small ? 3 : 2; DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref v, 4); }
            catch { }
        }

        // ---- processes and integrity levels
        [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool QueryFullProcessImageName(IntPtr process, uint flags, [Out] StringBuilder sb, ref uint size);
        [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
        [DllImport("kernel32.dll")] public static extern bool ProcessIdToSessionId(uint pid, out uint session);
        [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int which);
        [DllImport("kernel32.dll")] public static extern bool AttachConsole(int pid);
        [DllImport("kernel32.dll")] public static extern ushort GetUserDefaultUILanguage();
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr GetModuleHandle(string name);
        [DllImport("advapi32.dll", SetLastError = true)] public static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("advapi32.dll", SetLastError = true)] public static extern bool GetTokenInformation(IntPtr token, int cls, IntPtr info, int length, out int returned);
        [DllImport("advapi32.dll")] public static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
        [DllImport("advapi32.dll")] public static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
        public const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000, TOKEN_QUERY = 0x0008;
        public const int TokenIntegrityLevel = 25;

        /// <summary>Mandatory integrity level of a process (0x2000 medium, 0x3000 high), or -1 when it cannot be read.</summary>
        public static int IntegrityLevel(uint pid)
        {
            IntPtr proc = pid == 0 ? IntPtr.Zero : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (pid != 0 && proc == IntPtr.Zero) return -1;
            IntPtr token = IntPtr.Zero, buf = IntPtr.Zero;
            try
            {
                if (!OpenProcessToken(pid == 0 ? GetCurrentProcess() : proc, TOKEN_QUERY, out token)) return -1;
                int len;
                GetTokenInformation(token, TokenIntegrityLevel, IntPtr.Zero, 0, out len);
                if (len <= 0) return -1;
                buf = Marshal.AllocHGlobal(len);
                if (!GetTokenInformation(token, TokenIntegrityLevel, buf, len, out len)) return -1;
                IntPtr sid = Marshal.ReadIntPtr(buf); // TOKEN_MANDATORY_LABEL.Label.Sid
                int count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
                if (count <= 0) return -1;
                return Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
            }
            catch { return -1; }
            finally
            {
                if (buf != IntPtr.Zero) Marshal.FreeHGlobal(buf);
                if (token != IntPtr.Zero) CloseHandle(token);
                if (proc != IntPtr.Zero) CloseHandle(proc);
            }
        }

        public static string ProcessFileName(uint pid)
        {
            IntPtr proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (proc == IntPtr.Zero) return "";
            try
            {
                var sb = new StringBuilder(1024);
                uint size = (uint)sb.Capacity;
                if (QueryFullProcessImageName(proc, 0, sb, ref size)) return Path.GetFileName(sb.ToString());
                return "";
            }
            catch { return ""; }
            finally { CloseHandle(proc); }
        }

        // ---- session notifications
        [DllImport("wtsapi32.dll")] public static extern bool WTSRegisterSessionNotification(IntPtr hwnd, int flags);
        [DllImport("wtsapi32.dll")] public static extern bool WTSUnRegisterSessionNotification(IntPtr hwnd);
        public const int NOTIFY_FOR_THIS_SESSION = 0;
        public const int WTS_CONSOLE_CONNECT = 1, WTS_CONSOLE_DISCONNECT = 2, WTS_REMOTE_CONNECT = 3, WTS_REMOTE_DISCONNECT = 4,
            WTS_SESSION_LOCK = 7, WTS_SESSION_UNLOCK = 8;

        // ---- layered windows and GDI
        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct BLENDFUNCTION { public byte BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat; }
        [StructLayout(LayoutKind.Sequential)]
        public struct UPDATELAYEREDWINDOWINFO
        {
            public int cbSize; public IntPtr hdcDst; public IntPtr pptDst; public IntPtr psize; public IntPtr hdcSrc;
            public IntPtr pptSrc; public uint crKey; public IntPtr pblend; public uint dwFlags; public IntPtr prcDirty;
        }
        [StructLayout(LayoutKind.Sequential)]
        public struct BITMAPINFOHEADER
        {
            public int biSize, biWidth, biHeight; public short biPlanes, biBitCount; public int biCompression, biSizeImage,
                biXPelsPerMeter, biYPelsPerMeter, biClrUsed, biClrImportant;
        }
        [DllImport("user32.dll", SetLastError = true)] public static extern bool UpdateLayeredWindowIndirect(IntPtr hwnd, ref UPDATELAYEREDWINDOWINFO info);
        [DllImport("user32.dll", SetLastError = true)] public static extern bool UpdateLayeredWindow(IntPtr hwnd, IntPtr hdcDst, ref POINT pptDst, ref SIZE psize, IntPtr hdcSrc, ref POINT pptSrc, uint crKey, ref BLENDFUNCTION blend, uint flags);
        [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr hdc);
        [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr hdc);
        [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr hdc, IntPtr obj);
        [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr obj);
        [DllImport("gdi32.dll")] public static extern IntPtr CreateDIBSection(IntPtr hdc, ref BITMAPINFOHEADER bmi, uint usage, out IntPtr bits, IntPtr section, uint offset);
        public const uint ULW_ALPHA = 0x2;
        public const byte AC_SRC_OVER = 0, AC_SRC_ALPHA = 1;

        // ---- low-level mouse hook (experimental first-click mode)
        public delegate IntPtr LowLevelProc(int code, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr SetWindowsHookEx(int id, LowLevelProc proc, IntPtr hmod, uint threadId);
        [DllImport("user32.dll")] public static extern bool UnhookWindowsHookEx(IntPtr hook);
        [DllImport("user32.dll")] public static extern IntPtr CallNextHookEx(IntPtr hook, int code, IntPtr wParam, IntPtr lParam);
        public const int WH_MOUSE_LL = 14;
        [StructLayout(LayoutKind.Sequential)]
        public struct MSLLHOOKSTRUCT { public POINT pt; public uint mouseData, flags, time; public IntPtr extra; }

        /// <summary>Lets this process take the foreground when no input of ours caused it (hot corner).</summary>
        public static void ForceForeground(IntPtr hwnd)
        {
            if (!IsWindows || hwnd == IntPtr.Zero) return;
            if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return;
            IntPtr fg = GetForegroundWindow();
            uint pid;
            uint fgThread = fg == IntPtr.Zero ? 0 : GetWindowThreadProcessId(fg, out pid);
            uint me = GetCurrentThreadId();
            bool attached = fgThread != 0 && fgThread != me && AttachThreadInput(me, fgThread, true);
            try
            {
                BringWindowToTop(hwnd);
                SetForegroundWindow(hwnd);
            }
            finally
            {
                if (attached) AttachThreadInput(me, fgThread, false);
            }
        }
    }

    #endregion

    #region Utilities: log, clock, rectangles, intervals

    internal static class Log
    {
        static readonly object Gate = new object();
        static string path;
        static bool debug;
        static string session = "?";

        public static bool DebugEnabled { get { return debug; } }

        public static string Folder
        {
            get
            {
                string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (string.IsNullOrEmpty(local)) local = Path.GetTempPath();
                return Path.Combine(local, "Glimpse");
            }
        }

        public static string FilePath { get { return path; } }

        public static void Init(bool debugLevel)
        {
            debug = debugLevel;
            try
            {
                string dir = Folder;
                Directory.CreateDirectory(dir);
                path = Path.Combine(dir, "Glimpse.log");
                if (Native.IsWindows)
                {
                    uint s;
                    if (Native.ProcessIdToSessionId((uint)Process.GetCurrentProcess().Id, out s)) session = s.ToString(CultureInfo.InvariantCulture);
                }
            }
            catch { path = null; }
        }

        public static void SetDebug(bool on) { debug = on; }
        public static void Info(string msg) { Write("INFO ", msg); }
        public static void Warn(string msg) { Write("WARN ", msg); }
        public static void Debug(string msg) { if (debug) Write("DEBUG", msg); }
        public static void Error(string msg, Exception ex) { Write("ERROR", ex == null ? msg : msg + ": " + ex); }

        static void Write(string level, string msg)
        {
            if (path == null) return;
            lock (Gate)
            {
                try
                {
                    var fi = new FileInfo(path);
                    if (fi.Exists && fi.Length > 1024 * 1024)
                    {
                        string old = path + ".1";
                        try { File.Delete(old); } catch { }
                        try { File.Move(path, old); } catch { }
                    }
                    string line = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture)
                        + " [" + session + "] " + level + " " + msg + "\r\n";
                    byte[] b = Encoding.UTF8.GetBytes(line);
                    using (var fs = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete))
                        fs.Write(b, 0, b.Length);
                }
                catch { }
            }
        }
    }

    /// <summary>Monotonic milliseconds.</summary>
    internal static class Clock
    {
        static readonly Stopwatch Watch = Stopwatch.StartNew();
        public static long Ms { get { return Watch.ElapsedMilliseconds; } }
    }

    internal static class Util
    {
        public static bool ParseBool(string s, bool fallback)
        {
            if (s == null) return fallback;
            switch (s.Trim().ToLowerInvariant())
            {
                case "1": case "true": case "yes": case "on": return true;
                case "0": case "false": case "no": case "off": return false;
                default: return fallback;
            }
        }

        public static int ParseInt(string s, int fallback, int min, int max)
        {
            int v;
            if (s == null || !int.TryParse(s.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out v)) return fallback;
            return Math.Max(min, Math.Min(max, v));
        }

        public static string Inv(int v) { return v.ToString(CultureInfo.InvariantCulture); }
        public static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
        public static int Scale(int px, int dpi) { return (int)Math.Round(px * dpi / 96.0); }

        /// <summary>Title with digit runs replaced by #, so "Order #4711" and "Order #4712" share a kind.</summary>
        public static string TitleShape(string title)
        {
            if (string.IsNullOrEmpty(title)) return "";
            var sb = new StringBuilder(title.Length);
            bool inDigits = false;
            foreach (char c in title)
            {
                if (char.IsDigit(c)) { if (!inDigits) sb.Append('#'); inDigits = true; }
                else { sb.Append(c); inDigits = false; }
            }
            string s = sb.ToString();
            return s.Length > 80 ? s.Substring(0, 80) : s;
        }

        public static string Shorten(string s, int max)
        {
            if (s == null) return "";
            return s.Length <= max ? s : s.Substring(0, Math.Max(0, max - 1)) + "\u2026";
        }
    }

    /// <summary>Integer rectangle, right and bottom exclusive (like RECT).</summary>
    internal struct Rc
    {
        public int L, T, R, B;
        public Rc(int l, int t, int r, int b) { L = l; T = t; R = r; B = b; }
        public static readonly Rc Empty = new Rc(0, 0, 0, 0);
        public static Rc XYWH(int x, int y, int w, int h) { return new Rc(x, y, x + w, y + h); }
        public int W { get { return R - L; } }
        public int H { get { return B - T; } }
        public bool IsEmpty { get { return R <= L || B <= T; } }
        public long Area { get { return IsEmpty ? 0 : (long)W * H; } }
        public int CX { get { return L + W / 2; } }
        public int CY { get { return T + H / 2; } }
        public bool Intersects(Rc o) { return L < o.R && o.L < R && T < o.B && o.T < B; }
        public Rc Intersect(Rc o)
        {
            var r = new Rc(Math.Max(L, o.L), Math.Max(T, o.T), Math.Min(R, o.R), Math.Min(B, o.B));
            return r.IsEmpty ? new Rc(0, 0, 0, 0) : r;
        }
        public Rc Union(Rc o)
        {
            if (IsEmpty) return o;
            if (o.IsEmpty) return this;
            return new Rc(Math.Min(L, o.L), Math.Min(T, o.T), Math.Max(R, o.R), Math.Max(B, o.B));
        }
        public bool Contains(Rc o) { return o.L >= L && o.T >= T && o.R <= R && o.B <= B; }
        public bool Contains(int x, int y) { return x >= L && x < R && y >= T && y < B; }
        public Rc Offset(int dx, int dy) { return new Rc(L + dx, T + dy, R + dx, B + dy); }
        public Rc Inflate(int d) { return new Rc(L - d, T - d, R + d, B + d); }
        public bool Same(Rc o) { return L == o.L && T == o.T && R == o.R && B == o.B; }
        public bool Near(Rc o, int tol) { return Math.Abs(L - o.L) <= tol && Math.Abs(T - o.T) <= tol && Math.Abs(R - o.R) <= tol && Math.Abs(B - o.B) <= tol; }
        public Rectangle ToRectangle() { return new Rectangle(L, T, W, H); }
        public static Rc From(Native.RECT r) { return new Rc(r.Left, r.Top, r.Right, r.Bottom); }
        public static Rc From(Rectangle r) { return new Rc(r.Left, r.Top, r.Right, r.Bottom); }
        public override string ToString() { return Util.Inv(L) + "," + Util.Inv(T) + "," + Util.Inv(R) + "," + Util.Inv(B); }

        /// <summary>Distance in px from this rectangle to a point (0 inside).</summary>
        public double DistanceTo(int x, int y)
        {
            int dx = x < L ? L - x : (x >= R ? x - R + 1 : 0);
            int dy = y < T ? T - y : (y >= B ? y - B + 1 : 0);
            return Math.Sqrt((double)dx * dx + (double)dy * dy);
        }
    }

    /// <summary>Sorted set of disjoint closed integer intervals [lo, hi].</summary>
    internal sealed class IntervalSet
    {
        readonly List<int> v = new List<int>(); // lo0, hi0, lo1, hi1, ...

        public static IntervalSet Of(int lo, int hi)
        {
            var s = new IntervalSet();
            if (lo <= hi) { s.v.Add(lo); s.v.Add(hi); }
            return s;
        }

        public bool IsEmpty { get { return v.Count == 0; } }
        public int Count { get { return v.Count / 2; } }
        public int Lo(int i) { return v[2 * i]; }
        public int Hi(int i) { return v[2 * i + 1]; }
        public int First { get { return v[0]; } }

        public void Add(int lo, int hi)
        {
            if (lo > hi) return;
            var n = new List<int>(v.Count + 2);
            bool placed = false;
            for (int i = 0; i < v.Count; i += 2)
            {
                int a = v[i], b = v[i + 1];
                if (b < lo - 1) { n.Add(a); n.Add(b); }
                else if (a > hi + 1)
                {
                    if (!placed) { n.Add(lo); n.Add(hi); placed = true; }
                    n.Add(a); n.Add(b);
                }
                else { lo = Math.Min(lo, a); hi = Math.Max(hi, b); }
            }
            if (!placed) { n.Add(lo); n.Add(hi); }
            v.Clear();
            v.AddRange(n);
        }

        public void Subtract(int lo, int hi)
        {
            if (lo > hi || v.Count == 0) return;
            var n = new List<int>(v.Count + 2);
            for (int i = 0; i < v.Count; i += 2)
            {
                int a = v[i], b = v[i + 1];
                if (b < lo || a > hi) { n.Add(a); n.Add(b); continue; }
                if (a < lo) { n.Add(a); n.Add(lo - 1); }
                if (b > hi) { n.Add(hi + 1); n.Add(b); }
            }
            v.Clear();
            v.AddRange(n);
        }

        public IntervalSet Intersect(IntervalSet o)
        {
            var r = new IntervalSet();
            int i = 0, j = 0;
            while (i < v.Count && j < o.v.Count)
            {
                int lo = Math.Max(v[i], o.v[j]), hi = Math.Min(v[i + 1], o.v[j + 1]);
                if (lo <= hi) { r.v.Add(lo); r.v.Add(hi); }
                if (v[i + 1] < o.v[j + 1]) i += 2; else j += 2;
            }
            return r;
        }

        public bool Contains(int x)
        {
            for (int i = 0; i < v.Count; i += 2) if (x >= v[i] && x <= v[i + 1]) return true;
            return false;
        }

        public override string ToString()
        {
            var sb = new StringBuilder();
            for (int i = 0; i < v.Count; i += 2) sb.Append('[').Append(v[i]).Append(',').Append(v[i + 1]).Append(']');
            return sb.ToString();
        }
    }

    #endregion

    #region Settings: Glimpse.ini reader and writer, settings model

    /// <summary>INI file kept as lines, so comments and order survive a save.
    /// Reads UTF-8 with or without BOM and UTF-16 (LE/BE); writes back in the same encoding.</summary>
    internal sealed class IniFile
    {
        public readonly List<string> Lines = new List<string>();
        public Encoding Enc = new UTF8Encoding(false);
        public bool Bom;
        public string NewLine = "\r\n";

        public static IniFile Parse(byte[] data)
        {
            var f = new IniFile();
            int skip = 0;
            if (data.Length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) { f.Enc = new UTF8Encoding(false); f.Bom = true; skip = 3; }
            else if (data.Length >= 2 && data[0] == 0xFF && data[1] == 0xFE) { f.Enc = new UnicodeEncoding(false, false); f.Bom = true; skip = 2; }
            else if (data.Length >= 2 && data[0] == 0xFE && data[1] == 0xFF) { f.Enc = new UnicodeEncoding(true, false); f.Bom = true; skip = 2; }
            else if (LooksUtf16(data, false)) f.Enc = new UnicodeEncoding(false, false);
            else if (LooksUtf16(data, true)) f.Enc = new UnicodeEncoding(true, false);
            else
            {
                try { new UTF8Encoding(false, true).GetString(data); f.Enc = new UTF8Encoding(false); }
                catch (DecoderFallbackException) { f.Enc = Encoding.Default; }
            }
            string text = f.Enc.GetString(data, skip, data.Length - skip);
            f.SetText(text);
            return f;
        }

        public static IniFile FromText(string text)
        {
            var f = new IniFile();
            f.SetText(text);
            return f;
        }

        static bool LooksUtf16(byte[] d, bool bigEndian)
        {
            if (d.Length < 4 || d.Length % 2 != 0) return false;
            int zeros = 0, n = Math.Min(d.Length, 400);
            for (int i = bigEndian ? 0 : 1; i < n; i += 2) if (d[i] == 0) zeros++;
            return zeros > n / 2 * 0.8;
        }

        void SetText(string text)
        {
            Lines.Clear();
            NewLine = text.Contains("\r\n") || !text.Contains("\n") ? "\r\n" : "\n";
            string[] parts = text.Replace("\r\n", "\n").Split('\n');
            int count = parts.Length;
            if (count > 0 && parts[count - 1].Length == 0) count--; // trailing newline
            for (int i = 0; i < count; i++) Lines.Add(parts[i].TrimEnd('\r'));
        }

        public string Text
        {
            get
            {
                var sb = new StringBuilder();
                foreach (string l in Lines) sb.Append(l).Append(NewLine);
                return sb.ToString();
            }
        }

        public byte[] ToBytes()
        {
            byte[] body = Enc.GetBytes(Text);
            if (!Bom) return body;
            byte[] pre = Enc.GetPreamble();
            if (pre.Length == 0)
            {
                if (Enc is UTF8Encoding) pre = new byte[] { 0xEF, 0xBB, 0xBF };
                else if (Enc is UnicodeEncoding) pre = Enc.GetBytes("\ufeff");
            }
            var all = new byte[pre.Length + body.Length];
            Buffer.BlockCopy(pre, 0, all, 0, pre.Length);
            Buffer.BlockCopy(body, 0, all, pre.Length, body.Length);
            return all;
        }

        static bool IsComment(string t) { return t.Length == 0 || t[0] == ';' || t[0] == '#'; }

        static string SectionName(string line)
        {
            string t = line.Trim();
            if (t.Length < 2 || t[0] != '[') return null;
            int e = t.IndexOf(']');
            return e > 0 ? t.Substring(1, e - 1).Trim() : null;
        }

        static bool SplitKey(string line, out string key, out string value)
        {
            key = value = null;
            string t = line.Trim();
            if (IsComment(t) || t[0] == '[') return false;
            int eq = t.IndexOf('=');
            if (eq <= 0) return false;
            key = t.Substring(0, eq).Trim();
            value = t.Substring(eq + 1).Trim();
            return key.Length > 0;
        }

        int FindSection(string section)
        {
            for (int i = 0; i < Lines.Count; i++)
            {
                string s = SectionName(Lines[i]);
                if (s != null && string.Equals(s, section, StringComparison.OrdinalIgnoreCase)) return i;
            }
            return -1;
        }

        int SectionEnd(int header)
        {
            for (int i = header + 1; i < Lines.Count; i++) if (SectionName(Lines[i]) != null) return i;
            return Lines.Count;
        }

        int LastKeyLine(int header)
        {
            int end = SectionEnd(header), last = header;
            for (int i = header + 1; i < end; i++)
            {
                string k, v;
                if (SplitKey(Lines[i], out k, out v)) last = i;
            }
            return last;
        }

        int FindKey(string section, string key)
        {
            int h = FindSection(section);
            if (h < 0) return -1;
            int end = SectionEnd(h);
            for (int i = h + 1; i < end; i++)
            {
                string k, v;
                if (SplitKey(Lines[i], out k, out v) && string.Equals(k, key, StringComparison.OrdinalIgnoreCase)) return i;
            }
            return -1;
        }

        public bool HasSection(string section) { return FindSection(section) >= 0; }

        public string Get(string section, string key)
        {
            int i = FindKey(section, key);
            if (i < 0) return null;
            string k, v;
            SplitKey(Lines[i], out k, out v);
            return v;
        }

        public List<string> Sections()
        {
            var r = new List<string>();
            foreach (string l in Lines) { string s = SectionName(l); if (s != null) r.Add(s); }
            return r;
        }

        public List<KeyValuePair<string, string>> Entries(string section)
        {
            var r = new List<KeyValuePair<string, string>>();
            int h = FindSection(section);
            if (h < 0) return r;
            int end = SectionEnd(h);
            for (int i = h + 1; i < end; i++)
            {
                string k, v;
                if (SplitKey(Lines[i], out k, out v)) r.Add(new KeyValuePair<string, string>(k, v));
            }
            return r;
        }

        public void Set(string section, string key, string value)
        {
            int i = FindKey(section, key);
            if (i >= 0)
            {
                string k, v;
                SplitKey(Lines[i], out k, out v);
                Lines[i] = k + "=" + value;
                return;
            }
            int h = FindSection(section);
            if (h < 0)
            {
                if (Lines.Count > 0 && Lines[Lines.Count - 1].Trim().Length != 0) Lines.Add("");
                Lines.Add("[" + section + "]");
                Lines.Add(key + "=" + value);
                return;
            }
            Lines.Insert(LastKeyLine(h) + 1, key + "=" + value);
        }

        public void RemoveKey(string section, string key)
        {
            int i = FindKey(section, key);
            if (i >= 0) Lines.RemoveAt(i);
        }

        /// <summary>Removes the header and the lines up to its last key; a comment block after the
        /// last key (usually describing the next section) stays. Returns the index where it was.</summary>
        public int RemoveSection(string section)
        {
            int h = FindSection(section);
            if (h < 0) return -1;
            int last = LastKeyLine(h);
            Lines.RemoveRange(h, last - h + 1);
            // drop one blank line left behind before the next content
            if (h < Lines.Count && h > 0 && Lines[h].Trim().Length == 0 && Lines[h - 1].Trim().Length == 0) Lines.RemoveAt(h);
            return h;
        }

        /// <summary>Inserts a section before line <paramref name="at"/>, or appends it when at is -1.</summary>
        public void InsertSection(int at, string section, List<KeyValuePair<string, string>> entries)
        {
            var block = new List<string>();
            block.Add("[" + section + "]");
            foreach (var kv in entries) block.Add(kv.Key + "=" + kv.Value);
            if (at < 0 || at >= Lines.Count)
            {
                if (Lines.Count > 0 && Lines[Lines.Count - 1].Trim().Length != 0) Lines.Add("");
                Lines.AddRange(block);
                return;
            }
            if (Lines[at].Trim().Length != 0) block.Add("");
            Lines.InsertRange(at, block);
        }
    }

    internal enum FixMode { Guide, Assist, Strict }
    internal enum Corner { Off, TopLeft, TopRight, BottomLeft, BottomRight }

    internal sealed class MinSizeRule
    {
        public string Pattern = ".*";
        public int MinWidth = 200, MinHeight = 200;
        public string Note = "";
        public Regex Regex;
        public string Error;

        public bool Compile()
        {
            Regex = null;
            Error = null;
            try
            {
                Regex = new Regex(Pattern ?? "", RegexOptions.ECMAScript | RegexOptions.IgnoreCase, TimeSpan.FromMilliseconds(50));
                return true;
            }
            catch (ArgumentException ex) { Error = ex.Message; return false; }
        }

        public bool Matches(string title)
        {
            if (Regex == null) return false;
            try { return Regex.IsMatch(title ?? ""); }
            catch (RegexMatchTimeoutException) { return false; }
        }

        public MinSizeRule Clone()
        {
            var r = new MinSizeRule { Pattern = Pattern, MinWidth = MinWidth, MinHeight = MinHeight, Note = Note };
            r.Compile();
            return r;
        }
    }

    internal sealed class Exclusion
    {
        public bool IsProcess;
        public string Value = "";
        public Regex Regex;
        public string Error;

        public static Exclusion Process(string name) { return new Exclusion { IsProcess = true, Value = name.Trim() }; }
        public static Exclusion Title(string pattern)
        {
            var e = new Exclusion { IsProcess = false, Value = pattern };
            try { e.Regex = new Regex(pattern, RegexOptions.ECMAScript | RegexOptions.IgnoreCase, TimeSpan.FromMilliseconds(50)); }
            catch (ArgumentException ex) { e.Error = ex.Message; }
            return e;
        }

        public bool Matches(string process, string title)
        {
            if (IsProcess)
            {
                if (string.IsNullOrEmpty(process)) return false;
                string v = Value.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? Value : Value + ".exe";
                return string.Equals(process, v, StringComparison.OrdinalIgnoreCase);
            }
            if (Regex == null) return false;
            try { return Regex.IsMatch(title ?? ""); }
            catch (RegexMatchTimeoutException) { return false; }
        }
    }

    internal sealed class Settings
    {
        public FixMode Mode = FixMode.Assist;
        public int GlimpseSize = 50;
        public bool PlaceNewWindows = true;
        public bool RememberSizes = true;
        public int MaxWindows = 30;
        public string Language = "auto";
        public bool StartWithWindows;          // optional, off by default
        public bool DebugLog;
        public string Hotkey = "Ctrl+Alt+Space";
        public Corner HotCorner = Corner.TopLeft;
        public bool ShowMinimized = true;
        public List<Exclusion> Exclusions = new List<Exclusion>();
        public List<MinSizeRule> MinSizes = new List<MinSizeRule>();
        public bool FirstClickActivates;
        /// <summary>Problems found while reading (invalid patterns, bad values); shown on the tray.</summary>
        public List<string> Problems = new List<string>();

        public const int SmallSize = 32, MediumSize = 50, LargeSize = 72;

        public const string DefaultIniText =
"; Glimpse settings. Glimpse reloads this file when it changes, and the settings window writes to it.\r\n" +
"; Encoding: UTF-8 (with or without BOM) or UTF-16. Comment lines start with ; or #.\r\n" +
"\r\n" +
"[Glimpse]\r\n" +
"; Guide, Assist or Strict\r\n" +
";   Guide:  feedback and X-ray only, never moves anything\r\n" +
";   Assist: small fixes after drops and new windows\r\n" +
";   Strict: also fixes moves made by apps and may shrink windows\r\n" +
"Mode=Assist\r\n" +
"; glimpse size in px at 100% scaling: Small 32, Medium 50, Large 72, or any value\r\n" +
"GlimpseSize=50\r\n" +
"; place new windows so every glimpse stays visible\r\n" +
"PlaceNewWindows=1\r\n" +
"; give a new window the size you last chose for that kind of window\r\n" +
"RememberSizes=1\r\n" +
"; automatic fixes cover this many recently used windows\r\n" +
"MaxWindows=30\r\n" +
"; auto follows the Windows display language; or en, de, cs, sk\r\n" +
"Language=auto\r\n" +
"; optional: start Glimpse when you sign in (1), per user, no admin rights needed\r\n" +
"StartWithWindows=0\r\n" +
"; Info or Debug (Debug also writes window titles to the log)\r\n" +
"LogLevel=Info\r\n" +
"\r\n" +
"[XRay]\r\n" +
"Hotkey=Ctrl+Alt+Space\r\n" +
"; TopLeft, TopRight, BottomLeft, BottomRight or Off\r\n" +
"HotCorner=TopLeft\r\n" +
"ShowMinimized=1\r\n" +
"\r\n" +
"; never move these windows: ProcessN is a program file name, TitleN a title pattern\r\n" +
"; (regular expression), for example Process1=teams.exe or Title1=^Calculator$\r\n" +
"[Exclude]\r\n" +
"\r\n" +
"; minimum sizes: rules are tried in number order, the first matching title wins\r\n" +
"; px at 100% scaling, scaled per monitor (200 becomes 300 at 150%)\r\n" +
"; example: TitleRegex=^Order #\\d+  MinWidth=480  MinHeight=360  Note=ERP order forms\r\n" +
"[MinSize1]\r\n" +
"TitleRegex=.*\r\n" +
"MinWidth=200\r\n" +
"MinHeight=200\r\n" +
"Note=every other window\r\n" +
"\r\n" +
"[Experimental]\r\n" +
"; a click on a background window's glimpse only activates it (uses a mouse hook)\r\n" +
"FirstClickActivates=0\r\n";

        public static Settings Defaults()
        {
            return FromIni(IniFile.FromText(DefaultIniText));
        }

        public static Settings FromIni(IniFile ini)
        {
            var s = new Settings();
            string v = ini.Get("Glimpse", "Mode");
            if (v != null)
            {
                switch (v.Trim().ToLowerInvariant())
                {
                    case "guide": s.Mode = FixMode.Guide; break;
                    case "assist": s.Mode = FixMode.Assist; break;
                    case "strict": s.Mode = FixMode.Strict; break;
                    default: s.Problems.Add("Mode=" + v); break;
                }
            }
            s.GlimpseSize = Util.ParseInt(ini.Get("Glimpse", "GlimpseSize"), 50, 16, 400);
            s.PlaceNewWindows = Util.ParseBool(ini.Get("Glimpse", "PlaceNewWindows"), true);
            s.RememberSizes = Util.ParseBool(ini.Get("Glimpse", "RememberSizes"), true);
            s.MaxWindows = Util.ParseInt(ini.Get("Glimpse", "MaxWindows"), 30, 1, 500);
            v = ini.Get("Glimpse", "Language");
            s.Language = string.IsNullOrEmpty(v) ? "auto" : v.Trim();
            s.StartWithWindows = Util.ParseBool(ini.Get("Glimpse", "StartWithWindows"), false);
            v = ini.Get("Glimpse", "LogLevel");
            s.DebugLog = v != null && v.Trim().Equals("debug", StringComparison.OrdinalIgnoreCase);

            v = ini.Get("XRay", "Hotkey");
            if (v != null)
            {
                HotkeySpec hk;
                if (v.Trim().Length == 0 || v.Trim().Equals("off", StringComparison.OrdinalIgnoreCase)) s.Hotkey = "";
                else if (HotkeySpec.TryParse(v, out hk)) s.Hotkey = hk.ToString();
                else s.Problems.Add("Hotkey=" + v);
            }
            v = ini.Get("XRay", "HotCorner");
            if (v != null)
            {
                Corner c;
                if (TryParseCorner(v, out c)) s.HotCorner = c; else s.Problems.Add("HotCorner=" + v);
            }
            s.ShowMinimized = Util.ParseBool(ini.Get("XRay", "ShowMinimized"), true);

            foreach (var kv in ini.Entries("Exclude"))
            {
                if (kv.Value.Length == 0) continue;
                if (kv.Key.StartsWith("Process", StringComparison.OrdinalIgnoreCase)) s.Exclusions.Add(Exclusion.Process(kv.Value));
                else if (kv.Key.StartsWith("Title", StringComparison.OrdinalIgnoreCase))
                {
                    var e = Exclusion.Title(kv.Value);
                    if (e.Error != null) s.Problems.Add("[Exclude] " + kv.Key + ": " + e.Error);
                    s.Exclusions.Add(e);
                }
            }

            // [MinSize1], [MinSize2], ... in number order
            var numbered = new SortedDictionary<int, string>();
            foreach (string sec in ini.Sections())
            {
                if (!sec.StartsWith("MinSize", StringComparison.OrdinalIgnoreCase)) continue;
                int n;
                if (int.TryParse(sec.Substring(7), NumberStyles.Integer, CultureInfo.InvariantCulture, out n) && !numbered.ContainsKey(n)) numbered[n] = sec;
            }
            foreach (var kv in numbered)
            {
                var r = new MinSizeRule();
                r.Pattern = ini.Get(kv.Value, "TitleRegex") ?? ".*";
                r.MinWidth = Util.ParseInt(ini.Get(kv.Value, "MinWidth"), 0, 0, 10000);
                r.MinHeight = Util.ParseInt(ini.Get(kv.Value, "MinHeight"), 0, 0, 10000);
                r.Note = ini.Get(kv.Value, "Note") ?? "";
                if (!r.Compile()) s.Problems.Add("[" + kv.Value + "] TitleRegex: " + r.Error);
                s.MinSizes.Add(r);
            }

            s.FirstClickActivates = Util.ParseBool(ini.Get("Experimental", "FirstClickActivates"), false);
            return s;
        }

        public static bool TryParseCorner(string v, out Corner c)
        {
            switch ((v ?? "").Trim().ToLowerInvariant())
            {
                case "topleft": c = Corner.TopLeft; return true;
                case "topright": c = Corner.TopRight; return true;
                case "bottomleft": c = Corner.BottomLeft; return true;
                case "bottomright": c = Corner.BottomRight; return true;
                case "off": case "": case "none": c = Corner.Off; return true;
                default: c = Corner.Off; return false;
            }
        }

        /// <summary>Writes every setting into the INI, keeping comments and unrelated keys.</summary>
        public void WriteTo(IniFile ini)
        {
            ini.Set("Glimpse", "Mode", Mode.ToString());
            ini.Set("Glimpse", "GlimpseSize", Util.Inv(GlimpseSize));
            ini.Set("Glimpse", "PlaceNewWindows", PlaceNewWindows ? "1" : "0");
            ini.Set("Glimpse", "RememberSizes", RememberSizes ? "1" : "0");
            ini.Set("Glimpse", "MaxWindows", Util.Inv(MaxWindows));
            ini.Set("Glimpse", "Language", Language);
            ini.Set("Glimpse", "StartWithWindows", StartWithWindows ? "1" : "0");
            ini.Set("Glimpse", "LogLevel", DebugLog ? "Debug" : "Info");
            ini.Set("XRay", "Hotkey", string.IsNullOrEmpty(Hotkey) ? "Off" : Hotkey);
            ini.Set("XRay", "HotCorner", HotCorner.ToString());
            ini.Set("XRay", "ShowMinimized", ShowMinimized ? "1" : "0");

            // [Exclude]: replace the keys, keep the section where it is
            foreach (var kv in ini.Entries("Exclude")) ini.RemoveKey("Exclude", kv.Key);
            int np = 0, nt = 0;
            foreach (var e in Exclusions)
            {
                if (e.IsProcess) ini.Set("Exclude", "Process" + (++np).ToString(CultureInfo.InvariantCulture), e.Value);
                else ini.Set("Exclude", "Title" + (++nt).ToString(CultureInfo.InvariantCulture), e.Value);
            }

            // [MinSizeN]: remove all, insert them again where the first one was (or before [Experimental])
            int at = -1;
            foreach (string sec in ini.Sections())
            {
                if (!sec.StartsWith("MinSize", StringComparison.OrdinalIgnoreCase)) continue;
                int n;
                if (!int.TryParse(sec.Substring(7), NumberStyles.Integer, CultureInfo.InvariantCulture, out n)) continue;
                int pos = ini.RemoveSection(sec);
                if (at < 0 || pos < at) at = pos;
            }
            if (at < 0) at = IndexOfSection(ini, "Experimental");
            if (at >= 0)
            {
                for (int i = MinSizes.Count - 1; i >= 0; i--) ini.InsertSection(at, "MinSize" + Util.Inv(i + 1), RuleEntries(MinSizes[i]));
            }
            else
            {
                for (int i = 0; i < MinSizes.Count; i++) ini.InsertSection(-1, "MinSize" + Util.Inv(i + 1), RuleEntries(MinSizes[i]));
            }
            ini.Set("Experimental", "FirstClickActivates", FirstClickActivates ? "1" : "0");
        }

        static List<KeyValuePair<string, string>> RuleEntries(MinSizeRule r)
        {
            var entries = new List<KeyValuePair<string, string>>();
            entries.Add(new KeyValuePair<string, string>("TitleRegex", r.Pattern));
            entries.Add(new KeyValuePair<string, string>("MinWidth", Util.Inv(r.MinWidth)));
            entries.Add(new KeyValuePair<string, string>("MinHeight", Util.Inv(r.MinHeight)));
            if (!string.IsNullOrEmpty(r.Note)) entries.Add(new KeyValuePair<string, string>("Note", r.Note));
            return entries;
        }

        static int IndexOfSection(IniFile ini, string section)
        {
            for (int i = 0; i < ini.Lines.Count; i++)
            {
                string t = ini.Lines[i].Trim();
                if (t.StartsWith("[") && t.EndsWith("]") && string.Equals(t.Substring(1, t.Length - 2).Trim(), section, StringComparison.OrdinalIgnoreCase))
                {
                    // include the comment block right above the header
                    int j = i;
                    while (j > 0 && ini.Lines[j - 1].Trim().Length > 0 && (ini.Lines[j - 1].Trim()[0] == ';' || ini.Lines[j - 1].Trim()[0] == '#')) j--;
                    return j;
                }
            }
            return -1;
        }

        public MinSizeRule RuleFor(string title)
        {
            foreach (var r in MinSizes) if (r.Matches(title)) return r;
            return null;
        }

        public bool IsExcluded(string process, string title)
        {
            foreach (var e in Exclusions) if (e.Matches(process, title)) return true;
            return false;
        }

        public Settings Clone()
        {
            var ini = IniFile.FromText(DefaultIniText);
            WriteTo(ini);
            var c = FromIni(ini);
            return c;
        }
    }

    /// <summary>Finds, creates, reads, writes and watches Glimpse.ini.</summary>
    internal sealed class SettingsStore : IDisposable
    {
        public string FilePath;
        public bool Writable;
        public bool UsingDefaults;   // the file could not be read: built-in defaults are in use
        public string LoadProblem;
        public Settings Current = Settings.Defaults();
        public event EventHandler Changed;

        IniFile ini = IniFile.FromText(Settings.DefaultIniText);
        string lastHash = "";
        DateTime lastWrite;
        long lastLength = -1;
        FileSystemWatcher watcher;
        WinTimer debounce;
        SynchronizationContext ui;

        public static string ResolvePath(string iniArgument)
        {
            if (string.IsNullOrEmpty(iniArgument)) return Path.Combine(Program.ProgramDir, "Glimpse.ini");
            string p = Environment.ExpandEnvironmentVariables(iniArgument.Trim().Trim('"'));
            try { p = Path.GetFullPath(p); } catch { }
            return p;
        }

        public bool IsNextToProgram
        {
            get
            {
                try { return string.Equals(Path.GetFullPath(Path.GetDirectoryName(FilePath)), Path.GetFullPath(Program.ProgramDir), StringComparison.OrdinalIgnoreCase); }
                catch { return false; }
            }
        }

        public void Load(string iniArgument)
        {
            FilePath = ResolvePath(iniArgument);
            EnsureExists();
            ReadNow(true);
        }

        void EnsureExists()
        {
            if (File.Exists(FilePath)) return;
            try
            {
                string dir = Path.GetDirectoryName(FilePath);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                string template = Path.Combine(Program.ProgramDir, "Glimpse.ini");
                if (File.Exists(template) && !string.Equals(Path.GetFullPath(template), Path.GetFullPath(FilePath), StringComparison.OrdinalIgnoreCase))
                    File.Copy(template, FilePath, false);
                else
                    File.WriteAllText(FilePath, Settings.DefaultIniText, new UTF8Encoding(true));
                Log.Info("created " + FilePath);
            }
            catch (Exception ex) { Log.Warn("cannot create " + FilePath + ": " + ex.Message); }
        }

        /// <summary>Reads the file; returns true when the settings changed.</summary>
        bool ReadNow(bool first)
        {
            byte[] data = null;
            try
            {
                var fi = new FileInfo(FilePath);
                if (fi.Exists)
                {
                    lastWrite = fi.LastWriteTimeUtc;
                    lastLength = fi.Length;
                    using (var fs = new FileStream(FilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    {
                        data = new byte[fs.Length];
                        int read = 0;
                        while (read < data.Length)
                        {
                            int n = fs.Read(data, read, data.Length - read);
                            if (n <= 0) break;
                            read += n;
                        }
                    }
                }
            }
            catch (Exception ex) { LoadProblem = ex.Message; }

            if (data == null)
            {
                if (!first && !UsingDefaults) return false; // keep the last good settings
                ini = IniFile.FromText(Settings.DefaultIniText);
                Current = Settings.Defaults();
                UsingDefaults = true;
                Writable = false;
                if (LoadProblem == null) LoadProblem = "not found";
                Log.Warn("cannot read " + FilePath + " (" + LoadProblem + "), using built-in defaults");
                return true;
            }
            string hash = Hash(data);
            if (!first && hash == lastHash) return false;
            lastHash = hash;
            ini = IniFile.Parse(data);
            Current = Settings.FromIni(ini);
            UsingDefaults = false;
            LoadProblem = null;
            Writable = CheckWritable();
            foreach (string p in Current.Problems) Log.Warn("Glimpse.ini: " + p);
            return true;
        }

        bool CheckWritable()
        {
            try
            {
                using (new FileStream(FilePath, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite | FileShare.Delete)) { }
                return true;
            }
            catch { return false; }
        }

        static string Hash(byte[] data)
        {
            using (var sha = System.Security.Cryptography.SHA1.Create())
                return Convert.ToBase64String(sha.ComputeHash(data));
        }

        /// <summary>Saves settings into the INI (keeping comments). Returns false when the file is read-only.</summary>
        public bool Save(Settings s)
        {
            if (!Writable) return false;
            try
            {
                s.WriteTo(ini);
                byte[] data = ini.ToBytes();
                File.WriteAllBytes(FilePath, data);
                lastHash = Hash(data);
                var fi = new FileInfo(FilePath);
                lastWrite = fi.LastWriteTimeUtc;
                lastLength = fi.Length;
                Current = Settings.FromIni(ini);
                return true;
            }
            catch (Exception ex)
            {
                Log.Warn("cannot write " + FilePath + ": " + ex.Message);
                Writable = false;
                return false;
            }
        }

        public void StartWatching()
        {
            ui = SynchronizationContext.Current;
            debounce = new WinTimer { Interval = 300 };
            debounce.Tick += delegate { debounce.Stop(); Poll(); };
            try
            {
                string dir = Path.GetDirectoryName(FilePath);
                if (Directory.Exists(dir))
                {
                    watcher = new FileSystemWatcher(dir, Path.GetFileName(FilePath));
                    watcher.NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.Size | NotifyFilters.FileName | NotifyFilters.CreationTime;
                    FileSystemEventHandler h = delegate { Kick(); };
                    watcher.Changed += h;
                    watcher.Created += h;
                    watcher.Renamed += delegate { Kick(); };
                    watcher.EnableRaisingEvents = true;
                }
            }
            catch (Exception ex) { Log.Warn("file watcher: " + ex.Message); }
        }

        void Kick()
        {
            if (ui != null) ui.Post(delegate { debounce.Stop(); debounce.Start(); }, null);
        }

        /// <summary>Cheap check (also run by the 10 s safety timer).</summary>
        public void Poll()
        {
            try
            {
                var fi = new FileInfo(FilePath);
                if (fi.Exists && fi.LastWriteTimeUtc == lastWrite && fi.Length == lastLength && !UsingDefaults) return;
                if (!fi.Exists && UsingDefaults) return;
            }
            catch { return; }
            if (ReadNow(false))
            {
                Log.Info("Glimpse.ini changed, settings reloaded");
                var h = Changed;
                if (h != null) h(this, EventArgs.Empty);
            }
        }

        public void Dispose()
        {
            if (watcher != null) { watcher.EnableRaisingEvents = false; watcher.Dispose(); watcher = null; }
            if (debounce != null) { debounce.Dispose(); debounce = null; }
        }
    }

    #endregion

    #region Hotkeys: parsing, display names, checks

    /// <summary>A hotkey such as Ctrl+Alt+Space. Canonical modifier order: Ctrl, Shift, Alt, Win.</summary>
    internal struct HotkeySpec
    {
        public uint Mods;   // MOD_* flags
        public uint Vk;

        public static readonly string[] Offered = { "Ctrl+Alt+Space", "Ctrl+Shift+Alt+Space", "Ctrl+Alt+Enter", "Pause" };

        static Dictionary<string, uint> byName;
        static Dictionary<uint, string> canonical;

        static void Init()
        {
            if (byName != null) return;
            var n = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
            var c = new Dictionary<uint, string>();
            Action<string, uint> add = delegate(string name, uint vk) { n[name] = vk; if (!c.ContainsKey(vk)) c[vk] = name; };
            add("Space", 0x20); add("Enter", 0x0D); n["Return"] = 0x0D; add("Pause", 0x13); n["Break"] = 0x13;
            add("Tab", 0x09); add("Esc", 0x1B); n["Escape"] = 0x1B; add("Backspace", 0x08);
            add("Insert", 0x2D); n["Ins"] = 0x2D; add("Delete", 0x2E); n["Del"] = 0x2E; add("Home", 0x24); add("End", 0x23);
            add("PageUp", 0x21); n["PgUp"] = 0x21; add("PageDown", 0x22); n["PgDn"] = 0x22;
            add("Left", 0x25); add("Up", 0x26); add("Right", 0x27); add("Down", 0x28);
            add("PrintScreen", 0x2C); add("ScrollLock", 0x91); add("Apps", 0x5D);
            for (uint i = 0; i < 24; i++) add("F" + (i + 1).ToString(CultureInfo.InvariantCulture), 0x70 + i);
            for (char ch = 'A'; ch <= 'Z'; ch++) add(ch.ToString(), ch);
            for (char ch = '0'; ch <= '9'; ch++) add(ch.ToString(), ch);
            for (uint i = 0; i < 10; i++) add("NumPad" + i.ToString(CultureInfo.InvariantCulture), 0x60 + i);
            add("Multiply", 0x6A); add("Add", 0x6B); n["Plus"] = 0x6B; add("Subtract", 0x6D); n["Minus"] = 0x6D;
            add("Decimal", 0x6E); add("Divide", 0x6F);
            add("Oem1", 0xBA); add("OemPlus", 0xBB); add("OemComma", 0xBC); add("OemMinus", 0xBD); add("OemPeriod", 0xBE);
            add("Oem2", 0xBF); add("Oem3", 0xC0); add("Oem4", 0xDB); add("Oem5", 0xDC); add("Oem6", 0xDD); add("Oem7", 0xDE);
            add("Oem102", 0xE2);
            byName = n;
            canonical = c;
        }

        public bool IsValid { get { return Vk != 0; } }

        public static bool IsModifierVk(uint vk)
        {
            return vk == 0x10 || vk == 0x11 || vk == 0x12 || (vk >= 0xA0 && vk <= 0xA5) || vk == 0x5B || vk == 0x5C;
        }

        public static bool TryParse(string text, out HotkeySpec spec)
        {
            Init();
            spec = new HotkeySpec();
            if (string.IsNullOrEmpty(text)) return false;
            string[] parts = text.Split('+');
            for (int i = 0; i < parts.Length; i++)
            {
                string p = parts[i].Trim();
                if (p.Length == 0)
                {
                    // "Ctrl+Alt++" means the plus key
                    if (i == parts.Length - 1 && spec.Vk == 0) { spec.Vk = 0xBB; continue; }
                    continue;
                }
                switch (p.ToLowerInvariant())
                {
                    case "ctrl": case "control": case "strg": spec.Mods |= Native.MOD_CONTROL; continue;
                    case "alt": spec.Mods |= Native.MOD_ALT; continue;
                    case "shift": spec.Mods |= Native.MOD_SHIFT; continue;
                    case "win": case "windows": spec.Mods |= Native.MOD_WIN; continue;
                }
                if (spec.Vk != 0) return false; // two keys
                uint vk;
                if (byName.TryGetValue(p, out vk)) { spec.Vk = vk; continue; }
                if (p.StartsWith("VK", StringComparison.OrdinalIgnoreCase))
                {
                    string hex = p.Substring(2).TrimStart('_');
                    if (uint.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out vk) && vk > 0 && vk < 0xFF) { spec.Vk = vk; continue; }
                }
                return false;
            }
            return spec.Vk != 0 && !IsModifierVk(spec.Vk);
        }

        public static HotkeySpec FromKeys(Keys keyData)
        {
            Init();
            var s = new HotkeySpec();
            if ((keyData & Keys.Control) != 0) s.Mods |= Native.MOD_CONTROL;
            if ((keyData & Keys.Shift) != 0) s.Mods |= Native.MOD_SHIFT;
            if ((keyData & Keys.Alt) != 0) s.Mods |= Native.MOD_ALT;
            uint vk = (uint)(keyData & Keys.KeyCode);
            if (!IsModifierVk(vk)) s.Vk = vk;
            return s;
        }

        static string KeyName(uint vk)
        {
            Init();
            string name;
            if (canonical.TryGetValue(vk, out name)) return name;
            return "VK" + vk.ToString("X2", CultureInfo.InvariantCulture);
        }

        public override string ToString()
        {
            if (Vk == 0) return "";
            var sb = new StringBuilder();
            if ((Mods & Native.MOD_CONTROL) != 0) sb.Append("Ctrl+");
            if ((Mods & Native.MOD_SHIFT) != 0) sb.Append("Shift+");
            if ((Mods & Native.MOD_ALT) != 0) sb.Append("Alt+");
            if ((Mods & Native.MOD_WIN) != 0) sb.Append("Win+");
            sb.Append(KeyName(Vk));
            return sb.ToString();
        }

        /// <summary>Key caps for display, with names from Windows in the user's language (Strg, Leertaste).</summary>
        public List<string> DisplayParts()
        {
            var r = new List<string>();
            if (Vk == 0) return r;
            if ((Mods & Native.MOD_CONTROL) != 0) r.Add(WindowsKeyName(0x11, "Ctrl"));
            if ((Mods & Native.MOD_SHIFT) != 0) r.Add(WindowsKeyName(0x10, "Shift"));
            if ((Mods & Native.MOD_ALT) != 0) r.Add(WindowsKeyName(0x12, "Alt"));
            if ((Mods & Native.MOD_WIN) != 0) r.Add("Win");
            r.Add(WindowsKeyName(Vk, KeyName(Vk)));
            return r;
        }

        public string DisplayText() { return string.Join("+", DisplayParts().ToArray()); }

        static string WindowsKeyName(uint vk, string fallback)
        {
            if (!Native.IsWindows) return fallback;
            try
            {
                uint scan;
                bool extended = false;
                if (vk == 0x13) scan = 0x45; // Pause: 0x45 without the extended bit
                else
                {
                    scan = Native.MapVirtualKeyEx(vk, Native.MAPVK_VK_TO_VSC, Native.GetKeyboardLayout(0));
                    switch (vk)
                    {
                        case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
                        case 0x2D: case 0x2E: case 0x6F: case 0x5D: extended = true; break;
                    }
                }
                if (scan == 0) return fallback;
                int lp = (int)(scan << 16) | (extended ? (1 << 24) : 0);
                var sb = new StringBuilder(64);
                int len = Native.GetKeyNameText(lp, sb, sb.Capacity);
                if (len <= 0) return fallback;
                string s = sb.ToString();
                // Windows returns upper case for many keys (CTRL, LEERTASTE); show them in title case.
                if (s.Length > 1 && s.ToUpperInvariant() == s) s = s.Substring(0, 1) + s.Substring(1).ToLowerInvariant();
                return s;
            }
            catch { return fallback; }
        }

        /// <summary>Check 2: the combination must type no character on any installed keyboard layout.
        /// Returns null when fine, else the character and the layout that types it.</summary>
        public string TypesCharacter(out string layoutName)
        {
            layoutName = null;
            if (!Native.IsWindows || Vk == 0) return null;
            try
            {
                int n = Native.GetKeyboardLayoutList(0, null);
                if (n <= 0) return null;
                var list = new IntPtr[n];
                Native.GetKeyboardLayoutList(n, list);
                foreach (IntPtr hkl in list)
                {
                    var state = new byte[256];
                    if ((Mods & Native.MOD_CONTROL) != 0) { state[0x11] = 0x80; state[0xA2] = 0x80; }
                    if ((Mods & Native.MOD_ALT) != 0) { state[0x12] = 0x80; state[0xA4] = 0x80; }
                    if ((Mods & Native.MOD_SHIFT) != 0) { state[0x10] = 0x80; state[0xA0] = 0x80; }
                    uint scan = Native.MapVirtualKeyEx(Vk, Native.MAPVK_VK_TO_VSC, hkl);
                    var buf = new StringBuilder(8);
                    // flag 0x4: do not change the keyboard state (Windows 10 1607 and later)
                    int r = Native.ToUnicodeEx(Vk, scan, state, buf, buf.Capacity, 0x4, hkl);
                    if (r > 0 || r < 0)
                    {
                        string ch = buf.ToString();
                        if (r > 0 && ch.Length > 0 && !char.IsControl(ch[0]))
                        {
                            try { layoutName = new CultureInfo((int)(hkl.ToInt64() & 0xFFFF)).DisplayName; }
                            catch { layoutName = "0x" + (hkl.ToInt64() & 0xFFFF).ToString("X4", CultureInfo.InvariantCulture); }
                            return ch;
                        }
                    }
                }
            }
            catch (Exception ex) { Log.Warn("hotkey layout check: " + ex.Message); }
            return null;
        }
    }

    #endregion

    #region Texts: language files, plural rules, placeholders

    /// <summary>Every visible string comes from lang\&lt;code&gt;.txt (UTF-8, key = text).
    /// English is built in (identical to lang\en.txt) and is the fallback for any missing key.</summary>
    internal static class Texts
    {
        static Dictionary<string, string> english;
        static Dictionary<string, string> current;
        static string code = "en";
        static string displayName = "English";

        public static string Code { get { return code; } }
        public static string DisplayName { get { return displayName; } }
        public static string LangFolder { get { return Path.Combine(Program.ProgramDir, "lang"); } }

        public static Dictionary<string, string> English
        {
            get { if (english == null) english = Parse(BuiltInEnglish); return english; }
        }

        public static Dictionary<string, string> Parse(string text)
        {
            var d = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (string raw in text.Replace("\r\n", "\n").Split('\n'))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == '#' || line[0] == ';') continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string key = line.Substring(0, eq).Trim();
                string val = line.Substring(eq + 1).Trim();
                d[key] = Unescape(val);
            }
            return d;
        }

        /// <summary>\n is a line break and \uXXXX a character (the built-in English uses it to stay ASCII).</summary>
        public static string Unescape(string v)
        {
            if (v.IndexOf('\\') < 0) return v;
            var sb = new StringBuilder(v.Length);
            for (int i = 0; i < v.Length; i++)
            {
                char c = v[i];
                if (c == '\\' && i + 1 < v.Length)
                {
                    char n = v[i + 1];
                    int code;
                    if (n == 'n') { sb.Append('\n'); i++; continue; }
                    if (n == 'u' && i + 5 < v.Length && int.TryParse(v.Substring(i + 2, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out code))
                    {
                        sb.Append((char)code);
                        i += 5;
                        continue;
                    }
                }
                sb.Append(c);
            }
            return sb.ToString();
        }

        /// <summary>Loads "auto" (Windows display language), a code such as de, or qps (pseudo-language).</summary>
        public static void Load(string language)
        {
            current = null;
            code = "en";
            displayName = "English";
            string want = string.IsNullOrEmpty(language) ? "auto" : language.Trim();
            var candidates = new List<string>();
            if (want.Equals("auto", StringComparison.OrdinalIgnoreCase))
            {
                CultureInfo ui = AutoCulture();
                if (ui != null) { candidates.Add(ui.Name); candidates.Add(ui.TwoLetterISOLanguageName); }
            }
            else candidates.Add(want);

            foreach (string c in candidates)
            {
                if (string.IsNullOrEmpty(c)) continue;
                if (c.Equals("qps", StringComparison.OrdinalIgnoreCase))
                {
                    current = Pseudo(English);
                    code = "qps";
                    displayName = "Pseudo";
                    return;
                }
                if (c.Equals("en", StringComparison.OrdinalIgnoreCase) || c.StartsWith("en-", StringComparison.OrdinalIgnoreCase)) break;
                string file = Path.Combine(LangFolder, c + ".txt");
                try
                {
                    if (!File.Exists(file)) continue;
                    current = Parse(File.ReadAllText(file, Encoding.UTF8));
                    code = c;
                    displayName = NameOf(c, current);
                    return;
                }
                catch (Exception ex) { Log.Warn("cannot read " + file + ": " + ex.Message); }
            }
        }

        public static CultureInfo AutoCulture()
        {
            try
            {
                if (Native.IsWindows) return new CultureInfo(Native.GetUserDefaultUILanguage());
            }
            catch { }
            return CultureInfo.CurrentUICulture;
        }

        static string NameOf(string c, Dictionary<string, string> d)
        {
            string n;
            if (d != null && d.TryGetValue("_name", out n) && n.Length > 0) return n;
            try { return CultureInfo.GetCultureInfo(c).NativeName; } catch { return c; }
        }

        /// <summary>Language files next to the program: code and display name, English first.</summary>
        public static List<KeyValuePair<string, string>> Available()
        {
            var r = new List<KeyValuePair<string, string>>();
            r.Add(new KeyValuePair<string, string>("en", English["_name"]));
            try
            {
                if (Directory.Exists(LangFolder))
                {
                    var files = Directory.GetFiles(LangFolder, "*.txt");
                    Array.Sort(files, StringComparer.OrdinalIgnoreCase);
                    foreach (string f in files)
                    {
                        string c = Path.GetFileNameWithoutExtension(f);
                        if (c.Equals("en", StringComparison.OrdinalIgnoreCase)) continue;
                        Dictionary<string, string> d = null;
                        try { d = Parse(File.ReadAllText(f, Encoding.UTF8)); } catch { }
                        r.Add(new KeyValuePair<string, string>(c, NameOf(c, d)));
                    }
                }
            }
            catch { }
            return r;
        }

        public static string T(string key)
        {
            string v;
            if (current != null && current.TryGetValue(key, out v)) return v;
            if (English.TryGetValue(key, out v)) return v;
            return key;
        }

        /// <summary>Text with named placeholders: F("drag.movesPx", "px", 80).</summary>
        public static string F(string key, params object[] pairs)
        {
            return Fill(T(key), pairs);
        }

        public static string Fill(string text, params object[] pairs)
        {
            if (pairs == null) return text;
            for (int i = 0; i + 1 < pairs.Length; i += 2)
            {
                object val = pairs[i + 1];
                string s = val is IFormattable ? ((IFormattable)val).ToString(null, CultureInfo.CurrentCulture) : Convert.ToString(val);
                text = text.Replace("{" + pairs[i] + "}", s);
            }
            return text;
        }

        /// <summary>Plural text: picks key.one / .few / .many / .other by the language's CLDR rule; fills {count}.</summary>
        public static string P(string key, long count, params object[] pairs)
        {
            string cat = Category(code == "qps" ? "en" : code, count);
            string text = null;
            if (current != null)
            {
                if (!current.TryGetValue(key + "." + cat, out text) && !current.TryGetValue(key + ".other", out text)) text = null;
            }
            if (text == null)
            {
                string enCat = Category("en", count);
                if (!English.TryGetValue(key + "." + enCat, out text) && !English.TryGetValue(key + ".other", out text)) text = key;
            }
            var all = new List<object>();
            all.Add("count");
            all.Add(count);
            if (pairs != null) all.AddRange(pairs);
            return Fill(text, all.ToArray());
        }

        /// <summary>CLDR plural category for whole numbers.</summary>
        public static string Category(string lang, long n)
        {
            string l = (lang ?? "en").ToLowerInvariant();
            int dash = l.IndexOf('-');
            if (dash > 0) l = l.Substring(0, dash);
            long a = Math.Abs(n), m10 = a % 10, m100 = a % 100;
            switch (l)
            {
                case "cs": case "sk":
                    if (a == 1) return "one";
                    if (a >= 2 && a <= 4) return "few";
                    return "other";
                case "pl":
                    if (a == 1) return "one";
                    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "few";
                    return "many";
                case "ru": case "uk": case "be":
                    if (m10 == 1 && m100 != 11) return "one";
                    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "few";
                    return "many";
                case "hr": case "sr": case "bs":
                    if (m10 == 1 && m100 != 11) return "one";
                    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "few";
                    return "other";
                case "sl":
                    if (m100 == 1) return "one";
                    if (m100 == 2) return "two";
                    if (m100 == 3 || m100 == 4) return "few";
                    return "other";
                case "lt":
                    if (m10 == 1 && (m100 < 11 || m100 > 19)) return "one";
                    if (m10 >= 2 && (m100 < 11 || m100 > 19)) return "few";
                    return "other";
                case "lv":
                    if (m10 == 0 || (m100 >= 11 && m100 <= 19)) return "zero";
                    if (m10 == 1 && m100 != 11) return "one";
                    return "other";
                case "ro":
                    if (a == 1) return "one";
                    if (a == 0 || (m100 >= 2 && m100 <= 19)) return "few";
                    return "other";
                case "fr": case "pt":
                    return a <= 1 ? "one" : "other";
                case "ar":
                    if (a == 0) return "zero";
                    if (a == 1) return "one";
                    if (a == 2) return "two";
                    if (m100 >= 3 && m100 <= 10) return "few";
                    if (m100 >= 11) return "many";
                    return "other";
                case "he":
                    if (a == 1) return "one";
                    if (a == 2) return "two";
                    return "other";
                case "ja": case "zh": case "ko": case "vi": case "th": case "id": case "ms":
                    return "other";
                default:
                    return a == 1 ? "one" : "other";
            }
        }

        /// <summary>Pseudo-language: accented and about 40% longer, placeholders untouched.</summary>
        public static Dictionary<string, string> Pseudo(Dictionary<string, string> src)
        {
            const string plain = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
            const string fancy = "\u00e5\u0180\u00e7\u00f0\u00e9\u0192\u011d\u0125\u00ee\u0135\u0137\u013c\u0271\u00f1\u00f6\u00fe\u01eb\u0155\u0161\u0163\u00fb\u1e7d\u0175\u1e8b\u00fd\u017e\u00c5\u0181\u00c7\u00d0\u00c9\u0191\u011c\u0124\u00ce\u0134\u0136\u013b\u1e3e\u00d1\u00d6\u00de\u01ea\u0154\u0160\u0162\u00db\u1e7c\u0174\u1e8a\u00dd\u017d";
            var d = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var kv in src)
            {
                if (kv.Key.StartsWith("_")) { d[kv.Key] = kv.Value; continue; }
                var sb = new StringBuilder();
                bool inPlaceholder = false;
                int letters = 0;
                foreach (char c in kv.Value)
                {
                    if (c == '{') inPlaceholder = true;
                    if (!inPlaceholder)
                    {
                        int i = plain.IndexOf(c);
                        sb.Append(i >= 0 ? fancy[i] : c);
                        if (char.IsLetter(c)) letters++;
                    }
                    else sb.Append(c);
                    if (c == '}') inPlaceholder = false;
                }
                int pad = Math.Max(1, (int)Math.Round(kv.Value.Length * 0.4));
                d[kv.Key] = "[" + sb + " " + new string('\u00b7', pad) + "]";
            }
            d["_name"] = "Pseudo";
            return d;
        }

        public static int DumpBuiltIn(string file)
        {
            string text = Regex.Replace(BuiltInEnglish, @"\\u([0-9a-fA-F]{4})", delegate(Match m) { return ((char)int.Parse(m.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture)).ToString(); });
            File.WriteAllText(file, text.Replace("\r\n", "\n").Replace("\n", "\r\n"), new UTF8Encoding(false));
            Console.WriteLine("wrote " + file);
            return 0;
        }

        /// <summary>The English texts, identical to lang\en.txt (the source for translators).</summary>
        public const string BuiltInEnglish = @"# Glimpse texts: English (the source for every translation).
# Format: key = text, UTF-8. {name} is a placeholder; keep it as it is.
# Plural keys end in .one, .few, .many or .other (CLDR categories); \n is a line break.
_name = English

# ---- general
windows.one = {count} window
windows.other = {count} windows

# ---- tray icon and menu
status.guarding.one = Guarding {count} window
status.guarding.other = Guarding {count} windows
status.allInReach = all in reach
status.hidden.one = {count} window reachable through X-ray
status.hidden.other = {count} windows reachable through X-ray
status.paused = Paused until {time}
status.pausedTomorrow = Paused until tomorrow
status.guide = Guide mode: feedback and X-ray only
status.suspended = Automatic fixes paused: too many moves in a short time
status.overLimit = Automatic fixes cover the {count} most recently used windows
status.invalidPattern = A title pattern in Glimpse.ini is not valid
status.iniDefaults = Glimpse.ini cannot be read: using built-in defaults
status.hotkeyTaken = The X-ray hotkey {hotkey} is used by another program
status.idle = Idle while the session is locked or disconnected
menu.xray = X-ray: show every window
menu.arrange = Arrange now
menu.undo = Undo last fix
menu.pause = Pause
menu.pauseHint = 15 min \u00b7 1 h \u00b7 until tomorrow
menu.pause15 = 15 min
menu.pause60 = 1 h
menu.pauseTomorrow = Until tomorrow
menu.resume = Resume
menu.resumeFixes = Resume automatic fixes
menu.settings = Settings
menu.exit = Exit Glimpse

# ---- toasts
toast.moved.one = Moved {count} window to keep it in reach
toast.moved.other = Moved {count} windows to keep them in reach
toast.display.one = Display changed: {count} window moved
toast.display.other = Display changed: {count} windows moved
toast.arranged.one = Arranged {count} window
toast.arranged.other = Arranged {count} windows
toast.allInReach = Every window is already in reach
toast.layout = This display setup was used before
toast.layoutHint = Put windows back where they were on it?
toast.restore = Restore
toast.undo = Undo

# ---- while dragging
drag.keeps = This window keeps a glimpse
drag.noGlimpse = This window would have no glimpse
drag.stay.one = {count} stays visible
drag.stay.other = {count} stay visible
drag.move.one = {count} moves on drop
drag.move.other = {count} move on drop
drag.hide.one = {count} would be hidden
drag.hide.other = {count} would be hidden
drag.peek = {title} will peek out here
drag.movesPx = moves {px} px on drop
drag.wouldBeHidden = Would be hidden
drag.reason.elevated = Admin windows can't be moved
drag.reason.stubborn = This window keeps moving back
drag.reason.excluded = Excluded in Settings
drag.reason.hung = The app is not responding
drag.reason.topmost = Always-on-top window
drag.reason.noSpace = No small move frees a glimpse
drag.reason.guide = Guide mode never moves windows
drag.reason.paused = Glimpse is paused
drag.reason.limit = Not among the most recently used windows
drag.reason.shift = Shift: this drop skips the fix
drag.guide = Drop edge here: nothing needs to move
drag.minSize = Grows back to {width} \u00d7 {height}

# ---- X-ray
xray.search = Type to find a window
xray.matches.one = {count} match
xray.matches.other = {count} matches
xray.noMatch = No match
xray.enter = Enter
xray.minimized = Minimized
xray.hint = **Click** a window or press its **number** \u00b7 **type** to filter \u00b7 **Shift+click** also gives it a glimpse \u00b7 **Esc** closes
xray.empty = No other windows are open

# ---- settings window
settings.title = Glimpse settings
settings.size.title = Glimpse size
settings.size.subtitle = How much of every window stays visible, scaled per monitor
settings.size.small = Small \u00b7 {px} px
settings.size.medium = Medium \u00b7 {px} px
settings.size.large = Large \u00b7 {px} px
settings.size.custom = Custom \u00b7 {px} px
settings.mode.title = When a window would disappear
settings.mode.guide = Guide
settings.mode.guide.desc = Show feedback and X-ray. Never move windows.
settings.mode.assist = Assist
settings.mode.assist.desc = Small fixes after drops and new windows, with Undo.
settings.mode.strict = Strict
settings.mode.strict.desc = Also fix moves made by apps. May shrink windows.
settings.recommended = Recommended
settings.new.title = New windows
settings.new.place = Place new windows so every glimpse stays visible
settings.new.remember = Remember window sizes per app
settings.min.title = Minimum window size
settings.min.subtitle = Windows are grown back after a resize \u00b7 the first matching title wins
settings.min.add = + Add rule
settings.min.pattern = Title pattern (regular expression)
settings.min.note = Note
settings.min.invalid = Not a valid pattern: {error}
settings.min.remove = Remove this rule
settings.xray.title = X-ray: reach any window
settings.xray.hotkey = Hotkey
settings.xray.none = None
settings.xray.test = Press to test
settings.xray.change = Change
settings.xray.record = Record another combination\u2026
settings.xray.noHotkey = No hotkey
settings.xray.pressNow = Press {hotkey} now\u2026
settings.xray.recordNow = Press the new combination. Esc cancels.
settings.xray.arrived = {hotkey} arrives in this session.
settings.xray.silent = {hotkey} did not arrive: your remote client or your PC keeps it. Try {next}.
settings.xray.taken = {hotkey} is already used by another program in this session.
settings.xray.types = {hotkey} types \u201c{char}\u201d with the {layout} keyboard layout.
settings.xray.corner = Hot corner
settings.xray.cornerDesc = {corner}, after 0.4 s
settings.xray.cornerOff = Off
settings.xray.minimized = Show minimized windows
settings.corner.topLeft = top-left
settings.corner.topRight = top-right
settings.corner.bottomLeft = bottom-left
settings.corner.bottomRight = bottom-right
settings.exclude.title = Never move these
settings.exclude.subtitle = Excluded apps keep their place
settings.exclude.pick = Pick a window
settings.exclude.pickHint = Hold the mouse button and drag onto a window
settings.exclude.add = Add app\u2026
settings.exclude.prompt = Program file name, for example teams.exe
settings.exclude.remove = Remove
settings.exclude.titleTip = Windows whose title matches {pattern}
settings.start = Start with Windows
settings.language = Language
settings.language.auto = Automatic \u00b7 {language}
settings.saved = Saved to Glimpse.ini next to {program}
settings.savedPath = Saved to {path}
settings.readOnly = {path} cannot be written: settings are read-only
settings.defaults = Glimpse.ini cannot be read: built-in defaults are in use
settings.done = Done
";
    }

    #endregion

    #region Geometry engine: free glimpse zones (pure, no Windows calls)

    internal enum Edge { Top = 0, Bottom = 1, Left = 2, Right = 3 }

    internal sealed class GeoMonitor
    {
        public Rc Bounds, Work;
        public int Dpi = 96;
        public GeoMonitor(Rc bounds, Rc work, int dpi) { Bounds = bounds; Work = work.IsEmpty ? bounds : work; Dpi = dpi <= 0 ? 96 : dpi; }
    }

    /// <summary>What a click does along the top edge, from the window's own hit-test.
    /// A step function over x relative to the frame's left edge: 3 title bar (only activates),
    /// 2 content, 1 window-menu icon (avoid), 0 caption buttons (never counts).</summary>
    internal sealed class TopEdgeMap
    {
        public readonly List<int> Starts = new List<int>();
        public readonly List<int> Qualities = new List<int>();
        public void Add(int relX, int quality)
        {
            if (Qualities.Count > 0 && Qualities[Qualities.Count - 1] == quality) return;
            Starts.Add(relX);
            Qualities.Add(quality);
        }
    }

    internal sealed class GeoWindow
    {
        public string Id = "";
        public Rc Frame;
        public bool Subject = true;     // must keep a free glimpse
        public bool Blocker = true;     // other windows' glimpses must keep clear of it
        public bool Movable = true;     // the resolver may move or resize it
        public bool Resizable = true;   // has a resize border: minimum size and shrinking apply
        public string FixedReason;      // elevated, stubborn, excluded, topmost, hung, limit
        public int MinW, MinH;          // minimum frame size in px on its monitor (0: none)
        public int Dpi = 96;            // DPI of the monitor that holds most of the window
        public int CaptionW = -1;       // caption-button width in px (-1: 138 at 100%)
        public int IconW = -1;          // window-menu icon width in px (-1: 32 at 100%)
        public int CaptionH = -1;       // height of the title-bar band in px (-1: 32 at 100%)
        public TopEdgeMap TopMap;       // from hit-test probing, optional
        public Rc PrevWork;             // work area of its monitor before a display change (empty: unknown)
        public object Tag;
        public long Order;              // input order, used to break ties
    }

    internal sealed class Zone
    {
        public Edge Edge;
        public Rc Rect;
        public int Quality;   // 3 title bar, 2 edge or content, 1 window-menu icon
    }

    internal static class Geometry
    {
        /// <summary>Glimpse size P in physical px: GlimpseSize at 100% scaling, scaled with the monitor.</summary>
        public static int GlimpsePx(int glimpseSize, int dpi)
        {
            return Math.Max(8, (int)Math.Round(glimpseSize * (dpi <= 0 ? 96 : dpi) / 96.0));
        }

        /// <summary>Quality ranges along the top edge as absolute [start, end) runs.</summary>
        static List<int[]> TopRanges(GeoWindow w, Rc f)
        {
            var r = new List<int[]>();
            if (w != null && w.TopMap != null && w.TopMap.Starts.Count > 0)
            {
                for (int i = 0; i < w.TopMap.Starts.Count; i++)
                {
                    int a = f.L + Math.Max(0, w.TopMap.Starts[i]);
                    int b = i + 1 < w.TopMap.Starts.Count ? f.L + w.TopMap.Starts[i + 1] : f.R;
                    if (b > f.R) b = f.R;
                    if (b > a) r.Add(new[] { a, b, w.TopMap.Qualities[i] });
                }
                return r;
            }
            int dpi = w == null ? 96 : w.Dpi;
            int icon = w != null && w.IconW >= 0 ? w.IconW : Util.Scale(32, dpi);
            int caption = w != null && w.CaptionW >= 0 ? w.CaptionW : Util.Scale(138, dpi);
            int iconEnd = Math.Min(f.R, f.L + icon);
            int capStart = Math.Max(iconEnd, f.R - caption);
            if (iconEnd > f.L) r.Add(new[] { f.L, iconEnd, 1 });
            if (capStart > iconEnd) r.Add(new[] { iconEnd, capStart, 3 });
            if (f.R > capStart) r.Add(new[] { capStart, f.R, 0 });
            return r;
        }

        static int MinQuality(List<int[]> ranges, int x0, int x1)
        {
            int q = 3;
            foreach (int[] r in ranges) if (r[0] < x1 && r[1] > x0) q = Math.Min(q, r[2]);
            return q;
        }

        /// <summary>Square start positions s where [s, s+P) lies in runs of at least quality q.</summary>
        static IntervalSet GoodStarts(List<int[]> ranges, int q, int P)
        {
            var set = new IntervalSet();
            int runStart = int.MinValue, runEnd = int.MinValue;
            foreach (int[] r in ranges)
            {
                if (r[2] >= q)
                {
                    if (runStart != int.MinValue && r[0] == runEnd) runEnd = r[1];
                    else
                    {
                        if (runStart != int.MinValue) set.Add(runStart, runEnd - P);
                        runStart = r[0];
                        runEnd = r[1];
                    }
                }
                else if (runStart != int.MinValue)
                {
                    set.Add(runStart, runEnd - P);
                    runStart = int.MinValue;
                }
            }
            if (runStart != int.MinValue) set.Add(runStart, runEnd - P);
            return set;
        }

        /// <summary>The best free glimpse zone of a window with frame f and glimpse size P, or null.
        /// A zone is a P x P square inside the frame, touching its edge, inside one monitor's work area,
        /// overlapping none of the other rectangles. Higher quality wins; ties go to Top, Bottom, Left, Right.</summary>
        public static Zone BestZone(GeoWindow w, Rc f, int P, List<Rc> others, IList<GeoMonitor> mons)
        {
            Zone best = null;
            for (int e = 0; e < 4; e++)
            {
                var edge = (Edge)e;
                bool horiz = edge == Edge.Top || edge == Edge.Bottom;
                int len = horiz ? f.W : f.H, depth = horiz ? f.H : f.W;
                if (len < P || depth < P) continue;
                int c0 = edge == Edge.Top ? f.T : edge == Edge.Bottom ? f.B - P : edge == Edge.Left ? f.L : f.R - P;
                int c1 = c0 + P;
                int s0 = horiz ? f.L : f.T, s1 = (horiz ? f.R : f.B) - P;

                var allowed = new IntervalSet();
                foreach (var m in mons)
                {
                    int mc0 = horiz ? m.Work.T : m.Work.L, mc1 = horiz ? m.Work.B : m.Work.R;
                    if (mc0 > c0 || mc1 < c1) continue;
                    int ms0 = horiz ? m.Work.L : m.Work.T, ms1 = (horiz ? m.Work.R : m.Work.B) - P;
                    allowed.Add(Math.Max(s0, ms0), Math.Min(s1, ms1));
                }
                if (allowed.IsEmpty) continue;
                foreach (Rc o in others)
                {
                    int oc0 = horiz ? o.T : o.L, oc1 = horiz ? o.B : o.R;
                    if (oc1 <= c0 || oc0 >= c1) continue;
                    int os0 = horiz ? o.L : o.T, os1 = horiz ? o.R : o.B;
                    if (os1 <= os0) continue;
                    allowed.Subtract(os0 - P + 1, os1 - 1);
                    if (allowed.IsEmpty) break;
                }
                if (allowed.IsEmpty) continue;

                int quality, pos;
                if (edge == Edge.Top)
                {
                    var ranges = TopRanges(w, f);
                    quality = 0;
                    pos = 0;
                    for (int q = 3; q >= 1; q--)
                    {
                        var hit = allowed.Intersect(GoodStarts(ranges, q, P));
                        if (!hit.IsEmpty) { quality = q; pos = hit.First; break; }
                    }
                    if (quality == 0) continue; // only caption buttons are free: never counts
                }
                else if (edge == Edge.Bottom) { quality = 2; pos = allowed.First; }
                else
                {
                    // a side square reaching into the title-bar band inherits what a click there does:
                    // never on caption buttons, only as a last resort on the window-menu icon
                    int band = w != null && w.CaptionH >= 0 ? w.CaptionH : Util.Scale(32, w == null ? 96 : w.Dpi);
                    int qBand = MinQuality(TopRanges(w, f), c0, c1);
                    if (qBand == 0) allowed.Subtract(f.T - P + 1, f.T + band - 1);
                    if (allowed.IsEmpty) continue;
                    quality = 2;
                    pos = allowed.First;
                    if (qBand == 1)
                    {
                        var below = IntervalSet.Of(f.T + band, int.MaxValue - 1).Intersect(allowed);
                        if (below.IsEmpty) quality = 1; else pos = below.First;
                    }
                }

                if (best != null && best.Quality >= quality) continue;
                Rc z;
                switch (edge)
                {
                    case Edge.Top: z = new Rc(pos, f.T, pos + P, f.T + P); break;
                    case Edge.Bottom: z = new Rc(pos, f.B - P, pos + P, f.B); break;
                    case Edge.Left: z = new Rc(f.L, pos, f.L + P, pos + P); break;
                    default: z = new Rc(f.R - P, pos, f.R, pos + P); break;
                }
                best = new Zone { Edge = edge, Rect = z, Quality = quality };
                if (quality == 3) break;
            }
            return best;
        }

        /// <summary>Index of the monitor holding most of r, else the nearest one.</summary>
        public static int MonitorOf(Rc r, IList<GeoMonitor> mons)
        {
            int best = -1;
            long bestArea = 0;
            for (int i = 0; i < mons.Count; i++)
            {
                long a = r.Intersect(mons[i].Bounds).Area;
                if (a > bestArea) { bestArea = a; best = i; }
            }
            if (best >= 0) return best;
            double bestDist = double.MaxValue;
            for (int i = 0; i < mons.Count; i++)
            {
                double d = mons[i].Work.DistanceTo(r.CX, r.CY);
                if (d < bestDist) { bestDist = d; best = i; }
            }
            return Math.Max(0, best);
        }

        /// <summary>Moves r inside the work area without resizing (top-left wins if it does not fit).</summary>
        public static Rc ClampInto(Rc r, Rc work)
        {
            int dx = 0, dy = 0;
            if (r.R > work.R) dx = work.R - r.R;
            if (r.L + dx < work.L) dx = work.L - r.L;
            if (r.B > work.B) dy = work.B - r.B;
            if (r.T + dy < work.T) dy = work.T - r.T;
            return r.Offset(dx, dy);
        }

        /// <summary>Second rule: grow a too small window back to its minimum. After a user resize the
        /// dragged edges move back; otherwise the top-left corner stays. The window shifts inward
        /// if growing would leave the work area, and fills it if the work area is smaller.</summary>
        public static Rc GrowToMin(Rc f, int minW, int minH, string draggedEdges, Rc work)
        {
            Rc r = f;
            string edges = draggedEdges ?? "";
            if (minW > 0 && r.W < minW)
            {
                if (work.W <= minW) { r.L = work.L; r.R = work.R; }
                else if (edges.IndexOf('L') >= 0 && edges.IndexOf('R') < 0) r.L = r.R - minW;
                else r.R = r.L + minW;
            }
            if (minH > 0 && r.H < minH)
            {
                if (work.H <= minH) { r.T = work.T; r.B = work.B; }
                else if (edges.IndexOf('T') >= 0 && edges.IndexOf('B') < 0) r.T = r.B - minH;
                else r.B = r.T + minH;
            }
            if (r.Same(f)) return f;
            // shift inward only by what growing pushed out
            int over = r.R - Math.Max(work.R, f.R);
            if (over > 0) r = r.Offset(-over, 0);
            over = Math.Min(work.L, f.L) - r.L;
            if (over > 0) r = r.Offset(over, 0);
            over = r.B - Math.Max(work.B, f.B);
            if (over > 0) r = r.Offset(0, -over);
            over = Math.Min(work.T, f.T) - r.T;
            if (over > 0) r = r.Offset(0, over);
            return r;
        }

        /// <summary>True when at least a P x P part of r lies inside some work area.</summary>
        public static bool Reachable(Rc r, int P, IList<GeoMonitor> mons)
        {
            foreach (var m in mons)
            {
                Rc i = r.Intersect(m.Work);
                if (i.W >= P && i.H >= P) return true;
            }
            return false;
        }

        /// <summary>Rescue after a display change: the nearest monitor, same relative position,
        /// shrunk to the work area when larger (only if resizable).</summary>
        public static Rc Rescue(Rc f, Rc prevWork, bool resizable, IList<GeoMonitor> mons)
        {
            int mi = 0;
            double bestDist = double.MaxValue;
            for (int i = 0; i < mons.Count; i++)
            {
                double d = mons[i].Work.DistanceTo(f.CX, f.CY);
                if (d < bestDist) { bestDist = d; mi = i; }
            }
            Rc work = mons[mi].Work;
            Rc old = prevWork.IsEmpty ? work : prevWork;
            int w = f.W, h = f.H;
            if (resizable) { w = Math.Min(w, work.W); h = Math.Min(h, work.H); }
            double rx = old.W > f.W ? (f.L - old.L) / (double)(old.W - f.W) : 0;
            double ry = old.H > f.H ? (f.T - old.T) / (double)(old.H - f.H) : 0;
            rx = Math.Max(0, Math.Min(1, rx));
            ry = Math.Max(0, Math.Min(1, ry));
            int x = work.L + (int)Math.Round(rx * Math.Max(0, work.W - w));
            int y = work.T + (int)Math.Round(ry * Math.Max(0, work.H - h));
            return new Rc(x, y, x + w, y + h);
        }
    }

    #endregion

    #region Resolver: minimum sizes first, then the cheapest moves that restore every glimpse

    internal enum Cause { Check, Drop, New, Restore, AppMove, AppResize, Display, Arrange, GiveGlimpse }

    /// <summary>What the resolver may do for one settled change.</summary>
    internal sealed class Request
    {
        public Cause Cause = Cause.Check;
        public int Target = -1;              // anchor (drop) or intruder (new, app move, shift+click)
        public bool Fix = true;              // false: only report (Guide mode, paused, preview of nothing)
        public bool AllowShrink;             // Strict mode
        public bool Rescue;                  // display change: bring off-screen windows back
        public bool MinSizeAll;              // display change, arrange now
        public bool MinSizeTarget;           // user resize, new or restored window, app resize
        public string DraggedEdges = "";     // user resize: L, T, R, B
        public bool Victims;                 // windows that lost their glimpse move (drop, display, arrange)
        public bool Intruder;                // the target moves so every glimpse stays (new, app move)
        public Rc AnchorStart;               // drop: the anchor's frame before the drag
        public HashSet<string> AlreadyMoved = new HashSet<string>(); // one fix per window per user action

        /// <summary>Standard request for a cause in a mode (see the Mode setting).</summary>
        public static Request For(Cause cause, FixMode mode, int target, bool placeNewWindows)
        {
            var r = new Request { Cause = cause, Target = target, AllowShrink = mode == FixMode.Strict };
            r.Fix = mode != FixMode.Guide || cause == Cause.GiveGlimpse;
            switch (cause)
            {
                case Cause.Drop: r.Victims = true; break;
                case Cause.New:
                case Cause.Restore: r.MinSizeTarget = true; r.Intruder = placeNewWindows; break;
                case Cause.AppMove: r.Intruder = mode == FixMode.Strict; break;
                case Cause.AppResize: r.MinSizeTarget = true; r.Intruder = mode == FixMode.Strict; break;
                case Cause.Display: r.Rescue = true; r.MinSizeAll = true; r.Victims = true; break;
                case Cause.Arrange: r.MinSizeAll = true; r.Victims = true; break;
                case Cause.GiveGlimpse: r.Intruder = true; break;
            }
            return r;
        }
    }

    internal sealed class Move
    {
        public int Index;
        public GeoWindow W;
        public Rc From, To;
        public string Why;   // minsize, glimpse, place, cascade, rescue
    }

    internal sealed class Outcome
    {
        public readonly List<Move> Moves = new List<Move>();
        public Zone[] Zones;               // per window after the plan (null: none or not a subject)
        public readonly List<int> Hidden = new List<int>();   // subjects without a free glimpse
        public string[] Reasons;           // why a hidden window could not be fixed
        public Rc[] Frames;                // frames after the plan

        public Move MoveOf(int index)
        {
            foreach (var m in Moves) if (m.Index == index) return m;
            return null;
        }
    }

    /// <summary>Working state: windows, their frames and the monitors.</summary>
    internal sealed class Layout
    {
        public readonly List<GeoWindow> Windows = new List<GeoWindow>();
        public readonly List<GeoMonitor> Monitors = new List<GeoMonitor>();
        public int GlimpseSize = 50;
        public Rc[] Frames;

        public void Begin()
        {
            Frames = new Rc[Windows.Count];
            for (int i = 0; i < Windows.Count; i++) { Frames[i] = Windows[i].Frame; Windows[i].Order = i; }
        }

        public int P(int i) { return Geometry.GlimpsePx(GlimpseSize, Windows[i].Dpi); }

        public Rc WorkOf(Rc r) { return Monitors[Geometry.MonitorOf(r, Monitors)].Work; }

        /// <summary>Blocker rectangles other than window i; window moved (if &gt;= 0) is taken at movedTo,
        /// or left out when skipMoved.</summary>
        public List<Rc> Others(int i, int moved, Rc movedTo, bool skipMoved)
        {
            var list = new List<Rc>(Windows.Count);
            for (int j = 0; j < Windows.Count; j++)
            {
                if (j == i || !Windows[j].Blocker) continue;
                if (j == moved) { if (!skipMoved) list.Add(movedTo); continue; }
                list.Add(Frames[j]);
            }
            return list;
        }

        public Zone ZoneOf(int i) { return Geometry.BestZone(Windows[i], Frames[i], P(i), Others(i, -1, default(Rc), false), Monitors); }

        public Zone ZoneAt(int i, Rc frame) { return Geometry.BestZone(Windows[i], frame, P(i), Others(i, -1, default(Rc), false), Monitors); }

        /// <summary>Zone of window j if window moved were at movedTo (or absent).</summary>
        public Zone ZoneWith(int j, int moved, Rc movedTo, bool absent)
        {
            return Geometry.BestZone(Windows[j], Frames[j], P(j), Others(j, moved, movedTo, absent), Monitors);
        }
    }

    internal static class Resolver
    {
        const int MonitorPenalty = 100000;

        public static Outcome Resolve(Layout L, Request q)
        {
            if (L.Frames == null || L.Frames.Length != L.Windows.Count) L.Begin();
            var o = new Outcome();
            int n = L.Windows.Count;
            var reasons = new string[n];

            if (q.Fix && L.Monitors.Count > 0)
            {
                if (q.Rescue) RescuePass(L, o);
                MinSizePass(L, q, o);
                if (q.Victims) VictimPass(L, q, o, reasons);
                if (q.Intruder && q.Target >= 0) IntruderPass(L, q, o, reasons);
            }

            o.Zones = new Zone[n];
            o.Reasons = new string[n];
            for (int i = 0; i < n; i++)
            {
                if (!L.Windows[i].Subject) continue;
                o.Zones[i] = L.Monitors.Count == 0 ? null : L.ZoneOf(i);
                if (o.Zones[i] == null)
                {
                    o.Hidden.Add(i);
                    var w = L.Windows[i];
                    string why = reasons[i];
                    if (w.FixedReason != null && !w.Movable) why = w.FixedReason;
                    else if (i == q.Target && q.Cause == Cause.Drop) why = "anchor";
                    else if (!q.Fix) why = "guide";
                    o.Reasons[i] = why ?? "noSpace";
                }
            }
            o.Frames = (Rc[])L.Frames.Clone();
            return o;
        }

        static void Record(Layout L, Outcome o, int i, Rc to, string why)
        {
            Rc from = L.Frames[i];
            if (from.Same(to)) return;
            L.Frames[i] = to;
            var m = o.MoveOf(i);
            if (m == null) o.Moves.Add(new Move { Index = i, W = L.Windows[i], From = from, To = to, Why = why });
            else { m.To = to; if (m.Why == "minsize" && why != "minsize") m.Why = why; }
        }

        static void RescuePass(Layout L, Outcome o)
        {
            for (int i = 0; i < L.Windows.Count; i++)
            {
                var w = L.Windows[i];
                if (!w.Movable) continue;
                if (Geometry.Reachable(L.Frames[i], L.P(i), L.Monitors)) continue;
                Record(L, o, i, Geometry.Rescue(L.Frames[i], w.PrevWork, w.Resizable, L.Monitors), "rescue");
            }
        }

        static void MinSizePass(Layout L, Request q, Outcome o)
        {
            for (int i = 0; i < L.Windows.Count; i++)
            {
                bool inScope = q.MinSizeAll || (q.MinSizeTarget && i == q.Target) || (q.Cause == Cause.Drop && i == q.Target && q.DraggedEdges.Length > 0);
                if (!inScope) continue;
                var w = L.Windows[i];
                if (!w.Resizable || !w.Movable || !w.Subject || (w.MinW <= 0 && w.MinH <= 0)) continue;
                Rc to = Geometry.GrowToMin(L.Frames[i], w.MinW, w.MinH, i == q.Target ? q.DraggedEdges : "", L.WorkOf(L.Frames[i]));
                Record(L, o, i, to, "minsize");
            }
        }

        /// <summary>Windows that lost their glimpse yield, smallest first, by the cheapest nudge.</summary>
        static void VictimPass(Layout L, Request q, Outcome o, string[] reasons)
        {
            int n = L.Windows.Count;
            var victims = new List<int>();
            Rc anchorEnd = q.Target >= 0 ? L.Frames[q.Target] : default(Rc);
            for (int i = 0; i < n; i++)
            {
                var w = L.Windows[i];
                if (!w.Subject || i == q.Target) continue;
                if (q.Cause == Cause.Drop && q.Target >= 0)
                {
                    // only windows the drop can have affected
                    bool touched = L.Frames[i].Intersects(anchorEnd) || (!q.AnchorStart.IsEmpty && L.Frames[i].Intersects(q.AnchorStart));
                    if (!touched) continue;
                }
                if (L.ZoneOf(i) == null) victims.Add(i);
            }
            victims.Sort(delegate(int a, int b)
            {
                int c = L.Frames[a].Area.CompareTo(L.Frames[b].Area);
                return c != 0 ? c : a.CompareTo(b);
            });
            foreach (int i in victims)
            {
                var w = L.Windows[i];
                if (L.ZoneOf(i) != null) continue; // an earlier move freed it
                if (!w.Movable) { reasons[i] = w.FixedReason ?? "noSpace"; continue; }
                if (q.AlreadyMoved.Contains(w.Id)) { reasons[i] = "noSpace"; continue; }
                Rc to;
                if (BestNudge(L, q, i, out to)) Record(L, o, i, to, "glimpse");
                else reasons[i] = "noSpace";
            }
        }

        sealed class Cand
        {
            public Rc To;
            public int Cost, Dx, Dy, Seq;
        }

        static bool[] SubjectsWithZone(Layout L, int except)
        {
            var has = new bool[L.Windows.Count];
            for (int j = 0; j < L.Windows.Count; j++)
                if (j != except && L.Windows[j].Subject) has[j] = L.ZoneOf(j) != null;
            return has;
        }

        /// <summary>Window i at frame to has a glimpse, and every window in mustKeep keeps its own.</summary>
        static bool Valid(Layout L, int i, Rc to, bool[] mustKeep, out Zone zone)
        {
            zone = L.ZoneAt(i, to);
            if (zone == null) return false;
            if (!L.Windows[i].Blocker) return true;
            for (int j = 0; j < L.Windows.Count; j++)
            {
                if (j == i || !mustKeep[j]) continue;
                if (!L.Frames[j].Intersects(to)) continue;
                if (L.ZoneWith(j, i, to, false) == null) return false;
            }
            return true;
        }

        public static bool BestNudge(Layout L, Request q, int i, out Rc best)
        {
            best = default(Rc);
            var w = L.Windows[i];
            Rc f = L.Frames[i];
            int P = L.P(i);
            int mon = Geometry.MonitorOf(f, L.Monitors);
            Rc work = L.Monitors[mon].Work;
            int maxNudge = Math.Min(Math.Min(f.W, f.H) / 4, Util.Scale(240, w.Dpi));

            var dxs = new List<int> { 0 };
            var dys = new List<int> { 0 };
            var coverers = new List<Rc>();
            for (int j = 0; j < L.Windows.Count; j++)
            {
                if (j == i || !L.Windows[j].Blocker) continue;
                Rc c = L.Frames[j];
                if (!c.Intersects(f)) continue;
                coverers.Add(c);
                AddUnique(dys, c.T - P - f.T); AddUnique(dys, c.B + P - f.B); AddUnique(dys, c.B - f.T); AddUnique(dys, c.T - f.B);
                AddUnique(dxs, c.L - P - f.L); AddUnique(dxs, c.R + P - f.R); AddUnique(dxs, c.R - f.L); AddUnique(dxs, c.L - f.R);
            }
            // the work area's edges matter when the glimpse lies under the taskbar or off-screen
            AddUnique(dys, work.T - f.T); AddUnique(dys, work.B - f.B);
            AddUnique(dxs, work.L - f.L); AddUnique(dxs, work.R - f.R);

            var cands = new List<Cand>();
            var seen = new HashSet<long>();
            int seq = 0;
            foreach (int dx in dxs)
                foreach (int dy in dys)
                {
                    if (dx == 0 && dy == 0) continue;
                    Rc to = Geometry.ClampInto(f.Offset(dx, dy), work);
                    int ax = to.L - f.L, ay = to.T - f.T;
                    if (ax == 0 && ay == 0) continue;
                    if (Math.Sqrt((double)ax * ax + (double)ay * ay) > maxNudge) continue;
                    long key = ((long)ax << 32) ^ (uint)ay;
                    if (!seen.Add(key)) continue;
                    int cost = Math.Abs(ax) + Math.Abs(ay);
                    if (Geometry.MonitorOf(to, L.Monitors) != mon) cost += MonitorPenalty;
                    cands.Add(new Cand { To = to, Cost = cost, Dx = ax, Dy = ay, Seq = seq++ });
                }

            if (q.AllowShrink && w.Resizable)
            {
                int minW = Math.Max(w.MinW, 2 * P), minH = Math.Max(w.MinH, 2 * P);
                foreach (Rc c in coverers)
                {
                    TryShrink(cands, new Rc(f.L, f.T, c.L, f.B), f, minW, minH, ref seq);
                    TryShrink(cands, new Rc(c.R, f.T, f.R, f.B), f, minW, minH, ref seq);
                    TryShrink(cands, new Rc(f.L, f.T, f.R, c.T), f, minW, minH, ref seq);
                    TryShrink(cands, new Rc(f.L, c.B, f.R, f.B), f, minW, minH, ref seq);
                }
            }

            cands.Sort(delegate(Cand a, Cand b)
            {
                int c = a.Cost.CompareTo(b.Cost);
                return c != 0 ? c : a.Seq.CompareTo(b.Seq);
            });

            bool[] mustKeep = SubjectsWithZone(L, i);
            Cand chosen = null;
            int chosenQuality = -1;
            foreach (var c in cands)
            {
                if (chosen != null && c.Cost > chosen.Cost) break;
                Zone z;
                if (!Valid(L, i, c.To, mustKeep, out z)) continue;
                if (chosen == null || z.Quality > chosenQuality) { chosen = c; chosenQuality = z.Quality; }
            }
            if (chosen == null) return false;
            best = chosen.To;
            return true;
        }

        static void TryShrink(List<Cand> cands, Rc to, Rc f, int minW, int minH, ref int seq)
        {
            if (to.W < minW || to.H < minH || to.Same(f)) return;
            if (to.W < f.W * 2 / 3 || to.H < f.H * 2 / 3) return; // never collapse a window
            int shrunk = (f.W - to.W) + (f.H - to.H);
            cands.Add(new Cand { To = to, Cost = 3 * shrunk, Seq = seq++ });
        }

        static void AddUnique(List<int> list, int v) { if (!list.Contains(v)) list.Add(v); }

        /// <summary>A new, restored or app-moved window yields: the nearest spot on its monitor where every
        /// glimpse stays, at the same size; Strict mode then tries 70% and 50%; last resort a cascade offset.</summary>
        static void IntruderPass(Layout L, Request q, Outcome o, string[] reasons)
        {
            int t = q.Target;
            var w = L.Windows[t];
            if (!w.Movable) { reasons[t] = w.FixedReason; return; }
            if (!w.Subject) return;
            int n = L.Windows.Count;

            // windows that have a glimpse when the intruder is not there must keep it
            var keep = new bool[n];
            bool violates = false;
            for (int j = 0; j < n; j++)
            {
                if (j == t || !L.Windows[j].Subject) continue;
                keep[j] = L.ZoneWith(j, t, default(Rc), true) != null;
                if (keep[j] && L.ZoneOf(j) == null) violates = true;
            }
            if (!violates && L.ZoneOf(t) != null) return;

            Rc f = L.Frames[t];
            Rc work = L.WorkOf(f);
            var sizes = new List<int[]>();
            sizes.Add(new[] { f.W, f.H });
            if (q.AllowShrink && w.Resizable)
            {
                int P = L.P(t);
                foreach (double k in new[] { 0.7, 0.5 })
                {
                    int sw = Math.Max(Math.Max(w.MinW, 2 * P), (int)(f.W * k));
                    int sh = Math.Max(Math.Max(w.MinH, 2 * P), (int)(f.H * k));
                    if (sw < f.W || sh < f.H) sizes.Add(new[] { Math.Min(sw, f.W), Math.Min(sh, f.H) });
                }
            }
            foreach (int[] size in sizes)
            {
                Rc to;
                if (NearestSpot(L, t, size[0], size[1], work, keep, out to))
                {
                    Record(L, o, t, to, size[0] == f.W && size[1] == f.H ? "place" : "shrink");
                    return;
                }
            }

            // last resort: cascade offset from the window it covers most
            int c = -1;
            long bestArea = 0;
            for (int j = 0; j < n; j++)
            {
                if (j == t || !L.Windows[j].Subject) continue;
                long a = L.Frames[j].Intersect(f).Area;
                if (a > bestArea) { bestArea = a; c = j; }
            }
            if (c >= 0)
            {
                int P = L.P(t);
                Rc cf = L.Frames[c];
                Rc to = Geometry.ClampInto(Rc.XYWH(cf.L + P, cf.T + P, f.W, f.H), work);
                if (Violations(L, t, to, keep) < Violations(L, t, f, keep))
                {
                    Record(L, o, t, to, "cascade");
                    return;
                }
            }
            reasons[t] = "noSpace";
        }

        static int Violations(Layout L, int t, Rc at, bool[] keep)
        {
            int v = L.ZoneAt(t, at) == null ? 1 : 0;
            for (int j = 0; j < L.Windows.Count; j++)
                if (keep[j] && L.ZoneWith(j, t, at, false) == null) v++;
            return v;
        }

        static void AddPos(List<int> list, int v, int lo, int hi)
        {
            if (hi < lo) v = lo; else v = Math.Max(lo, Math.Min(hi, v));
            if (!list.Contains(v)) list.Add(v);
        }

        static bool NearestSpot(Layout L, int t, int w, int h, Rc work, bool[] keep, out Rc best)
        {
            best = default(Rc);
            Rc f = L.Frames[t];
            int xlo = work.L, xhi = work.R - w, ylo = work.T, yhi = work.B - h;
            var xs = new List<int>();
            var ys = new List<int>();
            AddPos(xs, f.L, xlo, xhi); AddPos(xs, work.L, xlo, xhi); AddPos(xs, work.R - w, xlo, xhi);
            AddPos(ys, f.T, ylo, yhi); AddPos(ys, work.T, ylo, yhi); AddPos(ys, work.B - h, ylo, yhi);
            for (int j = 0; j < L.Windows.Count; j++)
            {
                if (j == t || !L.Windows[j].Blocker) continue;
                Rc c = L.Frames[j];
                if (!c.Intersects(work)) continue;
                int Pj = L.P(j);
                AddPos(xs, c.R, xlo, xhi); AddPos(xs, c.L - w, xlo, xhi); AddPos(xs, c.L + Pj, xlo, xhi); AddPos(xs, c.R - Pj - w, xlo, xhi);
                AddPos(ys, c.B, ylo, yhi); AddPos(ys, c.T - h, ylo, yhi); AddPos(ys, c.T + Pj, ylo, yhi); AddPos(ys, c.B - Pj - h, ylo, yhi);
            }
            var cands = new List<long[]>(xs.Count * ys.Count);
            foreach (int x in xs)
                foreach (int y in ys)
                {
                    long dx = x - f.L, dy = y - f.T;
                    cands.Add(new[] { dx * dx + dy * dy, Math.Abs(dy), Math.Abs(dx), y, x });
                }
            cands.Sort(delegate(long[] a, long[] b)
            {
                for (int k = 0; k < 5; k++) { int c = a[k].CompareTo(b[k]); if (c != 0) return c; }
                return 0;
            });
            int limit = Math.Min(cands.Count, 8000);
            for (int k = 0; k < limit; k++)
            {
                Rc to = Rc.XYWH((int)cands[k][4], (int)cands[k][3], w, h);
                Zone z;
                if (Valid(L, t, to, keep, out z)) { best = to; return true; }
            }
            return false;
        }

        /// <summary>Snap guides while dragging: positions near the anchor where one of its edges just clears
        /// a neighbour's glimpse and nothing needs to move.</summary>
        public static List<Guide> Guides(Layout L, int anchor, int range)
        {
            var result = new List<Guide>();
            Rc f = L.Frames[anchor];
            int n = L.Windows.Count;
            // natural zones of the neighbours (without the anchor); they must all stay free
            var zones = new Zone[n];
            var keep = new bool[n];
            for (int j = 0; j < n; j++)
            {
                if (j == anchor || !L.Windows[j].Subject) continue;
                zones[j] = L.ZoneWith(j, anchor, default(Rc), true);
                keep[j] = zones[j] != null;
            }
            for (int j = 0; j < n; j++)
            {
                if (zones[j] == null) continue;
                Rc zr = zones[j].Rect;
                // anchor bottom above the zone, top below it, right edge left of it, left edge right of it
                TryGuide(L, anchor, f, result, keep, Edge.Bottom, zr.T, range, new Rc(f.L, zr.T - f.H, f.R, zr.T), zr);
                TryGuide(L, anchor, f, result, keep, Edge.Top, zr.B, range, new Rc(f.L, zr.B, f.R, zr.B + f.H), zr);
                TryGuide(L, anchor, f, result, keep, Edge.Right, zr.L, range, new Rc(zr.L - f.W, f.T, zr.L, f.B), zr);
                TryGuide(L, anchor, f, result, keep, Edge.Left, zr.R, range, new Rc(zr.R, f.T, zr.R + f.W, f.B), zr);
            }
            // by distance; equal distances keep the order they were found in (List.Sort is not stable)
            for (int k = 0; k < result.Count; k++) result[k].Seq = k;
            result.Sort(delegate(Guide a, Guide b)
            {
                int c = a.Distance.CompareTo(b.Distance);
                return c != 0 ? c : a.Seq.CompareTo(b.Seq);
            });
            return result;
        }

        static void TryGuide(Layout L, int anchor, Rc f, List<Guide> result, bool[] keep, Edge edge, int coord, int range, Rc snapped, Rc zone)
        {
            int dist = Math.Abs(snapped.L - f.L) + Math.Abs(snapped.T - f.T);
            if (dist == 0 || dist > range) return;
            // the snapped frame must not cut through the zone it clears
            if (snapped.Intersects(zone)) return;
            foreach (var g in result) if (g.Edge == edge && g.Coord == coord) return;
            Rc work = L.WorkOf(f);
            if (!work.Contains(snapped)) return;
            // every window that has a glimpse without the anchor keeps it, and the anchor has one
            Zone az;
            if (!Valid(L, anchor, snapped, keep, out az)) return;
            result.Add(new Guide { Edge = edge, Coord = coord, Snapped = snapped, Distance = dist, Zone = zone });
        }
    }

    internal sealed class Guide
    {
        public Edge Edge;       // which edge of the dragged window lines up
        public int Coord;       // x or y of the guide line
        public Rc Snapped;      // the dragged window's frame on the guide
        public Rc Zone;         // the neighbour's glimpse it clears
        public int Distance;
        public int Seq;         // order found, breaks distance ties
    }

    #endregion

    #region Self-test: shared geometry cases and unit tests (Glimpse.exe /selftest)

    internal static class SelfTest
    {
        static int pass, fail;

        public static int Run(string casesPath)
        {
            Console.WriteLine("Glimpse " + Program.Version + " self-test (" + (Program.IsScript ? "PowerShell" : "EXE") + ", " + (IntPtr.Size * 8) + "-bit, CLR " + Environment.Version + ")");
            try { UnitTests(); }
            catch (Exception ex) { Check(false, "unit tests crashed", ex.ToString()); }
            string path = casesPath ?? FindCases();
            if (path == null) Check(false, "geometry cases", "tests\\geometry-cases.txt not found; pass /selftest:<file>");
            else
            {
                try { RunCases(path); }
                catch (Exception ex) { Check(false, "geometry cases crashed", ex.ToString()); }
            }
            Console.WriteLine((fail == 0 ? "PASS" : "FAIL") + ": " + pass + " passed, " + fail + " failed");
            Console.Out.Flush();
            return fail == 0 ? 0 : 1;
        }

        static string FindCases()
        {
            string[] rel = { @"tests\geometry-cases.txt", "geometry-cases.txt", @"..\tests\geometry-cases.txt", @"..\..\tests\geometry-cases.txt", @"..\..\..\tests\geometry-cases.txt" };
            foreach (string r in rel)
            {
                string p = Path.Combine(Program.ProgramDir, r.Replace('\\', Path.DirectorySeparatorChar));
                if (File.Exists(p)) return Path.GetFullPath(p);
            }
            return null;
        }

        static void Check(bool ok, string name, string detail)
        {
            if (ok) { pass++; Console.WriteLine("ok - " + name); }
            else { fail++; Console.WriteLine("not ok - " + name + (string.IsNullOrEmpty(detail) ? "" : ": " + detail)); }
        }

        static void Eq(object expected, object actual, string name)
        {
            bool ok = Equals(expected, actual);
            Check(ok, name, ok ? null : "expected <" + expected + "> got <" + actual + ">");
        }

        // ------------------------------------------------------------------ unit tests

        static void UnitTests()
        {
            // intervals
            var s = IntervalSet.Of(0, 100);
            s.Subtract(10, 20);
            s.Subtract(50, 200);
            Eq("[0,9][21,49]", s.ToString(), "intervals: subtract");
            s.Add(5, 30);
            Eq("[0,49]", s.ToString(), "intervals: add merges");
            Eq("[10,20]", IntervalSet.Of(0, 20).Intersect(IntervalSet.Of(10, 40)).ToString(), "intervals: intersect");

            // INI: encodings, comments, round trip
            string text = "; comment\r\n[Glimpse]\r\nMode=Strict\r\n# other comment\r\nGlimpseSize = 72\r\n\r\n[MinSize1]\r\nTitleRegex=^Objedn\u00e1vka \u010d\\.\\d+\r\nMinWidth=480\r\nMinHeight=360\r\n";
            var utf8bom = new UTF8Encoding(true).GetPreamble();
            byte[] body = Encoding.UTF8.GetBytes(text);
            var withBom = new byte[utf8bom.Length + body.Length];
            Buffer.BlockCopy(utf8bom, 0, withBom, 0, utf8bom.Length);
            Buffer.BlockCopy(body, 0, withBom, utf8bom.Length, body.Length);
            var ini = IniFile.Parse(withBom);
            Eq(true, ini.Bom, "ini: UTF-8 BOM detected");
            Eq("Strict", ini.Get("glimpse", "mode"), "ini: keys and sections ignore case");
            Eq("72", ini.Get("Glimpse", "GlimpseSize"), "ini: spaces around =");
            Eq("^Objedn\u00e1vka \u010d\\.\\d+", ini.Get("MinSize1", "TitleRegex"), "ini: UTF-8 text with diacritics");
            ini.Set("Glimpse", "Mode", "Guide");
            ini.Set("Glimpse", "Language", "cs");
            ini.Set("XRay", "HotCorner", "Off");
            string round = ini.Text;
            Check(round.Contains("; comment") && round.Contains("# other comment"), "ini: comments survive a save", round);
            Check(round.IndexOf("Language=cs", StringComparison.Ordinal) < round.IndexOf("[MinSize1]", StringComparison.Ordinal), "ini: new key goes into its section", round);
            Check(round.TrimEnd().EndsWith("[XRay]\r\nHotCorner=Off"), "ini: new section appended", round);
            byte[] again = ini.ToBytes();
            Eq(0xEF, (int)again[0], "ini: BOM written back");
            var utf16 = IniFile.Parse(Concat(new byte[] { 0xFF, 0xFE }, new UnicodeEncoding(false, false).GetBytes(text)));
            Eq("^Objedn\u00e1vka \u010d\\.\\d+", utf16.Get("MinSize1", "TitleRegex"), "ini: UTF-16 LE with BOM");
            byte[] u16 = utf16.ToBytes();
            Check(u16.Length > 2 && u16[0] == 0xFF && u16[1] == 0xFE, "ini: UTF-16 written back as UTF-16", null);
            var nobom = IniFile.Parse(Encoding.UTF8.GetBytes("[A]\nKey=\u00e4\n"));
            Eq("\u00e4", nobom.Get("A", "Key"), "ini: UTF-8 without BOM");
            Eq("\n", nobom.NewLine, "ini: keeps LF line ends");

            // settings model
            var def = Settings.Defaults();
            Eq(FixMode.Assist, def.Mode, "settings: default mode Assist");
            Eq(50, def.GlimpseSize, "settings: default glimpse 50 px");
            Eq(1, def.MinSizes.Count, "settings: one default minimum-size rule");
            Eq(200, def.MinSizes[0].MinWidth, "settings: default minimum 200 px");
            Eq("Ctrl+Alt+Space", def.Hotkey, "settings: default hotkey");
            Eq(Corner.TopLeft, def.HotCorner, "settings: default hot corner");
            Eq(0, def.Exclusions.Count, "settings: no exclusions by default");
            Eq(false, def.StartWithWindows, "settings: Start with Windows is off by default");
            Eq(false, Settings.FromIni(IniFile.FromText("[Glimpse]\r\n")).StartWithWindows, "settings: Start with Windows stays off when the key is missing");
            Eq(0, def.Problems.Count, "settings: defaults have no problems");

            var st = Settings.Defaults();
            st.Mode = FixMode.Strict;
            st.GlimpseSize = 72;
            st.Language = "sk";
            st.HotCorner = Corner.BottomRight;
            st.Hotkey = "Ctrl+Shift+Alt+Space";
            st.MinSizes.Insert(0, new MinSizeRule { Pattern = @"^Order #\d+", MinWidth = 480, MinHeight = 360, Note = "ERP order forms" });
            st.Exclusions.Add(Exclusion.Process("teams.exe"));
            st.Exclusions.Add(Exclusion.Title("^Calculator$"));
            var ini2 = IniFile.FromText(Settings.DefaultIniText);
            st.WriteTo(ini2);
            var back = Settings.FromIni(IniFile.FromText(ini2.Text));
            Eq(FixMode.Strict, back.Mode, "settings: round trip mode");
            Eq(72, back.GlimpseSize, "settings: round trip glimpse size");
            Eq("sk", back.Language, "settings: round trip language");
            Eq(Corner.BottomRight, back.HotCorner, "settings: round trip hot corner");
            Eq("Ctrl+Shift+Alt+Space", back.Hotkey, "settings: round trip hotkey");
            Eq(2, back.MinSizes.Count, "settings: round trip rule count");
            Eq(@"^Order #\d+", back.MinSizes[0].Pattern, "settings: rules keep their order");
            Eq(".*", back.MinSizes[1].Pattern, "settings: catch-all rule stays last");
            Eq("ERP order forms", back.MinSizes[0].Note, "settings: rule note");
            Eq(2, back.Exclusions.Count, "settings: round trip exclusions");
            Check(ini2.Text.Contains("; minimum sizes: rules are tried"), "settings: comments kept on save", null);
            Check(ini2.Text.IndexOf("[MinSize2]", StringComparison.Ordinal) < ini2.Text.IndexOf("[Experimental]", StringComparison.Ordinal), "settings: rules stay before [Experimental]", ini2.Text);
            var twice = IniFile.FromText(ini2.Text);
            back.WriteTo(twice);
            Eq(ini2.Text, twice.Text, "settings: saving twice changes nothing");

            Eq(@"^Order #\d+", back.RuleFor("order #4711 - ERP").Pattern, "rules: case-insensitive, first match wins");
            Eq(".*", back.RuleFor("Inbox - Outlook").Pattern, "rules: catch-all");
            Check(back.IsExcluded("Teams.exe", "Chat"), "exclusions: process name, any case", null);
            Check(back.IsExcluded("calc.exe", "Calculator"), "exclusions: title pattern", null);
            Check(!back.IsExcluded("notepad.exe", "Calculator notes"), "exclusions: anchored pattern", null);
            var bad = Settings.FromIni(IniFile.FromText("[MinSize1]\nTitleRegex=([unclosed\nMinWidth=300\n[MinSize2]\nTitleRegex=.*\nMinWidth=200\nMinHeight=200\n"));
            Check(bad.Problems.Count == 1, "rules: an invalid pattern is reported", string.Join("; ", bad.Problems.ToArray()));
            Eq(".*", bad.RuleFor("anything").Pattern, "rules: an invalid pattern disables only its own rule");
            var ecma = new MinSizeRule { Pattern = @"^\w+$" };
            ecma.Compile();
            Check(!ecma.Matches("Objedn\u00e1vka"), "rules: ECMAScript \\w is ASCII only, as in std::regex", null);

            // hotkeys
            HotkeySpec hk;
            Check(HotkeySpec.TryParse("Ctrl+Alt+Space", out hk) && hk.Mods == (Native.MOD_CONTROL | Native.MOD_ALT) && hk.Vk == 0x20, "hotkey: Ctrl+Alt+Space", null);
            Check(HotkeySpec.TryParse("alt + shift + ctrl + enter", out hk) && hk.ToString() == "Ctrl+Shift+Alt+Enter", "hotkey: canonical order", hk.ToString());
            Check(HotkeySpec.TryParse("Pause", out hk) && hk.Vk == 0x13 && hk.Mods == 0, "hotkey: Pause alone", null);
            Check(!HotkeySpec.TryParse("Ctrl+Alt", out hk), "hotkey: modifiers only is invalid", null);
            Check(!HotkeySpec.TryParse("Ctrl+Alt+Foo", out hk), "hotkey: unknown key is invalid", null);
            Check(HotkeySpec.TryParse("Ctrl+Alt+F12", out hk) && hk.Vk == 0x7B, "hotkey: F-keys", null);
            Eq("Ctrl+Alt+Space", HotkeySpec.FromKeys(Keys.Control | Keys.Alt | Keys.Space).ToString(), "hotkey: from recorded keys");

            // texts and plural rules
            Eq("one", Texts.Category("cs", 1), "plural cs 1");
            Eq("few", Texts.Category("cs", 3), "plural cs 3");
            Eq("other", Texts.Category("sk", 5), "plural sk 5");
            Eq("few", Texts.Category("pl", 22), "plural pl 22");
            Eq("many", Texts.Category("pl", 25), "plural pl 25");
            Eq("one", Texts.Category("ru", 21), "plural ru 21");
            Eq("other", Texts.Category("de", 0), "plural de 0");
            Eq("one", Texts.Category("fr", 0), "plural fr 0");
            Texts.Load("en");
            Eq("3 windows", Texts.P("windows", 3), "texts: English plural");
            Eq("1 window", Texts.P("windows", 1), "texts: English singular");
            Eq("moves 80 px on drop", Texts.F("drag.movesPx", "px", 80), "texts: named placeholder");
            Eq("no.such.key", Texts.T("no.such.key"), "texts: unknown key shows the key");
            var pseudo = Texts.Pseudo(Texts.English);
            Check(pseudo["windows.other"].Contains("{count}") && pseudo["windows.other"].Length >= Texts.English["windows.other"].Length * 1.3, "texts: pseudo-language keeps placeholders, 40% longer", pseudo["windows.other"]);
            CheckEnglishFile();

            // options
            var o = Options.Parse(new[] { "/ini:%APPDATA%\\Glimpse\\Glimpse.ini" });
            Eq("%APPDATA%\\Glimpse\\Glimpse.ini", o.IniArgument, "options: /ini:path");
            o = Options.Parse(new[] { "-Ini", "C:\\x y\\G.ini", "/settings" });
            Check(o.IniArgument == "C:\\x y\\G.ini" && o.ShowSettings, "options: -Ini path /settings", null);
            Check(Options.Parse(new[] { "/bogus" }).Error != null, "options: unknown option", null);

            // misc
            Eq("Order ## \u2014 ERP", Util.TitleShape("Order #4711 \u2014 ERP"), "size memory: title shape");
            Eq(75, Geometry.GlimpsePx(50, 144), "glimpse: 50 px at 150% is 75 px");
            Eq(300, Util.Scale(200, 144), "minimum size: 200 px at 150% is 300 px");
        }

        static byte[] Concat(byte[] a, byte[] b)
        {
            var r = new byte[a.Length + b.Length];
            Buffer.BlockCopy(a, 0, r, 0, a.Length);
            Buffer.BlockCopy(b, 0, r, a.Length, b.Length);
            return r;
        }

        static void CheckEnglishFile()
        {
            string[] places = { Path.Combine(Texts.LangFolder, "en.txt"), Path.Combine(Program.ProgramDir, Path.Combine("..", Path.Combine("lang", "en.txt"))) };
            foreach (string p in places)
            {
                if (!File.Exists(p)) continue;
                var file = Texts.Parse(File.ReadAllText(p, Encoding.UTF8));
                var diff = new List<string>();
                foreach (var kv in Texts.English)
                {
                    string v;
                    if (!file.TryGetValue(kv.Key, out v)) diff.Add("missing " + kv.Key);
                    else if (v != kv.Value) diff.Add("differs " + kv.Key);
                }
                foreach (var kv in file) if (!Texts.English.ContainsKey(kv.Key)) diff.Add("extra " + kv.Key);
                Check(diff.Count == 0, "texts: lang\\en.txt matches the built-in English", string.Join(", ", diff.ToArray()));
                return;
            }
            Console.WriteLine("# lang\\en.txt not found next to the program; skipped");
        }

        // ------------------------------------------------------------------ geometry cases

        sealed class Case
        {
            public string Name;
            public Layout L = new Layout();
            public Dictionary<string, int> Ids = new Dictionary<string, int>();
            public List<string[]> Windows = new List<string[]>();
            public FixMode Mode = FixMode.Assist;
            public bool Place = true;
            public string[] Action;
            public List<string[]> Expects = new List<string[]>();
            public List<string> Already = new List<string>();
            public int Line;
        }

        static void RunCases(string path)
        {
            Console.WriteLine("# cases: " + path);
            string[] lines = File.ReadAllLines(path, Encoding.UTF8);
            Case c = null;
            int count = 0;
            for (int ln = 0; ln < lines.Length; ln++)
            {
                string line = lines[ln];
                int hash = line.IndexOf('#');
                if (hash >= 0) line = line.Substring(0, hash);
                line = line.Trim();
                if (line.Length == 0) continue;
                string[] t = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                string kw = t[0].ToLowerInvariant();
                try
                {
                    if (kw == "case") { c = new Case { Name = line.Substring(4).Trim(), Line = ln + 1 }; continue; }
                    if (c == null) throw new FormatException("'" + kw + "' outside a case");
                    switch (kw)
                    {
                        case "monitor":
                            {
                                Rc b = R(t, 1), w = b;
                                int dpi = 96;
                                for (int i = 5; i < t.Length; i++)
                                {
                                    if (t[i] == "work") { w = R(t, i + 1); i += 4; }
                                    else if (t[i] == "dpi") { dpi = int.Parse(t[i + 1], CultureInfo.InvariantCulture); i++; }
                                }
                                c.L.Monitors.Add(new GeoMonitor(b, w, dpi));
                                break;
                            }
                        case "glimpse": c.L.GlimpseSize = int.Parse(t[1], CultureInfo.InvariantCulture); break;
                        case "mode": c.Mode = (FixMode)Enum.Parse(typeof(FixMode), t[1], true); break;
                        case "place": c.Place = t[1] == "1" || t[1] == "on"; break;
                        case "window": c.Windows.Add(t); break;
                        case "already": c.Already.Add(t[1]); break;
                        case "action": c.Action = t; break;
                        case "expect": c.Expects.Add(t); break;
                        case "end":
                            RunCase(c);
                            count++;
                            c = null;
                            break;
                        default: throw new FormatException("unknown keyword '" + kw + "'");
                    }
                }
                catch (Exception ex)
                {
                    Check(false, "cases line " + (ln + 1), ex.Message);
                    c = null;
                }
            }
            Check(count > 0, "geometry cases found", count + " cases");
        }

        static Rc R(string[] t, int i)
        {
            return new Rc(int.Parse(t[i], CultureInfo.InvariantCulture), int.Parse(t[i + 1], CultureInfo.InvariantCulture),
                int.Parse(t[i + 2], CultureInfo.InvariantCulture), int.Parse(t[i + 3], CultureInfo.InvariantCulture));
        }

        static Rc RList(string s)
        {
            string[] p = s.Split(',');
            return new Rc(int.Parse(p[0], CultureInfo.InvariantCulture), int.Parse(p[1], CultureInfo.InvariantCulture),
                int.Parse(p[2], CultureInfo.InvariantCulture), int.Parse(p[3], CultureInfo.InvariantCulture));
        }

        static void RunCase(Case c)
        {
            if (c.L.Monitors.Count == 0) c.L.Monitors.Add(new GeoMonitor(new Rc(0, 0, 1920, 1080), new Rc(0, 0, 1920, 1040), 96));
            foreach (string[] t in c.Windows)
            {
                var w = new GeoWindow { Id = t[1], Frame = R(t, 2) };
                w.Dpi = c.L.Monitors[Geometry.MonitorOf(w.Frame, c.L.Monitors)].Dpi;
                for (int i = 6; i < t.Length; i++)
                {
                    string f = t[i].ToLowerInvariant();
                    if (f == "elevated" || f == "stubborn" || f == "excluded" || f == "hung") { w.Movable = false; w.FixedReason = f; }
                    else if (f == "topmost") { w.Movable = false; w.Subject = false; w.FixedReason = f; }
                    else if (f == "maximized" || f == "notcounted") { w.Subject = false; w.Blocker = false; w.Movable = false; w.FixedReason = f; }
                    else if (f == "noresize") w.Resizable = false;
                    else if (f.StartsWith("min="))
                    {
                        string[] wh = f.Substring(4).Split('x');
                        w.MinW = Util.Scale(int.Parse(wh[0], CultureInfo.InvariantCulture), w.Dpi);
                        w.MinH = Util.Scale(int.Parse(wh[1], CultureInfo.InvariantCulture), w.Dpi);
                    }
                    else if (f.StartsWith("caption=")) w.CaptionW = Util.Scale(int.Parse(f.Substring(8), CultureInfo.InvariantCulture), w.Dpi);
                    else if (f.StartsWith("icon=")) w.IconW = Util.Scale(int.Parse(f.Substring(5), CultureInfo.InvariantCulture), w.Dpi);
                    else if (f.StartsWith("prev=")) w.PrevWork = RList(f.Substring(5));
                    else throw new FormatException("unknown window flag " + t[i]);
                }
                c.Ids[w.Id] = c.L.Windows.Count;
                c.L.Windows.Add(w);
            }
            c.L.Begin();

            Outcome before = Resolver.Resolve(c.L, new Request { Fix = false });
            c.L.Begin();
            Outcome after = before;
            List<Guide> guides = null;
            string[] a = c.Action ?? new[] { "action", "check" };
            string act = a[1].ToLowerInvariant();
            if (act == "guides")
            {
                int idx = c.Ids[a[2]];
                guides = Resolver.Guides(c.L, idx, int.Parse(a[3], CultureInfo.InvariantCulture));
            }
            else if (act != "check")
            {
                Cause cause;
                switch (act)
                {
                    case "drop": cause = Cause.Drop; break;
                    case "new": cause = Cause.New; break;
                    case "restore": cause = Cause.Restore; break;
                    case "appmove": cause = Cause.AppMove; break;
                    case "appresize": cause = Cause.AppResize; break;
                    case "display": cause = Cause.Display; break;
                    case "arrange": cause = Cause.Arrange; break;
                    case "glimpse": cause = Cause.GiveGlimpse; break;
                    default: throw new FormatException("unknown action " + act);
                }
                int target = a.Length > 2 && c.Ids.ContainsKey(a[2]) ? c.Ids[a[2]] : -1;
                var q = Request.For(cause, c.Mode, target, c.Place);
                for (int i = 3; i < a.Length; i++)
                {
                    if (a[i].StartsWith("edges=")) q.DraggedEdges = a[i].Substring(6).ToUpperInvariant();
                    else if (a[i].StartsWith("from=")) q.AnchorStart = RList(a[i].Substring(5));
                }
                foreach (string id in c.Already) q.AlreadyMoved.Add(id);
                after = Resolver.Resolve(c.L, q);
            }

            var problems = new List<string>();
            foreach (string[] e in c.Expects)
            {
                string what = e[1].ToLowerInvariant();
                string err = null;
                Func<string, int> id = delegate(string name)
                {
                    int ix;
                    if (!c.Ids.TryGetValue(name, out ix)) throw new FormatException("unknown window " + name);
                    return ix;
                };
                switch (what)
                {
                    case "zone":
                        {
                            Outcome src = act == "check" ? before : after;
                            Zone z = src.Zones[id(e[2])];
                            string got = z == null ? "none" : z.Edge.ToString().ToLowerInvariant();
                            if (got != e[3].ToLowerInvariant()) err = "zone of " + e[2] + ": expected " + e[3] + " got " + got + (z == null ? "" : " " + z.Rect);
                            else if (e.Length > 4 && z != null && z.Quality != int.Parse(e[4], CultureInfo.InvariantCulture)) err = "zone quality of " + e[2] + ": expected " + e[4] + " got " + z.Quality;
                            break;
                        }
                    case "free":
                        if (after.Zones[id(e[2])] == null) err = e[2] + " has no free glimpse (" + after.Reasons[id(e[2])] + ")";
                        break;
                    case "hidden":
                        {
                            int ix = id(e[2]);
                            if (after.Zones[ix] != null) err = e[2] + " still has a glimpse " + after.Zones[ix].Rect;
                            else if (e.Length > 3 && after.Reasons[ix] != e[3]) err = e[2] + " hidden because " + after.Reasons[ix] + ", expected " + e[3];
                            break;
                        }
                    case "at":
                        {
                            Rc want = R(e, 3), got = after.Frames[id(e[2])];
                            if (!want.Same(got)) err = e[2] + " at " + got + ", expected " + want;
                            break;
                        }
                    case "moved":
                        if (after.MoveOf(id(e[2])) == null) err = e[2] + " was not moved";
                        break;
                    case "unchanged":
                        {
                            var m = after.MoveOf(id(e[2]));
                            if (m != null) err = e[2] + " moved to " + m.To;
                            break;
                        }
                    case "moves":
                        {
                            int want = int.Parse(e[2], CultureInfo.InvariantCulture);
                            if (after.Moves.Count != want)
                            {
                                var sb = new StringBuilder();
                                foreach (var m in after.Moves) sb.Append(" " + m.W.Id + "->" + m.To + "(" + m.Why + ")");
                                err = "expected " + want + " moves, got " + after.Moves.Count + ":" + sb;
                            }
                            break;
                        }
                    case "why":
                        {
                            var m = after.MoveOf(id(e[2]));
                            if (m == null || m.Why != e[3]) err = e[2] + " moved because " + (m == null ? "-" : m.Why) + ", expected " + e[3];
                            break;
                        }
                    case "guide":
                        {
                            bool found = false;
                            if (guides != null)
                                foreach (var g in guides)
                                    if (g.Edge.ToString().ToLowerInvariant() == e[2].ToLowerInvariant() && g.Coord == int.Parse(e[3], CultureInfo.InvariantCulture)) found = true;
                            if (!found)
                            {
                                var sb = new StringBuilder();
                                if (guides != null) foreach (var g in guides) sb.Append(" " + g.Edge + "@" + g.Coord);
                                err = "no " + e[2] + " guide at " + e[3] + "; got" + sb;
                            }
                            break;
                        }
                    case "guides":
                        {
                            int want = int.Parse(e[2], CultureInfo.InvariantCulture);
                            int got = guides == null ? 0 : guides.Count;
                            if (got != want) err = "expected " + want + " guides, got " + got;
                            break;
                        }
                    default: err = "unknown expectation " + what; break;
                }
                if (err != null) problems.Add(err);
            }
            Check(problems.Count == 0, "case " + c.Name, string.Join("; ", problems.ToArray()));
        }
    }

    /// <summary>Glimpse.exe /checklang: missing, obsolete and mismatched keys per language file.</summary>
    internal static class LangCheck
    {
        public static int Run()
        {
            var en = Texts.English;
            int problems = 0;
            string folder = Texts.LangFolder;
            if (!Directory.Exists(folder)) { Console.WriteLine("no lang folder next to the program: " + folder); return 1; }
            var files = Directory.GetFiles(folder, "*.txt");
            Array.Sort(files, StringComparer.OrdinalIgnoreCase);
            foreach (string f in files)
            {
                string code = Path.GetFileNameWithoutExtension(f);
                var d = Texts.Parse(File.ReadAllText(f, Encoding.UTF8));
                var lines = new List<string>();
                foreach (var kv in en)
                {
                    if (kv.Key.StartsWith("_")) continue;
                    string baseKey = PluralBase(kv.Key);
                    if (baseKey != null)
                    {
                        // a plural key needs .other and the categories its language uses
                        if (!HasAnyForm(d, baseKey)) lines.Add("missing   " + kv.Key);
                        continue;
                    }
                    string v;
                    if (!d.TryGetValue(kv.Key, out v)) { lines.Add("missing   " + kv.Key); continue; }
                    string pe = Placeholders(kv.Value), pv = Placeholders(v);
                    if (pe != pv) lines.Add("outdated  " + kv.Key + " (placeholders " + pv + ", English has " + pe + ")");
                }
                foreach (var kv in d)
                {
                    if (kv.Key.StartsWith("_")) continue;
                    string baseKey = PluralBase(kv.Key);
                    bool known = en.ContainsKey(kv.Key) || (baseKey != null && HasAnyForm(en, baseKey));
                    if (!known) lines.Add("obsolete  " + kv.Key);
                }
                // plural categories this language needs
                var cats = new HashSet<string>();
                for (int n = 0; n < 200; n++) cats.Add(Texts.Category(code, n));
                foreach (var kv in en)
                {
                    string baseKey = PluralBase(kv.Key);
                    if (baseKey == null || !kv.Key.EndsWith(".other")) continue;
                    foreach (string cat in cats)
                        if (!d.ContainsKey(baseKey + "." + cat) && HasAnyForm(d, baseKey)) lines.Add("missing   " + baseKey + "." + cat + " (plural form for " + code + ")");
                }
                lines.Sort(StringComparer.Ordinal);
                var unique = new List<string>();
                foreach (string l in lines) if (unique.Count == 0 || unique[unique.Count - 1] != l) unique.Add(l);
                Console.WriteLine(code + ": " + (unique.Count == 0 ? "complete" : unique.Count + " problem(s)"));
                foreach (string l in unique) Console.WriteLine("  " + l);
                if (!code.Equals("en", StringComparison.OrdinalIgnoreCase)) problems += unique.Count;
            }
            return problems == 0 ? 0 : 1;
        }

        static readonly string[] Forms = { ".zero", ".one", ".two", ".few", ".many", ".other" };

        static string PluralBase(string key)
        {
            foreach (string f in Forms) if (key.EndsWith(f)) return key.Substring(0, key.Length - f.Length);
            return null;
        }

        static bool HasAnyForm(Dictionary<string, string> d, string baseKey)
        {
            foreach (string f in Forms) if (d.ContainsKey(baseKey + f)) return true;
            return false;
        }

        static string Placeholders(string s)
        {
            var list = new List<string>();
            foreach (Match m in Regex.Matches(s, @"\{[a-z]+\}")) if (!list.Contains(m.Value)) list.Add(m.Value);
            list.Sort(StringComparer.Ordinal);
            return string.Join("", list.ToArray());
        }
    }

    #endregion

    #region Window registry: monitors and the windows that count

    internal sealed class MonitorInfo
    {
        public IntPtr Handle;
        public Rc Bounds, Work;
        public int Dpi = 96;
        public bool Primary;
        public string Device = "";
    }

    internal static class Monitors
    {
        public static List<MonitorInfo> Read()
        {
            var list = new List<MonitorInfo>();
            if (!Native.IsWindows)
            {
                list.Add(new MonitorInfo { Bounds = new Rc(0, 0, 1920, 1080), Work = new Rc(0, 0, 1920, 1040), Primary = true });
                return list;
            }
            Native.MonitorEnumProc cb = delegate(IntPtr hmon, IntPtr hdc, ref Native.RECT r, IntPtr data)
            {
                var mi = new Native.MONITORINFOEX();
                mi.cbSize = Marshal.SizeOf(typeof(Native.MONITORINFOEX));
                if (Native.GetMonitorInfo(hmon, ref mi))
                    list.Add(new MonitorInfo
                    {
                        Handle = hmon, Bounds = Rc.From(mi.rcMonitor), Work = Rc.From(mi.rcWork),
                        Dpi = Native.DpiForMonitor(hmon), Primary = (mi.dwFlags & 1) != 0, Device = mi.szDevice ?? ""
                    });
                return true;
            };
            Native.EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, cb, IntPtr.Zero);
            GC.KeepAlive(cb);
            if (list.Count == 0) list.Add(new MonitorInfo { Bounds = new Rc(0, 0, 1024, 768), Work = new Rc(0, 0, 1024, 768), Primary = true });
            list.Sort(delegate(MonitorInfo a, MonitorInfo b)
            {
                int c = a.Bounds.L.CompareTo(b.Bounds.L);
                return c != 0 ? c : a.Bounds.T.CompareTo(b.Bounds.T);
            });
            return list;
        }

        /// <summary>Layout signature: bounds, work area and DPI of every monitor.</summary>
        public static string Signature(List<MonitorInfo> list)
        {
            var sb = new StringBuilder();
            foreach (var m in list) sb.Append(m.Bounds).Append('|').Append(m.Work).Append('|').Append(m.Dpi).Append(';');
            return sb.ToString();
        }

        public static MonitorInfo At(List<MonitorInfo> list, Rc r)
        {
            if (list.Count == 0) return null;
            var geo = new List<GeoMonitor>();
            foreach (var m in list) geo.Add(new GeoMonitor(m.Bounds, m.Work, m.Dpi));
            return list[Geometry.MonitorOf(r, geo)];
        }

        public static MonitorInfo AtPoint(List<MonitorInfo> list, int x, int y)
        {
            foreach (var m in list) if (m.Bounds.Contains(x, y)) return m;
            return list.Count > 0 ? At(list, new Rc(x, y, x + 1, y + 1)) : null;
        }
    }

    internal sealed class TrackedWindow
    {
        public IntPtr Handle;
        public uint Pid;
        public string Process = "";
        public string Class = "";
        public string Title = "";
        public Rc Frame;            // visible bounds (DWM extended frame), physical px
        public Rc WindowRect;       // GetWindowRect, includes the invisible resize border
        public bool Counted;        // visible app window, not minimized, not cloaked
        public bool Minimized, Maximized, FullScreen;
        public bool Topmost, Elevated, Hung, Resizable, HasCaption, Excluded, Stubborn;
        public int Dpi = 96;
        public long LastActive;     // MRU: last time it was the foreground window
        public long ZOrder;         // position in the last EnumWindows pass (0 = top)
        public bool Placed;         // new-window placement already ran (or it existed at start)
        public bool WasMaximized;
        public Rc SettledFrame;     // frame after the last processed change
        public long SuppressUntil;  // ignore location events caused by our own move
        public Rc FixedFrom, FixedTo;
        public long FixedAt;
        public readonly List<long> Reverts = new List<long>();
        public string RuleKey;      // the minimum-size rule last applied (title changes)
        public TopEdgeMap TopMap;
        public int TopMapWidth = -1;
        public int CaptionW = -1, CaptionH = -1, IconW = -1;
        public Rc PrevWork;
        public bool Hidden;         // no free glimpse: reachable through X-ray
        public string HiddenReason;
        public Bitmap Icon;
        public bool IconTried;
        public long UserPlacedAt;
        public Zone Zone;           // its free glimpse at the last check

        public string ShortTitle { get { return Util.Shorten(Title.Length > 0 ? Title : Process, 60); } }
    }

    internal sealed class WindowRegistry
    {
        public readonly Dictionary<IntPtr, TrackedWindow> All = new Dictionary<IntPtr, TrackedWindow>();
        readonly Dictionary<uint, string> processNames = new Dictionary<uint, string>();
        readonly Dictionary<uint, int> integrity = new Dictionary<uint, int>();
        readonly uint ownPid = (uint)Process.GetCurrentProcess().Id;
        readonly int ownIntegrity;
        long probeBudgetMs;

        static readonly HashSet<string> ShellClasses = new HashSet<string>(StringComparer.Ordinal)
        {
            "Shell_TrayWnd", "Shell_SecondaryTrayWnd", "Progman", "WorkerW", "NotifyIconOverflowWindow",
            "TopLevelWindowForOverflowXamlIsland", "XamlExplorerHostIslandWindow", "Windows.UI.Core.CoreWindow",
            "MultitaskingViewFrame", "ForegroundStaging", "Shell_InputSwitchTopLevelWindow",
            "ApplicationManager_ImmersiveShellWindow", "EdgeUiInputTopWndClass", "Internet Explorer_Hidden",
            "#32768", "#32770_hidden", "tooltips_class32", "SysShadow", "ThumbnailDeviceHelperWnd",
            "Windows.Internal.Shell.TabProxyWindow", "DummyDWMListenerWindow", "IME", "MSCTFIME UI",
            "Shell_LightDismissOverlay", "Windows.UI.Input.InputSite.WindowClass", "SnapAssistFlyout",
            "TaskListThumbnailWnd", "TaskListOverlayWnd", "CEF-OSC-WIDGET", "PseudoConsoleWindow"
        };

        public WindowRegistry()
        {
            ownIntegrity = Native.IsWindows ? Native.IntegrityLevel(0) : 0x2000;
        }

        public TrackedWindow Get(IntPtr h)
        {
            TrackedWindow w;
            return All.TryGetValue(h, out w) ? w : null;
        }

        /// <summary>Visible top-level app window, roughly what Alt+Tab shows (minimized ones included).</summary>
        public bool IsAppWindow(IntPtr h, out string cls)
        {
            cls = "";
            if (!Native.IsWindowVisible(h)) return false;
            if (Native.GetAncestor(h, Native.GA_ROOT) != h) return false;
            long style = Native.GetWindowLongPtr(h, Native.GWL_STYLE);
            long ex = Native.GetWindowLongPtr(h, Native.GWL_EXSTYLE);
            if ((style & Native.WS_CHILD) != 0) return false;
            bool app = (ex & Native.WS_EX_APPWINDOW) != 0;
            if (!app)
            {
                if ((ex & Native.WS_EX_TOOLWINDOW) != 0) return false;
                if ((ex & Native.WS_EX_NOACTIVATE) != 0) return false;
                if (Native.GetWindow(h, Native.GW_OWNER) != IntPtr.Zero) return false;
            }
            uint pid;
            Native.GetWindowThreadProcessId(h, out pid);
            if (pid == ownPid) return false;
            var sb = new StringBuilder(256);
            Native.GetClassName(h, sb, sb.Capacity);
            cls = sb.ToString();
            if (ShellClasses.Contains(cls)) return false;
            if (!app && Native.GetWindowTextLength(h) == 0) return false;
            if (Native.IsCloaked(h)) return false;
            return true;
        }

        string ProcessName(uint pid)
        {
            string n;
            if (processNames.TryGetValue(pid, out n)) return n;
            n = Native.ProcessFileName(pid);
            processNames[pid] = n;
            return n;
        }

        bool IsElevated(uint pid)
        {
            int il;
            if (!integrity.TryGetValue(pid, out il)) { il = Native.IntegrityLevel(pid); integrity[pid] = il; }
            if (ownIntegrity <= 0) return false;
            return il < 0 || il > ownIntegrity;
        }

        static string Text(IntPtr h)
        {
            int len = Native.GetWindowTextLength(h);
            if (len <= 0) return "";
            var sb = new StringBuilder(Math.Min(len + 1, 1024));
            Native.GetWindowText(h, sb, sb.Capacity);
            return sb.ToString();
        }

        /// <summary>Re-reads every window: frames, states and flags. Cheap (a few syscalls per window).</summary>
        public void Refresh(Settings s, List<MonitorInfo> mons)
        {
            if (!Native.IsWindows) return;
            var seen = new HashSet<IntPtr>();
            long z = 0;
            probeBudgetMs = Clock.Ms + 120;
            Native.EnumWindowsProc cb = delegate(IntPtr h, IntPtr lp)
            {
                string cls;
                if (!IsAppWindow(h, out cls)) return true;
                seen.Add(h);
                TrackedWindow w;
                if (!All.TryGetValue(h, out w))
                {
                    w = new TrackedWindow { Handle = h, Class = cls };
                    Native.GetWindowThreadProcessId(h, out w.Pid);
                    w.Process = ProcessName(w.Pid);
                    w.Elevated = IsElevated(w.Pid);
                    All[h] = w;
                }
                w.ZOrder = z++;
                Update(w, s, mons);
                return true;
            };
            Native.EnumWindows(cb, IntPtr.Zero);
            GC.KeepAlive(cb);
            var gone = new List<IntPtr>();
            foreach (var kv in All) if (!seen.Contains(kv.Key)) gone.Add(kv.Key);
            foreach (IntPtr h in gone)
            {
                var w = All[h];
                if (w.Icon != null) w.Icon.Dispose();
                All.Remove(h);
            }
            if (gone.Count > 0)
            {
                var live = new HashSet<uint>();
                foreach (var w in All.Values) live.Add(w.Pid);
                foreach (uint pid in new List<uint>(processNames.Keys)) if (!live.Contains(pid)) { processNames.Remove(pid); integrity.Remove(pid); }
            }
        }

        public void Update(TrackedWindow w, Settings s, List<MonitorInfo> mons)
        {
            IntPtr h = w.Handle;
            w.Title = Text(h);
            long style = Native.GetWindowLongPtr(h, Native.GWL_STYLE);
            long ex = Native.GetWindowLongPtr(h, Native.GWL_EXSTYLE);
            w.Minimized = Native.IsIconic(h);
            w.WasMaximized = w.Maximized;
            w.Maximized = Native.IsZoomed(h);
            w.Topmost = (ex & Native.WS_EX_TOPMOST) != 0;
            w.Resizable = (style & Native.WS_THICKFRAME) != 0;
            w.HasCaption = (style & Native.WS_CAPTION) == Native.WS_CAPTION;
            w.Hung = Native.IsHungAppWindow(h);
            w.Excluded = s != null && s.IsExcluded(w.Process, w.Title);
            Native.RECT r;
            if (Native.GetWindowRect(h, out r)) w.WindowRect = Rc.From(r);
            if (Native.TryFrame(h, out r)) w.Frame = Rc.From(r);
            var mon = Monitors.At(mons, w.Frame);
            w.Dpi = mon == null ? 96 : mon.Dpi;
            w.FullScreen = !w.Maximized && mon != null && w.Frame.Contains(mon.Bounds) && !w.HasCaption;
            w.Counted = !w.Minimized;
            if (w.Counted && !w.Hung && w.Frame.W != w.TopMapWidth && Clock.Ms < probeBudgetMs) ProbeTopEdge(w);
        }

        public void Touch(IntPtr h)
        {
            var w = Get(h);
            if (w != null) w.LastActive = Clock.Ms;
        }

        /// <summary>Counted windows, most recently used first (never-activated ones by z-order).</summary>
        public List<TrackedWindow> CountedByMru()
        {
            var list = new List<TrackedWindow>();
            foreach (var w in All.Values) if (w.Counted) list.Add(w);
            list.Sort(CompareMru);
            return list;
        }

        public List<TrackedWindow> AllByMru()
        {
            var list = new List<TrackedWindow>(All.Values);
            list.Sort(CompareMru);
            return list;
        }

        static int CompareMru(TrackedWindow a, TrackedWindow b)
        {
            int c = b.LastActive.CompareTo(a.LastActive);
            return c != 0 ? c : a.ZOrder.CompareTo(b.ZOrder);
        }

        /// <summary>What a click along the title bar does, from the window's own hit-test and the
        /// caption-button bounds (this also covers browsers and Office with custom title bars).</summary>
        void ProbeTopEdge(TrackedWindow w)
        {
            Rc f = w.Frame;
            w.TopMapWidth = f.W;
            w.TopMap = null;
            if (f.W < 40 || f.H < 20) return;
            var map = new TopEdgeMap();
            int y = f.T + Util.Scale(12, w.Dpi);
            int step = Math.Max(Util.Scale(16, w.Dpi), f.W / 24);
            int capL = int.MaxValue, capR = int.MinValue;
            try
            {
                Native.RECT cb;
                if (Native.DwmGetWindowAttribute(w.Handle, Native.DWMWA_CAPTION_BUTTON_BOUNDS, out cb, Marshal.SizeOf(typeof(Native.RECT))) == 0 && cb.Right > cb.Left)
                {
                    // relative to the window rectangle
                    capL = w.WindowRect.L + cb.Left;
                    capR = w.WindowRect.L + cb.Right;
                    w.CaptionH = Math.Max(Util.Scale(24, w.Dpi), cb.Bottom - cb.Top);
                }
            }
            catch { }
            int prevQ = -1;
            for (int x = f.L + 2; x < f.R; x += step)
            {
                int q;
                if (x >= capL && x < capR) q = 0;
                else
                {
                    var pt = new Native.POINT(x, y);
                    try { Native.PhysicalToLogicalPointForPerMonitorDPI(w.Handle, ref pt); } catch { }
                    IntPtr res;
                    IntPtr lp = new IntPtr((pt.Y << 16) | (pt.X & 0xFFFF));
                    IntPtr ok = Native.SendMessageTimeout(w.Handle, Native.WM_NCHITTEST, IntPtr.Zero, lp, Native.SMTO_ABORTIFHUNG, 40, out res);
                    if (ok == IntPtr.Zero) { w.TopMap = null; return; } // slow or hung: keep the default map
                    q = HitQuality((int)res.ToInt64());
                }
                if (q != prevQ) { map.Add(Math.Max(0, x - f.L - (prevQ < 0 ? 2 : step / 2)), q); prevQ = q; }
                if (Clock.Ms > probeBudgetMs + 200) break;
            }
            if (map.Starts.Count > 0)
            {
                map.Starts[0] = 0;
                w.TopMap = map;
            }
        }

        static int HitQuality(int ht)
        {
            switch (ht)
            {
                case Native.HTCAPTION: case Native.HTTOP: case Native.HTTOPLEFT: case Native.HTTOPRIGHT: case Native.HTBORDER: return 3;
                case Native.HTSYSMENU: return 1;
                case Native.HTMINBUTTON: case Native.HTMAXBUTTON: case Native.HTCLOSE: case Native.HTHELP: case Native.HTMENU: return 0;
                default: return 2; // client area, nowhere, transparent
            }
        }

        public static Bitmap LoadIcon(TrackedWindow w, int size)
        {
            if (w.IconTried || w.Handle == IntPtr.Zero) return w.Icon;
            w.IconTried = true;
            if (!Native.IsWindows) return null;
            IntPtr hIcon = IntPtr.Zero, res;
            foreach (int kind in new[] { Native.ICON_SMALL2, Native.ICON_SMALL, Native.ICON_BIG })
            {
                if (Native.SendMessageTimeout(w.Handle, Native.WM_GETICON, new IntPtr(kind), IntPtr.Zero, Native.SMTO_ABORTIFHUNG, 50, out res) != IntPtr.Zero && res != IntPtr.Zero) { hIcon = res; break; }
            }
            if (hIcon == IntPtr.Zero) hIcon = Native.GetClassLongPtr(w.Handle, Native.GCLP_HICONSM);
            if (hIcon == IntPtr.Zero) hIcon = Native.GetClassLongPtr(w.Handle, Native.GCLP_HICON);
            if (hIcon == IntPtr.Zero) return null;
            try
            {
                using (var ico = Icon.FromHandle(hIcon))
                using (var bmp = ico.ToBitmap())
                {
                    var scaled = new Bitmap(size, size, PixelFormat.Format32bppPArgb);
                    using (var g = Graphics.FromImage(scaled))
                    {
                        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                        g.DrawImage(bmp, new Rectangle(0, 0, size, size));
                    }
                    w.Icon = scaled;
                }
            }
            catch { w.Icon = null; }
            return w.Icon;
        }
    }

    #endregion

    #region Event intake, actuator and Undo

    /// <summary>Out-of-context WinEvent hooks: events arrive on Glimpse's own message loop,
    /// no code runs inside other processes.</summary>
    internal sealed class EventIntake : IDisposable
    {
        readonly Native.WinEventProc proc;
        readonly List<IntPtr> hooks = new List<IntPtr>();
        readonly Action<uint, IntPtr> handler;

        public EventIntake(Action<uint, IntPtr> handler)
        {
            this.handler = handler;
            proc = Callback;
            uint[,] ranges =
            {
                { Native.EVENT_SYSTEM_FOREGROUND, Native.EVENT_SYSTEM_FOREGROUND },
                { Native.EVENT_SYSTEM_MOVESIZESTART, Native.EVENT_SYSTEM_MOVESIZEEND },
                { Native.EVENT_SYSTEM_MINIMIZESTART, Native.EVENT_SYSTEM_MINIMIZEEND },
                { Native.EVENT_OBJECT_DESTROY, Native.EVENT_OBJECT_HIDE },
                { Native.EVENT_OBJECT_LOCATIONCHANGE, Native.EVENT_OBJECT_NAMECHANGE },
                { Native.EVENT_OBJECT_CLOAKED, Native.EVENT_OBJECT_UNCLOAKED }
            };
            for (int i = 0; i < ranges.GetLength(0); i++)
            {
                IntPtr h = Native.SetWinEventHook(ranges[i, 0], ranges[i, 1], IntPtr.Zero, proc, 0, 0,
                    Native.WINEVENT_OUTOFCONTEXT | Native.WINEVENT_SKIPOWNPROCESS);
                if (h == IntPtr.Zero) Log.Warn("SetWinEventHook failed for 0x" + ranges[i, 0].ToString("X4"));
                else hooks.Add(h);
            }
        }

        void Callback(IntPtr hook, uint evt, IntPtr hwnd, int idObject, int idChild, uint thread, uint time)
        {
            if (hwnd == IntPtr.Zero || idObject != Native.OBJID_WINDOW || idChild != Native.CHILDID_SELF) return;
            try { handler(evt, hwnd); }
            catch (Exception ex) { Log.Error("event 0x" + evt.ToString("X4"), ex); }
        }

        public void Dispose()
        {
            foreach (IntPtr h in hooks) Native.UnhookWinEvent(h);
            hooks.Clear();
            GC.KeepAlive(proc);
        }
    }

    internal sealed class UndoItem
    {
        public IntPtr Handle;
        public Rc Before, After;   // window rectangles
        public string Title;
    }

    internal sealed class UndoGroup
    {
        public readonly List<UndoItem> Items = new List<UndoItem>();
        public string Reason;
        public long Time;
    }

    /// <summary>Moves and resizes other windows without changing focus or stacking order.
    /// Measures with the DWM frame, moves with the window rectangle.</summary>
    internal sealed class Actuator
    {
        public const int OwnMoveQuietMs = 500;

        /// <summary>Puts the visible frame of w at frame; returns false when Windows refused (admin window).</summary>
        public bool MoveFrame(TrackedWindow w, Rc frame, UndoGroup undo, out bool accessDenied)
        {
            accessDenied = false;
            Native.RECT r;
            if (!Native.GetWindowRect(w.Handle, out r)) return false;
            Rc wr = Rc.From(r);
            Rc fr = Native.TryFrame(w.Handle, out r) ? Rc.From(r) : wr;
            int dl = fr.L - wr.L, dt = fr.T - wr.T, dr = wr.R - fr.R, db = wr.B - fr.B;
            var target = new Rc(frame.L - dl, frame.T - dt, frame.R + dr, frame.B + db);
            if (!SetRect(w, wr, target, out accessDenied)) return false;
            if (undo != null)
            {
                // one entry per window: the first "before" and the last "after"
                UndoItem item = null;
                foreach (var it in undo.Items) if (it.Handle == w.Handle) item = it;
                if (item == null) undo.Items.Add(new UndoItem { Handle = w.Handle, Before = wr, After = target, Title = w.ShortTitle });
                else item.After = target;
            }
            w.Frame = frame;
            w.WindowRect = target;
            return true;
        }

        public bool SetRect(TrackedWindow w, Rc current, Rc target, out bool accessDenied)
        {
            accessDenied = false;
            uint flags = Native.SWP_NOZORDER | Native.SWP_NOACTIVATE | Native.SWP_NOOWNERZORDER | Native.SWP_ASYNCWINDOWPOS;
            if (target.W == current.W && target.H == current.H) flags |= Native.SWP_NOSIZE;
            if (target.L == current.L && target.T == current.T) flags |= Native.SWP_NOMOVE;
            if (w != null) w.SuppressUntil = Clock.Ms + OwnMoveQuietMs;
            if (Native.SetWindowPos(w == null ? IntPtr.Zero : w.Handle, IntPtr.Zero, target.L, target.T, target.W, target.H, flags)) return true;
            int err = Marshal.GetLastWin32Error();
            accessDenied = err == 5;
            Log.Warn("SetWindowPos failed (" + err + ") for " + (w == null ? "?" : w.Process));
            return false;
        }

        public int UndoGroup(UndoGroup g, WindowRegistry reg)
        {
            int n = 0;
            foreach (var it in g.Items)
            {
                if (!Native.IsWindow(it.Handle)) continue;
                var w = reg.Get(it.Handle);
                Native.RECT r;
                if (!Native.GetWindowRect(it.Handle, out r)) continue;
                bool denied;
                if (w != null) w.Placed = true;
                var tw = w ?? new TrackedWindow { Handle = it.Handle };
                if (SetRect(tw, Rc.From(r), it.Before, out denied)) n++;
            }
            return n;
        }
    }

    internal sealed class UndoStack
    {
        readonly List<UndoGroup> groups = new List<UndoGroup>();
        public UndoGroup Last { get { return groups.Count == 0 ? null : groups[groups.Count - 1]; } }
        public void Push(UndoGroup g)
        {
            if (g == null || g.Items.Count == 0) return;
            g.Time = Clock.Ms;
            groups.Add(g);
            if (groups.Count > 20) groups.RemoveAt(0);
        }
        public bool Remove(UndoGroup g) { return groups.Remove(g); }
        public bool Contains(UndoGroup g) { return groups.Contains(g); }
    }

    /// <summary>A move or resize the user is doing right now.</summary>
    internal sealed class DragState
    {
        public IntPtr Handle;
        public TrackedWindow W;
        public Rc StartFrame;
        public Native.POINT StartCursor;
        public bool FullDrag;
        public int Hit = Native.HTCAPTION;   // what was grabbed: caption or an edge
        public Rc Current;
        public bool ShiftAtEnd;
        public bool LastShift;
        public bool Computed;
        public bool ByButton;   // detected from the mouse button (no MOVESIZESTART from the app)
        public List<Guide> Guides = new List<Guide>();
        public bool Resizing { get { return Hit != Native.HTCAPTION; } }

        public string Edges()
        {
            switch (Hit)
            {
                case Native.HTLEFT: return "L";
                case Native.HTRIGHT: return "R";
                case Native.HTTOP: return "T";
                case Native.HTBOTTOM: return "B";
                case Native.HTTOPLEFT: return "TL";
                case Native.HTTOPRIGHT: return "TR";
                case Native.HTBOTTOMLEFT: return "BL";
                case Native.HTBOTTOMRIGHT: return "BR";
                default: return "";
            }
        }

        /// <summary>Outline drag (common in RDP): the rectangle follows the cursor and the grabbed edge.</summary>
        public Rc Predict(Native.POINT c)
        {
            int dx = c.X - StartCursor.X, dy = c.Y - StartCursor.Y;
            Rc f = StartFrame;
            if (!Resizing) return f.Offset(dx, dy);
            string e = Edges();
            int min = Util.Scale(80, W == null ? 96 : W.Dpi);
            if (e.IndexOf('L') >= 0) f.L = Math.Min(f.L + dx, f.R - min);
            if (e.IndexOf('R') >= 0) f.R = Math.Max(f.R + dx, f.L + min);
            if (e.IndexOf('T') >= 0) f.T = Math.Min(f.T + dy, f.B - min);
            if (e.IndexOf('B') >= 0) f.B = Math.Max(f.B + dy, f.T + min);
            return f;
        }

        public static string ChangedEdges(Rc a, Rc b)
        {
            string s = "";
            if (a.L != b.L) s += "L";
            if (a.T != b.T) s += "T";
            if (a.R != b.R) s += "R";
            if (a.B != b.B) s += "B";
            return s;
        }
    }

    #endregion

    #region Hotkey, hot corner, Start with Windows, size and layout memory, first-click mode

    internal sealed class HotkeyManager
    {
        public const int XRayId = 1, TestId = 2;
        readonly IntPtr owner;
        public HotkeySpec Current;
        public bool Registered;
        public string Problem;      // why the X-ray hotkey is not active
        public Action TestArrived;  // set while "Press it now" waits

        public HotkeyManager(IntPtr owner) { this.owner = owner; }

        public void Apply(string text)
        {
            Unregister();
            Problem = null;
            if (string.IsNullOrEmpty(text)) return;
            HotkeySpec spec;
            if (!HotkeySpec.TryParse(text, out spec)) { Problem = text; return; }
            Current = spec;
            if (!Native.IsWindows) return;
            Registered = Native.RegisterHotKey(owner, XRayId, spec.Mods | Native.MOD_NOREPEAT, spec.Vk);
            if (!Registered)
            {
                Problem = spec.ToString();
                Log.Warn("hotkey " + spec + " is taken (error " + Marshal.GetLastWin32Error() + ")");
            }
        }

        public void Unregister()
        {
            if (Registered && Native.IsWindows) Native.UnregisterHotKey(owner, XRayId);
            Registered = false;
        }

        /// <summary>Check 1: Windows accepts the registration (no other program in the session holds it).</summary>
        public bool CanRegister(HotkeySpec spec)
        {
            if (!Native.IsWindows) return true;
            if (Registered && spec.Mods == Current.Mods && spec.Vk == Current.Vk) return true;
            bool ok = Native.RegisterHotKey(owner, TestId, spec.Mods | Native.MOD_NOREPEAT, spec.Vk);
            if (ok) Native.UnregisterHotKey(owner, TestId);
            return ok;
        }
    }

    /// <summary>X-ray opens when the cursor rests 0.4 s in the chosen outer screen corner.</summary>
    internal sealed class HotCorner : IDisposable
    {
        readonly WinTimer timer = new WinTimer { Interval = 100 };
        readonly Action fire;
        public Corner Which = Corner.Off;
        long since = -1;
        bool fired;

        public HotCorner(Action fire)
        {
            this.fire = fire;
            timer.Tick += delegate { Tick(); };
        }

        public void Set(Corner c)
        {
            Which = c;
            since = -1;
            fired = false;
            timer.Enabled = c != Corner.Off && Native.IsWindows;
        }

        public void Suspend(bool off) { timer.Enabled = !off && Which != Corner.Off && Native.IsWindows; }

        void Tick()
        {
            Native.POINT p;
            if (!Native.GetCursorPos(out p)) return;
            bool inCorner = InCorner(p) && !Native.KeyDown(Native.VK_LBUTTON) && !Native.KeyDown(Native.VK_RBUTTON);
            if (!inCorner) { since = -1; fired = false; return; }
            if (since < 0) since = Clock.Ms;
            if (!fired && Clock.Ms - since >= 400)
            {
                fired = true;
                fire();
            }
        }

        bool InCorner(Native.POINT p)
        {
            IntPtr hmon = Native.MonitorFromPoint(p, Native.MONITOR_DEFAULTTONULL);
            if (hmon == IntPtr.Zero) return false;
            var mi = new Native.MONITORINFOEX();
            mi.cbSize = Marshal.SizeOf(typeof(Native.MONITORINFOEX));
            if (!Native.GetMonitorInfo(hmon, ref mi)) return false;
            Rc b = Rc.From(mi.rcMonitor);
            int tol = 2;
            bool left = p.X <= b.L + tol, right = p.X >= b.R - 1 - tol, top = p.Y <= b.T + tol, bottom = p.Y >= b.B - 1 - tol;
            int ox, oy;
            switch (Which)
            {
                case Corner.TopLeft: if (!(left && top)) return false; ox = b.L - 1; oy = b.T - 1; break;
                case Corner.TopRight: if (!(right && top)) return false; ox = b.R; oy = b.T - 1; break;
                case Corner.BottomLeft: if (!(left && bottom)) return false; ox = b.L - 1; oy = b.B; break;
                case Corner.BottomRight: if (!(right && bottom)) return false; ox = b.R; oy = b.B; break;
                default: return false;
            }
            // only an outer corner of the desktop, where the cursor stops
            if (Native.MonitorFromPoint(new Native.POINT(ox, p.Y), Native.MONITOR_DEFAULTTONULL) != IntPtr.Zero) return false;
            if (Native.MonitorFromPoint(new Native.POINT(p.X, oy), Native.MONITOR_DEFAULTTONULL) != IntPtr.Zero) return false;
            return true;
        }

        public void Dispose() { timer.Dispose(); }
    }

    /// <summary>HKCU Run entry: per user, no admin rights. Keeps /ini when used.</summary>
    internal static class StartWithWindows
    {
        const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string ValueName = "Glimpse";

        public static void Sync(bool enabled)
        {
            if (!Native.IsWindows) return;
            try
            {
                using (var k = Registry.CurrentUser.CreateSubKey(RunKey))
                {
                    if (k == null) return;
                    string want = Program.StartCommandLine();
                    string have = k.GetValue(ValueName, null, RegistryValueOptions.DoNotExpandEnvironmentNames) as string;
                    if (enabled)
                    {
                        if (have != want)
                        {
                            k.SetValue(ValueName, want, want.Contains("%") ? RegistryValueKind.ExpandString : RegistryValueKind.String);
                            Log.Info("Start with Windows: " + want);
                        }
                    }
                    else if (have != null && have.IndexOf(Program.LaunchPath, StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        k.DeleteValue(ValueName, false);
                        Log.Info("Start with Windows removed");
                    }
                }
            }
            catch (Exception ex) { Log.Warn("Start with Windows: " + ex.Message); }
        }
    }

    /// <summary>Size memory: the size you last gave a kind of window (program, class, title shape)
    /// goes to the next window of that kind. Stored in %LOCALAPPDATA%\Glimpse\Memory.ini.</summary>
    internal sealed class SizeMemory
    {
        readonly string path = Path.Combine(Log.Folder, "Memory.ini");
        IniFile ini;
        bool dirty;
        const int MaxEntries = 300;

        public void Load()
        {
            try { ini = File.Exists(path) ? IniFile.Parse(File.ReadAllBytes(path)) : IniFile.FromText("; Glimpse size memory\r\n[Sizes]\r\n"); }
            catch { ini = IniFile.FromText("[Sizes]\r\n"); }
        }

        public static string KindOf(TrackedWindow w)
        {
            return (w.Process + "|" + w.Class + "|" + Util.TitleShape(w.Title)).ToLowerInvariant();
        }

        static string Key(string kind)
        {
            using (var sha = System.Security.Cryptography.SHA1.Create())
            {
                byte[] h = sha.ComputeHash(Encoding.UTF8.GetBytes(kind));
                var sb = new StringBuilder();
                for (int i = 0; i < 8; i++) sb.Append(h[i].ToString("x2"));
                return sb.ToString();
            }
        }

        public void Remember(TrackedWindow w, Rc frame)
        {
            if (ini == null) Load();
            int lw = (int)Math.Round(frame.W * 96.0 / w.Dpi), lh = (int)Math.Round(frame.H * 96.0 / w.Dpi);
            string key = Key(KindOf(w));
            ini.RemoveKey("Sizes", key); // re-insert last: the file is in least-recently-used order
            ini.Set("Sizes", key, Util.Inv(lw) + "," + Util.Inv(lh) + ";" + KindOf(w).Replace("\r", " ").Replace("\n", " "));
            var entries = ini.Entries("Sizes");
            for (int i = 0; i < entries.Count - MaxEntries; i++) ini.RemoveKey("Sizes", entries[i].Key);
            dirty = true;
        }

        public bool TryGet(TrackedWindow w, out int width, out int height)
        {
            width = height = 0;
            if (ini == null) Load();
            string v = ini.Get("Sizes", Key(KindOf(w)));
            if (v == null) return false;
            int semi = v.IndexOf(';');
            string[] p = (semi >= 0 ? v.Substring(0, semi) : v).Split(',');
            if (p.Length != 2) return false;
            int lw = Util.ParseInt(p[0], 0, 0, 20000), lh = Util.ParseInt(p[1], 0, 0, 20000);
            if (lw <= 0 || lh <= 0) return false;
            width = Util.Scale(lw, w.Dpi);
            height = Util.Scale(lh, w.Dpi);
            return true;
        }

        public void SaveIfDirty()
        {
            if (!dirty || ini == null) return;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                File.WriteAllBytes(path, ini.ToBytes());
                dirty = false;
            }
            catch (Exception ex) { Log.Warn("cannot write " + path + ": " + ex.Message); }
        }
    }

    /// <summary>Layout memory: window positions per monitor layout, for this session, so a display
    /// setup that returns (an RDP reconnect from the office) can be restored.</summary>
    internal sealed class LayoutMemory
    {
        readonly Dictionary<string, Dictionary<IntPtr, Rc>> bySignature = new Dictionary<string, Dictionary<IntPtr, Rc>>();

        public void Save(string signature, IEnumerable<TrackedWindow> windows)
        {
            var d = new Dictionary<IntPtr, Rc>();
            foreach (var w in windows) if (w.Counted && !w.Maximized && !w.FullScreen) d[w.Handle] = w.WindowRect;
            if (d.Count > 0) bySignature[signature] = d;
        }

        public Dictionary<IntPtr, Rc> Get(string signature)
        {
            Dictionary<IntPtr, Rc> d;
            return bySignature.TryGetValue(signature, out d) ? d : null;
        }
    }

    /// <summary>Experimental "first click only activates": a click on a background window's free glimpse
    /// brings that window forward and does not reach its content. Off by default (uses a mouse hook).</summary>
    internal sealed class FirstClickHook : IDisposable
    {
        readonly Native.LowLevelProc proc;
        IntPtr hook;
        readonly Func<int, int, IntPtr> zoneOwnerAt;  // window whose glimpse is at a point, or zero
        readonly Action<IntPtr> activate;
        bool swallowUp;

        public FirstClickHook(Func<int, int, IntPtr> zoneOwnerAt, Action<IntPtr> activate)
        {
            this.zoneOwnerAt = zoneOwnerAt;
            this.activate = activate;
            proc = Callback;
            hook = Native.SetWindowsHookEx(Native.WH_MOUSE_LL, proc, Native.GetModuleHandle(null), 0);
            if (hook == IntPtr.Zero) Log.Warn("first-click mode: mouse hook not available");
        }

        IntPtr Callback(int code, IntPtr wParam, IntPtr lParam)
        {
            try
            {
                if (code >= 0)
                {
                    int msg = wParam.ToInt32();
                    if (msg == Native.WM_LBUTTONDOWN)
                    {
                        var info = (Native.MSLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(Native.MSLLHOOKSTRUCT));
                        IntPtr target = zoneOwnerAt(info.pt.X, info.pt.Y);
                        if (target != IntPtr.Zero && target != Native.GetForegroundWindow())
                        {
                            swallowUp = true;
                            activate(target);
                            return new IntPtr(1);
                        }
                    }
                    else if (msg == Native.WM_LBUTTONUP && swallowUp)
                    {
                        swallowUp = false;
                        return new IntPtr(1);
                    }
                }
            }
            catch { }
            return Native.CallNextHookEx(hook, code, wParam, lParam);
        }

        public void Dispose()
        {
            if (hook != IntPtr.Zero) { Native.UnhookWindowsHookEx(hook); hook = IntPtr.Zero; }
            GC.KeepAlive(proc);
        }
    }

    /// <summary>Hidden top-level window (not message-only: broadcasts such as WM_DISPLAYCHANGE
    /// reach top-level windows only). Receives hotkeys, session changes and commands.</summary>
    internal sealed class MessageWindow : NativeWindow, IDisposable
    {
        readonly Func<Message, bool> handler;

        public MessageWindow(Func<Message, bool> handler)
        {
            this.handler = handler;
            var cp = new CreateParams { Caption = Ipc.WindowTitle, Style = unchecked((int)0x80000000), ExStyle = (int)Native.WS_EX_TOOLWINDOW };
            CreateHandle(cp);
        }

        protected override void WndProc(ref Message m)
        {
            bool handled = false;
            try { handled = handler(m); }
            catch (Exception ex) { Log.Error("message 0x" + m.Msg.ToString("X4"), ex); }
            if (!handled) base.WndProc(ref m);
        }

        public void Dispose() { DestroyHandle(); }
    }

    #endregion

    #region Coordinator: one Glimpse process per session

    internal sealed class GlimpseApp : ApplicationContext
    {
        public static GlimpseApp Current;
        public readonly SettingsStore Store = new SettingsStore();
        public Settings S { get { return Store.Current; } }
        public WindowRegistry Reg;
        public readonly Actuator Act = new Actuator();
        public readonly UndoStack Undo = new UndoStack();
        public List<MonitorInfo> Mons = new List<MonitorInfo>();
        public string LayoutSig = "";
        public MessageWindow Msg;
        public HotkeyManager Hotkeys;
        public OverlayManager Overlays;
        public XRayView XRay;
        public TrayUi Tray;
        public SettingsForm SettingsWindow;
        public readonly SizeMemory Sizes = new SizeMemory();
        readonly LayoutMemory layouts = new LayoutMemory();
        EventIntake intake;
        HotCorner corner;
        FirstClickHook firstClick;
        public DragState Drag;
        DragState dropping;
        bool lastSceneThreatened;
        readonly WinTimer debounce = new WinTimer { Interval = 150 };
        readonly WinTimer settle = new WinTimer { Interval = 1500 };
        readonly WinTimer safety = new WinTimer { Interval = 10000 };
        readonly WinTimer readback = new WinTimer { Interval = 250 };
        readonly WinTimer dragTimer = new WinTimer { Interval = 33 };
        readonly WinTimer dropTimer = new WinTimer { Interval = 120 };
        readonly Dictionary<IntPtr, int> pending = new Dictionary<IntPtr, int>();
        readonly List<long> recentMoves = new List<long>();
        readonly List<UndoItem> toVerify = new List<UndoItem>();
        public DateTime PausedUntil = DateTime.MinValue;
        public bool SessionActive = true;
        long suspendedUntil;
        public int HiddenCount, GuardedCount, OverLimit;
        bool disposed;

        const int PShown = 1, PRestored = 2, PMoved = 4, PTitle = 8, PGone = 16, PMinimized = 32, PCloak = 64;

        public GlimpseApp()
        {
            Current = this;
            if (!(SynchronizationContext.Current is WindowsFormsSynchronizationContext))
                SynchronizationContext.SetSynchronizationContext(new WindowsFormsSynchronizationContext());
            Store.Load(Program.Opts.IniArgument);
            Log.SetDebug(S.DebugLog);
            Log.Info("Glimpse " + Program.Version + (Program.IsScript ? " (PowerShell)" : "") + ", " + (IntPtr.Size * 8) + "-bit, settings "
                + Store.FilePath + (Store.Writable ? "" : " (read-only)") + (Store.UsingDefaults ? " (built-in defaults)" : ""));
            Texts.Load(S.Language);
            Reg = new WindowRegistry();
            Msg = new MessageWindow(HandleMessage);
            Mons = Monitors.Read();
            LayoutSig = Monitors.Signature(Mons);
            Overlays = new OverlayManager(this);
            XRay = new XRayView(this);
            Hotkeys = new HotkeyManager(Msg.Handle);
            Hotkeys.Apply(S.Hotkey);
            corner = new HotCorner(delegate { ShowXRay(false); });
            corner.Set(S.HotCorner);
            Tray = new TrayUi(this);
            Sizes.Load();
            if (Native.IsWindows)
            {
                try { Native.WTSRegisterSessionNotification(Msg.Handle, Native.NOTIFY_FOR_THIS_SESSION); }
                catch (Exception ex) { Log.Warn("session notifications: " + ex.Message); }
            }
            Reg.Refresh(S, Mons);
            foreach (var w in Reg.All.Values) { w.Placed = true; w.SettledFrame = w.Frame; }
            intake = new EventIntake(OnWinEvent);
            debounce.Tick += delegate { ProcessPending(); };
            settle.Tick += delegate { OnDisplaySettled(); };
            safety.Tick += delegate { OnSafetyTick(); };
            readback.Tick += delegate { OnReadback(); };
            dragTimer.Tick += delegate { OnDragTick(); };
            dropTimer.Tick += delegate { OnDropSettled(); };
            safety.Start();
            Store.Changed += delegate { ApplySettings(true); };
            Store.StartWatching();
            StartWithWindows.Sync(S.StartWithWindows);
            ApplyFirstClick();
            UpdateStatus();
            layouts.Save(LayoutSig, Reg.All.Values);
            Log.Info(Mons.Count + " monitor(s), " + GuardedCount + " window(s) guarded, mode " + S.Mode + ", hotkey "
                + (Hotkeys.Registered ? Hotkeys.Current.ToString() : "none") + ", hot corner " + S.HotCorner);
            if (Program.Opts.ShowSettings) ShowSettings();
        }

        // ------------------------------------------------------------------ state

        public bool Paused { get { return DateTime.Now < PausedUntil; } }
        public bool Suspended { get { return Clock.Ms < suspendedUntil; } }
        public bool AutoFixAllowed { get { return SessionActive && !Paused && !Suspended && S.Mode != FixMode.Guide; } }

        public void Pause(int minutes, bool untilTomorrow)
        {
            PausedUntil = untilTomorrow ? DateTime.Today.AddDays(1) : DateTime.Now.AddMinutes(minutes);
            Log.Info("paused until " + PausedUntil.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture));
            Overlays.HideDrag();
            Tray.Update();
        }

        public void Resume()
        {
            PausedUntil = DateTime.MinValue;
            suspendedUntil = 0;
            recentMoves.Clear();
            Log.Info("resumed");
            Tray.Update();
        }

        // ------------------------------------------------------------------ messages

        bool HandleMessage(Message m)
        {
            switch (m.Msg)
            {
                case Native.WM_DISPLAYCHANGE:
                    DisplaySignal("display change");
                    return false;
                case Native.WM_SETTINGCHANGE:
                    DisplaySignal(m.WParam.ToInt64() == Native.SPI_SETWORKAREA ? "work area" : "setting");
                    Theme.Refresh();
                    return false;
                case Native.WM_WTSSESSION_CHANGE:
                    OnSession(m.WParam.ToInt32());
                    return true;
                case Native.WM_HOTKEY:
                    if (m.WParam.ToInt32() == HotkeyManager.XRayId) OnHotkey();
                    return true;
            }
            if (Ipc.Message != 0 && m.Msg == (int)Ipc.Message)
            {
                int cmd = m.WParam.ToInt32();
                if (cmd == Ipc.CmdSettings) ShowSettings();
                else if (cmd == Ipc.CmdExit) ExitApp();
                else if (cmd == Ipc.CmdXRay) ShowXRay(true);
                return true;
            }
            return false;
        }

        void OnHotkey()
        {
            var test = Hotkeys.TestArrived;
            if (test != null) { Hotkeys.TestArrived = null; test(); return; }
            if (XRay.IsOpen) XRay.Close(); else ShowXRay(true);
        }

        void OnSession(int code)
        {
            switch (code)
            {
                case Native.WTS_SESSION_LOCK:
                case Native.WTS_REMOTE_DISCONNECT:
                case Native.WTS_CONSOLE_DISCONNECT:
                    if (!SessionActive) return;
                    SessionActive = false;
                    Log.Info("session " + (code == Native.WTS_SESSION_LOCK ? "locked" : "disconnected") + ": idle");
                    CancelDrag();
                    XRay.Close();
                    Overlays.HideAll();
                    corner.Suspend(true);
                    Tray.Update();
                    break;
                case Native.WTS_SESSION_UNLOCK:
                case Native.WTS_REMOTE_CONNECT:
                case Native.WTS_CONSOLE_CONNECT:
                    if (SessionActive && code == Native.WTS_SESSION_UNLOCK) return;
                    SessionActive = true;
                    Log.Info("session " + (code == Native.WTS_SESSION_UNLOCK ? "unlocked" : "connected"));
                    corner.Suspend(false);
                    DisplaySignal("session");
                    Tray.Update();
                    break;
            }
        }

        // ------------------------------------------------------------------ window events

        /// <summary>GLIMPSE_TRACE=1 logs every window event (diagnostics).</summary>
        static readonly bool Trace = Environment.GetEnvironmentVariable("GLIMPSE_TRACE") == "1";

        void OnWinEvent(uint evt, IntPtr h)
        {
            if (Trace)
            {
                var tw = Reg.Get(h);
                Log.Info("event 0x" + evt.ToString("X4") + " " + h.ToInt64().ToString("X") + (tw == null ? "" : " " + tw.Process)
                    + " lbutton=" + Native.KeyDown(Native.VK_LBUTTON) + " fg=" + (Native.GetForegroundWindow() == h));
            }
            if (disposed || !SessionActive) return;
            switch (evt)
            {
                case Native.EVENT_SYSTEM_FOREGROUND:
                    Reg.Touch(h);
                    return;
                case Native.EVENT_SYSTEM_MOVESIZESTART:
                    BeginDrag(h, false);
                    return;
                case Native.EVENT_SYSTEM_MOVESIZEEND:
                    EndDrag(h);
                    return;
                case Native.EVENT_OBJECT_LOCATIONCHANGE:
                    {
                        var w = Reg.Get(h);
                        if (w == null) return;
                        if (Drag != null && Drag.Handle == h) return; // the drag timer reads it
                        if (Clock.Ms < w.SuppressUntil) return;       // our own move
                        // apps with their own drag code (no system move loop, no MOVESIZESTART):
                        // the foreground window moving while the left button is down is a user drag
                        if (Drag == null && dropping == null && !w.Maximized && Native.KeyDown(Native.VK_LBUTTON) && Native.GetForegroundWindow() == h)
                        {
                            BeginDrag(h, true);
                            return;
                        }
                        AddPending(h, PMoved);
                        return;
                    }
                case Native.EVENT_OBJECT_SHOW:
                    if (Native.GetAncestor(h, Native.GA_ROOT) == h) AddPending(h, PShown);
                    return;
                case Native.EVENT_OBJECT_UNCLOAKED:
                case Native.EVENT_OBJECT_CLOAKED:
                    if (Native.GetAncestor(h, Native.GA_ROOT) == h) AddPending(h, PCloak);
                    return;
                case Native.EVENT_SYSTEM_MINIMIZEEND:
                    AddPending(h, PRestored);
                    return;
                case Native.EVENT_SYSTEM_MINIMIZESTART:
                    if (Reg.Get(h) != null) AddPending(h, PMinimized);
                    return;
                case Native.EVENT_OBJECT_HIDE:
                case Native.EVENT_OBJECT_DESTROY:
                    if (Reg.Get(h) != null) AddPending(h, PGone);
                    return;
                case Native.EVENT_OBJECT_NAMECHANGE:
                    if (Reg.Get(h) != null) AddPending(h, PTitle);
                    return;
            }
        }

        void AddPending(IntPtr h, int flag)
        {
            int f;
            pending.TryGetValue(h, out f);
            pending[h] = f | flag;
            debounce.Stop();
            debounce.Start();
        }

        void RefreshRegistry()
        {
            Reg.Refresh(S, Mons);
        }

        /// <summary>Handles the merged burst of events (about 150 ms after the last one).</summary>
        void ProcessPending()
        {
            debounce.Stop();
            if (disposed || !SessionActive || Drag != null || dropping != null) return;
            if (pending.Count == 0) return;
            var items = new List<KeyValuePair<IntPtr, int>>(pending);
            pending.Clear();
            var knownBefore = new HashSet<IntPtr>(Reg.All.Keys);
            RefreshRegistry();
            foreach (var kv in items)
            {
                var w = Reg.Get(kv.Key);
                if (w == null) continue;
                int f = kv.Value;
                if (!knownBefore.Contains(kv.Key))
                {
                    // shown now: a new window (or one shown again); uncloaked ones only switched desktops
                    w.Placed = (f & PShown) == 0;
                    w.SettledFrame = w.Frame;
                }
                if (!w.Counted) { w.SettledFrame = w.Frame; continue; }
                try
                {
                    if (!w.Placed)
                    {
                        w.Placed = true;
                        NewWindow(w);
                    }
                    else if ((f & PRestored) != 0 || (w.WasMaximized && !w.Maximized && (f & PMoved) != 0))
                    {
                        RunAndApply(Cause.Restore, w, "", default(Rc), null, null);
                    }
                    else if ((f & PMoved) != 0 && !w.Frame.Same(w.SettledFrame) && !w.Maximized && !w.FullScreen)
                    {
                        if (!DetectRevert(w))
                        {
                            bool resized = w.Frame.W != w.SettledFrame.W || w.Frame.H != w.SettledFrame.H;
                            RunAndApply(resized ? Cause.AppResize : Cause.AppMove, w, "", default(Rc), null, null);
                        }
                    }
                    else if ((f & PTitle) != 0)
                    {
                        var rule = S.RuleFor(w.Title);
                        string key = rule == null ? "" : rule.Pattern;
                        if (w.RuleKey != null && key != w.RuleKey) RunAndApply(Cause.AppResize, w, "", default(Rc), null, null);
                        w.RuleKey = key;
                    }
                }
                catch (Exception ex) { Log.Error("processing " + w.Process, ex); }
                w.SettledFrame = w.Frame;
                if (w.RuleKey == null) { var r = S.RuleFor(w.Title); w.RuleKey = r == null ? "" : r.Pattern; }
            }
            UpdateStatus();
        }

        /// <summary>A window that moves back twice within 10 seconds after a fix is stubborn for the session.</summary>
        bool DetectRevert(TrackedWindow w)
        {
            if (w.FixedAt == 0 || Clock.Ms - w.FixedAt > 10000) return false;
            if (!w.Frame.Near(w.FixedFrom, 4)) return false;
            return CountRevert(w, "moved back");
        }

        bool CountRevert(TrackedWindow w, string what)
        {
            long now = Clock.Ms;
            w.Reverts.RemoveAll(delegate(long t) { return now - t > 10000; });
            w.Reverts.Add(now);
            if (w.Reverts.Count >= 2 && !w.Stubborn)
            {
                w.Stubborn = true;
                Log.Info(w.Process + " " + what + " twice within 10 s: left alone for this session");
            }
            return true;
        }

        void NewWindow(TrackedWindow w)
        {
            Rc start = w.Frame;
            Rc? sized = null;
            int sw, sh;
            if (S.RememberSizes && w.Resizable && !w.Maximized && Sizes.TryGet(w, out sw, out sh) && (Math.Abs(sw - start.W) > 2 || Math.Abs(sh - start.H) > 2))
            {
                var mon = Monitors.At(Mons, start);
                Rc r = Rc.XYWH(start.L, start.T, Math.Min(sw, mon.Work.W), Math.Min(sh, mon.Work.H));
                sized = Geometry.ClampInto(r, mon.Work);
            }
            RunAndApply(Cause.New, w, "", default(Rc), sized, null);
        }

        // ------------------------------------------------------------------ resolver glue

        /// <summary>Builds the geometry model from the registry. The 30 (MaxWindows) most recently used
        /// windows count; the rest stay reachable through X-ray.</summary>
        Layout BuildLayout(out List<TrackedWindow> list, TrackedWindow target, Rc? targetFrame)
        {
            var L = new Layout { GlimpseSize = S.GlimpseSize };
            foreach (var m in Mons) L.Monitors.Add(new GeoMonitor(m.Bounds, m.Work, m.Dpi));
            list = Reg.CountedByMru();
            if (target != null && !list.Contains(target) && target.Counted) list.Insert(0, target);
            int k = 0;
            foreach (var w in list)
            {
                Rc frame = w == target && targetFrame.HasValue ? targetFrame.Value : w.Frame;
                var g = new GeoWindow
                {
                    Id = w.Handle.ToInt64().ToString("X", CultureInfo.InvariantCulture), Frame = frame, Dpi = w.Dpi, Tag = w,
                    Resizable = w.Resizable && !w.Maximized, CaptionW = w.CaptionW, CaptionH = w.CaptionH, IconW = w.IconW,
                    TopMap = w.TopMap, PrevWork = w.PrevWork
                };
                int P = Geometry.GlimpsePx(S.GlimpseSize, w.Dpi);
                if (w.Maximized || w.FullScreen) { g.Subject = g.Blocker = g.Movable = false; g.FixedReason = "maximized"; }
                else if (frame.W < 2 * P || frame.H < 2 * P) { g.Subject = g.Blocker = g.Movable = false; g.FixedReason = "small"; }
                else if (++k > S.MaxWindows && w != target) { g.Subject = g.Blocker = g.Movable = false; g.FixedReason = "limit"; }
                else if (w.Topmost) { g.Subject = false; g.Movable = false; g.FixedReason = "topmost"; }
                else if (w.Elevated) { g.Movable = false; g.FixedReason = "elevated"; }
                else if (w.Hung) { g.Movable = false; g.FixedReason = "hung"; }
                else if (w.Stubborn) { g.Movable = false; g.FixedReason = "stubborn"; }
                else if (w.Excluded) { g.Movable = false; g.FixedReason = "excluded"; }
                var rule = w.Resizable ? S.RuleFor(w.Title) : null;
                if (rule != null) { g.MinW = Util.Scale(rule.MinWidth, w.Dpi); g.MinH = Util.Scale(rule.MinHeight, w.Dpi); }
                L.Windows.Add(g);
            }
            L.Begin();
            return L;
        }

        Outcome RunAndApply(Cause cause, TrackedWindow target, string edges, Rc anchorStart, Rc? targetFrame, UndoGroup group)
        {
            List<TrackedWindow> list;
            var L = BuildLayout(out list, target, targetFrame);
            int ti = target == null ? -1 : list.IndexOf(target);
            var q = Request.For(cause, S.Mode, ti, S.PlaceNewWindows);
            q.DraggedEdges = edges ?? "";
            q.AnchorStart = anchorStart;
            if (!(SessionActive && !Paused && !Suspended)) q.Fix = false;
            var o = Resolver.Resolve(L, q);
            if (!q.Fix) return o;
            var g = group ?? new UndoGroup { Reason = cause.ToString() };
            Apply(list, o, cause, g);
            if (group == null) Undo.Push(g);
            lastMoved = g.Items.Count;
            lastGroup = g;
            return o;
        }

        int lastMoved;
        UndoGroup lastGroup;

        void Apply(List<TrackedWindow> list, Outcome o, Cause cause, UndoGroup g)
        {
            var flashes = new List<Rc>();
            for (int i = 0; i < list.Count; i++)
            {
                var w = list[i];
                Rc to = o.Frames[i];
                if (to.Same(w.Frame)) continue;
                if (!RateOk()) { SuspendAutoFix(); break; }
                Rc from = w.Frame;
                bool denied;
                if (Act.MoveFrame(w, to, g, out denied))
                {
                    recentMoves.Add(Clock.Ms);
                    w.FixedFrom = from;
                    w.FixedTo = to;
                    w.FixedAt = Clock.Ms;
                    w.SettledFrame = to;
                    flashes.Add(to);
                    Log.Info(cause + ": " + w.Process + (Log.DebugEnabled ? " \"" + w.Title + "\"" : "") + " " + from + " -> " + to);
                }
                else if (denied)
                {
                    w.Elevated = true;
                    Log.Info("cannot move " + w.Process + ": treated as an admin window");
                }
            }
            if (flashes.Count > 0)
            {
                Overlays.Flash(flashes);
                foreach (var it in g.Items) toVerify.Add(it);
                readback.Stop();
                readback.Start();
            }
        }

        bool RateOk()
        {
            long now = Clock.Ms;
            recentMoves.RemoveAll(delegate(long t) { return now - t > 10000; });
            return recentMoves.Count < 10;
        }

        void SuspendAutoFix()
        {
            if (Suspended) return;
            suspendedUntil = Clock.Ms + 5 * 60 * 1000;
            Log.Warn("more than 10 moves within 10 seconds: automatic fixes paused for 5 minutes");
            Tray.Update();
        }

        /// <summary>Reads moved windows back: an app that refuses twice within 10 s is stubborn.</summary>
        void OnReadback()
        {
            readback.Stop();
            foreach (var it in toVerify)
            {
                var w = Reg.Get(it.Handle);
                if (w == null || !Native.IsWindow(it.Handle)) continue;
                Native.RECT r;
                if (!Native.GetWindowRect(it.Handle, out r)) continue;
                Rc now = Rc.From(r);
                if (!now.Near(it.After, 3))
                {
                    Log.Debug(w.Process + " did not take the new size or position");
                    CountRevert(w, "refused a fix");
                }
                if (Native.TryFrame(it.Handle, out r)) { w.Frame = Rc.From(r); w.SettledFrame = w.Frame; }
                w.WindowRect = now;
            }
            toVerify.Clear();
        }

        public void UpdateStatus()
        {
            if (disposed) return;
            List<TrackedWindow> list;
            var L = BuildLayout(out list, null, null);
            var o = Resolver.Resolve(L, new Request { Fix = false });
            int hidden = 0, guarded = 0, over = 0;
            for (int i = 0; i < list.Count; i++)
            {
                var w = list[i];
                var g = L.Windows[i];
                w.Zone = o.Zones[i];
                if (g.FixedReason == "limit") over++;
                if (g.FixedReason == "maximized") guarded++;
                if (!g.Subject) { w.Hidden = false; continue; }
                guarded++;
                w.Hidden = o.Zones[i] == null;
                w.HiddenReason = o.Reasons[i];
                if (w.Hidden) hidden++;
            }
            HiddenCount = hidden;
            GuardedCount = guarded;
            OverLimit = over;
            Tray.Update();
        }

        // ------------------------------------------------------------------ drag and drop

        void BeginDrag(IntPtr h, bool byButton)
        {
            CancelDrag();
            var known = Reg.Get(h);
            Rc settled = known == null ? Rc.Empty : known.SettledFrame;
            RefreshRegistry();
            var w = Reg.Get(h);
            if (w == null || !w.Counted || w.Maximized) return;
            Native.POINT p;
            Native.GetCursorPos(out p);
            Rc start = byButton && !settled.IsEmpty ? settled : w.Frame;
            Drag = new DragState
            {
                Handle = h, W = w, StartFrame = start, StartCursor = p, FullDrag = byButton || Native.DragFullWindows(),
                Hit = byButton ? GuessEdge(start, w.Frame) : HitAt(h, p), Current = w.Frame, ByButton = byButton
            };
            lastSceneThreatened = false;
            Log.Debug("drag start " + w.Process + " " + w.Frame + (Drag.FullDrag ? " (full window)" : " (outline)") + " hit " + Drag.Hit);
            if (!Paused && SessionActive) dragTimer.Start();
        }

        static int HitAt(IntPtr h, Native.POINT p)
        {
            var pt = p;
            try { Native.PhysicalToLogicalPointForPerMonitorDPI(h, ref pt); } catch { }
            IntPtr res;
            IntPtr lp = new IntPtr((pt.Y << 16) | (pt.X & 0xFFFF));
            if (Native.SendMessageTimeout(h, Native.WM_NCHITTEST, IntPtr.Zero, lp, Native.SMTO_ABORTIFHUNG, 50, out res) == IntPtr.Zero) return Native.HTCAPTION;
            int ht = (int)res.ToInt64();
            return ht >= Native.HTLEFT && ht <= Native.HTBOTTOMRIGHT ? ht : Native.HTCAPTION;
        }

        void CancelDrag()
        {
            dragTimer.Stop();
            if (Drag != null) Overlays.HideDrag();
            Drag = null;
        }

        void OnDragTick()
        {
            var d = Drag;
            if (d == null) { dragTimer.Stop(); return; }
            if (!Native.IsWindow(d.Handle)) { CancelDrag(); return; }
            if (d.ByButton && !Native.KeyDown(Native.VK_LBUTTON)) { EndDrag(d.Handle); return; }
            Rc cur;
            Native.RECT r;
            Native.POINT p;
            Native.GetCursorPos(out p);
            if (d.FullDrag && Native.TryFrame(d.Handle, out r)) cur = Rc.From(r);
            else cur = d.Predict(p);
            if (d.FullDrag && d.Hit == Native.HTCAPTION && (cur.W != d.StartFrame.W || cur.H != d.StartFrame.H)) d.Hit = GuessEdge(d.StartFrame, cur);
            bool shift = Native.KeyDown(Native.VK_SHIFT);
            if (d.Computed && cur.Same(d.Current) && shift == d.LastShift) return;
            d.Computed = true;
            d.Current = cur;
            d.LastShift = shift;
            var scene = BuildScene(d, cur, shift, p);
            lastSceneThreatened = scene.Threatened;
            d.Guides = scene.Guides;
            if (scene.Threatened || !scene.MinOutline.IsEmpty) Overlays.ShowDrag(scene);
            else Overlays.HideDrag();
        }

        static int GuessEdge(Rc a, Rc b)
        {
            bool l = a.L != b.L, r = a.R != b.R, t = a.T != b.T, bo = a.B != b.B;
            if (t && l) return Native.HTTOPLEFT;
            if (t && r) return Native.HTTOPRIGHT;
            if (bo && l) return Native.HTBOTTOMLEFT;
            if (bo && r) return Native.HTBOTTOMRIGHT;
            if (l) return Native.HTLEFT;
            if (r) return Native.HTRIGHT;
            if (t) return Native.HTTOP;
            if (bo) return Native.HTBOTTOM;
            return Native.HTCAPTION;
        }

        /// <summary>What happens if the dragged window is dropped at cur: the labels of the overlay.</summary>
        public DragScene BuildScene(DragState d, Rc cur, bool shift, Native.POINT cursor)
        {
            List<TrackedWindow> list;
            var L = BuildLayout(out list, d.W, cur);
            int ai = list.IndexOf(d.W);
            if (ai < 0) return new DragScene { Dragged = cur, DraggedWindow = d.W, Cursor = cursor };
            var scene = DragScene.Compute(L, list, ai, d, cur, shift, AutoFixAllowed, S, Paused || Suspended);
            scene.Cursor = cursor;
            return scene;
        }

        void EndDrag(IntPtr h)
        {
            var d = Drag;
            if (d == null || d.Handle != h) return;
            dragTimer.Stop();
            Overlays.HideDrag();
            d.ShiftAtEnd = Native.KeyDown(Native.VK_SHIFT);
            Drag = null;
            dropping = d;
            dropTimer.Stop();
            dropTimer.Start();
        }

        void OnDropSettled()
        {
            dropTimer.Stop();
            var d = dropping;
            dropping = null;
            if (d == null) return;
            try { Drop(d); }
            catch (Exception ex) { Log.Error("drop", ex); }
            ProcessPending();
        }

        void Drop(DragState d)
        {
            RefreshRegistry();
            var w = Reg.Get(d.Handle);
            if (w == null || !w.Counted) return;
            Rc end = w.Frame;
            w.SettledFrame = end;
            Log.Debug("drop " + w.Process + " " + d.StartFrame + " -> " + end + (d.ShiftAtEnd ? " (shift)" : ""));
            if (end.Same(d.StartFrame)) { UpdateStatus(); return; }
            bool resized = end.W != d.StartFrame.W || end.H != d.StartFrame.H;
            string edges = resized ? DragState.ChangedEdges(d.StartFrame, end) : "";
            w.UserPlacedAt = Clock.Ms;
            if (resized && S.RememberSizes && !w.Maximized) { Sizes.Remember(w, end); Sizes.SaveIfDirty(); }
            if (w.Maximized || d.ShiftAtEnd || !AutoFixAllowed) { UpdateStatus(); return; }

            var g = new UndoGroup { Reason = "drop" };
            // a drop within 12 px of a snap guide lands on it
            if (!resized && lastSceneThreatened && d.Guides != null)
            {
                int snap = Util.Scale(12, w.Dpi);
                foreach (var gd in d.Guides)
                {
                    if (Math.Abs(gd.Snapped.L - end.L) + Math.Abs(gd.Snapped.T - end.T) > snap) continue;
                    bool denied;
                    if (Act.MoveFrame(w, gd.Snapped, g, out denied)) { end = gd.Snapped; w.SettledFrame = end; }
                    break;
                }
            }
            RunAndApply(Cause.Drop, w, edges, d.StartFrame, end, g);
            Undo.Push(g);
            int others = 0;
            var titles = new List<string>();
            foreach (var it in g.Items) if (it.Handle != w.Handle) { others++; titles.Add(it.Title); }
            if (others > 0)
                Toast.Show(this, end, Texts.P("toast.moved", others), string.Join(", ", titles.ToArray()), Texts.T("toast.undo"), delegate { UndoGroupNow(g); });
            UpdateStatus();
        }

        // ------------------------------------------------------------------ display changes

        public void DisplaySignal(string why)
        {
            if (disposed || !SessionActive) return;
            Log.Debug("display signal: " + why);
            settle.Stop();
            settle.Start();
        }

        /// <summary>1.5 s after the last display event: rescue, rescale, minimum sizes, resolver, one toast.</summary>
        void OnDisplaySettled()
        {
            settle.Stop();
            if (disposed || !SessionActive) return;
            var mons = Monitors.Read();
            string sig = Monitors.Signature(mons);
            if (sig == LayoutSig) return;
            Log.Info("display changed: " + mons.Count + " monitor(s) " + sig);
            var old = Mons;
            foreach (var w in Reg.All.Values)
            {
                var m = Monitors.At(old, w.Frame);
                w.PrevWork = m == null ? default(Rc) : m.Work;
                w.TopMapWidth = -1; // probe again at the new scaling
            }
            layouts.Save(LayoutSig, Reg.All.Values);
            Mons = mons;
            LayoutSig = sig;
            Overlays.OnMonitorsChanged();
            XRay.Close();
            RefreshRegistry();
            var g = new UndoGroup { Reason = "display" };
            RunAndApply(Cause.Display, null, "", default(Rc), null, g);
            Undo.Push(g);
            foreach (var w in Reg.All.Values) w.PrevWork = default(Rc);
            UpdateStatus();
            var saved = layouts.Get(sig);
            if (saved != null && saved.Count > 0)
            {
                Toast.Show(this, Rc.Empty, Texts.T("toast.layout"), Texts.T("toast.layoutHint"), Texts.T("toast.restore"), delegate { RestoreLayout(saved); });
            }
            else if (g.Items.Count > 0)
            {
                Toast.Show(this, Rc.Empty, Texts.P("toast.display", g.Items.Count), "", Texts.T("toast.undo"), delegate { UndoGroupNow(g); });
            }
        }

        void RestoreLayout(Dictionary<IntPtr, Rc> saved)
        {
            RefreshRegistry();
            var g = new UndoGroup { Reason = "restore layout" };
            foreach (var kv in saved)
            {
                var w = Reg.Get(kv.Key);
                if (w == null || !w.Counted || w.Maximized || w.Elevated) continue;
                if (w.WindowRect.Same(kv.Value)) continue;
                bool denied;
                Rc before = w.WindowRect;
                if (Act.SetRect(w, before, kv.Value, out denied)) g.Items.Add(new UndoItem { Handle = w.Handle, Before = before, After = kv.Value, Title = w.ShortTitle });
            }
            Undo.Push(g);
            Log.Info("restored " + g.Items.Count + " window position(s) for this display setup");
            UpdateStatus();
        }

        void OnSafetyTick()
        {
            if (disposed) return;
            Store.Poll();
            if (Paused == false && PausedUntil != DateTime.MinValue) { PausedUntil = DateTime.MinValue; Tray.Update(); }
            if (!SessionActive) return;
            string sig = Monitors.Signature(Monitors.Read());
            if (sig != LayoutSig) DisplaySignal("periodic check");
            if (Drag == null && dropping == null && !settle.Enabled)
            {
                RefreshRegistry();
                UpdateStatus();
                layouts.Save(LayoutSig, Reg.All.Values);
            }
            Sizes.SaveIfDirty();
        }

        // ------------------------------------------------------------------ commands

        public void ShowXRay(bool fromKeyboard)
        {
            if (!SessionActive) return;
            if (XRay.IsOpen) return;
            CancelDrag();
            RefreshRegistry();
            XRay.Show(fromKeyboard);
        }

        /// <summary>Brings a window forward (restoring it if minimized); with giveGlimpse it also moves so it has one.</summary>
        public void BringForward(TrackedWindow w, bool giveGlimpse)
        {
            if (w == null || !Native.IsWindow(w.Handle)) return;
            if (Native.IsIconic(w.Handle)) Native.ShowWindowAsync(w.Handle, Native.SW_RESTORE);
            Native.ForceForeground(w.Handle);
            if (!giveGlimpse) return;
            var t = new WinTimer { Interval = 350 };
            t.Tick += delegate
            {
                t.Stop();
                t.Dispose();
                RefreshRegistry();
                var cur = Reg.Get(w.Handle);
                if (cur == null || !cur.Counted) return;
                var g = new UndoGroup { Reason = "x-ray" };
                RunAndApply(Cause.GiveGlimpse, cur, "", default(Rc), null, g);
                Undo.Push(g);
                UpdateStatus();
            };
            t.Start();
        }

        public void Arrange()
        {
            RefreshRegistry();
            var g = new UndoGroup { Reason = "arrange" };
            RunAndApply(Cause.Arrange, null, "", default(Rc), null, g);
            Undo.Push(g);
            UpdateStatus();
            if (g.Items.Count > 0) Toast.Show(this, Rc.Empty, Texts.P("toast.arranged", g.Items.Count), "", Texts.T("toast.undo"), delegate { UndoGroupNow(g); });
            else Toast.Show(this, Rc.Empty, Texts.T("toast.allInReach"), "", null, null);
        }

        public void UndoLast()
        {
            var g = Undo.Last;
            if (g != null) UndoGroupNow(g);
        }

        public void UndoGroupNow(UndoGroup g)
        {
            if (!Undo.Contains(g)) return;
            int n = Act.UndoGroup(g, Reg);
            Undo.Remove(g);
            Log.Info("undo: " + n + " window(s) restored");
            foreach (var it in g.Items)
            {
                var w = Reg.Get(it.Handle);
                if (w != null) { w.FixedAt = 0; w.UserPlacedAt = Clock.Ms; }
            }
            var t = new WinTimer { Interval = 300 };
            t.Tick += delegate { t.Stop(); t.Dispose(); RefreshRegistry(); foreach (var w in Reg.All.Values) w.SettledFrame = w.Frame; UpdateStatus(); };
            t.Start();
        }

        public void ShowSettings()
        {
            if (SettingsWindow == null || SettingsWindow.IsDisposed) SettingsWindow = new SettingsForm(this);
            if (!SettingsWindow.Visible) SettingsWindow.Show();
            if (SettingsWindow.WindowState == FormWindowState.Minimized) SettingsWindow.WindowState = FormWindowState.Normal;
            Native.ForceForeground(SettingsWindow.Handle);
        }

        /// <summary>Saves settings from the settings window; false when Glimpse.ini is read-only.</summary>
        public bool SaveSettings(Settings s)
        {
            if (!Store.Save(s)) { ApplySettings(false); return false; }
            ApplySettings(false);
            return true;
        }

        public void ApplySettings(bool fromFile)
        {
            Log.SetDebug(S.DebugLog);
            Texts.Load(S.Language);
            Hotkeys.Apply(S.Hotkey);
            corner.Set(S.HotCorner);
            StartWithWindows.Sync(S.StartWithWindows);
            ApplyFirstClick();
            foreach (var w in Reg.All.Values) w.Excluded = S.IsExcluded(w.Process, w.Title);
            Tray.Rebuild();
            if (fromFile && SettingsWindow != null && !SettingsWindow.IsDisposed) SettingsWindow.ReloadFromApp();
            UpdateStatus();
        }

        void ApplyFirstClick()
        {
            if (!Native.IsWindows) return;
            if (S.FirstClickActivates && firstClick == null)
            {
                firstClick = new FirstClickHook(GlimpseOwnerAt, delegate(IntPtr h) { Native.ForceForeground(h); });
                Log.Info("first-click mode on");
            }
            else if (!S.FirstClickActivates && firstClick != null)
            {
                firstClick.Dispose();
                firstClick = null;
            }
        }

        /// <summary>The background window whose free glimpse is at x, y (content part only), or zero.</summary>
        IntPtr GlimpseOwnerAt(int x, int y)
        {
            if (!SessionActive || Paused) return IntPtr.Zero;
            IntPtr hit = Native.GetAncestor(Native.WindowFromPoint(new Native.POINT(x, y)), Native.GA_ROOT);
            var w = Reg.Get(hit);
            if (w == null || w.Zone == null || !w.Zone.Rect.Contains(x, y)) return IntPtr.Zero;
            // a click on the title bar only activates anyway, and must stay free to drag the window
            if (y < w.Frame.T + Util.Scale(32, w.Dpi)) return IntPtr.Zero;
            return hit;
        }

        public void ExitApp()
        {
            Log.Info("exit");
            ExitThread();
        }

        protected override void ExitThreadCore()
        {
            Shutdown();
            base.ExitThreadCore();
        }

        void Shutdown()
        {
            if (disposed) return;
            disposed = true;
            foreach (var t in new[] { debounce, settle, safety, readback, dragTimer, dropTimer }) { t.Stop(); t.Dispose(); }
            if (intake != null) intake.Dispose();
            if (corner != null) corner.Dispose();
            if (firstClick != null) firstClick.Dispose();
            if (Hotkeys != null) Hotkeys.Unregister();
            if (SettingsWindow != null && !SettingsWindow.IsDisposed) SettingsWindow.Dispose();
            if (XRay != null) XRay.Dispose();
            if (Overlays != null) Overlays.Dispose();
            if (Tray != null) Tray.Dispose();
            Sizes.SaveIfDirty();
            Store.Dispose();
            if (Native.IsWindows && Msg != null) { try { Native.WTSUnRegisterSessionNotification(Msg.Handle); } catch { } }
            if (Msg != null) Msg.Dispose();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) Shutdown();
            base.Dispose(disposing);
        }
    }

    /// <summary>Everything the drag overlay shows for one position of the dragged window.</summary>
    internal sealed class DragScene
    {
        public Rc Dragged;
        public TrackedWindow DraggedWindow;
        public Zone DraggedZone;
        public readonly List<SceneWindow> Others = new List<SceneWindow>();
        public List<Guide> Guides = new List<Guide>();
        public Native.POINT Cursor;
        public int Stay, Moves, Hidden;
        public Rc MinOutline;
        public string MinText;
        public bool Shift, Paused, Threatened;
        public FixMode Mode;

        /// <summary>Simulates the drop at cur and sorts the other windows into keeps a glimpse,
        /// moves on drop and would be hidden.</summary>
        public static DragScene Compute(Layout L, List<TrackedWindow> list, int ai, DragState d, Rc cur, bool shift, bool fix, Settings S, bool paused)
        {
            var scene = new DragScene { Dragged = cur, DraggedWindow = list[ai], Shift = shift, Mode = S.Mode, Paused = paused };
            Rc[] before = (Rc[])L.Frames.Clone();
            if (!d.Resizing) scene.Guides = Resolver.Guides(L, ai, Util.Scale(60, list[ai].Dpi));
            var q = Request.For(Cause.Drop, S.Mode, ai, S.PlaceNewWindows);
            q.AnchorStart = d.StartFrame;
            q.DraggedEdges = d.Resizing ? d.Edges() : "";
            q.Fix = !shift && fix;
            var o = Resolver.Resolve(L, q);
            var g = L.Windows[ai];
            if (d.Resizing && g.Resizable && (cur.W < g.MinW || cur.H < g.MinH))
            {
                scene.MinOutline = Geometry.GrowToMin(cur, g.MinW, g.MinH, d.Edges(), L.WorkOf(cur));
                var rule = S.RuleFor(list[ai].Title);
                if (rule != null) scene.MinText = Texts.F("drag.minSize", "width", rule.MinWidth, "height", rule.MinHeight);
            }
            scene.DraggedZone = o.Zones[ai];
            for (int i = 0; i < list.Count; i++)
            {
                if (i == ai || !L.Windows[i].Subject) continue;
                var w = list[i];
                var sw = new SceneWindow { W = w, Frame = before[i], Title = w.ShortTitle, Icon = w.Icon };
                var mv = o.MoveOf(i);
                if (mv != null && mv.Why != "minsize")
                {
                    sw.State = 1;
                    sw.NewFrame = mv.To;
                    sw.NewZone = o.Zones[i];
                    sw.Zone = Geometry.BestZone(L.Windows[i], mv.From, L.P(i), new List<Rc>(), L.Monitors);
                    scene.Moves++;
                }
                else if (o.Zones[i] != null) { sw.State = 0; sw.Zone = o.Zones[i]; scene.Stay++; }
                else
                {
                    sw.State = 2;
                    sw.Reason = shift ? "shift" : (paused ? "paused" : o.Reasons[i]);
                    scene.Hidden++;
                }
                scene.Others.Add(sw);
            }
            scene.Threatened = scene.Moves + scene.Hidden > 0 || scene.DraggedZone == null;
            // windows that were already hidden before the drag are not news
            if (scene.Moves == 0 && scene.DraggedZone != null)
            {
                bool news = false;
                foreach (var sw in scene.Others) if (sw.State == 2 && !sw.W.Hidden) news = true;
                scene.Threatened = news;
            }
            return scene;
        }
    }

    internal sealed class SceneWindow
    {
        public TrackedWindow W;
        public string Title = "";
        public Bitmap Icon;
        public Rc Frame, NewFrame;
        public Zone Zone, NewZone;
        public int State;      // 0 keeps a glimpse, 1 moves on drop, 2 would be hidden
        public string Reason;
    }

    #endregion

    #region Look: theme, fonts and drawing helpers

    /// <summary>Light and dark theme for the tray menu, toasts and settings; flat colours, light on RDP.</summary>
    internal static class Theme
    {
        public static bool Dark;
        public static readonly Color Accent = Color.FromArgb(91, 79, 224);
        public static readonly Color AccentHover = Color.FromArgb(76, 64, 205);
        public static readonly Color Green = Color.FromArgb(22, 163, 74);
        public static readonly Color GreenSoft = Color.FromArgb(220, 252, 231);
        public static readonly Color GreenLine = Color.FromArgb(34, 197, 94);
        public static readonly Color Amber = Color.FromArgb(245, 158, 11);
        public static readonly Color AmberSoft = Color.FromArgb(254, 243, 199);
        public static readonly Color Red = Color.FromArgb(220, 38, 38);
        public static readonly Color RedSoft = Color.FromArgb(254, 226, 226);
        public static readonly Color Guide = Color.FromArgb(124, 108, 242);
        public static readonly Color Ink = Color.FromArgb(27, 27, 34);
        public static readonly Color Chip = Color.FromArgb(30, 32, 40);

        public static Color Page { get { return Dark ? Color.FromArgb(32, 32, 38) : Color.FromArgb(244, 244, 247); } }
        public static Color Card { get { return Dark ? Color.FromArgb(43, 43, 50) : Color.White; } }
        public static Color Border { get { return Dark ? Color.FromArgb(62, 62, 72) : Color.FromArgb(226, 226, 233); } }
        public static Color Text { get { return Dark ? Color.FromArgb(242, 242, 245) : Color.FromArgb(27, 27, 34); } }
        public static Color Sub { get { return Dark ? Color.FromArgb(166, 166, 178) : Color.FromArgb(104, 104, 118); } }
        public static Color Hover { get { return Dark ? Color.FromArgb(55, 55, 64) : Color.FromArgb(240, 240, 246); } }
        public static Color Input { get { return Dark ? Color.FromArgb(32, 32, 38) : Color.FromArgb(246, 246, 249); } }
        public static Color AccentSoft { get { return Dark ? Color.FromArgb(49, 45, 92) : Color.FromArgb(237, 235, 255); } }
        public static Color AccentText { get { return Dark ? Color.FromArgb(168, 160, 255) : Accent; } }
        public static Color Off { get { return Dark ? Color.FromArgb(90, 90, 102) : Color.FromArgb(203, 203, 212); } }

        /// <summary>Set by the preview renderer to draw a theme regardless of Windows.</summary>
        public static bool? Forced;

        public static void Refresh()
        {
            if (Forced.HasValue) { Dark = Forced.Value; return; }
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"))
                {
                    object v = k == null ? null : k.GetValue("AppsUseLightTheme");
                    Dark = v is int && (int)v == 0;
                }
            }
            catch { Dark = false; }
        }
    }

    internal static class Draw
    {
        static readonly Dictionary<string, Font> fonts = new Dictionary<string, Font>();
        static string family;
        static Graphics measure;

        public static string Family
        {
            get
            {
                if (family != null) return family;
                family = "Segoe UI";
                try
                {
                    using (var f = new Font(family, 10f)) if (!f.Name.Equals(family, StringComparison.OrdinalIgnoreCase)) family = SystemFonts.MessageBoxFont.FontFamily.Name;
                }
                catch { family = FontFamily.GenericSansSerif.Name; }
                return family;
            }
        }

        /// <summary>Font sized in pixels (callers scale by DPI), so it is right on every monitor.</summary>
        public static Font Font(float px, bool bold)
        {
            px = (float)Math.Round(Math.Max(6f, px), 1);
            string key = px.ToString(CultureInfo.InvariantCulture) + (bold ? "b" : "");
            Font f;
            if (fonts.TryGetValue(key, out f)) return f;
            string fam = Family;
            if (bold)
            {
                try
                {
                    using (var test = new Font("Segoe UI Semibold", 10f))
                        if (test.Name == "Segoe UI Semibold") { f = new Font("Segoe UI Semibold", px, FontStyle.Regular, GraphicsUnit.Pixel); }
                }
                catch { }
                if (f == null) f = new Font(fam, px, FontStyle.Bold, GraphicsUnit.Pixel);
            }
            else f = new Font(fam, px, FontStyle.Regular, GraphicsUnit.Pixel);
            fonts[key] = f;
            return f;
        }

        public static Font Mono(float px)
        {
            string key = "mono" + px.ToString(CultureInfo.InvariantCulture);
            Font f;
            if (fonts.TryGetValue(key, out f)) return f;
            try { f = new Font("Consolas", px, FontStyle.Regular, GraphicsUnit.Pixel); }
            catch { f = new Font(FontFamily.GenericMonospace, px, FontStyle.Regular, GraphicsUnit.Pixel); }
            fonts[key] = f;
            return f;
        }

        static Graphics Measurer
        {
            get
            {
                if (measure == null)
                {
                    measure = Graphics.FromImage(new Bitmap(1, 1));
                    measure.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                }
                return measure;
            }
        }

        public static readonly StringFormat Typo = MakeTypo();
        static StringFormat MakeTypo()
        {
            var sf = (StringFormat)StringFormat.GenericTypographic.Clone();
            sf.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces | StringFormatFlags.NoWrap;
            sf.Trimming = StringTrimming.EllipsisCharacter;
            return sf;
        }

        public static SizeF Measure(string s, Font f)
        {
            if (string.IsNullOrEmpty(s)) return new SizeF(0, f.Height);
            lock (fonts) return Measurer.MeasureString(s, f, int.MaxValue, Typo);
        }

        public static SizeF MeasureWrapped(string s, Font f, float width)
        {
            lock (fonts) return Measurer.MeasureString(s ?? "", f, new SizeF(width, 10000), StringFormat.GenericDefault);
        }

        public static void Text(Graphics g, string s, Font f, Color c, float x, float y)
        {
            using (var b = new SolidBrush(c)) g.DrawString(s, f, b, x, y, Typo);
        }

        public static void TextIn(Graphics g, string s, Font f, Color c, RectangleF r)
        {
            using (var b = new SolidBrush(c)) g.DrawString(s, f, b, r, Typo);
        }

        public static GraphicsPath Round(RectangleF r, float radius)
        {
            var p = new GraphicsPath();
            float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
            if (d <= 0.5f) { p.AddRectangle(r); return p; }
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        public static void Fill(Graphics g, RectangleF r, float radius, Color c)
        {
            using (var p = Round(r, radius)) using (var b = new SolidBrush(c)) g.FillPath(b, p);
        }

        public static void Stroke(Graphics g, RectangleF r, float radius, Color c, float width, bool dashed)
        {
            using (var p = Round(r, radius))
            using (var pen = new Pen(c, width))
            {
                if (dashed) pen.DashPattern = new[] { 4f, 3f };
                g.DrawPath(pen, p);
            }
        }

        public static Color Alpha(Color c, int a) { return Color.FromArgb(a, c.R, c.G, c.B); }

        /// <summary>Colour for a letter badge when an app has no icon.</summary>
        public static Color BadgeColor(string s)
        {
            Color[] palette = { Color.FromArgb(91, 79, 224), Color.FromArgb(14, 116, 204), Color.FromArgb(22, 128, 61), Color.FromArgb(217, 119, 6),
                Color.FromArgb(107, 114, 128), Color.FromArgb(190, 24, 93), Color.FromArgb(13, 148, 136) };
            int h = 0;
            foreach (char c in s ?? "") h = h * 31 + c;
            return palette[(h & 0x7fffffff) % palette.Length];
        }

        /// <summary>App icon, or a coloured letter badge as in the mock-ups.</summary>
        public static void AppBadge(Graphics g, RectangleF r, string name, Bitmap icon)
        {
            if (icon != null)
            {
                var old = g.InterpolationMode;
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.DrawImage(icon, r);
                g.InterpolationMode = old;
                return;
            }
            Fill(g, r, r.Width * 0.22f, BadgeColor(name));
            string letter = string.IsNullOrEmpty(name) ? "?" : name.Trim().Substring(0, 1).ToUpperInvariant();
            var f = Font(r.Height * 0.62f, true);
            var sz = Measure(letter, f);
            Text(g, letter, f, Color.White, r.X + (r.Width - sz.Width) / 2, r.Y + (r.Height - sz.Height) / 2);
        }

        /// <summary>The Glimpse mark: two overlapping windows, the front one filled.</summary>
        public static void Logo(Graphics g, RectangleF r, Color back, Color front, Color rim)
        {
            float u = r.Width / 16f;
            var b = new RectangleF(r.X + 0.75f * u, r.Y + 0.75f * u, 10.5f * u, 9.5f * u);
            Fill(g, b, 2.2f * u, Alpha(back, 70));
            Stroke(g, b, 2.2f * u, back, Math.Max(1f, 1.25f * u), false);
            var f = new RectangleF(r.X + 5f * u, r.Y + 5f * u, 10.25f * u, 10.25f * u);
            Fill(g, new RectangleF(f.X - 0.9f * u, f.Y - 0.9f * u, f.Width + 1.3f * u, f.Height + 1.3f * u), 3.1f * u, rim);
            Fill(g, f, 2.2f * u, front);
        }

        public static void CheckBadge(Graphics g, RectangleF r, Color fill)
        {
            Fill(g, r, r.Width / 2, fill);
            using (var pen = new Pen(Color.White, Math.Max(1.5f, r.Width / 9f)))
            {
                pen.StartCap = pen.EndCap = LineCap.Round;
                g.DrawLines(pen, new[] { new PointF(r.X + r.Width * 0.28f, r.Y + r.Height * 0.52f), new PointF(r.X + r.Width * 0.44f, r.Y + r.Height * 0.68f), new PointF(r.X + r.Width * 0.73f, r.Y + r.Height * 0.36f) });
            }
        }

        public static void CrossBadge(Graphics g, RectangleF r, Color fill, Color mark)
        {
            Fill(g, r, r.Width / 2, fill);
            using (var pen = new Pen(mark, Math.Max(1.5f, r.Width / 9f)))
            {
                pen.StartCap = pen.EndCap = LineCap.Round;
                float a = r.Width * 0.32f;
                g.DrawLine(pen, r.X + a, r.Y + a, r.Right - a, r.Bottom - a);
                g.DrawLine(pen, r.Right - a, r.Y + a, r.X + a, r.Bottom - a);
            }
        }

        public static void Arrow(Graphics g, PointF from, PointF to, Color c, float width)
        {
            using (var pen = new Pen(c, width))
            {
                pen.CustomEndCap = new AdjustableArrowCap(3.2f, 3.2f, true);
                g.DrawLine(pen, from, to);
            }
        }

        /// <summary>Simple line glyphs for menus and buttons.</summary>
        public static void Glyph(Graphics g, string name, RectangleF r, Color c)
        {
            float u = r.Width / 16f;
            using (var pen = new Pen(c, Math.Max(1.2f, 1.4f * u)))
            {
                pen.StartCap = pen.EndCap = LineCap.Round;
                pen.LineJoin = LineJoin.Round;
                switch (name)
                {
                    case "xray":
                        using (var p = Round(new RectangleF(r.X + 1.5f * u, r.Y + 2 * u, 8.5f * u, 7.5f * u), 1.5f * u)) g.DrawPath(pen, p);
                        using (var p = Round(new RectangleF(r.X + 6 * u, r.Y + 6.5f * u, 8.5f * u, 7.5f * u), 1.5f * u)) g.DrawPath(pen, p);
                        break;
                    case "arrange":
                        using (var p = Round(new RectangleF(r.X + 1.5f * u, r.Y + 2.5f * u, 13 * u, 11 * u), 1.5f * u)) g.DrawPath(pen, p);
                        g.DrawLine(pen, r.X + 1.5f * u, r.Y + 5.5f * u, r.X + 14.5f * u, r.Y + 5.5f * u);
                        using (var b = new SolidBrush(c)) g.FillRectangle(b, r.X + 3 * u, r.Y + 7.5f * u, 4 * u, 4.5f * u);
                        break;
                    case "undo":
                        g.DrawArc(pen, r.X + 3 * u, r.Y + 3.5f * u, 10 * u, 9 * u, 200, 250);
                        g.DrawLines(pen, new[] { new PointF(r.X + 2.5f * u, r.Y + 3 * u), new PointF(r.X + 3.2f * u, r.Y + 7 * u), new PointF(r.X + 7 * u, r.Y + 6.2f * u) });
                        break;
                    case "pause":
                        g.DrawLine(pen, r.X + 5.5f * u, r.Y + 3 * u, r.X + 5.5f * u, r.Y + 13 * u);
                        g.DrawLine(pen, r.X + 10.5f * u, r.Y + 3 * u, r.X + 10.5f * u, r.Y + 13 * u);
                        break;
                    case "play":
                        g.DrawPolygon(pen, new[] { new PointF(r.X + 5 * u, r.Y + 3 * u), new PointF(r.X + 13 * u, r.Y + 8 * u), new PointF(r.X + 5 * u, r.Y + 13 * u) });
                        break;
                    case "settings":
                        for (int i = 0; i < 3; i++)
                        {
                            float y = r.Y + (4 + i * 4) * u;
                            g.DrawLine(pen, r.X + 2 * u, y, r.X + 14 * u, y);
                            float x = r.X + (i == 1 ? 10 : 5.5f) * u;
                            using (var b = new SolidBrush(c)) g.FillEllipse(b, x - 1.6f * u, y - 1.6f * u, 3.2f * u, 3.2f * u);
                        }
                        break;
                    case "exit":
                        g.DrawArc(pen, r.X + 3 * u, r.Y + 3.5f * u, 10 * u, 10 * u, -60, 300);
                        g.DrawLine(pen, r.X + 8 * u, r.Y + 2 * u, r.X + 8 * u, r.Y + 8 * u);
                        break;
                    case "search":
                        g.DrawEllipse(pen, r.X + 2 * u, r.Y + 2 * u, 9 * u, 9 * u);
                        g.DrawLine(pen, r.X + 10 * u, r.Y + 10 * u, r.X + 14 * u, r.Y + 14 * u);
                        break;
                    case "target":
                        g.DrawEllipse(pen, r.X + 2.5f * u, r.Y + 2.5f * u, 11 * u, 11 * u);
                        g.DrawLine(pen, r.X + 8 * u, r.Y + 0.5f * u, r.X + 8 * u, r.Y + 5 * u);
                        g.DrawLine(pen, r.X + 8 * u, r.Y + 11 * u, r.X + 8 * u, r.Y + 15.5f * u);
                        g.DrawLine(pen, r.X + 0.5f * u, r.Y + 8 * u, r.X + 5 * u, r.Y + 8 * u);
                        g.DrawLine(pen, r.X + 11 * u, r.Y + 8 * u, r.X + 15.5f * u, r.Y + 8 * u);
                        break;
                    case "down": Arrow(g, new PointF(r.X + 8 * u, r.Y + 2 * u), new PointF(r.X + 8 * u, r.Y + 13 * u), c, pen.Width); break;
                    case "up": Arrow(g, new PointF(r.X + 8 * u, r.Y + 14 * u), new PointF(r.X + 8 * u, r.Y + 3 * u), c, pen.Width); break;
                    case "left": Arrow(g, new PointF(r.X + 14 * u, r.Y + 8 * u), new PointF(r.X + 3 * u, r.Y + 8 * u), c, pen.Width); break;
                    case "right": Arrow(g, new PointF(r.X + 2 * u, r.Y + 8 * u), new PointF(r.X + 13 * u, r.Y + 8 * u), c, pen.Width); break;
                }
            }
        }

        /// <summary>Text with **bold** parts on one line; returns its width.</summary>
        public static float RichText(Graphics g, string s, float px, Color normal, Color bold, float x, float y, bool draw)
        {
            float cx = x;
            bool b = false;
            foreach (string part in (s ?? "").Split(new[] { "**" }, StringSplitOptions.None))
            {
                if (part.Length > 0)
                {
                    var f = Font(px, b);
                    if (draw) Text(g, part, f, b ? bold : normal, cx, y);
                    cx += Measure(part, f).Width;
                }
                b = !b;
            }
            return cx - x;
        }
    }

    #endregion

    #region Overlays: drag feedback and flashes on click-through layered windows

    /// <summary>One thing drawn on an overlay. Equal keys mean equal pixels, so only changes are redrawn.</summary>
    internal abstract class OverlayElement
    {
        public Rc Bounds;
        public string Key;
        public abstract void Draw(Graphics g);
    }

    internal sealed class DelegateElement : OverlayElement
    {
        readonly Action<Graphics> paint;
        public DelegateElement(Rc bounds, string key, Action<Graphics> paint) { Bounds = bounds; Key = key; this.paint = paint; }
        public override void Draw(Graphics g) { paint(g); }
    }

    /// <summary>Builds overlay elements; sizes follow the DPI of the monitor under each element.</summary>
    internal static class Elements
    {
        public static float Scale(int dpi) { return dpi / 96f; }

        /// <summary>A label: optional badge, text, optional trailing check or cross.</summary>
        public static OverlayElement Pill(int x, int y, int dpi, string text, Bitmap icon, string iconName, Color fill, Color border, Color ink, int trailing, Color trailColor, float maxWidth)
        {
            float k = Scale(dpi);
            var f = Draw.Font(14 * k, true);
            float h = 28 * k, pad = 9 * k, badge = icon != null || iconName != null ? 18 * k : 0;
            float tw = Math.Min(Draw.Measure(text, f).Width, maxWidth > 0 ? maxWidth * k : 380 * k);
            float w = pad + (badge > 0 ? badge + 7 * k : 0) + tw + pad;
            float total = w + (trailing != 0 ? 8 * k + 22 * k : 0);
            var bounds = new Rc(x - 2, y - 2, x + (int)Math.Ceiling(total) + 3, y + (int)Math.Ceiling(h) + 3);
            string key = "pill|" + x + "," + y + "|" + text + "|" + fill.ToArgb() + "|" + trailing + "|" + dpi;
            return new DelegateElement(bounds, key, delegate(Graphics g)
            {
                var r = new RectangleF(x, y, w, h);
                Draw.Fill(g, r, 7 * k, fill);
                if (border.A > 0) Draw.Stroke(g, r, 7 * k, border, 2 * k, false);
                float cx = x + pad;
                if (badge > 0)
                {
                    Draw.AppBadge(g, new RectangleF(cx, y + (h - badge) / 2, badge, badge), iconName ?? text, icon);
                    cx += badge + 7 * k;
                }
                Draw.TextIn(g, text, f, ink, new RectangleF(cx, y + (h - f.Height) / 2 - 1, tw + 2, f.Height + 2));
                if (trailing != 0)
                {
                    var t = new RectangleF(x + w + 8 * k, y + (h - 22 * k) / 2, 22 * k, 22 * k);
                    if (trailing == 1) Draw.CheckBadge(g, t, trailColor); else Draw.CrossBadge(g, t, Color.White, fill);
                }
            });
        }

        public static OverlayElement Outline(Rc r, int dpi, Color c, bool dashed, int fillAlpha, float width)
        {
            float k = Scale(dpi);
            int pad = (int)Math.Ceiling(width * k) + 2;
            string key = "outline|" + r + "|" + c.ToArgb() + "|" + dashed + "|" + fillAlpha + "|" + width + "|" + dpi;
            return new DelegateElement(r.Inflate(pad), key, delegate(Graphics g)
            {
                var rf = new RectangleF(r.L + width * k / 2, r.T + width * k / 2, r.W - width * k, r.H - width * k);
                if (fillAlpha > 0) Draw.Fill(g, rf, 6 * k, Draw.Alpha(c, fillAlpha));
                using (var p = Draw.Round(rf, 6 * k))
                using (var pen = new Pen(c, width * k))
                {
                    if (dashed) pen.DashPattern = new[] { 3f, 2f };
                    g.DrawPath(pen, p);
                }
            });
        }

        public static OverlayElement Box(Rc r, int dpi, string text, Color fill, Color border, Color ink)
        {
            float k = Scale(dpi);
            string key = "box|" + r + "|" + text + "|" + fill.ToArgb() + "|" + dpi;
            return new DelegateElement(r.Inflate(3), key, delegate(Graphics g)
            {
                var rf = new RectangleF(r.L, r.T, r.W, r.H);
                Draw.Fill(g, rf, 6 * k, fill);
                Draw.Stroke(g, rf, 6 * k, border, 2 * k, false);
                var f = Draw.Font(14 * k, true);
                var sz = Draw.Measure(text, f);
                float tx = r.L + Math.Max(6 * k, (r.W - sz.Width) / 2);
                Draw.TextIn(g, text, f, ink, new RectangleF(tx, r.T + (r.H - f.Height) / 2, r.W - 12 * k, f.Height + 2));
            });
        }

        public static OverlayElement ArrowLine(PointF a, PointF b, int dpi, Color c)
        {
            float k = Scale(dpi);
            var bounds = new Rc((int)Math.Min(a.X, b.X) - 12, (int)Math.Min(a.Y, b.Y) - 12, (int)Math.Max(a.X, b.X) + 12, (int)Math.Max(a.Y, b.Y) + 12);
            string key = "arrow|" + a + "|" + b + "|" + c.ToArgb();
            return new DelegateElement(bounds, key, delegate(Graphics g) { Draw.Arrow(g, a, b, c, 2.5f * k); });
        }

        /// <summary>Dark summary chip near the cursor: coloured dots and counts.</summary>
        public static OverlayElement Chip(int x, int y, int dpi, List<KeyValuePair<Color, string>> items, Rc keepIn)
        {
            float k = Scale(dpi);
            var f = Draw.Font(14 * k, false);
            float h = 32 * k, pad = 12 * k, dot = 8 * k, gap = 14 * k;
            float w = pad;
            var sb = new StringBuilder();
            foreach (var it in items) { w += dot + 6 * k + Draw.Measure(it.Value, f).Width + gap; sb.Append(it.Value).Append('|'); }
            w += pad - gap;
            if (!keepIn.IsEmpty)
            {
                // flip to the left of the cursor or above it near the screen edge
                if (x + w > keepIn.R - 4) x = Math.Max(keepIn.L + 4, x - (int)w - (int)(56 * k));
                if (y + h > keepIn.B - 4) y = Math.Max(keepIn.T + 4, y - (int)h - (int)(52 * k));
            }
            string key = "chip|" + x + "," + y + "|" + sb + dpi;
            return new DelegateElement(new Rc(x - 2, y - 2, x + (int)w + 3, y + (int)h + 3), key, delegate(Graphics g)
            {
                Draw.Fill(g, new RectangleF(x, y, w, h), 8 * k, Draw.Alpha(Theme.Chip, 240));
                float cx = x + pad;
                foreach (var it in items)
                {
                    using (var b = new SolidBrush(it.Key)) g.FillEllipse(b, cx, y + (h - dot) / 2, dot, dot);
                    cx += dot + 6 * k;
                    Draw.Text(g, it.Value, f, Color.White, cx, y + (h - f.Height) / 2);
                    cx += Draw.Measure(it.Value, f).Width + gap;
                }
            });
        }

        public static OverlayElement GuideLine(Rc line, int dpi, string label, bool horizontal)
        {
            float k = Scale(dpi);
            var f = Draw.Font(13 * k, true);
            var sz = Draw.Measure(label, f);
            float lw = sz.Width + 20 * k, lh = 26 * k;
            float lx = horizontal ? line.R - lw : line.L + 6 * k;
            float ly = horizontal ? line.T + 6 * k : line.B - lh;
            var bounds = line.Inflate(4).Union(new Rc((int)lx - 2, (int)ly - 2, (int)(lx + lw) + 3, (int)(ly + lh) + 3));
            string key = "guide|" + line + "|" + label + "|" + dpi;
            return new DelegateElement(bounds, key, delegate(Graphics g)
            {
                using (var pen = new Pen(Theme.Guide, 2 * k))
                {
                    pen.DashPattern = new[] { 4f, 3f };
                    if (horizontal) g.DrawLine(pen, line.L, line.T, line.R, line.T);
                    else g.DrawLine(pen, line.L, line.T, line.L, line.B);
                }
                Draw.Fill(g, new RectangleF(lx, ly, lw, lh), 6 * k, Theme.Guide);
                Draw.Text(g, label, f, Color.White, lx + 10 * k, ly + (lh - f.Height) / 2);
            });
        }

        public static OverlayElement Callout(int x, int y, int dpi, string title, string text, Rc keepIn)
        {
            float k = Scale(dpi);
            var ft = Draw.Font(15 * k, true);
            var fs = Draw.Font(13 * k, false);
            float w = Math.Max(Draw.Measure(title, ft).Width, Draw.Measure(text, fs).Width) + 32 * k;
            float h = 62 * k;
            if (!keepIn.IsEmpty && x + w > keepIn.R - 4) x = Math.Max(keepIn.L + 4, keepIn.R - 4 - (int)Math.Ceiling(w));
            string key = "callout|" + x + "," + y + "|" + title + "|" + text + "|" + dpi;
            return new DelegateElement(new Rc(x - 3, y - 3, x + (int)w + 4, y + (int)h + 4), key, delegate(Graphics g)
            {
                var r = new RectangleF(x, y, w, h);
                Draw.Fill(g, r, 8 * k, Color.White);
                Draw.Stroke(g, r, 8 * k, Color.FromArgb(230, 230, 236), 1, false);
                var st = Draw.Measure(title, ft);
                Draw.Text(g, title, ft, Theme.Red, x + (w - st.Width) / 2, y + 10 * k);
                var ss = Draw.Measure(text, fs);
                Draw.Text(g, text, fs, Color.FromArgb(75, 75, 88), x + (w - ss.Width) / 2, y + 34 * k);
            });
        }

        /// <summary>The overlay elements for one drag position (see the drag mock-up).</summary>
        public static List<OverlayElement> ForDrag(DragScene s, Func<Rc, int> dpiAt, Func<Rc, Rc> screenAt)
        {
            var els = new List<OverlayElement>();
            int cursorDpi = dpiAt(new Rc(s.Cursor.X, s.Cursor.Y, s.Cursor.X + 1, s.Cursor.Y + 1));
            foreach (var w in s.Others)
            {
                int dpi = dpiAt(w.Frame);
                float k = Scale(dpi);
                Bitmap icon = w.W == null || w.W.Handle == IntPtr.Zero ? w.Icon : WindowRegistry.LoadIcon(w.W, (int)(18 * k));
                if (w.State == 0 && w.Zone != null)
                {
                    els.Add(Pill(w.Zone.Rect.L + (int)(4 * k), w.Zone.Rect.T + (int)(4 * k), dpi, w.Title, icon, w.Title, Theme.GreenSoft, Theme.GreenLine, Theme.Ink, 1, Theme.Green, 300));
                }
                else if (w.State == 1)
                {
                    els.Add(Outline(w.Frame, dpi, Theme.Amber, true, 40, 2.5f));
                    Rc at = w.Zone != null ? w.Zone.Rect : w.Frame;
                    els.Add(Pill(at.L + (int)(4 * k), at.T + (int)(4 * k), dpi, w.Title, icon, w.Title, Color.FromArgb(251, 191, 36), Color.Empty, Theme.Ink, 0, Color.Empty, 300));
                    if (w.NewZone != null)
                    {
                        Rc z = w.NewZone.Rect;
                        string peek = Texts.F("drag.peek", "title", Util.Shorten(w.Title, 30));
                        var f = Draw.Font(14 * k, true);
                        int bw = Math.Max(z.W, (int)(Draw.Measure(peek, f).Width + 40 * k));
                        int bh = Math.Max((int)(36 * k), Math.Min(z.H, (int)(48 * k)));
                        Rc box;
                        switch (w.NewZone.Edge)
                        {
                            case Edge.Bottom: box = new Rc(w.NewFrame.L, w.NewFrame.B - bh, w.NewFrame.L + Math.Max(bw, Math.Min(w.NewFrame.W, bw)), w.NewFrame.B); break;
                            case Edge.Top: box = new Rc(w.NewFrame.L, w.NewFrame.T, w.NewFrame.L + bw, w.NewFrame.T + bh); break;
                            default: box = new Rc(z.L, z.T, z.L + bw, z.T + bh); break;
                        }
                        els.Add(Box(box, dpi, peek, Theme.AmberSoft, Theme.Amber, Color.FromArgb(120, 53, 15)));
                        int dx = w.NewFrame.L - w.Frame.L, dy = w.NewFrame.T - w.Frame.T;
                        int px = (int)Math.Round(Math.Sqrt((double)dx * dx + (double)dy * dy) * 96.0 / dpi);
                        var from = new PointF(box.CX - dx, box.CY - dy - (dy > 0 ? bh : 0));
                        var to = new PointF(box.CX, dy > 0 ? box.T - 4 : box.CY);
                        if (Math.Abs(dx) > Math.Abs(dy)) { from = new PointF(box.CX - dx, box.CY); to = new PointF(box.CX + (dx > 0 ? -bw / 2 : bw / 2), box.CY); }
                        if (Math.Abs(to.X - from.X) + Math.Abs(to.Y - from.Y) > 12) els.Add(ArrowLine(from, to, dpi, Theme.Amber));
                        string dir = Math.Abs(dx) > Math.Abs(dy) ? (dx > 0 ? "right" : "left") : (dy > 0 ? "down" : "up");
                        els.Add(DirChip(box.R + (int)(16 * k), box.CY - (int)(16 * k), dpi, dir, Texts.F("drag.movesPx", "px", px), box, screenAt(box)));
                    }
                }
                else if (w.State == 2)
                {
                    els.Add(Outline(w.Frame, dpi, Theme.Red, true, 36, 2.5f));
                    Rc at = w.Zone != null ? w.Zone.Rect : w.Frame;
                    els.Add(Pill(w.Frame.L + (int)(12 * k), w.Frame.T + (int)(12 * k), dpi, w.Title, icon, w.Title, Color.FromArgb(239, 68, 68), Color.Empty, Color.White, 2, Color.FromArgb(255, 255, 255, 255), 260));
                    string reason = ReasonText(w.Reason);
                    els.Add(Callout(w.Frame.L + (int)(12 * k), w.Frame.T + (int)(56 * k), dpi, Texts.T("drag.wouldBeHidden"), reason, screenAt(w.Frame)));
                }
            }
            // the dragged window's own status
            {
                int dpi = dpiAt(s.Dragged);
                float k = Scale(dpi);
                if (s.DraggedZone != null)
                    els.Add(Pill(s.Dragged.L + (int)(40 * k), s.Dragged.T - (int)(40 * k), dpi, Texts.T("drag.keeps"), s.DraggedWindow == null || s.DraggedWindow.Handle == IntPtr.Zero ? null : WindowRegistry.LoadIcon(s.DraggedWindow, (int)(18 * k)),
                        s.DraggedWindow == null ? "?" : s.DraggedWindow.ShortTitle, Theme.GreenSoft, Theme.GreenLine, Theme.Ink, 1, Theme.Green, 360));
                else
                {
                    els.Add(Outline(s.Dragged, dpi, Theme.Red, false, 0, 3));
                    els.Add(Pill(s.Dragged.L + (int)(40 * k), s.Dragged.T - (int)(40 * k), dpi, Texts.T("drag.noGlimpse"), null, null, Theme.Red, Color.Empty, Color.White, 2, Color.FromArgb(255, 255, 255, 255), 360));
                }
            }
            if (!s.MinOutline.IsEmpty)
            {
                int dpi = dpiAt(s.MinOutline);
                float k = Scale(dpi);
                els.Add(Outline(s.MinOutline, dpi, Theme.Accent, true, 0, 2.5f));
                if (!string.IsNullOrEmpty(s.MinText))
                    els.Add(Pill(s.MinOutline.L + (int)(8 * k), s.MinOutline.B - (int)(36 * k), dpi, s.MinText, null, null, Theme.Accent, Color.Empty, Color.White, 0, Color.Empty, 300));
            }
            if (s.Guides != null)
            {
                int shown = 0;
                foreach (var gd in s.Guides)
                {
                    if (shown++ >= 2) break;
                    int dpi = dpiAt(gd.Snapped);
                    bool horiz = gd.Edge == Edge.Top || gd.Edge == Edge.Bottom;
                    Rc line = horiz ? new Rc(Math.Min(gd.Snapped.L, gd.Zone.L), gd.Coord, Math.Max(gd.Snapped.R, gd.Zone.R), gd.Coord + 1)
                                    : new Rc(gd.Coord, Math.Min(gd.Snapped.T, gd.Zone.T), gd.Coord + 1, Math.Max(gd.Snapped.B, gd.Zone.B));
                    els.Add(GuideLine(line, dpi, Texts.T("drag.guide"), horiz));
                }
            }
            // summary chip by the cursor
            var items = new List<KeyValuePair<Color, string>>();
            if (s.Stay > 0) items.Add(new KeyValuePair<Color, string>(Theme.GreenLine, Texts.P("drag.stay", s.Stay)));
            if (s.Moves > 0) items.Add(new KeyValuePair<Color, string>(Theme.Amber, Texts.P("drag.move", s.Moves)));
            if (s.Hidden > 0) items.Add(new KeyValuePair<Color, string>(Color.FromArgb(248, 113, 113), Texts.P("drag.hide", s.Hidden)));
            if (items.Count > 0)
            {
                float k = Scale(cursorDpi);
                var cursorRect = new Rc(s.Cursor.X, s.Cursor.Y, s.Cursor.X + 1, s.Cursor.Y + 1);
                els.Add(Chip(s.Cursor.X + (int)(28 * k), s.Cursor.Y + (int)(26 * k), cursorDpi, items, screenAt(cursorRect)));
            }
            return els;
        }

        static OverlayElement DirChip(int x, int y, int dpi, string dir, string text, Rc box, Rc keepIn)
        {
            float k = Scale(dpi);
            var f = Draw.Font(14 * k, true);
            float h = 32 * k, w = 14 * k + 18 * k + 8 * k + Draw.Measure(text, f).Width + 16 * k;
            if (!keepIn.IsEmpty && x + w > keepIn.R - 4)
            {
                // no room right of the box: put it under the box, or above when that leaves the screen
                x = Math.Max(keepIn.L + 4, Math.Min(box.L, keepIn.R - 4 - (int)w));
                y = box.B + (int)(10 * k);
                if (y + h > keepIn.B - 4) y = box.T - (int)h - (int)(10 * k);
            }
            string key = "dir|" + x + "," + y + "|" + dir + "|" + text + "|" + dpi;
            return new DelegateElement(new Rc(x - 2, y - 2, x + (int)w + 3, y + (int)h + 3), key, delegate(Graphics g)
            {
                Draw.Fill(g, new RectangleF(x, y, w, h), h / 2, Color.FromArgb(251, 191, 36));
                Draw.Glyph(g, dir, new RectangleF(x + 12 * k, y + (h - 18 * k) / 2, 18 * k, 18 * k), Theme.Ink);
                Draw.Text(g, text, f, Theme.Ink, x + 38 * k, y + (h - f.Height) / 2);
            });
        }

        public static string ReasonText(string reason)
        {
            switch (reason)
            {
                case "elevated": return Texts.T("drag.reason.elevated");
                case "stubborn": return Texts.T("drag.reason.stubborn");
                case "excluded": return Texts.T("drag.reason.excluded");
                case "hung": return Texts.T("drag.reason.hung");
                case "topmost": return Texts.T("drag.reason.topmost");
                case "guide": return Texts.T("drag.reason.guide");
                case "paused": return Texts.T("drag.reason.paused");
                case "limit": return Texts.T("drag.reason.limit");
                case "shift": return Texts.T("drag.reason.shift");
                default: return Texts.T("drag.reason.noSpace");
            }
        }

        public static List<OverlayElement> ForFlash(List<Rc> frames, Func<Rc, int> dpiAt)
        {
            var els = new List<OverlayElement>();
            foreach (Rc r in frames) els.Add(Outline(r.Inflate(2), dpiAt(r), Theme.Accent, false, 0, 3.5f));
            return els;
        }
    }

    /// <summary>One click-through, top-most layered window per monitor. While idle it is a 1x1 transparent
    /// window that still receives WM_DPICHANGED for its monitor; the drawing buffer exists only while needed.</summary>
    internal sealed class OverlaySurface : NativeWindow, IDisposable
    {
        public readonly MonitorInfo Mon;
        readonly Action<string> onDpi;
        IntPtr memDC, dib, oldObj, bits;
        Bitmap bmp;
        int bw, bh;
        Dictionary<string, Rc> shown = new Dictionary<string, Rc>();
        bool big;

        public OverlaySurface(MonitorInfo mon, Action<string> onDpi, IntPtr owner)
        {
            Mon = mon;
            this.onDpi = onDpi;
            var cp = new CreateParams
            {
                Caption = "Glimpse overlay",
                Parent = owner, // owned by the hidden message window: never a taskbar button
                X = mon.Bounds.L, Y = mon.Bounds.T, Width = 1, Height = 1,
                Style = unchecked((int)0x80000000),
                ExStyle = (int)(Native.WS_EX_LAYERED | Native.WS_EX_TRANSPARENT | Native.WS_EX_TOPMOST | Native.WS_EX_TOOLWINDOW | Native.WS_EX_NOACTIVATE)
            };
            CreateHandle(cp);
            Idle();
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == Native.WM_DPICHANGED) { onDpi("dpi"); m.Result = IntPtr.Zero; return; }
            if (m.Msg == Native.WM_NCHITTEST) { m.Result = new IntPtr(Native.HTTRANSPARENT); return; }
            if (m.Msg == Native.WM_MOUSEACTIVATE) { m.Result = new IntPtr(Native.MA_NOACTIVATE); return; }
            base.WndProc(ref m);
        }

        void Idle()
        {
            FreeBuffer();
            shown.Clear();
            big = false;
            if (!Native.IsWindows) return;
            // 1x1 transparent pixel: stays "visible" to get DPI notifications, costs nothing
            using (var one = new Bitmap(1, 1, PixelFormat.Format32bppPArgb)) Push(one, Mon.Bounds.L, Mon.Bounds.T, null);
            Native.ShowWindow(Handle, Native.SW_SHOWNA);
        }

        void EnsureBuffer()
        {
            int w = Mon.Bounds.W, h = Mon.Bounds.H;
            if (bmp != null && bw == w && bh == h) return;
            FreeBuffer();
            bw = w;
            bh = h;
            var bi = new Native.BITMAPINFOHEADER { biSize = Marshal.SizeOf(typeof(Native.BITMAPINFOHEADER)), biWidth = w, biHeight = -h, biPlanes = 1, biBitCount = 32 };
            memDC = Native.CreateCompatibleDC(IntPtr.Zero);
            dib = Native.CreateDIBSection(memDC, ref bi, 0, out bits, IntPtr.Zero, 0);
            if (dib == IntPtr.Zero) throw new Win32Exception("CreateDIBSection");
            oldObj = Native.SelectObject(memDC, dib);
            bmp = new Bitmap(w, h, w * 4, PixelFormat.Format32bppPArgb, bits);
        }

        void FreeBuffer()
        {
            if (bmp != null) { bmp.Dispose(); bmp = null; }
            if (memDC != IntPtr.Zero)
            {
                if (oldObj != IntPtr.Zero) Native.SelectObject(memDC, oldObj);
                Native.DeleteDC(memDC);
                memDC = IntPtr.Zero;
            }
            if (dib != IntPtr.Zero) { Native.DeleteObject(dib); dib = IntPtr.Zero; }
            bits = IntPtr.Zero;
        }

        /// <summary>Draws the elements that touch this monitor; only changed areas are repainted and sent.</summary>
        public void Render(List<OverlayElement> all)
        {
            if (!Native.IsWindows) return;
            var mine = new List<OverlayElement>();
            foreach (var e in all) if (e.Bounds.Intersects(Mon.Bounds)) mine.Add(e);
            if (mine.Count == 0) { if (big) Idle(); return; }
            var now = new Dictionary<string, Rc>();
            foreach (var e in mine) now[e.Key] = e.Bounds;
            Rc dirty = Rc.Empty;
            foreach (var kv in now) if (!shown.ContainsKey(kv.Key)) dirty = dirty.Union(kv.Value);
            foreach (var kv in shown) if (!now.ContainsKey(kv.Key)) dirty = dirty.Union(kv.Value);
            if (!big) dirty = Mon.Bounds;
            if (dirty.IsEmpty) return;
            dirty = dirty.Intersect(Mon.Bounds);
            EnsureBuffer();
            using (var g = Graphics.FromImage(bmp))
            {
                g.TranslateTransform(-Mon.Bounds.L, -Mon.Bounds.T);
                g.SetClip(dirty.ToRectangle());
                g.CompositingMode = CompositingMode.SourceCopy;
                using (var clear = new SolidBrush(Color.Transparent)) g.FillRectangle(clear, dirty.ToRectangle());
                g.CompositingMode = CompositingMode.SourceOver;
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                foreach (var e in mine) if (e.Bounds.Intersects(dirty)) { try { e.Draw(g); } catch (Exception ex) { Log.Error("overlay draw", ex); } }
            }
            PushDib(big ? (Rc?)dirty : null);
            if (!big) { Native.SetWindowPos(Handle, Native.HWND_TOPMOST, 0, 0, 0, 0, Native.SWP_NOMOVE | Native.SWP_NOSIZE | Native.SWP_NOACTIVATE); }
            big = true;
            shown = now;
        }

        public void Clear() { if (big) Idle(); }

        void PushDib(Rc? dirtyScreen)
        {
            var dst = new Native.POINT(Mon.Bounds.L, Mon.Bounds.T);
            var size = new Native.SIZE(bw, bh);
            var src = new Native.POINT(0, 0);
            var blend = new Native.BLENDFUNCTION { BlendOp = Native.AC_SRC_OVER, SourceConstantAlpha = 255, AlphaFormat = Native.AC_SRC_ALPHA };
            if (dirtyScreen == null)
            {
                Native.UpdateLayeredWindow(Handle, IntPtr.Zero, ref dst, ref size, memDC, ref src, 0, ref blend, Native.ULW_ALPHA);
                return;
            }
            Rc d = dirtyScreen.Value.Offset(-Mon.Bounds.L, -Mon.Bounds.T);
            IntPtr pDst = Marshal.AllocHGlobal(8), pSize = Marshal.AllocHGlobal(8), pSrc = Marshal.AllocHGlobal(8), pBlend = Marshal.AllocHGlobal(4), pDirty = Marshal.AllocHGlobal(16);
            try
            {
                Marshal.StructureToPtr(dst, pDst, false);
                Marshal.StructureToPtr(size, pSize, false);
                Marshal.StructureToPtr(src, pSrc, false);
                Marshal.StructureToPtr(blend, pBlend, false);
                Marshal.StructureToPtr(new Native.RECT { Left = d.L, Top = d.T, Right = d.R, Bottom = d.B }, pDirty, false);
                var info = new Native.UPDATELAYEREDWINDOWINFO
                {
                    cbSize = Marshal.SizeOf(typeof(Native.UPDATELAYEREDWINDOWINFO)), hdcDst = IntPtr.Zero, pptDst = pDst, psize = pSize,
                    hdcSrc = memDC, pptSrc = pSrc, crKey = 0, pblend = pBlend, dwFlags = Native.ULW_ALPHA, prcDirty = pDirty
                };
                if (!Native.UpdateLayeredWindowIndirect(Handle, ref info))
                    Native.UpdateLayeredWindow(Handle, IntPtr.Zero, ref dst, ref size, memDC, ref src, 0, ref blend, Native.ULW_ALPHA);
            }
            finally
            {
                Marshal.FreeHGlobal(pDst); Marshal.FreeHGlobal(pSize); Marshal.FreeHGlobal(pSrc); Marshal.FreeHGlobal(pBlend); Marshal.FreeHGlobal(pDirty);
            }
        }

        void Push(Bitmap b, int x, int y, Rc? dirty)
        {
            IntPtr screen = IntPtr.Zero;
            IntPtr hb = b.GetHbitmap(Color.FromArgb(0));
            IntPtr dc = Native.CreateCompatibleDC(screen);
            IntPtr old = Native.SelectObject(dc, hb);
            try
            {
                var dst = new Native.POINT(x, y);
                var size = new Native.SIZE(b.Width, b.Height);
                var src = new Native.POINT(0, 0);
                var blend = new Native.BLENDFUNCTION { BlendOp = Native.AC_SRC_OVER, SourceConstantAlpha = 255, AlphaFormat = Native.AC_SRC_ALPHA };
                Native.UpdateLayeredWindow(Handle, IntPtr.Zero, ref dst, ref size, dc, ref src, 0, ref blend, Native.ULW_ALPHA);
            }
            finally
            {
                Native.SelectObject(dc, old);
                Native.DeleteObject(hb);
                Native.DeleteDC(dc);
            }
        }

        public void Dispose()
        {
            FreeBuffer();
            DestroyHandle();
        }
    }

    /// <summary>Drag feedback, flashes and the window-picker highlight, composed per monitor.</summary>
    internal sealed class OverlayManager : IDisposable
    {
        readonly GlimpseApp app;
        readonly List<OverlaySurface> surfaces = new List<OverlaySurface>();
        List<OverlayElement> drag = new List<OverlayElement>();
        List<OverlayElement> flash = new List<OverlayElement>();
        List<OverlayElement> pick = new List<OverlayElement>();
        readonly WinTimer flashTimer = new WinTimer { Interval = 500 };
        long lastRender;

        public bool DragShown { get { return drag.Count > 0; } }

        public OverlayManager(GlimpseApp app)
        {
            this.app = app;
            flashTimer.Tick += delegate { flashTimer.Stop(); flash.Clear(); Compose(); };
            Theme.Refresh();
            OnMonitorsChanged();
        }

        public void OnMonitorsChanged()
        {
            foreach (var s in surfaces) s.Dispose();
            surfaces.Clear();
            if (!Native.IsWindows) return;
            foreach (var m in app.Mons)
            {
                try { surfaces.Add(new OverlaySurface(m, delegate(string why) { app.DisplaySignal(why); }, app.Msg == null ? IntPtr.Zero : app.Msg.Handle)); }
                catch (Exception ex) { Log.Error("overlay", ex); }
            }
        }

        public int DpiAt(Rc r)
        {
            var m = Monitors.At(app.Mons, r);
            return m == null ? 96 : m.Dpi;
        }

        public Rc ScreenAt(Rc r)
        {
            var m = Monitors.At(app.Mons, r);
            return m == null ? Rc.Empty : m.Bounds;
        }

        public void ShowDrag(DragScene scene)
        {
            // at most 30 redraws per second
            if (Clock.Ms - lastRender < 30 && drag.Count > 0) return;
            drag = Elements.ForDrag(scene, DpiAt, ScreenAt);
            Compose();
        }

        public void HideDrag()
        {
            if (drag.Count == 0) return;
            drag = new List<OverlayElement>();
            Compose();
        }

        /// <summary>Each automatic move flashes the moved window's outline for half a second.</summary>
        public void Flash(List<Rc> frames)
        {
            flash = Elements.ForFlash(frames, DpiAt);
            flashTimer.Stop();
            flashTimer.Start();
            Compose();
        }

        public void ShowPick(Rc frame)
        {
            pick = new List<OverlayElement> { Elements.Outline(frame.Inflate(2), DpiAt(frame), Theme.Accent, false, 50, 4) };
            Compose();
        }

        public void HidePick()
        {
            if (pick.Count == 0) return;
            pick = new List<OverlayElement>();
            Compose();
        }

        public void HideAll()
        {
            drag = new List<OverlayElement>();
            flash = new List<OverlayElement>();
            pick = new List<OverlayElement>();
            Compose();
        }

        void Compose()
        {
            lastRender = Clock.Ms;
            var all = new List<OverlayElement>(drag.Count + flash.Count + pick.Count);
            all.AddRange(drag);
            all.AddRange(flash);
            all.AddRange(pick);
            foreach (var s in surfaces)
            {
                try { if (all.Count == 0) s.Clear(); else s.Render(all); }
                catch (Exception ex) { Log.Error("overlay render", ex); }
            }
        }

        public void Dispose()
        {
            flashTimer.Dispose();
            foreach (var s in surfaces) s.Dispose();
            surfaces.Clear();
        }
    }

    #endregion

    #region X-ray: every window's outline and name, through whatever covers it

    internal sealed class XRayEntry
    {
        public TrackedWindow W;
        public Rc Frame;
        public string Title = "";
        public int Number;
        public bool Minimized;
        public Rc Label;   // screen rectangle of its name label (set while painting)
    }

    internal sealed class XRayView : IDisposable
    {
        readonly GlimpseApp app;
        readonly List<XRaySurface> surfaces = new List<XRaySurface>();
        public readonly List<XRayEntry> Entries = new List<XRayEntry>();
        public string Query = "";
        public int Selected = -1, Hover = -1;
        XRaySurface main;
        MonitorInfo mainMon;
        string digits = "";
        readonly WinTimer digitTimer = new WinTimer { Interval = 700 };
        public bool IsOpen { get { return surfaces.Count > 0; } }

        public XRayView(GlimpseApp app)
        {
            this.app = app;
            digitTimer.Tick += delegate { digitTimer.Stop(); PickNumber(true); };
        }

        /// <summary>Builds the list of windows (most recently used first, minimized ones last).</summary>
        public static List<XRayEntry> Collect(WindowRegistry reg, bool showMinimized)
        {
            var list = new List<XRayEntry>();
            IntPtr fg = Native.IsWindows ? Native.GetForegroundWindow() : IntPtr.Zero;
            int n = 0;
            var all = reg.AllByMru();
            foreach (var w in all)
            {
                if (w.Minimized) continue;
                if (w.Handle == fg && (w.Maximized || w.FullScreen)) continue; // it is the backdrop
                list.Add(new XRayEntry { W = w, Frame = w.Frame, Title = w.ShortTitle, Number = ++n });
            }
            if (showMinimized)
                foreach (var w in all)
                    if (w.Minimized) list.Add(new XRayEntry { W = w, Title = w.ShortTitle, Number = ++n, Minimized = true });
            return list;
        }

        public void Show(bool fromKeyboard)
        {
            Close();
            Entries.Clear();
            Entries.AddRange(Collect(app.Reg, app.S.ShowMinimized));
            Query = "";
            digits = "";
            Selected = Entries.Count > 0 ? 0 : -1;
            Hover = -1;
            Native.POINT p;
            Native.GetCursorPos(out p);
            mainMon = Monitors.AtPoint(app.Mons, p.X, p.Y) ?? app.Mons[0];
            foreach (var m in app.Mons)
            {
                var s = new XRaySurface(this, m, app.Msg == null ? IntPtr.Zero : app.Msg.Handle);
                surfaces.Add(s);
                if (m == mainMon) main = s;
            }
            RenderAll();
            foreach (var s in surfaces) s.ShowNow();
            if (main != null) Native.ForceForeground(main.Handle);
            Log.Debug("x-ray opened with " + Entries.Count + " windows" + (fromKeyboard ? " (hotkey)" : ""));
        }

        public void Close()
        {
            digitTimer.Stop();
            foreach (var s in surfaces) s.Dispose();
            surfaces.Clear();
            main = null;
        }

        public bool Owns(IntPtr h)
        {
            foreach (var s in surfaces) if (s.Handle == h) return true;
            return false;
        }

        public void RenderAll()
        {
            foreach (var s in surfaces) s.Render(delegate(Graphics g) { Paint(g, s.Mon, s == main); });
        }

        static bool Matches(string title, string q)
        {
            if (string.IsNullOrEmpty(q)) return true;
            return CultureInfo.CurrentCulture.CompareInfo.IndexOf(title ?? "", q, CompareOptions.IgnoreCase | CompareOptions.IgnoreNonSpace) >= 0;
        }

        public List<int> MatchList()
        {
            var r = new List<int>();
            for (int i = 0; i < Entries.Count; i++)
            {
                var e = Entries[i];
                if (Matches(e.Title, Query) || (e.W != null && Matches(e.W.Process, Query))) r.Add(i);
            }
            return r;
        }

        // ------------------------------------------------------------------ input

        public void OnKey(Keys key, bool shift)
        {
            var m = MatchList();
            switch (key)
            {
                case Keys.Escape: Close(); return;
                case Keys.Enter:
                    if (Selected >= 0 && Selected < Entries.Count && (Query.Length == 0 || m.Contains(Selected))) Activate(Entries[Selected], shift);
                    else if (m.Count > 0) Activate(Entries[m[0]], shift);
                    return;
                case Keys.Back:
                    if (Query.Length > 0) { Query = Query.Substring(0, Query.Length - 1); SelectFirstMatch(); RenderAll(); }
                    return;
                case Keys.Down: case Keys.Right: case Keys.Tab:
                    Step(m, key == Keys.Tab && shift ? -1 : 1);
                    return;
                case Keys.Up: case Keys.Left:
                    Step(m, -1);
                    return;
            }
        }

        void Step(List<int> m, int dir)
        {
            if (m.Count == 0) return;
            int pos = m.IndexOf(Selected);
            pos = pos < 0 ? 0 : (pos + dir + m.Count) % m.Count;
            Selected = m[pos];
            RenderAll();
        }

        public void OnChar(char c)
        {
            if (char.IsControl(c)) return;
            if (Query.Length == 0 && char.IsDigit(c))
            {
                digits += c;
                int count = Entries.Count;
                if (count < 10 || digits.Length >= count.ToString(CultureInfo.InvariantCulture).Length) PickNumber(false);
                else { digitTimer.Stop(); digitTimer.Start(); }
                return;
            }
            Query += c;
            SelectFirstMatch();
            RenderAll();
        }

        void PickNumber(bool timeout)
        {
            digitTimer.Stop();
            int n;
            string d = digits;
            digits = "";
            if (!int.TryParse(d, NumberStyles.Integer, CultureInfo.InvariantCulture, out n)) return;
            foreach (var e in Entries) if (e.Number == n) { Activate(e, Native.KeyDown(Native.VK_SHIFT)); return; }
        }

        void SelectFirstMatch()
        {
            var m = MatchList();
            Selected = m.Count > 0 ? m[0] : -1;
        }

        public void OnClick(int x, int y, bool shift)
        {
            int i = HitTest(x, y);
            if (i < 0) { Close(); return; }
            Activate(Entries[i], shift);
        }

        public void OnMove(int x, int y)
        {
            int i = HitTest(x, y);
            if (i != Hover) { Hover = i; RenderAll(); }
        }

        int HitTest(int x, int y)
        {
            for (int i = 0; i < Entries.Count; i++) if (Entries[i].Label.Contains(x, y)) return i;
            int best = -1;
            long bestArea = long.MaxValue;
            for (int i = 0; i < Entries.Count; i++)
            {
                var e = Entries[i];
                if (e.Minimized || !e.Frame.Contains(x, y)) continue;
                if (e.Frame.Area < bestArea) { bestArea = e.Frame.Area; best = i; }
            }
            return best;
        }

        void Activate(XRayEntry e, bool shift)
        {
            var w = e.W;
            // bring it forward while X-ray still holds the foreground, then close
            app.BringForward(w, shift);
            Close();
        }

        /// <summary>Another program took the focus: close (later, not inside the surface's own message).</summary>
        public void OnDeactivated(IntPtr other)
        {
            if (!IsOpen || Owns(other)) return;
            var ctx = SynchronizationContext.Current;
            if (ctx != null) ctx.Post(delegate { if (IsOpen && !Owns(Native.GetForegroundWindow())) Close(); }, null);
            else Close();
        }

        // ------------------------------------------------------------------ painting

        /// <summary>Paints one monitor of X-ray (also used for the preview images).</summary>
        public void Paint(Graphics g, MonitorInfo mon, bool isMain)
        {
            PaintScene(g, mon, isMain, Entries, Query, Selected, Hover);
        }

        public static void PaintScene(Graphics g, MonitorInfo mon, bool isMain, List<XRayEntry> entries, string query, int selected, int hover)
        {
            float k = mon.Dpi / 96f;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            using (var b = new SolidBrush(Color.FromArgb(160, 22, 22, 30))) g.FillRectangle(b, mon.Bounds.ToRectangle());
            bool filtering = !string.IsNullOrEmpty(query);

            // outlines, bottom of the stack first so the top window's outline stays on top
            for (int i = entries.Count - 1; i >= 0; i--)
            {
                var e = entries[i];
                if (e.Minimized) continue;
                bool hit = !filtering || Matches(e.Title, query) || (e.W != null && Matches(e.W.Process, query));
                bool sel = i == selected || i == hover;
                if (sel) continue;
                var r = new RectangleF(e.Frame.L, e.Frame.T, e.Frame.W, e.Frame.H);
                Draw.Stroke(g, r, 6 * k, Color.FromArgb(hit ? 210 : 70, 214, 214, 224), 1.6f * k, false);
            }
            for (int pass = 0; pass < 2; pass++)
                for (int i = entries.Count - 1; i >= 0; i--)
                {
                    var e = entries[i];
                    if (e.Minimized) continue;
                    bool sel = i == selected || i == hover;
                    if ((pass == 1) != sel) continue;
                    bool hit = !filtering || Matches(e.Title, query) || (e.W != null && Matches(e.W.Process, query));
                    if (sel)
                    {
                        var r = new RectangleF(e.Frame.L, e.Frame.T, e.Frame.W, e.Frame.H);
                        Draw.Fill(g, r, 8 * k, Color.FromArgb(225, 45, 38, 112));
                        Draw.Stroke(g, RectangleF.Inflate(r, 3 * k, 3 * k), 10 * k, Color.FromArgb(120, 124, 108, 242), 6 * k, false);
                        Draw.Stroke(g, r, 8 * k, Color.FromArgb(160, 150, 255), 3 * k, false);
                    }
                    e.Label = LabelAt(g, e, e.Frame.L + (int)(18 * k), e.Frame.T + (int)(16 * k), k, sel, hit ? 255 : 90);
                }
            if (!isMain) return;

            Rc work = mon.Work;
            // search box
            float bw = Math.Min(660 * k, work.W - 40 * k), bh = 64 * k;
            var box = new RectangleF(work.CX - bw / 2, work.T + 40 * k, bw, bh);
            Draw.Fill(g, new RectangleF(box.X, box.Y + 3 * k, box.Width, box.Height), 16 * k, Color.FromArgb(60, 0, 0, 0));
            Draw.Fill(g, box, 16 * k, Color.White);
            Draw.Glyph(g, "search", new RectangleF(box.X + 22 * k, box.Y + (bh - 24 * k) / 2, 24 * k, 24 * k), Color.FromArgb(70, 70, 82));
            var fq = Draw.Font(24 * k, false);
            float tx = box.X + 64 * k, ty = box.Y + (bh - fq.Height) / 2;
            float qw = Draw.Measure(query ?? "", fq).Width;
            Draw.Text(g, query ?? "", fq, Theme.Ink, tx, ty);
            using (var caret = new Pen(Theme.Accent, 2.5f * k)) g.DrawLine(caret, tx + qw + 2 * k, ty + 4 * k, tx + qw + 2 * k, ty + fq.Height - 4 * k);
            var fp = Draw.Font(20 * k, false);
            Draw.Text(g, Texts.T("xray.search"), fp, Color.FromArgb(160, 160, 172), tx + qw + 14 * k, box.Y + (bh - fp.Height) / 2);
            int matches = 0;
            foreach (var e in entries) if (!filtering || Matches(e.Title, query) || (e.W != null && Matches(e.W.Process, query))) matches++;
            var fk = Draw.Font(15 * k, true);
            string enter = Texts.T("xray.enter");
            float ew = Draw.Measure(enter, fk).Width + 20 * k;
            var keyR = new RectangleF(box.Right - ew - 22 * k, box.Y + (bh - 30 * k) / 2, ew, 30 * k);
            Draw.Fill(g, keyR, 6 * k, Color.FromArgb(246, 246, 249));
            Draw.Stroke(g, keyR, 6 * k, Color.FromArgb(205, 205, 214), 1.2f * k, false);
            Draw.Text(g, enter, fk, Theme.Ink, keyR.X + 10 * k, keyR.Y + (keyR.Height - fk.Height) / 2);
            if (filtering)
            {
                var fm = Draw.Font(16 * k, false);
                string mt = matches == 0 ? Texts.T("xray.noMatch") : Texts.P("xray.matches", matches);
                float mw = Draw.Measure(mt, fm).Width;
                Draw.Text(g, mt, fm, Color.FromArgb(110, 110, 124), keyR.X - mw - 16 * k, box.Y + (bh - fm.Height) / 2);
            }

            // minimized windows in a row along the bottom
            var mins = new List<int>();
            for (int i = 0; i < entries.Count; i++) if (entries[i].Minimized) mins.Add(i);
            if (mins.Count > 0)
            {
                var fl = Draw.Font(14 * k, true);
                string label = Texts.T("xray.minimized").ToUpperInvariant();
                float total = Draw.Measure(label, fl).Width + 18 * k;
                var widths = new List<float>();
                foreach (int i in mins) { float w = LabelWidth(entries[i], k); widths.Add(w); total += w + 12 * k; }
                float x = Math.Max(work.L + 20 * k, work.CX - total / 2);
                float y = work.B - 150 * k;
                Draw.Text(g, label, fl, Color.FromArgb(220, 220, 228), x, y + (34 * k - fl.Height) / 2);
                x += Draw.Measure(label, fl).Width + 18 * k;
                for (int j = 0; j < mins.Count; j++)
                {
                    int i = mins[j];
                    var e = entries[i];
                    bool hit = !filtering || Matches(e.Title, query) || (e.W != null && Matches(e.W.Process, query));
                    e.Label = LabelAt(g, e, (int)x, (int)y, k, i == selected || i == hover, hit ? 255 : 90);
                    x += widths[j] + 12 * k;
                    if (x > work.R - 40 * k) break;
                }
            }
            if (entries.Count == 0)
            {
                var fe = Draw.Font(20 * k, false);
                string t = Texts.T("xray.empty");
                var sz = Draw.Measure(t, fe);
                Draw.Text(g, t, fe, Color.White, work.CX - sz.Width / 2, work.CY - sz.Height / 2);
            }
            // hint line
            float hw = Draw.RichText(g, Texts.T("xray.hint"), 17 * k, Color.FromArgb(225, 225, 232), Color.White, 0, 0, false);
            Draw.RichText(g, Texts.T("xray.hint"), 17 * k, Color.FromArgb(225, 225, 232), Color.White, work.CX - hw / 2, work.B - 64 * k, true);
        }

        static float LabelWidth(XRayEntry e, float k)
        {
            var f = Draw.Font(16 * k, true);
            return 10 * k + 26 * k + 10 * k + 20 * k + 8 * k + Math.Min(Draw.Measure(e.Title, f).Width, 420 * k) + 16 * k;
        }

        static Rc LabelAt(Graphics g, XRayEntry e, int x, int y, float k, bool selected, int alpha)
        {
            var f = Draw.Font(16 * k, true);
            float w = LabelWidth(e, k), h = 38 * k;
            var r = new RectangleF(x, y, w, h);
            Draw.Fill(g, r, h / 2, selected ? Theme.Accent : Color.FromArgb(Math.Min(alpha, 230), 44, 44, 56));
            var num = new RectangleF(x + 8 * k, y + (h - 26 * k) / 2, 26 * k, 26 * k);
            Draw.Fill(g, num, 13 * k, selected ? Color.White : Color.FromArgb(alpha, 208, 208, 216));
            var fn = Draw.Font(14 * k, true);
            string ns = e.Number.ToString(CultureInfo.CurrentCulture);
            var nsz = Draw.Measure(ns, fn);
            Draw.Text(g, ns, fn, selected ? Theme.Accent : Color.FromArgb(alpha, 40, 40, 50), num.X + (num.Width - nsz.Width) / 2, num.Y + (num.Height - nsz.Height) / 2);
            var badge = new RectangleF(num.Right + 10 * k, y + (h - 20 * k) / 2, 20 * k, 20 * k);
            Bitmap icon = e.W == null ? null : WindowRegistry.LoadIcon(e.W, (int)(20 * k));
            Draw.AppBadge(g, badge, e.Title, icon);
            Draw.TextIn(g, e.Title, f, Color.FromArgb(alpha, 255, 255, 255), new RectangleF(badge.Right + 8 * k, y + (h - f.Height) / 2, Math.Min(Draw.Measure(e.Title, f).Width, 420 * k) + 2, f.Height + 2));
            return new Rc(x, y, x + (int)w, y + (int)h);
        }

        public void Dispose()
        {
            Close();
            digitTimer.Dispose();
        }
    }

    /// <summary>A focusable full-monitor layered window for X-ray.</summary>
    internal sealed class XRaySurface : NativeWindow, IDisposable
    {
        public readonly MonitorInfo Mon;
        readonly XRayView view;

        public XRaySurface(XRayView view, MonitorInfo mon, IntPtr owner)
        {
            this.view = view;
            Mon = mon;
            var cp = new CreateParams
            {
                Caption = "Glimpse X-ray",
                Parent = owner,
                X = mon.Bounds.L, Y = mon.Bounds.T, Width = mon.Bounds.W, Height = mon.Bounds.H,
                Style = unchecked((int)0x80000000),
                ExStyle = (int)(Native.WS_EX_LAYERED | Native.WS_EX_TOPMOST | Native.WS_EX_TOOLWINDOW)
            };
            CreateHandle(cp);
        }

        public void ShowNow() { Native.ShowWindow(Handle, Native.SW_SHOW); }

        public void Render(Action<Graphics> paint)
        {
            if (!Native.IsWindows) return;
            int w = Mon.Bounds.W, h = Mon.Bounds.H;
            var bi = new Native.BITMAPINFOHEADER { biSize = Marshal.SizeOf(typeof(Native.BITMAPINFOHEADER)), biWidth = w, biHeight = -h, biPlanes = 1, biBitCount = 32 };
            IntPtr bits;
            IntPtr dc = Native.CreateCompatibleDC(IntPtr.Zero);
            IntPtr dib = Native.CreateDIBSection(dc, ref bi, 0, out bits, IntPtr.Zero, 0);
            if (dib == IntPtr.Zero) { Native.DeleteDC(dc); return; }
            IntPtr old = Native.SelectObject(dc, dib);
            try
            {
                using (var bmp = new Bitmap(w, h, w * 4, PixelFormat.Format32bppPArgb, bits))
                using (var g = Graphics.FromImage(bmp))
                {
                    g.TranslateTransform(-Mon.Bounds.L, -Mon.Bounds.T);
                    try { paint(g); } catch (Exception ex) { Log.Error("x-ray paint", ex); }
                }
                var dst = new Native.POINT(Mon.Bounds.L, Mon.Bounds.T);
                var size = new Native.SIZE(w, h);
                var src = new Native.POINT(0, 0);
                var blend = new Native.BLENDFUNCTION { BlendOp = Native.AC_SRC_OVER, SourceConstantAlpha = 255, AlphaFormat = Native.AC_SRC_ALPHA };
                Native.UpdateLayeredWindow(Handle, IntPtr.Zero, ref dst, ref size, dc, ref src, 0, ref blend, Native.ULW_ALPHA);
            }
            finally
            {
                Native.SelectObject(dc, old);
                Native.DeleteObject(dib);
                Native.DeleteDC(dc);
            }
        }

        protected override void WndProc(ref Message m)
        {
            try
            {
                switch (m.Msg)
                {
                    case Native.WM_KEYDOWN:
                    case Native.WM_SYSKEYDOWN:
                        view.OnKey((Keys)m.WParam.ToInt32(), Native.KeyDown(Native.VK_SHIFT));
                        return;
                    case Native.WM_CHAR:
                        view.OnChar((char)m.WParam.ToInt32());
                        return;
                    case Native.WM_LBUTTONUP:
                        {
                            int x = (short)(m.LParam.ToInt64() & 0xFFFF) + Mon.Bounds.L, y = (short)((m.LParam.ToInt64() >> 16) & 0xFFFF) + Mon.Bounds.T;
                            view.OnClick(x, y, Native.KeyDown(Native.VK_SHIFT));
                            return;
                        }
                    case Native.WM_RBUTTONUP:
                        view.Close();
                        return;
                    case Native.WM_MOUSEMOVE:
                        {
                            int x = (short)(m.LParam.ToInt64() & 0xFFFF) + Mon.Bounds.L, y = (short)((m.LParam.ToInt64() >> 16) & 0xFFFF) + Mon.Bounds.T;
                            view.OnMove(x, y);
                            break;
                        }
                    case Native.WM_ACTIVATE:
                        if ((m.WParam.ToInt64() & 0xFFFF) == Native.WA_INACTIVE) view.OnDeactivated(m.LParam);
                        break;
                    case Native.WM_DPICHANGED:
                        m.Result = IntPtr.Zero;
                        return;
                }
            }
            catch (Exception ex) { Log.Error("x-ray input", ex); }
            base.WndProc(ref m);
        }

        public void Dispose() { DestroyHandle(); }
    }

    #endregion

    #region Tray icon and menu, toasts

    internal sealed class TrayUi : IDisposable
    {
        readonly GlimpseApp app;
        readonly NotifyIcon icon = new NotifyIcon();
        ContextMenuStrip menu;
        TrayHeader header;
        ToolStripMenuItem miXRay, miArrange, miUndo, miPause, miResume, miSettings, miExit;
        IntPtr hIcon = IntPtr.Zero;
        string iconKey = "";

        public TrayUi(GlimpseApp app)
        {
            this.app = app;
            icon.MouseUp += delegate(object s, MouseEventArgs e) { if (e.Button == MouseButtons.Left) app.ShowXRay(false); };
            Rebuild();
            icon.Visible = true;
        }

        public void Rebuild()
        {
            var old = menu;
            menu = MakeMenu(app, out header, out miXRay, out miArrange, out miUndo, out miPause, out miResume, out miSettings, out miExit);
            menu.Opening += delegate { Restyle(); Update(); };
            icon.ContextMenuStrip = menu;
            if (old != null) old.Dispose();
            Restyle();
            Update();
        }

        /// <summary>The tray menu: header, X-ray, Arrange now, Undo last fix, Pause, Settings, Exit.</summary>
        public static ContextMenuStrip MakeMenu(GlimpseApp app, out TrayHeader header, out ToolStripMenuItem miXRay, out ToolStripMenuItem miArrange,
            out ToolStripMenuItem miUndo, out ToolStripMenuItem miPause, out ToolStripMenuItem miResume, out ToolStripMenuItem miSettings, out ToolStripMenuItem miExit)
        {
            var menu = new ContextMenuStrip();
            menu.ShowImageMargin = true;
            menu.Renderer = new MenuRenderer();
            header = new TrayHeader();
            miXRay = Item(Texts.T("menu.xray"), "xray", delegate { if (app != null) app.ShowXRay(false); });
            miArrange = Item(Texts.T("menu.arrange"), "arrange", delegate { if (app != null) app.Arrange(); });
            miUndo = Item(Texts.T("menu.undo"), "undo", delegate { if (app != null) app.UndoLast(); });
            miPause = Item(Texts.T("menu.pause"), "pause", null);
            miPause.DropDownItems.Add(Item(Texts.T("menu.pause15"), null, delegate { if (app != null) app.Pause(15, false); }));
            miPause.DropDownItems.Add(Item(Texts.T("menu.pause60"), null, delegate { if (app != null) app.Pause(60, false); }));
            miPause.DropDownItems.Add(Item(Texts.T("menu.pauseTomorrow"), null, delegate { if (app != null) app.Pause(0, true); }));
            ((ToolStripDropDownMenu)miPause.DropDown).Renderer = new MenuRenderer();
            ((ToolStripDropDownMenu)miPause.DropDown).ShowImageMargin = false;
            miResume = Item(Texts.T("menu.resume"), "play", delegate { if (app != null) app.Resume(); });
            miSettings = Item(Texts.T("menu.settings"), "settings", delegate { if (app != null) app.ShowSettings(); });
            miExit = Item(Texts.T("menu.exit"), "exit", delegate { if (app != null) app.ExitApp(); });
            menu.Items.AddRange(new ToolStripItem[] { header, new ToolStripSeparator(), miXRay, miArrange, miUndo, miPause, miResume, new ToolStripSeparator(), miSettings, miExit });
            return menu;
        }

        static ToolStripMenuItem Item(string text, string glyph, EventHandler click)
        {
            var mi = new ToolStripMenuItem(text);
            mi.Tag = glyph;
            if (click != null) mi.Click += click;
            return mi;
        }

        void Restyle()
        {
            int dpi = 96;
            if (Native.IsWindows)
            {
                Native.POINT p;
                Native.GetCursorPos(out p);
                var m = Monitors.AtPoint(app.Mons, p.X, p.Y);
                if (m != null) dpi = m.Dpi;
            }
            Style(menu, header, dpi / 96f);
        }

        /// <summary>Fonts and glyphs for the DPI of the monitor where the menu opens, and the current theme.</summary>
        public static void Style(ContextMenuStrip menu, TrayHeader header, float k)
        {
            Theme.Refresh();
            menu.Font = Draw.Font(14.5f * k, false);
            menu.ImageScalingSize = new Size((int)(18 * k), (int)(18 * k));
            menu.BackColor = Theme.Card;
            header.K = k;
            foreach (ToolStripItem it in menu.Items)
            {
                var mi = it as ToolStripMenuItem;
                if (mi == null) continue;
                mi.Padding = new Padding((int)(4 * k), (int)(7 * k), (int)(4 * k), (int)(7 * k));
                mi.ForeColor = Theme.Text;
                string glyph = mi.Tag as string;
                if (glyph != null)
                {
                    var bmp = new Bitmap((int)(18 * k), (int)(18 * k), PixelFormat.Format32bppPArgb);
                    using (var g = Graphics.FromImage(bmp))
                    {
                        g.SmoothingMode = SmoothingMode.AntiAlias;
                        Draw.Glyph(g, glyph, new RectangleF(0, 0, bmp.Width, bmp.Height), Theme.Text);
                    }
                    if (mi.Image != null) mi.Image.Dispose();
                    mi.Image = bmp;
                }
                foreach (ToolStripItem sub in mi.DropDownItems) { sub.Font = menu.Font; sub.ForeColor = Theme.Text; sub.Padding = mi.Padding; }
                mi.DropDown.BackColor = Theme.Card;
            }
        }

        public string StatusText(out Color dot)
        {
            dot = Theme.Green;
            if (!app.SessionActive) { dot = Color.Gray; return Texts.T("status.idle"); }
            if (app.Paused)
            {
                dot = Color.Gray;
                if (app.PausedUntil.Date > DateTime.Today) return Texts.T("status.pausedTomorrow");
                return Texts.F("status.paused", "time", app.PausedUntil.ToString("t", CultureInfo.CurrentCulture));
            }
            if (app.Suspended) { dot = Theme.Red; return Texts.T("status.suspended"); }
            string s = Texts.P("status.guarding", app.GuardedCount);
            if (app.HiddenCount == 0) s += " \u00b7 " + Texts.T("status.allInReach");
            else { s += " \u00b7 " + Texts.P("status.hidden", app.HiddenCount); dot = Theme.Amber; }
            return s;
        }

        public List<string> Notes()
        {
            var n = new List<string>();
            if (app.S.Mode == FixMode.Guide) n.Add(Texts.T("status.guide"));
            if (app.OverLimit > 0) n.Add(Texts.F("status.overLimit", "count", app.S.MaxWindows));
            if (app.Store.UsingDefaults) n.Add(Texts.T("status.iniDefaults"));
            else if (app.S.Problems.Count > 0) n.Add(Texts.T("status.invalidPattern"));
            if (app.Hotkeys != null && app.Hotkeys.Problem != null) n.Add(Texts.F("status.hotkeyTaken", "hotkey", app.Hotkeys.Problem));
            return n;
        }

        public void Update()
        {
            if (menu == null) return;
            Color dot;
            string status = StatusText(out dot);
            var notes = Notes();
            header.Status = status;
            header.Dot = dot;
            header.Notes = notes;
            header.Invalidate();
            bool warn = app.Suspended || app.Store.UsingDefaults || app.S.Problems.Count > 0 || (app.Hotkeys != null && app.Hotkeys.Problem != null);
            string tip = "Glimpse \u00b7 " + status;
            // .NET Framework's NotifyIcon.Text throws above 63 characters
            icon.Text = tip.Length > 63 ? tip.Substring(0, 62) + "\u2026" : tip;
            miXRay.ShortcutKeyDisplayString = app.Hotkeys != null && app.Hotkeys.Registered ? app.Hotkeys.Current.DisplayText() : "";
            var last = app.Undo.Last;
            miUndo.Enabled = last != null;
            miUndo.ShortcutKeyDisplayString = last == null ? "" : Texts.P("windows", last.Items.Count);
            bool paused = app.Paused || app.Suspended;
            miPause.Visible = !paused;
            miPause.ShortcutKeyDisplayString = Texts.T("menu.pauseHint");
            miResume.Visible = paused;
            miResume.Text = app.Suspended && !app.Paused ? Texts.T("menu.resumeFixes") : Texts.T("menu.resume");
            int state = !app.SessionActive || app.Paused ? 1 : (warn ? 3 : (app.HiddenCount > 0 ? 2 : 0));
            SetIcon(state, app.HiddenCount);
        }

        void SetIcon(int state, int hidden)
        {
            int size = Native.IsWindows ? Native.GetSystemMetrics(Native.SM_CXSMICON) : 16;
            if (size <= 0) size = 16;
            string key = state + "|" + hidden + "|" + size;
            if (key == iconKey) return;
            iconKey = key;
            using (var bmp = RenderIcon(size, state, hidden))
            {
                IntPtr h = bmp.GetHicon();
                var ico = Icon.FromHandle(h);
                icon.Icon = ico;
                if (hIcon != IntPtr.Zero) Native.DestroyIcon(hIcon);
                hIcon = h;
            }
        }

        /// <summary>Tray icon: the Glimpse mark with a state dot (guarding), pause bars (paused),
        /// a count of windows reachable only through X-ray, or a warning.</summary>
        public static Bitmap RenderIcon(int size, int state, int hidden)
        {
            var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                Color front = state == 1 ? Color.FromArgb(140, 140, 150) : Theme.Accent;
                Draw.Logo(g, new RectangleF(0, 0, size, size), front, front, Color.White);
                float d = size * 0.5f;
                var badge = new RectangleF(size - d, size - d, d, d);
                switch (state)
                {
                    case 0:
                        float s = size * 0.36f;
                        Draw.Fill(g, new RectangleF(size - s - 0.5f, size - s - 0.5f, s, s), s / 2, Color.White);
                        Draw.Fill(g, new RectangleF(size - s + size * 0.06f - 0.5f, size - s + size * 0.06f - 0.5f, s - size * 0.12f, s - size * 0.12f), s / 2, Theme.GreenLine);
                        break;
                    case 1:
                        Draw.Fill(g, badge, d / 2, Color.FromArgb(90, 90, 100));
                        using (var b = new SolidBrush(Color.White))
                        {
                            g.FillRectangle(b, badge.X + d * 0.3f, badge.Y + d * 0.25f, d * 0.14f, d * 0.5f);
                            g.FillRectangle(b, badge.X + d * 0.56f, badge.Y + d * 0.25f, d * 0.14f, d * 0.5f);
                        }
                        break;
                    case 2:
                    case 3:
                        Draw.Fill(g, badge, d / 2, state == 2 ? Theme.Amber : Theme.Red);
                        string t = state == 3 ? "!" : (hidden > 9 ? "9+" : hidden.ToString(CultureInfo.InvariantCulture));
                        var f = Draw.Font(d * (t.Length > 1 ? 0.62f : 0.82f), true);
                        var sz = Draw.Measure(t, f);
                        Draw.Text(g, t, f, state == 2 ? Theme.Ink : Color.White, badge.X + (d - sz.Width) / 2, badge.Y + (d - sz.Height) / 2);
                        break;
                }
            }
            return bmp;
        }

        public void Dispose()
        {
            icon.Visible = false;
            icon.Dispose();
            if (menu != null) menu.Dispose();
            if (hIcon != IntPtr.Zero) { Native.DestroyIcon(hIcon); hIcon = IntPtr.Zero; }
        }
    }

    /// <summary>The menu header: the Glimpse mark, name, status dot and line, and any notes.</summary>
    internal sealed class TrayHeader : ToolStripItem
    {
        public float K = 1f;
        public string Status = "";
        public Color Dot = Color.Green;
        public List<string> Notes = new List<string>();

        public override bool CanSelect { get { return false; } }

        public override Size GetPreferredSize(Size constrainingSize)
        {
            var fs = Draw.Font(14.5f * K, false);
            float w = Draw.Measure(Status, fs).Width;
            var fn = Draw.Font(13 * K, false);
            foreach (string n in Notes) w = Math.Max(w, Draw.Measure(n, fn).Width);
            int h = (int)((62 + Notes.Count * 20) * K);
            return new Size((int)(w + 90 * K), h);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            float k = K;
            using (var b = new SolidBrush(Theme.Card)) g.FillRectangle(b, new Rectangle(Point.Empty, Size));
            Draw.Logo(g, new RectangleF(14 * k, 12 * k, 36 * k, 36 * k), Theme.Accent, Theme.Accent, Theme.Card);
            var ft = Draw.Font(17 * k, true);
            Draw.Text(g, "Glimpse", ft, Theme.Text, 64 * k, 10 * k);
            using (var b = new SolidBrush(Dot)) g.FillEllipse(b, 65 * k, 41 * k, 9 * k, 9 * k);
            Draw.Text(g, Status, Draw.Font(14.5f * k, false), Theme.Text, 80 * k, 35 * k);
            float y = 58 * k;
            foreach (string n in Notes)
            {
                Draw.Text(g, n, Draw.Font(13 * k, false), Theme.Sub, 64 * k, y);
                y += 20 * k;
            }
        }
    }

    internal sealed class MenuColors : ProfessionalColorTable
    {
        public override Color ToolStripDropDownBackground { get { return Theme.Card; } }
        public override Color ImageMarginGradientBegin { get { return Theme.Card; } }
        public override Color ImageMarginGradientMiddle { get { return Theme.Card; } }
        public override Color ImageMarginGradientEnd { get { return Theme.Card; } }
        public override Color MenuBorder { get { return Theme.Border; } }
        public override Color MenuItemBorder { get { return Color.Transparent; } }
        public override Color MenuItemSelected { get { return Theme.Hover; } }
        public override Color SeparatorDark { get { return Theme.Border; } }
        public override Color SeparatorLight { get { return Theme.Card; } }
    }

    internal sealed class MenuRenderer : ToolStripProfessionalRenderer
    {
        public MenuRenderer() : base(new MenuColors()) { RoundedEdges = true; }

        protected override void OnRenderMenuItemBackground(ToolStripItemRenderEventArgs e)
        {
            if (!e.Item.Selected || !e.Item.Enabled) return;
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            Draw.Fill(e.Graphics, new RectangleF(4, 1, e.Item.Width - 8, e.Item.Height - 2), 5, Theme.Hover);
        }

        protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e)
        {
            var mi = e.Item as ToolStripMenuItem;
            bool hint = mi != null && !string.IsNullOrEmpty(mi.ShortcutKeyDisplayString) && e.Text == mi.ShortcutKeyDisplayString;
            e.TextColor = !e.Item.Enabled || hint ? Theme.Sub : Theme.Text;
            base.OnRenderItemText(e);
        }

        protected override void OnRenderArrow(ToolStripArrowRenderEventArgs e)
        {
            e.ArrowColor = Theme.Sub;
            base.OnRenderArrow(e);
        }
    }

    /// <summary>A small toast that never takes focus: "Moved 2 windows to keep them in reach - Undo", 6 seconds.</summary>
    internal sealed class Toast : Form
    {
        static Toast current;
        readonly string title, subtitle, actionText;
        readonly Action action;
        readonly float k;
        RectangleF actionRect;
        bool hoverAction;
        readonly WinTimer life = new WinTimer { Interval = 6000 };

        public static void Show(GlimpseApp app, Rc near, string title, string subtitle, string actionText, Action action)
        {
            if (!Native.IsWindows) return;
            try
            {
                if (current != null && !current.IsDisposed) current.Close();
                var t = new Toast(app, near, title, subtitle, actionText, action);
                current = t;
                t.Show();
            }
            catch (Exception ex) { Log.Error("toast", ex); }
        }

        Toast(GlimpseApp app, Rc near, string title, string subtitle, string actionText, Action action)
        {
            this.title = title ?? "";
            this.subtitle = subtitle ?? "";
            this.actionText = actionText;
            this.action = action;
            Theme.Refresh();
            MonitorInfo mon = near.IsEmpty ? null : Monitors.At(app.Mons, near);
            if (mon == null) { foreach (var m in app.Mons) if (m.Primary) mon = m; }
            if (mon == null) mon = app.Mons[0];
            k = mon.Dpi / 96f;
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            TopMost = true;
            StartPosition = FormStartPosition.Manual;
            AutoScaleMode = AutoScaleMode.None;
            DoubleBuffered = true;
            BackColor = Theme.Card;
            Text = "Glimpse toast";   // not shown (no title bar); lets tests and accessibility tools find it
            AccessibleName = this.title;
            Size sz = Measure(this.title, this.subtitle, actionText, k);
            Rc work = mon.Work;
            int x, y, gap = (int)(12 * k);
            if (near.IsEmpty) { x = work.R - sz.Width - (int)(16 * k); y = work.B - sz.Height - (int)(16 * k); }
            else
            {
                x = near.R - sz.Width;
                y = near.B + gap;
                if (y + sz.Height > work.B) y = near.T - sz.Height - gap;
                if (y < work.T) y = near.B - sz.Height - gap;
            }
            x = Util.Clamp(x, work.L + gap, Math.Max(work.L + gap, work.R - sz.Width - gap));
            y = Util.Clamp(y, work.T + gap, Math.Max(work.T + gap, work.B - sz.Height - gap));
            Bounds = new Rectangle(x, y, sz.Width, sz.Height);
            life.Tick += delegate { Close(); };
            life.Start();
        }

        public static Size Measure(string title, string subtitle, string actionText, float k)
        {
            float w = Math.Max(Draw.Measure(title, Draw.Font(16 * k, true)).Width, Math.Min(Draw.Measure(subtitle, Draw.Font(14 * k, false)).Width, 360 * k));
            float bw = string.IsNullOrEmpty(actionText) ? 0 : Draw.Measure(actionText, Draw.Font(15 * k, true)).Width + 28 * k + 16 * k;
            int width = (int)Math.Max(300 * k, Math.Min(620 * k, 20 * k + 34 * k + 16 * k + w + 20 * k + bw));
            int height = (int)((string.IsNullOrEmpty(subtitle) ? 60 : 76) * k);
            return new Size(width, height);
        }

        protected override bool ShowWithoutActivation { get { return true; } }

        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                cp.ExStyle |= (int)(Native.WS_EX_TOOLWINDOW | Native.WS_EX_NOACTIVATE | Native.WS_EX_TOPMOST);
                cp.ClassStyle |= 0x20000; // CS_DROPSHADOW
                return cp;
            }
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Native.SetRoundCorners(Handle, false);
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == Native.WM_MOUSEACTIVATE) { m.Result = new IntPtr(Native.MA_NOACTIVATE); return; }
            base.WndProc(ref m);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            actionRect = PaintToast(e.Graphics, ClientSize, title, subtitle, actionText, hoverAction, k);
        }

        public static RectangleF PaintToast(Graphics g, Size size, string title, string subtitle, string actionText, bool hover, float k)
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            using (var b = new SolidBrush(Theme.Card)) g.FillRectangle(b, 0, 0, size.Width, size.Height);
            using (var pen = new Pen(Theme.Border)) g.DrawRectangle(pen, 0, 0, size.Width - 1, size.Height - 1);
            float x = 18 * k;
            Draw.Logo(g, new RectangleF(x, (size.Height - 32 * k) / 2, 32 * k, 32 * k), Theme.Accent, Theme.Accent, Theme.Card);
            x += 32 * k + 16 * k;
            RectangleF ar = RectangleF.Empty;
            float right = size.Width - 16 * k;
            if (!string.IsNullOrEmpty(actionText))
            {
                var fa = Draw.Font(15 * k, true);
                float bw = Draw.Measure(actionText, fa).Width + 28 * k, bh = 36 * k;
                ar = new RectangleF(right - bw, (size.Height - bh) / 2, bw, bh);
                Draw.Fill(g, ar, 7 * k, hover ? Theme.Accent : Theme.AccentSoft);
                Draw.Text(g, actionText, fa, hover ? Color.White : Theme.AccentText, ar.X + 14 * k, ar.Y + (bh - fa.Height) / 2);
                right = ar.X - 12 * k;
            }
            var ft = Draw.Font(16 * k, true);
            if (string.IsNullOrEmpty(subtitle))
                Draw.TextIn(g, title, ft, Theme.Text, new RectangleF(x, (size.Height - ft.Height) / 2, right - x, ft.Height + 2));
            else
            {
                Draw.TextIn(g, title, ft, Theme.Text, new RectangleF(x, size.Height / 2f - ft.Height - 1 * k, right - x, ft.Height + 2));
                var fs = Draw.Font(14 * k, false);
                Draw.TextIn(g, subtitle, fs, Theme.Sub, new RectangleF(x, size.Height / 2f + 3 * k, right - x, fs.Height + 2));
            }
            return ar;
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            bool h = actionRect.Contains(e.Location);
            if (h != hoverAction) { hoverAction = h; Cursor = h ? Cursors.Hand : Cursors.Default; Invalidate(); }
            base.OnMouseMove(e);
        }

        protected override void OnMouseEnter(EventArgs e) { life.Stop(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { life.Start(); base.OnMouseLeave(e); }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            bool act = actionRect.Contains(e.Location) && action != null;
            Close();
            if (act) { try { action(); } catch (Exception ex) { Log.Error("toast action", ex); } }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) life.Dispose();
            base.Dispose(disposing);
        }
    }

    #endregion

    #region Settings window: custom controls

    internal class GControl : Control
    {
        public float K = 1f;
        protected bool Hot;

        public GControl()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            BackColor = Theme.Card;
        }

        protected override void OnMouseEnter(EventArgs e) { Hot = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { Hot = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

        protected Graphics Begin(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            using (var b = new SolidBrush(BackColor)) g.FillRectangle(b, ClientRectangle);
            return g;
        }

        protected void FocusRing(Graphics g, RectangleF r, float radius)
        {
            if (Focused && ShowFocusCues) Draw.Stroke(g, RectangleF.Inflate(r, -1, -1), radius, Theme.Accent, 1.5f * K, true);
        }
    }

    internal sealed class GLabel : GControl
    {
        public string Label = "";
        public Font F;
        public Color Ink = Color.Black;
        public bool Wrap;

        public GLabel() { SetStyle(ControlStyles.Selectable, false); TabStop = false; }

        public static Size Measure(string text, Font f, int width, bool wrap)
        {
            var flags = TextFormatFlags.NoPrefix | (wrap ? TextFormatFlags.WordBreak : TextFormatFlags.SingleLine);
            Size s = TextRenderer.MeasureText(text ?? "", f, new Size(wrap ? width : int.MaxValue, int.MaxValue), flags);
            return new Size(Math.Min(width, s.Width + 2), s.Height);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var flags = TextFormatFlags.NoPrefix | (Wrap ? TextFormatFlags.WordBreak : TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | TextFormatFlags.VerticalCenter);
            TextRenderer.DrawText(g, Label, F ?? Font, ClientRectangle, Enabled ? Ink : Theme.Sub, flags);
        }
    }

    internal sealed class Toggle : GControl
    {
        bool on;
        public event EventHandler Changed;
        public bool On { get { return on; } set { if (on != value) { on = value; Invalidate(); } } }
        public Toggle() { TabStop = true; Cursor = Cursors.Hand; }

        protected override void OnClick(EventArgs e)
        {
            if (!Enabled) return;
            On = !On;
            Focus();
            var h = Changed;
            if (h != null) h(this, EventArgs.Empty);
            base.OnClick(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space) { OnClick(EventArgs.Empty); e.Handled = true; }
            base.OnKeyDown(e);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            float h = 26 * K, w = 50 * K;
            var r = new RectangleF(Width - w - 2, (Height - h) / 2, w, h);
            Draw.Fill(g, r, h / 2, !Enabled ? Theme.Off : on ? (Hot ? Theme.AccentHover : Theme.Accent) : Theme.Off);
            float d = h - 6 * K;
            float x = on ? r.Right - d - 3 * K : r.X + 3 * K;
            Draw.Fill(g, new RectangleF(x, r.Y + 3 * K, d, d), d / 2, Color.White);
            FocusRing(g, RectangleF.Inflate(r, 3 * K, 3 * K), h / 2 + 3 * K);
        }
    }

    internal sealed class Segmented : GControl
    {
        public readonly List<string> Items = new List<string>();
        public int Selected;
        public event EventHandler Changed;
        public Segmented() { TabStop = true; Cursor = Cursors.Hand; }

        public int PreferredWidth()
        {
            float w = 0;
            foreach (string s in Items) w += Draw.Measure(s, Draw.Font(15 * K, true)).Width + 44 * K;
            return (int)w + 2;
        }

        float[] Edges()
        {
            var e = new float[Items.Count + 1];
            float total = 0;
            var widths = new float[Items.Count];
            for (int i = 0; i < Items.Count; i++) { widths[i] = Draw.Measure(Items[i], Draw.Font(15 * K, true)).Width + 44 * K; total += widths[i]; }
            float scale = total > 0 ? (Width - 2) / total : 1;
            e[0] = 1;
            for (int i = 0; i < Items.Count; i++) e[i + 1] = e[i] + widths[i] * scale;
            return e;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var outer = new RectangleF(1, 1, Width - 3, Height - 3);
            Draw.Fill(g, outer, 8 * K, Theme.Card);
            var edges = Edges();
            for (int i = 0; i < Items.Count; i++)
            {
                var r = new RectangleF(edges[i], 1, edges[i + 1] - edges[i], Height - 3);
                bool sel = i == Selected;
                if (sel)
                {
                    using (var clip = Draw.Round(outer, 8 * K))
                    {
                        var old = g.Clip;
                        g.SetClip(clip, CombineMode.Intersect);
                        using (var b = new SolidBrush(Enabled ? Theme.Accent : Theme.Off)) g.FillRectangle(b, r);
                        g.Clip = old;
                    }
                }
                else if (i > 0 && i - 1 != Selected)
                    using (var pen = new Pen(Theme.Border, 1)) g.DrawLine(pen, r.X, r.Y + 1, r.X, r.Bottom - 1);
                var f = Draw.Font(15 * K, sel);
                var sz = Draw.Measure(Items[i], f);
                Draw.Text(g, Items[i], f, sel ? Color.White : Theme.Text, r.X + (r.Width - sz.Width) / 2, r.Y + (r.Height - sz.Height) / 2);
            }
            Draw.Stroke(g, outer, 8 * K, Theme.Border, 1, false);
            FocusRing(g, RectangleF.Inflate(outer, 2, 2), 9 * K);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            if (!Enabled) return;
            var edges = Edges();
            for (int i = 0; i < Items.Count; i++)
                if (e.X >= edges[i] && e.X < edges[i + 1]) { Pick(i); break; }
            base.OnMouseUp(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Left && Selected > 0) Pick(Selected - 1);
            if (e.KeyCode == Keys.Right && Selected < Items.Count - 1) Pick(Selected + 1);
            base.OnKeyDown(e);
        }

        protected override bool IsInputKey(Keys keyData) { return keyData == Keys.Left || keyData == Keys.Right || base.IsInputKey(keyData); }

        void Pick(int i)
        {
            if (i == Selected) return;
            Selected = i;
            Invalidate();
            var h = Changed;
            if (h != null) h(this, EventArgs.Empty);
        }
    }

    internal sealed class ModeCard : GControl
    {
        public string Title = "", Badge, Desc = "";
        bool chk;
        public bool Checked { get { return chk; } set { chk = value; Invalidate(); } }
        public event EventHandler Picked;
        public ModeCard() { TabStop = true; Cursor = Cursors.Hand; }

        public static int HeightFor(string title, string badge, string desc, int width, float k)
        {
            int top = (int)(46 * k) + (BadgeBelow(title, badge, width, k) ? (int)(28 * k) : 0);
            return top + GLabel.Measure(desc, Draw.Font(14 * k, false), width - (int)(36 * k), true).Height + (int)(16 * k);
        }

        /// <summary>The "Recommended" badge goes below the title when it does not fit beside it.</summary>
        static bool BadgeBelow(string title, string badge, int width, float k)
        {
            if (string.IsNullOrEmpty(badge)) return false;
            float x = 18 * k + Draw.Measure(title, Draw.Font(16 * k, true)).Width + 8 * k;
            float bw = Draw.Measure(badge, Draw.Font(11.5f * k, true)).Width + 12 * k;
            return x + bw > width - 46 * k;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var r = new RectangleF(1.5f * K, 1.5f * K, Width - 3 * K, Height - 3 * K);
            Draw.Fill(g, r, 10 * K, chk ? Theme.AccentSoft : (Hot ? Theme.Hover : Theme.Card));
            Draw.Stroke(g, r, 10 * K, chk ? Theme.Accent : Theme.Border, chk ? 2 * K : 1, false);
            var ft = Draw.Font(16 * K, true);
            Draw.Text(g, Title, ft, Theme.Text, 18 * K, 16 * K);
            bool below = BadgeBelow(Title, Badge, Width, K);
            if (!string.IsNullOrEmpty(Badge))
            {
                var fb = Draw.Font(11.5f * K, true);
                float x = below ? 18 * K : 18 * K + Draw.Measure(Title, ft).Width + 8 * K;
                float y = below ? 16 * K + ft.Height + 6 * K : 16 * K + (ft.Height - 22 * K) / 2;
                float bw = Draw.Measure(Badge, fb).Width + 12 * K;
                var br = new RectangleF(x, y, bw, 22 * K);
                Draw.Fill(g, br, 11 * K, Theme.Dark ? Color.FromArgb(70, 62, 140) : Color.White);
                Draw.Stroke(g, br, 11 * K, Theme.Dark ? Color.FromArgb(120, 110, 220) : Color.FromArgb(205, 200, 250), 1, false);
                Draw.Text(g, Badge, fb, Theme.AccentText, br.X + 6 * K, br.Y + (br.Height - fb.Height) / 2);
            }
            // radio
            float d = 20 * K;
            var rr = new RectangleF(Width - d - 16 * K, 18 * K, d, d);
            using (var pen = new Pen(chk ? Theme.Accent : Theme.Off, 1.6f * K)) g.DrawEllipse(pen, rr);
            if (chk) Draw.Fill(g, RectangleF.Inflate(rr, -5 * K, -5 * K), d, Theme.Accent);
            var fd = Draw.Font(14 * K, false);
            int top = (int)(46 * K) + (below ? (int)(28 * K) : 0);
            var tr = new Rectangle((int)(18 * K), top, Width - (int)(36 * K), Height - top - (int)(4 * K));
            TextRenderer.DrawText(g, Desc, fd, tr, Theme.Sub, TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix);
            FocusRing(g, r, 10 * K);
        }

        protected override void OnClick(EventArgs e)
        {
            if (!Enabled) return;
            Focus();
            var h = Picked;
            if (h != null) h(this, EventArgs.Empty);
            base.OnClick(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) OnClick(EventArgs.Empty);
            base.OnKeyDown(e);
        }
    }

    internal sealed class LinkText : GControl
    {
        public string Label = "";
        public bool Bold = true;
        public event EventHandler Clicked;
        public LinkText() { TabStop = true; Cursor = Cursors.Hand; }

        public int PreferredWidth() { return (int)Draw.Measure(Label, Draw.Font(15 * K, Bold)).Width + (int)(6 * K); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var f = Draw.Font(15 * K, Bold);
            var sz = Draw.Measure(Label, f);
            Color c = Enabled ? Theme.AccentText : Theme.Sub;
            Draw.Text(g, Label, f, c, 2, (Height - sz.Height) / 2);
            if ((Hot || Focused) && Enabled)
                using (var pen = new Pen(c, 1)) g.DrawLine(pen, 2, (Height + sz.Height) / 2, 2 + sz.Width, (Height + sz.Height) / 2);
        }

        protected override void OnClick(EventArgs e)
        {
            if (!Enabled) return;
            var h = Clicked;
            if (h != null) h(this, EventArgs.Empty);
            base.OnClick(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) OnClick(EventArgs.Empty);
            base.OnKeyDown(e);
        }
    }

    internal sealed class KeyCaps : GControl
    {
        public List<string> Keys = new List<string>();
        public KeyCaps() { SetStyle(ControlStyles.Selectable, false); TabStop = false; }

        public int PreferredWidth()
        {
            if (Keys.Count == 0) return (int)Draw.Measure(Texts.T("settings.xray.none"), Draw.Font(15 * K, false)).Width + 4;
            float w = 0;
            foreach (string s in Keys) w += Draw.Measure(s, Draw.Font(15 * K, true)).Width + 26 * K;
            return (int)(w + (Keys.Count - 1) * 24 * K) + 2;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var f = Draw.Font(15 * K, true);
            if (Keys.Count == 0)
            {
                Draw.Text(g, Texts.T("settings.xray.none"), Draw.Font(15 * K, false), Theme.Sub, 0, (Height - f.Height) / 2);
                return;
            }
            float x = 0, h = 34 * K, y = (Height - h) / 2;
            for (int i = 0; i < Keys.Count; i++)
            {
                if (i > 0)
                {
                    var plus = Draw.Font(14 * K, false);
                    Draw.Text(g, "+", plus, Theme.Sub, x + 6 * K, (Height - plus.Height) / 2);
                    x += 24 * K;
                }
                float w = Draw.Measure(Keys[i], f).Width + 26 * K;
                var r = new RectangleF(x, y, w, h);
                Draw.Fill(g, r, 7 * K, Theme.Input);
                Draw.Stroke(g, r, 7 * K, Theme.Border, 1.2f * K, false);
                using (var pen = new Pen(Theme.Border, 2 * K)) g.DrawLine(pen, r.X + 4 * K, r.Bottom - 1, r.Right - 4 * K, r.Bottom - 1);
                Draw.Text(g, Keys[i], f, Theme.Text, r.X + 13 * K, r.Y + (h - f.Height) / 2);
                x += w;
            }
        }
    }

    internal sealed class CornerPicker : GControl
    {
        public Corner Value = Corner.TopLeft;
        public event EventHandler Changed;
        public CornerPicker() { TabStop = true; Cursor = Cursors.Hand; }

        RectangleF Box { get { return new RectangleF(1, 1, Width - 3, Height - 3); } }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var b = Box;
            Draw.Fill(g, b, 6 * K, Theme.Input);
            Draw.Stroke(g, b, 6 * K, Theme.Border, 1.2f * K, false);
            float s = 11 * K, m = 6 * K;
            var corners = new[] { Corner.TopLeft, Corner.TopRight, Corner.BottomLeft, Corner.BottomRight };
            foreach (var c in corners)
            {
                float x = c == Corner.TopLeft || c == Corner.BottomLeft ? b.X + m : b.Right - m - s;
                float y = c == Corner.TopLeft || c == Corner.TopRight ? b.Y + m : b.Bottom - m - s;
                Draw.Fill(g, new RectangleF(x, y, s, s), 2.5f * K, c == Value && Enabled ? Theme.Accent : Theme.Off);
            }
            FocusRing(g, b, 6 * K);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            if (!Enabled) return;
            var b = Box;
            bool left = e.X < b.X + b.Width / 2, top = e.Y < b.Y + b.Height / 2;
            Corner c = top ? (left ? Corner.TopLeft : Corner.TopRight) : (left ? Corner.BottomLeft : Corner.BottomRight);
            Value = c == Value ? Corner.Off : c;
            Invalidate();
            var h = Changed;
            if (h != null) h(this, EventArgs.Empty);
            base.OnMouseUp(e);
        }
    }

    internal sealed class ChipControl : GControl
    {
        public string Label = "";
        public Bitmap Icon;
        public string BadgeName = "";
        public event EventHandler Removed;
        RectangleF xr;
        public ChipControl() { TabStop = true; Cursor = Cursors.Default; }

        public int PreferredWidth() { return (int)(12 * K + 22 * K + 8 * K + Draw.Measure(Label, Draw.Font(15 * K, false)).Width + 10 * K + 18 * K + 10 * K); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var r = new RectangleF(1, 1, Width - 3, Height - 3);
            Draw.Fill(g, r, r.Height / 2, Theme.Input);
            Draw.Stroke(g, r, r.Height / 2, Theme.Border, 1.2f * K, false);
            Draw.AppBadge(g, new RectangleF(12 * K, (Height - 22 * K) / 2, 22 * K, 22 * K), BadgeName, Icon);
            var f = Draw.Font(15 * K, false);
            Draw.Text(g, Label, f, Theme.Text, 42 * K, (Height - f.Height) / 2);
            xr = new RectangleF(Width - 30 * K, (Height - 18 * K) / 2, 18 * K, 18 * K);
            using (var pen = new Pen(Hot ? Theme.Text : Theme.Sub, 1.5f * K))
            {
                float a = 5 * K;
                g.DrawLine(pen, xr.X + a, xr.Y + a, xr.Right - a, xr.Bottom - a);
                g.DrawLine(pen, xr.Right - a, xr.Y + a, xr.X + a, xr.Bottom - a);
            }
            FocusRing(g, r, r.Height / 2);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            if (Enabled && xr.Contains(e.Location)) { var h = Removed; if (h != null) h(this, EventArgs.Empty); }
            base.OnMouseUp(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (Enabled && (e.KeyCode == Keys.Delete || e.KeyCode == Keys.Back)) { var h = Removed; if (h != null) h(this, EventArgs.Empty); }
            base.OnKeyDown(e);
        }
    }

    /// <summary>"Pick a window": press, drag the target cursor onto a window, release. Uses mouse capture, no hook.</summary>
    internal sealed class TargetButton : GControl
    {
        public string Label = "";
        public event Action<Point> Hovering;
        public event Action<Point> Picked;
        bool picking;
        public TargetButton() { TabStop = true; Cursor = Cursors.Hand; }

        public int PreferredWidth() { return (int)(16 * K + 22 * K + 10 * K + Draw.Measure(Label, Draw.Font(15 * K, true)).Width + 18 * K); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var r = new RectangleF(1, 1, Width - 3, Height - 3);
            Draw.Fill(g, r, r.Height / 2, picking || Hot ? Theme.AccentSoft : Theme.Card);
            Draw.Stroke(g, r, r.Height / 2, Theme.Accent, 1.4f * K, true);
            Draw.Glyph(g, "target", new RectangleF(16 * K, (Height - 22 * K) / 2, 22 * K, 22 * K), Theme.AccentText);
            var f = Draw.Font(15 * K, true);
            Draw.Text(g, Label, f, Theme.AccentText, 48 * K, (Height - f.Height) / 2);
            FocusRing(g, r, r.Height / 2);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (!Enabled || e.Button != MouseButtons.Left) return;
            picking = true;
            Capture = true;
            Cursor = Cursors.Cross;
            Invalidate();
            base.OnMouseDown(e);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (picking) { var h = Hovering; if (h != null) h(Control.MousePosition); }
            base.OnMouseMove(e);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            if (picking)
            {
                picking = false;
                Capture = false;
                Cursor = Cursors.Hand;
                Invalidate();
                var h = Picked;
                if (h != null) h(Control.MousePosition);
            }
            base.OnMouseUp(e);
        }
    }

    internal sealed class PrimaryButton : GControl
    {
        public string Label = "";
        public event EventHandler Clicked;
        public PrimaryButton() { TabStop = true; Cursor = Cursors.Hand; }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var r = new RectangleF(1, 1, Width - 3, Height - 3);
            Draw.Fill(g, r, 8 * K, Hot ? Theme.AccentHover : Theme.Accent);
            var f = Draw.Font(16 * K, true);
            var sz = Draw.Measure(Label, f);
            Draw.Text(g, Label, f, Color.White, (Width - sz.Width) / 2, (Height - sz.Height) / 2);
            if (Focused && ShowFocusCues) Draw.Stroke(g, RectangleF.Inflate(r, -3 * K, -3 * K), 6 * K, Color.White, 1.2f * K, true);
        }

        protected override void OnClick(EventArgs e) { var h = Clicked; if (h != null) h(this, EventArgs.Empty); base.OnClick(e); }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) OnClick(EventArgs.Empty);
            base.OnKeyDown(e);
        }
    }

    /// <summary>A text box in a rounded frame; the frame turns red when the content is not valid.</summary>
    internal sealed class InputBox : GControl
    {
        public readonly TextBox Box = new TextBox();
        bool invalid;
        public bool Invalid { get { return invalid; } set { if (invalid != value) { invalid = value; Invalidate(); } } }

        public InputBox(Font f, float k)
        {
            K = k;
            SetStyle(ControlStyles.Selectable, false);
            Box.BorderStyle = BorderStyle.None;
            Box.Font = f;
            Box.BackColor = Theme.Input;
            Box.ForeColor = Theme.Text;
            Controls.Add(Box);
            BackColor = Theme.Card;
        }

        protected override void OnLayout(LayoutEventArgs e)
        {
            int padX = (int)(10 * K);
            Box.SetBounds(padX, Math.Max(1, (Height - Box.PreferredHeight) / 2), Math.Max(10, Width - 2 * padX), Box.PreferredHeight);
            base.OnLayout(e);
        }

        protected override void OnEnabledChanged(EventArgs e) { Box.Enabled = Enabled; base.OnEnabledChanged(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            var r = new RectangleF(1, 1, Width - 3, Height - 3);
            Draw.Fill(g, r, 7 * K, Theme.Input);
            Draw.Stroke(g, r, 7 * K, invalid ? Theme.Red : (Box.Focused ? Theme.Accent : Theme.Border), invalid || Box.Focused ? 1.6f * K : 1.2f * K, false);
        }

        public void Hook() { Box.GotFocus += delegate { Invalidate(); }; Box.LostFocus += delegate { Invalidate(); }; }
    }

    /// <summary>The picture next to the glimpse size: a back window peeking out from behind a front one.</summary>
    internal sealed class SizeIllustration : GControl
    {
        public int Px = 50;
        public SizeIllustration() { SetStyle(ControlStyles.Selectable, false); TabStop = false; }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = Begin(e);
            float w = Width - 2, h = Height - 2;
            float strip = Math.Max(6 * K, Math.Min(h * 0.32f, Px / 72f * h * 0.32f));
            var back = new RectangleF(1, 1, w * 0.62f, h * 0.72f);
            Draw.Fill(g, back, 6 * K, Theme.Card);
            Draw.Stroke(g, back, 6 * K, Theme.Border, 1.2f * K, false);
            Draw.Fill(g, new RectangleF(back.X + 4 * K, back.Y + 4 * K, back.Width * 0.4f, strip * 0.6f), 4 * K, Theme.AccentSoft);
            Draw.Stroke(g, new RectangleF(back.X + 4 * K, back.Y + 4 * K, back.Width * 0.4f, strip * 0.6f), 4 * K, Theme.Accent, 1.4f * K, false);
            var glimpse = new RectangleF(back.X + 4 * K, back.Bottom - strip - 4 * K, strip * 1.3f, strip);
            Draw.Fill(g, glimpse, 4 * K, Theme.AccentSoft);
            Draw.Stroke(g, glimpse, 4 * K, Theme.Accent, 1.4f * K, false);
            var front = new RectangleF(w * 0.3f, h * 0.22f, w * 0.7f, h * 0.78f);
            Draw.Fill(g, front, 6 * K, Theme.Card);
            Draw.Stroke(g, front, 6 * K, Theme.Border, 1.2f * K, false);
            using (var b = new SolidBrush(Theme.Hover)) g.FillRectangle(b, front.X + 1, front.Y + 1, front.Width - 2, 10 * K);
        }
    }

    internal sealed class CardPanel : Panel
    {
        public readonly List<RectangleF> Cards = new List<RectangleF>();
        public float K = 1f;

        public CardPanel()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            AutoScroll = true;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var b = new SolidBrush(Theme.Page)) g.FillRectangle(b, ClientRectangle);
            g.TranslateTransform(AutoScrollPosition.X, AutoScrollPosition.Y);
            foreach (var r in Cards)
            {
                Draw.Fill(g, r, 12 * K, Theme.Card);
                Draw.Stroke(g, r, 12 * K, Theme.Border, 1, false);
            }
        }

        protected override void OnScroll(ScrollEventArgs se) { Invalidate(); base.OnScroll(se); }
        protected override void OnMouseWheel(MouseEventArgs e) { base.OnMouseWheel(e); Invalidate(); }
    }

    #endregion

    #region Settings window: the one page

    internal sealed class SettingsForm : Form
    {
        readonly GlimpseApp app;          // null in preview images
        Settings edit;
        float k = 1f;
        readonly CardPanel content = new CardPanel();
        readonly ToolTip tips = new ToolTip();
        string hkStatus;
        Color hkStatusColor;
        bool recording, addingApp;
        readonly WinTimer testTimer = new WinTimer { Interval = 10000 };
        readonly WinTimer saveTimer = new WinTimer { Interval = 600 };
        IntPtr iconHandle = IntPtr.Zero;
        const int Width100 = 740;
        int M, CW, P, X0, IW, cardTop;
        InputBox focusAfterBuild;

        public SettingsForm(GlimpseApp app) : this(app, app.S.Clone(), 0) { }

        /// <summary>Preview constructor: no running Glimpse behind it.</summary>
        public SettingsForm(Settings s, int dpi) : this(null, s, dpi) { }

        SettingsForm(GlimpseApp app, Settings s, int dpi)
        {
            this.app = app;
            edit = s;
            Theme.Refresh();
            AutoScaleMode = AutoScaleMode.None;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.Manual;
            KeyPreview = true;
            content.Dock = DockStyle.Fill;
            Controls.Add(content);
            testTimer.Tick += delegate { TestTimeout(); };
            saveTimer.Tick += delegate { saveTimer.Stop(); SaveNow(); };
            MonitorInfo mon = null;
            if (app != null) { var p = Control.MousePosition; mon = Monitors.AtPoint(app.Mons, p.X, p.Y); }
            k = (dpi > 0 ? dpi : (mon == null ? 96 : mon.Dpi)) / 96f;
            SetIcon();
            Build();
            if (mon != null)
            {
                Rc work = mon.Work;
                Location = new Point(Math.Max(work.L, work.CX - Width / 2), Math.Max(work.T + S(12), work.CY - Height / 2));
            }
        }

        int S(float v) { return (int)Math.Round(v * k); }
        bool ReadOnly { get { return app != null && (!app.Store.Writable || app.Store.UsingDefaults); } }

        void SetIcon()
        {
            try
            {
                using (var bmp = new Bitmap(32, 32, PixelFormat.Format32bppArgb))
                {
                    using (var g = Graphics.FromImage(bmp))
                    {
                        g.SmoothingMode = SmoothingMode.AntiAlias;
                        Draw.Logo(g, new RectangleF(0, 0, 32, 32), Theme.Accent, Theme.Accent, Color.White);
                    }
                    iconHandle = bmp.GetHicon();
                    Icon = Icon.FromHandle(iconHandle);
                }
            }
            catch { }
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Native.SetDarkTitleBar(Handle, Theme.Dark);
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == Native.WM_DPICHANGED)
            {
                k = (int)(m.WParam.ToInt64() & 0xFFFF) / 96f;
                var r = (Native.RECT)Marshal.PtrToStructure(m.LParam, typeof(Native.RECT));
                Build();
                Location = new Point(r.Left, r.Top);
                m.Result = IntPtr.Zero;
                return;
            }
            base.WndProc(ref m);
        }

        public void ReloadFromApp()
        {
            if (app == null) return;
            edit = app.S.Clone();
            Build();
        }

        T Add<T>(T c, int x, int y, int w, int h) where T : Control
        {
            c.SetBounds(x, y, w, h);
            var gc = c as GControl;
            if (gc != null) gc.K = k;
            content.Controls.Add(c);
            return c;
        }

        GLabel Label(string text, float px, bool bold, Color ink, int x, int y, int w, bool wrap, Color back)
        {
            var l = new GLabel { Label = text ?? "", F = Draw.Font(px * k, bold), Ink = ink, Wrap = wrap, BackColor = back };
            var sz = GLabel.Measure(text, l.F, w, wrap);
            return Add(l, x, y, wrap ? w : Math.Min(w, sz.Width + S(4)), sz.Height);
        }

        void BeginCard(ref int y) { cardTop = y; y += S(20); }

        void EndCard(ref int y)
        {
            y += S(20);
            content.Cards.Add(new RectangleF(M, cardTop, CW, y - cardTop));
            y += S(12);
        }

        static string T(string key) { return Texts.T(key); }

        /// <summary>Lays the page out at the current DPI and in the current language.</summary>
        public void Build()
        {
            SuspendLayout();
            content.SuspendLayout();
            var scroll = content.AutoScrollPosition;
            content.AutoScrollPosition = Point.Empty;
            var old = new List<Control>();
            foreach (Control c in content.Controls) old.Add(c);
            content.Controls.Clear();
            foreach (var c in old) c.Dispose();
            content.Cards.Clear();
            content.K = k;
            BackColor = Theme.Page;
            content.BackColor = Theme.Page;
            Text = T("settings.title");
            Color card = Theme.Card;

            M = S(18); CW = S(Width100) - 2 * M; P = S(22); X0 = M + P; IW = CW - 2 * P;
            int y = S(16);

            // ---- glimpse size
            BeginCard(ref y);
            {
                int illW = S(150), illH = S(86), textW = IW - illW - S(20);
                var t = Label(T("settings.size.title"), 17, true, Theme.Text, X0, y, textW, false, card);
                int yy = y + t.Height + S(2);
                var sub = Label(T("settings.size.subtitle"), 14.5f, false, Theme.Sub, X0, yy, textW, true, card);
                yy += sub.Height + S(14);
                var seg = new Segmented { K = k };
                int[] sizes = { Settings.SmallSize, Settings.MediumSize, Settings.LargeSize };
                seg.Items.Add(Texts.F("settings.size.small", "px", sizes[0]));
                seg.Items.Add(Texts.F("settings.size.medium", "px", sizes[1]));
                seg.Items.Add(Texts.F("settings.size.large", "px", sizes[2]));
                int sel = Array.IndexOf(sizes, edit.GlimpseSize);
                if (sel < 0) { seg.Items.Add(Texts.F("settings.size.custom", "px", edit.GlimpseSize)); sel = 3; }
                seg.Selected = sel;
                var ill = Add(new SizeIllustration { Px = edit.GlimpseSize }, X0 + IW - illW, y, illW, illH);
                Add(seg, X0, yy, Math.Min(textW + S(8), seg.PreferredWidth()), S(42));
                int custom = edit.GlimpseSize;
                seg.Changed += delegate
                {
                    edit.GlimpseSize = seg.Selected < 3 ? sizes[seg.Selected] : custom;
                    ill.Px = edit.GlimpseSize;
                    ill.Invalidate();
                    SaveNow();
                };
                y = Math.Max(yy + S(42), y + illH);
            }
            EndCard(ref y);

            // ---- mode
            BeginCard(ref y);
            {
                var t = Label(T("settings.mode.title"), 17, true, Theme.Text, X0, y, IW, false, card);
                y += t.Height + S(14);
                int gap = S(12), cw = (IW - 2 * gap) / 3;
                var modes = new[] { FixMode.Guide, FixMode.Assist, FixMode.Strict };
                string[] names = { "guide", "assist", "strict" };
                int ch = 0;
                for (int i = 0; i < 3; i++) ch = Math.Max(ch, ModeCard.HeightFor(T("settings.mode." + names[i]), i == 1 ? T("settings.recommended") : null, T("settings.mode." + names[i] + ".desc"), cw, k));
                var cards = new List<ModeCard>();
                for (int i = 0; i < 3; i++)
                {
                    var mc = new ModeCard { Title = T("settings.mode." + names[i]), Desc = T("settings.mode." + names[i] + ".desc"), Badge = i == 1 ? T("settings.recommended") : null, Checked = edit.Mode == modes[i] };
                    Add(mc, X0 + i * (cw + gap), y, cw, ch);
                    cards.Add(mc);
                    FixMode m = modes[i];
                    mc.Picked += delegate
                    {
                        edit.Mode = m;
                        foreach (var c in cards) c.Checked = c == mc;
                        SaveNow();
                    };
                }
                y += ch;
            }
            EndCard(ref y);

            // ---- new windows
            BeginCard(ref y);
            {
                var t = Label(T("settings.new.title"), 17, true, Theme.Text, X0, y, IW, false, card);
                y += t.Height + S(10);
                y = ToggleRow(y, T("settings.new.place"), edit.PlaceNewWindows, delegate(bool v) { edit.PlaceNewWindows = v; });
                y = ToggleRow(y + S(4), T("settings.new.remember"), edit.RememberSizes, delegate(bool v) { edit.RememberSizes = v; });
                y -= S(6);
            }
            EndCard(ref y);

            // ---- minimum size rules
            BeginCard(ref y);
            {
                var add = new LinkText { Label = T("settings.min.add"), K = k, BackColor = card };
                int aw = add.PreferredWidth();
                var t = Label(T("settings.min.title"), 17, true, Theme.Text, X0, y, IW - aw - S(12), false, card);
                Add(add, X0 + IW - aw, y, aw, t.Height);
                add.Clicked += delegate
                {
                    var r = new MinSizeRule { Pattern = "^", MinWidth = 400, MinHeight = 300 };
                    r.Compile();
                    int at = edit.MinSizes.Count;
                    if (at > 0 && edit.MinSizes[at - 1].Pattern == ".*") at--;
                    edit.MinSizes.Insert(at, r);
                    SaveNow();
                    Build();
                };
                y += t.Height + S(2);
                var sub = Label(T("settings.min.subtitle"), 14.5f, false, Theme.Sub, X0, y, IW, true, card);
                y += sub.Height + S(12);
                var mono = Draw.Mono(15 * k);
                var normal = Draw.Font(15 * k, false);
                int rowH = S(38);
                for (int i = 0; i < edit.MinSizes.Count; i++)
                {
                    var rule = edit.MinSizes[i];
                    int x = X0;
                    var pat = Add(new InputBox(mono, k), x, y, S(210), rowH);
                    pat.Box.Text = rule.Pattern;
                    pat.Hook();
                    pat.Invalid = rule.Regex == null;
                    if (rule.Error != null) tips.SetToolTip(pat.Box, Texts.F("settings.min.invalid", "error", rule.Error));
                    tips.SetToolTip(pat, T("settings.min.pattern"));
                    x += S(210) + S(14);
                    var wBox = Add(new InputBox(normal, k), x, y, S(68), rowH);
                    wBox.Box.Text = Util.Inv(rule.MinWidth);
                    wBox.Hook();
                    x += S(68);
                    Label("\u00d7", 15, false, Theme.Sub, x + S(4), y + (rowH - S(20)) / 2, S(22), false, card);
                    x += S(26);
                    var hBox = Add(new InputBox(normal, k), x, y, S(68), rowH);
                    hBox.Box.Text = Util.Inv(rule.MinHeight);
                    hBox.Hook();
                    x += S(68);
                    Label("px", 15, false, Theme.Sub, x + S(6), y + (rowH - S(20)) / 2, S(34), false, card);
                    x += S(40);
                    var rm = new LinkText { Label = "\u00d7", K = k, BackColor = card, Bold = false };
                    int rmW = S(26);
                    var note = Add(new InputBox(normal, k), x, y, X0 + IW - rmW - S(10) - x, rowH);
                    note.Box.Text = rule.Note ?? "";
                    note.Hook();
                    tips.SetToolTip(note.Box, T("settings.min.note"));
                    Add(rm, X0 + IW - rmW, y, rmW, rowH);
                    tips.SetToolTip(rm, T("settings.min.remove"));
                    MinSizeRule r = rule;
                    pat.Box.TextChanged += delegate
                    {
                        r.Pattern = pat.Box.Text;
                        bool ok = r.Compile();
                        pat.Invalid = !ok;
                        tips.SetToolTip(pat.Box, ok ? "" : Texts.F("settings.min.invalid", "error", r.Error));
                        if (ok) ScheduleSave();
                    };
                    EventHandler sizeChanged = delegate
                    {
                        int wv, hv;
                        bool okW = int.TryParse(wBox.Box.Text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out wv) && wv >= 0 && wv <= 10000;
                        bool okH = int.TryParse(hBox.Box.Text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out hv) && hv >= 0 && hv <= 10000;
                        wBox.Invalid = !okW;
                        hBox.Invalid = !okH;
                        if (okW) r.MinWidth = wv;
                        if (okH) r.MinHeight = hv;
                        if (okW && okH) ScheduleSave();
                    };
                    wBox.Box.TextChanged += sizeChanged;
                    hBox.Box.TextChanged += sizeChanged;
                    note.Box.TextChanged += delegate { r.Note = note.Box.Text; ScheduleSave(); };
                    rm.Clicked += delegate { edit.MinSizes.Remove(r); SaveNow(); Build(); };
                    if (rule.Pattern == "^" && focusAfterBuild == null) focusAfterBuild = pat;
                    y += rowH + S(8);
                }
                y -= S(8);
            }
            EndCard(ref y);

            // ---- X-ray
            BeginCard(ref y);
            {
                var t = Label(T("settings.xray.title"), 17, true, Theme.Text, X0, y, IW, false, card);
                y += t.Height + S(12);
                int rowH = S(40);
                Label(T("settings.xray.hotkey"), 15.5f, false, Theme.Text, X0, y + (rowH - S(22)) / 2, S(200), false, card);
                var change = new LinkText { Label = T("settings.xray.change"), K = k, BackColor = card };
                var test = new LinkText { Label = T("settings.xray.test"), K = k, BackColor = card };
                var caps = new KeyCaps { K = k, BackColor = card };
                HotkeySpec spec;
                if (!recording && HotkeySpec.TryParse(edit.Hotkey, out spec)) caps.Keys = spec.DisplayParts();
                int cw = change.PreferredWidth(), tw = test.PreferredWidth(), kw = caps.PreferredWidth();
                int xr = X0 + IW;
                Add(change, xr - cw, y, cw, rowH);
                xr -= cw + S(18);
                Add(test, xr - tw, y, tw, rowH);
                xr -= tw + S(18);
                Add(caps, xr - kw, y, kw, rowH);
                test.Enabled = !string.IsNullOrEmpty(edit.Hotkey) && !recording;
                test.Clicked += delegate { StartTest(); };
                change.Clicked += delegate { ShowHotkeyMenu(change); };
                y += rowH;
                if (!string.IsNullOrEmpty(hkStatus))
                {
                    var st = Label(hkStatus, 14, false, hkStatusColor, X0, y + S(6), IW, true, card);
                    y += st.Height + S(6);
                }
                y += S(12);
                Label(T("settings.xray.corner"), 15.5f, false, Theme.Text, X0, y + (rowH - S(22)) / 2, S(130), false, card);
                var picker = Add(new CornerPicker { Value = edit.HotCorner, BackColor = card }, X0 + S(132), y, S(56), rowH);
                var desc = Label(CornerText(edit.HotCorner), 15, false, Theme.Sub, X0 + S(132) + S(56) + S(14), y + (rowH - S(22)) / 2, S(220), false, card);
                picker.Changed += delegate
                {
                    edit.HotCorner = picker.Value;
                    desc.Label = CornerText(edit.HotCorner);
                    desc.Width = GLabel.Measure(desc.Label, desc.F, S(220), false).Width + S(4);
                    desc.Invalidate();
                    SaveNow();
                };
                string smText = T("settings.xray.minimized");
                int smW = GLabel.Measure(smText, Draw.Font(15.5f * k, false), IW - S(70), false).Width;
                int leftEnd = X0 + S(132) + S(56) + S(14) + GLabel.Measure(desc.Label, desc.F, S(220), false).Width;
                if (leftEnd + S(24) > X0 + IW - S(54) - S(14) - smW) y += rowH + S(4); // no room: its own line
                var showMin = new Toggle { On = edit.ShowMinimized, BackColor = card };
                Add(showMin, X0 + IW - S(54), y + (rowH - S(30)) / 2, S(54), S(30));
                Label(smText, 15.5f, false, Theme.Text, X0 + IW - S(54) - S(14) - smW, y + (rowH - S(22)) / 2, smW + S(4), false, card);
                showMin.Changed += delegate { edit.ShowMinimized = showMin.On; SaveNow(); };
                y += rowH;
            }
            EndCard(ref y);

            // ---- exclusions
            BeginCard(ref y);
            {
                int leftW = S(230);
                var t = Label(T("settings.exclude.title"), 17, true, Theme.Text, X0, y, leftW, true, card);
                var sub = Label(T("settings.exclude.subtitle"), 14.5f, false, Theme.Sub, X0, y + t.Height + S(2), leftW, true, card);
                int leftBottom = y + t.Height + S(2) + sub.Height;
                int fx0 = X0 + leftW + S(16), fx1 = X0 + IW, cx = fx0, cy = y, chipH = S(40), gap = S(10);
                Action<Control, int> place = delegate(Control c, int w)
                {
                    if (cx > fx0 && cx + w > fx1) { cx = fx0; cy += chipH + gap; }
                    Add(c, cx, cy, Math.Min(w, fx1 - fx0), chipH);
                    cx += w + gap;
                };
                foreach (var ex in edit.Exclusions)
                {
                    string label = ex.IsProcess ? ex.Value : FriendlyPattern(ex.Value);
                    var chip = new ChipControl { K = k, BackColor = card, Label = label, BadgeName = label };
                    if (ex.IsProcess) chip.Icon = IconForProcess(ex.Value);
                    else tips.SetToolTip(chip, Texts.F("settings.exclude.titleTip", "pattern", ex.Value));
                    chip.AccessibleName = label;
                    chip.AccessibleDescription = T("settings.exclude.remove") + " (Delete)";
                    var e = ex;
                    chip.Removed += delegate { edit.Exclusions.Remove(e); SaveNow(); Build(); };
                    place(chip, chip.PreferredWidth());
                }
                var pick = new TargetButton { K = k, BackColor = card, Label = T("settings.exclude.pick") };
                tips.SetToolTip(pick, T("settings.exclude.pickHint"));
                pick.Hovering += delegate(Point p) { HoverPick(p); };
                pick.Picked += delegate(Point p) { Pick(p); };
                place(pick, pick.PreferredWidth());
                if (addingApp)
                {
                    var box = new InputBox(Draw.Font(15 * k, false), k);
                    box.Hook();
                    tips.SetToolTip(box.Box, T("settings.exclude.prompt"));
                    place(box, S(200));
                    box.Box.KeyDown += delegate(object s2, KeyEventArgs ke)
                    {
                        if (ke.KeyCode == Keys.Enter) { AddApp(box.Box.Text); ke.SuppressKeyPress = true; }
                        else if (ke.KeyCode == Keys.Escape) { addingApp = false; ke.SuppressKeyPress = true; Build(); }
                    };
                    focusAfterBuild = box;
                }
                else
                {
                    var addLink = new LinkText { K = k, BackColor = card, Label = T("settings.exclude.add") };
                    addLink.Clicked += delegate { addingApp = true; Build(); };
                    place(addLink, addLink.PreferredWidth());
                }
                y = Math.Max(leftBottom, cy + chipH);
            }
            EndCard(ref y);

            // ---- start with Windows, language
            {
                Color page = Theme.Page;
                int rowH = S(40);
                var start = new Toggle { On = edit.StartWithWindows, BackColor = page };
                Add(start, M + S(2), y + (rowH - S(30)) / 2, S(54), S(30));
                start.Changed += delegate { edit.StartWithWindows = start.On; SaveNow(); };
                Label(T("settings.start"), 15.5f, false, Theme.Text, M + S(66), y + (rowH - S(22)) / 2, S(260), false, page);
                var combo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat, Font = Draw.Font(15 * k, false), BackColor = Theme.Input, ForeColor = Theme.Text };
                var langs = Texts.Available();
                var codes = new List<string>();
                combo.Items.Add(Texts.F("settings.language.auto", "language", AutoLanguageName(langs)));
                codes.Add("auto");
                foreach (var kv in langs) { combo.Items.Add(kv.Value); codes.Add(kv.Key); }
                int idx = codes.FindIndex(delegate(string c) { return c.Equals(edit.Language, StringComparison.OrdinalIgnoreCase); });
                combo.SelectedIndex = idx < 0 ? 0 : idx;
                int comboW = S(250);
                Add(combo, M + CW - comboW, y + (rowH - combo.PreferredHeight) / 2, comboW, combo.PreferredHeight);
                string lt = T("settings.language");
                int lw = GLabel.Measure(lt, Draw.Font(15.5f * k, false), S(200), false).Width;
                Label(lt, 15.5f, false, Theme.Text, M + CW - comboW - S(14) - lw, y + (rowH - S(22)) / 2, lw + S(4), false, page);
                combo.SelectedIndexChanged += delegate
                {
                    if (combo.SelectedIndex < 0) return;
                    edit.Language = codes[combo.SelectedIndex];
                    SaveNow();
                    if (app == null) Texts.Load(edit.Language);
                    BeginInvoke(new Action(Build));
                };
                y += rowH + S(16);
            }

            // ---- footer
            {
                Color page = Theme.Page;
                int btnW = S(120), btnH = S(44);
                var done = new PrimaryButton { K = k, BackColor = page, Label = T("settings.done") };
                Add(done, M + CW - btnW, y, btnW, btnH);
                done.Clicked += delegate { Close(); };
                var f = Label(FooterText(), 14.5f, false, ReadOnly ? Theme.Red : Theme.Sub, M + S(2), y, CW - btnW - S(20), true, page);
                f.Top = y + Math.Max(0, (btnH - f.Height) / 2);
                y += Math.Max(btnH, f.Height) + S(18);
                if (ReadOnly) foreach (Control c in content.Controls) if (c != done && !(c is GLabel)) c.Enabled = false;
            }

            content.AutoScrollMinSize = new Size(0, y);
            int maxH = int.MaxValue;
            if (app != null)
            {
                var mon = Monitors.At(app.Mons, new Rc(Left, Top, Left + Width, Top + Height));
                if (mon != null) maxH = mon.Work.H - S(70);
            }
            int clientH = Math.Min(y, maxH);
            ClientSize = new Size(S(Width100) + (y > clientH ? SystemInformation.VerticalScrollBarWidth : 0), clientH);
            content.ResumeLayout();
            ResumeLayout();
            content.AutoScrollPosition = new Point(-scroll.X, -scroll.Y);
            content.Invalidate();
            if (focusAfterBuild != null) { var fb = focusAfterBuild; focusAfterBuild = null; fb.Box.Focus(); fb.Box.SelectAll(); }
        }

        int ToggleRow(int y, string text, bool value, Action<bool> set)
        {
            int rowH = S(34);
            var tg = new Toggle { On = value, BackColor = Theme.Card };
            Add(tg, X0 + IW - S(54), y + (rowH - S(30)) / 2, S(54), S(30));
            var l = Label(text, 15.5f, false, Theme.Text, X0, y, IW - S(70), true, Theme.Card);
            l.Top = y + Math.Max(0, (rowH - l.Height) / 2);
            tg.Changed += delegate { set(tg.On); SaveNow(); };
            return y + Math.Max(rowH, l.Height) + S(6);
        }

        /// <summary>^Calculator$ shows as Calculator; the exact pattern is in the tooltip.</summary>
        static string FriendlyPattern(string p)
        {
            string s = p ?? "";
            if (s.StartsWith("^")) s = s.Substring(1);
            if (s.EndsWith("$") && !s.EndsWith("\\$")) s = s.Substring(0, s.Length - 1);
            return Regex.Replace(s, @"\\([^a-zA-Z0-9])", "$1");
        }

        static string CornerText(Corner c)
        {
            if (c == Corner.Off) return T("settings.xray.cornerOff");
            string name = c == Corner.TopLeft ? "topLeft" : c == Corner.TopRight ? "topRight" : c == Corner.BottomLeft ? "bottomLeft" : "bottomRight";
            return Texts.F("settings.xray.cornerDesc", "corner", T("settings.corner." + name));
        }

        static string AutoLanguageName(List<KeyValuePair<string, string>> langs)
        {
            var ui = Texts.AutoCulture();
            if (ui != null)
                foreach (var kv in langs)
                    if (kv.Key.Equals(ui.Name, StringComparison.OrdinalIgnoreCase) || kv.Key.Equals(ui.TwoLetterISOLanguageName, StringComparison.OrdinalIgnoreCase)) return kv.Value;
            return "English";
        }

        string FooterText()
        {
            if (app == null) return Texts.F("settings.saved", "program", "Glimpse.exe");
            if (app.Store.UsingDefaults) return T("settings.defaults");
            if (!app.Store.Writable) return Texts.F("settings.readOnly", "path", app.Store.FilePath);
            if (app.Store.IsNextToProgram) return Texts.F("settings.saved", "program", Path.GetFileName(Program.LaunchPath));
            return Texts.F("settings.savedPath", "path", app.Store.FilePath);
        }

        Bitmap IconForProcess(string name)
        {
            if (app == null) return null;
            foreach (var w in app.Reg.All.Values)
                if (string.Equals(w.Process, name, StringComparison.OrdinalIgnoreCase) || string.Equals(w.Process, name + ".exe", StringComparison.OrdinalIgnoreCase))
                    return WindowRegistry.LoadIcon(w, S(22));
            return null;
        }

        void ScheduleSave() { saveTimer.Stop(); saveTimer.Start(); }

        void SaveNow()
        {
            saveTimer.Stop();
            if (app == null) return;
            if (!app.SaveSettings(edit)) Build();
        }

        // ---- exclusions: pick a window or type an app name

        TrackedWindow WindowAt(Point p)
        {
            if (app == null || !Native.IsWindows) return null;
            IntPtr h = Native.GetAncestor(Native.WindowFromPoint(new Native.POINT(p.X, p.Y)), Native.GA_ROOT);
            if (h == Handle) return null;
            var w = app.Reg.Get(h);
            if (w == null)
            {
                app.Reg.Refresh(app.S, app.Mons);
                w = app.Reg.Get(h);
            }
            return w;
        }

        void HoverPick(Point p)
        {
            var w = WindowAt(p);
            if (w != null) app.Overlays.ShowPick(w.Frame); else if (app != null) app.Overlays.HidePick();
        }

        void Pick(Point p)
        {
            if (app != null) app.Overlays.HidePick();
            var w = WindowAt(p);
            if (w == null || string.IsNullOrEmpty(w.Process)) return;
            AddApp(w.Process);
        }

        void AddApp(string name)
        {
            name = (name ?? "").Trim().Trim('"');
            addingApp = false;
            if (name.Length > 0)
            {
                bool have = false;
                foreach (var e in edit.Exclusions) if (e.IsProcess && e.Matches(name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name : name + ".exe", "")) have = true;
                if (!have) edit.Exclusions.Add(Exclusion.Process(name));
                SaveNow();
            }
            Build();
        }

        // ---- hotkey: offered list, recording, the three checks

        void ShowHotkeyMenu(Control anchor)
        {
            var menu = new ContextMenuStrip { Renderer = new MenuRenderer(), Font = Draw.Font(15 * k, false), ShowImageMargin = false, ShowCheckMargin = true };
            foreach (string o in HotkeySpec.Offered)
            {
                HotkeySpec s;
                HotkeySpec.TryParse(o, out s);
                var mi = new ToolStripMenuItem(s.DisplayText()) { Checked = string.Equals(o, edit.Hotkey, StringComparison.OrdinalIgnoreCase), ForeColor = Theme.Text };
                HotkeySpec chosen = s;
                mi.Click += delegate { TryHotkey(chosen); };
                menu.Items.Add(mi);
            }
            menu.Items.Add(new ToolStripSeparator());
            var rec = new ToolStripMenuItem(T("settings.xray.record")) { ForeColor = Theme.Text };
            rec.Click += delegate { StartRecording(); };
            menu.Items.Add(rec);
            var none = new ToolStripMenuItem(T("settings.xray.noHotkey")) { Checked = string.IsNullOrEmpty(edit.Hotkey), ForeColor = Theme.Text };
            none.Click += delegate { edit.Hotkey = ""; hkStatus = null; SaveNow(); Build(); };
            menu.Items.Add(none);
            menu.Closed += delegate { BeginInvoke(new Action(menu.Dispose)); };
            menu.Show(anchor, new Point(0, anchor.Height));
        }

        void StartRecording()
        {
            if (app == null) return;
            recording = true;
            app.Hotkeys.Unregister();
            hkStatus = T("settings.xray.recordNow");
            hkStatusColor = Theme.AccentText;
            Build();
            Focus();
        }

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            if (recording)
            {
                Keys key = keyData & Keys.KeyCode;
                if (key == Keys.Escape) { StopRecording(null); return true; }
                if (key == Keys.ControlKey || key == Keys.ShiftKey || key == Keys.Menu || key == Keys.LWin || key == Keys.RWin || key == Keys.LControlKey || key == Keys.RControlKey || key == Keys.LMenu || key == Keys.RMenu || key == Keys.LShiftKey || key == Keys.RShiftKey) return true;
                StopRecording(HotkeySpec.FromKeys(keyData));
                return true;
            }
            if (keyData == Keys.Escape) { Close(); return true; }
            return base.ProcessCmdKey(ref msg, keyData);
        }

        void StopRecording(HotkeySpec? spec)
        {
            recording = false;
            if (spec == null || !spec.Value.IsValid)
            {
                hkStatus = null;
                if (app != null) app.Hotkeys.Apply(edit.Hotkey);
                Build();
                return;
            }
            TryHotkey(spec.Value);
        }

        /// <summary>Check 1: Windows accepts it. Check 2: it types no character on any installed layout.
        /// Then check 3, "Press it now", confirms it reaches this session through the remote client.</summary>
        void TryHotkey(HotkeySpec spec)
        {
            if (app == null) return;
            string shown = spec.DisplayText();
            app.Hotkeys.Unregister();
            bool ok = app.Hotkeys.CanRegister(spec);
            string layout;
            string ch = ok ? spec.TypesCharacter(out layout) : null;
            layout = null;
            if (!ok)
            {
                hkStatus = Texts.F("settings.xray.taken", "hotkey", shown);
                hkStatusColor = Theme.Red;
                app.Hotkeys.Apply(edit.Hotkey);
                Build();
                return;
            }
            if (ch != null)
            {
                string lname;
                spec.TypesCharacter(out lname);
                hkStatus = Texts.F("settings.xray.types", "hotkey", shown, "char", ch, "layout", lname ?? "");
                hkStatusColor = Theme.Red;
                app.Hotkeys.Apply(edit.Hotkey);
                Build();
                return;
            }
            edit.Hotkey = spec.ToString();
            SaveNow();
            StartTest();
        }

        void StartTest()
        {
            if (app == null || string.IsNullOrEmpty(edit.Hotkey)) return;
            HotkeySpec spec;
            if (!HotkeySpec.TryParse(edit.Hotkey, out spec)) return;
            if (!app.Hotkeys.Registered) app.Hotkeys.Apply(edit.Hotkey);
            hkStatus = Texts.F("settings.xray.pressNow", "hotkey", spec.DisplayText());
            hkStatusColor = Theme.AccentText;
            app.Hotkeys.TestArrived = delegate
            {
                testTimer.Stop();
                hkStatus = "\u2713 " + Texts.F("settings.xray.arrived", "hotkey", spec.DisplayText());
                hkStatusColor = Theme.Green;
                if (!IsDisposed) Build();
            };
            testTimer.Stop();
            testTimer.Start();
            Build();
        }

        void TestTimeout()
        {
            testTimer.Stop();
            if (app == null) return;
            app.Hotkeys.TestArrived = null;
            HotkeySpec spec;
            if (!HotkeySpec.TryParse(edit.Hotkey, out spec)) return;
            // suggest the next offered combination
            int i = Array.FindIndex(HotkeySpec.Offered, delegate(string o) { return o.Equals(edit.Hotkey, StringComparison.OrdinalIgnoreCase); });
            string next = HotkeySpec.Offered[(i + 1 + HotkeySpec.Offered.Length) % HotkeySpec.Offered.Length];
            HotkeySpec ns;
            HotkeySpec.TryParse(next, out ns);
            hkStatus = Texts.F("settings.xray.silent", "hotkey", spec.DisplayText(), "next", ns.DisplayText());
            hkStatusColor = Theme.Red;
            Build();
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            if (saveTimer.Enabled) SaveNow();
            testTimer.Stop();
            if (app != null)
            {
                app.Hotkeys.TestArrived = null;
                if (recording || !app.Hotkeys.Registered) app.Hotkeys.Apply(app.S.Hotkey);
                app.Overlays.HidePick();
            }
            base.OnFormClosed(e);
            BeginInvoke(new Action(Dispose));
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                testTimer.Dispose();
                saveTimer.Dispose();
                tips.Dispose();
                if (iconHandle != IntPtr.Zero && Native.IsWindows) Native.DestroyIcon(iconHandle);
                iconHandle = IntPtr.Zero;
            }
            base.Dispose(disposing);
        }
    }

    #endregion

    #region Preview images: Glimpse.exe /render:<folder>

    /// <summary>Renders the drag overlay, X-ray, settings, toast and tray menu with fake windows,
    /// using the same drawing code as the real thing. Used for documentation and visual checks.</summary>
    internal static class Previews
    {
        static readonly Rc Screen = new Rc(0, 0, 1600, 1000);
        static readonly Rc Work = new Rc(0, 0, 1600, 952);

        public static int Render(string folder, string language)
        {
            try
            {
                Directory.CreateDirectory(folder);
                Texts.Load(string.IsNullOrEmpty(language) ? "en" : language);
                Theme.Forced = false;
                Theme.Dark = false;
                Save(DragPreview(), folder, "drag.png");
                Save(ResizePreview(), folder, "resize.png");
                Save(XRayPreview(), folder, "xray.png");
                Save(ToastPreview(), folder, "toast.png");
                Save(TrayIcons(), folder, "tray-icons.png");
                Save(TrayMenuPreview(), folder, "tray-menu.png");
                Save(SettingsPreview(96), folder, "settings.png");
                Save(SettingsPreview(144), folder, "settings-150.png");
                Theme.Forced = true;
                Theme.Dark = true;
                Save(SettingsPreview(96), folder, "settings-dark.png");
                Save(TrayMenuPreview(), folder, "tray-menu-dark.png");
                Save(ToastPreview(), folder, "toast-dark.png");
                Theme.Forced = null;
                Theme.Dark = false;
                Console.WriteLine("previews written to " + Path.GetFullPath(folder));
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("render failed: " + ex);
                return 1;
            }
        }

        static void Save(Bitmap b, string folder, string name)
        {
            if (b == null) return;
            using (b) b.Save(Path.Combine(folder, name), ImageFormat.Png);
            Console.WriteLine("  " + name);
        }

        static TrackedWindow Fake(string title, Rc frame)
        {
            return new TrackedWindow { Title = title, Frame = frame, WindowRect = frame, Counted = true, Resizable = true, HasCaption = true, Process = "app.exe" };
        }

        static void Desktop(Graphics g)
        {
            using (var b = new LinearGradientBrush(Screen.ToRectangle(), Color.FromArgb(28, 52, 82), Color.FromArgb(70, 120, 150), 30f)) g.FillRectangle(b, Screen.ToRectangle());
            using (var b = new SolidBrush(Color.FromArgb(32, 32, 38))) g.FillRectangle(b, new Rectangle(0, Work.B, Screen.W, Screen.B - Work.B));
        }

        /// <summary>A plain window: title bar with badge and caption buttons, skeleton content.</summary>
        public static void FakeWindow(Graphics g, Rc f, string title)
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var r = new RectangleF(f.L, f.T, f.W, f.H);
            Draw.Fill(g, new RectangleF(r.X + 2, r.Y + 4, r.Width, r.Height), 8, Color.FromArgb(50, 0, 0, 0));
            Draw.Fill(g, r, 8, Color.FromArgb(250, 250, 252));
            Draw.Stroke(g, r, 8, Color.FromArgb(205, 205, 214), 1, false);
            using (var b = new SolidBrush(Color.FromArgb(240, 240, 244))) g.FillRectangle(b, r.X + 1, r.Y + 1, r.Width - 2, 38);
            Draw.AppBadge(g, new RectangleF(r.X + 12, r.Y + 11, 18, 18), title, null);
            bool buttons = r.Width >= 300;
            Draw.TextIn(g, title, Draw.Font(14, false), Color.FromArgb(40, 40, 48), new RectangleF(r.X + 38, r.Y + 11, Math.Max(10, r.Width - (buttons ? 190 : 50)), 20));
            if (buttons)
            using (var pen = new Pen(Color.FromArgb(60, 60, 70), 1.2f))
            {
                float y = r.Y + 20;
                g.DrawLine(pen, r.Right - 128, y, r.Right - 118, y);
                g.DrawRectangle(pen, r.Right - 80, y - 5, 10, 10);
                g.DrawLine(pen, r.Right - 32, y - 5, r.Right - 22, y + 5);
                g.DrawLine(pen, r.Right - 22, y - 5, r.Right - 32, y + 5);
            }
            using (var b = new SolidBrush(Color.FromArgb(228, 228, 234)))
                for (float y = r.Y + 62; y < r.Bottom - 20; y += 34)
                {
                    g.FillRectangle(b, r.X + 20, y, Math.Min(90, r.Width / 5), 10);
                    if (r.Width > 260) g.FillRectangle(b, r.X + 30 + Math.Min(90, r.Width / 5), y, Math.Min(200, r.Width / 3), 10);
                    if (r.Width > 520) g.FillRectangle(b, r.X + r.Width * 0.6f, y, Math.Min(140, r.Width / 5), 10);
                }
        }

        static Layout MakeLayout(List<TrackedWindow> list, int dpi)
        {
            var L = new Layout { GlimpseSize = 50 };
            L.Monitors.Add(new GeoMonitor(Screen, Work, dpi));
            foreach (var w in list)
            {
                var g = new GeoWindow { Id = w.Title, Frame = w.Frame, Dpi = dpi, Tag = w, Resizable = true };
                if (w.Elevated) { g.Movable = false; g.FixedReason = "elevated"; }
                L.Windows.Add(g);
            }
            L.Begin();
            return L;
        }

        static Bitmap DragPreview()
        {
            var s = Settings.Defaults();
            var list = new List<TrackedWindow>
            {
                Fake("Order #4711 \u2014 ERP", new Rc(820, 180, 1520, 790)),
                Fake("Invoices \u2014 ERP", new Rc(40, 40, 860, 600)),
                Fake("Inbox \u2014 Outlook", new Rc(40, 640, 760, 940)),
                Fake("Q4 budget.xlsx \u2014 Excel", new Rc(880, 330, 1300, 760)),
                Fake("Services (admin)", new Rc(1250, 420, 1560, 700))
            };
            list[4].Elevated = true;
            var d = new DragState { W = list[0], StartFrame = new Rc(900, 120, 1600, 730), Current = list[0].Frame };
            var L = MakeLayout(list, 96);
            var scene = DragScene.Compute(L, list, 0, d, list[0].Frame, false, true, s, false);
            scene.Cursor = new Native.POINT(1010, 200);
            var bmp = new Bitmap(Screen.W, Screen.H, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(bmp))
            {
                Desktop(g);
                for (int i = list.Count - 1; i >= 1; i--) FakeWindow(g, list[i].Frame, list[i].Title);
                FakeWindow(g, list[0].Frame, list[0].Title);
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                foreach (var e in Elements.ForDrag(scene, delegate(Rc r) { return 96; }, delegate(Rc r) { return Screen; })) e.Draw(g);
            }
            return bmp;
        }

        static Bitmap ResizePreview()
        {
            var s = Settings.Defaults();
            var list = new List<TrackedWindow>
            {
                Fake("Order #4711 \u2014 ERP", new Rc(700, 200, 860, 520)),
                Fake("Invoices \u2014 ERP", new Rc(80, 80, 900, 640))
            };
            var L = MakeLayout(list, 96);
            L.Windows[0].MinW = 480;
            L.Windows[0].MinH = 360;
            s.MinSizes.Insert(0, new MinSizeRule { Pattern = "^Order #\\d+", MinWidth = 480, MinHeight = 360 });
            s.MinSizes[0].Compile();
            var d = new DragState { W = list[0], StartFrame = new Rc(700, 200, 1300, 620), Hit = Native.HTRIGHT, Current = list[0].Frame };
            var scene = DragScene.Compute(L, list, 0, d, list[0].Frame, false, true, s, false);
            scene.Cursor = new Native.POINT(860, 380);
            var bmp = new Bitmap(Screen.W, Screen.H, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(bmp))
            {
                Desktop(g);
                FakeWindow(g, list[1].Frame, list[1].Title);
                FakeWindow(g, list[0].Frame, list[0].Title);
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                foreach (var e in Elements.ForDrag(scene, delegate(Rc r) { return 96; }, delegate(Rc r) { return Screen; })) e.Draw(g);
            }
            return bmp;
        }

        static Bitmap XRayPreview()
        {
            var entries = new List<XRayEntry>
            {
                new XRayEntry { Title = "Inbox \u2014 Outlook", Frame = new Rc(70, 120, 690, 530), Number = 1 },
                new XRayEntry { Title = "Q4 budget.xlsx \u2014 Excel", Frame = new Rc(500, 310, 1150, 740), Number = 2 },
                new XRayEntry { Title = "Order #4711 \u2014 ERP", Frame = new Rc(880, 140, 1520, 590), Number = 3 },
                new XRayEntry { Title = "Services (admin)", Frame = new Rc(1200, 540, 1550, 800), Number = 4 },
                new XRayEntry { Title = "Teams \u2014 Chat", Number = 5, Minimized = true },
                new XRayEntry { Title = "Calculator", Number = 6, Minimized = true }
            };
            var mon = new MonitorInfo { Bounds = Screen, Work = Work, Dpi = 96, Primary = true };
            var bmp = new Bitmap(Screen.W, Screen.H, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(bmp))
            {
                Desktop(g);
                FakeWindow(g, Work, "Invoices \u2014 ERP");
                XRayView.PaintScene(g, mon, true, entries, "bud", 1, -1);
            }
            return bmp;
        }

        static Bitmap ToastPreview()
        {
            Size a = Toast.Measure(Texts.P("toast.moved", 2), "Q4 budget.xlsx, Inbox", Texts.T("toast.undo"), 1f);
            Size b = Toast.Measure(Texts.P("toast.display", 3), "", Texts.T("toast.undo"), 1f);
            Size c = Toast.Measure(Texts.T("toast.layout"), Texts.T("toast.layoutHint"), Texts.T("toast.restore"), 1f);
            int w = Math.Max(a.Width, Math.Max(b.Width, c.Width)) + 40;
            var bmp = new Bitmap(w, a.Height + b.Height + c.Height + 80, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(bmp))
            {
                g.Clear(Color.FromArgb(70, 110, 140));
                int y = 20;
                foreach (var t in new[] { new { S = a, T = Texts.P("toast.moved", 2), U = "Q4 budget.xlsx, Inbox", A = Texts.T("toast.undo") },
                                          new { S = b, T = Texts.P("toast.display", 3), U = "", A = Texts.T("toast.undo") },
                                          new { S = c, T = Texts.T("toast.layout"), U = Texts.T("toast.layoutHint"), A = Texts.T("toast.restore") } })
                {
                    var state = g.Save();
                    g.TranslateTransform(20, y);
                    Toast.PaintToast(g, t.S, t.T, t.U, t.A, false, 1f);
                    g.Restore(state);
                    y += t.S.Height + 20;
                }
            }
            return bmp;
        }

        static Bitmap TrayIcons()
        {
            int[] sizes = { 16, 20, 24, 32, 48 };
            var bmp = new Bitmap(4 * 70 + 20, sizes.Length * 60 + 20, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(bmp))
            {
                g.Clear(Color.FromArgb(32, 32, 38));
                for (int i = 0; i < sizes.Length; i++)
                    for (int st = 0; st < 4; st++)
                        using (var ic = TrayUi.RenderIcon(sizes[i], st, 2)) g.DrawImage(ic, 20 + st * 70, 20 + i * 60);
            }
            return bmp;
        }

        static Bitmap TrayMenuPreview()
        {
            TrayHeader header;
            ToolStripMenuItem x, a, u, p, r, s, e;
            using (var menu = TrayUi.MakeMenu(null, out header, out x, out a, out u, out p, out r, out s, out e))
            {
                TrayUi.Style(menu, header, 1f);
                header.Status = Texts.P("status.guarding", 12) + " \u00b7 " + Texts.T("status.allInReach");
                header.Dot = Theme.Green;
                x.ShortcutKeyDisplayString = "Ctrl+Alt+Space";
                u.ShortcutKeyDisplayString = Texts.P("windows", 2);
                p.ShortcutKeyDisplayString = Texts.T("menu.pauseHint");
                r.Visible = false;
                return Snapshot(menu);
            }
        }

        static Bitmap Snapshot(Control c)
        {
            c.Location = new Point(-20000, -20000);
            if (c is ToolStripDropDown) ((ToolStripDropDown)c).Show(new Point(-20000, -20000));
            else c.Show();
            Application.DoEvents();
            c.PerformLayout();
            Application.DoEvents();
            var bmp = new Bitmap(Math.Max(1, c.Width), Math.Max(1, c.Height));
            if (Native.IsWindows) c.DrawToBitmap(bmp, new Rectangle(0, 0, c.Width, c.Height));
            else using (var g = Graphics.FromImage(bmp)) PaintTree(c, g, true);
            c.Hide();
            return bmp;
        }

        /// <summary>Mono's DrawToBitmap skips child controls; paint the tree by hand (previews only).</summary>
        static void PaintTree(Control c, Graphics g, bool root)
        {
            var state = g.Save();
            if (!root) g.TranslateTransform(c.Left, c.Top);
            g.IntersectClip(new Rectangle(0, 0, c.Width, c.Height));
            var r = new Rectangle(0, 0, c.Width, c.Height);
            var pea = new PaintEventArgs(g, r);
            if (c is TextBox)
            {
                using (var b = new SolidBrush(c.BackColor)) g.FillRectangle(b, r);
                TextRenderer.DrawText(g, c.Text, c.Font, r, c.ForeColor, TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
            }
            else if (c is ComboBox)
            {
                using (var b = new SolidBrush(c.BackColor)) g.FillRectangle(b, r);
                using (var pen = new Pen(Theme.Border)) g.DrawRectangle(pen, 0, 0, c.Width - 1, c.Height - 1);
                TextRenderer.DrawText(g, c.Text, c.Font, new Rectangle(6, 0, c.Width - 30, c.Height), c.ForeColor, TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
                Draw.Glyph(g, "down", new RectangleF(c.Width - 22, (c.Height - 14) / 2f, 14, 14), Theme.Sub);
            }
            else
            {
                var bg = typeof(Control).GetMethod("OnPaintBackground", BindingFlags.Instance | BindingFlags.NonPublic);
                var fg = typeof(Control).GetMethod("OnPaint", BindingFlags.Instance | BindingFlags.NonPublic);
                try { bg.Invoke(c, new object[] { pea }); fg.Invoke(c, new object[] { pea }); }
                catch (Exception) { }
            }
            for (int i = c.Controls.Count - 1; i >= 0; i--) PaintTree(c.Controls[i], g, false);
            g.Restore(state);
        }

        static Bitmap SettingsPreview(int dpi)
        {
            var s = Settings.Defaults();
            s.MinSizes.Insert(0, new MinSizeRule { Pattern = "^Order #\\d+", MinWidth = 480, MinHeight = 360, Note = "ERP order forms" });
            s.MinSizes[0].Compile();
            s.Exclusions.Add(Exclusion.Process("teams.exe"));
            s.Exclusions.Add(Exclusion.Title("^Calculator$"));
            using (var f = new SettingsForm(s, dpi))
            {
                f.ShowInTaskbar = false;
                return Snapshot(f);
            }
        }
    }

    #endregion
}
