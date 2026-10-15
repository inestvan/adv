"""Stage 3: add tracks, vias and zones to hw/adv7280m-csi2.kicad_pcb (run after gen_pcb.py)."""
import os, sys, math
import pcbnew
from pcbnew import VECTOR2I, FromMM

HW = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'hw'))
PCB = os.path.join(HW, 'adv7280m-csi2.kicad_pcb')
F, B, IN1, IN2 = pcbnew.F_Cu, pcbnew.B_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu
# W_CVBS 0.13 mm with the F.Cu GND pour 0.4 mm away (hw/adv7280m-csi2.kicad_dru) = 75 R coplanar microstrip (docs/routing-rules.md)
W_MIPI, W_SIG, W_CVBS, W_PWR, W_PWR_S = 0.30, 0.2, 0.13, 0.4, 0.3
# MIPI: loosely coupled pairs 0.30 / 0.45 mm = 100 R diff, 28 R common mode (UG-637 "2 x 50 R"); W_NECK right at the QFN pins,
# W_PIN for the 0.5 mm pitch stretch of the CLK pair under the chip (docs/routing-rules.md)
W_NECK, W_PIN = 0.18, 0.24
VIA, VIA_DRILL = 0.5, 0.3
VIA_S, VIA_S_DRILL = 0.45, 0.2      # MIPI layer-change vias and their GND return vias
BOARD_T = 1.6

def mm(x): return FromMM(x)
def P(x, y): return VECTOR2I(mm(x), mm(y))

class Router:
    def __init__(self, board):
        self.b = board
        self.nets = {n.GetNetname(): n for n in board.GetNetInfo().NetsByName().values()} if hasattr(board.GetNetInfo(), 'NetsByName') else {}
        if not self.nets:
            for i in range(board.GetNetInfo().GetNetCount()):
                n = board.GetNetInfo().GetNetItem(i); self.nets[n.GetNetname()] = n
        self.pads = {}
        for fp in board.GetFootprints():
            for pad in fp.Pads():
                self.pads.setdefault(fp.GetReference() + '.' + pad.GetNumber(), pad)
        self.lengths = {}
    def net(self, name): return self.nets[name]
    def pad(self, ref_pin):
        p = self.pads[ref_pin].GetPosition(); return (p.x / 1e6, p.y / 1e6)
    def track(self, net, pts, w=W_SIG, layer=F):
        for a, b in zip(pts, pts[1:]):
            if a == b: continue
            t = pcbnew.PCB_TRACK(self.b); t.SetStart(P(*a)); t.SetEnd(P(*b)); t.SetWidth(mm(w)); t.SetLayer(layer); t.SetNet(self.net(net)); self.b.Add(t)
            self.lengths[net] = self.lengths.get(net, 0) + math.hypot(b[0] - a[0], b[1] - a[1])
    def via(self, net, xy, d=VIA, drill=VIA_DRILL):
        v = pcbnew.PCB_VIA(self.b); v.SetPosition(P(*xy)); v.SetViaType(pcbnew.VIATYPE_THROUGH); v.SetLayerPair(F, B)
        v.SetWidth(mm(d)); v.SetDrill(mm(drill)); v.SetNet(self.net(net)); self.b.Add(v)
        self.lengths[net] = self.lengths.get(net, 0) + BOARD_T
    def zone(self, net, layer, pts, clearance=0.25, minw=0.25, priority=0, thermal=True):
        z = pcbnew.ZONE(self.b); z.SetLayer(layer); z.SetNet(self.net(net))
        for x, y in pts: z.AppendCorner(P(x, y), -1)
        z.SetLocalClearance(mm(clearance)); z.SetMinThickness(mm(minw)); z.SetAssignedPriority(priority)
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL if thermal else pcbnew.ZONE_CONNECTION_FULL)
        z.SetThermalReliefGap(mm(0.25)); z.SetThermalReliefSpokeWidth(mm(0.3))
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        self.b.Add(z); return z
    def gnd_via(self, pad_ref, via_xy, w=W_SIG):
        self.track('GND', [self.pad(pad_ref), via_xy], w); self.via('GND', via_xy)

