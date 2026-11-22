/* sprutm-uvc: SPRUT-M (ADV7280-M over CSI-2) -> Raspberry Pi 4 -> USB UVC webcam and a web page, uncompressed.
 *
 * The decoder runs in interlaced mode: one 720 x 288 (PAL) / 720 x 240 (NTSC) field per CSI-2 frame, 50/60 fields
 * per second. Two fields of one frame are woven line by line into 720 x 576 / 720 x 480 and handed to the UVC
 * gadget (f_uvc, configured by sprutm-gadget.sh). No scaling, no compression. Default NV12: luma bit-exact, chroma
 * averaged over two lines of the same field (weave.h). YUY2 (every sample unchanged) needs 20.7 MB/s, but the
 * Pi 4 dwc2 gadget sends at most 2048 bytes per microframe = 16.3 MB/s. Deinterlacing is left to the host.
 *
 * Why the field parity is read over I2C: unicam (the Pi CSI-2 receiver) does not report TOP/BOTTOM, it only
 * counts frames (v4l2_buffer.sequence) and timestamps them at Frame Start. The decoder reports the field it is
 * currently receiving in Raw Status 2 (interrupt/VDP sub-map, 0x45 bit 4 EVEN_FIELD). Sampling it in the middle
 * of a known field gives the sequence -> parity relation once; the frame counter keeps it from then on.
 *
 * Why Media Controller mode: in legacy mode unicam never asks the sensor for an interlaced format and the adv7180
 * driver then switches the chip to its I2P deinterlacer. Here the subdevice is set to field ALTERNATE directly.
 *
 * Web page (web.h, at the same time as USB): http://<pi>/ shows every field bit-exact (YUY2-equivalent, 20.7 MB/s),
 * deinterlaced in the browser, with a luma histogram, the decoder status and its controls. Over a LAN cable all
 * 50 fields arrive; over Wi-Fi the page shows as many as the link carries.
 *
 * Build: gcc -O2 -Wall -o sprutm-uvc sprutm-uvc.c       Usage: sprutm-uvc --help
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/usb/ch9.h>
#include <linux/usb/video.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#if defined(__has_include)
#if __has_include(<linux/usb/g_uvc.h>)
#include <linux/usb/g_uvc.h>
#define HAVE_G_UVC_H 1
#endif
#endif
#ifndef HAVE_G_UVC_H	/* include/uapi/linux/usb/g_uvc.h, for older linux-libc-dev */
#define UVC_EVENT_FIRST		(V4L2_EVENT_PRIVATE_START + 0)
#define UVC_EVENT_CONNECT	(V4L2_EVENT_PRIVATE_START + 0)
#define UVC_EVENT_DISCONNECT	(V4L2_EVENT_PRIVATE_START + 1)
#define UVC_EVENT_STREAMON	(V4L2_EVENT_PRIVATE_START + 2)
#define UVC_EVENT_STREAMOFF	(V4L2_EVENT_PRIVATE_START + 3)
#define UVC_EVENT_SETUP		(V4L2_EVENT_PRIVATE_START + 4)
#define UVC_EVENT_DATA		(V4L2_EVENT_PRIVATE_START + 5)
#define UVC_EVENT_LAST		(V4L2_EVENT_PRIVATE_START + 5)
#define UVC_STRING_CONTROL_IDX		0
#define UVC_STRING_STREAMING_IDX	1
struct uvc_request_data {
	__s32 length;
	__u8 data[60];
};
struct uvc_event {
	union {
		enum usb_device_speed speed;
		struct usb_ctrlrequest req;
		struct uvc_request_data data;
	};
};
#define UVCIOC_SEND_RESPONSE	_IOW('U', 1, struct uvc_request_data)
#endif

#include "weave.h"
#include "web.h"

#define CAP_BUFS	6
#define OUT_BUFS	4
#define MAX_PAYLOAD	2048	/* = streaming_maxpacket in sprutm-gadget.sh: the Pi 4 dwc2 FIFOs hold at most 2048 bytes */
#define ADV_REG_CTRL	0x0e	/* register map select (adv7180.c: ADV7180_REG_CTRL) */
#define ADV_MAP_IRQ	0x20	/* interrupt/VDP sub-map (ADV7180_CTRL_IRQ_SPACE) */
#define ADV_RAW_STATUS2	0x45	/* UG-637: Raw Status 2, bit 4 EVEN_FIELD */
#define LOCK_NEED	5

struct mbuf {
	void *p;
	size_t len;
};

enum out_state { OUT_FREE, OUT_FILLING, OUT_QUEUED };

static struct {
	/* options */
	int ntsc, swap, invert, fixed_phase, verbose, i2c_bus, i2c_addr, nv12, web_port;
	char video[64], subdev[64], uvc[64], www[200];
	int sub_fd;			/* decoder subdevice, kept open for the web page controls */
	int peaking;			/* --peaking: luma peaking gain (reg 0xFB) set at start, -1 = leave as is */
	int comb3d;			/* --comb3d: decoder line comb off at start, for the web page's 3D comb */
	int hdmi;			/* --hdmi: video straight to the Pi's HDMI port (hdmi.h) */
	char hdmi_mode[32];		/* --hdmi-mode WxH@R */
	/* geometry */
	unsigned fw, fh, fbpl;		/* field from unicam */
	unsigned W, H;			/* frame to the host */
	size_t fsize;			/* bytes per frame to the host */
	uint32_t interval;		/* 100 ns units */
	int64_t field_ns;
	/* devices */
	int cap_fd, uvc_fd, i2c_fd;
	struct mbuf cap[CAP_BUFS];
	struct mbuf out[OUT_BUFS];
	enum out_state out_st[OUT_BUFS];
	int out_queued;
	int streaming;
	/* frame being assembled */
	int cur;			/* OUT_* index or -1 */
	uint32_t cur_seq;
	/* parity */
	struct phase_lock lock;
	int sample_pending;
	uint32_t sample_seq;
	int64_t sample_due, last_sample, start_ns;
	/* UVC control state */
	struct uvc_streaming_control probe, commit;
	int pending_set;		/* 0 none, 1 probe, 2 commit, 3 VC control */
	unsigned pending_key;
	struct { unsigned key; int32_t val; } vc[16];
	int nvc;
	/* stats */
	unsigned long n_fields, n_frames, n_drop, n_nobuf;
} g = { .nv12 = 1, .web_port = 80, .peaking = -1, .www = "/usr/local/share/sprutm", .fixed_phase = -1, .i2c_bus = 10,
	.i2c_addr = 0x21, .cap_fd = -1, .uvc_fd = -1, .i2c_fd = -1, .sub_fd = -1, .cur = -1 };

