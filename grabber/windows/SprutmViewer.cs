// SPRUT-M Видео: Windows launcher for the SPRUT-M grabber's web page (grabber/www/index.html on the Pi).
//
// What it does:
//  * one copy only: started again, it brings the open window to the front (two windows on one laptop made both lag;
//    the Pi also gives the video to one window only, see web_take in grabber/web.h);
//  * waits for the Pi: tries http://169.254.10.1/ (LAN cable straight into the laptop) and http://adv.local/
//    (router / Wi-Fi) every 2 s, with a small window saying so;
//  * then opens the page as an app window of Microsoft Edge (or Google Chrome): no tabs, no address bar, its own
//    profile (the page's remembered settings live there), the same WebGL processing as in the browser, and no
//    throttling of timers or rendering when the window is in the background (lower, steadier delay).
//
// Built with the C# compiler of the .NET Framework 4 that every Windows 10/11 has (build.cmd), no other tools.
// Options: --fullscreen (start in full screen; F11 toggles it anyway), --url http://... (a fixed address).

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("SPRUT-M Видео")]
[assembly: System.Reflection.AssemblyProduct("SPRUT-M")]
[assembly: System.Reflection.AssemblyVersion("1.0.0.0")]

static class SprutmViewer
{
	const string Title = "SPRUT-M Видео";
	const string PageTitle = "SPRUT-M";		// the page's <title> starts with it: finds the open window
	static readonly string[] Urls = { "http://169.254.10.1/", "http://adv.local/" };

	[STAThread]
	static int Main(string[] args)
	{
		bool fullscreen = false;
		string fixedUrl = null;
		for (int i = 0; i < args.Length; i++) {
			if (args[i] == "--fullscreen") fullscreen = true;
			else if (args[i] == "--url" && i + 1 < args.Length) fixedUrl = args[++i];
		}

		bool first;
		using (var mutex = new Mutex(true, "Local\\SPRUT-M-Viewer", out first)) {
			if (!first) {
				if (!BringToFront())
					MessageBox.Show("SPRUT-M Видео уже запускается.", Title, MessageBoxButtons.OK, MessageBoxIcon.Information);
				return 0;
			}
			Application.EnableVisualStyles();
			Application.SetCompatibleTextRenderingDefault(false);

			string browser = FindBrowser();
			if (browser == null) {
				MessageBox.Show("Не найден Microsoft Edge или Google Chrome. Откройте в браузере http://169.254.10.1/",
					Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
				return 1;
			}

			string url = fixedUrl;
			if (url == null) {
				var wait = new WaitForm(fixedUrl != null ? new[] { fixedUrl } : Urls);
				Application.Run(wait);
				url = wait.FoundUrl;
				if (url == null)
					return 0;		// closed by the user
			}

			string profile = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
				"SPRUT-M Viewer", "profile");
			Directory.CreateDirectory(profile);
			var a = new StringBuilder();
			a.Append("--app=\"").Append(url).Append("\" ");
			a.Append("--user-data-dir=\"").Append(profile).Append("\" ");
			a.Append("--no-first-run --no-default-browser-check --disable-sync ");
			a.Append("--window-size=1280,860 ");
			// keep rendering at full speed when the window is behind another one or not focused
			a.Append("--disable-background-timer-throttling --disable-renderer-backgrounding ");
			a.Append("--disable-backgrounding-occluded-windows --disable-features=CalculateNativeWinOcclusion ");
			if (fullscreen)
				a.Append("--start-fullscreen ");
			Process p;
			try {
				p = Process.Start(new ProcessStartInfo(browser, a.ToString()) { UseShellExecute = false });
			} catch (Exception e) {
				MessageBox.Show("Не удалось запустить браузер: " + e.Message, Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
				return 1;
			}
			// stay alive (holding the mutex) while the window is open: the browser process of our own profile
			if (p != null)
				p.WaitForExit();
			return 0;
		}
	}

	static string FindBrowser()
	{
		var list = new List<string>();
		foreach (var root in new[] {
			Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
			Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
			Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) }) {
			if (string.IsNullOrEmpty(root)) continue;
			list.Add(Path.Combine(root, @"Microsoft\Edge\Application\msedge.exe"));
			list.Add(Path.Combine(root, @"Google\Chrome\Application\chrome.exe"));
		}
		foreach (var f in list)
			if (File.Exists(f))
				return f;
		return null;
	}

