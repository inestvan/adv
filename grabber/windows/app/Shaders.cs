// The picture processing of the web page (grabber/www/index.html), ported from WebGL2 (GLSL ES 3.00) to desktop
// OpenGL 3.3 (GLSL 3.30) almost word for word - the algorithms and their tuning are the page's, see the comments
// there. Changes for a better picture than the page can give:
//  * chroma: UYVY carries Cb/Cr once per pixel pair, co-sited with the even pixel (BT.601); the page repeats it on
//    the odd pixel, here the odd pixel gets the mean of its pair and the next one (smoother, truer colour edges);
//  * all intermediate pictures are RGBA16F (the page: 8 bit), so the levels stretch, gamma and scaling add no banding;
//  * the Lanczos scaler has anti-ringing (the result is pulled into the range of the two nearest source pixels, as
//    in mpv / madVR): no light/dark halos around the OSD letters and other hard edges;
//  * the last pass dithers to the 8-bit screen (triangular noise of +-1 step): smooth gradients, no contour lines.

static class Shaders
{
	public const string VS = @"#version 330 core
void main() { vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0); gl_Position = vec4(p, 0.0, 1.0); }";

	// pass 1: deinterlace + colour into a texture at the source size (the page's FS)
	public const string FS_MAIN = @"#version 330 core
