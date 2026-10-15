"""Generate hw/adv7280m-csi2.kicad_pcb from the schematic netlist (kicad XML) with a scripted placement.
Stage 2: outline, layers, footprints, nets, placement. Routing is added by tools/route_pcb.py (stage 3)."""
import os, sys, json, math, re, subprocess
import xml.etree.ElementTree as ET
import pcbnew
from pcbnew import VECTOR2I, FromMM

HW = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'hw'))
KFP = os.environ.get('KICAD_FOOTPRINT_DIR') or ('C:/Program Files/KiCad/10.0/share/kicad/footprints' if os.name == 'nt' else '/usr/share/kicad/footprints')
PCB = os.path.join(HW, 'adv7280m-csi2.kicad_pcb')
NET = os.path.join(HW, 'adv7280m-csi2.xml')

# rev C: one solid board (decoder y < 32, THS7314 buffer y > 32), no breakaway slot
BOARD_W, BOARD_H, CORNER_R = 42.0, 55.0, 1.5
# breakaway slot between the decoder (y < SLOT_Y0) and the THS7314 buffer (y > SLOT_Y1); two tabs, CVBS_IN crosses the right one
SLOT_Y0, SLOT_Y1 = 32.0, 34.0
TABS = [(7.0, 9.5), (31.15, 33.65)]

# ref: (x, y, rot)  -- all top side
PLACE = {
    'U1': (20.0, 10.5, 0),
    'J1': (3.0, 15.0, 270),        # FFC, cable exits -X (left edge), pin 1 at top
    # analog input network (pad1/pad2 orientation chosen so that nets flow left->right)
    # AIN1 network up-right of U1: C1 at AIN1, node -> R1 (series, CVBS_IN east) and R2 (shunt) under the node, TP7 on the node
    'C1': (26.1, 11.6, 180), 'R1': (29.85, 11.6, 180), 'R2': (28.3, 13.2, 180), 'TP7': (30.9, 15.4, 0),
    # VREF, AVDD decoupling
    'C4': (24.0, 11.0, 90), 'C20': (24.9, 9.0, 0), 'C19': (26.9, 9.0, 0),       # AVDD: 10 nF (C20) next to the pin, 100 nF behind
    # crystal (pad 2 = XTALP on the left), load caps below the pads
    # crystal turned 90 deg: XTALN pad (oscillator input) right under pin 15, XTALP pad below it; C3 right of Y1, C2 under Y1
    'Y1': (22.5, 16.9, 270), 'C2': (22.5, 21.7, 90), 'C3': (25.5, 17.0, 90),
    # MVDD / PVDD decoupling (power pad on top, towards U1)
    'C18': (19.5, 17.3, 270), 'C17': (18.3, 17.3, 270),   # MVDD caps below the CLK pair: 10 nF (C18) first from the pin
    'C22': (25.2, 14.3, 270), 'C21': (26.25, 14.3, 270),   # PVDD: pin 16 -> 10 nF (C22) -> 100 nF (C21), right of Y1  # PVDD at the U1 corner: pin 16 -> 10 nF (C22) -> 100 nF (C21), PVDD pads on top
    # DVDDIO / DVDD decoupling (left of U1, power pad towards the chip)
    'C13': (15.4, 7.9, 180), 'C14': (15.4, 9.2, 180), 'C16': (15.4, 10.5, 180), 'C15': (15.4, 11.8, 180),   # 10 nF (C14, C16) next to pins 2 / 3
    # control (top): pull-ups with 3V3 pad on top (via to In2 3V3 band), signal pad towards U1
    'R4': (16.3, 5.5, 270), 'R5': (21.8, 5.5, 270), 'R3': (23.3, 5.5, 270), 'C5': (24.8, 5.5, 270),
    'R6': (26.3, 5.5, 270), 'R7': (27.8, 5.5, 270), 'TP5': (28.2, 2.7, 0),
    'JP3': (13.5, 2.7, 180), 'JP1': (19.6, 2.7, 0), 'JP2': (24.6, 2.7, 180),
    # power (bottom-left)
    'U2': (11.5, 22.0, 0), 'C6': (7.6, 24.9, 0), 'C7': (15.3, 20.8, 0), 'C8': (15.3, 22.6, 0), 'TP1': (11.4, 18.2, 0), 'TP2': (18.6, 21.7, 0),
    'U3': (11.5, 27.8, 0), 'C9': (15.3, 25.8, 0), 'C10': (15.3, 27.6, 0), 'FB1': (18.4, 25.8, 0), 'C11': (21.4, 25.8, 0), 'TP8': (18.4, 28.3, 0),
    'TP3': (30.4, 8.4, 0), 'TP4': (30.4, 5.6, 0),      # I2C test points next to the pull-ups R6/R7 (rev C: no I2C track under Y1)
    'TP6': (36.5, 22.0, 0),
    'C29': (11.0, 24.3, 0), 'C30': (11.0, 25.5, 0),     # LDO input caps: pad 1 (+3V3) next to U2 / U3 VIN, own GND via each
    'H1': (8.0, 3.2, 0), 'H2': (39.5, 27.5, 0),   # decoder section: H1 / H2 diagonal
    # ---- THS7314 buffer section (y 32..55) ----
    'H3': (3.2, 51.8, 0), 'H4': (39.5, 37.0, 0),    # buffer section: H3 bottom-left / H4 top-right (diagonal)
    # solder pads for the coax decoder <-> buffer when split (pin 1 on the CVBS_IN track, pin 2 GND to the right), D1 ESD at J2
    'J2': (32.4, 28.5, 90), 'D1': (33.5, 25.9, 0), 'J8': (32.4, 35.8, 90),
    'J5': (6.7, 37.9, 0),                                   # solder pads: CVBS input coax, pin 1 (signal) on top, pin 2 (GND) below
    'D3': (9.7, 40.0, 270), 'R8': (11.5, 40.0, 270),         # ESD + 75 R termination, signal pads on top, GND pads at the bottom
    'C23': (14.6, 41.0, 0), 'C24': (14.6, 43.0, 0), 'C25': (14.6, 45.0, 0),
    'U5': (21.2, 44.0, 0),                                  # SOIC-8, inputs left, outputs right, VS+ pin 4 bottom-left
    'TP9': (24.3, 53.4, 0), 'C26': (20.0, 47.4, 180),   # C26 pad 1 (VS_BUF) on the right
    'J4': (3.5, 45.35, 90),                                 # solder pads 5 V: pin 1 = +5 V (left), pin 2 = GND (right)
    'D2': (15.5, 49.6, 180), 'C28': (20.3, 50.6, 90), 'FB2': (23.0, 49.0, 0), 'C27': (26.0, 50.6, 90),
    'R9': (27.5, 40.4, 0), 'R10': (27.5, 43.4, 0), 'R11': (27.5, 45.6, 0),
    'J7': (29.8, 48.3, 90), 'J6': (36.9, 48.3, 90),          # solder pads for the output coaxes: J7 GX16 (CH3) left, J6 TV (CH2) right; pin 1 = signal (left)
}

