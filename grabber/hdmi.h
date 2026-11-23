/*
 * hdmi.h - SPRUT-M: every decoder field straight to the Pi 4's own HDMI port, with the least possible delay.
 *
 * Part of sprutm-uvc (included after logm/now_ns), on next to the USB webcam and the web page. Option --hdmi.
 *
 * No desktop, no GPU: the field is processed on the CPU in a worker thread with the same algorithms as the web page
 * (www/index.html) - 3D comb, motion-adaptive deinterlace "Авто 50", temporal noise reduction - into a 4:2:2 frame
 * (NV16: luma plane + Cb/Cr plane). The frame goes to a DRM overlay plane: the Pi's display hardware (HVS) scales it
 * to the monitor and converts BT.601 YCbCr to RGB on the fly, with no copy. Each field is shown at the next refresh
 * (nonblocking atomic page flip). The monitor mode is chosen at 50 Hz when it offers one: one refresh per field, no
 * judder. No monitor: the connector is checked every 2 s; unplugged: the output stops until it comes back.
 *
 * Delay: a frame committed at least ~3 ms before a refresh is shown at that refresh (measured on the Pi 4). The
 * camera's fields and the monitor's refreshes both run at 50 Hz from different crystals, so their phase is random
 * per start and then hardly moves: field -> refresh = processing (~5 ms) + 0..20 ms. If a frame misses a refresh,
 * the next one would find it still waiting and queue behind it, and every frame after that would be a refresh late
 * for good; so a queued frame that has waited more than 3 ms is dropped for the next, fresher one, which still makes
 * that refresh.
 *
 * Build: -DSPRUTM_HDMI $(pkg-config --cflags --libs libdrm) -lpthread
 */
#include <drm_fourcc.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/eventfd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define HD_W	720		/* field width */
#define HD_L	288		/* max field lines (PAL) */
#define HD_N	(HD_W * HD_L)

struct hd_buf {
	uint32_t handle, fb, pitch;
	size_t size;
	uint8_t *map;
};

static struct {
	pthread_t th;
	pthread_mutex_t mu;
	int efd;			/* eventfd: a new field for the worker */
	char mode_req[32];		/* --hdmi-mode WxH@R */
	volatile int comb;		/* decoder luma line comb off -> 3D comb here (reg 0x39 YCMP = 100) */
	volatile int lev_on;		/* auto levels beyond the decoder's contrast: luma through lut */
	uint8_t lut[256];
	/* the newest field from the capture loop (under mu; the worker takes the latest, older ones are skipped) */
	uint8_t in[HD_N * 2];
	int in_lines, in_top, in_new;
	uint32_t in_seq;
	int64_t in_ns;
	/* worker copy */
	uint8_t work[HD_N * 2];
	/* DRM */
	int fd, active, crtc_idx;
	uint32_t conn_id, crtc_id, pri_id, ovl_id, blob, prop_fb;
	drmModeModeInfo mode;
	struct hd_buf black, out[3];
	int shown, pending, queued;	/* out[] index, -1 none */
	int64_t pending_ns, queued_ns;	/* when that frame's field reached sprutm-uvc */
	int lines;			/* field lines of the stream (288 PAL, 240 NTSC) */
	/* processing history, [parity 0 top / 1 bottom] */
	uint8_t raw[2][3][HD_N];	/* raw luma of the last three fields: n, n-2, n-4 */
	uint32_t raw_seq[2][3];
	int raw_slot[2];
	uint8_t cy[2][2][HD_N];		/* cleaned luma, latest and previous */
	uint8_t cc[2][2][HD_N];		/* chroma Cb Cr Cb Cr ... (NV16 order), latest and previous */
	int cslot[2], have[2];
	int16_t dy[2][HD_N];		/* cleaned luma change over one frame */
	int16_t win[2][HD_N];		/* its signed sum over 5 px x 3 lines */
	int32_t t1[HD_N], t2[HD_N];	/* scratch: comb window, change window */
	/* the field being processed, shared by the helper threads */
	int jp, jL, jmode, jrs, jro, jcs, jf;
	struct hd_buf *jbuf;
	/* helper threads: each step of the processing is split by lines into HD_PAR parts */
	pthread_t helper[2];
	sem_t go[2], done;
	void (*job)(int, int);
	int job_n;
	uint8_t nrw[256];		/* noise reduction weight (x256) by luma difference */
	uint16_t ctab[730];		/* 3D comb: weight (x256) of the 3-pixel mean by motion, see hd_clean */
	/* stats */
	unsigned long n_in, n_shown, n_skip, n_busy, n_lat;
	double lat_sum, lat_max, proc_sum, wait_sum;	/* wait: commit -> the refresh that shows it */
	int64_t commit_ns, queued_ready;
	unsigned long n_late;		/* queued frames dropped for a fresher one */
	int64_t stat_t;
} hd = { .fd = -1, .efd = -1, .shown = -1, .pending = -1, .queued = -1 };

/* ------------------------------------------------------------------ small helpers */

static float hd_ss(float a, float b, float x)	/* smoothstep */
{
	float t = (x - a) / (b - a);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3 - 2 * t);
}

