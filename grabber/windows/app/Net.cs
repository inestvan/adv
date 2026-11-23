// The link to the Pi: the same WebSocket the web page uses (grabber/web.h), on a plain TCP socket.
// Server -> us: binary messages = one field each (16-byte header + 720 x 288 UYVY), text = JSON (stat, ctrls, taken).
// Us -> server: masked text frames "ctrl <id> <val>", "ctrls", "take".
// Connecting takes the video from any other window (the Pi serves one viewer at a time).

using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

sealed class FieldBuf
{
	public byte[] Data;		// header (16 bytes) + UYVY lines
	public int Len;
	public long ArriveTicks;	// Stopwatch ticks when the last byte arrived
	public uint Seq;
	public int Top, Locked, Width, Lines;
}

sealed class PiLink
{
	public static readonly string[] Hosts = { "169.254.10.1", "adv.local" };
	public const int Port = 80;

	public event Action<FieldBuf> Field;		// on the network thread
	public event Action<string> Text;		// on the network thread
	public event Action<string, bool> State;	// text for the user, connected?

	public string Host { get; private set; }
	public string FixedHost;			// --host
	readonly ConcurrentBag<FieldBuf> pool = new ConcurrentBag<FieldBuf>();
	Thread th;
	volatile bool stop;
	Socket sock;
	readonly object sendLock = new object();
	readonly Random rnd = new Random();
	long lastRx;			// Stopwatch ticks of the last message from the Pi
	long lastPoke;
	volatile bool taken;		// another window has the video: the Pi sends this window nothing
	Timer watchdog;

	public bool Taken { get { return taken; } }

	// "Смотреть здесь": take the video back
	public void TakeBack()
	{
		Interlocked.Exchange(ref lastRx, Stopwatch.GetTimestamp());
		taken = false;
		Send("take");
	}

	public void Start()
	{
		th = new Thread(Run) { IsBackground = true, Name = "PiLink", Priority = ThreadPriority.AboveNormal };
		th.Start();
		// a link that went silent (Pi switched off, cable out) is closed after 5 s. While another window has the video
		// the Pi sends this one nothing, so it is asked for the control list every 2 s (the Pi answers paused windows too)
		watchdog = new Timer(delegate {
			var s = sock;
			if (s == null) return;
			long now = Stopwatch.GetTimestamp();
			if (taken && now - lastPoke > 2 * Stopwatch.Frequency) { lastPoke = now; Send("ctrls"); }
			// the Pi sends at least a status a second (2.5 s of silence = the link is gone); a paused window only gets the
			// answers to its "ctrls" every 2 s (5 s)
			if (now - Interlocked.Read(ref lastRx) > (taken ? 5 : 2.5) * Stopwatch.Frequency)
				try { Log.W("link: silent, reconnecting"); s.Close(); } catch { }
			// on Wi-Fi (adv.local) after the cable was pulled: as soon as the cable answers again, go back to it
			// (not while another window has the video: reconnecting would take it from that window)
			if (FixedHost == null && Host != Hosts[0] && !taken && !probing && now - lastCableProbe > 2 * Stopwatch.Frequency) {
				lastCableProbe = now;
				probing = true;
				ThreadPool.QueueUserWorkItem(delegate {
					try {
						if (CableAnswers() && sock == s) {
							Log.W("link: the LAN cable answers again, switching from " + Host + " to " + Hosts[0]);
							toCable = true;
							try { s.Close(); } catch { }
						}
					} finally { probing = false; }
				});
			}
		}, null, 500, 500);
	}

	long lastCableProbe;
	volatile bool probing, toCable;		// toCable: reconnect over the cable at once, no pause

	// a quick TCP connect to the Pi over the cable (the web server just sees a connection that closes)
	static bool CableAnswers()
	{
		var c = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
		try {
			var ar = c.BeginConnect(IPAddress.Parse(Hosts[0]), Port, null, null);
			if (!ar.AsyncWaitHandle.WaitOne(500)) return false;
			c.EndConnect(ar);
			return true;
		} catch {
			return false;
		} finally {
			try { c.Close(); } catch { }
		}
	}

