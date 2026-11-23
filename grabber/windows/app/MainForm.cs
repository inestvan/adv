// The window: the video (OpenGL panel drawn by the Renderer thread), a side panel with the page's controls, the
// decoder status and controls from the Pi, a status line. F / F11 / double click: full screen, Esc: back,
// Tab: hide / show the panel, P: snapshot.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;

sealed class VideoPanel : Control
{
	public VideoPanel()
	{
		SetStyle(ControlStyles.Opaque | ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint, true);
		SetStyle(ControlStyles.OptimizedDoubleBuffer, false);
		BackColor = Color.Black;
	}
	protected override CreateParams CreateParams
	{
		get { var cp = base.CreateParams; cp.ClassStyle |= 0x20; return cp; }	// CS_OWNDC: OpenGL keeps its DC
	}
	protected override void OnPaintBackground(PaintEventArgs e) { }
	protected override void OnPaint(PaintEventArgs e) { }
}

// a row of buttons, one of them on (the page's segmented buttons)
sealed class Seg : FlowLayoutPanel
{
	public event Action<float> Changed;
	readonly List<Button> buttons = new List<Button>();
	readonly float[] values;
	public float Value { get; private set; }
	public Seg(string[] labels, float[] values, float current, string tip, ToolTip tt)
	{
		this.values = values;
		AutoSize = true;
		WrapContents = true;
		Margin = new Padding(0, 0, 0, 2);
		for (int i = 0; i < labels.Length; i++) {
			var b = new Button { Text = labels[i], AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
				FlatStyle = FlatStyle.Flat, Margin = new Padding(0, 0, 3, 3), Padding = new Padding(4, 1, 4, 1),
				Tag = values[i], Cursor = Cursors.Hand, TabStop = false };
			b.FlatAppearance.BorderColor = Theme.Line;
			b.Click += (s, e) => { Set((float)((Button)s).Tag); var h = Changed; if (h != null) h(Value); };
			if (tip != null) tt.SetToolTip(b, tip);
			buttons.Add(b);
			Controls.Add(b);
		}
		Set(current);
	}
	public void Set(float v)
	{
		Value = v;
		foreach (var b in buttons) {
			bool on = Math.Abs((float)b.Tag - v) < 1e-4;
			b.BackColor = on ? Theme.Accent : Theme.Panel;
			b.ForeColor = on ? Color.White : Theme.Text;
		}
	}
}

static class Theme
{
	public static readonly Color Bg = Color.FromArgb(0x10, 0x12, 0x14), Panel = Color.FromArgb(0x18, 0x1b, 0x1f),
		Line = Color.FromArgb(0x2a, 0x2f, 0x35), Text = Color.FromArgb(0xe6, 0xe8, 0xeb),
		Muted = Color.FromArgb(0x8b, 0x93, 0x9c), Accent = Color.FromArgb(0x2f, 0x7f, 0xd8),
		Ok = Color.FromArgb(0x3e, 0xcf, 0x8e), Warn = Color.FromArgb(0xf0, 0xb4, 0x29), Bad = Color.FromArgb(0xff, 0x5c, 0x5c);
}