def mm(x): return FromMM(x)
def P(x, y): return VECTOR2I(mm(x), mm(y))

# reference designators printed on silk (offset of the text from the part, mm); the rest stay on F.Fab
SILK_REF = {'JP1': (0, -1.55), 'JP2': (0, -1.55), 'JP3': (0, -1.55), 'TP1': (-2.0, 0), 'TP2': (2.0, 0), 'TP3': (2.0, 0), 'TP4': (2.0, 0),
            'TP5': (2.0, 0), 'TP6': (2.0, 0), 'TP7': (2.0, 0), 'TP8': (0, 1.55), 'TP9': (2.0, 0)}


def load_fp(libid):
    lib, name = libid.split(':')
    path = os.path.join(HW, 'lib', 'adv-parts.pretty') if lib == 'adv-parts' else os.path.join(KFP, lib + '.pretty')
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None: raise RuntimeError('footprint not found: ' + libid)
    return fp

def add_line(board, a, b, layer, w=0.1):
    s = pcbnew.PCB_SHAPE(board); s.SetShape(pcbnew.SHAPE_T_SEGMENT); s.SetStart(P(*a)); s.SetEnd(P(*b))
    s.SetLayer(layer); s.SetWidth(mm(w)); board.Add(s); return s

def add_arc(board, start, mid, end, layer, w=0.1):
    s = pcbnew.PCB_SHAPE(board); s.SetShape(pcbnew.SHAPE_T_ARC); s.SetArcGeometry(P(*start), P(*mid), P(*end))
    s.SetLayer(layer); s.SetWidth(mm(w)); board.Add(s); return s

def add_text(board, txt, x, y, layer, size=1.0, thick=0.15, rot=0, mirror=False):
    t = pcbnew.PCB_TEXT(board); t.SetText(txt); t.SetPosition(P(x, y)); t.SetLayer(layer)
    t.SetTextSize(VECTOR2I(mm(size), mm(size))); t.SetTextThickness(mm(thick)); t.SetTextAngleDegrees(rot)
    if mirror: t.SetMirrored(True)
    board.Add(t); return t