static volatile sig_atomic_t quit;

static void logm(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

/* ------------------------------------------------------------------ device discovery */

static int find_video(const char *driver, uint32_t cap_bit, char *out, size_t outlen)
{
	for (int i = 0; i < 64; i++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/video%d", i);
		int fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;
		struct v4l2_capability cap;
		memset(&cap, 0, sizeof cap);
		int ok = xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
		close(fd);
		if (!ok)
			continue;
		uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
		if (strcmp((const char *)cap.driver, driver) == 0 && (caps & cap_bit)) {
			snprintf(out, outlen, "%s", path);
			return 0;
		}
	}
	return -1;
}

static int find_subdev(char *out, size_t outlen)
{
	DIR *d = opendir("/sys/class/video4linux");
	if (!d)
		return -1;
	struct dirent *e;
	int found = -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "v4l-subdev", 10))
			continue;
		char p[300], name[64] = "";
		snprintf(p, sizeof p, "/sys/class/video4linux/%s/name", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f)
			continue;
		if (!fgets(name, sizeof name, f))
			name[0] = 0;
		fclose(f);
		if (strstr(name, "adv7180") || strstr(name, "adv728")) {
			snprintf(out, outlen, "/dev/%.50s", e->d_name);
			found = 0;
			break;
		}
	}
	closedir(d);
	return found;
}

/* ------------------------------------------------------------------ I2C: field parity */