precision highp float; precision highp int;
uniform sampler2D uTop, uBot, uTopP, uBotP;	// latest top/bottom field (cleaned), and the one before each (P)
uniform sampler2D uTopN, uBotN;			// the same latest fields after the recursive noise reduction
uniform int uMode, uCur, uView, uLines, uH, uW, uX0, uY0, uTNR, uChroma;
uniform float uCold, uPeak, uCore0, uCore1, uMotGain;
uniform vec2 uLev;	// auto levels: Y' = 16 + (Y - uLev.x) * uLev.y; (16, 1) = off
out vec4 o;
vec4 tex(int f, bool prev, ivec2 c) {
	return (f == 0 ? (prev ? texelFetch(uTopP, c, 0) : texelFetch(uTop, c, 0))
	               : (prev ? texelFetch(uBotP, c, 0) : texelFetch(uBot, c, 0))) * 255.0;
}
vec4 texN(int f, ivec2 c) { return (f == 0 ? texelFetch(uTopN, c, 0) : texelFetch(uBotN, c, 0)) * 255.0; }
// one pixel as (Y, U, V): the chroma of an odd pixel is interpolated between its pair and the next one
vec3 yuvAt(vec4 t, vec4 tn, bool odd) {
	if (!odd) return vec3(t.g, t.r, t.b);
	return uChroma == 1 ? vec3(t.a, 0.5 * (t.r + tn.r), 0.5 * (t.b + tn.b)) : vec3(t.a, t.r, t.b);
}
float fetchY(int f, bool prev, int x, int row) {
	row = clamp(row, 0, uLines - 1); x = clamp(x, 0, uW - 1);
	vec4 t = tex(f, prev, ivec2(x >> 1, row));
	return (x & 1) == 1 ? t.a : t.g;
}
vec3 fetch(int f, int x, int row) {
	row = clamp(row, 0, uLines - 1); x = clamp(x, 0, uW - 1);
	ivec2 c = ivec2(x >> 1, row);
	bool odd = (x & 1) == 1;
	vec4 t = tex(f, false, c);
	vec4 tn = odd ? tex(f, false, ivec2(min(c.x + 1, (uW >> 1) - 1), row)) : t;
	return yuvAt(t, tn, odd);
}
// the picture values: the noise-reduced field (recursive NR), or the field as cleaned
vec3 src(int f, int x, int row) {
	if (uTNR == 0) return fetch(f, x, row);
	row = clamp(row, 0, uLines - 1); x = clamp(x, 0, uW - 1);
	ivec2 c = ivec2(x >> 1, row);
	bool odd = (x & 1) == 1;
	vec4 t = texN(f, c);
	vec4 tn = odd ? texN(f, ivec2(min(c.x + 1, (uW >> 1) - 1), row)) : t;
	return yuvAt(t, tn, odd);
}
float srcY(int f, int x, int row) {
	row = clamp(row, 0, uLines - 1); x = clamp(x, 0, uW - 1);
	vec4 t = uTNR == 0 ? tex(f, false, ivec2(x >> 1, row)) : texN(f, ivec2(x >> 1, row));
	return (x & 1) == 1 ? t.a : t.g;
}
// page peaking (contours), after the noise reduction: band-pass (2Y[x] - Y[x-2] - Y[x+2]) / 4, cored
vec3 val(int f, int x, int row) {
	vec3 c = src(f, x, row);
	if (uPeak > 0.0) {
		float d = 0.25 * (2.0 * c.x - srcY(f, x - 2, row) - srcY(f, x + 2, row));
		c.x += uPeak * d * smoothstep(uCore0, uCore1, abs(d));
	}
	return c;
}
float dY(int f, int x, int row) { return fetchY(f, false, x, row) - fetchY(f, true, x, row); }	// change over a frame
// motion at a missing line: the mean SIGNED change over 5 px x 3 field lines of the opposite field and of the current
// field around it, plus any single pixel changing far beyond the noise (30) - see the page
float motion(int f, int op, int x, int r, int ra) {
	float sc = 0.0, sa = 0.0, sb = 0.0;
	for (int dy = -1; dy <= 1; dy++)
		for (int dx = -2; dx <= 2; dx++) sc += dY(op, x + dx, r + dy);
	for (int dy = -1; dy <= 2; dy++) {
		float row = 0.0;
		for (int dx = -2; dx <= 2; dx++) row += dY(f, x + dx, ra + dy);
		if (dy <= 1) sa += row;
		if (dy >= 0) sb += row;
	}
	float m = max(abs(sc), 0.5 * (abs(sa) + abs(sb))) / 15.0;
	float px = max(abs(dY(op, x, r)), 0.5 * (abs(dY(f, x, ra)) + abs(dY(f, x, ra + 1))));
	return max(m, px - 30.0);
}
void main() {
	int x = int(gl_FragCoord.x) + uX0, y = uH - 1 - int(gl_FragCoord.y) + uY0;
	vec3 yuv;
	if (uMode == 3) {
		int f = uCur, op = 1 - f, r = y >> 1;
		if ((y & 1) == f) yuv = val(f, x, r);
		else {
			int ra = f == 0 ? r : r - 1, rb = ra + 1;
			vec3 sp = 0.5 * (val(f, x, ra) + val(f, x, rb));
			vec3 wv = val(op, x, r);
			float de = max(motion(f, op, x, r, ra) * uMotGain - 4.0, 0.0) * 4.0;
			yuv = clamp(sp, wv - de, wv + de);
		}
	} else if (uMode == 1) {
		yuv = val(y & 1, x, y >> 1);
	} else {
		int f = uCur;
		if ((y & 1) == f) yuv = val(f, x, y >> 1);
		else {
			int r0 = f == 0 ? (y >> 1) : (y >> 1) - 1;
			yuv = uMode == 2 ? val(f, x, f == 0 ? r0 : r0 + 1) : 0.5 * (val(f, x, r0) + val(f, x, r0 + 1));
		}
	}
	float Y = uView == 2 ? yuv.x : 16.0 + (yuv.x - uLev.x) * uLev.y, u = yuv.y - 128.0, v = yuv.z - 128.0;
	vec3 rgb;
	if (uView == 0 || uView == 3) {
		float yc = (Y - 16.0) * 1.164383;
		rgb = vec3(yc + 1.596027 * v, yc - 0.391762 * u - 0.812968 * v, yc + 2.017232 * u) / 255.0;
		if (uView == 3) {	// AV monitor look: gamma 1.1 and colder colours (the page's fit to photos of the monitor)
			rgb = pow(clamp(rgb, 0.0, 1.0), vec3(1.1)) * 255.0;
			float yd = dot(rgb, vec3(0.299, 0.587, 0.114));
			vec2 cc = vec2(0.564 * (rgb.b - yd), 0.713 * (rgb.r - yd));
			mat2 m = mat2(1.0) + uCold * (mat2(1.192, -0.228, 0.131, 0.859) - mat2(1.0));
			cc = m * cc + uCold * vec2(20.0, -13.1);
			float r = yd + cc.y / 0.713, b = yd + cc.x / 0.564;
			rgb = vec3(r, (yd - 0.299 * r - 0.114 * b) / 0.587, b) / 255.0;
		}
	} else {
		float g = clamp((Y - 16.0) / 219.0, 0.0, 1.0);
		rgb = vec3(g);
		if (uView == 2) {
			if (Y <= 16.5) rgb = vec3(0.1, 0.35, 1.0);
			else if (Y >= 234.5) rgb = vec3(1.0, 0.15, 0.1);
		}
	}
	o = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}";

	// field cleaning, once per arriving field: 3D comb (the page's FS_CLEAN)
	public const string FS_CLEAN = @"#version 330 core
