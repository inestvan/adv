"""Generate the KiCad 9 schematic hw/adv7280m-csi2.kicad_sch (single sheet, A3)."""
import os, sys, math, uuid
sys.path.insert(0, os.path.dirname(__file__))
from kisexp import *

HW = os.path.join(os.path.dirname(__file__), '..', 'hw')
KSYM = '/usr/share/kicad/symbols'
PROJECT = 'adv7280m-csi2'
ROOT_UUID = '3f6a0c2e-7c1b-4a52-9d3e-adv7280m0001'.replace('adv7280m', 'a0b1c2d3')  # fixed uuid for reproducibility

LIBS = {'Device': 'Device.kicad_sym', 'Regulator_Linear': 'Regulator_Linear.kicad_sym', 'power': 'power.kicad_sym',
        'Connector_Generic': 'Connector_Generic.kicad_sym', 'Connector': 'Connector.kicad_sym', 'Jumper': 'Jumper.kicad_sym',
        'Mechanical': 'Mechanical.kicad_sym', 'adv-parts': os.path.join(HW, 'lib', 'adv-parts.kicad_sym')}

def g(v):  # snap to 1.27 grid
    return round(round(v / 1.27) * 1.27, 4)

def eff(size=1.27, hide=False, justify=None):
    e = S('effects', S('font', S('size', size, size)))
    if justify: e.append(S('justify', *[Sym(j) for j in justify.split()]))
    if hide: e.append(S('hide', Sym('yes')))
    return e

class Part:
    def __init__(self, sch, libid, ref, value, x, y, rot, fp, lcsc, desc, extra, dnp, in_bom, on_board, hide_ref=False, hide_val=False, ref_pos=None, val_pos=None, prop_angle=None):
        self.sch, self.libid, self.ref, self.value = sch, libid, ref, value
        self.x, self.y, self.rot = x, y, rot
        self.fp, self.lcsc, self.desc, self.extra = fp, lcsc, desc, extra or {}
        self.dnp, self.in_bom, self.on_board = dnp, in_bom, on_board
        self.hide_ref, self.hide_val = hide_ref, hide_val
        self.ref_pos, self.val_pos = ref_pos, val_pos
        self.prop_angle = prop_angle
        self.uuid = str(uuid.uuid5(uuid.NAMESPACE_URL, 'adv7280m-csi2/sym/' + ref))
        self.sym = sch.libsym(libid)
        self.pins = {p['number']: p for p in symbol_pins(self.sym)}
    def pin(self, num):
        p = self.pins[str(num)]
        r = math.radians(self.rot)
        c, s = round(math.cos(r)), round(math.sin(r))
        xp = p['x'] * c - p['y'] * s
        yp = p['x'] * s + p['y'] * c
        return (round(self.x + xp, 4), round(self.y - yp, 4))
    def pin_points(self):
        return [self.pin(n) for n in self.pins]