static int i2c_rd(uint8_t reg, uint8_t *val)
{
	struct i2c_msg m[2] = {
		{ .addr = (uint16_t)g.i2c_addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = (uint16_t)g.i2c_addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	struct i2c_rdwr_ioctl_data d = { .msgs = m, .nmsgs = 2 };
	return ioctl(g.i2c_fd, I2C_RDWR, &d) == 2 ? 0 : -1;
}

static int i2c_wr(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	struct i2c_msg m = { .addr = (uint16_t)g.i2c_addr, .flags = 0, .len = 2, .buf = b };
	struct i2c_rdwr_ioctl_data d = { .msgs = &m, .nmsgs = 1 };
	return ioctl(g.i2c_fd, I2C_RDWR, &d) == 1 ? 0 : -1;
}

/* 1 = even field, 0 = odd field, -1 = error. Restores the map select register: the kernel driver caches it. */
static int read_even_field(void)
{
	uint8_t page, st;
	if (i2c_rd(ADV_REG_CTRL, &page))
		return -1;
	if (i2c_wr(ADV_REG_CTRL, ADV_MAP_IRQ)) {
		i2c_wr(ADV_REG_CTRL, page);
		return -1;
	}
	int r = i2c_rd(ADV_RAW_STATUS2, &st);
	i2c_wr(ADV_REG_CTRL, page);
	if (r)
		return -1;
	return (st >> 4) & 1;
}

static void open_i2c(void)
{
	if (g.fixed_phase >= 0)
		return;
	char path[32];
	snprintf(path, sizeof path, "/dev/i2c-%d", g.i2c_bus);
	g.i2c_fd = open(path, O_RDWR);
	if (g.i2c_fd < 0) {
		logm("warning: %s: %s (modprobe i2c-dev?) - field parity cannot be read, assuming --phase 0", path,
		     strerror(errno));
		g.fixed_phase = 0;
		phase_lock_init(&g.lock, 0);
		return;
	}
	if (read_even_field() < 0) {
		logm("warning: no answer from 0x%02x on %s - assuming --phase 0", g.i2c_addr, path);
		close(g.i2c_fd);
		g.i2c_fd = -1;
		g.fixed_phase = 0;
		phase_lock_init(&g.lock, 0);
	}
}

/* ------------------------------------------------------------------ capture (unicam) */

static void setup_decoder(void)
{
	v4l2_std_id std = g.ntsc ? V4L2_STD_NTSC_M : V4L2_STD_PAL;
	int sfd = -1, mc = 0;

	if (!g.subdev[0])
		find_subdev(g.subdev, sizeof g.subdev);
	if (g.subdev[0])
		sfd = open(g.subdev, O_RDWR);

	if (sfd >= 0) {
		mc = 1;
		v4l2_std_id det = 0;
		if (xioctl(sfd, VIDIOC_SUBDEV_QUERYSTD, &det) == 0 && det && !(det & std))
			logm("warning: input looks like %s, running as %s (--std)",
			     (det & V4L2_STD_525_60) ? "NTSC" : "PAL", g.ntsc ? "NTSC" : "PAL");
		if (xioctl(sfd, VIDIOC_SUBDEV_S_STD, &std) < 0)
			logm("warning: VIDIOC_SUBDEV_S_STD: %s", strerror(errno));

		struct v4l2_subdev_format sf;
		memset(&sf, 0, sizeof sf);
		sf.which = V4L2_SUBDEV_FORMAT_ACTIVE;
		sf.pad = 0;
		sf.format.code = MEDIA_BUS_FMT_UYVY8_1X16;
		sf.format.width = 720;
		sf.format.height = g.ntsc ? 240 : 288;
		sf.format.field = V4L2_FIELD_ALTERNATE;
		if (xioctl(sfd, VIDIOC_SUBDEV_S_FMT, &sf) < 0) {
			logm("error: %s: VIDIOC_SUBDEV_S_FMT: %s", g.subdev, strerror(errno));
			exit(1);
		}
		if (sf.format.field != V4L2_FIELD_ALTERNATE) {
			logm("error: decoder refused interlaced output (field %u)", sf.format.field);
			exit(1);
		}
		g.fw = sf.format.width;
		g.fh = sf.format.height;
		close(sfd);
		if (g.verbose)
			logm("decoder %s: %ux%u fields, %s", g.subdev, g.fw, g.fh, g.ntsc ? "NTSC" : "PAL");
	} else {
		logm("warning: no adv7180 subdevice (overlay without ',media-controller'?) - legacy mode");
	}

	if (!g.video[0] && find_video("unicam", V4L2_CAP_VIDEO_CAPTURE, g.video, sizeof g.video)) {
		logm("error: no unicam capture node - is the board connected and dtoverlay=adv728x-m set?");
		exit(1);
	}
	g.cap_fd = open(g.video, O_RDWR | O_NONBLOCK);
	if (g.cap_fd < 0) {
		logm("error: %s: %s", g.video, strerror(errno));
		exit(1);
	}

	struct v4l2_format f;
	memset(&f, 0, sizeof f);
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (mc) {
		f.fmt.pix.width = g.fw;
		f.fmt.pix.height = g.fh;
		f.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
		f.fmt.pix.field = V4L2_FIELD_ALTERNATE;
		if (xioctl(g.cap_fd, VIDIOC_S_FMT, &f) < 0) {
			logm("error: %s: VIDIOC_S_FMT: %s", g.video, strerror(errno));
			exit(1);
		}
	} else {
		/* legacy: never S_FMT (unicam would request field NONE = I2P); only set the standard and read back */
		xioctl(g.cap_fd, VIDIOC_S_STD, &std);
		if (xioctl(g.cap_fd, VIDIOC_G_FMT, &f) < 0) {
			logm("error: %s: VIDIOC_G_FMT: %s", g.video, strerror(errno));
			exit(1);
		}
		if (f.fmt.pix.height > 300) {
			logm("error: the decoder is in progressive (I2P) mode, %ux%u. Use dtoverlay=adv728x-m,adv7280m=1,"
			     "media-controller and reboot (docs/usb-grabber.md).", f.fmt.pix.width, f.fmt.pix.height);
			exit(1);
		}
		g.fw = f.fmt.pix.width;
		g.fh = f.fmt.pix.height;
	}
	if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY || f.fmt.pix.width != g.fw || f.fmt.pix.height != g.fh) {
		logm("error: unicam gave %ux%u %.4s, expected %ux%u UYVY", f.fmt.pix.width, f.fmt.pix.height,
		     (char *)&f.fmt.pix.pixelformat, g.fw, g.fh);
		exit(1);
	}
	g.fbpl = f.fmt.pix.bytesperline ? f.fmt.pix.bytesperline : g.fw * 2;
}

static void start_capture(void)
{
	struct v4l2_requestbuffers rb = { .count = CAP_BUFS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					  .memory = V4L2_MEMORY_MMAP };
	if (xioctl(g.cap_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
		logm("error: capture REQBUFS: %s", strerror(errno));
		exit(1);
	}
	for (unsigned i = 0; i < rb.count && i < CAP_BUFS; i++) {
		struct v4l2_buffer b = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
		if (xioctl(g.cap_fd, VIDIOC_QUERYBUF, &b) < 0) {
			logm("error: capture QUERYBUF: %s", strerror(errno));
			exit(1);
		}
		g.cap[i].len = b.length;
		g.cap[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, g.cap_fd, b.m.offset);
		if (g.cap[i].p == MAP_FAILED) {
			logm("error: capture mmap: %s", strerror(errno));
			exit(1);
		}
		if (xioctl(g.cap_fd, VIDIOC_QBUF, &b) < 0) {
			logm("error: capture QBUF: %s", strerror(errno));
			exit(1);
		}
	}
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(g.cap_fd, VIDIOC_STREAMON, &type) < 0) {
		logm("error: capture STREAMON: %s%s", strerror(errno),
		     errno == EPIPE ? " (format mismatch between decoder and unicam)" : "");
		exit(1);
	}
	g.start_ns = now_ns();
}

/* ------------------------------------------------------------------ UVC output */

static void fill_streaming_control(struct uvc_streaming_control *c)
{
	memset(c, 0, sizeof *c);
	c->bmHint = htole16(1);
	c->bFormatIndex = 1;
	c->bFrameIndex = 1;
	c->dwFrameInterval = htole32(g.interval);
	c->dwMaxVideoFrameSize = htole32((uint32_t)g.fsize);
	c->dwMaxPayloadTransferSize = htole32(MAX_PAYLOAD);
	c->bmFramingInfo = 3;
	c->bPreferedVersion = 1;
	c->bMaxVersion = 1;
}

static void stop_streaming(void)
{
	if (!g.streaming)
		return;
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	xioctl(g.uvc_fd, VIDIOC_STREAMOFF, &type);
	for (int i = 0; i < OUT_BUFS; i++)
		if (g.out[i].p) {
			munmap(g.out[i].p, g.out[i].len);
			g.out[i].p = NULL;
		}
	struct v4l2_requestbuffers rb = { .count = 0, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP };
	xioctl(g.uvc_fd, VIDIOC_REQBUFS, &rb);
	g.streaming = 0;
	g.out_queued = 0;
	g.cur = -1;
	logm("host stopped streaming");
}

static void start_streaming(void)
{
	stop_streaming();
	struct v4l2_format f;
	memset(&f, 0, sizeof f);
	f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	f.fmt.pix.width = g.W;
	f.fmt.pix.height = g.H;
	f.fmt.pix.pixelformat = g.nv12 ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_YUYV;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.bytesperline = g.nv12 ? g.W : g.W * 2;
	f.fmt.pix.sizeimage = (uint32_t)g.fsize;
	if (xioctl(g.uvc_fd, VIDIOC_S_FMT, &f) < 0)
		logm("warning: uvc S_FMT: %s", strerror(errno));
	/* f_uvc spreads every frame evenly over (frame interval / 125 us) requests, and its interval defaults to
	 * 666666 (15 fps) unless set here: without this a 25 fps stream is sent at 15 fps. */
	struct v4l2_streamparm sp;
	memset(&sp, 0, sizeof sp);
	sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	sp.parm.output.timeperframe.numerator = g.ntsc ? 1001 : 1;
	sp.parm.output.timeperframe.denominator = g.ntsc ? 30000 : 25;
	if (xioctl(g.uvc_fd, VIDIOC_S_PARM, &sp) < 0)
		logm("warning: uvc S_PARM: %s", strerror(errno));

	struct v4l2_requestbuffers rb = { .count = OUT_BUFS, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
					  .memory = V4L2_MEMORY_MMAP };
	if (xioctl(g.uvc_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
		logm("error: uvc REQBUFS: %s", strerror(errno));
		return;
	}
	for (unsigned i = 0; i < OUT_BUFS; i++) {
		g.out[i].p = NULL;
		g.out_st[i] = OUT_FREE;
		if (i >= rb.count) {
			g.out_st[i] = OUT_QUEUED;	/* never use */
			continue;
		}
		struct v4l2_buffer b = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP };
		if (xioctl(g.uvc_fd, VIDIOC_QUERYBUF, &b) < 0) {
			logm("error: uvc QUERYBUF: %s", strerror(errno));
			return;
		}
		g.out[i].len = b.length;
		g.out[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, g.uvc_fd, b.m.offset);
		if (g.out[i].p == MAP_FAILED) {
			g.out[i].p = NULL;
			logm("error: uvc mmap: %s", strerror(errno));
			return;
		}
	}
	g.streaming = 1;
	g.out_queued = 0;
	g.cur = -1;
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (xioctl(g.uvc_fd, VIDIOC_STREAMON, &type) < 0)	/* also completes the delayed USB status stage */
		logm("error: uvc STREAMON: %s", strerror(errno));
	logm("host started streaming %ux%u %s @ %s", g.W, g.H, g.nv12 ? "NV12" : "YUY2", g.ntsc ? "29.97" : "25");
}

static void reclaim_out(void)
{
	for (;;) {
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP };
		if (xioctl(g.uvc_fd, VIDIOC_DQBUF, &b) < 0)
			break;
		if (b.index < OUT_BUFS && g.out_st[b.index] == OUT_QUEUED) {
			g.out_st[b.index] = OUT_FREE;
			g.out_queued--;
		}
	}
}