sealed class MainForm : Form
{
	public const string AppTitle = "SPRUT-M Видео";
	readonly ViewSettings S = new ViewSettings();
	readonly PiLink link = new PiLink();
	Renderer rend;
	readonly VideoPanel video = new VideoPanel();
	readonly Panel stage = new Panel(), side = new Panel(), taken = new Panel();
	readonly FlowLayoutPanel flow = new FlowLayoutPanel();
	readonly Label statusLine = new Label(), msg = new Label();
	readonly ToolTip tt = new ToolTip { AutoPopDelay = 20000, InitialDelay = 400 };
	readonly Dictionary<string, Label> cells = new Dictionary<string, Label>();
	readonly PictureBox hist = new PictureBox();
	readonly Label histStats = new Label();
	readonly FlowLayoutPanel ctrlBox = new FlowLayoutPanel();
	Seg combSeg, viewSeg;
	Panel coldRow;
	readonly JavaScriptSerializer json = new JavaScriptSerializer();
	float dpi = 1f;
	bool fullscreen;
	Rectangle normalBounds;
	FormWindowState normalState;
	readonly Timer cursorTimer = new Timer { Interval = 2000 };
	Point lastMouse;
	string connText = "подключение…";
	bool connOk;
	int combId;
	List<Dictionary<string, object>> ctrls = new List<Dictionary<string, object>>();
	public static string IniOverride;	// --ini FILE (tests)
	readonly string iniPath = IniOverride ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
		"SPRUT-M", "video.ini");
	readonly Dictionary<string, string> ini = new Dictionary<string, string>();
	long skippedPrev = -1;
	DateTime skippedT;
	string skipText = "—";
	bool startFullscreen;

	static readonly Dictionary<string, string> RU = new Dictionary<string, string> {
		{ "Brightness", "Яркость" }, { "Contrast", "Контраст" }, { "Saturation", "Насыщенность" }, { "Hue", "Оттенок" },
		{ "Fast Switching", "Быстрое переключение входов" }, { "Test Pattern", "Тестовая картинка" },
		{ "Luma Peaking", "Резкость в декодере (64 — выкл.; лучше «Контуры»)" },
		{ "Auto Levels", "Авто-уровни как в ТВ (если декодер затемняет картинку, например с FPV-приёмника)" } };
	static readonly string[] STD = { "NTSC M/J", "NTSC 4.43", "PAL M", "PAL 60", "PAL B/G/H/I/D", "SECAM", "PAL Comb. N", "SECAM 525" };

	public string TestDir;		// --test DIR (see Renderer.TestDir)
	public int TestFields = 150;

	public MainForm(string fixedHost, bool fs)
	{
		link.FixedHost = fixedHost;
		startFullscreen = fs;
		Text = AppTitle;
		BackColor = Theme.Bg;
		ForeColor = Theme.Text;
		KeyPreview = true;
		try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
		dpi = GetDpiForSystem() / 96f;		// (CreateGraphics here would create the window too early)
		Font = new Font("Segoe UI", 9f);
		MinimumSize = new Size(D(640), D(420));
		LoadIni();
		var wa = Screen.PrimaryScreen.WorkingArea;
		Size = new Size(Math.Min(D(1280), wa.Width * 9 / 10), Math.Min(D(820), wa.Height * 9 / 10));
		StartPosition = FormStartPosition.CenterScreen;
		int x, y, w, h;
		if (int.TryParse(Get("win.x", ""), out x) && int.TryParse(Get("win.y", ""), out y) &&
		    int.TryParse(Get("win.w", ""), out w) && int.TryParse(Get("win.h", ""), out h)) {
			var r = new Rectangle(x, y, w, h);
			foreach (var sc in Screen.AllScreens)
				if (sc.WorkingArea.IntersectsWith(r)) { StartPosition = FormStartPosition.Manual; Bounds = r; break; }
		}
		if (Get("win.max", "0") == "1") WindowState = FormWindowState.Maximized;

		// ---- layout: stage (video + status line) | side panel
		side.Dock = DockStyle.Right;
		side.Width = D(CW + 16) + SystemInformation.VerticalScrollBarWidth + 4;
		side.BackColor = Theme.Bg;
		side.Padding = new Padding(D(8), D(8), D(4), D(8));
		flow.Dock = DockStyle.Fill;
		flow.FlowDirection = FlowDirection.TopDown;
		flow.WrapContents = false;
		flow.AutoScroll = true;
		flow.BackColor = Theme.Bg;
		side.Controls.Add(flow);

		stage.Dock = DockStyle.Fill;
		stage.BackColor = Color.Black;
		statusLine.Dock = DockStyle.Bottom;
		statusLine.Height = D(24);
		statusLine.BackColor = Theme.Panel;
		statusLine.ForeColor = Theme.Muted;
		statusLine.TextAlign = ContentAlignment.MiddleLeft;
		statusLine.Padding = new Padding(D(8), 0, 0, 0);
		video.Dock = DockStyle.Fill;
		msg.Dock = DockStyle.Fill;
		msg.TextAlign = ContentAlignment.MiddleCenter;
		msg.ForeColor = Theme.Muted;
		msg.BackColor = Color.Black;
		msg.Font = new Font("Segoe UI", 12f);
		msg.Text = "Подключаюсь к Pi…";
		BuildTaken();
		stage.Controls.Add(video);
		stage.Controls.Add(msg);
		stage.Controls.Add(taken);
		stage.Controls.Add(statusLine);
		msg.BringToFront();
		Controls.Add(stage);
		// no settings in the window: the picture is the reference (ViewSettings defaults); the side panel with the
		// settings, the decoder status and controls is built but not shown (Controls.Add(side) left out on purpose)
		BuildSide();
		side.Visible = false;

		video.DoubleClick += (s, e) => ToggleFullscreen();
		msg.DoubleClick += (s, e) => ToggleFullscreen();
		MouseEventHandler mm = (s, e) => { var p = Cursor.Position; if (p != lastMouse) { lastMouse = p; ShowCursor(); } };
		video.MouseMove += mm; msg.MouseMove += mm; taken.MouseMove += mm;
		foreach (Control c in taken.Controls) c.MouseMove += mm;
		cursorTimer.Tick += (s, e) => {
			cursorTimer.Stop();
			if (fullscreen && !side.Visible && !taken.Visible && !cursorHidden) { Cursor.Hide(); cursorHidden = true; }
		};

		link.Field += f => { var r = rend; if (r != null) r.Push(f); else link.Recycle(f); };
		link.Text += t => Ui(() => OnText(t));
		link.State += (t, ok) => Ui(() => OnLinkState(t, ok));
	}

	// run on the UI thread; ignored while the window is closing
	void Ui(Action a)
	{
		if (IsDisposed || Disposing || !IsHandleCreated) return;
		try { BeginInvoke(a); } catch (InvalidOperationException) { }	// (also ObjectDisposedException)
	}

	bool cursorHidden;
	void ShowCursor()
	{
		if (cursorHidden) { Cursor.Show(); cursorHidden = false; }
		if (fullscreen) { cursorTimer.Stop(); cursorTimer.Start(); }
	}

	int D(int px) { return (int)Math.Round(px * dpi); }
	[DllImport("user32.dll")] static extern int GetDpiForSystem();
	const int CW = 300;		// width of the side panel content (before DPI scaling)

	[DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int val, int size);
	[DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
	[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
	struct DEVMODE
	{
		[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
		public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields;
		public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
		public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
		[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
		public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
		public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
	}
	int ScreenHz()
	{
		try {
			var dm = new DEVMODE();
			dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
			if (EnumDisplaySettings(Screen.FromControl(this).DeviceName, -1, ref dm)) return dm.dmDisplayFrequency;
		} catch { }
		return 0;
	}
	int screenHz;

	protected override void OnHandleCreated(EventArgs e)
	{
		base.OnHandleCreated(e);
		int on = 1;
		try { DwmSetWindowAttribute(Handle, 20, ref on, 4); } catch { }	// dark title bar (Windows 10 2004+ / 11)
	}

	protected override void OnMove(EventArgs e) { base.OnMove(e); screenHz = 0; }

	protected override void OnShown(EventArgs e)
	{
		base.OnShown(e);
		rend = new Renderer(video.Handle, S, link);
		rend.Failed += m => Ui(() => {
			MessageBox.Show(this, "Не удалось запустить вывод через видеокарту:\n" + m, AppTitle, MessageBoxButtons.OK,
				MessageBoxIcon.Error);
			Close();
		});
		rend.StatsReady += () => Ui(UpdateStats);
		rend.Snapshot += f => Ui(() => {
			if (f.StartsWith("!")) { snapNote = "снимок не сохранён: " + f.Substring(1); snapT = DateTime.Now; }
			else { snapNote = "снимок: " + f; snapT = DateTime.Now; }
			UpdateStatusLine();
		});
		if (TestDir != null) {
			rend.TestDir = TestDir;
			rend.TestFields = TestFields;
			rend.TestDone += () => Ui(Close);
		}
		rend.Start();
		link.Start();
		if (startFullscreen) ToggleFullscreen();
	}

	string snapNote;
	DateTime snapT;

	protected override void OnResize(EventArgs e) { base.OnResize(e); if (rend != null) rend.Redraw(); }

	protected override void OnFormClosing(FormClosingEventArgs e)
	{
		if (fullscreen) ToggleFullscreen();
		Put("win.max", WindowState == FormWindowState.Maximized ? "1" : "0");
		var b = WindowState == FormWindowState.Normal ? Bounds : RestoreBounds;
		Put("win.x", b.X.ToString()); Put("win.y", b.Y.ToString()); Put("win.w", b.Width.ToString()); Put("win.h", b.Height.ToString());
		SaveIni();
		link.Stop();
		if (rend != null) rend.Stop();
		base.OnFormClosing(e);
	}

	protected override bool ProcessCmdKey(ref Message m, Keys k)
	{
		switch (k) {
		case Keys.F: case Keys.F11: ToggleFullscreen(); return true;
		case Keys.Escape: if (fullscreen) { ToggleFullscreen(); return true; } break;
		case Keys.P: if (rend != null) rend.RequestSnapshot(); return true;
		}
		return base.ProcessCmdKey(ref m, k);
	}

	bool panelBeforeFs;
	void ToggleFullscreen()
	{
		SuspendLayout();
		if (!fullscreen) {
			normalState = WindowState;
			normalBounds = WindowState == FormWindowState.Normal ? Bounds : RestoreBounds;
			panelBeforeFs = side.Visible;
			var sc = Screen.FromControl(this).Bounds;
			FormBorderStyle = FormBorderStyle.None;
			WindowState = FormWindowState.Normal;
			Bounds = sc;
			side.Visible = false;
			statusLine.Visible = false;
			fullscreen = true;
			cursorTimer.Start();
		} else {
			fullscreen = false;
			FormBorderStyle = FormBorderStyle.Sizable;
			Bounds = normalBounds;
			WindowState = normalState;
			side.Visible = panelBeforeFs;
			statusLine.Visible = true;
			cursorTimer.Stop();
			ShowCursor();
		}
		ResumeLayout();
		if (rend != null) { rend.Fullscreen = fullscreen; rend.Redraw(); }
	}

	// ---------------------------------------------------------------- side panel
	Label Head(string t)
	{
		return new Label { Text = t.ToUpper(), AutoSize = true, ForeColor = Theme.Muted, Font = new Font("Segoe UI", 8.5f, FontStyle.Bold),
			Margin = new Padding(0, D(10), 0, D(4)) };
	}
	Control Row(string label, Control c)
	{
		var p = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Margin = new Padding(0, 0, 0, D(2)), Width = D(CW) };
		p.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, D(92)));
		p.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, D(CW - 92)));
		var l = new Label { Text = label, ForeColor = Theme.Muted, AutoSize = false, Width = D(90), Height = D(24),
			TextAlign = ContentAlignment.MiddleLeft, Margin = new Padding(0) };
		c.MaximumSize = new Size(D(CW - 92), 0);
		p.Controls.Add(l, 0, 0);
		p.Controls.Add(c, 1, 0);
		return p;
	}

	Seg MakeSeg(string key, string[] labels, float[] vals, float def, string tip, Action<float> set)
	{
		float v;
		v = def;			// the reference; settings files of older versions are ignored
		set(v);
		var s = new Seg(labels, vals, v, tip, tt);
		s.Changed += x => { set(x); if (rend != null) rend.Redraw(); };
		return s;
	}

	void BuildSide()
	{
		flow.Controls.Add(Head("Картинка"));
		flow.Controls.Add(Row("Деинтерлейс", MakeSeg("mode", new[] { "Авто 50", "Bob 50", "Weave 25", "Поле ×2" },
			new[] { 3f, 0f, 1f, 2f }, 3f, "Авто 50 — как в телевизоре: где картинка стоит, оба поля склеены (полная чёткость), где " +
			"движение — строки интерполируются, 50 кадров/с.\nBob 50 — каждое поле отдельно, строки интерполируются всегда.\n" +
			"Weave 25 — поля склеены как есть.\nПоле ×2 — строки поля удвоены.", v => S.Mode = (int)v)));
		viewSeg = MakeSeg("view", new[] { "Цвет", "Яркость", "Клиппинг", "Как ТВ" }, new[] { 0f, 1f, 2f, 3f }, 0f,
			"Клиппинг: синий — ниже чёрного (Y ≤ 16), красный — выше белого (Y ≥ 235).\nКак ТВ: тени глубже (гамма 1,1) и цвета " +
			"холоднее — как на AV-мониторе.", v => { S.View = (int)v; if (coldRow != null) coldRow.Visible = v == 3f; });
		flow.Controls.Add(Row("Показ", viewSeg));
		var cold = new TrackBar { Minimum = 0, Maximum = 200, TickFrequency = 50, Width = D(150), AutoSize = false, Height = D(26) };
		int cv = 100;
		cold.Value = Math.Max(0, Math.Min(200, cv));
		S.Cold = cold.Value / 100f;
		var coldVal = new Label { Text = cold.Value + " %", AutoSize = true, ForeColor = Theme.Muted, Margin = new Padding(0, D(5), 0, 0) };
		cold.ValueChanged += (s, e) => { S.Cold = cold.Value / 100f; coldVal.Text = cold.Value + " %"; if (rend != null) rend.Redraw(); };
		cold.MouseUp += (s, e) => { if (e.Button == MouseButtons.Right) cold.Value = 100; };
		var cf = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0) };
		cf.Controls.Add(cold); cf.Controls.Add(coldVal);
		coldRow = (Panel)Row("Холод", cf);
		coldRow.Visible = S.View == 3;
		tt.SetToolTip(cold, "Насколько холоднее цвета в режиме «Как ТВ»: 100 % — как у монитора, 0 — без сдвига. Правый щелчок — 100 %");
		flow.Controls.Add(coldRow);
		flow.Controls.Add(Row("Шумодав", MakeSeg("nr", new[] { "вкл", "выкл" }, new[] { 1f, 0f }, 1f,
			"Как 3D-шумодав телевизора: где картинка стоит, новое поле подмешивается к накопленному (≈ 7 кадров) — зерно не " +
			"ползает; где движение — берётся новое поле.", v => S.Nr = (int)v)));
		flow.Controls.Add(Row("Контуры", MakeSeg("peak", new[] { "выкл", "мягко", "как ТВ" }, new[] { 0f, 0.5f, 1f }, 1f,
			"Подчёркивание контуров после шумодава, как «резкость» телевизора: «как ТВ» = +6 дБ на 3,4 МГц, мелкая рябь (шум) " +
			"не усиливается.", v => S.Peak = v)));
		flow.Controls.Add(Row("Сглаживание", MakeSeg("aa", new[] { "вкл", "выкл" }, new[] { 1f, 0f }, 1f,
			"Убирает «лесенку» на почти горизонтальных краях (как скалер телевизора): сглаживает только вдоль края.",
			v => S.Aa = (int)v)));
		combSeg = new Seg(new[] { "вкл", "выкл" }, new[] { 1f, 0f }, 0f, "3D-гребёнка: декодер отдаёт яркость без строчной " +
			"гребёнки (вся мелкая детализация), а программа убирает сетку цветовой поднесущей, складывая поле с тем же полем двумя " +
			"кадрами раньше. Это настройка декодера: действует и на странице.", tt);
		combSeg.Changed += v => { if (combId != 0) SendCtrl(combId, (int)v); };
		flow.Controls.Add(Row("3D-гребёнка", combSeg));
		flow.Controls.Add(Row("Края", MakeSeg("crop", new[] { "обрезать", "весь кадр" }, new[] { 1f, 0f }, 1f,
			"Обрезать края гашения: по 9 точек слева и справа, по 4 строки сверху и снизу (как overscan телевизора).",
			v => S.Crop = (int)v)));
		flow.Controls.Add(Row("Растяжение", MakeSeg("sharp", new[] { "чёткое", "мягкое" }, new[] { 1f, 0f }, 1f,
			"Чёткое: Ланцош до пикселей экрана (как скалер телевизора). Мягкое: билинейное.", v => S.Sharp = (int)v)));
		flow.Controls.Add(Row("Ореолы", MakeSeg("ar", new[] { "как есть", "меньше", "убрать" }, new[] { 0f, 0.5f, 1f }, 0.5f,
			"Подавление светлых и тёмных ореолов вокруг резких краёв (буквы OSD), которые даёт растяжение Ланцошем.",
			v => S.AntiRing = v)));
		flow.Controls.Add(Row("Формат", MakeSeg("px11", new[] { "4:3", "1:1" }, new[] { 0f, 1f }, 0f,
			"4:3 — как на телевизоре; 1:1 — квадратные пиксели 720×576.", v => S.Px11 = (int)v)));

		flow.Controls.Add(Head("Вывод"));
		flow.Controls.Add(Row("Цвет точек", MakeSeg("chroma", new[] { "плавно", "парами" }, new[] { 1f, 0f }, 1f,
			"Цвет в видео передаётся на каждые две точки. «Плавно» — цвет нечётной точки берётся между соседними парами " +
			"(чище цветные края); «парами» — как на странице.", v => S.Chroma = (int)v)));
		flow.Controls.Add(Row("Дизеринг", MakeSeg("dither", new[] { "вкл", "выкл" }, new[] { 1f, 0f }, 1f,
			"Незаметный шум в ±1 шаг яркости на выходе: плавные переходы без полос.", v => S.Dither = (int)v)));
		flow.Controls.Add(Row("Разрывы", MakeSeg("notear", new[] { "без разрывов", "мин. задержка" }, new[] { 1f, 0f }, 1f,
			"Действует только во весь экран (в окне кадр всегда выводится сразу, разрывов там не бывает). Без разрывов: кадр " +
			"выводится по кадровому импульсу экрана (240 Гц — ждать не больше 4 мс). Мин. задержка: сразу, возможен разрыв кадра " +
			"на движении.", v => S.NoTear = (int)v)));

		var btns = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0, D(6), 0, 0) };
		btns.Controls.Add(MakeButton("Снимок PNG (P)", () => { if (rend != null) rend.RequestSnapshot(); },
			"Картинка до растяжения, в «Изображения\\SPRUT-M»"));
		btns.Controls.Add(MakeButton("Во весь экран (F)", ToggleFullscreen, "F, F11 или двойной щелчок; Esc — выход"));
		flow.Controls.Add(btns);

		flow.Controls.Add(Head("Сигнал"));
		var tbl = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Width = D(CW), Margin = new Padding(0) };
		tbl.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, D(146)));
		tbl.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, D(CW - 146)));
		foreach (var kv in new[] { "lock|Видеосигнал", "inlock|Захват (IN_LOCK)", "std|Стандарт", "fsc|Цвет (поднесущая)",
			"pw|АРУ по пиковому белому", "fps|Полей в программе", "lat|Задержка в ноутбуке", "skip|Пропуск (сеть)",
			"phase|Чётность полей", "usb|USB-вебкамера", "host|Адрес Pi", "gpu|Видеокарта" }) {
			var parts = kv.Split('|');
			var a = new Label { Text = parts[1], ForeColor = Theme.Muted, AutoSize = true, Margin = new Padding(0, 1, 0, 1) };
			var b = new Label { Text = "—", AutoSize = true, Margin = new Padding(0, 1, 0, 1), MaximumSize = new Size(D(CW - 146), 0) };
			cells[parts[0]] = b;
			tbl.Controls.Add(a);
			tbl.Controls.Add(b);
		}
		tt.SetToolTip(cells["lat"], "От прихода поля из сети до вывода картинки на экран (без учёта самого экрана, " +
			"плюс до одного обновления экрана)");
		flow.Controls.Add(tbl);

		flow.Controls.Add(Head("Яркость (Y), гистограмма"));
		hist.Size = new Size(D(CW), D(96));
		hist.Margin = new Padding(0);
		hist.BackColor = Color.FromArgb(0x0b, 0x0c, 0x0e);
		hist.Paint += PaintHist;
		flow.Controls.Add(hist);
		histStats.AutoSize = true;
		histStats.ForeColor = Theme.Muted;
		histStats.MaximumSize = new Size(D(CW), 0);
		flow.Controls.Add(histStats);

		flow.Controls.Add(Head("Настройки декодера"));
		ctrlBox.FlowDirection = FlowDirection.TopDown;
		ctrlBox.WrapContents = false;
		ctrlBox.AutoSize = true;
		ctrlBox.Margin = new Padding(0);
		ctrlBox.Controls.Add(new Label { Text = "нет данных", ForeColor = Theme.Muted, AutoSize = true });
		flow.Controls.Add(ctrlBox);
		flow.Controls.Add(MakeButton("Всё по умолчанию", () => {
			foreach (var c in ctrls) {
				int id = Int(c, "id"), v = Int(c, "val"), d = Int(c, "def");
				if (v != d) SendCtrl(id, d);
			}
		}, "Ползунки и переключатели декодера ниже — заводские (3D-гребёнка — выше, отдельно)"));
		flow.Controls.Add(new Label { Text = "Настройки декодера действуют сразу, после перезапуска Pi вернутся заводские. " +
			"Клавиши: F — весь экран, P — снимок.", ForeColor = Theme.Muted, AutoSize = true,
			MaximumSize = new Size(D(CW), 0), Margin = new Padding(0, D(6), 0, D(10)) });
	}

	Button MakeButton(string text, Action a, string tip)
	{
		var b = new Button { Text = text, AutoSize = true, FlatStyle = FlatStyle.Flat, BackColor = Theme.Panel, ForeColor = Theme.Text,
			Margin = new Padding(0, 0, D(6), D(4)), Padding = new Padding(D(4), D(2), D(4), D(2)), TabStop = false };
		b.FlatAppearance.BorderColor = Theme.Line;
		b.Click += (s, e) => a();
		if (tip != null) tt.SetToolTip(b, tip);
		return b;
	}

	void BuildTaken()
	{
		taken.Dock = DockStyle.Fill;
		taken.BackColor = Color.FromArgb(0x10, 0x10, 0x10);
		taken.Visible = false;
		var l = new Label { Text = "Видео открыто в другом окне.\nPi отдаёт видео только в одно окно: два сразу тормозят ноутбук.",
			ForeColor = Theme.Text, Font = new Font("Segoe UI", 12f), AutoSize = false, TextAlign = ContentAlignment.MiddleCenter,
			Height = D(100) };
		var b = new Button { Text = "Смотреть здесь", FlatStyle = FlatStyle.Flat, BackColor = Theme.Accent, ForeColor = Color.White,
			Font = new Font("Segoe UI", 11f), Size = new Size(D(200), D(40)) };
		b.FlatAppearance.BorderSize = 0;
		b.Click += (s, e) => { link.TakeBack(); taken.Visible = false; connText = link.Host == PiLink.Hosts[0] ? "подключено по кабелю" : "подключено (" + link.Host + ")"; UpdateStatusLine(); };
		taken.Resize += (s, e) => {
			l.Width = taken.Width; l.Left = 0; l.Top = taken.Height / 2 - D(110);
			b.Left = (taken.Width - b.Width) / 2; b.Top = taken.Height / 2 + D(10);
		};
		taken.Controls.Add(l);
		taken.Controls.Add(b);
	}

	// ---------------------------------------------------------------- from the Pi
	void OnLinkState(string text, bool ok)
	{
		connText = !ok ? text : link.Host == PiLink.Hosts[0] ? "подключено по кабелю"
			: link.Host == PiLink.Hosts[1] ? "подключено по Wi-Fi (медленно; вернусь на кабель, как только он ответит)"
			: "подключено (" + link.Host + ")";
		connOk = ok;
		if (!ok) {
			msg.Text = text;
			msg.Visible = true;
			msg.BringToFront();
			taken.Visible = false;
		} else {
			cells["host"].Text = link.Host;
			skippedPrev = -1;
			msg.Text = "Жду видео…";		// hidden as soon as fields come in (UpdateStats)
		}
		UpdateStatusLine();
	}

	void OnText(string t)
	{
		Dictionary<string, object> m;
		try { m = json.Deserialize<Dictionary<string, object>>(t); } catch { return; }
		string type = m.ContainsKey("type") ? m["type"] as string : null;
		try {
			if (type == "stat") OnStat(m);
			else if (type == "ctrls") OnCtrls(m);
		} catch (Exception e) { Log.W("message " + type + ": " + e.Message); }
		if (type == "taken") {
			taken.Visible = true;
			taken.BringToFront();
			connText = "видео в другом окне";
			UpdateStatusLine();
		}
	}

	static string CName(Dictionary<string, object> m) { object o; return m.TryGetValue("name", out o) ? o as string ?? "" : ""; }
	static int Int(Dictionary<string, object> m, string k)
	{
		object o;
		if (!m.TryGetValue(k, out o) || o == null) return 0;
		try { return Convert.ToInt32(o, CultureInfo.InvariantCulture); } catch { return 0; }
	}

	void Cell(string k, string text, Color c) { var l = cells[k]; l.Text = text; l.ForeColor = c; }

	void OnStat(Dictionary<string, object> s)
	{
		var now = DateTime.Now;
		long skipped = Int(s, "skipped");
		if (skippedPrev >= 0) {
			double dt = (now - skippedT).TotalSeconds, sk = dt > 0 ? (skipped - skippedPrev) / dt : 0;
			skipText = sk < 0.5 ? "нет" : sk.ToString("0") + " /с" + (sk > 5 ? " — медленная сеть" : "");
			Cell("skip", skipText, sk < 0.5 ? Theme.Ok : Theme.Warn);
		}
		skippedPrev = skipped; skippedT = now;
		if (s.ContainsKey("st1")) {
			int st1 = Int(s, "st1"), st3 = Int(s, "st3");
			if (rend != null) rend.St1 = st1;
			bool pattern = false;
			foreach (var c in ctrls) if (CName(c) == "Test Pattern" && Int(c, "val") != Int(c, "max")) pattern = true;
			bool freeRun = (st3 & 0x10) != 0 && !pattern, lk = (st1 & 1) != 0;
			Cell("lock", pattern ? "тестовая картинка" : freeRun ? "нет (свободный ход)" : (st3 & 1) != 0 ? "есть" : "есть, строки не захвачены",
				freeRun ? Theme.Bad : (st3 & 1) != 0 ? Theme.Ok : Theme.Warn);
			Cell("inlock", lk ? ((st1 & 2) != 0 ? "да, были срывы" : "да") : ((st1 & 2) != 0 ? "нет, срывается" : "нет"),
				lk && (st1 & 2) == 0 ? Theme.Ok : Theme.Warn);
			Cell("std", STD[(st1 >> 4) & 7] + ((st3 & 4) != 0 ? ", 50 Гц" : ", 60 Гц"), Theme.Text);
			Cell("fsc", (st1 & 4) != 0 ? "захвачен" : "нет", (st1 & 4) != 0 ? Theme.Ok : Theme.Warn);
			Cell("pw", (st1 & 8) != 0 ? "ограничивает" : "нет", (st1 & 8) != 0 ? Theme.Warn : Theme.Text);
			noSignal = freeRun;
			if (freeRun) { msg.Text = "Нет видеосигнала на входе"; msg.Visible = true; msg.BringToFront(); }
			else if (lastFps > 1) msg.Visible = false;
		} else {
			if (rend != null) rend.St1 = -1;
			noSignal = false;
		}
		int phase = Int(s, "phase");
		Cell("phase", phase >= 0 ? "определена" : "ищется", phase >= 0 ? Theme.Ok : Theme.Warn);
		Cell("usb", Int(s, "usb") != 0 ? "идёт поток" : "не используется", Theme.Text);
	}

	void OnCtrls(Dictionary<string, object> m)
	{
		var list = new List<Dictionary<string, object>>();
		var arr = m.ContainsKey("list") ? m["list"] as System.Collections.ArrayList : null;
		if (arr != null) foreach (var o in arr) { var d = o as Dictionary<string, object>; if (d != null) list.Add(d); }
		var comb = list.Find(c => CName(c) == "3D Comb");
		if (comb != null) {
			combId = Int(comb, "id");
			if (rend != null) rend.Comb3d = Int(comb, "val");
			combSeg.Set(Int(comb, "val"));
			list.Remove(comb);
		}
		var al = list.Find(c => CName(c) == "Auto Levels");
		if (rend != null) rend.AutoLev = al != null && Int(al, "val") != 0;
		var lp = list.Find(c => CName(c) == "Luma Peaking");
		if (rend != null) {
			int v = lp != null ? Int(lp, "val") : 245;	// page: motGain, x2 at 64, x1 at 245
			rend.MotGain = Math.Min(2f, Math.Max(1f, 2f - (v - 64) / 181f));
		}
		bool same = list.Count == ctrls.Count;
		for (int i = 0; same && i < list.Count; i++) same = Int(list[i], "id") == Int(ctrls[i], "id");
		ctrls = list;
		if (same && ctrlControls.Count == list.Count) {		// only the values changed (auto levels moving them)
			for (int i = 0; i < list.Count; i++) SetCtrlValue(ctrlControls[i], list[i]);
			return;
		}
		ctrlBox.SuspendLayout();
		var old = new List<Control>();
		foreach (Control c in ctrlBox.Controls) old.Add(c);
		ctrlBox.Controls.Clear();
		foreach (var c in old) c.Dispose();
		ctrlControls.Clear();
		foreach (var c in list) {
			string name = CName(c);
			string ru;
			if (!RU.TryGetValue(name, out ru)) ru = name;
			int id = Int(c, "id"), type = Int(c, "type"), min = Int(c, "min"), max = Int(c, "max"), def = Int(c, "def");
			Control ctl;
			if (type == 3) {
				var p = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, D(6)) };
				p.Controls.Add(new Label { Text = ru, AutoSize = true, MaximumSize = new Size(D(CW - 2), 0), Margin = new Padding(0) });
				var cb = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = D(CW - 8), BackColor = Theme.Panel, ForeColor = Theme.Text, FlatStyle = FlatStyle.Flat };
				var menu = c.ContainsKey("menu") ? c["menu"] as System.Collections.ArrayList : null;
				for (int i = min; i <= max; i++) {
					string t = menu != null && i - min < menu.Count ? menu[i - min] as string : null;
					cb.Items.Add(string.IsNullOrEmpty(t) ? i.ToString() : t);
				}
				cb.Tag = c;
				cb.SelectedIndexChanged += (s, e) => { if (!updating) SendCtrl(id, min + cb.SelectedIndex); };
				p.Controls.Add(cb);
				ctl = p;
			} else if (type == 2) {
				var ch = new CheckBox { Text = ru, AutoSize = true, MaximumSize = new Size(D(CW - 2), 0), Margin = new Padding(0, 0, 0, D(6)) };
				ch.CheckedChanged += (s, e) => { if (!updating) SendCtrl(id, ch.Checked ? 1 : 0); };
				ctl = ch;
			} else {
				var p = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, D(4)) };
				var l = new Label { AutoSize = true, MaximumSize = new Size(D(CW - 2), 0), Margin = new Padding(0) };
				var tb = new TrackBar { Minimum = min, Maximum = Math.Max(min + 1, max), Width = D(CW - 8), AutoSize = false, Height = D(26),
					TickStyle = TickStyle.None, SmallChange = 1, LargeChange = Math.Max(1, (max - min) / 16) };
				tb.Tag = l;
				tb.ValueChanged += (s, e) => { l.Text = ru + ": " + tb.Value + " (по умолч. " + def + ")"; if (!updating) SendCtrl(id, tb.Value); };
				tb.MouseUp += (s, e) => { if (e.Button == MouseButtons.Right) tb.Value = Math.Max(tb.Minimum, Math.Min(tb.Maximum, def)); };
				tt.SetToolTip(tb, "Правый щелчок — по умолчанию (" + def + ")");
				p.Controls.Add(l);
				p.Controls.Add(tb);
				ctl = p;
				ctl.Tag = new object[] { ru, def };
			}
			ctrlControls.Add(ctl);
			SetCtrlValue(ctl, c);
			ctrlBox.Controls.Add(ctl);
		}
		if (list.Count == 0) ctrlBox.Controls.Add(new Label { Text = "декодер не отдал настроек", ForeColor = Theme.Muted, AutoSize = true });
		ctrlBox.ResumeLayout();
	}

	readonly List<Control> ctrlControls = new List<Control>();
	bool updating;

	void SetCtrlValue(Control ctl, Dictionary<string, object> c)
	{
		updating = true;
		try {
			int v = Int(c, "val");
			var ch = ctl as CheckBox;
			if (ch != null) { ch.Checked = v != 0; return; }
			foreach (Control k in ctl.Controls) {
				var cb = k as ComboBox;
				if (cb != null) { int i = v - Int(c, "min"); if (i >= 0 && i < cb.Items.Count && !cb.DroppedDown) cb.SelectedIndex = i; }
				var tb = k as TrackBar;
				if (tb != null && !tb.Capture) {
					tb.Value = Math.Max(tb.Minimum, Math.Min(tb.Maximum, v));
					var meta = (object[])ctl.Tag;
					((Label)tb.Tag).Text = meta[0] + ": " + tb.Value + " (по умолч. " + meta[1] + ")";
				}
			}
		} finally { updating = false; }
	}

	readonly Dictionary<int, int> pendingCtrl = new Dictionary<int, int>();
	Timer ctrlTimer;
	void SendCtrl(int id, int v)
	{
		pendingCtrl[id] = v;
		if (ctrlTimer == null) {
			ctrlTimer = new Timer { Interval = 60 };
			ctrlTimer.Tick += (s, e) => {
				ctrlTimer.Stop();
				foreach (var kv in pendingCtrl) link.Send("ctrl " + kv.Key + " " + kv.Value);
				pendingCtrl.Clear();
			};
		}
		if (!ctrlTimer.Enabled) ctrlTimer.Start();
	}

	// ---------------------------------------------------------------- stats
	void UpdateStats()
	{
		var st = rend.St;
		double fps, lat, latMax, gpu, refresh;
		string hs;
		lock (st) { fps = st.FieldsPerSec; lat = st.LatAvgMs; latMax = st.LatMaxMs; gpu = st.GpuMs; refresh = st.Refresh;
			hs = "мин " + st.HMin + " · 1 % " + st.HP1 + " · медиана " + st.HP50 + " · 99 % " + st.HP99 + " · макс " + st.HMax +
				" · ≥235: " + st.HClip.ToString("0.0") + " %"; }
		if (histStats.Text != hs) histStats.Text = hs;
		Cell("fps", fps.ToString("0.0") + " /с", fps > 45 ? Theme.Ok : fps > 1 ? Theme.Warn : Theme.Muted);
		Cell("lat", fps > 1 ? lat.ToString("0.0") + " мс (макс " + latMax.ToString("0") + ")" : "—", Theme.Text);
		Cell("gpu", st.Gpu, Theme.Muted);
		hist.Invalidate();
		lastFps = fps; lastLat = lat; lastRefresh = refresh;
		if (fps > 1 && !noSignal && connOk && msg.Visible) msg.Visible = false;
		UpdateStatusLine();
	}
	double lastFps, lastLat, lastRefresh;
	bool noSignal;

	void UpdateStatusLine()
	{
		var sb = new StringBuilder();
		sb.Append(connOk ? "● " : "○ ").Append(connText);
		if (connOk && lastFps > 1) {
			sb.Append("   ·   ").Append(lastFps.ToString("0.0")).Append(" полей/с");
			sb.Append("   ·   задержка в ноутбуке ").Append(lastLat.ToString("0.0")).Append(" мс");
			if (screenHz == 0) screenHz = ScreenHz();
			if (screenHz > 0) sb.Append(" + экран ").Append(screenHz).Append(" Гц");
		}
		if (connOk && rend != null && rend.LastLocked == 0 && lastFps > 1) sb.Append("   ·   чётность полей определяется…");
		if (snapNote != null && (DateTime.Now - snapT).TotalSeconds < 6) sb.Append("   ·   ").Append(snapNote);
		statusLine.Text = sb.ToString();
		statusLine.ForeColor = connOk ? Theme.Muted : Theme.Warn;
	}

	void PaintHist(object sender, PaintEventArgs e)
	{
		if (rend == null) return;
		var g = e.Graphics;
		int W = hist.Width, H = hist.Height;
		var b = new int[256];
		lock (rend.St) Array.Copy(rend.St.Hist, b, 256);
		double peak = 1;
		for (int i = 0; i < 256; i++) peak = Math.Max(peak, Math.Sqrt(b[i]));
		using (var br = new SolidBrush(Color.FromArgb(0x4f, 0xa3, 0xff)))
			for (int i = 0; i < 256; i++) {
				float v = (float)(Math.Sqrt(b[i]) / peak * (H - 4));
				float x0 = i * W / 256f, x1 = (i + 1) * W / 256f;
				g.FillRectangle(br, x0, H - v, Math.Max(1f, x1 - x0), v);
			}
		using (var p = new Pen(Theme.Muted)) {
			g.DrawLine(p, 16 * W / 256f, 0, 16 * W / 256f, H);
			g.DrawLine(p, 235 * W / 256f, 0, 235 * W / 256f, H);
		}
	}

	// ---------------------------------------------------------------- settings file
	string Get(string k, string def) { string v; return ini.TryGetValue(k, out v) ? v : def; }
	void Put(string k, string v) { ini[k] = v; }
	void LoadIni()
	{
		try {
			foreach (var line in File.ReadAllLines(iniPath)) {
				int i = line.IndexOf('=');
				if (i > 0) ini[line.Substring(0, i).Trim()] = line.Substring(i + 1).Trim();
			}
		} catch { }
	}
	void SaveIni()
	{
		try {
			Directory.CreateDirectory(Path.GetDirectoryName(iniPath));
			var sb = new StringBuilder();
			foreach (var kv in ini) sb.Append(kv.Key).Append('=').Append(kv.Value).Append("\r\n");
			File.WriteAllText(iniPath, sb.ToString());
		} catch { }
	}
}
