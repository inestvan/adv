/* sprutm-uvc: SPRUT-M (ADV7280-M over CSI-2) -> Raspberry Pi 4 -> USB UVC webcam, uncompressed and lossless.
 *
 * The decoder runs in interlaced mode: one 720 x 288 (PAL) / 720 x 240 (NTSC) field per CSI-2 frame, 50/60 fields
 * per second. Two fields of one frame are woven line by line into 720 x 576 / 720 x 480 YUY2 and handed to the
 * UVC gadget (f_uvc, configured by sprutm-gadget.sh). No scaling, no filtering, no compression: the only change
 * is the byte order UYVY -> YUYV. Deinterlacing is left to the host (OBS "Yadif 2x" etc.).
 *
 * Why the field parity is read over I2C: unicam (the Pi CSI-2 receiver) does not report TOP/BOTTOM, it only
 * counts frames (v4l2_buffer.sequence) and timestamps them at Frame Start. The decoder reports the field it is
 * currently receiving in Raw Status 2 (interrupt/VDP sub-map, 0x45 bit 4 EVEN_FIELD). Sampling it in the middle
 * of a known field gives the sequence -> parity relation once; the frame counter keeps it from then on.
 *
 * Why Media Controller mode: in legacy mode unicam never asks the sensor for an interlaced format and the adv7180
 * driver then switches the chip to its I2P deinterlacer. Here the subdevice is set to field ALTERNATE directly.
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

#define CAP_BUFS	6
#define OUT_BUFS	4
#define MAX_PAYLOAD	3072	/* = streaming_maxpacket in sprutm-gadget.sh (HS, 3 x 1024 per microframe) */
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
	int ntsc, swap, invert, fixed_phase, verbose, i2c_bus, i2c_addr;
	char video[64], subdev[64], uvc[64];
	/* geometry */
	unsigned fw, fh, fbpl;		/* field from unicam */
	unsigned W, H;			/* frame to the host */
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
} g = { .fixed_phase = -1, .i2c_bus = 10, .i2c_addr = 0x21, .cap_fd = -1, .uvc_fd = -1, .i2c_fd = -1, .cur = -1 };

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
			snprintf(out, outlen, "/dev/%s", e->d_name);
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
	c->dwMaxVideoFrameSize = htole32(g.W * g.H * 2);
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
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.bytesperline = g.W * 2;
	f.fmt.pix.sizeimage = g.W * g.H * 2;
	if (xioctl(g.uvc_fd, VIDIOC_S_FMT, &f) < 0)
		logm("warning: uvc S_FMT: %s", strerror(errno));

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
	logm("host started streaming %ux%u YUY2 @ %s", g.W, g.H, g.ntsc ? "29.97" : "25");
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
		logm("error: no UVC gadget node - run sprutm-gadget.sh first (dtoverlay=dwc2,dr_mode=peripheral)");
		exit(1);
	}
	g.uvc_fd = open(g.uvc, O_RDWR | O_NONBLOCK);
	if (g.uvc_fd < 0) {
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

/* ------------------------------------------------------------------ fields -> frames */

static void field_done(uint32_t seq, const uint8_t *data)
{
	g.n_fields++;
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
			if (g.fw < g.W || 2 * g.fh < g.H)
				fill_black_yuyv(g.out[g.cur].p, (size_t)g.W * g.H * 2);
		}
		g.cur_seq = seq;
		weave_field(g.out[g.cur].p, g.W, g.H, data, g.fw, g.fh, g.fbpl, top);
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
	weave_field(g.out[g.cur].p, g.W, g.H, data, g.fw, g.fh, g.fbpl, top);
	struct v4l2_buffer b = { .index = (uint32_t)g.cur, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
				 .memory = V4L2_MEMORY_MMAP, .bytesused = g.W * g.H * 2 };
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

/* ------------------------------------------------------------------ main */

static void on_signal(int s)
{
	(void)s;
	quit = 1;
}

static void usage(void)
{
	puts("sprutm-uvc: SPRUT-M (ADV7280-M) fields -> lossless woven YUY2 frames -> USB UVC gadget\n"
	     "  --std pal|ntsc     input standard (default pal: 720x576 @ 25)\n"
	     "  --swap             swap top/bottom lines (picture looks line-swapped / jagged)\n"
	     "  --invert-phase     pair fields the other way round (still picture clean, motion jerks back and forth)\n"
	     "  --phase 0|1        fixed field parity, do not read it over I2C (the right value changes on each start)\n"
	     "  --i2c-bus N        I2C bus of the decoder (default 10)\n"
	     "  --i2c-addr A       decoder address (default 0x21)\n"
	     "  --video DEV        unicam capture node (default: auto)\n"
	     "  --subdev DEV       decoder subdevice (default: auto)\n"
	     "  --uvc DEV          UVC gadget node (default: auto)\n"
	     "  -v                 verbose (twice: log every USB request)");
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "std", required_argument, 0, 's' }, { "swap", no_argument, 0, 'w' },
		{ "invert-phase", no_argument, 0, 'n' },
		{ "phase", required_argument, 0, 'p' }, { "i2c-bus", required_argument, 0, 'b' },
		{ "i2c-addr", required_argument, 0, 'a' }, { "video", required_argument, 0, 'V' },
		{ "subdev", required_argument, 0, 'S' }, { "uvc", required_argument, 0, 'U' },
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
		case 'w': g.swap = 1; break;
		case 'n': g.invert = 1; break;
		case 'p': g.fixed_phase = atoi(optarg) & 1; break;
		case 'b': g.i2c_bus = atoi(optarg); break;
		case 'a': g.i2c_addr = (int)strtol(optarg, NULL, 0); break;
		case 'V': snprintf(g.video, sizeof g.video, "%s", optarg); break;
		case 'S': snprintf(g.subdev, sizeof g.subdev, "%s", optarg); break;
		case 'U': snprintf(g.uvc, sizeof g.uvc, "%s", optarg); break;
		case 'v': g.verbose++; break;
		default:
			usage();
			return o == 'h' ? 0 : 2;
		}
	}
	g.W = 720;
	g.H = g.ntsc ? 480 : 576;
	g.interval = g.ntsc ? 333667 : 400000;
	g.field_ns = g.ntsc ? 16683333 : 20000000;
	phase_lock_init(&g.lock, g.fixed_phase);

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	setup_decoder();
	open_i2c();
	open_uvc();
	start_capture();
	logm("running: %s -> %s, fields %ux%u (bpl %u), frames %ux%u YUY2, parity %s%s%s", g.video, g.uvc, g.fw,
	     g.fh, g.fbpl, g.W, g.H, g.fixed_phase >= 0 ? "fixed" : "over I2C", g.invert ? ", inverted" : "",
	     g.swap ? ", swapped" : "");

	int64_t last_stat = now_ns();
	int lock_warned = 0;
	while (!quit) {
		struct pollfd p[2] = { { .fd = g.cap_fd, .events = POLLIN },
				       { .fd = g.uvc_fd, .events = POLLPRI | (g.out_queued > 0 ? POLLOUT : 0) } };
		int timeout = 200;
		if (g.sample_pending) {
			int64_t dt = g.sample_due - now_ns();
			timeout = dt <= 0 ? 0 : (int)((dt + 999999) / 1000000);
		}
		int r = poll(p, 2, timeout);
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

		int64_t t = now_ns();
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