/* ------------------------------------------------------------------ UVC control requests */

static int32_t *vc_slot(unsigned key)
{
	for (int i = 0; i < g.nvc; i++)
		if (g.vc[i].key == key)
			return &g.vc[i].val;
	if (g.nvc == (int)(sizeof g.vc / sizeof g.vc[0]))
		return NULL;
	g.vc[g.nvc].key = key;
	g.vc[g.nvc].val = 128;
	return &g.vc[g.nvc++].val;
}

static void put_le(uint8_t *d, unsigned len, int32_t v)
{
	for (unsigned i = 0; i < len; i++)
		d[i] = (uint8_t)((uint32_t)v >> (8 * (i < 4 ? i : 3)));
	if (len > 4)
		memset(d + 4, 0, len - 4);
}

/* Video Control interface: no real controls are offered (bmControls 0 in the gadget), but some hosts still
 * probe; answer like a simple 0..255 control instead of stalling, and report "no error" for the error code. */
static void control_request(const struct usb_ctrlrequest *r, unsigned entity, unsigned cs, unsigned len,
			    struct uvc_request_data *resp)
{
	if (entity == 0 && cs == UVC_VC_REQUEST_ERROR_CODE_CONTROL) {
		if (r->bRequest == UVC_GET_INFO) {
			resp->data[0] = 0x01;
			resp->length = 1;
		} else if (r->bRequest == UVC_GET_CUR) {
			resp->data[0] = 0;
			resp->length = 1;
		}
		return;
	}
	unsigned key = entity << 8 | cs;
	int32_t *slot = vc_slot(key);
	switch (r->bRequest) {
	case UVC_SET_CUR:
		g.pending_set = 3;
		g.pending_key = key;
		resp->length = (int)len;
		break;
	case UVC_GET_INFO:
		resp->data[0] = 0x03;
		resp->length = 1;
		break;
	case UVC_GET_LEN:
		put_le(resp->data, 2, 2);
		resp->length = 2;
		break;
	case UVC_GET_CUR:
		put_le(resp->data, len, slot ? *slot : 128);
		resp->length = (int)len;
		break;
	case UVC_GET_MIN:
		put_le(resp->data, len, 0);
		resp->length = (int)len;
		break;
	case UVC_GET_MAX:
		put_le(resp->data, len, 255);
		resp->length = (int)len;
		break;
	case UVC_GET_DEF:
		put_le(resp->data, len, 128);
		resp->length = (int)len;
		break;
	case UVC_GET_RES:
		put_le(resp->data, len, 1);
		resp->length = (int)len;
		break;
	}
}

static void streaming_request(const struct usb_ctrlrequest *r, unsigned cs, unsigned len,
			      struct uvc_request_data *resp)
{
	if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)
		return;
	struct uvc_streaming_control c;
	unsigned n = len < sizeof c ? len : (unsigned)sizeof c;
	switch (r->bRequest) {
	case UVC_SET_CUR:
		g.pending_set = cs == UVC_VS_PROBE_CONTROL ? 1 : 2;
		resp->length = (int)len;
		break;
	case UVC_GET_CUR:
		c = cs == UVC_VS_PROBE_CONTROL ? g.probe : g.commit;
		memcpy(resp->data, &c, n);
		resp->length = (int)n;
		break;
	case UVC_GET_MIN:
	case UVC_GET_MAX:
	case UVC_GET_DEF:
	case UVC_GET_RES:
		fill_streaming_control(&c);	/* one format, one frame size, one interval */
		memcpy(resp->data, &c, n);
		resp->length = (int)n;
		break;
	case UVC_GET_LEN:
		put_le(resp->data, 2, (int32_t)sizeof c);
		resp->length = 2;
		break;
	case UVC_GET_INFO:
		resp->data[0] = 0x03;
		resp->length = 1;
		break;
	}
}