precision highp float; precision highp int;
uniform sampler2D uRaw, uOld;	// raw field n, and the same-parity field two frames earlier
uniform int uComb;		// 0 copy, 1 3D comb, 2 3-pixel mean only (no valid history)
out vec4 o;
float L(int w, int x, int row) {
	ivec2 sz = textureSize(uRaw, 0);
	x = clamp(x, 0, sz.x * 2 - 1); row = clamp(row, 0, sz.y - 1);
	vec4 v = (w == 0 ? texelFetch(uRaw, ivec2(x >> 1, row), 0) : texelFetch(uOld, ivec2(x >> 1, row), 0)) * 255.0;
	return (x & 1) == 1 ? v.a : v.g;
}
float B(int w, int x, int row) { return (L(w, x - 1, row) + L(w, x, row) + L(w, x + 1, row)) / 3.0; }
float cleanY(int x, int row) {
	float yn = L(0, x, row);
	if (uComb == 0) return yn;
	float bn = B(0, x, row);
	if (uComb == 2) return bn;
	float s = 0.0;
	for (int dy = -1; dy <= 1; dy++)
		for (int k = -5; k <= 5; k++) {
			int a = k < 0 ? -k : k;
			s += float(a == 5 ? 1 : a == 4 ? 2 : 3) * (L(0, x + k, row + dy) - L(1, x + k, row + dy));
		}
	float m = max(abs(s) / 81.0, abs(bn - B(1, x, row)) - 20.0);
	return mix(0.5 * (yn + L(1, x, row)), bn, smoothstep(3.0, 6.0, m));
}
void main() {
	ivec2 c = ivec2(gl_FragCoord.xy);
	vec4 v = texelFetch(uRaw, c, 0);
	o = vec4(v.r, cleanY(c.x * 2, c.y) / 255.0, v.b, cleanY(c.x * 2 + 1, c.y) / 255.0);
}";

	// recursive motion-adaptive temporal noise reduction (the page's FS_TNR)
	public const string FS_TNR = @"#version 330 core