/* out[x] = sum of in[x-r .. x+r], edges repeated */
static void hd_box(const int32_t *in, int32_t *out, int n, int r)
{
	int32_t s = 0;
	int x = 0;
	for (int k = -r; k <= r; k++)
		s += in[k < 0 ? 0 : k >= n ? n - 1 : k];
	for (; x <= r && x < n; x++) {		/* left edge */
		out[x] = s;
		int add = x + r + 1, sub = x - r;
		s += in[add >= n ? n - 1 : add] - in[sub < 0 ? 0 : sub];
	}
	for (; x < n - r - 1; x++) {		/* middle: no clamping */
		out[x] = s;
		s += in[x + r + 1] - in[x - r];
	}
	for (; x < n; x++) {			/* right edge */
		out[x] = s;
		int add = x + r + 1, sub = x - r;
		s += in[add >= n ? n - 1 : add] - in[sub < 0 ? 0 : sub];
	}
}

static void hd_tables(void)
{
	for (int d = 0; d < 256; d++)		/* noise reduction weight, as the web page: 0.5 * (1 - smoothstep(10, 30, d)) */
		hd.nrw[d] = (uint8_t)(128 * (1 - hd_ss(10, 30, (float)d)) + 0.5f);
	for (int i = 0; i < 730; i++)		/* smoothstep(3, 6, motion), motion in 1/243 units from 3 */
		hd.ctab[i] = (uint16_t)(256 * hd_ss(3, 6, 3 + i / 243.0f) + 0.5f);
}

static int hd_clampr(int r, int L)
{
	return r < 0 ? 0 : r >= L ? L - 1 : r;
}

/* ------------------------------------------------------------------ parallel lines */

#define HD_PAR	3			/* this thread + 2 helpers (the Pi 4 has 4 cores; capture, USB and web use ~1/4) */

static void hd_part(int i, int *a, int *b)
{
	*a = hd.job_n * i / HD_PAR;
	*b = hd.job_n * (i + 1) / HD_PAR;
}

static void *hd_helper(void *arg)
{
	int i = (int)(intptr_t)arg, a, b;
	for (;;) {
		sem_wait(&hd.go[i - 1]);
		hd_part(i, &a, &b);
		hd.job(a, b);
		sem_post(&hd.done);
	}
	return NULL;
}

/* run fn(first line, end line) over n lines, split between this thread and the helpers; returns when all are done */
static void hd_par(void (*fn)(int, int), int n)
{
	int a, b;
	hd.job = fn;
	hd.job_n = n;
	for (int i = 1; i < HD_PAR; i++)
		sem_post(&hd.go[i - 1]);
	hd_part(0, &a, &b);
	fn(a, b);
	for (int i = 1; i < HD_PAR; i++)
		sem_wait(&hd.done);
}

