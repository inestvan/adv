"""Generate hw/adv7280m-csi2.kicad_pcb from the schematic netlist (kicad XML) with a scripted placement.
Stage 2: outline, layers, footprints, nets, placement. Routing is added by tools/route_pcb.py (stage 3)."""
import os, sys, json, math, re, subprocess
import xml.etree.ElementTree as ET
import pcbnew
from pcbnew import VECTOR2I, FromMM

HW = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'hw'))
KFP = '/usr/share/kicad/footprints'
PCB = os.path.join(HW, 'adv7280m-csi2.kicad_pcb')
NET = os.path.join(HW, 'adv7280m-csi2.xml')

BOARD_W, BOARD_H, CORNER_R = 42.0, 32.0, 1.5

# ref: (x, y, rot)  -- all top side
PLACE = {
    'U1': (20.0, 10.5, 0),
    'J1': (3.0, 15.0, 270),        # FFC, cable exits -X (left edge), pin 1 at top
    'J2': (37.0, 8.0, 180),        # RCA, barrel out of the right edge
    'J3': (38.0, 22.0, 270),       # SMA, barrel out of the right edge
    # analog input network (pad1/pad2 orientation chosen so that nets flow left->right)
    'C1': (26.2, 12.9, 180), 'R2': (28.6, 15.0, 270), 'R1': (30.95, 14.9, 180), 'D1': (33.2, 16.4, 90), 'TP7': (30.4, 17.8, 0),
    # VREF, AVDD decoupling
    'C4': (24.0, 11.0, 90), 'C19': (24.9, 9.0, 0), 'C20': (26.9, 9.0, 0),
    # crystal (pad 2 = XTALP on the left), load caps below the pads
    'Y1': (24.8, 18.6, 180), 'C2': (22.95, 22.4, 90), 'C3': (26.65, 22.4, 90),
    # MVDD / PVDD decoupling (power pad on top, towards U1)
    'C17': (20.0, 16.0, 270), 'C18': (18.8, 16.0, 270), 'C21': (22.9, 14.4, 0), 'C22': (24.8, 14.4, 0),
    # DVDDIO / DVDD decoupling (left of U1, power pad towards the chip)
    'C14': (15.4, 7.9, 180), 'C13': (15.4, 9.2, 180), 'C15': (15.4, 10.5, 180), 'C16': (15.4, 11.8, 180),
    # control (top): pull-ups with 3V3 pad on top (via to In2 3V3 band), signal pad towards U1
    'R4': (16.3, 5.5, 270), 'R5': (21.8, 5.5, 270), 'R3': (23.3, 5.5, 270), 'C5': (24.8, 5.5, 270),
    'R6': (26.3, 5.5, 270), 'R7': (27.8, 5.5, 270), 'TP5': (28.2, 2.7, 0),
    'JP3': (13.5, 2.7, 180), 'JP1': (19.6, 2.7, 0), 'JP2': (24.6, 2.7, 180),
    # power (bottom-left)
    'U2': (11.5, 22.0, 0), 'C6': (7.6, 24.9, 0), 'C7': (15.3, 20.8, 0), 'C8': (15.3, 22.6, 0), 'TP1': (11.4, 18.2, 0), 'TP2': (18.6, 21.7, 0),
    'U3': (11.5, 27.0, 0), 'C9': (15.3, 25.8, 0), 'C10': (15.3, 27.6, 0), 'FB1': (18.4, 25.8, 0), 'C11': (21.4, 25.8, 0), 'TP8': (18.4, 28.3, 0),
    'TP3': (26.3, 27.2, 0), 'TP4': (28.9, 27.2, 0), 'TP6': (31.5, 29.6, 0),
    'H1': (8.0, 3.2, 0), 'H2': (35.5, 29.0, 0),
}

def mm(x): return FromMM(x)
def P(x, y): return VECTOR2I(mm(x), mm(y))

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
    add_line(board, (r, 0), (W - r, 0), L); add_line(board, (W, r), (W, H - r), L)
    add_line(board, (W - r, H), (r, H), L); add_line(board, (0, H - r), (0, r), L)
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
            if k in ('Footprint', 'Datasheet', 'Description') or not v: continue
            fp.SetField(k, v); fp.GetFieldByName(k).SetVisible(False)
        if c['fields'].get('Assemble') == 'no':
            pass  # populated by the owner; keep in BOM/CPL logic at stage 4
        x, y, rot = PLACE[ref]
        fp.SetPosition(P(x, y)); fp.SetOrientationDegrees(rot)
        # reference text: small, on silk
        fp.Reference().SetTextSize(VECTOR2I(mm(0.6), mm(0.6))); fp.Reference().SetTextThickness(mm(0.1))
        fp.Reference().SetLayer(pcbnew.F_Fab)
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
    add_text(board, 'CVBS IN', 37.2, 15.8, pcbnew.F_SilkS, 0.8, 0.12)
    add_text(board, 'rev A', 39.0, 31.0, pcbnew.F_SilkS, 0.8, 0.12)
    add_text(board, 'ADV7280M-CSI2 rev A  github inestvan/adv', 21.0, 16.0, pcbnew.B_SilkS, 1.0, 0.15, mirror=True)
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
