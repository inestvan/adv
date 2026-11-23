// SPRUT-M Видео: a native Windows viewer for the SPRUT-M grabber on the Raspberry Pi (no browser).
// One copy only: started again, it brings the open window to the front.
// Options: --fullscreen, --host <address> (default: 169.254.10.1 over the LAN cable, then adv.local).

using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("SPRUT-M Видео")]
[assembly: System.Reflection.AssemblyProduct("SPRUT-M")]
[assembly: System.Reflection.AssemblyVersion("2.0.0.0")]

static class Program
{
	[STAThread]
	static int Main(string[] args)
	{
		bool fs = false;
		string host = null, test = null;
		int testFields = 150;
		for (int i = 0; i < args.Length; i++) {
			if (args[i] == "--fullscreen") fs = true;
			else if (args[i] == "--host" && i + 1 < args.Length) host = args[++i];
			else if (args[i] == "--test" && i + 1 < args.Length) test = args[++i];
			else if (args[i] == "--test-fields" && i + 1 < args.Length) testFields = int.Parse(args[++i]);
			else if (args[i] == "--ini" && i + 1 < args.Length) MainForm.IniOverride = args[++i];
		}
		bool first;
		using (var mutex = new Mutex(true, "Local\\SPRUT-M-Video", out first)) {
			if (!first) { BringToFront(); return 0; }
			try { System.IO.File.Delete(Log.File); } catch { }
			Log.W("start " + string.Join(" ", args));
			AppDomain.CurrentDomain.UnhandledException += (o, e) => Log.W("CRASH: " + e.ExceptionObject);
			Application.ThreadException += (o, e) => Log.W("UI exception: " + e.Exception);
			Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
			timeBeginPeriod(1);		// 1 ms timer resolution: the render thread wakes on time
			try {
				Application.EnableVisualStyles();
				Application.SetCompatibleTextRenderingDefault(false);
				var f = new MainForm(host, fs);
				f.TestDir = test;
				f.TestFields = testFields;
				Application.Run(f);
			} finally { timeEndPeriod(1); }
			return 0;
		}
	}

	[DllImport("winmm.dll")] static extern int timeBeginPeriod(int ms);
	[DllImport("winmm.dll")] static extern int timeEndPeriod(int ms);
	delegate bool EnumProc(IntPtr h, IntPtr l);
	[DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
	[DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
	[DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
	[DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
	[DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int c);
	[DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);

	static void BringToFront()
	{
		IntPtr found = IntPtr.Zero;
		EnumWindows(delegate (IntPtr h, IntPtr l) {
			if (!IsWindowVisible(h)) return true;
			var sb = new StringBuilder(256);
			GetWindowText(h, sb, sb.Capacity);
			if (sb.ToString() == MainForm.AppTitle) { found = h; return false; }
			return true;
		}, IntPtr.Zero);
		if (found == IntPtr.Zero) return;
		if (IsIconic(found)) ShowWindow(found, 9);
		SetForegroundWindow(found);
	}
}

// a small log in %TEMP%\sprutm-video.log (start-up, link, GPU, errors) - for finding faults
static class Log
{
	static readonly object lk = new object();
	public static readonly string File = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "sprutm-video.log");
	public static void W(string s)
	{
		lock (lk) {
			try { System.IO.File.AppendAllText(File, DateTime.Now.ToString("HH:mm:ss.fff") + " " + s + "\r\n"); } catch { }
		}
	}
}
