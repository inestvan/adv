// The renderer: one thread that owns the OpenGL context of the video panel. Every field from the Pi is processed as
// soon as it arrives (upload, 3D comb, noise reduction - the page's passes), then the picture is drawn and shown at
// once: no browser compositor in between. Logic and constants follow grabber/www/index.html.

using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Threading;

sealed class ViewSettings
{
	public volatile int Mode = 3;		// 3 Авто 50, 0 Bob 50, 1 Weave 25, 2 Поле x2
	public volatile int View = 0;		// 0 colour, 1 luma, 2 clipping, 3 "Как ТВ"
	public volatile int Nr = 1, Aa = 1, Crop = 1, Sharp = 1, Px11 = 0, Chroma = 1, Dither = 1, NoTear = 1;
	public float Peak = 1f, Cold = 1f, AntiRing = 0.5f;
}

sealed class Stats
{
	public double FieldsPerSec, LatAvgMs, LatMaxMs, GpuMs, Refresh;
	public long Fields, Dropped;
	public int[] Hist = new int[256];
	public int HMin, HP1, HP50, HP99, HMax;
	public double HClip;
	public string Gpu = "";
}

sealed class Renderer
{
	readonly IntPtr hwnd;
	readonly ViewSettings S;
	readonly PiLink link;
	public readonly Stats St = new Stats();
	public event Action StatsReady;		// ~4 times a second, on the render thread
	public event Action<string> Failed;
	public event Action<string> Snapshot;	// file saved (or error text starting with "!")

	readonly ConcurrentQueue<FieldBuf> queue = new ConcurrentQueue<FieldBuf>();
	readonly AutoResetEvent wake = new AutoResetEvent(false);
	Thread th;
	volatile bool stop, redraw, snapReq;
	public volatile int Comb3d;		// decoder line comb off -> 3D comb here (from the "3D Comb" control)
	public volatile float MotGain = 1f;	// motion test sensitivity by the decoder's peaking (page: motGain)
	public volatile bool AutoLev;		// "Auto Levels" switch of the Pi
	public volatile int St1 = -1;		// decoder Status 1 from the last "stat" (-1: none)
	public volatile int LastLocked = 1;	// field parity of the Pi locked (header flag bit 1)
	public volatile bool Fullscreen;	// windowed: never wait for the vertical blank (the desktop compositor
					// already shows whole frames); full screen: as set (S.NoTear)
	public string TestDir;			// --test DIR: after TestFields fields save the window and the source picture, quit
	public int TestFields = 150;
	public event Action TestDone;
	readonly System.Collections.Generic.List<double> testLat = new System.Collections.Generic.List<double>();
	readonly System.Collections.Generic.List<double> testGpu = new System.Collections.Generic.List<double>();

	public Renderer(IntPtr hwnd, ViewSettings s, PiLink link) { this.hwnd = hwnd; S = s; this.link = link; }

	public void Start()
	{
		th = new Thread(Run) { IsBackground = true, Name = "Render", Priority = ThreadPriority.Highest };
		th.Start();
	}
	public void Stop() { stop = true; wake.Set(); if (th != null) th.Join(2000); }
	public void Push(FieldBuf f)
	{
		queue.Enqueue(f);
		while (queue.Count > 12) {		// far behind: drop the oldest (the history checks see the gap)
			FieldBuf old;
			if (queue.TryDequeue(out old)) { link.Recycle(old); St.Dropped++; }
		}
		wake.Set();
	}
	public void Redraw() { redraw = true; wake.Set(); }
	public void RequestSnapshot() { snapReq = true; redraw = true; wake.Set(); }

	// ---------------------------------------------------------------- GL state
	IntPtr hdc, ctx;
	int progMain, progClean, progTnr, progAA, progScale, vao;
	readonly int[] tex = new int[10];		// 0..5 raw fields (3 per parity), 6..9 cleaned (2 per parity)
	readonly int[] cleanFb = new int[4];
	readonly int[] accTex = new int[4], accFb = new int[4];
	readonly int[] fbTex = new int[3], fb = new int[3];
	readonly int[,] fbDim = new int[3, 2];
	readonly int[] rawSlot = new int[2], cleanSlot = new int[2], accSlot = new int[2];
	readonly long[] rawSeq = new long[6], accSeq = new long[2];
	readonly bool[] cut = new bool[2];
	readonly FieldBuf[] prevField = new FieldBuf[2];
	int width = 720, lines = 288, cur;
	int srcW = 702, srcH = 568;
	int[] outRect = { 0, 0, 1, 1 };
	int winW = 1, winH = 1;
	bool haveField;
	float levB = 16f, levG = 1f;
	int histTick;
	// uniform locations
	int mTop, mBot, mTopP, mBotP, mTopN, mBotN, mMode, mCur, mView, mLines, mH, mW, mX0, mY0, mTNR, mChroma, mCold,
		mPeak, mCore0, mCore1, mMotGain, mLev;
	int cRaw, cOld, cComb, tCur, tAcc, tReset, tKmin, tKminC, tT0, tT1, tTpx, tCw, aSrc, aAA, sSrc, sAxis, sSharp,
		sDither, sAR, sRect;