	public void Stop()
	{
		stop = true;
		if (watchdog != null) watchdog.Dispose();
		var s = sock;
		if (s != null) try { s.Close(); } catch { }
	}

	public void Recycle(FieldBuf f) { if (f != null) pool.Add(f); }

	FieldBuf Rent(int len)
	{
		FieldBuf f;
		if (!pool.TryTake(out f)) f = new FieldBuf();
		if (f.Data == null || f.Data.Length < len) f.Data = new byte[Math.Max(len, 16 + 720 * 2 * 312)];
		f.Len = len;
		return f;
	}

	void Say(string s, bool ok) { Log.W("link: " + s); var h = State; if (h != null) h(s, ok); }

	void Run()
	{
		int attempt = 0;
		while (!stop) {
			string[] hosts = FixedHost != null ? new[] { FixedHost } : Hosts;
			foreach (var host in hosts) {
				if (stop) return;
				Say(attempt == 0 ? "Подключаюсь к Pi…" : "Нет связи с Pi, пробую снова (" + host + ")…", false);
				try {
					Session(host);
				} catch (Exception e) {
					if (stop) return;
					Log.W("link " + host + ": " + e.Message);
				}
			}
			attempt++;
			for (int i = 0; i < 10 && !stop && !toCable; i++) Thread.Sleep(100);
			toCable = false;
		}
	}

	// a name lookup (adv.local) can hang for seconds on Windows fallbacks: wait at most 1.5 s
	static IPAddress Resolve(string host)
	{
		IPAddress ip;
		if (IPAddress.TryParse(host, out ip)) return ip;
		var ar = Dns.BeginGetHostAddresses(host, null, null);
		if (!ar.AsyncWaitHandle.WaitOne(1500)) throw new IOException("имя " + host + " не нашлось");
		foreach (var a in Dns.EndGetHostAddresses(ar))
			if (a.AddressFamily == AddressFamily.InterNetwork) return a;
		throw new IOException("нет адреса " + host);
	}