def outline(board):
    W, H, r = BOARD_W, BOARD_H, CORNER_R
    L = pcbnew.Edge_Cuts
    add_line(board, (r, 0), (W - r, 0), L)
    add_line(board, (W - r, H), (r, H), L)
    for x in (0, W):   # side edges are interrupted by the slot
        add_line(board, (x, r), (x, SLOT_Y0), L); add_line(board, (x, SLOT_Y1), (x, H - r), L)
    xs = [0.0] + [v for t in TABS for v in t] + [W]
    for a, b in zip(xs[0::2], xs[1::2]):
        add_line(board, (a, SLOT_Y0), (b, SLOT_Y0), L); add_line(board, (a, SLOT_Y1), (b, SLOT_Y1), L)
    for t0, t1 in TABS:
        add_line(board, (t0, SLOT_Y0), (t0, SLOT_Y1), L); add_line(board, (t1, SLOT_Y0), (t1, SLOT_Y1), L)
    k = r * (1 - math.sqrt(0.5))
    add_arc(board, (W - r, 0), (W - k, k), (W, r), L)
    add_arc(board, (W, H - r), (W - k, H - k), (W - r, H), L)
    add_arc(board, (r, H), (k, H - k), (0, H - r), L)
    add_arc(board, (0, r), (k, k), (r, 0), L)

def build():
    board = pcbnew.BOARD()
    bds = board.GetDesignSettings()
    bds.SetCopperLayerCount(4)
    board.SetLayerName(pcbnew.In1_Cu, 'GND'); board.SetLayerName(pcbnew.In2_Cu, 'PWR')
    bds.m_MinClearance = mm(0.127); bds.m_TrackMinWidth = mm(0.127); bds.m_ViasMinSize = mm(0.45); bds.m_MinThroughDrill = mm(0.2)
    bds.m_CopperEdgeClearance = mm(0.3); bds.m_SolderMaskMinWidth = mm(0.0); bds.m_HoleClearance = mm(0.2); bds.m_HoleToHoleMin = mm(0.5)
    bds.m_SilkClearance = mm(0.0)
    outline(board)

    root = ET.parse(NET).getroot()
    comps = {}
    for c in root.find('components'):
        ref = c.get('ref')
        fields = {f.get('name'): (f.text or '') for f in (c.find('fields') or [])}
        props = {p.get('name') for p in c.findall('property')}
        comps[ref] = dict(fp=c.findtext('footprint'), value=c.findtext('value'), tstamp=c.findtext('tstamps'), fields=fields, props=props)
    nets = {}
    for n in root.find('nets'):
        nets[n.get('name')] = [(nd.get('ref'), nd.get('pin')) for nd in n.findall('node')]
    netobj = {}
    for name in nets:
        ni = pcbnew.NETINFO_ITEM(board, name); board.Add(ni); netobj[name] = ni
    fps = {}
    for ref, c in comps.items():
        if ref not in PLACE: raise RuntimeError('no placement for ' + ref)
        fp = load_fp(c['fp'])
        fp.SetFPIDAsString(c['fp'])
        fp.SetReference(ref); fp.SetValue(c['value'])
        # match symbol BOM/board flags
        attrs = fp.GetAttributes()
        attrs &= ~(pcbnew.FP_EXCLUDE_FROM_BOM | pcbnew.FP_EXCLUDE_FROM_POS_FILES | pcbnew.FP_BOARD_ONLY)
        if 'exclude_from_bom' in c['props']: attrs |= pcbnew.FP_EXCLUDE_FROM_BOM
        if c['fields'].get('Assemble') == 'no': attrs |= pcbnew.FP_EXCLUDE_FROM_POS_FILES
        fp.SetAttributes(attrs)
        fp.SetPath(pcbnew.KIID_PATH('/' + c['tstamp']))
        for k, v in c['fields'].items():
            if k == 'Footprint': continue
            fp.SetField(k, v); fp.GetField(k).SetVisible(False)   # KiCad 10 DRC compares Datasheet/Description with the symbol too
        if c['fields'].get('Assemble') == 'no':
            pass  # populated by the owner; keep in BOM/CPL logic at stage 4
        x, y, rot = PLACE[ref]
        fp.SetPosition(P(x, y)); fp.SetOrientationDegrees(rot)
        if ref in ('J2', 'J4', 'J5', 'J6', 'J7', 'J8'):
            fp.Models().clear()   # bare solder holes for wires / coax: no pin-header model in renders / STEP
        if ref in ('U2', 'U3'):
            for pad in fp.Pads():
                if pad.GetNumber() == '2': pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)   # LDO GND: solid to the pour (heat)
        # reference text: small; on silk only for parts the user touches (jumpers, test points), elsewhere on F.Fab
        fp.Reference().SetTextSize(VECTOR2I(mm(0.6), mm(0.6))); fp.Reference().SetTextThickness(mm(0.1))
        fp.Reference().SetLayer(pcbnew.F_SilkS if ref in SILK_REF else pcbnew.F_Fab)
        if ref in SILK_REF:
            dx, dy = SILK_REF[ref]; fp.Reference().SetTextAngleDegrees(0); fp.Reference().SetPosition(P(x + dx, y + dy))
        fp.Value().SetVisible(False)
        board.Add(fp); fps[ref] = fp
    for name, nodes in nets.items():
        for ref, pin in nodes:
            fp = fps[ref]
            for pad in fp.Pads():
                if pad.GetNumber() == pin: pad.SetNet(netobj[name])
    # silkscreen texts
    add_text(board, 'CVBS>CSI-2 ADV7280-M', 21.0, 31.0, pcbnew.F_SilkS, 0.9, 0.15)
    add_text(board, 'TO RPi CAMERA', 10.5, 6.2, pcbnew.F_SilkS, 0.8, 0.12)
    add_text(board, 'rev C', 39.0, 31.0, pcbnew.F_SilkS, 0.8, 0.12)
    add_text(board, 'CVBS', 33.7, 30.75, pcbnew.F_SilkS, 0.7, 0.12); add_text(board, 'TO ADV', 33.7, 37.75, pcbnew.F_SilkS, 0.7, 0.12)
    add_text(board, 'СПРУТ-М', 21.0, 35.4, pcbnew.F_SilkS, 1.6, 0.25)   # product name, free band between the decoder and the buffer
    add_text(board, 'ADV7280M-CSI2 rev C  github inestvan/adv', 21.0, 16.0, pcbnew.B_SilkS, 1.0, 0.15, mirror=True)
    # buffer section silkscreen
    add_text(board, 'THS7314 BUFFER', 21.0, 39.2, pcbnew.F_SilkS, 0.9, 0.15)
    add_text(board, 'CVBS IN', 4.5, 35.2, pcbnew.F_SilkS, 0.7, 0.12)
    add_text(board, '+5V', 3.5, 42.6, pcbnew.F_SilkS, 0.7, 0.12); add_text(board, '-', 6.0, 42.6, pcbnew.F_SilkS, 0.9, 0.15)
    add_text(board, 'GX16', 30.8, 45.6, pcbnew.F_SilkS, 0.7, 0.12); add_text(board, 'TV', 37.9, 45.6, pcbnew.F_SilkS, 0.7, 0.12)
    add_text(board, 'THS7314 BUFFER rev C  SPRUT-2', 21.0, 44.5, pcbnew.B_SilkS, 0.9, 0.15, mirror=True)
    return board, fps, netobj