	// ---------------------------------------------------------------- the open window to the front
	delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
	[DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
	[DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hWnd);
	[DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hWnd, StringBuilder s, int n);
	[DllImport("user32.dll")] static extern bool IsIconic(IntPtr hWnd);
	[DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hWnd, int cmd);
	[DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hWnd);

	static bool BringToFront()
	{
		IntPtr found = IntPtr.Zero;
		EnumWindows(delegate (IntPtr h, IntPtr l) {
			if (!IsWindowVisible(h)) return true;
			var sb = new StringBuilder(256);
			GetWindowText(h, sb, sb.Capacity);
			string t = sb.ToString();
			if (t.StartsWith(PageTitle) || t.StartsWith(Title)) { found = h; return false; }
			return true;
		}, IntPtr.Zero);
		if (found == IntPtr.Zero)
			return false;
		if (IsIconic(found))
			ShowWindow(found, 9);	// SW_RESTORE
		SetForegroundWindow(found);
		return true;
	}

	// ---------------------------------------------------------------- "waiting for the Pi" window
	class WaitForm : Form
	{
		public string FoundUrl;
		readonly string[] urls;
		readonly Label status;
		readonly System.Windows.Forms.Timer timer;
		int tries;
		volatile bool busy;

		public WaitForm(string[] urls)
		{
			this.urls = urls;
			Text = Title;
			FormBorderStyle = FormBorderStyle.FixedDialog;
			MaximizeBox = false;
			StartPosition = FormStartPosition.CenterScreen;
			ClientSize = new Size(420, 150);
			BackColor = Color.FromArgb(0x16, 0x18, 0x1c);
			ForeColor = Color.FromArgb(0xe8, 0xea, 0xed);
			Font = new Font("Segoe UI", 10f);
			try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
			var head = new Label { Text = "Подключаюсь к SPRUT-M…", Font = new Font("Segoe UI", 12f, FontStyle.Bold),
				AutoSize = false, Bounds = new Rectangle(20, 16, 380, 28) };
			status = new Label { AutoSize = false, Bounds = new Rectangle(20, 48, 380, 54),
				ForeColor = Color.FromArgb(0x9a, 0xa3, 0xad),
				Text = "Ищу Pi по кабелю (169.254.10.1) и в сети (adv.local)." };
			var cancel = new Button { Text = "Отмена", Bounds = new Rectangle(300, 108, 100, 30),
				FlatStyle = FlatStyle.System };
			cancel.Click += delegate { Close(); };
			Controls.Add(head);
			Controls.Add(status);
			Controls.Add(cancel);
			CancelButton = cancel;
			timer = new System.Windows.Forms.Timer { Interval = 2000 };
			timer.Tick += delegate { Probe(); };
			Shown += delegate { Probe(); timer.Start(); };
		}

		void Probe()
		{
			if (busy) return;
			busy = true;
			tries++;
			ThreadPool.QueueUserWorkItem(delegate {
				string ok = null;
				foreach (var u in urls)
					if (Reachable(u)) { ok = u; break; }
				try {
					BeginInvoke((MethodInvoker)delegate {
						busy = false;
						if (ok != null) { FoundUrl = ok; timer.Stop(); Close(); return; }
						status.Text = "Pi не отвечает (попытка " + tries + "). Проверьте, что Pi включён и кабель LAN " +
							"вставлен; Pi загружается около минуты.";
					});
				} catch (InvalidOperationException) { }	// the window was closed meanwhile
			});
		}

		static bool Reachable(string url)
		{
			try {
				var r = (HttpWebRequest)WebRequest.Create(url);
				r.Timeout = 1500;
				r.ReadWriteTimeout = 1500;
				r.Proxy = null;
				using (var resp = (HttpWebResponse)r.GetResponse())
					return resp.StatusCode == HttpStatusCode.OK;
			} catch {
				return false;
			}
		}
	}
}
