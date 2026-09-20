"""Stage 3: add tracks, vias and zones to hw/adv7280m-csi2.kicad_pcb (run after gen_pcb.py)."""
import os, sys, math
import pcbnew
from pcbnew import VECTOR2I, FromMM

HW = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'hw'))
PCB = os.path.join(HW, 'adv7280m-csi2.kicad_pcb')
F, B, IN1, IN2 = pcbnew.F_Cu, pcbnew.B_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu
W_MIPI, W_SIG, W_CVBS, W_PWR, W_PWR_S = 0.18, 0.2, 0.15, 0.4, 0.3
VIA, VIA_DRILL = 0.5, 0.3
VIA_S, VIA_S_DRILL = 0.45, 0.2      # MIPI crossover vias
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

D0_RET, CLK_TAIL = 11.95, 17.75

def route(r):
    U = r.pad
    # ---------------- MIPI D0 (P: U1.9 -> J1.3 ; N: U1.10 -> J1.2), crossover on N ----------------
    r.track('/CSI_D0P', [U('U1.9'), (18.25, 13.35), (17.95, 13.65), (13.95, 13.65), (13.5, 13.2), (9.6, 13.2),
                         (9.6, 10.6), (8.4, 10.6), (8.4, D0_RET), (7.8, D0_RET), (7.8, 10.0), U('J1.3')], W_MIPI)
    r.track('/CSI_D0N', [U('U1.10'), (18.75, 13.45), (18.22, 13.98), (13.2, 13.98)], W_MIPI)
    r.via('/CSI_D0N', (13.2, 13.98), VIA_S, VIA_S_DRILL)
    r.track('/CSI_D0N', [(13.2, 13.98), (11.8, 12.6)], W_MIPI, B)
    r.via('/CSI_D0N', (11.8, 12.6), VIA_S, VIA_S_DRILL)
    r.track('/CSI_D0N', [(11.8, 12.6), (11.8, 12.3), (8.5, 9.0), U('J1.2')], W_MIPI)
    # ---------------- MIPI CLK (P: U1.11 -> J1.9 ; N: U1.12 -> J1.8), crossover on N ----------------
    r.track('/CSI_CLKP', [U('U1.11'), (19.25, 13.5), (18.15, 14.6), (9.9, 14.6), (9.4, 15.1), (9.4, CLK_TAIL), (7.8, CLK_TAIL), (7.8, 16.0), U('J1.9')], W_MIPI)
    r.track('/CSI_CLKN', [U('U1.12'), (19.75, 13.5), (18.32, 14.93), (10.6, 14.93), (10.1, 15.43)], W_MIPI)
    r.via('/CSI_CLKN', (10.1, 15.43), VIA_S, VIA_S_DRILL)
    r.track('/CSI_CLKN', [(10.1, 15.43), (9.0, 14.5)], W_MIPI, B)
    r.via('/CSI_CLKN', (9.0, 14.5), VIA_S, VIA_S_DRILL)
    r.track('/CSI_CLKN', [(9.0, 14.5), (8.5, 15.0), U('J1.8')], W_MIPI)
    # ---------------- MVDD (U1.13) ----------------
    r.track('+1V8D', [U('U1.13'), (20.25, 15.0), (20.0, 15.25), U('C17.1')], W_SIG)
    r.track('+1V8D', [(20.0, 15.3), (18.8, 15.3), U('C18.1')], W_SIG)
    r.track('+1V8D', [U('C18.1'), (18.2, 15.52), (17.9, 15.7)], W_SIG); r.via('+1V8D', (17.9, 15.7))
    # ---------------- crystal ----------------
    r.track('Net-(U1-XTALP)', [U('U1.14'), (20.75, 17.6), (21.75, 18.6), U('Y1.2'), U('C2.2')], W_SIG)
    r.track('Net-(U1-XTALN)', [U('U1.15'), (21.25, 16.3), (21.85, 16.9), (26.0, 16.9), (26.65, 17.55), U('Y1.1'), U('C3.2')], W_SIG)
    # ---------------- PVDD ----------------
    r.track('PVDD', [U('U1.16'), (21.75, 13.6), (22.42, 14.27), U('C21.1'), (22.42, 15.4)], W_PWR_S); r.via('PVDD', (22.42, 15.4))
    r.track('PVDD', [(22.42, 15.4), (22.42, 16.0), (24.32, 16.0), (24.32, 15.4)], W_PWR_S, B); r.via('PVDD', (24.32, 15.4))
    r.track('PVDD', [(24.32, 15.4), U('C22.1')], W_PWR_S)
    r.track('PVDD', [(22.42, 16.0), (21.9, 16.5), (21.9, 24.6), (20.2, 24.6), (20.0, 24.8)], W_PWR, B); r.via('PVDD', (20.0, 24.8))
    r.track('PVDD', [(20.0, 24.8), (19.5, 25.4), U('FB1.2')], W_PWR_S)
    r.track('PVDD', [(20.0, 24.8), (20.4, 25.4), U('C11.1')], W_PWR_S)
    # ---------------- AVDD / VREF / AIN1 ----------------
    r.track('+1V8A', [U('U1.21'), (23.1, 10.25), (23.6, 9.75), (24.42, 9.0), (24.42, 8.1)], W_PWR_S); r.via('+1V8A', (24.42, 8.1))
    r.track('+1V8A', [U('C20.1'), (27.1, 8.2)], W_PWR_S); r.via('+1V8A', (27.1, 8.2))
    r.track('Net-(U1-VREFP)', [U('U1.19'), (23.4, 11.25), (23.7, 11.48), U('C4.1')], W_SIG)
    r.track('Net-(U1-VREFN)', [U('U1.20'), (23.4, 10.75), (23.7, 10.52), U('C4.2')], W_SIG)
    r.track('Net-(U1-AIN1)', [U('U1.17'), (23.3, 12.25), (23.95, 12.9), U('C1.2')], W_CVBS)
    # ---------------- termination node / CVBS input ----------------
    r.track('Net-(C1-Pad1)', [U('C1.1'), (27.5, 13.4), (28.2, 14.1), (28.6, 14.1), U('R2.1')], W_CVBS)
    r.track('Net-(C1-Pad1)', [(28.2, 14.1), (29.0, 14.1), (29.8, 14.9), U('R1.2')], W_CVBS)
    r.track('Net-(C1-Pad1)', [U('R1.2'), (29.975, 16.4), (30.4, 16.8), U('TP7.1')], W_CVBS)
    r.track('/CVBS_IN', [U('R1.1'), (31.9, 15.2), (31.9, 16.75), U('D1.1'), (34.4, 16.75), (35.7, 15.45), (35.7, 9.3), U('J2.1')], W_CVBS)
    # ---------------- DVDDIO / DVDD ----------------
    r.track('+3V3', [U('R4.1'), (16.3, 3.9)], W_PWR_S); r.via('+3V3', (16.3, 3.9))
    r.track('+3V3', [U('R4.1'), (15.35, 5.6), (15.35, 7.3), U('C14.1'), U('C13.1'), (16.6, 9.25), U('U1.2')], W_PWR_S)
    r.track('+1V8D', [U('U1.3'), (16.9, 9.75), (16.3, 10.35), U('C15.1'), U('C16.1'), (15.88, 12.8)], W_PWR_S); r.via('+1V8D', (15.88, 12.8))
    # ---------------- control ----------------
    r.track('/PWRDWN', [U('R4.2'), (17.1, 6.325), (18.25, 7.475), U('U1.32')], W_SIG)
    r.track('/PWRDWN', [(17.1, 6.325), (17.1, 3.1), (14.55, 3.1), U('JP3.1')], W_SIG)
    r.track('/ALSB', [U('U1.29'), (19.75, 3.5), (20.25, 3.0), U('JP1.2')], W_SIG)
    r.track('/ALSB', [U('R5.2'), (21.5, 6.0), (19.75, 6.0)], W_SIG)
    r.track('/~{RESET}', [U('R3.2'), (22.5, 7.1), (20.7, 7.1), (20.25, 7.55), U('U1.28')], W_SIG)
    r.track('/~{RESET}', [U('R3.2'), (24.0, 6.325), (24.0, 5.2), (24.4, 4.8), U('C5.1'), (25.25, 4.2), U('JP2.1'), (27.45, 2.7), U('TP5.1')], W_SIG)
    for ref, x in [('R5', 21.8), ('R3', 23.3), ('R6', 26.3), ('R7', 27.8)]:
        r.track('+3V3', [U(ref + '.1'), (x, 3.9)], W_PWR_S); r.via('+3V3', (x, 3.9))
    # I2C: vias next to U1 pads, B.Cu bus to FFC (west) and to pull-ups / test points (east)
    r.track('/SDA', [U('U1.30'), (19.2, 7.6), (19.2, 6.9)], W_SIG); r.via('/SDA', (19.2, 6.9))
    r.track('/SCL', [U('U1.31'), (18.75, 7.4), (18.5, 7.15), (18.5, 6.5)], W_SIG); r.via('/SCL', (18.5, 6.5))
    r.track('/SDA', [(19.2, 6.9), (19.2, 7.4), (15.1, 7.4), (15.1, 21.0), (8.3, 21.0)], W_SIG, B); r.via('/SDA', (8.3, 21.0))
    r.track('/SDA', [(8.3, 21.0), U('J1.14')], W_SIG)
    r.track('/SCL', [(18.5, 6.5), (14.4, 6.5), (14.4, 20.0), (8.3, 20.0)], W_SIG, B); r.via('/SCL', (8.3, 20.0))
    r.track('/SCL', [(8.3, 20.0), U('J1.13')], W_SIG)
    r.track('/SDA', [(19.2, 7.4), (26.1, 7.4), (26.3, 7.6), (26.3, 25.9)], W_SIG, B); r.via('/SDA', (26.3, 7.6)); r.via('/SDA', (26.3, 25.9))
    r.track('/SDA', [U('R6.2'), (26.3, 7.6)], W_SIG); r.track('/SDA', [(26.3, 25.9), U('TP3.1')], W_SIG)
    r.track('/SCL', [(18.5, 6.5), (18.5, 6.0), (27.6, 6.0), (27.8, 6.2), (27.8, 25.2), (28.5, 25.9), (28.9, 25.9)], W_SIG, B)
    r.via('/SCL', (27.8, 7.6)); r.via('/SCL', (28.9, 25.9))
    r.track('/SCL', [U('R7.2'), (27.8, 7.6)], W_SIG); r.track('/SCL', [(28.9, 25.9), U('TP4.1')], W_SIG)
    # CAM_GPIO: jumpers on top edge, B.Cu to FFC pin 11
    r.via('/CAM_GPIO', (23.95, 1.5)); r.via('/CAM_GPIO', (12.85, 1.5)); r.via('/CAM_GPIO', (8.3, 18.6))
    r.track('/CAM_GPIO', [U('JP2.2'), (23.95, 1.5)], W_SIG); r.track('/CAM_GPIO', [U('JP3.2'), (12.85, 1.5)], W_SIG)
    r.track('/CAM_GPIO', [(23.95, 1.5), (10.9, 1.5), (10.9, 18.6), (8.3, 18.6)], W_SIG, B)
    r.track('/CAM_GPIO', [(8.3, 18.6), (8.0, 18.3), (7.25, 18.0), U('J1.11')], W_SIG)
    # ---------------- 3V3 bottom-left ----------------
    r.track('+3V3', [U('J1.15'), (7.25, 22.0), (7.8, 22.55), (7.8, 22.8)], W_PWR_S); r.via('+3V3', (7.8, 22.8))
    r.track('+3V3', [(7.8, 22.8), (7.8, 23.6), (7.2, 24.3), U('C6.1')], W_PWR_S)
    r.via('+3V3', (11.4, 19.4)); r.track('+3V3', [U('TP1.1'), (11.4, 19.4), (11.4, 19.8), (10.6, 20.6), U('U2.1')], W_PWR_S)
    r.via('+3V3', (9.4, 23.5)); r.track('+3V3', [(9.4, 23.5), (9.9, 23.0), U('U2.3')], W_PWR_S)
    r.via('+3V3', (9.4, 25.5)); r.track('+3V3', [(9.4, 25.5), (9.9, 26.0), U('U3.1')], W_PWR_S)
    r.via('+3V3', (9.4, 28.5)); r.track('+3V3', [(9.4, 28.5), (9.9, 28.0), U('U3.3')], W_PWR_S)
    # ---------------- LDO outputs ----------------
    r.track('+1V8D', [U('U2.5'), (14.0, 21.05), U('C7.1'), U('C8.1'), (14.525, 23.7)], W_PWR_S); r.via('+1V8D', (14.525, 23.7))
    r.via('+1V8D', (18.6, 22.9)); r.track('+1V8D', [(18.6, 22.9), U('TP2.1')], W_PWR_S)
    r.track('+1V8A', [U('U3.5'), (14.0, 26.05), U('C9.1'), U('C10.1')], W_PWR_S)
    r.via('+1V8A', (13.5, 25.0)); r.track('+1V8A', [(13.5, 25.0), (13.5, 25.6), (13.1, 26.0), U('U3.5')], W_PWR_S)
    r.via('+1V8A', (17.6, 25.0)); r.track('+1V8A', [(17.6, 25.0), U('FB1.1'), (17.613, 26.9), (18.4, 27.7), U('TP8.1')], W_PWR_S)
    # ---------------- GND vias ----------------
    for ref, xy in [('U2.2', (11.5, 22.4)), ('U3.2', (11.5, 27.4)), ('R2.2', (28.6, 16.9)), ('D1.2', (33.2, 15.2)),
                    ('C2.1', (22.95, 24.2)), ('C3.1', (27.1, 24.3)), ('C6.2', (8.375, 25.8)), ('C7.2', (16.075, 19.9)), ('C8.2', (16.075, 23.5)),
                    ('C9.2', (16.075, 24.9)), ('C10.2', (16.075, 28.5)), ('C11.2', (22.175, 26.7)), ('TP6.1', (36.5, 20.7)), ('JP1.1', (18.2, 2.2)),
                    ('C17.2', (20.0, 17.3)), ('C18.2', (18.8, 17.3)), ('C21.2', (23.38, 15.3)), ('C22.2', (25.28, 15.3)),
                    ('C19.2', (25.38, 10.0)),
                    ('J1.1', (4.6, 8.0)), ('J1.4', (4.6, 11.0)), ('J1.7', (4.6, 14.0)), ('J1.10', (4.6, 17.0))]:
        r.gnd_via(ref, xy)
    r.track('GND', [U('U1.4'), (18.3, 10.25)], W_SIG); r.track('GND', [U('U1.1'), (18.3, 8.75)], W_SIG)   # corner/mid GND pads straight into the EP
    r.track('GND', [U('C5.2'), (24.8, 7.0), (25.38, 7.6), U('C19.2')], W_SIG)
    r.track('GND', [U('C20.2'), (27.38, 9.8), (27.0, 10.2), (27.0, 10.6)], W_SIG); r.via('GND', (27.0, 10.6))
    r.track('GND', [U('C14.2'), (13.5, 8.55), U('C13.2')], W_SIG); r.via('GND', (13.5, 8.55))
    r.track('GND', [U('C15.2'), (13.5, 11.15), U('C16.2')], W_SIG); r.via('GND', (13.5, 11.15))
    for dx in (-1.0, 0.0, 1.0):
        for dy in (-1.0, 0.0, 1.0):
            r.via('GND', (20.0 + dx, 10.5 + dy))
    # ---------------- GND return vias next to the MIPI crossover vias (D0N) ----------------
    r.via('GND', (12.5, 12.2)); r.via('GND', (12.6, 15.9))
    # ================= stage 5: THS7314 buffer section =================
    # CVBS_IN link: straight down from the decoder input node through the right tab (x 30.5..33) to JP4 pad 2
    r.track('/CVBS_IN', [(31.9, 16.75), (31.9, 36.75), U('JP4.2')], W_CVBS)
    # JP4 is a net-tie footprint: its bridge polygon must only be touched by its own pads -> enter pad 1 from the right
    r.track('/CVBS_BUF', [U('R9.2'), (29.5, 40.4), (31.9, 38.0), (34.0, 38.0), (34.6, 37.4), (34.6, 36.8), (36.9, 36.8), U('J8.1')], W_CVBS)
    r.track('/CVBS_BUF', [(34.6, 36.8), (33.5, 36.8)], W_CVBS)
    # input node
    r.track('/CVBS_SRC', [U('J5.1'), (12.9, 38.3), (12.9, 45.0), U('C25.1')], W_CVBS)
    r.track('/CVBS_SRC', [(12.9, 41.0), U('C23.1')], W_CVBS); r.track('/CVBS_SRC', [(12.9, 43.0), U('C24.1')], W_CVBS)
    r.track('/CVBS_SRC', [U('D3.1'), (9.7, 38.3)], W_CVBS); r.track('/CVBS_SRC', [U('R8.1'), (11.5, 38.3)], W_CVBS)
    r.track('Net-(U5-CH1_IN)', [U('C23.2'), (16.6, 41.0), (17.69, 42.09), U('U5.1')], W_CVBS)
    r.track('Net-(U5-CH2_IN)', [U('C24.2'), (17.4, 43.0), (17.77, 43.37), U('U5.2')], W_CVBS)
    r.track('Net-(U5-CH3_IN)', [U('C25.2'), (17.4, 45.0), (17.77, 44.63), U('U5.3')], W_CVBS)
    # outputs
    r.track('Net-(U5-CH1_OUT)', [U('U5.8'), (24.9, 42.09), (26.59, 40.4), U('R9.1')], W_CVBS)
    r.track('Net-(U5-CH2_OUT)', [U('U5.7'), U('R10.1')], W_CVBS)
    r.track('Net-(U5-CH3_OUT)', [U('U5.6'), (24.9, 44.63), (26.67, 46.4), U('R11.1')], W_CVBS)
    r.track('/MON1', [U('R10.2'), (29.4, 43.4), (37.0, 51.0), U('J6.1')], W_CVBS)
    r.track('/MON2', [U('R11.2'), (29.9, 46.4), (30.0, 46.5), U('J7.1')], W_CVBS)
    # power: J4 -> D2 -> C28 -> FB2 -> C27/C26/TP9 -> U5.4
    r.track('/5V_IN', [U('J4.1'), (9.1, 49.6), U('D2.2')], W_PWR_S)
    r.track('/5V_BUF', [U('D2.1'), (17.5, 51.55), U('C28.1'), (21.4, 51.55), (22.21, 50.74), U('FB2.1')], W_PWR_S)
    r.track('/VS_BUF', [U('FB2.2'), (24.3, 49.0), (24.3, 51.55), U('C27.1'), (27.3, 51.55), (28.6, 50.25), U('TP9.1')], W_PWR_S)
    r.track('/VS_BUF', [U('FB2.2'), (23.79, 48.1), (20.48, 48.1), U('C26.1')], W_PWR_S)
    r.track('/VS_BUF', [U('C26.1'), (20.48, 46.69), (19.7, 45.91), U('U5.4')], W_PWR_S)
    # GND vias of the section
    for ref, xy in [('R8.2', (11.5, 41.8)), ('D3.2', (9.7, 41.3)), ('U5.5', (24.4, 46.6)), ('C26.2', (18.6, 47.4)),
                    ('C28.2', (19.2, 49.65)), ('C27.2', (26.0, 48.5))]:
        r.gnd_via(ref, xy)
    for xy in [(2.6, 42.5), (14.0, 37.5), (22.0, 37.5), (28.0, 37.5), (35.5, 44.5), (16.0, 53.8), (23.0, 53.8), (3.5, 41.5),
               (8.25, 30.8), (8.25, 35.2), (29.6, 30.8), (29.8, 35.4), (34.2, 41.0), (40.8, 41.0), (12.0, 47.0), (31.5, 42.5)]:
        r.via('GND', xy)
    # ---------------- zones ----------------
    W, H = 42.0, 55.0   # zones cover both sections; the fill follows the board outline (slot + tabs)
    rect = [(0, 0), (W, 0), (W, H), (0, H)]
    r.zone('GND', IN1, rect, clearance=0.25, minw=0.25)
    r.zone('GND', F, rect, clearance=0.3, minw=0.25)
    r.zone('GND', B, rect, clearance=0.3, minw=0.25)
    # +3V3 zone bulges to x = 13.9 under the D0N crossover stub (B.Cu, x 11.8..13.2) so that both MIPI B.Cu stubs see one continuous reference plane
    r.zone('+3V3', IN2, [(4.5, 3.4), (31.0, 3.4), (31.0, 4.4), (11.5, 4.4), (11.5, 11.6), (13.9, 11.6), (13.9, 16.5), (11.5, 16.5), (11.5, 29.5), (4.5, 29.5)], clearance=0.25, minw=0.25)
    r.zone('+1V8D', IN2, [(12.2, 7.2), (22.3, 7.2), (22.3, 13.2), (19.9, 13.2), (19.9, 24.0), (12.2, 24.0), (12.2, 17.0), (14.4, 17.0), (14.4, 11.1), (12.2, 11.1)], clearance=0.25, minw=0.25)
    # GND zone on In2 for the buffer section (reference for its B.Cu-side vias, stitching)
    r.zone('GND', IN2, [(0, 34.0), (W, 34.0), (W, H), (0, H)], clearance=0.25, minw=0.25)
    r.zone('+1V8A', IN2, [(22.6, 7.5), (28.6, 7.5), (28.6, 29.5), (12.0, 29.5), (12.0, 24.6), (27.0, 24.6), (27.0, 13.4), (22.6, 13.4)], clearance=0.25, minw=0.25)

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