static void handle_setup(const struct usb_ctrlrequest *r)
{
	struct uvc_request_data resp;
	memset(&resp, 0, sizeof resp);
	resp.length = -EL2HLT;		/* negative = stall */
	if ((r->bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS &&
	    (r->bRequestType & USB_RECIP_MASK) == USB_RECIP_INTERFACE) {
		unsigned windex = le16toh(r->wIndex), cs = le16toh(r->wValue) >> 8, len = le16toh(r->wLength);
		if (len > sizeof resp.data)
			len = sizeof resp.data;
		if ((windex & 0xff) == UVC_STRING_STREAMING_IDX)
			streaming_request(r, cs, len, &resp);
		else
			control_request(r, windex >> 8, cs, len, &resp);
	}
	if (g.verbose > 1)
		logm("setup %02x %02x wValue %04x wIndex %04x wLength %u -> %d", r->bRequestType, r->bRequest,
		     le16toh(r->wValue), le16toh(r->wIndex), le16toh(r->wLength), resp.length);
	if (xioctl(g.uvc_fd, UVCIOC_SEND_RESPONSE, &resp) < 0)
		logm("warning: UVCIOC_SEND_RESPONSE: %s", strerror(errno));
}

static void handle_data(const struct uvc_request_data *d)
{
	if (g.pending_set == 1) {
		fill_streaming_control(&g.probe);	/* whatever the host asked, there is exactly one choice */
	} else if (g.pending_set == 2) {
		fill_streaming_control(&g.commit);
		if (g.verbose)
			logm("host committed %ux%u", g.W, g.H);
	} else if (g.pending_set == 3) {
		int32_t *slot = vc_slot(g.pending_key);
		if (slot && d->length > 0) {
			int32_t v = 0;
			for (int i = 0; i < d->length && i < 4; i++)
				v |= (int32_t)d->data[i] << (8 * i);
			*slot = v;
		}
	}
	g.pending_set = 0;
}

static void handle_uvc_event(void)
{
	struct v4l2_event ev;
	while (xioctl(g.uvc_fd, VIDIOC_DQEVENT, &ev) == 0) {
		struct uvc_event *u = (struct uvc_event *)(void *)&ev.u.data;
		switch (ev.type) {
		case UVC_EVENT_CONNECT:
			if (g.verbose)
				logm("host connected");
			break;
		case UVC_EVENT_DISCONNECT:
			logm("host disconnected");
			stop_streaming();
			break;
		case UVC_EVENT_SETUP:
			handle_setup(&u->req);
			break;
		case UVC_EVENT_DATA:
			handle_data(&u->data);
			break;
		case UVC_EVENT_STREAMON:
			start_streaming();
			break;
		case UVC_EVENT_STREAMOFF:
			stop_streaming();
			break;
		}
	}
}

static void open_uvc(void)
{
	if (!g.uvc[0] && find_video("g_uvc", V4L2_CAP_VIDEO_OUTPUT, g.uvc, sizeof g.uvc)) {
		if (web.listen_fd >= 0) {
			logm("warning: no UVC gadget node - USB webcam off, web page only");
			return;
		}
		logm("error: no UVC gadget node - run sprutm-gadget.sh first (dtoverlay=dwc2,dr_mode=peripheral)");
		exit(1);
	}
	g.uvc_fd = open(g.uvc, O_RDWR | O_NONBLOCK);
	if (g.uvc_fd < 0) {
		if (web.listen_fd >= 0) {
			logm("warning: %s: %s - USB webcam off, web page only", g.uvc, strerror(errno));
			return;
		}
		logm("error: %s: %s", g.uvc, strerror(errno));
		exit(1);
	}
	static const uint32_t evs[] = { UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT, UVC_EVENT_SETUP, UVC_EVENT_DATA,
					UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF };
	for (unsigned i = 0; i < sizeof evs / sizeof evs[0]; i++) {
		struct v4l2_event_subscription s;
		memset(&s, 0, sizeof s);
		s.type = evs[i];
		if (xioctl(g.uvc_fd, VIDIOC_SUBSCRIBE_EVENT, &s) < 0)
			logm("warning: subscribe event %u: %s", evs[i], strerror(errno));
	}
	fill_streaming_control(&g.probe);
	fill_streaming_control(&g.commit);
}

#ifdef SPRUTM_HDMI
#include "hdmi.h"
#else
static void hdmi_set_comb(int on)
{
	(void)on;
}
static int hdmi_start(const char *mode)
{
	(void)mode;
	logm("warning: built without HDMI output (needs libdrm-dev and -DSPRUTM_HDMI, see install.sh)");
	return -1;
}
static void hdmi_field(uint32_t seq, const uint8_t *data, unsigned w, unsigned lines, unsigned bpl, int top)
{
	(void)seq, (void)data, (void)w, (void)lines, (void)bpl, (void)top;
}
#endif

/* ------------------------------------------------------------------ fields -> frames */

static void weave(uint8_t *frame, const uint8_t *field, int top)
{
	if (g.nv12)
		weave_field_nv12(frame, g.W, g.H, field, g.fw, g.fh, g.fbpl, top);
	else
		weave_field(frame, g.W, g.H, field, g.fw, g.fh, g.fbpl, top);
}

static void field_done(uint32_t seq, const uint8_t *data)
{
	g.n_fields++;
	if (web.listen_fd >= 0 || g.hdmi) {
		int locked = g.lock.phase >= 0;
		int wodd = field_is_odd(seq, (locked ? g.lock.phase : 0) ^ g.invert);
		int wtop = field_is_top(wodd, g.ntsc, g.swap);
		if (web.listen_fd >= 0)
			web_field(seq, data, g.fw, g.fh, g.fbpl, wtop, locked);
		if (g.hdmi)
			hdmi_field(seq, data, g.fw, g.fh, g.fbpl, wtop);
	}
	if (!g.streaming || g.lock.phase < 0)
		return;
	reclaim_out();
	/* --invert-phase: pair the fields the other way round if the I2C sample turns out to be one field off */
	int odd = field_is_odd(seq, g.lock.phase ^ g.invert);
	int top = field_is_top(odd, g.ntsc, g.swap);

	if (odd) {			/* first field of a frame */
		if (g.cur < 0) {
			for (int i = 0; i < OUT_BUFS; i++)
				if (g.out_st[i] == OUT_FREE && g.out[i].p) {
					g.cur = i;
					break;
				}
			if (g.cur < 0) {
				g.n_nobuf++;
				return;
			}
			g.out_st[g.cur] = OUT_FILLING;
			if (g.fw < g.W || 2 * g.fh < g.H) {
				if (g.nv12)
					fill_black_nv12(g.out[g.cur].p, g.W, g.H);
				else
					fill_black_yuyv(g.out[g.cur].p, g.fsize);
			}
		}
		g.cur_seq = seq;
		weave(g.out[g.cur].p, data, top);
		return;
	}
	if (g.cur < 0)
		return;
	if (seq != g.cur_seq + 1) {	/* lost the second field: drop the half-built frame */
		g.n_drop++;
		g.out_st[g.cur] = OUT_FREE;
		g.cur = -1;
		return;
	}
	weave(g.out[g.cur].p, data, top);
	struct v4l2_buffer b = { .index = (uint32_t)g.cur, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
				 .memory = V4L2_MEMORY_MMAP, .bytesused = (uint32_t)g.fsize };
	if (xioctl(g.uvc_fd, VIDIOC_QBUF, &b) < 0) {
		g.n_drop++;
		g.out_st[g.cur] = OUT_FREE;
	} else {
		g.out_st[g.cur] = OUT_QUEUED;
		g.out_queued++;
		g.n_frames++;
	}
	g.cur = -1;
}

static void handle_capture(void)
{
	for (;;) {
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
		if (xioctl(g.cap_fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno != EAGAIN)
				logm("warning: capture DQBUF: %s", strerror(errno));
			return;
		}
		int64_t ts = (int64_t)b.timestamp.tv_sec * 1000000000LL + (int64_t)b.timestamp.tv_usec * 1000;
		/* schedule a parity sample in the middle of the next field (timestamps are taken at Frame Start) */
		if (g.i2c_fd >= 0 && !g.sample_pending &&
		    (g.lock.phase < 0 || now_ns() - g.last_sample > 1000000000LL)) {
			g.sample_pending = 1;
			g.sample_seq = b.sequence + 1;
			g.sample_due = ts + g.field_ns + g.field_ns * 45 / 100;
		}
		if (!(b.flags & V4L2_BUF_FLAG_ERROR) && b.index < CAP_BUFS)
			field_done(b.sequence, g.cap[b.index].p);
		if (xioctl(g.cap_fd, VIDIOC_QBUF, &b) < 0)
			logm("warning: capture QBUF: %s", strerror(errno));
	}
}

static void do_parity_sample(void)
{
	int64_t t = now_ns();
	g.sample_pending = 0;
	if (t - g.sample_due > g.field_ns * 30 / 100)
		return;		/* too late, the next field may have begun */
	int even = read_even_field();
	g.last_sample = t;
	if (even < 0)
		return;
	int was = g.lock.phase;
	if (phase_lock_feed(&g.lock, phase_from_sample(g.sample_seq, even), LOCK_NEED)) {
		if (was < 0)
			logm("field parity locked (phase %d)", g.lock.phase);
		else
			logm("field parity changed to phase %d (lost fields?)", g.lock.phase);
		if (g.cur >= 0) {	/* the half-built frame was paired with the wrong parity */
			g.out_st[g.cur] = OUT_FREE;
			g.cur = -1;
		}
	}
}

/* ------------------------------------------------------------------ web page: status and controls */

struct sbuf {
	char *p;
	size_t n, cap;
};

static void sb_add(struct sbuf *b, const char *fmt, ...)
{
	if (b->n >= b->cap)
		return;
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
	va_end(ap);
	if (r > 0)
		b->n = b->n + (size_t)r < b->cap ? b->n + (size_t)r : b->cap;
}

static void sb_str(struct sbuf *b, const char *s)	/* JSON string, printable ASCII only */
{
	char t[64];
	size_t i = 0;
	for (; *s && i < sizeof t - 1; s++)
		t[i++] = (*s == '"' || *s == '\\' || (unsigned char)*s < 32 || (unsigned char)*s > 126) ? ' ' : *s;
	t[i] = 0;
	sb_add(b, "\"%s\"", t);
}

/* main register map; the kernel driver caches the map select, so it is restored */
static int rd_main(uint8_t reg, uint8_t *v)
{
	uint8_t page;
	if (g.i2c_fd < 0 || i2c_rd(ADV_REG_CTRL, &page))
		return -1;
	if (page)
		i2c_wr(ADV_REG_CTRL, 0);
	int r = i2c_rd(reg, v);
	if (page)
		i2c_wr(ADV_REG_CTRL, page);
	return r;
}

static int wr_main(uint8_t reg, uint8_t v)
{
	uint8_t page;
	if (g.i2c_fd < 0 || i2c_rd(ADV_REG_CTRL, &page))
		return -1;
	if (page)
		i2c_wr(ADV_REG_CTRL, 0);
	int r = i2c_wr(reg, v);
	if (page)
		i2c_wr(ADV_REG_CTRL, page);
	return r;
}

/* Keep the decoder's analog input clamp on. The kernel adv7180 driver writes the test pattern number straight into
 * register 0x14 (Analog Clamp Control) and so clears CCLEN, bit 4 (the current sources that hold the dc level of
 * the ac-coupled input), already at start-up with "Disable". Without them the input floats: no IN_LOCK, the luma
 * AGC wanders and black sits at Y 110-150 instead of 16 (washed-out picture). Checked once a second and after
 * every control change from the web page. */
static void clamp_check(void)
{
	static int told;
	uint8_t v;
	if (rd_main(0x14, &v) || (v & 0x10))
		return;
	if (wr_main(0x14, (uint8_t)(v | 0x10)) == 0 && !told++)
		logm("decoder input clamp was off (reg 0x14 = 0x%02x, cleared by the kernel driver): turned on", v);
}

/* Decoder settings the kernel driver does not offer as V4L2 controls; written straight to the main register map.
 * mask 0: a plain 0..255 register. mask != 0: an on/off switch, on = (reg & mask) == on, off = those bits 0.
 * Luma Peaking (UG-637 "DNR and luma peaking filter"): boosts luma around 3 MHz, i.e. sharpens edges;
 *   0x40 = 0 dB (off, the chip default), higher = sharper, lower = softer.
 * 3D Comb: PAL luma comb mode YCMP (reg 0x39 bits 2:0) 100 = no line comb, the luma keeps its full detail plus
 *   the colour subcarrier as a fine dot pattern; the web page removes the dots with a 3D comb (fields two frames
 *   apart, where the PAL subcarrier is inverted) and keeps the detail. Off = 000, the chip's adaptive line comb,
 *   which also blurs fine strokes such as small text. The USB webcam gets no 3D comb: it shows the dots. */
static const struct {
	uint32_t id;
	const char *name;
	uint8_t reg, def, mask, on;
} reg_ctls[] = { { 0x7ad000fbu, "Luma Peaking", 0xfb, 0x40, 0, 0 }, { 0x7ad00039u, "3D Comb", 0x39, 0, 0x07, 0x04 } };

static int reg_ctl_set(size_t i, int val)
{
	uint8_t v = 0;
	if (reg_ctls[i].mask) {
		if (rd_main(reg_ctls[i].reg, &v))
			return -1;
		v = (uint8_t)((v & ~reg_ctls[i].mask) | (val ? reg_ctls[i].on : 0));
	} else {
		v = (uint8_t)(val < 0 ? 0 : val > 255 ? 255 : val);
	}
	int e = wr_main(reg_ctls[i].reg, v);
	if (!e && reg_ctls[i].reg == 0x39)
		hdmi_set_comb(val != 0);	/* the HDMI output does the 3D comb too */
	return e;
}

/* the decoder's V4L2 controls (brightness, contrast, ..., test pattern) as JSON, to one viewer or all */
static void web_send_ctrls(struct web_client *to)
{
	char buf[6000];
	struct sbuf b = { buf, 0, sizeof buf };
	sb_add(&b, "{\"type\":\"ctrls\",\"list\":[");
	int first = 1;
	if (g.sub_fd >= 0) {
		struct v4l2_queryctrl q;
		memset(&q, 0, sizeof q);
		q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
		while (xioctl(g.sub_fd, VIDIOC_QUERYCTRL, &q) == 0) {
			if (!(q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)) &&
			    (q.type == V4L2_CTRL_TYPE_INTEGER || q.type == V4L2_CTRL_TYPE_BOOLEAN ||
			     q.type == V4L2_CTRL_TYPE_MENU)) {
				struct v4l2_control ct = { .id = q.id };
				xioctl(g.sub_fd, VIDIOC_G_CTRL, &ct);
				sb_add(&b, "%s{\"id\":%u,\"name\":", first ? "" : ",", q.id);
				sb_str(&b, (const char *)q.name);
				sb_add(&b, ",\"type\":%u,\"min\":%d,\"max\":%d,\"step\":%d,\"def\":%d,\"val\":%d", q.type,
				       q.minimum, q.maximum, q.step, q.default_value, ct.value);
				if (q.type == V4L2_CTRL_TYPE_MENU) {
					sb_add(&b, ",\"menu\":[");
					for (int i = q.minimum; i <= q.maximum && i < 32; i++) {
						struct v4l2_querymenu m;
						memset(&m, 0, sizeof m);
						m.id = q.id;
						m.index = (uint32_t)i;
						if (i > q.minimum)
							sb_add(&b, ",");
						sb_str(&b, xioctl(g.sub_fd, VIDIOC_QUERYMENU, &m) == 0 ? (const char *)m.name : "");
					}
					sb_add(&b, "]");
				}
				sb_add(&b, "}");
				first = 0;
			}
			q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
		}
	}
	/* decoder registers the kernel driver has no control for (ids outside the V4L2 range) */
	for (size_t i = 0; i < sizeof reg_ctls / sizeof reg_ctls[0]; i++) {
		uint8_t v;
		if (rd_main(reg_ctls[i].reg, &v))
			continue;
		sb_add(&b, "%s{\"id\":%u,\"name\":", first ? "" : ",", reg_ctls[i].id);
		sb_str(&b, reg_ctls[i].name);
		if (reg_ctls[i].mask)
			sb_add(&b, ",\"type\":2,\"min\":0,\"max\":1,\"step\":1,\"def\":%d,\"val\":%d}", reg_ctls[i].def,
			       (v & reg_ctls[i].mask) == reg_ctls[i].on);
		else
			sb_add(&b, ",\"type\":1,\"min\":0,\"max\":255,\"step\":1,\"def\":%d,\"val\":%d}", reg_ctls[i].def, v);
		first = 0;
	}
	sb_add(&b, "]}");
	if (b.n >= b.cap)
		return;
	if (to)
		web_text(to, buf);
	else
		web_text_all(buf);
}