def bump(a, b, n, h, t=0.6, c=0.15):
    """Points from a to b (axis-aligned) with one length-tuning bump of height h on side n (unit normal), centred on the segment."""
    L = math.hypot(b[0] - a[0], b[1] - a[1]); u = ((b[0] - a[0]) / L, (b[1] - a[1]) / L)
    s0 = (L - t) / 2
    P = lambda s, k: (round(a[0] + u[0] * s + n[0] * k, 4), round(a[1] + u[1] * s + n[1] * k, 4))
    return [a, P(s0 - c, 0), P(s0, c), P(s0, h - c), P(s0 + c, h), P(s0 + t - c, h), P(s0 + t, h - c), P(s0 + t, c), P(s0 + t + c, 0), b]

NORTH, SOUTH = (0, -1), (0, 1)
D0P_BUMP, CLKP_BUMP = 1.163, 0.957     # length tuning, mm (see the printout: P and N within 0.05 mm)

def mipi_pair(r, p, n, P, N):
    """Route one MIPI pair: P stays on F.Cu except a straight B.Cu hop, N crosses under P on B.Cu.
    Both lines get exactly two vias (symmetric discontinuities, equal via delay)."""
    for net, segs in ((p, P), (n, N)):
        for kind, pts, w in segs:
            if kind == 'F': r.track(net, pts, w)
            elif kind == 'B': r.track(net, pts, w, B)
            else: r.via(net, pts, VIA_S, VIA_S_DRILL)