def stackup_text():
    return '''\t(stackup
\t\t(layer "F.SilkS" (type "Top Silk Screen"))
\t\t(layer "F.Paste" (type "Top Solder Paste"))
\t\t(layer "F.Mask" (type "Top Solder Mask") (thickness 0.01))
\t\t(layer "F.Cu" (type "copper") (thickness 0.035))
\t\t(layer "dielectric 1" (type "prepreg") (thickness 0.2104) (material "FR4 7628") (epsilon_r 4.4) (loss_tangent 0.02))
\t\t(layer "In1.Cu" (type "copper") (thickness 0.0152))
\t\t(layer "dielectric 2" (type "core") (thickness 1.065) (material "FR4") (epsilon_r 4.6) (loss_tangent 0.02))
\t\t(layer "In2.Cu" (type "copper") (thickness 0.0152))
\t\t(layer "dielectric 3" (type "prepreg") (thickness 0.2104) (material "FR4 7628") (epsilon_r 4.4) (loss_tangent 0.02))
\t\t(layer "B.Cu" (type "copper") (thickness 0.035))
\t\t(layer "B.Mask" (type "Bottom Solder Mask") (thickness 0.01))
\t\t(layer "B.Paste" (type "Bottom Solder Paste"))
\t\t(layer "B.SilkS" (type "Bottom Silk Screen"))
\t\t(copper_finish "None")
\t\t(dielectric_constraints no)
\t)
'''

def save(board):
    pcbnew.SaveBoard(PCB, board)
    t = open(PCB, encoding='utf-8').read()
    if '(stackup' not in t:
        t = t.replace('\t(setup\n', '\t(setup\n' + stackup_text(), 1)
        open(PCB, 'w', encoding='utf-8').write(t)

if __name__ == '__main__':
    board, fps, nets = build()
    save(board)
    print('saved', PCB, 'footprints', len(fps), 'nets', len(nets))