class Sch:
    def __init__(self):
        self.libsyms = {}
        self.parts = []
        self.wires = []
        self.labels = []
        self.ncs = []
        self.texts = []
        self.pwr_n = 0
    def libsym(self, libid):
        if libid not in self.libsyms:
            lib, name = libid.split(':')
            path = LIBS[lib] if lib == 'adv-parts' else os.path.join(KSYM, LIBS[lib])
            s = flatten_symbol(load_lib(path), name)
            s[1] = libid
            self.libsyms[libid] = s
        return self.libsyms[libid]
    def place(self, libid, ref, value, x, y, rot=0, fp='', lcsc='', desc='', extra=None, dnp=False, in_bom=True, on_board=True, **kw):
        p = Part(self, libid, ref, value, g(x), g(y), rot, fp, lcsc, desc, extra, dnp, in_bom, on_board, **kw)
        self.parts.append(p)
        return p
    def power(self, value, x, y, rot=0, lib='power', **kw):
        self.pwr_n += 1
        p = Part(self, f'{lib}:{value}', f'#PWR{self.pwr_n:03d}', value, g(x), g(y), rot, '', '', '', {}, False, False, True, hide_ref=True, **kw)
        self.parts.append(p)
        return p
    def flag(self, x, y, rot=0):
        self.pwr_n += 1
        p = Part(self, 'power:PWR_FLAG', f'#FLG{self.pwr_n:03d}', 'PWR_FLAG', g(x), g(y), rot, '', '', '', {}, False, False, True, hide_ref=True)
        self.parts.append(p)
        return p
    def wire(self, a, b):
        a = (g(a[0]), g(a[1])); b = (g(b[0]), g(b[1]))
        if a == b: return
        assert a[0] == b[0] or a[1] == b[1], f'non-orthogonal wire {a}-{b}'
        self.wires.append((a, b))
    def path(self, *pts):
        for a, b in zip(pts, pts[1:]): self.wire(a, b)
    def label(self, name, x, y, rot=0):
        self.labels.append((name, g(x), g(y), rot))
    def nc(self, pt):
        self.ncs.append((g(pt[0]), g(pt[1])))
    def text(self, s, x, y, size=1.27, bold=False):
        self.texts.append((s, g(x), g(y), size, bold))

    # ---------- wire splitting + junction computation ----------
    def _split_wires(self):
        """Split wire segments at every point where another wire ends or a pin/label sits in the interior
        (KiCad connectivity only joins wires at segment endpoints)."""
        pts = set()
        for a, b in self.wires: pts.add(a); pts.add(b)
        for p in self.parts:
            for pt in p.pin_points(): pts.add(pt)
        for name, x, y, rot in self.labels: pts.add((x, y))
        out = []
        for a, b in self.wires:
            inner = []
            for pt in pts:
                if a[0] == b[0] == pt[0] and min(a[1], b[1]) < pt[1] < max(a[1], b[1]): inner.append(pt)
                elif a[1] == b[1] == pt[1] and min(a[0], b[0]) < pt[0] < max(a[0], b[0]): inner.append(pt)
            chain = [a] + sorted(inner, key=lambda q: (abs(q[0] - a[0]) + abs(q[1] - a[1]))) + [b]
            for u, v in zip(chain, chain[1:]): out.append((u, v))
        self.wires = out

    def junctions(self):
        from collections import defaultdict
        self._split_wires()
        ends = defaultdict(int)
        for a, b in self.wires:
            ends[a] += 1; ends[b] += 1
        pinpts = defaultdict(int)
        for p in self.parts:
            for pt in p.pin_points(): pinpts[pt] += 1
        js = []
        for pt in set(list(ends) + list(pinpts)):
            if ends[pt] + pinpts[pt] >= 3: js.append(pt)
        return js

    # ---------- emit ----------
    def emit(self, title, rev, date):
        root = S('kicad_sch', S('version', 20250114), S('generator', 'eeschema'), S('generator_version', '9.0'),
                 S('uuid', ROOT_UUID), S('paper', 'A3'),
                 S('title_block', S('title', title), S('date', date), S('rev', rev),
                   S('comment', 1, 'Target: Raspberry Pi 4 CSI camera port, dtoverlay=adv728x-m,adv7280m=1'),
                   S('comment', 2, 'Sources: ADV7280 DS Rev.A, UG-637 Rev.A, AN-1260; see docs/requirements.md')))
        ls = S('lib_symbols')
        for k in sorted(self.libsyms): ls.append(self.libsyms[k])
        root.append(ls)
        for pt in self.junctions():
            root.append(S('junction', S('at', pt[0], pt[1]), S('diameter', 0), S('color', 0, 0, 0, 0), S('uuid', new_uuid())))
        for pt in self.ncs:
            root.append(S('no_connect', S('at', pt[0], pt[1]), S('uuid', new_uuid())))
        for a, b in self.wires:
            root.append(S('wire', S('pts', S('xy', a[0], a[1]), S('xy', b[0], b[1])),
                          S('stroke', S('width', 0), S('type', Sym('default'))), S('uuid', new_uuid())))
        for name, x, y, rot in self.labels:
            just = 'right bottom' if rot == 180 else 'left bottom'
            root.append(S('label', name, S('at', x, y, rot), S('effects', S('font', S('size', 1.27, 1.27)), S('justify', *[Sym(j) for j in just.split()])), S('uuid', new_uuid())))
        for s, x, y, size, bold in self.texts:
            f = S('font', S('size', size, size))
            if bold: f.append(S('bold', Sym('yes')))
            root.append(S('text', s, S('exclude_from_sim', Sym('no')), S('at', x, y, 0), S('effects', f, S('justify', Sym('left'), Sym('bottom'))), S('uuid', new_uuid())))
        for p in self.parts:
            root.append(self.emit_part(p))
        root.append(S('sheet_instances', S('path', '/', S('page', '1'))))
        return root

    def emit_part(self, p):
        yn = lambda b: Sym('yes' if b else 'no')
        node = S('symbol', S('lib_id', p.libid), S('at', p.x, p.y, p.rot), S('unit', 1),
                 S('exclude_from_sim', Sym('no')), S('in_bom', yn(p.in_bom)), S('on_board', yn(p.on_board)), S('dnp', yn(p.dnp)),
                 S('uuid', p.uuid))
        # property placement
        if p.ref_pos: rx, ry, rj = p.ref_pos
        elif p.rot in (0, 180): rx, ry, rj = p.x + 2.54, p.y - 1.27, 'left'
        else: rx, ry, rj = p.x, p.y - 2.54, None
        if p.val_pos: vx, vy, vj = p.val_pos
        elif p.rot in (0, 180): vx, vy, vj = p.x + 2.54, p.y + 1.27, 'left'
        else: vx, vy, vj = p.x, p.y + 2.54, None
        pa = p.prop_angle if p.prop_angle is not None else (90 if p.rot in (90, 270) else 0)
        node.append(S('property', 'Reference', p.ref, S('at', g(rx), g(ry), pa), eff(hide=p.hide_ref, justify=rj)))
        node.append(S('property', 'Value', p.value, S('at', g(vx), g(vy), pa), eff(hide=p.hide_val, justify=vj)))
        node.append(S('property', 'Footprint', p.fp, S('at', p.x, p.y, 0), eff(hide=True)))
        ds = ''
        for pr in find_all(p.sym, 'property'):
            if pr[1] == 'Datasheet': ds = pr[2]
        node.append(S('property', 'Datasheet', p.extra.get('Datasheet', ds), S('at', p.x, p.y, 0), eff(hide=True)))
        node.append(S('property', 'Description', p.desc, S('at', p.x, p.y, 0), eff(hide=True)))
        if not p.ref.startswith('#'):
            node.append(S('property', 'LCSC', p.lcsc, S('at', p.x, p.y, 0), eff(hide=True)))
            for k, v in p.extra.items():
                if k == 'Datasheet': continue
                node.append(S('property', k, v, S('at', p.x, p.y, 0), eff(hide=True)))
        for num in p.pins:
            node.append(S('pin', num, S('uuid', new_uuid())))
        node.append(S('instances', S('project', PROJECT, S('path', '/' + ROOT_UUID, S('reference', p.ref), S('unit', 1)))))
        return node