static void web_opened(struct web_client *c)
{
	if (g.verbose)
		logm("web: viewer connected (%d)", web_clients());
	web_send_ctrls(c);
}

static void web_command(struct web_client *c, char *msg)
{
	unsigned id;
	int val;
	if (sscanf(msg, "ctrl %u %d", &id, &val) == 2) {
		for (size_t i = 0; i < sizeof reg_ctls / sizeof reg_ctls[0]; i++)
			if (reg_ctls[i].id == id) {
				if (reg_ctl_set(i, val))
					logm("warning: web: register 0x%02x: write failed", reg_ctls[i].reg);
				else if (g.verbose)
					logm("web: %s (reg 0x%02x) = %d", reg_ctls[i].name, reg_ctls[i].reg, val);
				web_send_ctrls(NULL);
				return;
			}
		if (g.sub_fd < 0)
			return;
		struct v4l2_control ct = { .id = id, .value = val };
		if (xioctl(g.sub_fd, VIDIOC_S_CTRL, &ct) < 0)
			logm("warning: web: control 0x%x = %d: %s", id, val, strerror(errno));
		else if (g.verbose)
			logm("web: control 0x%x = %d", id, val);
		clamp_check();
		web_send_ctrls(NULL);
	} else if (!strncmp(msg, "ctrls", 5)) {
		web_send_ctrls(c);
	}
}

