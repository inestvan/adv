"""2D electrostatic field solver for (coupled) microstrip on the JLC04161H-7628 outer layer.
Finite-volume Laplace on a uniform grid, potentials on nodes, permittivity on cells; C from field energy.
Z0 = 1 / (c * sqrt(C * C_air)).  Units: mm."""
import numpy as np, scipy.sparse as sp, scipy.sparse.linalg as spl, math, sys

C0 = 299792458.0
EPS0 = 8.854187817e-12

def solve(traces, h=0.2104, er=4.4, t=0.035, mask=0.015, er_mask=3.8, gnd_gap=None, d=0.005, W=7.0, H=2.5, use_dielectric=True):
    """traces: list of (x_left, width, volt). gnd_gap: coplanar ground pour this far from the outer traces (None = no pour).
    Returns field energy per metre (J/m) for 1 V scale."""
    nx, ny = int(round(W / d)) + 1, int(round(H / d)) + 1
    xs = np.arange(nx) * d - W / 2
    ys = np.arange(ny) * d
    # cell permittivity (cells between nodes): (nx-1) x (ny-1)
    xc = (xs[:-1] + xs[1:]) / 2; yc = (ys[:-1] + ys[1:]) / 2
    XC, YC = np.meshgrid(xc, yc, indexing='ij')
    eps = np.ones_like(XC)
    # conductor masks (on nodes)
    X, Y = np.meshgrid(xs, ys, indexing='ij')
    fixed = np.zeros(X.shape, bool); val = np.zeros(X.shape)
    fixed[:, 0] = True                                      # In1 ground plane at y = 0
    fixed[:, -1] = True; fixed[0, :] = True; fixed[-1, :] = True  # far grounded box
    cond_cells = np.zeros(XC.shape, bool)
    for (x0, w, v) in traces:
        m = (X >= x0 - 1e-9) & (X <= x0 + w + 1e-9) & (Y >= h - 1e-9) & (Y <= h + t + 1e-9)
        fixed |= m; val[m] = v
        cond_cells |= (XC > x0) & (XC < x0 + w) & (YC > h) & (YC < h + t)
    if gnd_gap is not None:
        xl = min(x0 for x0, w, v in traces) - gnd_gap; xr = max(x0 + w for x0, w, v in traces) + gnd_gap
        m = ((X <= xl + 1e-9) | (X >= xr - 1e-9)) & (Y >= h - 1e-9) & (Y <= h + t + 1e-9)
        fixed |= m; val[m] = 0.0
        cond_cells |= ((XC < xl) | (XC > xr)) & (YC > h) & (YC < h + t)
    if use_dielectric:
        eps[YC < h] = er
        if mask > 0:
            # conformal solder mask: within `mask` of the dielectric surface or of any copper on top
            near = (YC >= h) & (YC < h + mask)
            cu = np.zeros_like(near)
            for (x0, w, v) in traces:
                cu |= (XC > x0 - mask) & (XC < x0 + w + mask) & (YC >= h) & (YC < h + t + mask)
            if gnd_gap is not None:
                cu |= ((XC < xl + mask) | (XC > xr - mask)) & (YC >= h) & (YC < h + t + mask)
            eps[(near | cu) & ~cond_cells] = er_mask
    # link coefficients: x-links between (i,j)-(i+1,j): average of cells (i,j-1),(i,j)
    def pad(a): return np.pad(a, ((0, 0), (1, 1)), mode='edge')
    ep = pad(eps)                                           # (nx-1, ny+1)
    kx = (ep[:, :-1] + ep[:, 1:]) / 2                       # (nx-1, ny)
    ep2 = np.pad(eps, ((1, 1), (0, 0)), mode='edge')        # (nx+1, ny-1)
    ky = (ep2[:-1, :] + ep2[1:, :]) / 2                     # (nx, ny-1)
    N = nx * ny
    idx = np.arange(N).reshape(nx, ny)
    rows, cols, data = [], [], []
    diag = np.zeros(N)
    def add_link(a, b, k):
        nonlocal rows, cols, data
        rows.append(a); cols.append(b); data.append(-k)
        rows.append(b); cols.append(a); data.append(-k)
        np.add.at(diag, a, k); np.add.at(diag, b, k)
    add_link(idx[:-1, :].ravel(), idx[1:, :].ravel(), kx.ravel())
    add_link(idx[:, :-1].ravel(), idx[:, 1:].ravel(), ky.ravel())
    A = sp.csr_matrix((np.concatenate(data), (np.concatenate(rows), np.concatenate(cols))), shape=(N, N)) + sp.diags(diag)
    f = fixed.ravel(); u = ~f
    phi = val.ravel().copy()
    b = -A[u][:, f] @ phi[f]
    phi[u] = spl.spsolve(A[u][:, u].tocsc(), b)
    P = phi.reshape(nx, ny)
    Wx = 0.5 * np.sum(kx * (P[1:, :] - P[:-1, :]) ** 2)
    Wy = 0.5 * np.sum(ky * (P[:, 1:] - P[:, :-1]) ** 2)
    return EPS0 * (Wx + Wy)                                 # J/m per (V^2), d cancels in 2D