# ------------------------------------------------------------------------------------------------
FP = dict(R='Resistor_SMD:R_0603_1608Metric', C0402='Capacitor_SMD:C_0402_1005Metric', C0603='Capacitor_SMD:C_0603_1608Metric',
          L='Inductor_SMD:L_0603_1608Metric', SOT235='Package_TO_SOT_SMD:SOT-23-5', XTAL='Crystal:Crystal_SMD_5032-2Pin_5.0x3.2mm',
          JP='Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm', TP='TestPoint:TestPoint_Pad_D1.5mm',
          HOLE='MountingHole:MountingHole_2.2mm_M2', ESD='adv-parts:TI_X1SON-2_DPY0002A_1.0x0.6mm',
          FFC='adv-parts:Amphenol_SFW15R-1STE1LF', RCA='adv-parts:RCA_Multicomp_PSG01546_Horizontal',
          SMA='adv-parts:SMA_BWSMA-KWE-Z001_EdgeMount', QFN='Package_DFN_QFN:QFN-32-1EP_5x5mm_P0.5mm_EP3.6x3.6mm')

LCSC = {'R22': 'C23345', 'R51': 'C23197', 'R10k': 'C25804', 'R4k7': 'C23162',
        'C100n_0402': 'C1525', 'C10n_0402': 'C15195', 'C100n_0603': 'C14663', 'C1u': 'C15849', 'C10u': 'C19702', 'C27p': 'C92670',
        'XTAL': 'C112564', 'ESD': 'C48260', 'FB': 'C14709', 'LDO': 'C176944', 'FFC': 'C3168538', 'SMA': 'C496551', 'U1': 'C662261'}