/* once a second: decoder status (Status 1/2/3, UG-637) and counters, to every viewer */
static void web_status(void)
{
	uint8_t s1 = 0, s2 = 0, s3 = 0;
	int ok = rd_main(0x10, &s1) == 0 && rd_main(0x12, &s2) == 0 && rd_main(0x13, &s3) == 0;
	for (int i = 0; i < WEB_MAX_CLIENTS; i++) {
		struct web_client *c = &web.c[i];
		if (c->fd < 0 || !c->ws)
			continue;
		char buf[400];
		struct sbuf b = { buf, 0, sizeof buf };
		sb_add(&b, "{\"type\":\"stat\",\"std\":\"%s\",\"fields\":%lu,\"frames\":%lu,\"drop\":%lu,\"phase\":%d,"
			   "\"usb\":%d,\"sent\":%lu,\"skipped\":%lu,\"viewers\":%d", g.ntsc ? "ntsc" : "pal", g.n_fields,
		       g.n_frames, g.n_drop, g.lock.phase, g.streaming, c->fields_sent, c->fields_skipped, web_clients());
		if (ok)
			sb_add(&b, ",\"st1\":%u,\"st2\":%u,\"st3\":%u", s1, s2, s3);
		sb_add(&b, "}");
		web_text(c, buf);
	}
}

/* ------------------------------------------------------------------ main */

static void on_signal(int s)
{
	(void)s;
	quit = 1;
}