precision highp float; precision highp int;
uniform sampler2D uCur, uAcc;
uniform int uReset;
uniform float uKmin, uKminC, uT0, uT1, uTpx, uCw;
out vec4 o;
vec4 T(sampler2D s, int x, int y) {
	ivec2 sz = textureSize(s, 0);
	return texelFetch(s, ivec2(clamp(x, 0, sz.x - 1), clamp(y, 0, sz.y - 1)), 0) * 255.0;
}
void main() {
	ivec2 c = ivec2(gl_FragCoord.xy);
	vec4 cur = T(uCur, c.x, c.y), acc = T(uAcc, c.x, c.y);
	if (uReset == 1) { o = cur / 255.0; return; }
	float d[18];
	vec2 dc = vec2(0.0);
	vec2 cl = vec2(0.0), cr = vec2(0.0);
	for (int dy = -1; dy <= 1; dy++)
		for (int i = -1; i <= 1; i++) {
			vec4 n = T(uCur, c.x + i, c.y + dy), e = n - T(uAcc, c.x + i, c.y + dy);
			int j = 6 * (dy + 1) + 2 * (i + 1);
			d[j] = e.g; d[j + 1] = e.a;
			if (i == 0) dc += e.rb;
			if (dy == 0 && i == -1) cl = n.rb;
			if (dy == 0 && i == 1) cr = n.rb;
		}
	float mc = uCw * max(abs(dc.x), abs(dc.y)) / 3.0;
	vec2 a;
	for (int p = 0; p < 2; p++) {
		float s = 0.0;
		for (int l = 0; l < 3; l++)
			for (int k = 0; k < 5; k++) s += d[6 * l + p + k];
		float mp = abs(d[7 + p] + d[8 + p] + d[9 + p]) / 3.0 - uTpx;
		a[p] = smoothstep(uT0, uT1, max(max(abs(s) / 15.0, mp), mc));
	}
	vec2 k = mix(vec2(uKmin), vec2(1.0), a);
	float kc = mix(uKminC, 1.0, max(a.x, a.y));
	vec2 cc = 0.25 * (cl + cr) + 0.5 * cur.rb;
	o = vec4(mix(acc.r, cc.x, kc), mix(acc.g, cur.g, k.x), mix(acc.b, cc.y, kc), mix(acc.a, cur.a, k.y)) / 255.0;
}";

	// along-edge anti-aliasing of near-horizontal edges (the page's FS_AA)
	public const string FS_AA = @"#version 330 core
precision highp float; precision highp int;
uniform sampler2D uSrc;
uniform int uAA;
out vec4 o;
const float K0 = 4.0, K1 = 10.0;
const float C0 = 0.2, C1 = 0.45;
const float SMAX = 0.6;
const int N = 8; const float SIG = 4.0;
float lum(ivec2 p, ivec2 sz) {
	return dot(texelFetch(uSrc, clamp(p, ivec2(0), sz - 1), 0).rgb, vec3(0.299, 0.587, 0.114)) * 219.0;
}
void main() {
	ivec2 p = ivec2(gl_FragCoord.xy), sz = textureSize(uSrc, 0);
	vec4 c0 = texelFetch(uSrc, p, 0);
	if (uAA == 0) { o = c0; return; }
	float L[45];
	for (int j = 0; j < 5; j++)
		for (int i = 0; i < 9; i++) L[j * 9 + i] = lum(p + ivec2(i - 4, j - 2), sz);
	float xx = 0.0, xy = 0.0, yy = 0.0;
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 8; i++) {
			int k = j * 9 + i;
			float gx = 0.5 * (L[k + 1] - L[k] + L[k + 10] - L[k + 9]);
			float gy = 0.5 * (L[k + 9] - L[k] + L[k + 10] - L[k + 1]);
			xx += gx * gx; xy += gx * gy; yy += gy * gy;
		}
	xx /= 32.0; xy /= 32.0; yy /= 32.0;
	float tr = xx + yy, dt = sqrt((xx - yy) * (xx - yy) + 4.0 * xy * xy);
	float coh = dt / max(tr, 1e-6);
	float vy = 0.5 * (tr + dt) - xx;
	float s = -xy / max(vy, 1e-6);
	float w = smoothstep(K0, K1, sqrt(tr)) * smoothstep(C0, C1, coh)
	        * (yy > xx ? 1.0 - smoothstep(0.75 * SMAX, SMAX, abs(s)) : 0.0);
	if (w <= 0.0) { o = c0; return; }
	s = clamp(s, -SMAX, SMAX);
	vec2 isz = 1.0 / vec2(sz);
	vec3 acc = vec3(0.0); float ws = 0.0;
	for (int t = -N; t <= N; t++) {
		float ft = float(t), g = exp(-0.5 * ft * ft / (SIG * SIG));
		acc += g * textureLod(uSrc, (vec2(p) + 0.5 + vec2(ft, s * ft)) * isz, 0.0).rgb;
		ws += g;
	}
	o = vec4(mix(c0.rgb, acc / ws, w), 1.0);
}";

	// scale to the screen pixels, separable Lanczos-3 (horizontal pass, then vertical), with anti-ringing; the last
	// pass dithers to 8 bits
	public const string FS_SCALE = @"#version 330 core