def build():
    s = Sch()
    # ---------------- U1 ----------------
    Ux, Uy = 200.66, 129.54
    U1 = s.place('adv-parts:ADV7280WBCPZ-M', 'U1', 'ADV7280WBCPZ-M', Ux, Uy, 0, FP['QFN'], LCSC['U1'],
                 'SDTV video decoder, MIPI CSI-2 output, LFCSP-32 (hand-soldered by owner, not by JLCPCB)',
                 extra={'Assemble': 'no', 'Manufacturer': 'Analog Devices', 'MPN': 'ADV7280WBCPZ-M'},
                 ref_pos=(Ux - 17.78, Uy - 24.13, 'left'), val_pos=(Ux, Uy - 1.27, None))
    def UP(name):
        for n, p in U1.pins.items():
            if p['name'] == name: return U1.pin(n)
        raise KeyError(name)

    # ---------------- analog input ----------------
    ain1 = UP('AIN1')                                   # (180.34, 109.22)
    yA = ain1[1]
    C1 = s.place('Device:C', 'C1', '100nF', 170.18, yA, 90, FP['C0603'], LCSC['C100n_0603'], 'AC coupling, 50 V X7R 0603')
    s.wire(ain1, C1.pin(2))
    xT = 157.48                                          # termination node
    s.wire(C1.pin(1), (xT, yA))
    R1 = s.place('Device:R', 'R1', '22', 149.86, yA, 90, FP['R'], LCSC['R22'], 'Series termination (24 R in UG-637 Fig. 8; 22 R = nearest JLCPCB Basic)')
    s.wire(R1.pin(2), (xT, yA))
    R2 = s.place('Device:R', 'R2', '51', xT, 116.84, 0, FP['R'], LCSC['R51'], 'Shunt termination, 22+51 = 73 R')
    s.wire((xT, yA), R2.pin(1))
    s.power('GND', *R2.pin(2))
    TP7 = s.place('Connector:TestPoint', 'TP7', 'CVBS_T', xT, 101.6, 0, FP['TP'], '', 'CVBS after termination', ref_pos=(xT + 1.27, 99.06, 'left'), val_pos=(xT + 1.27, 101.6, 'left'))
    s.wire(TP7.pin(1), (xT, yA))
    xE = 134.62
    s.wire(R1.pin(1), (xE, yA))
    D1 = s.place('Device:D_TVS', 'D1', 'TPD1E10B06', xE, 116.84, 270, FP['ESD'], LCSC['ESD'], 'ESD TVS, 12 pF, bidirectional', ref_pos=(xE + 2.54, 115.57, 'left'), val_pos=(xE + 2.54, 118.11, 'left'))
    s.wire((xE, yA), D1.pin(1))
    s.power('GND', *D1.pin(2))
    xJ = 127.0
    s.wire((xE, yA), (xJ, yA))
    J2 = s.place('Connector:Conn_Coaxial', 'J2', 'CVBS_RCA', 116.84, 99.06, 180, FP['RCA'], '', 'RCA jack, hand-soldered (not stocked by JLCPCB)',
                 extra={'Assemble': 'no', 'MPN': 'PSG01546 or equivalent'}, ref_pos=(109.22, 96.52, 'left'), val_pos=(106.68, 104.14, 'left'))
    J3 = s.place('Connector:Conn_Coaxial', 'J3', 'CVBS_SMA', 116.84, 121.92, 180, FP['SMA'], LCSC['SMA'], 'SMA edge-mount jack (alternative input)',
                 extra={'MPN': 'BWSMA-KWE-Z001'}, ref_pos=(109.22, 119.38, 'left'), val_pos=(106.68, 127.0, 'left'))
    s.path(J2.pin(1), (xJ, J2.pin(1)[1]), (xJ, J3.pin(1)[1]), J3.pin(1))
    s.power('GND', J2.pin(2)[0], J2.pin(2)[1], 180)
    s.power('GND', J3.pin(2)[0], J3.pin(2)[1], 180)
    s.label('CVBS_IN', xJ, yA - 0.0, 0)
    for nm in ['AIN2', 'AIN3', 'AIN4', 'AIN5', 'AIN6', 'AIN7', 'AIN8']:
        s.nc(UP(nm))
    # VREF
    vp, vn = UP('VREFP'), UP('VREFN')
    C4 = s.place('Device:C', 'C4', '100nF', 172.72, 133.35, 0, FP['C0402'], LCSC['C100n_0402'], 'VREFP-VREFN, close to U1', ref_pos=(170.18, 132.08, 'right'), val_pos=(170.18, 134.62, 'right'))
    s.path(vp, (176.53, vp[1]), (176.53, C4.pin(1)[1]), C4.pin(1))
    s.path(vn, (176.53, vn[1]), (176.53, C4.pin(2)[1]), C4.pin(2))
    # crystal
    xp, xn = UP('XTALP'), UP('XTALN')
    Y1 = s.place('Device:Crystal', 'Y1', '28.63636MHz', 167.64, 143.51, 90, FP['XTAL'], LCSC['XTAL'], 'YXC YSX530GA 5032, CL=20pF, ESR<=40R, +-20ppm',
                 extra={'MPN': 'X50322863636MSB2GI'}, ref_pos=(167.64, 151.13, None), val_pos=(167.64, 153.67, None))
    s.wire(xp, Y1.pin(2))                                              # XTALP straight to crystal top pin (y=139.7)
    s.path(xn, (175.26, xn[1]), (175.26, Y1.pin(1)[1]), Y1.pin(1))
    C2 = s.place('Device:C', 'C2', '27pF', 156.21, Y1.pin(2)[1], 90, FP['C0603'], LCSC['C27p'], 'Crystal load, C0G')
    C3 = s.place('Device:C', 'C3', '27pF', 156.21, Y1.pin(1)[1], 90, FP['C0603'], LCSC['C27p'], 'Crystal load, C0G')
    s.wire(Y1.pin(2), C2.pin(2)); s.wire(Y1.pin(1), C3.pin(2))
    s.wire(C2.pin(1), C3.pin(1))
    s.wire(C3.pin(1), (C3.pin(1)[0], 152.4))
    s.power('GND', C3.pin(1)[0], 152.4)

    # ---------------- supplies at U1 ----------------
    dvddio, dvdd, avdd, pvdd, mvdd = UP('DVDDIO'), UP('DVDD'), UP('AVDD'), UP('PVDD'), UP('MVDD')
    s.path(dvddio, (dvddio[0], 99.06), (180.34, 99.06)); s.power('+3V3', 180.34, 99.06)
    s.path(dvdd, (dvdd[0], 93.98)); s.power('+1V8D', dvdd[0], 93.98, lib='adv-parts')
    s.path(avdd, (avdd[0], 99.06)); s.power('+1V8A', avdd[0], 99.06, lib='adv-parts')
    s.path(pvdd, (pvdd[0], 91.44)); s.power('PVDD', pvdd[0], 91.44, lib='adv-parts')
    s.path(mvdd, (mvdd[0], 99.06), (220.98, 99.06)); s.power('+1V8D', 220.98, 99.06, lib='adv-parts')
    g1, g4, ep = U1.pin('1'), U1.pin('4'), U1.pin('33')
    yG = g1[1] + 2.54
    s.path(g1, (g1[0], yG), (ep[0], yG), ep)
    s.wire(g4, (g4[0], yG))
    s.wire((g4[0], yG), (g4[0], yG + 2.54)); s.power('GND', g4[0], yG + 2.54)

    # ---------------- digital side ----------------
    for nm, net in [('D0P', 'CSI_D0P'), ('D0N', 'CSI_D0N'), ('CLKP', 'CSI_CLKP'), ('CLKN', 'CSI_CLKN'),
                    ('SDATA', 'SDA'), ('SCLK', 'SCL'), ('ALSB', 'ALSB'), ('~{RESET}', '~{RESET}'), ('~{PWRDWN}', 'PWRDWN')]:
        p = UP(nm)
        s.wire(p, (p[0] + 7.62, p[1])); s.label(net, p[0] + 7.62, p[1], 0)
    for nm in ['INTRQ', 'GPO0', 'GPO1', 'GPO2']:
        s.nc(UP(nm))

    # ---------------- J1: Raspberry Pi camera FFC ----------------
    J1 = s.place('Connector_Generic:Conn_01x15', 'J1', 'RPi_CSI_15pin', 330.2, 127.0, 0, FP['FFC'], LCSC['FFC'],
                 'FFC 15 pos 1.0 mm bottom contact (Amphenol SFW15R-1STE1LF, same as Pi Camera Module)', extra={'MPN': 'SFW15R-1STE1LF'},
                 ref_pos=(327.66, 106.68, 'left'), val_pos=(327.66, 148.59, 'left'))
    nets = {2: 'CSI_D0N', 3: 'CSI_D0P', 8: 'CSI_CLKN', 9: 'CSI_CLKP', 11: 'CAM_GPIO', 13: 'SCL', 14: 'SDA'}
    for n in range(1, 16):
        p = J1.pin(n)
        if n in (1, 4, 7, 10):
            s.wire(p, (p[0] - 5.08, p[1])); s.power('GND', p[0] - 5.08, p[1], 270, val_pos=(p[0] - 9.53, p[1] - 0.635, 'right'))
        elif n == 15:
            s.wire(p, (p[0] - 5.08, p[1])); s.power('+3V3', p[0] - 5.08, p[1], 90, val_pos=(p[0] - 9.53, p[1] - 0.635, 'right'))
        elif n in nets:
            s.wire(p, (p[0] - 10.16, p[1])); s.label(nets[n], p[0] - 10.16, p[1], 180)
        else:
            s.nc(p)
    s.text('Pi 4 J3 (CAMERA) pinout, 1:1 with Camera Module v2.1:\n1 GND  2 D0N  3 D0P  4 GND  5 D1N(nc)  6 D1P(nc)  7 GND\n8 CLKN  9 CLKP  10 GND  11 CAM_GPIO  12 nc  13 SCL0  14 SDA0  15 3V3\nI2C on Pi 4 = GPIO44/45 (i2c_csi_dsi); pull-ups to 3V3 exist on the Pi.', 300.0, 165.1, 1.27)

    # ---------------- control: reset / pwrdwn / alsb / i2c pull-ups ----------------
    # RESET
    xR = 243.84
    R3 = s.place('Device:R', 'R3', '10k', xR, 55.88, 0, FP['R'], LCSC['R10k'], 'RESET pull-up')
    s.power('+3V3', xR, R3.pin(1)[1] - 2.54); s.wire((xR, R3.pin(1)[1] - 2.54), R3.pin(1))
    yRn = 62.23
    s.wire(R3.pin(2), (xR, yRn))
    C5 = s.place('Device:C', 'C5', '1uF', xR, 66.04, 0, FP['C0603'], LCSC['C1u'], 'RESET RC, tau = 10 ms (>5 ms min)')
    s.wire((xR, yRn), C5.pin(1)); s.power('GND', *C5.pin(2))
    s.wire((xR, yRn), (251.46, yRn)); s.label('~{RESET}', 251.46, yRn, 0)
    TP5 = s.place('Connector:TestPoint', 'TP5', 'nRESET', 236.22, 57.15, 0, FP['TP'], '', 'RESET test point', ref_pos=(234.95, 54.61, 'right'), val_pos=(234.95, 57.15, 'right'))
    s.wire(TP5.pin(1), (236.22, yRn)); s.wire((236.22, yRn), (xR, yRn))
    JP2 = s.place('Jumper:SolderJumper_2_Open', 'JP2', 'CAM_GPIO>RST', 266.7, yRn, 0, FP['JP'], '', 'Open by default: connects CAM_GPIO to RESET', ref_pos=(262.89, 59.69, 'left'), val_pos=(259.08, 66.04, 'left'))
    s.wire((251.46, yRn), JP2.pin(1))
    s.wire(JP2.pin(2), (274.32, yRn)); s.label('CAM_GPIO', 274.32, yRn, 0)
    # PWRDWN
    R4 = s.place('Device:R', 'R4', '10k', xR, 83.82, 0, FP['R'], LCSC['R10k'], 'PWRDWN pull-up (chip enabled by default)')
    s.power('+3V3', xR, R4.pin(1)[1] - 2.54); s.wire((xR, R4.pin(1)[1] - 2.54), R4.pin(1))
    yPn = 90.17
    s.wire(R4.pin(2), (xR, yPn)); s.wire((xR, yPn), (251.46, yPn)); s.label('PWRDWN', 251.46, yPn, 0)
    JP3 = s.place('Jumper:SolderJumper_2_Open', 'JP3', 'CAM_GPIO>PWRDWN', 266.7, yPn, 0, FP['JP'], '', 'Open by default: connects CAM_GPIO to PWRDWN', ref_pos=(262.89, 87.63, 'left'), val_pos=(257.81, 93.98, 'left'))
    s.wire((251.46, yPn), JP3.pin(1)); s.wire(JP3.pin(2), (274.32, yPn)); s.label('CAM_GPIO', 274.32, yPn, 0)
    # ALSB
    xA = 302.26
    R5 = s.place('Device:R', 'R5', '10k', xA, 55.88, 0, FP['R'], LCSC['R10k'], 'ALSB pull-up: I2C addr 0x21 (write 0x42) = overlay default')
    s.power('+3V3', xA, R5.pin(1)[1] - 2.54); s.wire((xA, R5.pin(1)[1] - 2.54), R5.pin(1))
    s.wire(R5.pin(2), (xA, yRn)); s.wire((xA, yRn), (309.88, yRn)); s.label('ALSB', 309.88, yRn, 0)
    JP1 = s.place('Jumper:SolderJumper_2_Open', 'JP1', 'ALSB>GND', xA, 68.58, 90, FP['JP'], '', 'Close for I2C addr 0x20 (needs dtoverlay addr=0x20)', ref_pos=(305.0, 67.31, 'left'), val_pos=(305.0, 69.85, 'left'))
    s.wire((xA, yRn), JP1.pin(2)); s.power('GND', *JP1.pin(1))
    # I2C pull-ups
    R6 = s.place('Device:R', 'R6', '4.7k', 325.12, 83.82, 0, FP['R'], LCSC['R4k7'], 'SDA pull-up (in parallel with Pi pull-up)')
    R7 = s.place('Device:R', 'R7', '4.7k', 345.44, 83.82, 0, FP['R'], LCSC['R4k7'], 'SCL pull-up (in parallel with Pi pull-up)')
    yT = R6.pin(1)[1] - 2.54
    s.path(R6.pin(1), (325.12, yT), (335.28, yT)); s.path((335.28, yT), (345.44, yT), R7.pin(1)); s.power('+3V3', 335.28, yT)
    s.path(R6.pin(2), (325.12, yPn), (332.74, yPn)); s.label('SDA', 332.74, yPn, 0)
    s.path(R7.pin(2), (345.44, yPn), (353.06, yPn)); s.label('SCL', 353.06, yPn, 0)
    s.text('Reset / power-down / address\nOverlay adv728x-m sets no reset/powerdown GPIO and does not enable cam1_reg,\nso RESET and PWRDWN are pulled up on board; JP2/JP3 optionally route CAM_GPIO (Pi pin 11).\nALSB=1 -> 7-bit addr 0x21 as in adv7282m-overlay.dts (reg = <0x21>). Close JP1 for 0x20.', 240.03, 44.45, 1.27)

    # ---------------- power: 3V3 in, two LDOs ----------------
    y3 = 198.12
    s.power('+3V3', 55.88, y3); s.flag(63.5, y3); s.path((55.88, y3), (63.5, y3), (73.66, y3))
    TP1 = s.place('Connector:TestPoint', 'TP1', '3V3', 68.58, 193.04, 0, FP['TP'], '', '3.3 V test point', ref_pos=(69.85, 190.5, 'left'), val_pos=(69.85, 193.04, 'left'))
    s.wire(TP1.pin(1), (68.58, y3))
    C6 = s.place('Device:C', 'C6', '10uF', 73.66, 201.93, 0, FP['C0603'], LCSC['C10u'], '3V3 input bulk, 10 V X5R')
    s.power('GND', *C6.pin(2))
    U2 = s.place('Regulator_Linear:AP2112K-1.8', 'U2', 'AP2112K-1.8', 96.52, 200.66, 0, FP['SOT235'], LCSC['LDO'], 'LDO 1.8 V digital (DVDD, MVDD), 600 mA, low noise',
                 extra={'MPN': 'AP2112K-1.8TRG1'}, ref_pos=(88.9, 189.23, 'left'), val_pos=(88.9, 191.77, 'left'))
    s.wire((73.66, y3), U2.pin(1))
    s.path(U2.pin(3), (86.36, U2.pin(3)[1]), (86.36, y3))
    s.power('GND', *U2.pin(2))
    C7 = s.place('Device:C', 'C7', '1uF', 109.22, 201.93, 0, FP['C0603'], LCSC['C1u'], 'LDO output, X7R')
    C8 = s.place('Device:C', 'C8', '10uF', 116.84, 201.93, 0, FP['C0603'], LCSC['C10u'], 'LDO output bulk')
    s.path(U2.pin(5), C7.pin(1)); s.path(C7.pin(1), C8.pin(1)); s.path(C8.pin(1), (124.46, y3))
    s.path(C7.pin(2), C8.pin(2)); s.power('GND', *C8.pin(2))
    s.power('+1V8D', 124.46, y3, lib='adv-parts')
    TP2 = s.place('Connector:TestPoint', 'TP2', '1V8D', 121.92, 193.04, 0, FP['TP'], '', '1.8 V digital test point', ref_pos=(123.19, 190.5, 'left'), val_pos=(123.19, 193.04, 'left'))
    s.wire(TP2.pin(1), (121.92, y3))

    y8 = 228.6
    s.power('+3V3', 73.66, y8)
    U3 = s.place('Regulator_Linear:AP2112K-1.8', 'U3', 'AP2112K-1.8', 96.52, 231.14, 0, FP['SOT235'], LCSC['LDO'], 'LDO 1.8 V analog (AVDD, PVDD), 600 mA, low noise',
                 extra={'MPN': 'AP2112K-1.8TRG1'}, ref_pos=(88.9, 219.71, 'left'), val_pos=(88.9, 222.25, 'left'))
    s.wire((73.66, y8), U3.pin(1))
    s.path(U3.pin(3), (86.36, U3.pin(3)[1]), (86.36, y8))
    s.power('GND', *U3.pin(2))
    C9 = s.place('Device:C', 'C9', '1uF', 109.22, 232.41, 0, FP['C0603'], LCSC['C1u'], 'LDO output, X7R')
    C10 = s.place('Device:C', 'C10', '10uF', 116.84, 232.41, 0, FP['C0603'], LCSC['C10u'], 'LDO output bulk')
    s.path(U3.pin(5), C9.pin(1)); s.path(C9.pin(1), C10.pin(1)); s.path(C10.pin(1), (124.46, y8))
    s.path(C9.pin(2), C10.pin(2)); s.power('GND', *C10.pin(2))
    s.power('+1V8A', 124.46, y8, lib='adv-parts')
    TP8 = s.place('Connector:TestPoint', 'TP8', '1V8A', 121.92, 223.52, 0, FP['TP'], '', '1.8 V analog test point', ref_pos=(123.19, 220.98, 'left'), val_pos=(123.19, 223.52, 'left'))
    s.wire(TP8.pin(1), (121.92, y8))
    FB1 = s.place('Device:FerriteBead', 'FB1', '120R@100MHz', 139.7, y8, 90, FP['L'], LCSC['FB'], 'BLM18PG121SN1D, PVDD filter', extra={'MPN': 'BLM18PG121SN1D'}, ref_pos=(139.7, y8 - 3.81, None), val_pos=(139.7, y8 + 3.81, None))
    s.wire((124.46, y8), FB1.pin(1))
    C11 = s.place('Device:C', 'C11', '10uF', 149.86, 232.41, 0, FP['C0603'], LCSC['C10u'], 'PVDD bulk after ferrite')
    s.path(FB1.pin(2), C11.pin(1)); s.path(C11.pin(1), (157.48, y8), (167.64, y8)); s.power('GND', *C11.pin(2))
    s.power('PVDD', 157.48, y8, lib='adv-parts'); s.flag(167.64, y8)
    s.text('Power: 3.3 V from Pi camera connector (pin 15). Two AP2112K-1.8 LDOs:\nU2 -> DVDD+MVDD (~84 mA typ), U3 -> AVDD (~47 mA) and PVDD via FB1 (~12 mA).\nUG-637 recommends separate regulated supplies for the analog/PLL group.', 55.88, 254.0, 1.27)

    # ---------------- decoupling at U1 pins ----------------
    cn = 12
    for i, (rail, lib, nm) in enumerate([('+3V3', 'power', 'DVDDIO'), ('+1V8D', 'adv-parts', 'DVDD'), ('+1V8D', 'adv-parts', 'MVDD'), ('+1V8A', 'adv-parts', 'AVDD'), ('PVDD', 'adv-parts', 'PVDD')]):
        x0 = 185.42 + i * 20.32
        yc = 201.93
        Ca = s.place('Device:C', f'C{cn+1}', '100nF', x0, yc, 0, FP['C0402'], LCSC['C100n_0402'], f'{nm} decoupling, at pin')
        Cb = s.place('Device:C', f'C{cn+2}', '10nF', x0 + 7.62, yc, 0, FP['C0402'], LCSC['C10n_0402'], f'{nm} decoupling, at pin')
        cn += 2
        s.path(Ca.pin(1), (x0 + 3.81, Ca.pin(1)[1])); s.path((x0 + 3.81, Ca.pin(1)[1]), Cb.pin(1))
        s.wire((x0 + 3.81, Ca.pin(1)[1]), (x0 + 3.81, Ca.pin(1)[1] - 5.08)); s.power(rail, x0 + 3.81, Ca.pin(1)[1] - 5.08, lib=lib)
        s.path(Ca.pin(2), (x0 + 3.81, Ca.pin(2)[1])); s.path((x0 + 3.81, Ca.pin(2)[1]), Cb.pin(2))
        s.power('GND', x0 + 3.81, Ca.pin(2)[1])
        s.text(nm, x0 - 1.27, 212.09, 1.27)
    s.text('Decoupling per UG-637: 100 nF + 10 nF at each supply pin, same side as U1, via under the 100 nF pad.', 185.42, 218.44, 1.27)

    # ---------------- test points / flags ----------------
    TP3 = s.place('Connector:TestPoint', 'TP3', 'SDA', 276.86, 241.3, 0, FP['TP'], '', 'SDA test point', ref_pos=(278.13, 238.76, 'left'), val_pos=(278.13, 241.3, 'left'))
    s.path(TP3.pin(1), (276.86, 243.84), (281.94, 243.84)); s.label('SDA', 281.94, 243.84, 0)
    TP4 = s.place('Connector:TestPoint', 'TP4', 'SCL', 297.18, 241.3, 0, FP['TP'], '', 'SCL test point', ref_pos=(298.45, 238.76, 'left'), val_pos=(298.45, 241.3, 'left'))
    s.path(TP4.pin(1), (297.18, 243.84), (302.26, 243.84)); s.label('SCL', 302.26, 243.84, 0)
    TP6 = s.place('Connector:TestPoint', 'TP6', 'GND', 317.5, 241.3, 0, FP['TP'], '', 'GND test point', ref_pos=(318.77, 238.76, 'left'), val_pos=(318.77, 241.3, 'left'))
    s.path(TP6.pin(1), (317.5, 246.38)); s.power('GND', 317.5, 246.38)
    s.flag(322.58, 243.84); s.wire((317.5, 243.84), (322.58, 243.84))
    H1 = s.place('Mechanical:MountingHole', 'H1', 'M2', 340.36, 241.3, 0, FP['HOLE'], '', 'Mounting hole', in_bom=False)
    H2 = s.place('Mechanical:MountingHole', 'H2', 'M2', 355.6, 241.3, 0, FP['HOLE'], '', 'Mounting hole', in_bom=False)
    s.text('Analog input (UG-637 Fig. 8): 75 R termination (R1+R2), gain 0.7, AC coupled by C1.\nJ2 (RCA) and J3 (SMA) are alternative footprints for the same input; fit one.', 55.88, 140.97, 1.27)
    s.text('Crystal 28.63636 MHz, fundamental, CL 20 pF:\nC = 2(CL-Cs)-Cpg = 2(20-3)-4 = 30 pF -> 27 pF (AN-1260)', 55.88, 165.1, 1.27)
    s.text('MIPI CSI-2, 1 data lane + clock (216 Mbps interlaced / 432 Mbps I2P).\nRoute as 100 R differential (2 x 50 R loosely coupled per UG-637), no vias, GND plane under.', 236.22, 104.14, 1.27)
    return s

if __name__ == '__main__':
    s = build()
    root = s.emit('CVBS to MIPI CSI-2 video decoder (ADV7280-M) for Raspberry Pi 4', 'A', '2026-09-20')
    out = os.path.join(HW, PROJECT + '.kicad_sch')
    with open(out, 'w', encoding='utf-8') as f:
        f.write(dump(root) + '\n')
    print('wrote', out, 'parts', len(s.parts), 'wires', len(s.wires), 'junctions', len(s.junctions()))