static void usage(void)
{
	puts("sprutm-uvc: SPRUT-M (ADV7280-M) fields -> woven uncompressed frames -> USB UVC gadget\n"
	     "  --std pal|ntsc     input standard (default pal: 720x576 @ 25)\n"
	     "  --format nv12|yuyv frame format, must match sprutm-gadget.sh (default nv12: luma bit-exact; YUY2\n"
	     "                     needs 20.7 MB/s, more than the Pi 4 USB-C can send)\n"
	     "  --swap             swap top/bottom lines (picture looks line-swapped / jagged)\n"
	     "  --invert-phase     pair fields the other way round (still picture clean, motion jerks back and forth)\n"
	     "  --phase 0|1        fixed field parity, do not read it over I2C (the right value changes on each start)\n"
	     "  --i2c-bus N        I2C bus of the decoder (default 10)\n"
	     "  --i2c-addr A       decoder address (default 0x21)\n"
	     "  --video DEV        unicam capture node (default: auto)\n"
	     "  --subdev DEV       decoder subdevice (default: auto)\n"
	     "  --uvc DEV          UVC gadget node (default: auto)\n"
	     "  --web-port N       web page port (default 80, 0 = off)\n"
	     "  --www DIR          directory with index.html (default /usr/local/share/sprutm)\n"
	     "  --peaking N        decoder luma peaking (sharpness) 0..255, reg 0xFB: 64 = off (chip default),\n"
	     "                     higher = sharper edges (the web page has a slider to find the value)\n"
	     "  --comb3d           decoder luma line comb off at start: the web page's 3D comb keeps fine detail\n"
	     "                     (small text) sharp; the USB webcam then shows the colour subcarrier as fine dots\n"
	     "  --hdmi             video on the Pi's HDMI port too (lowest delay; 50 Hz mode when the monitor has one)\n"
	     "  --hdmi-mode WxH@R  HDMI mode, e.g. 1280x720@50 (default: the monitor's resolution at 50 Hz)\n"
	     "  -v                 verbose (twice: log every USB request)");
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "std", required_argument, 0, 's' }, { "format", required_argument, 0, 'f' }, { "swap", no_argument, 0, 'w' },
		{ "invert-phase", no_argument, 0, 'n' },
		{ "phase", required_argument, 0, 'p' }, { "i2c-bus", required_argument, 0, 'b' },
		{ "i2c-addr", required_argument, 0, 'a' }, { "video", required_argument, 0, 'V' },
		{ "subdev", required_argument, 0, 'S' }, { "uvc", required_argument, 0, 'U' },
		{ "web-port", required_argument, 0, 'P' }, { "www", required_argument, 0, 'W' },
		{ "peaking", required_argument, 0, 'K' }, { "comb3d", no_argument, 0, 'C' },
		{ "hdmi", no_argument, 0, 'D' }, { "hdmi-mode", required_argument, 0, 'M' },
		{ "help", no_argument, 0, 'h' }, { 0, 0, 0, 0 } };
	int o;
	while ((o = getopt_long(argc, argv, "vh", opts, NULL)) != -1) {
		switch (o) {
		case 's':
			if (!strcasecmp(optarg, "ntsc"))
				g.ntsc = 1;
			else if (strcasecmp(optarg, "pal")) {
				usage();
				return 2;
			}
			break;
		case 'f':
			if (!strcasecmp(optarg, "yuyv") || !strcasecmp(optarg, "yuy2"))
				g.nv12 = 0;
			else if (strcasecmp(optarg, "nv12")) {
				usage();
				return 2;
			}
			break;
		case 'w': g.swap = 1; break;
		case 'n': g.invert = 1; break;
		case 'p': g.fixed_phase = atoi(optarg) & 1; break;
		case 'b': g.i2c_bus = atoi(optarg); break;
		case 'a': g.i2c_addr = (int)strtol(optarg, NULL, 0); break;
		case 'V': snprintf(g.video, sizeof g.video, "%s", optarg); break;
		case 'S': snprintf(g.subdev, sizeof g.subdev, "%s", optarg); break;
		case 'U': snprintf(g.uvc, sizeof g.uvc, "%s", optarg); break;
		case 'P': g.web_port = atoi(optarg); break;
		case 'W': snprintf(g.www, sizeof g.www, "%s", optarg); break;
		case 'K': g.peaking = (int)strtol(optarg, NULL, 0) & 255; break;
		case 'C': g.comb3d = 1; break;
		case 'D': g.hdmi = 1; break;
		case 'M': g.hdmi = 1; snprintf(g.hdmi_mode, sizeof g.hdmi_mode, "%s", optarg); break;
		case 'v': g.verbose++; break;
		default:
			usage();
			return o == 'h' ? 0 : 2;
		}
	}
	g.W = 720;
	g.H = g.ntsc ? 480 : 576;
	g.interval = g.ntsc ? 333667 : 400000;
	g.fsize = g.nv12 ? (size_t)g.W * g.H * 3 / 2 : (size_t)g.W * g.H * 2;
	g.field_ns = g.ntsc ? 16683333 : 20000000;
	phase_lock_init(&g.lock, g.fixed_phase);

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	setup_decoder();
	open_i2c();
	if (g.web_port > 0) {
		if (web_init(g.web_port, g.www) < 0)
			logm("warning: web page off: port %d: %s", g.web_port, strerror(errno));
		web.on_open = web_opened;
		web.on_text = web_command;
		if (g.subdev[0])
			g.sub_fd = open(g.subdev, O_RDWR);
	}
	open_uvc();
	start_capture();
	clamp_check();
	if (g.peaking >= 0 && wr_main(0xfb, (uint8_t)g.peaking) == 0)
		logm("decoder luma peaking (sharpness) = %d (64 = off)", g.peaking);
	if (g.comb3d && reg_ctl_set(1, 1) == 0)
		logm("decoder luma line comb off (web page 3D comb)");
	if (g.hdmi) {
		uint8_t v;
		if (rd_main(0x39, &v) == 0)
			hdmi_set_comb((v & 0x07) == 0x04);
		hdmi_start(g.hdmi_mode);
	}
	logm("running: %s -> %s, fields %ux%u (bpl %u), frames %ux%u %s, parity %s%s%s", g.video,
	     g.uvc_fd >= 0 ? g.uvc : "(no USB)", g.fw, g.fh, g.fbpl, g.W, g.H, g.nv12 ? "NV12" : "YUY2",
	     g.fixed_phase >= 0 ? "fixed" : "over I2C", g.invert ? ", inverted" : "", g.swap ? ", swapped" : "");
	if (web.listen_fd >= 0)
		logm("web page: http://<this Pi>:%d/  (%s/index.html)", g.web_port, g.www);

	int64_t last_stat = now_ns(), last_web = last_stat;
	int lock_warned = 0;
	while (!quit) {
		struct pollfd p[2 + WEB_MAX_CLIENTS + 1] = {
			{ .fd = g.cap_fd, .events = POLLIN },
			{ .fd = g.uvc_fd, .events = POLLPRI | (g.out_queued > 0 ? POLLOUT : 0) } };
		int np = 2 + web_pollfds(p + 2);
		int timeout = 200;
		if (g.sample_pending) {
			int64_t dt = g.sample_due - now_ns();
			timeout = dt <= 0 ? 0 : (int)((dt + 999999) / 1000000);
		}
		int r = poll(p, (nfds_t)np, timeout);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			logm("error: poll: %s", strerror(errno));
			break;
		}
		if (p[1].revents & POLLPRI)
			handle_uvc_event();
		if (p[1].revents & POLLOUT)
			reclaim_out();
		if (p[0].revents & POLLIN)
			handle_capture();
		if (p[0].revents & POLLERR) {
			logm("error: capture stream failed");
			break;
		}
		if (g.sample_pending && now_ns() >= g.sample_due)
			do_parity_sample();
		web_handle(p + 2);

		int64_t t = now_ns();
		if (t - last_web >= 1000000000LL && !g.sample_pending) {
			clamp_check();
			if (web.listen_fd >= 0 && web_clients())
				web_status();
			last_web = t;
		}
		if (g.lock.phase < 0 && !lock_warned && t - g.start_ns > 3000000000LL) {
			logm("warning: field parity not locked after 3 s (%s) - using phase 0, try --phase 1 if the "
			     "picture jumps", g.n_fields ? "I2C reads inconsistent" : "no fields from the decoder");
			phase_lock_init(&g.lock, 0);
			lock_warned = 1;
		}
		if (g.verbose && t - last_stat >= 5000000000LL) {
			logm("fields %lu, frames %lu, dropped %lu, no free buffer %lu, phase %d", g.n_fields, g.n_frames,
			     g.n_drop, g.n_nobuf, g.lock.phase);
			last_stat = t;
		}
	}
	stop_streaming();
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(g.cap_fd, VIDIOC_STREAMOFF, &type);
	logm("stopped");
	return 0;
}