precision highp float; precision highp int;
uniform sampler2D uSrc;
uniform int uAxis, uSharp, uDither;
uniform float uAR;		// anti-ringing strength 0..1
uniform vec4 uRect;		// where this pass draws, in pixels of its target: x, y, width, height
out vec4 o;
float lanczos3(float x) {
	x = abs(x);
	if (x < 1e-4) return 1.0;
	if (x >= 3.0) return 0.0;
	float p = 3.14159265 * x;
	return 3.0 * sin(p) * sin(p / 3.0) / (p * p);
}
// triangular-pdf noise of +-1 step of an 8-bit screen, from the pixel position (static, white). Hash without sine
// (D. Hoskins): float only and precise at screen coordinates (a sin() hash loses precision at large coordinates, an
// integer hash next to the Lanczos loop hung the Intel UHD driver, interleaved gradient noise is periodic).
vec3 hash32(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yxz + 33.33);
	return fract((p3.xxy + p3.yzz) * p3.zyx);
}
vec3 dither(vec2 xy) { return (hash32(xy) + hash32(xy + vec2(1913.0, 717.0)) - 1.0) / 255.0; }
void main() {
	vec2 q = gl_FragCoord.xy - uRect.xy;
	vec3 res;
	if (uSharp == 0) {
		res = texture(uSrc, q / uRect.zw).rgb;	// plain bilinear
	} else {
		ivec2 sz = textureSize(uSrc, 0);
		bool hz = uAxis == 0;
		float n = float(hz ? sz.x : sz.y), outN = hz ? uRect.z : uRect.w;
		float s = (hz ? q.x : q.y) * n / outN - 0.5;
		float k = min(outN / n, 1.0);
		int other = int(hz ? q.y : q.x), lim = (hz ? sz.x : sz.y) - 1, c = int(floor(s));
		vec4 acc = vec4(0.0);
		float ws = 0.0;
		int r = int(ceil(3.0 / k)) + 1;	// the kernel is 3 / k source pixels wide (wider when shrinking)
		for (int i = -r; i <= r; i++) {
			int j = c + i;
			float w = lanczos3((s - float(j)) * k);
			if (w == 0.0) continue;
			int jj = clamp(j, 0, lim);
			acc += w * texelFetch(uSrc, hz ? ivec2(jj, other) : ivec2(other, jj), 0);
			ws += w;
		}
		res = acc.rgb / ws;
		if (uAR > 0.0 && k >= 1.0) {	// enlarging: keep the result within the two nearest source pixels
			int j0 = clamp(c, 0, lim), j1 = clamp(c + 1, 0, lim);
			vec3 a = texelFetch(uSrc, hz ? ivec2(j0, other) : ivec2(other, j0), 0).rgb;
			vec3 b = texelFetch(uSrc, hz ? ivec2(j1, other) : ivec2(other, j1), 0).rgb;
			res = mix(res, clamp(res, min(a, b), max(a, b)), uAR);
		}
	}
	res = clamp(res, 0.0, 1.0);
	if (uDither == 1) res = clamp(res + dither(gl_FragCoord.xy), 0.0, 1.0);
	o = vec4(res, 1.0);
}";
}
