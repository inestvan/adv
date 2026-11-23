// OpenGL 3.3 core from C# with nothing but P/Invoke: opengl32.dll exports the GL 1.1 functions, everything newer is
// loaded through wglGetProcAddress into delegates. Only what the viewer uses.

using System;
using System.Runtime.InteropServices;
using System.Text;

static class Gl
{
	public const int TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0, RGBA = 0x1908, RGBA8 = 0x8058, RGBA16F = 0x881A,
		UNSIGNED_BYTE = 0x1401, HALF_FLOAT = 0x140B, FLOAT = 0x1406, NEAREST = 0x2600, LINEAR = 0x2601,
		TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803,
		CLAMP_TO_EDGE = 0x812F, FRAMEBUFFER = 0x8D40, READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9,
		COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5, TRIANGLES = 0x0004, COLOR_BUFFER_BIT = 0x4000,
		UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05, VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30,
		COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, INFO_LOG_LENGTH = 0x8B84, VENDOR = 0x1F00, RENDERER = 0x1F01,
		VERSION = 0x1F02, SHADING_LANGUAGE_VERSION = 0x8B8C, TIMESTAMP = 0x8E28, QUERY_RESULT = 0x8866,
		QUERY_RESULT_AVAILABLE = 0x8867, DITHER = 0x0BD0, FRAMEBUFFER_SRGB = 0x8DB9, BACK = 0x0405;

	// ---------------------------------------------------------------- opengl32.dll (GL 1.1)
	const string GL = "opengl32.dll";
	[DllImport(GL, EntryPoint = "glViewport")] public static extern void Viewport(int x, int y, int w, int h);
	[DllImport(GL, EntryPoint = "glClearColor")] public static extern void ClearColor(float r, float g, float b, float a);
	[DllImport(GL, EntryPoint = "glClear")] public static extern void Clear(int mask);
	[DllImport(GL, EntryPoint = "glGetString")] static extern IntPtr glGetString(int name);
	[DllImport(GL, EntryPoint = "glGetError")] public static extern int GetError();
	[DllImport(GL, EntryPoint = "glGenTextures")] static extern void glGenTextures(int n, int[] t);
	[DllImport(GL, EntryPoint = "glBindTexture")] public static extern void BindTexture(int target, int tex);
	[DllImport(GL, EntryPoint = "glTexParameteri")] public static extern void TexParameteri(int target, int p, int v);
	[DllImport(GL, EntryPoint = "glTexImage2D")] public static extern void TexImage2D(int target, int level, int ifmt, int w, int h,
		int border, int fmt, int type, IntPtr data);
	[DllImport(GL, EntryPoint = "glTexSubImage2D")] public static extern void TexSubImage2D(int target, int level, int x, int y, int w,
		int h, int fmt, int type, IntPtr data);
	[DllImport(GL, EntryPoint = "glDrawArrays")] public static extern void DrawArrays(int mode, int first, int count);
	[DllImport(GL, EntryPoint = "glPixelStorei")] public static extern void PixelStorei(int p, int v);
	[DllImport(GL, EntryPoint = "glReadPixels")] public static extern void ReadPixels(int x, int y, int w, int h, int fmt, int type,
		IntPtr data);
	[DllImport(GL, EntryPoint = "glFinish")] public static extern void Finish();
	[DllImport(GL, EntryPoint = "glFlush")] public static extern void Flush();
	[DllImport(GL, EntryPoint = "glEnable")] public static extern void Enable(int cap);
	[DllImport(GL, EntryPoint = "glDisable")] public static extern void Disable(int cap);
	[DllImport(GL, EntryPoint = "glReadBuffer")] public static extern void ReadBuffer(int mode);

	public static string GetString(int name)
	{
		IntPtr p = glGetString(name);
		return p == IntPtr.Zero ? "" : Marshal.PtrToStringAnsi(p);
	}
	public static int GenTexture() { var t = new int[1]; glGenTextures(1, t); return t[0]; }

