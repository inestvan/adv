/* web.h - a tiny HTTP + WebSocket server for sprutm-uvc: serves the viewer page (www/index.html) and streams
 * every decoder field to the browser uncompressed (UYVY, bit-exact). Single-threaded, driven by the main poll().
 *
 * WebSocket messages, server -> browser:
 *   binary: 16-byte header + field.  [0] 'F'  [1] flags: bit0 top field, bit1 parity locked
 *           [2..3] width  [4..5] lines  [6..7] 0  [8..11] field sequence  [12..15] 0   (little endian)
 *           then width * 2 * lines bytes UYVY (U Y0 V Y1), rows packed.
 *   text:   JSON, {"type":"stat",...} once a second and {"type":"ctrls",...} on connect / after a change.
 * browser -> server (text): "ctrl <id> <value>"  (V4L2 control of the decoder), "ctrls" (resend the list).
 *
 * A client that has not finished receiving the previous message skips fields (counted), so a slow link (Wi-Fi)
 * shows fewer fields instead of building up delay. Gigabit Ethernet carries all 50 fields (20.7 MB/s).
 */
#ifndef SPRUTM_WEB_H
#define SPRUTM_WEB_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define WEB_MAX_CLIENTS	6
#define WEB_FIELD_HDR	16
#define WEB_SNDBUF	(2 * 1024 * 1024)

struct web_client {
	int fd;
	int ws;				/* 0: reading the HTTP request, 1: WebSocket */
	int close_after;		/* HTTP response: close once sent */
	uint8_t in[4096];
	size_t in_len;
	uint8_t *out;
	size_t out_cap, out_len, out_off;
	char *txt[2];			/* pending text, one per kind (0 status, 1 control list), sent before fields */
	unsigned long fields_sent, fields_skipped;
	int paused;			/* another window took the video (web_take): no fields, no status */
};

static struct {
	int listen_fd;
	char www[200];
	struct web_client c[WEB_MAX_CLIENTS];
	int map[WEB_MAX_CLIENTS + 1];	/* pollfd slot -> client index (-1 = listen socket) */
	int nmap;
	void (*on_open)(struct web_client *);
	void (*on_text)(struct web_client *, char *msg);
} web = { .listen_fd = -1 };

/* ------------------------------------------------------------------ SHA-1 + base64 (WebSocket handshake only) */

static void web_sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
	uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
	size_t total = ((len + 8) / 64 + 1) * 64;
	uint8_t blk[64];
	for (size_t off = 0; off < total; off += 64) {
		for (size_t i = 0; i < 64; i++) {
			size_t k = off + i;
			uint8_t b = 0;
			if (k < len)
				b = msg[k];
			else if (k == len)
				b = 0x80;
			else if (k >= total - 8)
				b = (uint8_t)(((uint64_t)len * 8) >> (8 * (total - 1 - k)));
			blk[i] = b;
		}
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 | (uint32_t)blk[4 * i + 2] << 8 |
			       blk[4 * i + 3];
		for (int i = 16; i < 80; i++) {
			uint32_t t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
			w[i] = t << 1 | t >> 31;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5A827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDC;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6;
			}
			uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
			e = d;
			d = c;
			c = b << 30 | b >> 2;
			b = a;
			a = t;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 4; j++)
			out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

static void web_base64(const uint8_t *in, size_t len, char *out)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) |
			     (i + 2 < len ? in[i + 2] : 0);
		out[o++] = t[v >> 18 & 63];
		out[o++] = t[v >> 12 & 63];
		out[o++] = i + 1 < len ? t[v >> 6 & 63] : '=';
		out[o++] = i + 2 < len ? t[v & 63] : '=';
	}
	out[o] = 0;
}

/* ------------------------------------------------------------------ connections */

static int web_init(int port, const char *www)
{
	snprintf(web.www, sizeof web.www, "%s", www);
	for (int i = 0; i < WEB_MAX_CLIENTS; i++)
		web.c[i].fd = -1;
	int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	int one = 1, zero = 0;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);	/* IPv4 too */
	struct sockaddr_in6 a;
	memset(&a, 0, sizeof a);
	a.sin6_family = AF_INET6;
	a.sin6_addr = in6addr_any;
	a.sin6_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	web.listen_fd = fd;
	return 0;
}

static void web_close(struct web_client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	free(c->out);
	free(c->txt[0]);
	free(c->txt[1]);
	memset(c, 0, sizeof *c);
	c->fd = -1;
}

