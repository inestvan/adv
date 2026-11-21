/* Host test for weave.h (no Linux headers needed): gcc -O2 -Wall -o test_weave test_weave.c && ./test_weave
 * Simulates the decoder: a known YUYV frame is split into odd/even UYVY fields exactly as a PAL / NTSC source
 * would send them, the field counter starts at an arbitrary phase, the parity is learnt from simulated EVEN_FIELD
 * samples, and the woven output must be bit-identical to the original frame. */
#include <stdio.h>
#include <stdlib.h>
#include "weave.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_swap(void)
{
	uint8_t uyvy[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };	/* U0 Y0 V0 Y1 | U1 Y2 V1 Y3 */
	uint8_t out[8] = { 0 };
	uyvy_to_yuyv_line(out, uyvy, 4);
	const uint8_t want[8] = { 0x22, 0x11, 0x44, 0x33, 0x66, 0x55, 0x88, 0x77 };	/* Y0 U0 Y1 V0 ... */
	for (int i = 0; i < 8; i++)
		CHECK(out[i] == want[i], "swap byte %d: %02x != %02x", i, out[i], want[i]);
}

static void test_black(void)
{
	uint8_t f[8];
	fill_black_yuyv(f, sizeof f);
	CHECK(f[0] == 0x10 && f[1] == 0x80 && f[6] == 0x10 && f[7] == 0x80, "black");
}

static void test_lock(void)
{
	struct phase_lock pl;
	phase_lock_init(&pl, -1);
	int changed = 0;
	for (int i = 0; i < 4; i++)
		changed |= phase_lock_feed(&pl, 1, 5);
	CHECK(!changed && pl.phase == -1, "locked too early");
	CHECK(phase_lock_feed(&pl, 1, 5) && pl.phase == 1, "no lock after 5 samples");
	/* noise: 4 contrary samples then one agreeing keeps the lock */
	for (int i = 0; i < 4; i++)
		phase_lock_feed(&pl, 0, 5);
	phase_lock_feed(&pl, 1, 5);
	for (int i = 0; i < 4; i++)
		phase_lock_feed(&pl, 0, 5);
	CHECK(pl.phase == 1, "lock lost on noise");
	CHECK(phase_lock_feed(&pl, 0, 5) && pl.phase == 0, "no re-lock after 5 contrary samples");
}

/* full pipeline for one standard and one starting counter value */
static void test_pipeline(int ntsc, uint32_t seq0, int field1_first_seq_parity)
{
	const unsigned W = 720, H = ntsc ? 480 : 576, FH = H / 2, BPL = W * 2 + 64;	/* padded stride on purpose */
	uint8_t *orig = malloc((size_t)W * H * 2), *out = malloc((size_t)W * H * 2);
	uint8_t *fld[2];
	fld[0] = malloc((size_t)BPL * FH);
	fld[1] = malloc((size_t)BPL * FH);
	srand(seq0 * 7 + (unsigned)ntsc);
	for (size_t i = 0; i < (size_t)W * H * 2; i++)
		orig[i] = (uint8_t)rand();

	/* the source: field 1 (odd) = top rows for PAL, bottom rows for NTSC; field 1 is sent first */
	for (int f = 0; f < 2; f++) {			/* f 0 = field 1 (odd), f 1 = field 2 (even) */
		int top = ntsc ? (f == 1) : (f == 0);
		for (unsigned i = 0; i < FH; i++) {
			const uint8_t *src = orig + (size_t)(2 * i + (top ? 0 : 1)) * W * 2;
			uint8_t *dst = fld[f] + (size_t)i * BPL;
			for (unsigned j = 0; j < W * 2; j += 4) {	/* YUYV -> UYVY as the decoder delivers */
				dst[j] = src[j + 1];
				dst[j + 1] = src[j];
				dst[j + 2] = src[j + 3];
				dst[j + 3] = src[j + 2];
			}
		}
	}
	/* sequence numbers: the field with number s is odd iff (s & 1) == field1_first_seq_parity */
	struct phase_lock pl;
	phase_lock_init(&pl, -1);
	for (uint32_t s = seq0; pl.phase < 0; s++) {
		int odd = (int)(s & 1) == field1_first_seq_parity;
		phase_lock_feed(&pl, phase_from_sample(s, !odd), 5);	/* EVEN_FIELD = !odd */
	}
	CHECK(pl.phase == field1_first_seq_parity, "phase %d, expected %d", pl.phase, field1_first_seq_parity);

	/* run 4 fields starting at an arbitrary counter value; weave whenever an odd field is followed by its even */
	int frames = 0, cur = 0;
	uint32_t cur_seq = 0;
	for (uint32_t s = seq0; s < seq0 + 5; s++) {
		int odd = field_is_odd(s, pl.phase);
		const uint8_t *data = fld[odd ? 0 : 1];
		int top = field_is_top(odd, ntsc, 0);
		if (odd) {
			fill_black_yuyv(out, (size_t)W * H * 2);
			weave_field(out, W, H, data, W, FH, BPL, top);
			cur = 1;
			cur_seq = s;
		} else if (cur && s == cur_seq + 1) {
			weave_field(out, W, H, data, W, FH, BPL, top);
			frames++;
			cur = 0;
			size_t bad = 0;
			for (size_t i = 0; i < (size_t)W * H * 2; i++)
				bad += out[i] != orig[i];
			CHECK(bad == 0, "%s seq0 %u: %zu bytes differ", ntsc ? "NTSC" : "PAL", seq0, bad);
		}
	}
	CHECK(frames >= 2, "only %d frames from 5 fields", frames);

	/* --swap must produce the line-swapped picture (row 0 <-> row 1) */
	weave_field(out, W, H, fld[0], W, FH, BPL, field_is_top(1, ntsc, 1));
	weave_field(out, W, H, fld[1], W, FH, BPL, field_is_top(0, ntsc, 1));
	CHECK(memcmp(out, orig + W * 2, W * 2) == 0 && memcmp(out + W * 2, orig, W * 2) == 0, "swap rows");

	free(orig); free(out); free(fld[0]); free(fld[1]);
}