	// ---------------------------------------------------------------- WGL / GDI
	[DllImport(GL)] public static extern IntPtr wglCreateContext(IntPtr hdc);
	[DllImport(GL)] public static extern bool wglMakeCurrent(IntPtr hdc, IntPtr ctx);
	[DllImport(GL)] public static extern bool wglDeleteContext(IntPtr ctx);
	[DllImport(GL)] static extern IntPtr wglGetProcAddress(string name);
	[DllImport("gdi32.dll")] public static extern int ChoosePixelFormat(IntPtr hdc, ref PIXELFORMATDESCRIPTOR pfd);
	[DllImport("gdi32.dll")] public static extern bool SetPixelFormat(IntPtr hdc, int fmt, ref PIXELFORMATDESCRIPTOR pfd);
	[DllImport("gdi32.dll")] public static extern bool SwapBuffers(IntPtr hdc);
	[DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hwnd);
	[DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);
	[DllImport("kernel32.dll", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr mod, string name);
	[DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr LoadLibrary(string name);

	[StructLayout(LayoutKind.Sequential)]
	public struct PIXELFORMATDESCRIPTOR
	{
		public short nSize, nVersion; public int dwFlags; public byte iPixelType, cColorBits, cRedBits, cRedShift,
		cGreenBits, cGreenShift, cBlueBits, cBlueShift, cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits,
		cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits, cStencilBits, cAuxBuffers, iLayerType, bReserved;
		public int dwLayerMask, dwVisibleMask, dwDamageMask;
	}

	// ---------------------------------------------------------------- GL 2.0+ through wglGetProcAddress
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate IntPtr D_CreateContextAttribs(IntPtr hdc, IntPtr share, int[] attr);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate bool D_SwapInterval(int i);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate int D_I_I(int a);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate int D_I();
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_V_I(int a);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_V_II(int a, int b);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_V_IIII(int a, int b, int c, int d);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_V_IIIII(int a, int b, int c, int d, int e);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_V_IF(int a, float b);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_V_IFF(int a, float b, float c);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_V_IFFFF(int a, float b, float c, float d, float e);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_Gen(int n, int[] ids);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_ShaderSource(int s, int n, string[] src, int[] len);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_GetIv(int o, int p, out int v);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_GetLog(int o, int max, out int len, StringBuilder log);
	[UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Ansi)] public delegate int D_GetUniform(int p, string name);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_GetQ64(int id, int p, out long v);
	[UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate void D_Blit(int sx0, int sy0, int sx1, int sy1, int dx0,
		int dy0, int dx1, int dy1, int mask, int filter);

	public static D_I_I CreateShader;
	public static D_I CreateProgram;
	public static D_ShaderSource ShaderSource;
	public static D_V_I CompileShader, LinkProgram, UseProgram, ActiveTexture, BindVertexArray;
	public static D_V_II AttachShader, BindFramebuffer, QueryCounter;
	public static D_GetIv GetShaderiv, GetProgramiv;
	public static D_GetLog GetShaderInfoLog, GetProgramInfoLog;
	public static D_GetUniform GetUniformLocation;
	public static D_V_II Uniform1i;
	public static D_V_IF Uniform1f;
	public static D_V_IFF Uniform2f;
	public static D_V_IFFFF Uniform4f;
	public static D_Gen GenFramebuffers, GenVertexArrays, GenQueries;
	public static D_V_IIIII FramebufferTexture2D;
	public static D_I_I CheckFramebufferStatus;
	public static D_GetQ64 GetQueryObjecti64v;
	public static D_Blit BlitFramebuffer;
	public static D_SwapInterval SwapIntervalEXT;
	public static D_CreateContextAttribs CreateContextAttribsARB;

	static IntPtr gl32;
	static IntPtr Proc(string name)
	{
		IntPtr p = wglGetProcAddress(name);
		long v = p.ToInt64();
		if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) {	// not a WGL extension: maybe a GL 1.1 export
			if (gl32 == IntPtr.Zero) gl32 = LoadLibrary("opengl32.dll");
			p = GetProcAddress(gl32, name);
		}
		return p;
	}
	static T Load<T>(string name) where T : class
	{
		IntPtr p = Proc(name);
		if (p == IntPtr.Zero) throw new Exception("OpenGL: нет функции " + name);
		return (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
	}
	static T LoadOpt<T>(string name) where T : class
	{
		IntPtr p = Proc(name);
		return p == IntPtr.Zero ? null : (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
	}

	// needs a current context (a legacy one is enough for these two)
	public static void LoadWgl()
	{
		CreateContextAttribsARB = LoadOpt<D_CreateContextAttribs>("wglCreateContextAttribsARB");
		SwapIntervalEXT = LoadOpt<D_SwapInterval>("wglSwapIntervalEXT");
	}

	public static void LoadAll()
	{
		CreateShader = Load<D_I_I>("glCreateShader");
		CreateProgram = Load<D_I>("glCreateProgram");
		ShaderSource = Load<D_ShaderSource>("glShaderSource");
		CompileShader = Load<D_V_I>("glCompileShader");
		LinkProgram = Load<D_V_I>("glLinkProgram");
		UseProgram = Load<D_V_I>("glUseProgram");
		ActiveTexture = Load<D_V_I>("glActiveTexture");
		BindVertexArray = Load<D_V_I>("glBindVertexArray");
		AttachShader = Load<D_V_II>("glAttachShader");
		BindFramebuffer = Load<D_V_II>("glBindFramebuffer");
		GetShaderiv = Load<D_GetIv>("glGetShaderiv");
		GetProgramiv = Load<D_GetIv>("glGetProgramiv");
		GetShaderInfoLog = Load<D_GetLog>("glGetShaderInfoLog");
		GetProgramInfoLog = Load<D_GetLog>("glGetProgramInfoLog");
		GetUniformLocation = Load<D_GetUniform>("glGetUniformLocation");
		Uniform1i = Load<D_V_II>("glUniform1i");
		Uniform1f = Load<D_V_IF>("glUniform1f");
		Uniform2f = Load<D_V_IFF>("glUniform2f");
		Uniform4f = Load<D_V_IFFFF>("glUniform4f");
		GenFramebuffers = Load<D_Gen>("glGenFramebuffers");
		GenVertexArrays = Load<D_Gen>("glGenVertexArrays");
		FramebufferTexture2D = Load<D_V_IIIII>("glFramebufferTexture2D");
		CheckFramebufferStatus = Load<D_I_I>("glCheckFramebufferStatus");
		BlitFramebuffer = Load<D_Blit>("glBlitFramebuffer");
		GenQueries = LoadOpt<D_Gen>("glGenQueries");
		QueryCounter = LoadOpt<D_V_II>("glQueryCounter");
		GetQueryObjecti64v = LoadOpt<D_GetQ64>("glGetQueryObjecti64v");
	}

	public static int GenOne(D_Gen f) { var a = new int[1]; f(1, a); return a[0]; }

	static int Compile(int type, string src)
	{
		int s = CreateShader(type);
		ShaderSource(s, 1, new[] { src }, null);
		CompileShader(s);
		int ok;
		GetShaderiv(s, COMPILE_STATUS, out ok);
		if (ok == 0) {
			var log = new StringBuilder(8192);
			int n;
			GetShaderInfoLog(s, log.Capacity, out n, log);
			throw new Exception("шейдер не собрался:\n" + log);
		}
		return s;
	}

	public static int Program(string vs, string fs)
	{
		int p = CreateProgram();
		AttachShader(p, Compile(VERTEX_SHADER, vs));
		AttachShader(p, Compile(FRAGMENT_SHADER, fs));
		LinkProgram(p);
		int ok;
		GetProgramiv(p, LINK_STATUS, out ok);
		if (ok == 0) {
			var log = new StringBuilder(8192);
			int n;
			GetProgramInfoLog(p, log.Capacity, out n, log);
			throw new Exception("программа шейдеров не собралась:\n" + log);
		}
		return p;
	}
}