def z_single(w, **kw):
    tr = [(-w / 2, w, 1.0)]
    C = 2 * solve(tr, **kw); Ca = 2 * solve(tr, use_dielectric=False, **kw)
    z = 1 / (C0 * math.sqrt(C * Ca)); eeff = C / Ca
    return z, eeff

def z_pair(w, s, **kw):
    """returns Zodd, Zeven, Zdiff, Zcm, eeff_odd, eeff_even"""
    out = {}
    for mode, v2 in (('odd', -1.0), ('even', 1.0)):
        tr = [(-s / 2 - w, w, 1.0), (s / 2, w, v2)]
        Wd = solve(tr, **kw); Wa = solve(tr, use_dielectric=False, **kw)
        C, Ca = Wd, Wa                                      # per line: W_total = C_mode * V^2 (two lines, 1/2 C V^2 each)
        out[mode] = (1 / (C0 * math.sqrt(C * Ca)), C / Ca)
    zo, eo = out['odd']; ze, ee = out['even']
    return zo, ze, 2 * zo, ze / 2, eo, ee

def hammerstad(w, h=0.2104, er=4.4, t=0.035):
    """Hammerstad-Jensen microstrip with thickness correction (no mask), for cross-checking the solver."""
    u = w / h
    du1 = t / (math.pi * h) * math.log(1 + 4 * math.e / (t / h * (1 / math.tanh(math.sqrt(6.517 * u))) ** 2))
    dur = 0.5 * (1 + 1 / math.cosh(math.sqrt(er - 1))) * du1
    def z0air(u):
        f = 6 + (2 * math.pi - 6) * math.exp(-(30.666 / u) ** 0.7528)
        return 60 * math.log(f / u + math.sqrt(1 + (2 / u) ** 2))
    def eeff(u):
        a = 1 + math.log((u ** 4 + (u / 52) ** 2) / (u ** 4 + 0.432)) / 49 + math.log(1 + (u / 18.1) ** 3) / 18.7
        b = 0.564 * ((er - 0.9) / (er + 3)) ** 0.053
        return (er + 1) / 2 + (er - 1) / 2 * (1 + 10 / u) ** (-a * b)
    u1, ur = u + du1, u + dur
    return z0air(ur) / math.sqrt(eeff(ur)), eeff(ur) * (z0air(u1) / z0air(ur)) ** 2

def rich(fn, *a, **kw):
    """Richardson extrapolation over grid 5 um / 2.5 um (first-order convergence observed)."""
    kw.setdefault('W', 5.0); kw.setdefault('H', 1.5)
    r1 = fn(*a, d=0.005, **kw); r2 = fn(*a, d=0.0025, **kw)
    return tuple(2 * b - a_ for a_, b in zip(r1, r2))