/* NV12: luma bit-exact, chroma = average of two lines of the same field, chroma rows interleaved by field */
static void test_nv12(int ntsc)
{
	const unsigned W = 720, H = ntsc ? 480 : 576, FH = H / 2, BPL = W * 2 + 32;
	uint8_t *orig = malloc((size_t)W * H * 2), *out = malloc((size_t)W * H * 3 / 2);
	uint8_t *fld[2] = { malloc((size_t)BPL * FH), malloc((size_t)BPL * FH) };	/* [0] top rows, [1] bottom rows */
	srand(77 + (unsigned)ntsc);
	for (size_t i = 0; i < (size_t)W * H * 2; i++)
		orig[i] = (uint8_t)rand();
	for (int p = 0; p < 2; p++)
		for (unsigned i = 0; i < FH; i++) {
			const uint8_t *src = orig + (size_t)(2 * i + p) * W * 2;
			uint8_t *dst = fld[p] + (size_t)i * BPL;
			for (unsigned j = 0; j < W * 2; j += 4) {
				dst[j] = src[j + 1];
				dst[j + 1] = src[j];
				dst[j + 2] = src[j + 3];
				dst[j + 3] = src[j + 2];
			}
		}
	fill_black_nv12(out, W, H);
	weave_field_nv12(out, W, H, fld[1], W, FH, BPL, 0);	/* order of the two calls does not matter */
	weave_field_nv12(out, W, H, fld[0], W, FH, BPL, 1);
	size_t bady = 0, baduv = 0;
	for (unsigned r = 0; r < H; r++)
		for (unsigned x = 0; x < W; x++)
			bady += out[(size_t)r * W + x] != orig[((size_t)r * W + x) * 2];
	const uint8_t *uv = out + (size_t)W * H;
	for (unsigned c = 0; c < H / 2; c++) {
		unsigned p = c & 1, j = c >> 1, r0 = 4 * j + p, r1 = 4 * j + 2 + p;	/* frame rows of field lines 2j, 2j+1 */
		for (unsigned k = 0; k < W / 2; k++) {
			unsigned u = (orig[((size_t)r0 * W + 2 * k) * 2 + 1] + orig[((size_t)r1 * W + 2 * k) * 2 + 1] + 1) >> 1;
			unsigned v = (orig[((size_t)r0 * W + 2 * k) * 2 + 3] + orig[((size_t)r1 * W + 2 * k) * 2 + 3] + 1) >> 1;
			baduv += (uv[(size_t)c * W + 2 * k] != u) + (uv[(size_t)c * W + 2 * k + 1] != v);
		}
	}
	CHECK(bady == 0, "%s NV12: %zu luma samples differ", ntsc ? "NTSC" : "PAL", bady);
	CHECK(baduv == 0, "%s NV12: %zu chroma samples differ", ntsc ? "NTSC" : "PAL", baduv);
	free(orig); free(out); free(fld[0]); free(fld[1]);
}

static void test_short_field(void)
{
	/* NTSC fields (240 lines) into a PAL frame: rows past 480 stay black */
	const unsigned W = 8, H = 576;
	uint8_t frame[8 * 576 * 2], field[8 * 2 * 240];
	memset(field, 0x55, sizeof field);
	fill_black_yuyv(frame, sizeof frame);
	weave_field(frame, W, H, field, W, 240, W * 2, 1);
	weave_field(frame, W, H, field, W, 240, W * 2, 0);
	CHECK(frame[479 * W * 2] == 0x55 && frame[480 * W * 2] == 0x10 && frame[575 * W * 2 + 1] == 0x80, "short field");
}

int main(void)
{
	test_swap();
	test_black();
	test_lock();
	for (int ntsc = 0; ntsc < 2; ntsc++)
		for (uint32_t s0 = 0; s0 < 4; s0++)
			for (int p = 0; p < 2; p++)
				test_pipeline(ntsc, s0 + 1000 * (uint32_t)p, p);
	test_short_field();
	test_nv12(0);
	test_nv12(1);
	printf(fails ? "%d FAILED\n" : "all weave tests passed\n", fails);
	return fails != 0;
}