def route(r):
    U = r.pad
    # ================= MIPI CSI-2 =================
    # Pins U1.9..12 = D0P, D0N, CLKP, CLKN (west->east); the Pi FFC has N above P, so each pair needs one P/N crossover.
    # D0: pair runs west at y 13.35 / 14.1, crossover (N under P) at x 12.1..13.6, turns north at x 8.25 / 9.0 into J1 pins 3 / 2.
    mipi_pair(r, '/CSI_D0P', '/CSI_D0N', [
        ('F', [U('U1.9'), (18.25, 13.2), (18.1, 13.35)], W_NECK),
        ('F', [(18.1, 13.35), (17.3, 13.35)], W_MIPI), ('V', (17.3, 13.35), 0),
        ('B', [(17.3, 13.35), (15.9, 13.35)], W_MIPI), ('V', (15.9, 13.35), 0),
        ('F', [(15.9, 13.35)] + bump((14.35, 13.35), (13.15, 13.35), NORTH, D0P_BUMP) + [(8.65, 13.35), (8.25, 12.95), (8.25, 10.4), (7.85, 10.0), U('J1.3')], W_MIPI),
    ], [
        ('F', [U('U1.10'), (18.75, 13.7), (18.35, 14.1)], W_NECK),
        ('F', [(18.35, 14.1), (13.6, 14.1)], W_MIPI), ('V', (13.6, 14.1), 0),
        ('B', [(13.6, 14.1), (12.1, 12.6)], W_MIPI), ('V', (12.1, 12.6), 0),
        ('F', [(12.1, 12.6), (9.4, 12.6), (9.0, 12.2), (9.0, 9.4), (8.6, 9.0), U('J1.2')], W_MIPI),
    ])
    # CLK: pair goes down between D0 and MVDD at the chip pitch, runs west at y 15.3 / 16.05, crossover at x 10.1..11.6, J1 pins 9 / 8.
    mipi_pair(r, '/CSI_CLKP', '/CSI_CLKN', [
        ('F', [U('U1.11'), (19.25, 13.9)], W_NECK),
        ('F', [(19.25, 13.9), (19.25, 14.9), (18.85, 15.3)], W_PIN),
        ('F', [(18.85, 15.3), (17.7, 15.3)], W_MIPI), ('V', (17.7, 15.3), 0),
        ('B', [(17.7, 15.3), (16.2, 15.3)], W_MIPI), ('V', (16.2, 15.3), 0),
        ('F', [(16.2, 15.3)] + bump((10.1, 15.3), (8.9, 15.3), SOUTH, CLKP_BUMP) + [(8.2, 16.0), U('J1.9')], W_MIPI),
    ], [
        ('F', [U('U1.12'), (19.75, 13.9)], W_NECK),
        ('F', [(19.75, 13.9), (19.75, 15.3), (19.0, 16.05)], W_PIN),
        ('F', [(19.0, 16.05), (11.6, 16.05)], W_MIPI), ('V', (11.6, 16.05), 0),
        ('B', [(11.6, 16.05), (10.1, 14.55)], W_MIPI), ('V', (10.1, 14.55), 0),
        ('F', [(10.1, 14.55), (8.6, 14.55), (8.15, 15.0), U('J1.8')], W_MIPI),
    ])
    # GND return vias (In1 GND <-> In2 GND) within ~1 mm of every MIPI layer change; In2 under all MIPI B.Cu hops is GND (see zones)
    for xy in [(13.0, 14.7), (12.8, 12.0),     # D0N crossover
               (12.2, 16.7), (10.8, 14.0),     # CLKN crossover
               (16.6, 12.65),                  # D0P hop
               (16.95, 14.7)]:                 # CLKP hop
        r.via('GND', xy, VIA_S, VIA_S_DRILL)
    # ---------------- MVDD (U1.13): runs down east of CLKN, caps below the CLK pair ----------------
    r.track('+1V8D', [U('U1.13'), (20.25, 16.35), (20.0, 16.6), U('C18.1')], W_SIG)
    r.track('+1V8D', [(20.0, 16.6), (18.3, 16.6), U('C17.1')], W_SIG)
    r.track('+1V8D', [(18.3, 16.6), (17.8, 16.6), (17.4, 17.0)], W_SIG); r.via('+1V8D', (17.4, 17.0))
    # ---------------- crystal ----------------
    # Y1 turned 90 deg: XTALN (oscillator input) straight down into the top pad; XTALP down x 20.75 past it into the bottom pad
    r.track('Net-(U1-XTALN)', [U('U1.15'), (21.25, 14.3), U('Y1.1'), U('C3.2')], W_SIG)
    r.track('Net-(U1-XTALP)', [U('U1.14'), (20.75, 18.25), (21.25, 18.75), U('Y1.2'), U('C2.2')], W_SIG)
    # ---------------- PVDD ----------------
    # pin 16 -> C22 10 nF (2 mm) -> C21 100 nF -> via; the feed from FB1 / C11 runs on B.Cu around the crystal (east and south)
    r.track('PVDD', [U('U1.16'), (21.75, 13.45), (24.85, 13.45), U('C22.1'), U('C21.1'), (26.25, 13.0)], W_PWR_S); r.via('PVDD', (26.25, 13.0))
    r.track('PVDD', [(26.25, 13.0), (29.2, 13.0), (29.5, 13.3), (29.5, 23.6), (29.2, 23.9), (20.6, 23.9), (20.0, 24.5), (20.0, 24.8)], W_PWR, B)
    # B.Cu feed passes left of the Y1 courtyard (x < 21.66), nothing under the crystal
    r.via('PVDD', (20.0, 24.8))
    r.track('PVDD', [(20.0, 24.8), (19.5, 25.4), U('FB1.2')], W_PWR_S)
    r.track('PVDD', [(20.0, 24.8), (20.4, 25.4), U('C11.1')], W_PWR_S)
    # ---------------- AVDD / VREF / AIN1 ----------------
    r.track('+1V8A', [U('U1.21'), (23.1, 10.25), (23.6, 9.75), (24.42, 9.0), (24.42, 8.1)], W_PWR_S)   # plane enters only at C19 (27.1; 8.2)
    r.track('+1V8A', [U('C19.1'), (27.1, 8.2)], W_PWR_S); r.via('+1V8A', (27.1, 8.2))
    # C19 (100 nF) also tied by track to the AVDD node next to C20 (10 nF, at the pin)
    r.track('+1V8A', [(24.42, 8.1), (24.57, 8.25), (26.15, 8.25), (26.42, 8.52), U('C19.1')], W_PWR_S)
    r.track('Net-(U1-VREFP)', [U('U1.19'), (23.4, 11.25), (23.7, 11.48), U('C4.1')], W_SIG)
    r.track('Net-(U1-VREFN)', [U('U1.20'), (23.4, 10.75), (23.7, 10.52), U('C4.2')], W_SIG)
    r.track('Net-(U1-AIN1)', [U('U1.17'), (24.675, 12.25), U('C1.2')], W_CVBS)
    # ---------------- termination node / CVBS input ----------------
    r.track('Net-(C1-Pad1)', [U('C1.1'), U('R1.2'), U('R2.1')], W_CVBS)
    r.track('Net-(C1-Pad1)', [U('R2.1'), U('TP7.1')], W_CVBS)
    # CVBS_IN: straight down from the decoder input node to buffer CH1 (R9)
    r.track('/CVBS_IN', [U('R1.1'), (32.1, 11.6), (32.4, 11.9), (32.4, 37.5), (29.5, 40.4), U('R9.2')], W_CVBS)
    # ---------------- DVDDIO / DVDD ----------------
    r.track('+3V3', [U('R4.1'), (16.3, 3.9)], W_PWR_S); r.via('+3V3', (16.3, 3.9))
    r.track('+3V3', [U('R4.1'), (15.35, 5.6), (15.35, 7.3), U('C13.1'), U('C14.1'), (16.6, 9.25), U('U1.2')], W_PWR_S)
    r.track('+1V8D', [U('U1.3'), (16.9, 9.75), (16.3, 10.35), U('C16.1'), U('C15.1'), (16.6, 11.8)], W_PWR_S); r.via('+1V8D', (16.6, 11.8))   # via kept above the MIPI corridor
    # ---------------- control ----------------
    r.track('/PWRDWN', [U('R4.2'), (17.1, 6.325), (18.25, 7.475), U('U1.32')], W_SIG)
    r.track('/PWRDWN', [(17.1, 6.325), (17.1, 3.1), (14.55, 3.1), U('JP3.1')], W_SIG)
    r.track('/ALSB', [U('U1.29'), (19.75, 3.5), (20.25, 3.0), U('JP1.2')], W_SIG)
    r.track('/ALSB', [U('R5.2'), (21.5, 6.0), (19.75, 6.0)], W_SIG)
    r.track('/~{RESET}', [U('R3.2'), (22.5, 7.1), (20.7, 7.1), (20.25, 7.55), U('U1.28')], W_SIG)
    r.track('/~{RESET}', [U('R3.2'), (24.0, 6.325), (24.0, 5.2), (24.4, 4.8), U('C5.1'), (25.25, 4.2), U('JP2.1'), (27.45, 2.7), U('TP5.1')], W_SIG)
    for ref, x in [('R5', 21.8), ('R3', 23.3), ('R6', 26.3), ('R7', 27.8)]:
        r.track('+3V3', [U(ref + '.1'), (x, 3.9)], W_PWR_S); r.via('+3V3', (x, 3.9))
    # I2C: vias next to U1 pads; every B.Cu run lies over one In2 plane (GND), see zones:
    # west legs y 6.5 / 7.4, down at x 14.45 / 15.05 (between the MIPI hops), west at y 20 / 21 to the FFC
    r.track('/SDA', [U('U1.30'), (19.2, 7.6), (19.2, 6.9)], W_SIG); r.via('/SDA', (19.2, 6.9))
    r.track('/SCL', [U('U1.31'), (18.75, 7.4), (18.5, 7.15), (18.5, 6.5)], W_SIG); r.via('/SCL', (18.5, 6.5))
    r.track('/SDA', [(19.2, 6.9), (19.2, 7.4), (15.05, 7.4), (15.05, 21.0), (8.7, 21.0)], W_SIG, B); r.via('/SDA', (8.7, 21.0))
    r.track('/SDA', [(8.7, 21.0), U('J1.14')], W_SIG)
    r.track('/SCL', [(18.5, 6.5), (14.45, 6.5), (14.45, 20.0), (8.7, 20.0)], W_SIG, B); r.via('/SCL', (8.7, 20.0))
    r.track('/SCL', [(8.7, 20.0), U('J1.13')], W_SIG)
    # east legs to the pull-ups R6/R7 and test points TP3/TP4 (nothing under the crystal Y1, B.Cu over In2 GND)
    r.track('/SDA', [(19.2, 6.9), (26.0, 6.9), (26.3, 7.2)], W_SIG, B); r.via('/SDA', (26.3, 7.2))
    r.track('/SDA', [U('R6.2'), (26.3, 7.2), (26.7, 7.6), (29.4, 7.6), U('TP3.1')], W_SIG)
    r.track('/SCL', [(18.5, 6.5), (18.5, 5.4), (28.3, 5.4), (28.6, 5.7), (28.6, 6.3)], W_SIG, B); r.via('/SCL', (28.6, 6.3))
    r.track('/SCL', [U('R7.2'), (28.6, 6.3), U('TP4.1')], W_SIG)
    # CAM_GPIO: jumpers on top edge, B.Cu along the top edge, around the H1 mounting hole (>= 2.5 mm from its centre: standoff/washer)
    # and down the left edge, all over the In2 +3V3 strip/column
    r.via('/CAM_GPIO', (23.95, 1.5)); r.via('/CAM_GPIO', (12.85, 1.5)); r.via('/CAM_GPIO', (7.75, 18.0))
    r.track('/CAM_GPIO', [U('JP2.2'), (23.95, 1.5)], W_SIG); r.track('/CAM_GPIO', [U('JP3.2'), (12.85, 1.5)], W_SIG)
    r.track('/CAM_GPIO', [(23.95, 1.5), (11.3, 1.5), (11.3, 5.4), (10.8, 5.9), (6.9, 5.9), (6.4, 6.4), (6.4, 17.4), (7.0, 18.0), (7.75, 18.0)], W_SIG, B)
    r.track('/CAM_GPIO', [(7.75, 18.0), U('J1.11')], W_SIG)
    # ---------------- 3V3 bottom-left ----------------
    r.track('+3V3', [U('J1.15'), (7.25, 22.0), (7.8, 22.55), (7.8, 22.8)], W_PWR_S); r.via('+3V3', (7.8, 22.8))
    r.track('+3V3', [(7.8, 22.8), (7.8, 23.6), (7.2, 24.3), U('C6.1')], W_PWR_S)
    # TP1 / U2 EN get 3V3 from U2 VIN on F.Cu (no via here: In2 under the I2C legs at y 20..21 stays GND)
    r.track('+3V3', [U('TP1.1'), (11.4, 19.8), (10.6, 20.6), U('U2.1')], W_PWR_S)
    r.track('+3V3', [U('U2.1'), (9.35, 21.05), (9.35, 22.95), U('U2.3')], W_SIG)
    r.via('+3V3', (9.4, 23.5)); r.track('+3V3', [(9.4, 23.5), (9.9, 23.0), U('U2.3')], W_PWR_S)
    r.via('+3V3', (9.4, 26.3)); r.track('+3V3', [(9.4, 26.3), (9.9, 26.8), U('U3.1')], W_PWR_S)
    r.via('+3V3', (9.4, 29.3)); r.track('+3V3', [(9.4, 29.3), (9.9, 28.8), U('U3.3')], W_PWR_S)
    # LDO input caps C29 / C30 (1 uF) straight to the VIN pins, GND through their own vias
    r.track('+3V3', [U('C29.1'), U('U2.3')], W_PWR_S); r.track('+3V3', [U('C30.1'), U('U3.1')], W_PWR_S)
    r.gnd_via('C29.2', (12.35, 24.3)); r.gnd_via('C30.2', (12.35, 25.5))
    # ---------------- LDO outputs ----------------
    r.track('+1V8D', [U('U2.5'), (14.0, 21.05), U('C7.1'), U('C8.1'), (14.525, 23.7)], W_PWR_S); r.via('+1V8D', (14.525, 23.7))
    r.via('+1V8D', (18.6, 22.9)); r.track('+1V8D', [(18.6, 22.9), U('TP2.1')], W_PWR_S)
    r.track('+1V8A', [U('U3.5'), (14.525, 26.85)], W_PWR_S); r.track('+1V8A', [U('C9.1'), U('C10.1')], W_PWR_S)
    r.via('+1V8A', (13.5, 25.0)); r.track('+1V8A', [(13.5, 25.0), (13.5, 26.4), (13.05, 26.85), U('U3.5')], W_PWR_S)
    r.via('+1V8A', (17.6, 25.0)); r.track('+1V8A', [(17.6, 25.0), U('FB1.1'), (17.613, 26.9), (18.4, 27.7), U('TP8.1')], W_PWR_S)
    # ---------------- GND vias ----------------
    for ref, xy in [('U2.2', (11.5, 22.4)), ('U3.2', (11.5, 28.2)), ('R2.2', (27.5, 14.3)),
                    ('C2.1', (21.55, 22.6)), ('C3.1', (25.5, 18.65)), ('C6.2', (8.375, 25.8)), ('C7.2', (16.075, 19.9)), ('C8.2', (16.075, 23.5)),
                    ('C9.2', (16.075, 24.9)), ('C10.2', (16.075, 28.5)), ('C11.2', (22.175, 26.7)), ('TP6.1', (36.5, 20.7)), ('JP1.1', (18.2, 2.2)),
                    ('C18.2', (19.5, 18.6)), ('C17.2', (18.3, 18.6)), ('C22.2', (25.72, 15.2)),
                    ('C20.2', (25.38, 10.0)),
                    ('J1.1', (4.6, 8.0)), ('J1.4', (4.6, 11.0)), ('J1.7', (4.6, 14.0)), ('J1.10', (4.6, 17.0))]:
        r.gnd_via(ref, xy)
    r.track('GND', [U('U1.4'), (18.3, 10.25)], W_SIG); r.track('GND', [U('U1.1'), (18.3, 8.75)], W_SIG)   # corner/mid GND pads straight into the EP
    r.track('GND', [U('C5.2'), (25.38, 6.15)], W_SIG); r.via('GND', (25.38, 6.15), VIA_S, VIA_S_DRILL)   # between the B.Cu I2C legs (y 5.4 / 6.9)
    r.track('GND', [U('C19.2'), (27.38, 9.8), (27.0, 10.2), (27.0, 10.6)], W_SIG); r.via('GND', (27.0, 10.6))
    r.track('GND', [U('C21.2'), (25.72, 15.2)], W_SIG)   # C21 / C22 share the GND via between their bottom pads
    r.track('GND', [U('C14.2'), (13.5, 8.55), U('C13.2')], W_SIG); r.via('GND', (13.5, 8.55))
    r.track('GND', [U('C15.2'), (13.5, 11.15), U('C16.2')], W_SIG); r.via('GND', (13.5, 11.15))
    # U1 exposed pad: 5 thermal/GND vias in the paste-free cross between the 4 paste windows (gen_pcb.ep_paste)
    for dx, dy in [(0, 0), (0, -1.2), (0, 1.2), (-1.2, 0), (1.2, 0)]:
        r.via('GND', (20.0 + dx, 10.5 + dy), VIA_S, VIA_S_DRILL)   # 0.2 mm holes: >= 0.2 mm from the paste
    # ================= THS7314 buffer section =================
    # input node
    r.track('/CVBS_SRC', [U('J5.1'), (7.9, 37.9), (8.3, 38.3), (12.9, 38.3), (12.9, 45.0), U('C25.1')], W_CVBS)
    r.track('/CVBS_SRC', [(12.9, 41.0), U('C23.1')], W_CVBS); r.track('/CVBS_SRC', [(12.9, 43.0), U('C24.1')], W_CVBS)
    r.track('/CVBS_SRC', [U('D3.1'), (9.7, 38.3)], W_CVBS); r.track('/CVBS_SRC', [U('R8.1'), (11.5, 38.3)], W_CVBS)
    r.track('Net-(U5-CH1_IN)', [U('C23.2'), (16.6, 41.0), (17.69, 42.09), U('U5.1')], W_CVBS)
    r.track('Net-(U5-CH2_IN)', [U('C24.2'), (17.4, 43.0), (17.77, 43.37), U('U5.2')], W_CVBS)
    r.track('Net-(U5-CH3_IN)', [U('C25.2'), (17.4, 45.0), (17.77, 44.63), U('U5.3')], W_CVBS)
    # outputs
    r.track('Net-(U5-CH1_OUT)', [U('U5.8'), (24.9, 42.09), (26.59, 40.4), U('R9.1')], W_CVBS)
    r.track('Net-(U5-CH2_OUT)', [U('U5.7'), U('R10.1')], W_CVBS)
    r.track('Net-(U5-CH3_OUT)', [U('U5.6'), (24.9, 44.635), (25.865, 45.6), U('R11.1')], W_CVBS)
    r.track('/TV_OUT', [U('R10.2'), (29.4, 43.4), (36.0, 43.4), (36.9, 44.3), U('J6.1')], W_CVBS)
    r.track('/GX16_OUT', [U('R11.2'), (29.2, 45.6), (29.8, 46.2), U('J7.1')], W_CVBS)
    # power: J4 -> D2 -> C28 -> FB2 -> C27/C26/TP9 -> U5.4
    r.track('/5V_IN', [U('J4.1'), (3.5, 46.4), (4.35, 47.25), (11.15, 47.25), U('D2.2')], W_PWR_S)   # under the pins, no hook over the GND pin
    r.track('/5V_BUF', [U('D2.1'), (17.5, 51.55), U('C28.1'), (21.4, 51.55), (22.21, 50.74), U('FB2.1')], W_PWR_S)
    r.track('/VS_BUF', [U('FB2.2'), (24.3, 49.0), (24.3, 51.55), U('C27.1')], W_PWR_S)
    r.track('/VS_BUF', [(24.3, 51.55), U('TP9.1')], W_PWR_S)
    r.track('/VS_BUF', [U('FB2.2'), (23.79, 48.1), (20.48, 48.1), U('C26.1')], W_PWR_S)
    r.track('/VS_BUF', [U('C26.1'), (20.48, 46.69), (19.7, 45.91), U('U5.4')], W_PWR_S)
    # GND vias of the section
    for ref, xy in [('R8.2', (11.5, 41.8)), ('D3.2', (9.7, 41.3)), ('U5.5', (24.4, 46.6)), ('C26.2', (18.6, 47.4)),
                    ('C28.2', (19.2, 49.65)), ('C27.2', (26.0, 48.5))]:
        r.gnd_via(ref, xy)
    for xy in [(2.6, 42.5), (14.0, 37.5), (22.0, 37.5), (28.0, 37.5), (35.5, 44.5), (16.0, 53.8), (21.5, 53.8), (3.5, 41.5),
               (8.25, 30.8), (8.25, 35.2), (29.6, 30.8), (29.8, 35.4), (34.2, 41.0), (40.8, 41.0), (11.0, 48.5), (31.5, 42.5), (14.0, 33.3), (22.0, 33.3), (36.0, 33.3)]:
        r.via('GND', xy)
    # GND stitching, upper-right area (old RCA place) and right edge: ties F/In1/In2/B GND every ~5 mm
    for xy in [(33.5, 2.5), (38.0, 2.5), (40.3, 6.5), (34.5, 7.5), (38.0, 10.5), (34.5, 12.5), (40.3, 14.5),
               (37.5, 17.5), (33.8, 20.0), (40.3, 22.0), (39.0, 26.5)]:
        r.via('GND', xy)
    # ---------------- zones ----------------
    W, H = 42.0, 55.0
    rect = [(0, 0), (W, 0), (W, H), (0, H)]
    r.zone('GND', IN1, rect, clearance=0.25, minw=0.25)
    r.zone('GND', F, rect, clearance=0.3, minw=0.25)
    r.zone('GND', B, rect, clearance=0.3, minw=0.25)
    # In2: GND everywhere (lowest priority) with compact power zones cut in. Shaped so that every B.Cu signal lies over ONE plane:
    #   GND under the MIPI hops (x 10..18, y 12.3..16.8), the I2C runs (y 6..7.5, x 14.75 / 15.35, y 20..21) and the east legs;
    #   +3V3 (top strip y 0.6..4.4 + left column x 4.5..8) under the whole CAM_GPIO run.
    r.zone('GND', IN2, rect, clearance=0.25, minw=0.25, priority=0)
    r.zone('+3V3', IN2, [(4.5, 0.6), (31.0, 0.6), (31.0, 4.4), (11.8, 4.4), (11.8, 6.9), (8.0, 6.9), (8.0, 21.9), (11.5, 21.9), (11.5, 29.9), (4.5, 29.9)], clearance=0.25, minw=0.25, priority=1)
    r.zone('+1V8D', IN2, [(15.9, 8.1), (22.3, 8.1), (22.3, 13.2), (19.9, 13.2), (19.9, 24.0), (12.2, 24.0), (12.2, 21.9), (16.1, 21.9),
                          (16.1, 16.8), (18.45, 16.8), (18.45, 12.3), (15.9, 12.3)], clearance=0.25, minw=0.25, priority=1)
    r.zone('+1V8A', IN2, [(22.6, 7.75), (28.6, 7.75), (28.6, 29.5), (12.0, 29.5), (12.0, 24.6), (27.0, 24.6), (27.0, 13.4), (22.6, 13.4)], clearance=0.25, minw=0.25, priority=1)

if __name__ == '__main__':
    board = pcbnew.LoadBoard(PCB)
    r = Router(board)
    route(r)
    filler = pcbnew.ZONE_FILLER(board); filler.Fill(board.Zones())
    pcbnew.SaveBoard(PCB, board)
    L = r.lengths
    print('MIPI lengths (mm, incl. 1.6 per via):', {k: round(v, 2) for k, v in L.items() if 'CSI' in k})
    print('D0 skew', round(L['/CSI_D0P'] - L['/CSI_D0N'], 2), 'CLK skew', round(L['/CSI_CLKP'] - L['/CSI_CLKN'], 2),
          'pair diff', round((L['/CSI_D0P'] + L['/CSI_D0N']) / 2 - (L['/CSI_CLKP'] + L['/CSI_CLKN']) / 2, 2))
