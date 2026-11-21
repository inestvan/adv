/* Field weaving for sprutm-uvc: pure C, no Linux headers, so it is unit-tested on any host (test_weave.c).
 *
 * The ADV7280-M (interlaced mode) sends one field per CSI-2 frame: 720 x 288 (PAL) or 720 x 240 (NTSC),
 * UYVY. Two consecutive fields are woven, line by line, into one 720 x 576 / 720 x 480 frame. Nothing is
 * interpolated or scaled.
 *
 * Two output formats:
 *  - YUY2 (4:2:2): every sample of the decoder unchanged, only the byte order UYVY -> YUYV. 20.7 MB/s at
 *    720x576 @ 25: more than the Pi 4 USB-C gadget can send (dwc2: 2048 bytes per microframe = 16.3 MB/s).
 *  - NV12 (4:2:0, the default): luma unchanged, chroma averaged over two lines OF THE SAME FIELD (interlaced
 *    4:2:0 as in MPEG-2), chroma rows of the two fields interleaved. 15.6 MB/s. PAL already averages chroma
 *    of adjacent lines (delay line), so this loses practically nothing.
 */
#ifndef SPRUTM_WEAVE_H
#define SPRUTM_WEAVE_H

#include <stdint.h>
#include <string.h>

/* UYVY (U0 Y0 V0 Y1) -> YUYV (Y0 U0 Y1 V0): swap the two bytes of every 16-bit half. width in pixels (even). */
static inline void uyvy_to_yuyv_line(uint8_t *dst, const uint8_t *src, unsigned width)
{
	const uint32_t *s = (const uint32_t *)(const void *)src;
	uint32_t *d = (uint32_t *)(void *)dst;
	unsigned n = width / 2;			/* one 32-bit word = 2 pixels */
	for (unsigned i = 0; i < n; i++) {
		uint32_t v = s[i];
		d[i] = ((v >> 8) & 0x00ff00ffu) | ((v << 8) & 0xff00ff00u);
	}
}

/* Copy one field into the even (top) or odd (bottom) rows of a YUYV frame.
 * frame: frame_w x frame_h, stride frame_w * 2. field: field_w x field_h UYVY with stride field_bpl.
 * A field narrower/shorter than the frame leaves the rest of the frame untouched (pre-filled black). */
static inline void weave_field(uint8_t *frame, unsigned frame_w, unsigned frame_h,
			       const uint8_t *field, unsigned field_w, unsigned field_h, unsigned field_bpl, int top)
{
	unsigned w = field_w < frame_w ? field_w : frame_w;
	for (unsigned i = 0; i < field_h; i++) {
		unsigned row = 2 * i + (top ? 0 : 1);
		if (row >= frame_h)
			break;
		uyvy_to_yuyv_line(frame + (size_t)row * frame_w * 2, field + (size_t)i * field_bpl, w);
	}
}

/* Black in YUYV (studio range): Y 16, U/V 128. */
static inline void fill_black_yuyv(uint8_t *frame, size_t bytes)
{
	for (size_t i = 0; i + 1 < bytes; i += 2) {
		frame[i] = 0x10;
		frame[i + 1] = 0x80;
	}
}

/* NV12 frame: Y plane frame_w x frame_h (stride frame_w), then the UV plane frame_w x frame_h / 2 (U V per two
 * pixels, stride frame_w). One field goes into the even (top) or odd (bottom) Y rows; its chroma, averaged over
 * field lines 2j and 2j+1, goes into chroma row 2j (top) / 2j+1 (bottom). */
static inline void weave_field_nv12(uint8_t *frame, unsigned frame_w, unsigned frame_h,
				    const uint8_t *field, unsigned field_w, unsigned field_h, unsigned field_bpl, int top)
{
	unsigned w = (field_w < frame_w ? field_w : frame_w) & ~1u;
	unsigned par = top ? 0 : 1;
	uint8_t *yp = frame, *uvp = frame + (size_t)frame_w * frame_h;

	for (unsigned i = 0; i < field_h; i++) {
		unsigned row = 2 * i + par;
		if (row >= frame_h)
			break;
		const uint8_t *s = field + (size_t)i * field_bpl;
		uint8_t *d = yp + (size_t)row * frame_w;
		for (unsigned x = 0; x < w; x++)
			d[x] = s[2 * x + 1];				/* U Y V Y: luma at odd bytes */
	}
	for (unsigned j = 0; 2 * j < field_h; j++) {
		unsigned crow = 2 * j + par;
		if (crow >= frame_h / 2)
			break;
		const uint8_t *s0 = field + (size_t)(2 * j) * field_bpl;
		const uint8_t *s1 = 2 * j + 1 < field_h ? s0 + field_bpl : s0;
		uint8_t *d = uvp + (size_t)crow * frame_w;
		for (unsigned x = 0; x < w; x += 2) {			/* pixel pair x/2: U at byte 2x, V at 2x+2 */
			d[x] = (uint8_t)((s0[2 * x] + s1[2 * x] + 1) >> 1);
			d[x + 1] = (uint8_t)((s0[2 * x + 2] + s1[2 * x + 2] + 1) >> 1);
		}
	}
}

/* Black in NV12: Y 16, U/V 128. */
static inline void fill_black_nv12(uint8_t *frame, unsigned frame_w, unsigned frame_h)
{
	size_t ysz = (size_t)frame_w * frame_h;
	memset(frame, 0x10, ysz);
	memset(frame + ysz, 0x80, ysz / 2);
}

/* Field parity bookkeeping.
 * unicam counts every CSI-2 frame end in v4l2_buffer.sequence, also when it had no buffer to fill, so the
 * parity of 'sequence' follows the field parity of the decoder. 'phase' links the two:
 *     field 'seq' is the odd field (field 1) of the frame  <=>  (seq & 1) == phase
 * phase_from_sample(): the decoder reported EVEN_FIELD = even for the field with sequence number seq. */
static inline int phase_from_sample(uint32_t seq, int even)
{
	return (int)((seq & 1u) ^ (even ? 1u : 0u));
}

static inline int field_is_odd(uint32_t seq, int phase)
{
	return (int)(seq & 1u) == phase;
}

/* Spatial position. PAL/625: field 1 (odd) holds the top line of the picture and is sent first (top field
 * first). NTSC/525: field 1 is the bottom field and is still sent first (bottom field first). 'swap' inverts
 * the rule in case the decoder crop shifts the fields by one line (picture looks line-swapped). */
static inline int field_is_top(int odd, int ntsc, int swap)
{
	int top = ntsc ? !odd : odd;
	return swap ? !top : top;
}

/* Majority vote over parity samples: keeps the locked phase until 'need' samples in a row disagree. */
struct phase_lock {
	int phase;		/* -1 = not locked yet */
	int candidate;
	int run;		/* consecutive samples equal to 'candidate' */
};

static inline void phase_lock_init(struct phase_lock *pl, int fixed_phase)
{
	pl->phase = fixed_phase;
	pl->candidate = -1;
	pl->run = 0;
}

/* Returns 1 when the locked phase changed (first lock or re-lock). */
static inline int phase_lock_feed(struct phase_lock *pl, int sample, int need)
{
	if (sample == pl->phase) {
		pl->run = 0;
		pl->candidate = -1;
		return 0;
	}
	if (sample == pl->candidate)
		pl->run++;
	else {
		pl->candidate = sample;
		pl->run = 1;
	}
	if (pl->run >= need) {
		pl->phase = sample;
		pl->run = 0;
		pl->candidate = -1;
		return 1;
	}
	return 0;
}

#endif