	[DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
	[StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
	[DllImport("dwmapi.dll")] static extern int DwmGetCompositionTimingInfo(IntPtr h, ref DWM_TIMING_INFO t);
	[StructLayout(LayoutKind.Sequential, Pack = 1)]
	struct DWM_TIMING_INFO
	{
		public int cbSize; public uint rateRefreshNum, rateRefreshDen; public ulong qpcRefreshPeriod;
		public uint rateComposeNum, rateComposeDen; public ulong qpcVBlank, cRefresh; public uint cDXRefresh;
		public ulong qpcCompose, cFrame; public uint cDXPresent; public ulong cRefreshFrame, cFrameSubmitted;
		public uint cDXPresentSubmitted; public ulong cFrameConfirmed; public uint cDXPresentConfirmed;
		public ulong cRefreshConfirmed; public uint cDXRefreshConfirmed; public ulong cFramesLate;
		public uint cFramesOutstanding; public ulong cFrameDisplayed, qpcFrameDisplayed, cRefreshFrameDisplayed,
		cFrameComplete, qpcFrameComplete, cFramePending, qpcFramePending, cFramesDisplayed, cFramesComplete,
		cFramesPending, cFramesAvailable, cFramesDropped, cFramesMissed, cRefreshNextDisplayed,
		cRefreshNextPresented, cRefreshesDisplayed, cRefreshesPresented, cRefreshStarted, cPixelsReceived,
		cPixelsDrawn, cBuffersEmpty;
	}

	void InitGL()
	{
		hdc = Gl.GetDC(hwnd);
		var pfd = new Gl.PIXELFORMATDESCRIPTOR();
		pfd.nSize = (short)Marshal.SizeOf(typeof(Gl.PIXELFORMATDESCRIPTOR));
		pfd.nVersion = 1;
		pfd.dwFlags = 0x4 | 0x20 | 0x1;		// DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
		pfd.iPixelType = 0;
		pfd.cColorBits = 32;
		pfd.cAlphaBits = 8;
		int pf = Gl.ChoosePixelFormat(hdc, ref pfd);
		if (pf == 0 || !Gl.SetPixelFormat(hdc, pf, ref pfd)) throw new Exception("SetPixelFormat не удался");
		IntPtr legacy = Gl.wglCreateContext(hdc);
		if (legacy == IntPtr.Zero || !Gl.wglMakeCurrent(hdc, legacy)) throw new Exception("OpenGL недоступен");
		Gl.LoadWgl();
		if (Gl.CreateContextAttribsARB == null) throw new Exception("видеокарта не даёт OpenGL 3.3");
		int[] attr = { 0x2091, 3, 0x2092, 3, 0x9126, 0x1, 0 };	// MAJOR 3, MINOR 3, PROFILE_MASK = CORE
		ctx = Gl.CreateContextAttribsARB(hdc, IntPtr.Zero, attr);
		if (ctx == IntPtr.Zero) throw new Exception("видеокарта не даёт OpenGL 3.3 core");
		Gl.wglMakeCurrent(IntPtr.Zero, IntPtr.Zero);
		Gl.wglDeleteContext(legacy);
		if (!Gl.wglMakeCurrent(hdc, ctx)) throw new Exception("wglMakeCurrent не удался");
		Gl.LoadWgl();
		Gl.LoadAll();
		St.Gpu = Gl.GetString(Gl.RENDERER) + " · OpenGL " + Gl.GetString(Gl.VERSION).Split(' ')[0];

		progMain = Gl.Program(Shaders.VS, Shaders.FS_MAIN);
		progClean = Gl.Program(Shaders.VS, Shaders.FS_CLEAN);
		progTnr = Gl.Program(Shaders.VS, Shaders.FS_TNR);
		progAA = Gl.Program(Shaders.VS, Shaders.FS_AA);
		progScale = Gl.Program(Shaders.VS, Shaders.FS_SCALE);
		vao = Gl.GenOne(Gl.GenVertexArrays);
		Gl.BindVertexArray(vao);

		Func<int, string, int> U = (p, n) => Gl.GetUniformLocation(p, n);
		mTop = U(progMain, "uTop"); mBot = U(progMain, "uBot"); mTopP = U(progMain, "uTopP"); mBotP = U(progMain, "uBotP");
		mTopN = U(progMain, "uTopN"); mBotN = U(progMain, "uBotN"); mMode = U(progMain, "uMode"); mCur = U(progMain, "uCur");
		mView = U(progMain, "uView"); mLines = U(progMain, "uLines"); mH = U(progMain, "uH"); mW = U(progMain, "uW");
		mX0 = U(progMain, "uX0"); mY0 = U(progMain, "uY0"); mTNR = U(progMain, "uTNR"); mChroma = U(progMain, "uChroma");
		mCold = U(progMain, "uCold"); mPeak = U(progMain, "uPeak"); mCore0 = U(progMain, "uCore0");
		mCore1 = U(progMain, "uCore1"); mMotGain = U(progMain, "uMotGain"); mLev = U(progMain, "uLev");
		cRaw = U(progClean, "uRaw"); cOld = U(progClean, "uOld"); cComb = U(progClean, "uComb");
		tCur = U(progTnr, "uCur"); tAcc = U(progTnr, "uAcc"); tReset = U(progTnr, "uReset"); tKmin = U(progTnr, "uKmin");
		tKminC = U(progTnr, "uKminC"); tT0 = U(progTnr, "uT0"); tT1 = U(progTnr, "uT1"); tTpx = U(progTnr, "uTpx");
		tCw = U(progTnr, "uCw");
		aSrc = U(progAA, "uSrc"); aAA = U(progAA, "uAA");
		sSrc = U(progScale, "uSrc"); sAxis = U(progScale, "uAxis"); sSharp = U(progScale, "uSharp");
		sDither = U(progScale, "uDither"); sAR = U(progScale, "uAR"); sRect = U(progScale, "uRect");

		// fixed texture units per program
		Gl.UseProgram(progMain);
		Gl.Uniform1i(mTop, 0); Gl.Uniform1i(mBot, 1); Gl.Uniform1i(mTopP, 2); Gl.Uniform1i(mBotP, 3);
		Gl.Uniform1i(mTopN, 4); Gl.Uniform1i(mBotN, 5);
		Gl.Uniform1f(mCore0, 2f); Gl.Uniform1f(mCore1, 6f);
		Gl.UseProgram(progClean); Gl.Uniform1i(cRaw, 0); Gl.Uniform1i(cOld, 1);
		Gl.UseProgram(progTnr); Gl.Uniform1i(tCur, 0); Gl.Uniform1i(tAcc, 1);
		Gl.Uniform1f(tKmin, 0.25f); Gl.Uniform1f(tKminC, 0.2f);	// still areas: ~7 frames (luma), ~9 (chroma)
		Gl.Uniform1f(tT0, 1.5f); Gl.Uniform1f(tT1, 4f); Gl.Uniform1f(tTpx, 6f); Gl.Uniform1f(tCw, 0.5f);
		Gl.UseProgram(progAA); Gl.Uniform1i(aSrc, 0);
		Gl.UseProgram(progScale); Gl.Uniform1i(sSrc, 0);

		for (int i = 0; i < 10; i++) tex[i] = NewTex(Gl.NEAREST);
		for (int i = 0; i < 4; i++) { cleanFb[i] = Gl.GenOne(Gl.GenFramebuffers); accTex[i] = NewTex(Gl.NEAREST); accFb[i] = Gl.GenOne(Gl.GenFramebuffers); }
		for (int i = 0; i < 3; i++) { fbTex[i] = NewTex(Gl.LINEAR); fb[i] = Gl.GenOne(Gl.GenFramebuffers); }
		Gl.PixelStorei(Gl.UNPACK_ALIGNMENT, 1);
		Gl.PixelStorei(Gl.PACK_ALIGNMENT, 1);
		AllocTex();
	}

	static int NewTex(int filter)
	{
		int t = Gl.GenTexture();
		Gl.BindTexture(Gl.TEXTURE_2D, t);
		Gl.TexParameteri(Gl.TEXTURE_2D, Gl.TEXTURE_MIN_FILTER, filter);
		Gl.TexParameteri(Gl.TEXTURE_2D, Gl.TEXTURE_MAG_FILTER, filter);
		Gl.TexParameteri(Gl.TEXTURE_2D, Gl.TEXTURE_WRAP_S, Gl.CLAMP_TO_EDGE);
		Gl.TexParameteri(Gl.TEXTURE_2D, Gl.TEXTURE_WRAP_T, Gl.CLAMP_TO_EDGE);
		return t;
	}

	void AllocTex()
	{
		var black = new byte[width * 2 * lines];
		for (int i = 0; i < black.Length; i += 2) { black[i] = 128; black[i + 1] = 16; }
		var h = GCHandle.Alloc(black, GCHandleType.Pinned);
		try {
			for (int i = 0; i < 10; i++) {
				Gl.BindTexture(Gl.TEXTURE_2D, tex[i]);
				Gl.TexImage2D(Gl.TEXTURE_2D, 0, Gl.RGBA8, width / 2, lines, 0, Gl.RGBA, Gl.UNSIGNED_BYTE, h.AddrOfPinnedObject());
			}
		} finally { h.Free(); }
		for (int i = 0; i < 4; i++) {
			Gl.BindFramebuffer(Gl.FRAMEBUFFER, cleanFb[i]);
			Gl.FramebufferTexture2D(Gl.FRAMEBUFFER, Gl.COLOR_ATTACHMENT0, Gl.TEXTURE_2D, tex[6 + i], 0);
		}
		for (int i = 0; i < 4; i++) {
			Gl.BindTexture(Gl.TEXTURE_2D, accTex[i]);
			Gl.TexImage2D(Gl.TEXTURE_2D, 0, Gl.RGBA16F, width / 2, lines, 0, Gl.RGBA, Gl.HALF_FLOAT, IntPtr.Zero);
			Gl.BindFramebuffer(Gl.FRAMEBUFFER, accFb[i]);
			Gl.FramebufferTexture2D(Gl.FRAMEBUFFER, Gl.COLOR_ATTACHMENT0, Gl.TEXTURE_2D, accTex[i], 0);
		}
		Gl.BindFramebuffer(Gl.FRAMEBUFFER, 0);
		for (int i = 0; i < 6; i++) rawSeq[i] = -100;
		accSeq[0] = accSeq[1] = -100;
		for (int i = 0; i < 3; i++) fbDim[i, 0] = fbDim[i, 1] = 0;
		Layout();
	}

	void FbSize(int i, int w, int h)
	{
		if (fbDim[i, 0] == w && fbDim[i, 1] == h) return;
		fbDim[i, 0] = w; fbDim[i, 1] = h;
		Gl.BindTexture(Gl.TEXTURE_2D, fbTex[i]);
		Gl.TexImage2D(Gl.TEXTURE_2D, 0, Gl.RGBA16F, w, h, 0, Gl.RGBA, Gl.HALF_FLOAT, IntPtr.Zero);
		Gl.BindFramebuffer(Gl.FRAMEBUFFER, fb[i]);
		Gl.FramebufferTexture2D(Gl.FRAMEBUFFER, Gl.COLOR_ATTACHMENT0, Gl.TEXTURE_2D, fbTex[i], 0);
		Gl.BindFramebuffer(Gl.FRAMEBUFFER, 0);
	}

	int dCrop = 1, dPx11, dSharp = 1;		// the settings of the frame being drawn, read once per frame
	int CropX { get { return dCrop == 1 ? 9 : 0; } }
	int CropY { get { return dCrop == 1 ? 4 : 0; } }
	// picture shape: 4:3 for the 702 active samples x 576 lines (BT.601), or square pixels
	double Aspect { get { return dPx11 == 1 ? (double)srcW / srcH : (srcW / 702.0) * (lines * 2.0 / srcH) * 4.0 / 3.0; } }

	void Layout()
	{
		srcW = width - 2 * CropX;
		srcH = lines * 2 - 2 * CropY;
		FbSize(0, srcW, srcH);
		FbSize(2, srcW, srcH);
		RECT r;
		GetClientRect(hwnd, out r);
		winW = Math.Max(1, r.R - r.L);
		winH = Math.Max(1, r.B - r.T);
		double a = Aspect;
		int w = winW, h = (int)Math.Round(winW / a);
		if (h > winH) { h = winH; w = (int)Math.Round(winH * a); }
		outRect = new[] { (winW - w) >> 1, (winH - h) >> 1, w, h };
		FbSize(1, w, srcH);
	}

	// ---------------------------------------------------------------- fields
	void OnField(FieldBuf f)
	{
		if (St.Fields == 0 && !haveField) Log.W("first field " + f.Width + "x" + f.Lines + " seq " + f.Seq);
		if (f.Width != width || f.Lines != lines) {
			width = f.Width; lines = f.Lines;
			AllocTex();
			for (int p = 0; p < 2; p++) { link.Recycle(prevField[p]); prevField[p] = null; }
		}
		int par = f.Top == 1 ? 0 : 1;		// 0 = top field
		rawSlot[par] = (rawSlot[par] + 1) % 3;
		int ri = 3 * par + rawSlot[par];
		Gl.ActiveTexture(Gl.TEXTURE0);
		Gl.BindTexture(Gl.TEXTURE_2D, tex[ri]);
		var h = GCHandle.Alloc(f.Data, GCHandleType.Pinned);
		try {
			Gl.TexSubImage2D(Gl.TEXTURE_2D, 0, 0, 0, width / 2, lines, Gl.RGBA, Gl.UNSIGNED_BYTE,
				h.AddrOfPinnedObject() + 16);
		} finally { h.Free(); }
		rawSeq[ri] = f.Seq;
		cut[par] = SceneCut(f, prevField[par]);
		CleanField(par);
		TnrField(par, f.Seq);
		link.Recycle(prevField[par]);
		prevField[par] = f;
		cur = par;
		LastLocked = f.Locked;
		haveField = true;
		if (++histTick % 5 == 0) histDue = f;
	}
	FieldBuf histDue;		// the field to take the histogram from, after the picture is out

	// scene cut: mean |3-px luma sum change| / 3 against the previous raw field of the same parity (page: sceneCut)
	bool SceneCut(FieldBuf a, FieldBuf b)
	{
		if (b == null || b.Width != a.Width || b.Lines != a.Lines) return true;
		int row = width * 2;
		long s = 0, n = 0;
		byte[] A = a.Data, B = b.Data;
		for (int y = 0; y < lines; y += 8)
			for (int x = 0; x < width - 2; x += 12) {
				int i = 16 + y * row + x * 2 + 1;
				s += Math.Abs(A[i] + A[i + 2] + A[i + 4] - B[i] - B[i + 2] - B[i + 4]);
				n++;
			}
		return (double)s / n / 3 > 16;
	}

	void Quad(int fbo, int w, int h)
	{
		Gl.BindFramebuffer(Gl.FRAMEBUFFER, fbo);
		Gl.Viewport(0, 0, w, h);
		Gl.DrawArrays(Gl.TRIANGLES, 0, 3);
	}
	static void Bind(int unit, int t) { Gl.ActiveTexture(Gl.TEXTURE0 + unit); Gl.BindTexture(Gl.TEXTURE_2D, t); }

	void CleanField(int f)
	{
		cleanSlot[f] ^= 1;
		int raw = 3 * f + rawSlot[f], old = 3 * f + (rawSlot[f] + 1) % 3;
		int m = Comb3d == 0 ? 0 : rawSeq[raw] - rawSeq[old] == 4 ? 1 : 2;	// history broken: no 3D
		Gl.UseProgram(progClean);
		Bind(0, tex[raw]); Bind(1, tex[old]);
		Gl.Uniform1i(cComb, m);
		Quad(cleanFb[2 * f + cleanSlot[f]], width / 2, lines);
	}

	void TnrField(int f, long seq)
	{
		int prev = 2 * f + accSlot[f];
		accSlot[f] ^= 1;
		bool reset = S.Nr == 0 || seq - accSeq[f] != 2 || cut[f];
		accSeq[f] = seq;
		Gl.UseProgram(progTnr);
		Bind(0, tex[6 + 2 * f + cleanSlot[f]]); Bind(1, accTex[prev]);
		Gl.Uniform1i(tReset, reset ? 1 : 0);
		Quad(accFb[2 * f + accSlot[f]], width / 2, lines);
	}

	void Histogram(FieldBuf f)
	{
		var b = new int[256];
		int total = width * lines;
		byte[] d = f.Data;
		for (int i = 17, end = 16 + width * 2 * lines; i < end; i += 2) b[d[i]]++;
		Func<double, int> pct = p => { long s = 0; for (int i = 0; i < 256; i++) { s += b[i]; if (s >= total * p) return i; } return 255; };
		int mn = 0; while (mn < 255 && b[mn] == 0) mn++;
		int mx = 255; while (mx > 0 && b[mx] == 0) mx--;
		long clip = 0; for (int i = 235; i < 256; i++) clip += b[i];
		lock (St) {
			Array.Copy(b, St.Hist, 256);
			St.HMin = mn; St.HP1 = pct(0.01); St.HP50 = pct(0.5); St.HP99 = pct(0.99); St.HMax = mx;
			St.HClip = 100.0 * clip / total;
		}
		AutoLevels(pct(0.005), pct(0.995));
	}

	// page: autoLevels - with the Pi's "Auto Levels" on and the decoder's peak-white AGC limiting (Status 1 bits 0 + 3)
	// the picture is stretched (up to x3) so that the darkest / brightest 0.5 % land on Y 20 / 200, eased in
	void AutoLevels(int lo, int hi)
	{
		float tb = 16f, tg = 1f;
		int s1 = St1;
		if (AutoLev && s1 >= 0 && (s1 & 9) == 9 && hi - lo >= 8) {
			tg = Math.Min(3f, Math.Max(1f, 180f / (hi - lo)));
			tb = Math.Max(lo, 16) - 4f / tg;
		}
		levG += (tg - levG) * 0.25f;
		levB += (tb - levB) * 0.25f;
		if (Math.Abs(tg - levG) < 0.005f) levG = tg;
	}

	// ---------------------------------------------------------------- picture
	void Draw()
	{
		dCrop = S.Crop; dPx11 = S.Px11; dSharp = S.Sharp;
		int view = S.View, nr = S.Nr;
		Layout();
		// pass 1: deinterlace + colour at the source size
		Gl.UseProgram(progMain);
		Bind(0, tex[6 + cleanSlot[0]]); Bind(1, tex[8 + cleanSlot[1]]);
		Bind(2, tex[6 + (cleanSlot[0] ^ 1)]); Bind(3, tex[8 + (cleanSlot[1] ^ 1)]);
		Bind(4, accTex[accSlot[0]]); Bind(5, accTex[2 + accSlot[1]]);
		Gl.Uniform1i(mMode, S.Mode); Gl.Uniform1i(mCur, cur); Gl.Uniform1i(mView, view);
		Gl.Uniform1i(mLines, lines); Gl.Uniform1i(mH, srcH); Gl.Uniform1i(mW, width);
		Gl.Uniform1i(mX0, CropX); Gl.Uniform1i(mY0, CropY); Gl.Uniform1f(mCold, S.Cold);
		Gl.Uniform1i(mTNR, nr); Gl.Uniform1f(mPeak, view == 2 ? 0f : S.Peak); Gl.Uniform1f(mMotGain, MotGain);
		Gl.Uniform1i(mChroma, S.Chroma); Gl.Uniform2f(mLev, levB, levG);
		Quad(fb[0], srcW, srcH);
		// pass 1b: along-edge anti-aliasing
		Gl.UseProgram(progAA);
		Bind(0, fbTex[0]);
		Gl.Uniform1i(aAA, S.Aa);
		Quad(fb[2], srcW, srcH);
		// pass 2 + 3: scale to the screen pixels
		Gl.UseProgram(progScale);
		Gl.Uniform1i(sSharp, dSharp); Gl.Uniform1f(sAR, S.AntiRing);
		int x0 = outRect[0], y0 = outRect[1], w = outRect[2], h = outRect[3];
		int src = fbTex[2];
		if (dSharp == 1) {
			Bind(0, fbTex[2]);
			Gl.Uniform1i(sAxis, 0); Gl.Uniform1i(sDither, 0); Gl.Uniform4f(sRect, 0, 0, w, srcH);
			Quad(fb[1], w, srcH);
			src = fbTex[1];
		}
		Gl.BindFramebuffer(Gl.FRAMEBUFFER, 0);
		Gl.Viewport(0, 0, winW, winH);
		Gl.ClearColor(0, 0, 0, 1);
		Gl.Clear(Gl.COLOR_BUFFER_BIT);
		Gl.Viewport(x0, y0, w, h);
		Bind(0, src);
		Gl.Uniform1i(sAxis, 1); Gl.Uniform1i(sDither, S.Dither); Gl.Uniform4f(sRect, x0, y0, w, h);
		Gl.DrawArrays(Gl.TRIANGLES, 0, 3);
	}

	// the picture before scaling, read back here; the PNG is written on the thread pool (no hitch in the video)
	void SaveSnapshot()
	{
		try {
			int w = srcW, h = srcH;
			var px = new byte[w * h * 4];
			Gl.BindFramebuffer(Gl.FRAMEBUFFER, fb[2]);
			var hd = GCHandle.Alloc(px, GCHandleType.Pinned);
			try { Gl.ReadPixels(0, 0, w, h, Gl.RGBA, Gl.UNSIGNED_BYTE, hd.AddrOfPinnedObject()); }
			finally { hd.Free(); }
			Gl.BindFramebuffer(Gl.FRAMEBUFFER, 0);
			ThreadPool.QueueUserWorkItem(delegate {
				try {
					string dir = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyPictures), "SPRUT-M");
					System.IO.Directory.CreateDirectory(dir);
					string file = System.IO.Path.Combine(dir, "sprutm_" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff") + ".png");
					SavePng(px, w, h, file);
					var e = Snapshot; if (e != null) e(file);
				} catch (Exception ex) {
					var e = Snapshot; if (e != null) e("!" + ex.Message);
				}
			});
		} catch (Exception ex) {
			var e = Snapshot; if (e != null) e("!" + ex.Message);
		}
	}

	void SaveTest()
	{
		System.IO.Directory.CreateDirectory(TestDir);
		var px = new byte[winW * winH * 4];
		Gl.BindFramebuffer(Gl.FRAMEBUFFER, 0);
		Gl.ReadBuffer(Gl.BACK);
		var h = GCHandle.Alloc(px, GCHandleType.Pinned);
		try { Gl.ReadPixels(0, 0, winW, winH, Gl.RGBA, Gl.UNSIGNED_BYTE, h.AddrOfPinnedObject()); } finally { h.Free(); }
		SavePng(px, winW, winH, System.IO.Path.Combine(TestDir, "window.png"));
		var sp = new byte[srcW * srcH * 4];
		Gl.BindFramebuffer(Gl.FRAMEBUFFER, fb[2]);
		h = GCHandle.Alloc(sp, GCHandleType.Pinned);
		try { Gl.ReadPixels(0, 0, srcW, srcH, Gl.RGBA, Gl.UNSIGNED_BYTE, h.AddrOfPinnedObject()); } finally { h.Free(); }
		Gl.BindFramebuffer(Gl.FRAMEBUFFER, 0);
		SavePng(sp, srcW, srcH, System.IO.Path.Combine(TestDir, "source.png"));
	}

	static void SavePng(byte[] px, int w, int h, string file)
	{
		using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppRgb)) {
			var bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
			var line = new byte[w * 4];
			for (int y = 0; y < h; y++) {
				Buffer.BlockCopy(px, (h - 1 - y) * w * 4, line, 0, line.Length);
				for (int x = 0; x < w; x++) { byte t = line[x * 4]; line[x * 4] = line[x * 4 + 2]; line[x * 4 + 2] = t; }
				Marshal.Copy(line, 0, bd.Scan0 + y * bd.Stride, line.Length);
			}
			bmp.UnlockBits(bd);
			bmp.Save(file, ImageFormat.Png);
		}
	}

	void WriteTestStats()
	{
		var sb = new System.Text.StringBuilder();
		Func<System.Collections.Generic.List<double>, string> sum = l => {
			if (l.Count == 0) return "-";
			var a = l.ToArray(); Array.Sort(a);
			return string.Format(System.Globalization.CultureInfo.InvariantCulture, "n={0} avg={1:0.00} p50={2:0.00} p95={3:0.00} max={4:0.00}",
				a.Length, System.Linq.Enumerable.Average(a), a[a.Length / 2], a[(int)(a.Length * 0.95)], a[a.Length - 1]);
		};
		sb.AppendLine("gpu: " + St.Gpu);
		sb.AppendLine("window: " + winW + "x" + winH + " out " + string.Join(",", outRect) + " src " + srcW + "x" + srcH);
		sb.AppendLine("latency_ms (arrival->swap+finish): " + sum(testLat));
		sb.AppendLine("render_ms (draw+swap+finish): " + sum(testGpu));
		sb.AppendLine("fields: " + St.Fields + " dropped: " + St.Dropped + " refresh: " + St.Refresh);
		System.IO.File.WriteAllText(System.IO.Path.Combine(TestDir, "stats.txt"), sb.ToString());
	}

	// ---------------------------------------------------------------- thread
	void Run()
	{
		try {
			InitGL();
		} catch (Exception e) {
			Log.W("GL init failed: " + e);
			var h = Failed; if (h != null) h(e.Message);
			return;
		}
		Log.W("GL: " + St.Gpu + " / " + Gl.GetString(Gl.VENDOR));
		int interval = -1;
		long statT = Stopwatch.GetTimestamp(), nShown = 0;
		double latSum = 0, latMax = 0, gpuSum = 0;
		var ti = new DWM_TIMING_INFO { cbSize = Marshal.SizeOf(typeof(DWM_TIMING_INFO)) };
		while (!stop) {
			wake.WaitOne(100);
			if (stop) break;
			try {
			FieldBuf f, last = null;
			int n = 0;
			while (queue.TryDequeue(out f)) { OnField(f); last = f; n++; }
			if (last == null && !redraw) {
				if (Stopwatch.GetTimestamp() - statT > Stopwatch.Frequency / 4) goto stats;
				continue;
			}
			redraw = false;
			if (!haveField && last == null) {
				if (snapReq) { snapReq = false; var e = Snapshot; if (e != null) e("!ещё нет картинки"); }
				Layout();
				Gl.BindFramebuffer(Gl.FRAMEBUFFER, 0);
				Gl.Viewport(0, 0, winW, winH);
				Gl.ClearColor(0, 0, 0, 1);
				Gl.Clear(Gl.COLOR_BUFFER_BIT);
				Gl.SwapBuffers(hdc);
				continue;
			}
			int want = Fullscreen && S.NoTear == 1 ? 1 : 0;
			if (want != interval && Gl.SwapIntervalEXT != null) { Gl.SwapIntervalEXT(want); interval = want; }
			long t0 = Stopwatch.GetTimestamp();
			Draw();
			if (snapReq) { snapReq = false; SaveSnapshot(); }
			bool testNow = TestDir != null && St.Fields + n >= TestFields;
			if (testNow) SaveTest();
			Gl.SwapBuffers(hdc);
			Gl.Finish();
			long t1 = Stopwatch.GetTimestamp();
			if (last != null) {
				St.Fields += n;
				double lat = (t1 - last.ArriveTicks) * 1000.0 / Stopwatch.Frequency;
				latSum += lat; if (lat > latMax) latMax = lat;
				gpuSum += (t1 - t0) * 1000.0 / Stopwatch.Frequency;
				nShown++;
				if (TestDir != null) { testLat.Add(lat); testGpu.Add((t1 - t0) * 1000.0 / Stopwatch.Frequency); }
			}
			if (testNow) {
				WriteTestStats();
				TestDir = null;
				var td = TestDone; if (td != null) td();
			}
			// (only if that buffer is still held: a later field of the same parity in this batch recycles it)
			if (histDue != null && (histDue == prevField[0] || histDue == prevField[1])) Histogram(histDue);
			histDue = null;
		stats:
			long now = Stopwatch.GetTimestamp();
			double dt = (double)(now - statT) / Stopwatch.Frequency;
			if (dt >= 0.25) {
				lock (St) {
					St.FieldsPerSec = (St.Fields - fieldsAtStat) / dt;
					fieldsAtStat = St.Fields;
					St.LatAvgMs = nShown > 0 ? latSum / nShown : 0;
					St.LatMaxMs = latMax;
					St.GpuMs = nShown > 0 ? gpuSum / nShown : 0;
					if (DwmGetCompositionTimingInfo(IntPtr.Zero, ref ti) == 0 && ti.rateRefreshDen != 0)
						St.Refresh = (double)ti.rateRefreshNum / ti.rateRefreshDen;
				}
				latSum = latMax = gpuSum = 0; nShown = 0; statT = now;
				var e = StatsReady; if (e != null) e();
			}
			} catch (Exception ex) {
				if (Stopwatch.GetTimestamp() - lastErrLog > Stopwatch.Frequency) { Log.W("render: " + ex); lastErrLog = Stopwatch.GetTimestamp(); }
				Thread.Sleep(50);
			}
		}
		try { Gl.wglMakeCurrent(IntPtr.Zero, IntPtr.Zero); Gl.wglDeleteContext(ctx); Gl.ReleaseDC(hwnd, hdc); } catch { }
	}

	long fieldsAtStat, lastErrLog;
}