static int hd_par_init(void)
{
	sem_init(&hd.done, 0, 0);
	for (int i = 1; i < HD_PAR; i++) {
		sem_init(&hd.go[i - 1], 0, 0);
		if (pthread_create(&hd.helper[i - 1], NULL, hd_helper, (void *)(intptr_t)i))
			return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ processing (port of www/index.html) */

/* step 1, per line: UYVY -> luma, Cb Cr; for the 3D comb the 11-tap (1,2,3,...,3,2,1) sum of the change n - (n-4) */
static void hd_split(int r0, int r1)
{
	uint8_t *yn = hd.raw[hd.jp][hd.jrs], *yo = hd.raw[hd.jp][hd.jro], *cc = hd.cc[hd.jp][hd.jcs];
	int32_t a[HD_W], t[HD_W];
	for (int r = r0; r < r1; r++) {
		const uint8_t *w = hd.work + (size_t)r * HD_W * 2;
		uint8_t *py = yn + r * HD_W, *pc = cc + r * HD_W;
		for (int x = 0; x < HD_W; x++) {
			py[x] = w[2 * x + 1];
			pc[x] = w[2 * x];
		}
		if (hd.jmode != 1)
			continue;
		for (int x = 0; x < HD_W; x++)
			a[x] = py[x] - yo[r * HD_W + x];
		hd_box(a, t, HD_W, 1);
		hd_box(t, hd.t1 + r * HD_W, HD_W, 4);
	}
}

/* step 2, per line: 3D comb into the cleaned luma, then its change over a frame and the 5-pixel sum of that.
 * 3D comb: field n and n-4 have the PAL subcarrier inverted; their mean keeps all detail where nothing moved.
 * Motion: signed mean of the 3-pixel-smoothed change over 9 px x 3 lines, or one change > 20; there the 3-pixel
 * mean (zero at the subcarrier, 1/3 cycle/pixel) removes the dots instead. */
static void hd_comb(int r0, int r1)
{
	int L = hd.jL, p = hd.jp;
	const uint8_t *yn = hd.raw[p][hd.jrs], *yo = hd.raw[p][hd.jro], *prev = hd.cy[p][hd.jcs ^ 1];
	uint8_t *cy = hd.cy[p][hd.jcs];
	int16_t *dy = hd.dy[p];
	int32_t a[HD_W], bn[HD_W], bo[HD_W], ti[HD_W];
	for (int r = r0; r < r1; r++) {
		uint8_t *o = cy + r * HD_W;
		const uint8_t *pn = yn + r * HD_W, *po = yo + r * HD_W;
		if (hd.jmode == 0) {
			memcpy(o, pn, HD_W);
		} else {
			for (int x = 0; x < HD_W; x++)
				a[x] = pn[x];
			hd_box(a, bn, HD_W, 1);
			if (hd.jmode == 2) {
				for (int x = 0; x < HD_W; x++)
					o[x] = (uint8_t)((bn[x] + 1) / 3);
			} else {
				const int32_t *u = hd.t1 + hd_clampr(r - 1, L) * HD_W, *m = hd.t1 + r * HD_W,
					      *d = hd.t1 + hd_clampr(r + 1, L) * HD_W;
				for (int x = 0; x < HD_W; x++)
					a[x] = po[x];
				hd_box(a, bo, HD_W, 1);
				for (int x = 0; x < HD_W; x++) {
					/* motion = max(|window| / 81, |3-px mean change| - 20), here in 1/243 units */
					int ta = 3 * abs(u[x] + m[x] + d[x]), tb = 81 * abs(bn[x] - bo[x]) - 4860;
					int t = (ta > tb ? ta : tb) - 729;
					ti[x] = t < 0 ? 0 : t > 729 ? 729 : t;
				}
				for (int x = 0; x < HD_W; x++)
					ti[x] = hd.ctab[ti[x]];
				for (int x = 0; x < HD_W; x++)	/* (1 - w) * (yn + yo) / 2 + w * bn / 3, w in 1/256 */
					o[x] = (uint8_t)(((256 - ti[x]) * (pn[x] + po[x]) * 3 + ti[x] * bn[x] * 2 + 768) / 1536);
			}
		}
		const uint8_t *q = prev + r * HD_W;
		for (int x = 0; x < HD_W; x++) {
			a[x] = o[x] - q[x];
			dy[r * HD_W + x] = (int16_t)a[x];
		}
		hd_box(a, hd.t2 + r * HD_W, HD_W, 2);
	}
}

/* step 3, per line: the change's signed 5 px x 3 line window sum */
static void hd_window(int r0, int r1)
{
	int L = hd.jL;
	int16_t *wn = hd.win[hd.jp];
	for (int r = r0; r < r1; r++) {
		const int32_t *u = hd.t2 + hd_clampr(r - 1, L) * HD_W, *m = hd.t2 + r * HD_W, *d = hd.t2 + hd_clampr(r + 1, L) * HD_W;
		for (int x = 0; x < HD_W; x++)
			wn[r * HD_W + x] = (int16_t)(u[x] + m[x] + d[x]);
	}
}

/* the newest field (parity p) in hd.work: split, 3D comb, change over a frame and its window sums */
static void hd_clean(int p, uint32_t seq, int L)
{
	int rs = hd.raw_slot[p] = (hd.raw_slot[p] + 1) % 3, ro = (rs + 1) % 3;	/* ro: two frames earlier */
	hd.raw_seq[p][rs] = seq;
	hd.jp = p;
	hd.jL = L;
	hd.jrs = rs;
	hd.jro = ro;
	hd.jcs = hd.cslot[p] ^= 1;
	hd.jmode = !hd.comb ? 0 : (uint32_t)(seq - hd.raw_seq[p][ro]) == 4 ? 1 : 2;	/* history broken: no 3D */
	hd_par(hd_split, L);
	hd_par(hd_comb, L);
	hd_par(hd_window, L);
	if (hd.have[p] < 3)
		hd.have[p]++;
}

/* weave + "Авто 50" into an NV16 frame, lines y0..y1: still parts from both fields, moving parts interpolated;
 * temporal noise reduction against the same line a frame earlier. Plain per-line loops without data-dependent
 * branches (the compiler vectorizes them; with branches the noisy picture cost ~25 ns per pixel). */
static void hd_render_lines(int y0, int y1)
{
	int f = hd.jf, L = hd.jL, op = 1 - f, cf = hd.cslot[f], co = hd.cslot[op], haveop = hd.have[op] > 0;
	const uint8_t *yf = hd.cy[f][cf], *yfp = hd.cy[f][cf ^ 1], *cfl = hd.cc[f][cf], *cfp = hd.cc[f][cf ^ 1];
	const uint8_t *yo = hd.cy[op][co], *yop = hd.cy[op][co ^ 1], *col = hd.cc[op][co], *cop = hd.cc[op][co ^ 1];
	uint8_t ly[HD_W], lc[HD_W];
	int32_t wl[HD_W], ew[HD_W];	/* noise reduction weight, motion limit, per pixel */
	struct hd_buf *b = hd.jbuf;
	uint8_t *Yp = b->map, *Cp = b->map + (size_t)b->pitch * 2 * L;
	for (int y = y0; y < y1; y++) {
		int r = y >> 1;
		if ((y & 1) == f) {			/* a line of the newest field */
			const uint8_t *a = yf + r * HD_W, *ap = yfp + r * HD_W, *c = cfl + r * HD_W, *cp = cfp + r * HD_W;
			for (int x = 0; x < HD_W; x++)
				wl[x] = hd.nrw[abs(a[x] - ap[x])];
			for (int x = 0; x < HD_W; x++)
				ly[x] = (uint8_t)(a[x] + (((ap[x] - a[x]) * wl[x] + 128) >> 8));
			for (int x = 0; x < HD_W; x++)	/* a Cb Cr pair takes the weight of its first pixel */
				lc[x] = (uint8_t)(c[x] + (((cp[x] - c[x]) * wl[x & ~1] + 128) >> 8));
		} else {				/* missing line: the other field where still, interpolated where moving */
			int ra = hd_clampr(f == 0 ? r : r - 1, L), rb = hd_clampr(ra + 1, L);
			const uint8_t *A = yf + ra * HD_W, *B = yf + rb * HD_W, *o = yo + r * HD_W, *opv = yop + r * HD_W;
			const uint8_t *cA = cfl + ra * HD_W, *cB = cfl + rb * HD_W, *oc = col + r * HD_W, *ocp = cop + r * HD_W;
			if (!haveop) {
				for (int x = 0; x < HD_W; x++) {
					ly[x] = (uint8_t)((A[x] + B[x] + 1) >> 1);
					lc[x] = (uint8_t)((cA[x] + cB[x] + 1) >> 1);
				}
			} else {
				const int16_t *wo = hd.win[op] + r * HD_W, *wa = hd.win[f] + ra * HD_W, *wb = hd.win[f] + rb * HD_W;
				const int16_t *dop = hd.dy[op] + r * HD_W, *da = hd.dy[f] + ra * HD_W, *db = hd.dy[f] + rb * HD_W;
				for (int x = 0; x < HD_W; x++)
					wl[x] = hd.nrw[abs(o[x] - opv[x])];
				for (int x = 0; x < HD_W; x++) {
					/* d = max(window / 15, single change - 30); e = max(d - 4, 0) * 4 */
					int m1 = abs(wo[x]), m2 = (abs(wa[x]) + abs(wb[x])) >> 1, m = m1 > m2 ? m1 : m2;
					int p1 = abs(dop[x]), p2 = (abs(da[x]) + abs(db[x])) >> 1, px = p1 > p2 ? p1 : p2;
					int e1 = ((4 * m - 60) * 4369) >> 16, e2 = 4 * (px - 34);	/* x 4369 >> 16 = / 15 */
					int e = e1 > e2 ? e1 : e2;
					ew[x] = e > 0 ? e : 0;
				}
				for (int x = 0; x < HD_W; x++) {
					int sp = (A[x] + B[x] + 1) >> 1, wv = o[x] + (((opv[x] - o[x]) * wl[x] + 128) >> 8);
					int lo = wv - ew[x], hi = wv + ew[x];
					ly[x] = (uint8_t)(sp < lo ? lo : sp > hi ? hi : sp);
				}
				for (int x = 0; x < HD_W; x += 2)	/* a Cb Cr pair: the larger limit of its two pixels */
					ew[x] = ew[x + 1] = ew[x] > ew[x + 1] ? ew[x] : ew[x + 1];
				for (int x = 0; x < HD_W; x++) {
					int sp = (cA[x] + cB[x] + 1) >> 1, wv = oc[x] + (((ocp[x] - oc[x]) * wl[x & ~1] + 128) >> 8);
					int lo = wv - ew[x], hi = wv + ew[x];
					lc[x] = (uint8_t)(sp < lo ? lo : sp > hi ? hi : sp);
				}
			}
		}
		if (hd.lev_on)
			for (int x = 0; x < HD_W; x++)
				ly[x] = hd.lut[ly[x]];
		memcpy(Yp + (size_t)y * b->pitch, ly, HD_W);	/* write-combined memory: whole lines */
		memcpy(Cp + (size_t)y * b->pitch, lc, HD_W);
	}
}

static void hd_render(struct hd_buf *b, int f, int L)
{
	hd.jf = f;
	hd.jL = L;
	hd.jbuf = b;
	hd_par(hd_render_lines, 2 * L);
}

/* ------------------------------------------------------------------ DRM */

static uint32_t hd_prop(uint32_t obj, uint32_t type, const char *name, uint64_t *val)
{
	drmModeObjectProperties *pp = drmModeObjectGetProperties(hd.fd, obj, type);
	uint32_t id = 0;
	for (uint32_t i = 0; pp && i < pp->count_props && !id; i++) {
		drmModePropertyRes *pr = drmModeGetProperty(hd.fd, pp->props[i]);
		if (pr && !strcmp(pr->name, name)) {
			id = pr->prop_id;
			if (val)
				*val = pp->prop_values[i];
		}
		drmModeFreeProperty(pr);
	}
	drmModeFreeObjectProperties(pp);
	return id;
}

static void hd_add(drmModeAtomicReq *r, uint32_t obj, uint32_t type, const char *name, uint64_t v)
{
	uint32_t id = hd_prop(obj, type, name, NULL);
	if (id)
		drmModeAtomicAddProperty(r, obj, id, v);
}

static void hd_free_buf(struct hd_buf *b)
{
	if (b->map)
		munmap(b->map, b->size);
	if (b->fb)
		drmModeRmFB(hd.fd, b->fb);
	if (b->handle) {
		struct drm_mode_destroy_dumb d = { .handle = b->handle };
		drmIoctl(hd.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	memset(b, 0, sizeof *b);
}

/* XR24 (black) or NV16 (luma plane then Cb/Cr plane, same pitch) */
static int hd_new_buf(struct hd_buf *b, uint32_t w, uint32_t h, uint32_t fourcc)
{
	int nv16 = fourcc == DRM_FORMAT_NV16;
	struct drm_mode_create_dumb c = { .width = w, .height = nv16 ? 2 * h : h, .bpp = nv16 ? 8 : 32 };
	if (drmIoctl(hd.fd, DRM_IOCTL_MODE_CREATE_DUMB, &c))
		return -1;
	b->handle = c.handle;
	b->pitch = c.pitch;
	b->size = c.size;
	uint32_t hs[4] = { c.handle, nv16 ? c.handle : 0 }, ps[4] = { c.pitch, nv16 ? c.pitch : 0 },
		 os[4] = { 0, nv16 ? c.pitch * h : 0 };
	struct drm_mode_map_dumb m = { .handle = c.handle };
	if (drmModeAddFB2(hd.fd, w, h, fourcc, hs, ps, os, &b->fb, 0) || drmIoctl(hd.fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
		goto fail;
	b->map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, hd.fd, (off_t)m.offset);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		goto fail;
	}
	if (nv16) {			/* black: Y 16, Cb Cr 128 */
		memset(b->map, 16, (size_t)c.pitch * h);
		memset(b->map + (size_t)c.pitch * h, 128, (size_t)c.pitch * h);
	} else {
		memset(b->map, 0, c.size);
	}
	return 0;
fail:
	hd_free_buf(b);
	return -1;
}

static int hd_hz(const drmModeModeInfo *m)
{
	return m->htotal && m->vtotal ? (int)(m->clock * 1000.0 / ((double)m->htotal * m->vtotal) + 0.5) : 0;
}

/* CEA-861 50 Hz modes, for --hdmi-mode when the monitor does not list one (or nothing is plugged in, for tests) */
static const drmModeModeInfo hd_cea50[] = {
	{ 148500, 1920, 2448, 2492, 2640, 0, 1080, 1084, 1089, 1125, 0, 50,
	  DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC, DRM_MODE_TYPE_USERDEF, "1920x1080" },
	{ 74250, 1280, 1720, 1760, 1980, 0, 720, 725, 730, 750, 0, 50,
	  DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC, DRM_MODE_TYPE_USERDEF, "1280x720" },
	{ 27000, 720, 732, 796, 864, 0, 576, 581, 586, 625, 0, 50,
	  DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC, DRM_MODE_TYPE_USERDEF, "720x576" },
};

static int hd_pick_mode(const drmModeConnector *c, drmModeModeInfo *out)
{
	int w, h, r;
	if (hd.mode_req[0] && sscanf(hd.mode_req, "%dx%d@%d", &w, &h, &r) == 3) {
		for (int i = 0; i < c->count_modes; i++)
			if (c->modes[i].hdisplay == w && c->modes[i].vdisplay == h && hd_hz(&c->modes[i]) == r &&
			    !(c->modes[i].flags & DRM_MODE_FLAG_INTERLACE)) {
				*out = c->modes[i];
				return 0;
			}
		for (size_t i = 0; i < sizeof hd_cea50 / sizeof hd_cea50[0]; i++)
			if (hd_cea50[i].hdisplay == w && hd_cea50[i].vdisplay == h && r == 50) {
				*out = hd_cea50[i];
				logm("hdmi: %s not in the monitor's list, forcing the standard CEA timing", hd.mode_req);
				return 0;
			}
		logm("warning: hdmi: mode %s unknown, choosing automatically", hd.mode_req);
	}
	const drmModeModeInfo *pref = NULL, *best = NULL;
	for (int i = 0; i < c->count_modes; i++)
		if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED)
			pref = &c->modes[i];
	if (!pref && c->count_modes)
		pref = &c->modes[0];
	for (int i = 0; i < c->count_modes; i++) {
		const drmModeModeInfo *m = &c->modes[i];
		if (hd_hz(m) != 50 || (m->flags & DRM_MODE_FLAG_INTERLACE) || m->hdisplay * m->vdisplay > 1920 * 1080)
			continue;
		if (pref && m->hdisplay == pref->hdisplay && m->vdisplay == pref->vdisplay) {
			best = m;			/* the monitor's own resolution at 50 Hz */
			break;
		}
		if (!best || m->hdisplay * m->vdisplay > best->hdisplay * best->vdisplay)
			best = m;
	}
	if (best) {
		*out = *best;
		return 0;
	}
	if (!pref)
		return -1;
	*out = *pref;
	logm("warning: hdmi: the monitor offers no 50 Hz mode, using %dx%d@%d: motion judders a little",
	     pref->hdisplay, pref->vdisplay, hd_hz(pref));
	return 0;
}

static void hd_stop(const char *why)
{
	if (hd.active) {
		drmModeAtomicReq *r = drmModeAtomicAlloc();
		hd_add(r, hd.ovl_id, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
		hd_add(r, hd.ovl_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
		hd_add(r, hd.pri_id, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
		hd_add(r, hd.pri_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
		hd_add(r, hd.conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", 0);
		hd_add(r, hd.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", 0);
		hd_add(r, hd.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 0);
		drmModeAtomicCommit(hd.fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		drmModeAtomicFree(r);
		logm("hdmi: output off (%s)", why);
	}
	hd.active = 0;
	for (int i = 0; i < 3; i++)
		hd_free_buf(&hd.out[i]);
	hd_free_buf(&hd.black);
	if (hd.blob)
		drmModeDestroyPropertyBlob(hd.fd, hd.blob);
	hd.blob = 0;
	hd.shown = hd.pending = hd.queued = -1;
	hd.have[0] = hd.have[1] = 0;
}

/* a monitor is connected: mode, planes, buffers, first (modeset) commit */
static int hd_setup(void)
{
	drmModeRes *res = drmModeGetResources(hd.fd);
	drmModeConnector *c = NULL;
	drmModePlaneRes *pr = NULL;
	int ok = -1;
	if (!res)
		return -1;
	for (int i = 0; i < res->count_connectors && !c; i++) {
		c = drmModeGetConnector(hd.fd, res->connectors[i]);	/* probes the port */
		if (c && (c->connection != DRM_MODE_CONNECTED || !c->count_modes)) {
			drmModeFreeConnector(c);
			c = NULL;
		}
	}
	if (!c)
		goto out;				/* nothing plugged in: quietly try again later */
	if (hd_pick_mode(c, &hd.mode))
		goto out;
	hd.conn_id = c->connector_id;
	hd.crtc_id = 0;
	for (int e = 0; e < c->count_encoders && !hd.crtc_id; e++) {
		drmModeEncoder *enc = drmModeGetEncoder(hd.fd, c->encoders[e]);
		for (int i = 0; enc && i < res->count_crtcs; i++)
			if (enc->possible_crtcs & (1u << i)) {
				hd.crtc_id = res->crtcs[i];
				hd.crtc_idx = i;
				break;
			}
		drmModeFreeEncoder(enc);
	}
	pr = drmModeGetPlaneResources(hd.fd);
	hd.pri_id = hd.ovl_id = 0;
	for (uint32_t i = 0; pr && i < pr->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(hd.fd, pr->planes[i]);
		uint64_t type = 99;
		if (p && (p->possible_crtcs & (1u << hd.crtc_idx))) {
			hd_prop(p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
			int nv16 = 0;
			for (uint32_t k = 0; k < p->count_formats; k++)
				nv16 |= p->formats[k] == DRM_FORMAT_NV16;
			if (type == DRM_PLANE_TYPE_PRIMARY && !hd.pri_id)
				hd.pri_id = p->plane_id;
			else if (type == DRM_PLANE_TYPE_OVERLAY && nv16 && !hd.ovl_id)
				hd.ovl_id = p->plane_id;
		}
		drmModeFreePlane(p);
	}
	if (!hd.crtc_id || !hd.pri_id || !hd.ovl_id) {
		logm("warning: hdmi: no display pipe (crtc %u, primary %u, overlay %u)", hd.crtc_id, hd.pri_id, hd.ovl_id);
		goto out;
	}
	int W = hd.mode.hdisplay, H = hd.mode.vdisplay, L = hd.lines;
	if (hd_new_buf(&hd.black, (uint32_t)W, (uint32_t)H, DRM_FORMAT_XRGB8888))
		goto fail;
	for (int i = 0; i < 3; i++)
		if (hd_new_buf(&hd.out[i], HD_W, (uint32_t)(2 * L), DRM_FORMAT_NV16))
			goto fail;
	if (drmModeCreatePropertyBlob(hd.fd, &hd.mode, sizeof hd.mode, &hd.blob))
		goto fail;
	/* picture: the active 702 samples x (2L - 8) lines, 4:3 for 702 x 2L; the monitor's pixels taken as square.
	 * A 720x576 / 720x480 mode is the SD picture itself: shown whole, on the whole screen. */
	int sx = 9, sy = 4, sw = 702, sh = 2 * L - 8, dx, dy, dw, dh;
	if (W == HD_W && H == 2 * L) {
		sx = sy = 0;
		sw = HD_W;
		sh = 2 * L;
		dx = dy = 0;
		dw = W;
		dh = H;
	} else {
		double a = 4.0 / 3.0 * (2.0 * L) / sh;
		dh = H;
		dw = (int)(H * a + 0.5);
		if (dw > W) {
			dw = W;
			dh = (int)(W / a + 0.5);
		}
		dx = (W - dw) / 2;
		dy = (H - dh) / 2;
	}
	drmModeAtomicReq *r = drmModeAtomicAlloc();
	hd_add(r, hd.conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", hd.crtc_id);
	hd_add(r, hd.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", hd.blob);
	hd_add(r, hd.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
	const struct { uint32_t plane, fb; int sx, sy, sw, sh, dx, dy, dw, dh; } pl[2] = {
		{ hd.pri_id, hd.black.fb, 0, 0, W, H, 0, 0, W, H },
		{ hd.ovl_id, hd.out[0].fb, sx, sy, sw, sh, dx, dy, dw, dh } };
	for (int i = 0; i < 2; i++) {
		uint32_t p = pl[i].plane;
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "FB_ID", pl[i].fb);
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "CRTC_ID", hd.crtc_id);
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "SRC_X", (uint64_t)pl[i].sx << 16);
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "SRC_Y", (uint64_t)pl[i].sy << 16);
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)pl[i].sw << 16);
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)pl[i].sh << 16);
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "CRTC_X", (uint64_t)pl[i].dx);
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "CRTC_Y", (uint64_t)pl[i].dy);
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "CRTC_W", (uint64_t)pl[i].dw);
		hd_add(r, p, DRM_MODE_OBJECT_PLANE, "CRTC_H", (uint64_t)pl[i].dh);
	}
	hd_add(r, hd.ovl_id, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING", 0);	/* ITU-R BT.601 (the default is BT.709) */
	hd_add(r, hd.ovl_id, DRM_MODE_OBJECT_PLANE, "COLOR_RANGE", 0);	/* limited (studio) range 16..235 */
	int e = drmModeAtomicCommit(hd.fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(r);
	if (e) {
		logm("warning: hdmi: mode %dx%d@%d refused: %s", W, H, hd_hz(&hd.mode), strerror(errno));
		goto fail;
	}
	hd.prop_fb = hd_prop(hd.ovl_id, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
	hd.shown = 0;
	hd.active = 1;
	hd.stat_t = now_ns();
	hd.n_shown = hd.n_lat = 0;
	hd.lat_sum = hd.lat_max = hd.proc_sum = 0;
	logm("hdmi: %dx%d@%d on connector %u, picture %dx%d at %d,%d%s", W, H, hd_hz(&hd.mode), hd.conn_id, dw, dh, dx, dy,
	     hd.comb ? ", 3D comb" : "");
	ok = 0;
	goto out;
fail:
	hd.active = 1;			/* let hd_stop undo what was done */
	hd_stop("setup failed");
out:
	drmModeFreePlaneResources(pr);
	drmModeFreeConnector(c);
	drmModeFreeResources(res);
	return ok;
}

static int hd_flip(int idx)
{
	drmModeAtomicReq *r = drmModeAtomicAlloc();
	drmModeAtomicAddProperty(r, hd.ovl_id, hd.prop_fb, hd.out[idx].fb);
	hd.commit_ns = now_ns();
	int e = drmModeAtomicCommit(hd.fd, r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL);
	drmModeAtomicFree(r);
	return e;
}

static void hd_flipped(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
	(void)fd, (void)seq, (void)data;
	if (hd.pending < 0)
		return;
	/* field arrival -> the refresh that starts showing it (the event time is that vblank, CLOCK_MONOTONIC) */
	int64_t vb = (int64_t)sec * 1000000000LL + (int64_t)usec * 1000;
	double lat = (vb - hd.pending_ns) / 1e6;
	if (lat > 0 && lat < 1000) {
		hd.wait_sum += (vb - hd.commit_ns) / 1e6;
		hd.lat_sum += lat;
		hd.lat_max = lat > hd.lat_max ? lat : hd.lat_max;
		hd.n_lat++;
	}
	hd.shown = hd.pending;
	hd.pending = -1;
	hd.n_shown++;
	if (hd.queued >= 0) {
		if (vb - hd.queued_ready > 3000000) {
			hd.n_late++;		/* the next frame comes in time for the next refresh: no chain of late frames */
		} else if (hd_flip(hd.queued) == 0) {
			hd.pending = hd.queued;
			hd.pending_ns = hd.queued_ns;
		}
	}
	hd.queued = -1;
}

/* ------------------------------------------------------------------ worker thread */

static void hd_field(int64_t t_in)
{
	pthread_mutex_lock(&hd.mu);
	if (!hd.in_new) {
		pthread_mutex_unlock(&hd.mu);
		return;
	}
	int L = hd.in_lines, top = hd.in_top;
	uint32_t seq = hd.in_seq;
	int64_t ns = hd.in_ns;
	memcpy(hd.work, hd.in, (size_t)L * HD_W * 2);
	hd.in_new = 0;
	pthread_mutex_unlock(&hd.mu);
	hd.n_in++;
	if (L != hd.lines) {			/* first field, or the standard changed */
		if (hd.active)
			hd_stop("field size changed");
		hd.lines = L;
	}
	if (!hd.active)
		return;
	int f = top ? 0 : 1;
	hd_clean(f, seq, L);
	int idx = -1;			/* a buffer neither on screen nor waiting for the refresh */
	for (int i = 0; i < 3 && idx < 0; i++)
		if (i != hd.shown && i != hd.pending && i != hd.queued)
			idx = i;
	if (idx < 0)
		idx = hd.queued;		/* replace the frame that has not been shown yet */
	if (idx < 0) {
		hd.n_busy++;
		return;
	}
	hd_render(&hd.out[idx], f, L);
	hd.proc_sum += (now_ns() - t_in) / 1e6;
	static unsigned long rendered;
	const char *dump = getenv("SPRUTM_HDMI_DUMP");	/* debug: the 200th frame as raw NV16 720 x 2L to this file */
	if (dump && ++rendered == 200) {
		FILE *fp = fopen(dump, "wb");
		for (int y = 0; fp && y < 4 * L; y++)		/* luma lines, then Cb Cr lines */
			fwrite(hd.out[idx].map + (size_t)y * hd.out[idx].pitch, 1, HD_W, fp);
		if (fp)
			fclose(fp);
		logm("hdmi: frame dumped to %s", dump);
	}
	if (hd.pending < 0) {
		if (hd_flip(idx) == 0) {
			hd.pending = idx;
			hd.pending_ns = ns;
		} else {
			hd.n_busy++;
		}
	} else {
		hd.queued = idx;		/* shown right after the flip in progress, unless it waits too long */
		hd.queued_ns = ns;
		hd.queued_ready = now_ns();
	}
}

static void *hd_thread(void *arg)
{
	(void)arg;
	int64_t next_check = 0;
	drmEventContext ev = { .version = 2, .page_flip_handler = hd_flipped };
	for (;;) {
		struct pollfd p[2] = { { .fd = hd.efd, .events = POLLIN }, { .fd = hd.active ? hd.fd : -1, .events = POLLIN } };
		if (poll(p, 2, 500) < 0 && errno != EINTR)
			break;
		if (p[1].revents & POLLIN)
			drmHandleEvent(hd.fd, &ev);
		if (p[0].revents & POLLIN) {
			uint64_t n;
			if (read(hd.efd, &n, sizeof n) == sizeof n)
				hd_field(now_ns());
		}
		int64_t t = now_ns();
		if (t >= next_check) {
			next_check = t + 2000000000LL;
			if (hd.active) {
				drmModeConnector *c = drmModeGetConnectorCurrent(hd.fd, hd.conn_id);
				if (!c || c->connection != DRM_MODE_CONNECTED)
					hd_stop("monitor unplugged");
				drmModeFreeConnector(c);
			} else if (hd.lines) {
				hd_setup();
			}
		}
		if (hd.active && g.verbose && t - hd.stat_t >= 10000000000LL) {
			double s = (t - hd.stat_t) / 1e9;
			logm("hdmi: %.1f frames/s shown, field -> refresh %.1f ms avg (max %.1f), processing %.1f ms, "
			     "commit -> refresh %.1f ms, skipped %lu, late %lu, busy %lu", hd.n_shown / s,
			     hd.n_lat ? hd.lat_sum / hd.n_lat : 0, hd.lat_max, hd.n_in ? hd.proc_sum / hd.n_in : 0,
			     hd.n_lat ? hd.wait_sum / hd.n_lat : 0, hd.n_skip, hd.n_late, hd.n_busy);
			hd.stat_t = t;
			hd.n_shown = hd.n_lat = hd.n_in = 0;
			hd.lat_sum = hd.lat_max = hd.proc_sum = hd.wait_sum = 0;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ API for sprutm-uvc */

static void hdmi_set_comb(int on)
{
	hd.comb = on;
}

/* luma levels on top of the decoder (auto levels): Y' = 16 + (Y - black) * gain / 1000; gain 1000 = off */
static void hdmi_set_levels(int black, int gain)
{
	if (gain <= 1000) {
		hd.lev_on = 0;
		return;
	}
	for (int i = 0; i < 256; i++) {
		int v = 16 + (i - black) * gain / 1000;
		hd.lut[i] = (uint8_t)(v < 1 ? 1 : v > 254 ? 254 : v);
	}
	hd.lev_on = 1;
}

static int hdmi_start(const char *mode_req)
{
	char path[32];
	for (int i = 0; i < 8 && hd.fd < 0; i++) {
		snprintf(path, sizeof path, "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		drmModeRes *r = drmModeGetResources(fd);
		if (r && r->count_connectors > 0)
			hd.fd = fd;
		else
			close(fd);
		drmModeFreeResources(r);
	}
	if (hd.fd < 0) {
		logm("warning: hdmi: no display device (/dev/dri/card*), HDMI output off");
		return -1;
	}
	if (drmSetClientCap(hd.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) || drmSetClientCap(hd.fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		logm("warning: hdmi: the display driver has no atomic modesetting (dtoverlay=vc4-kms-v3d?), HDMI output off");
		close(hd.fd);
		hd.fd = -1;
		return -1;
	}
	snprintf(hd.mode_req, sizeof hd.mode_req, "%s", mode_req ? mode_req : "");
	hd_tables();
	if (hd_par_init()) {
		logm("warning: hdmi: no helper threads, HDMI output off");
		return -1;
	}
	hd.efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	pthread_mutex_init(&hd.mu, NULL);
	if (hd.efd < 0 || pthread_create(&hd.th, NULL, hd_thread, NULL)) {
		logm("warning: hdmi: %s, HDMI output off", strerror(errno));
		return -1;
	}
	logm("hdmi: output on, waiting for a monitor%s%s", hd.mode_req[0] ? ", mode " : "", hd.mode_req);
	return 0;
}

/* from the capture loop: hand the newest field to the worker (cheap: one copy) */
static void hdmi_field(uint32_t seq, const uint8_t *data, unsigned w, unsigned lines, unsigned bpl, int top)
{
	if (hd.efd < 0 || w != HD_W || lines > HD_L)
		return;
	pthread_mutex_lock(&hd.mu);
	if (hd.in_new)
		hd.n_skip++;
	for (unsigned r = 0; r < lines; r++)
		memcpy(hd.in + (size_t)r * HD_W * 2, data + (size_t)r * bpl, HD_W * 2);
	hd.in_lines = (int)lines;
	hd.in_top = top;
	hd.in_seq = seq;
	hd.in_ns = now_ns();
	hd.in_new = 1;
	pthread_mutex_unlock(&hd.mu);
	uint64_t one = 1;
	if (write(hd.efd, &one, sizeof one) < 0) {
		/* counter full: the worker is far behind, it takes the newest anyway */
	}
}