	void Session(string host)
	{
		int port = Port, colon = host.LastIndexOf(':');
		string name = host;
		if (colon > 0 && int.TryParse(host.Substring(colon + 1), out port)) name = host.Substring(0, colon);
		else port = Port;
		var ip = Resolve(name);
		var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
		s.NoDelay = true;
		s.ReceiveBufferSize = 4 << 20;
		var ar = s.BeginConnect(ip, port, null, null);
		if (!ar.AsyncWaitHandle.WaitOne(1500)) { s.Close(); throw new IOException("нет ответа"); }
		try { s.EndConnect(ar); } catch { s.Close(); throw; }
		Interlocked.Exchange(ref lastRx, Stopwatch.GetTimestamp());	// before the watchdog can see the socket
		taken = false;
		sock = s;
		try {
			var ns = new NetworkStream(s, false);
			var key = Convert.ToBase64String(Guid.NewGuid().ToByteArray());
			var req = "GET /ws HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
				"Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
			var rb = Encoding.ASCII.GetBytes(req);
			ns.Write(rb, 0, rb.Length);
			s.ReceiveTimeout = 3000;
			var head = new StringBuilder();
			var one = new byte[1];
			while (!head.ToString().EndsWith("\r\n\r\n")) {
				if (ns.Read(one, 0, 1) != 1) throw new IOException("соединение закрыто");
				head.Append((char)one[0]);
				if (head.Length > 4096) throw new IOException("плохой ответ");
			}
			if (!head.ToString().StartsWith("HTTP/1.1 101")) throw new IOException("Pi не принял WebSocket");
			string accept = Convert.ToBase64String(SHA1.Create().ComputeHash(
				Encoding.ASCII.GetBytes(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
			if (head.ToString().IndexOf(accept, StringComparison.Ordinal) < 0) throw new IOException("плохой ключ WebSocket");
			Host = host;
			s.ReceiveTimeout = 0;
			Interlocked.Exchange(ref lastRx, Stopwatch.GetTimestamp());
			Say("подключено", true);
			ReadLoop(ns);
		} finally {
			sock = null;
			try { s.Close(); } catch { }
			Say("Нет связи с Pi…", false);
		}
	}

	static void ReadFull(Stream st, byte[] b, int off, int n)
	{
		while (n > 0) {
			int r = st.Read(b, off, n);
			if (r <= 0) throw new IOException("соединение закрыто");
			off += r;
			n -= r;
		}
	}

	void ReadLoop(NetworkStream ns)
	{
		var st = new BufferedStream(ns, 1 << 20);
		var h = new byte[14];
		byte[] ctl = new byte[256];
		while (!stop) {
			ReadFull(st, h, 0, 2);
			Interlocked.Exchange(ref lastRx, Stopwatch.GetTimestamp());
			int op = h[0] & 15;
			bool masked = (h[1] & 0x80) != 0;
			long len = h[1] & 127;
			if (len == 126) { ReadFull(st, h, 2, 2); len = (h[2] << 8) | h[3]; }
			else if (len == 127) {
				ReadFull(st, h, 2, 8);
				len = 0;
				for (int i = 0; i < 8; i++) len = (len << 8) | h[2 + i];
			}
			if (len > 4 << 20) throw new IOException("слишком длинное сообщение");
			byte[] mask = null;
			if (masked) { mask = new byte[4]; ReadFull(st, mask, 0, 4); }
			if (op == 2) {
				var f = Rent((int)len);
				ReadFull(st, f.Data, 0, (int)len);
				f.ArriveTicks = Stopwatch.GetTimestamp();
				if (masked) Unmask(f.Data, (int)len, mask);
				if (len < 16 || f.Data[0] != (byte)'F') { Recycle(f); continue; }
				f.Top = f.Data[1] & 1;
				f.Locked = (f.Data[1] >> 1) & 1;
				f.Width = f.Data[2] | (f.Data[3] << 8);
				f.Lines = f.Data[4] | (f.Data[5] << 8);
				f.Seq = (uint)(f.Data[8] | (f.Data[9] << 8) | (f.Data[10] << 16) | (f.Data[11] << 24));
				if (f.Width <= 0 || f.Width > 1024 || f.Lines <= 0 || f.Lines > 320 || len < 16 + f.Width * 2 * f.Lines) {
					Recycle(f);
					continue;
				}
				var fh = Field;
				if (fh != null) fh(f); else Recycle(f);
			} else if (op == 1 || op == 8 || op == 9 || op == 10) {
				var b = len <= ctl.Length ? ctl : new byte[len];
				ReadFull(st, b, 0, (int)len);
				if (masked) Unmask(b, (int)len, mask);
				if (op == 1) {
					if (len < 40 && Encoding.ASCII.GetString(b, 0, (int)len).Contains("\"taken\"")) taken = true;
					var th = Text;
					if (th != null) th(Encoding.UTF8.GetString(b, 0, (int)len));
				} else if (op == 9) {
					SendFrame(10, b, (int)len);
				} else if (op == 8) {
					throw new IOException("Pi закрыл соединение");
				}
			} else {
				var skip = new byte[len];
				ReadFull(st, skip, 0, (int)len);
			}
		}
	}

	static void Unmask(byte[] b, int n, byte[] m) { for (int i = 0; i < n; i++) b[i] ^= m[i & 3]; }

	public void Send(string text)
	{
		var b = Encoding.UTF8.GetBytes(text);
		try { SendFrame(1, b, b.Length); } catch { }
	}

	void SendFrame(int op, byte[] payload, int n)
	{
		var s = sock;
		if (s == null) return;
		var f = new byte[2 + (n >= 126 ? 2 : 0) + 4 + n];
		int p = 0;
		f[p++] = (byte)(0x80 | op);
		if (n < 126) f[p++] = (byte)(0x80 | n);
		else { f[p++] = 0x80 | 126; f[p++] = (byte)(n >> 8); f[p++] = (byte)n; }
		var m = new byte[4];
		lock (rnd) rnd.NextBytes(m);
		Buffer.BlockCopy(m, 0, f, p, 4);
		p += 4;
		for (int i = 0; i < n; i++) f[p + i] = (byte)(payload[i] ^ m[i & 3]);
		lock (sendLock) s.Send(f);
	}
}