static int web_idle(const struct web_client *c)
{
	return c->out_off >= c->out_len;
}

static void web_flush(struct web_client *c)
{
	while (c->out_off < c->out_len) {
		ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return;
			web_close(c);
			return;
		}
		c->out_off += (size_t)n;
	}
	if (c->close_after) {
		web_close(c);
		return;
	}
}

/* Queue raw bytes (the client must be idle). head + body are concatenated. */
static int web_queue(struct web_client *c, const void *head, size_t hlen, const void *body, size_t blen)
{
	size_t need = hlen + blen;
	if (need > c->out_cap) {
		uint8_t *p = realloc(c->out, need);
		if (!p)
			return -1;
		c->out = p;
		c->out_cap = need;
	}
	memcpy(c->out, head, hlen);
	if (blen)
		memcpy(c->out + hlen, body, blen);
	c->out_len = need;
	c->out_off = 0;
	return 0;
}

/* WebSocket frame header for a server -> client message (never masked) */
static size_t web_ws_header(uint8_t *h, int opcode, size_t len)
{
	h[0] = (uint8_t)(0x80 | opcode);
	if (len < 126) {
		h[1] = (uint8_t)len;
		return 2;
	}
	if (len < 65536) {
		h[1] = 126;
		h[2] = (uint8_t)(len >> 8);
		h[3] = (uint8_t)len;
		return 4;
	}
	h[1] = 127;
	for (int i = 0; i < 8; i++)
		h[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
	return 10;
}

static void web_send_text_now(struct web_client *c, const char *s)
{
	uint8_t h[10];
	size_t n = strlen(s), hl = web_ws_header(h, 1, n);
	if (web_queue(c, h, hl, s, n) == 0)
		web_flush(c);
}

/* Text to one client: sent at once if idle, else kept (a newer message of the same kind replaces it: status by
 * status, control list by control list) and sent before the next field. */
static void web_text(struct web_client *c, const char *s)
{
	if (c->fd < 0 || !c->ws)
		return;
	if (web_idle(c)) {
		web_send_text_now(c, s);
		return;
	}
	int kind = strstr(s, "\"type\":\"ctrls\"") != NULL;
	free(c->txt[kind]);
	c->txt[kind] = strdup(s);
}

/* send one pending text message if the client is idle; 1 if something was sent */
static int web_send_pending(struct web_client *c)
{
	for (int k = 1; k >= 0; k--) {
		if (c->fd < 0 || !web_idle(c) || !c->txt[k])
			continue;
		char *t = c->txt[k];
		c->txt[k] = NULL;
		web_send_text_now(c, t);
		free(t);
		return 1;
	}
	return 0;
}

static void web_text_all(const char *s)
{
	for (int i = 0; i < WEB_MAX_CLIENTS; i++)
		web_text(&web.c[i], s);
}

static int web_clients(void)
{
	int n = 0;
	for (int i = 0; i < WEB_MAX_CLIENTS; i++)
		n += web.c[i].fd >= 0 && web.c[i].ws && !web.c[i].paused;
	return n;
}

/* One viewer at a time: each full stream is ~21 MB/s, and two windows on one laptop made both of them lag (the
 * second one's socket queue grew to ~17 ms of video). The window that connects (or asks with "take") gets the
 * video; every other one is paused and told so, and can take it back with a button. */
static void web_take(struct web_client *keep)
{
	keep->paused = 0;
	for (int i = 0; i < WEB_MAX_CLIENTS; i++) {
		struct web_client *c = &web.c[i];
		if (c == keep || c->fd < 0 || !c->ws || c->paused)
			continue;
		c->paused = 1;
		web_text(c, "{\"type\":\"taken\"}");
	}
}

/* One field to every WebSocket client that is ready for it. */
static void web_field(uint32_t seq, const uint8_t *data, unsigned w, unsigned lines, unsigned bpl, int top, int locked)
{
	size_t row = (size_t)w * 2, body = row * lines, len = WEB_FIELD_HDR + body;
	for (int i = 0; i < WEB_MAX_CLIENTS; i++) {
		struct web_client *c = &web.c[i];
		if (c->fd < 0 || !c->ws)
			continue;
		if (c->paused) {		/* still deliver its pending text ("taken") */
			web_send_pending(c);
			continue;
		}
		if (!web_idle(c)) {
			c->fields_skipped++;
			continue;
		}
		if (web_send_pending(c)) {	/* pending text first; this field is skipped for this client */
			c->fields_skipped++;
			continue;
		}
		uint8_t h[10];
		size_t hl = web_ws_header(h, 2, len);
		if (hl + len > c->out_cap) {
			uint8_t *p = realloc(c->out, hl + len);
			if (!p)
				continue;
			c->out = p;
			c->out_cap = hl + len;
		}
		uint8_t *o = c->out;
		memcpy(o, h, hl);
		o += hl;
		memset(o, 0, WEB_FIELD_HDR);
		o[0] = 'F';
		o[1] = (uint8_t)((top ? 1 : 0) | (locked ? 2 : 0));
		o[2] = (uint8_t)w;
		o[3] = (uint8_t)(w >> 8);
		o[4] = (uint8_t)lines;
		o[5] = (uint8_t)(lines >> 8);
		for (int k = 0; k < 4; k++)
			o[8 + k] = (uint8_t)(seq >> (8 * k));
		o += WEB_FIELD_HDR;
		if (bpl == row)
			memcpy(o, data, body);
		else
			for (unsigned y = 0; y < lines; y++)
				memcpy(o + y * row, data + (size_t)y * bpl, row);
		c->out_len = hl + len;
		c->out_off = 0;
		c->fields_sent++;
		web_flush(c);
	}
}

/* ------------------------------------------------------------------ HTTP */

static const char *web_header_value(const char *req, const char *name)
{
	size_t n = strlen(name);
	for (const char *p = strstr(req, "\r\n"); p; p = strstr(p, "\r\n")) {
		p += 2;
		if (!strncasecmp(p, name, n) && p[n] == ':') {
			p += n + 1;
			while (*p == ' ' || *p == '\t')
				p++;
			return p;
		}
	}
	return NULL;
}

static void web_http_reply(struct web_client *c, const char *status, const char *type, const void *body, size_t blen)
{
	char h[300];
	int hl = snprintf(h, sizeof h,
			  "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
			  "Connection: close\r\n\r\n", status, type, blen);
	c->close_after = 1;
	if (web_queue(c, h, (size_t)hl, body, blen) == 0)
		web_flush(c);
	else
		web_close(c);
}

static void web_serve_file(struct web_client *c, const char *name, const char *type)
{
	char path[300];
	snprintf(path, sizeof path, "%s/%s", web.www, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		static const char msg[] = "viewer page not found - reinstall (sudo sh install.sh)\n";
		web_http_reply(c, "404 Not Found", "text/plain; charset=utf-8", msg, sizeof msg - 1);
		return;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = n > 0 ? malloc((size_t)n) : NULL;
	size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
	fclose(f);
	web_http_reply(c, "200 OK", type, buf, got);
	free(buf);
}

static void web_handle_request(struct web_client *c)
{
	char *req = (char *)c->in;
	char method[8] = "", path[128] = "";
	if (sscanf(req, "%7s %127s", method, path) != 2 || strcmp(method, "GET")) {
		static const char msg[] = "bad request\n";
		web_http_reply(c, "400 Bad Request", "text/plain", msg, sizeof msg - 1);
		return;
	}
	char *q = strchr(path, '?');
	if (q)
		*q = 0;
	if (!strcmp(path, "/ws")) {
		const char *key = web_header_value(req, "Sec-WebSocket-Key");
		if (!key) {
			static const char msg[] = "websocket only\n";
			web_http_reply(c, "400 Bad Request", "text/plain", msg, sizeof msg - 1);
			return;
		}
		char k[128];
		size_t kl = strcspn(key, "\r\n ");
		if (kl > 60)
			kl = 60;
		memcpy(k, key, kl);
		memcpy(k + kl, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 37);
		uint8_t sha[20];
		char acc[32];
		web_sha1((const uint8_t *)k, kl + 36, sha);
		web_base64(sha, 20, acc);
		char h[256];
		int hl = snprintf(h, sizeof h,
				  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
				  "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
		c->ws = 1;
		c->in_len = 0;
		int sb = WEB_SNDBUF, one = 1;
		setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
		setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		if (web_queue(c, h, (size_t)hl, NULL, 0) == 0)
			web_flush(c);
		if (c->fd >= 0 && web.on_open)
			web.on_open(c);
		return;
	}
	if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
		web_serve_file(c, "index.html", "text/html; charset=utf-8");
		return;
	}
	if (!strcmp(path, "/favicon.ico")) {
		web_http_reply(c, "204 No Content", "text/plain", NULL, 0);
		return;
	}
	static const char msg[] = "not found\n";
	web_http_reply(c, "404 Not Found", "text/plain", msg, sizeof msg - 1);
}

/* parse complete WebSocket frames from the browser (always masked) */
static void web_ws_input(struct web_client *c)
{
	for (;;) {
		if (c->in_len < 2)
			return;
		uint8_t *b = c->in;
		int op = b[0] & 15;
		size_t len = b[1] & 127, hl = 2;
		if (len == 126) {
			if (c->in_len < 4)
				return;
			len = (size_t)b[2] << 8 | b[3];
			hl = 4;
		} else if (len == 127) {
			web_close(c);	/* nothing we accept is that long */
			return;
		}
		if (!(b[1] & 0x80) || hl + 4 + len > sizeof c->in - 1) {
			web_close(c);
			return;
		}
		if (c->in_len < hl + 4 + len)
			return;
		uint8_t *m = b + hl, *p = b + hl + 4;
		for (size_t i = 0; i < len; i++)
			p[i] ^= m[i & 3];
		if (op == 8) {
			web_close(c);
			return;
		}
		if (op == 1 && web.on_text) {
			char msg[sizeof c->in];
			memcpy(msg, p, len);
			msg[len] = 0;
			web.on_text(c, msg);
			if (c->fd < 0)
				return;
		}
		size_t used = hl + 4 + len;
		memmove(c->in, c->in + used, c->in_len - used);
		c->in_len -= used;
	}
}

static void web_read(struct web_client *c)
{
	for (;;) {
		if (c->in_len >= sizeof c->in - 1) {
			web_close(c);
			return;
		}
		ssize_t n = recv(c->fd, c->in + c->in_len, sizeof c->in - 1 - c->in_len, MSG_DONTWAIT);
		if (n == 0) {
			web_close(c);
			return;
		}
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				web_close(c);
			return;
		}
		c->in_len += (size_t)n;
		if (c->ws) {
			web_ws_input(c);
			if (c->fd < 0)
				return;
		} else if (!c->close_after) {
			c->in[c->in_len] = 0;
			if (strstr((char *)c->in, "\r\n\r\n")) {
				web_handle_request(c);
				if (c->fd < 0 || !c->ws)
					return;
			}
		}
	}
}

static void web_accept(void)
{
	for (;;) {
		int fd = accept4(web.listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0)
			return;
		int slot = -1;
		for (int i = 0; i < WEB_MAX_CLIENTS; i++)
			if (web.c[i].fd < 0) {
				slot = i;
				break;
			}
		if (slot < 0) {
			close(fd);
			continue;
		}
		memset(&web.c[slot], 0, sizeof web.c[slot]);
		web.c[slot].fd = fd;
	}
}

/* ------------------------------------------------------------------ poll integration */

static int web_pollfds(struct pollfd *p)
{
	int n = 0;
	web.nmap = 0;
	if (web.listen_fd < 0)
		return 0;
	p[n].fd = web.listen_fd;
	p[n].events = POLLIN;
	p[n].revents = 0;
	web.map[n++] = -1;
	for (int i = 0; i < WEB_MAX_CLIENTS; i++) {
		struct web_client *c = &web.c[i];
		if (c->fd < 0)
			continue;
		p[n].fd = c->fd;
		p[n].events = (short)(POLLIN | (web_idle(c) ? 0 : POLLOUT));
		p[n].revents = 0;
		web.map[n++] = i;
	}
	web.nmap = n;
	return n;
}

static void web_handle(const struct pollfd *p)
{
	for (int k = 0; k < web.nmap; k++) {
		if (!p[k].revents)
			continue;
		if (web.map[k] < 0) {
			web_accept();
			continue;
		}
		struct web_client *c = &web.c[web.map[k]];
		if (c->fd != p[k].fd)
			continue;
		if (p[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			web_close(c);
			continue;
		}
		if (p[k].revents & POLLOUT) {
			web_flush(c);
			while (web_send_pending(c) && web_idle(c))
				;
		}
		if (c->fd >= 0 && (p[k].revents & POLLIN))
			web_read(c);
	}
}

#endif
