"""Generate custom footprints into hw/lib/adv-parts.pretty (KiCad 9 format)."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from kisexp import new_uuid

OUT = os.path.join(os.path.dirname(__file__), '..', 'hw', 'lib', 'adv-parts.pretty')

def prop(name, val, x, y, layer, hide=False, size=1.0):
    h = ' (hide yes)' if hide else ''
    return f'''\t(property "{name}" "{val}" (at {x} {y} 0) (layer "{layer}"){h} (uuid "{new_uuid()}")
\t\t(effects (font (size {size} {size}) (thickness 0.15)))
\t)'''

def line(x1, y1, x2, y2, layer, w=0.12):
    return f'\t(fp_line (start {x1:.3f} {y1:.3f}) (end {x2:.3f} {y2:.3f}) (stroke (width {w}) (type solid)) (layer "{layer}") (uuid "{new_uuid()}"))'

def rect(x1, y1, x2, y2, layer, w=0.12):
    return f'\t(fp_rect (start {x1:.3f} {y1:.3f}) (end {x2:.3f} {y2:.3f}) (stroke (width {w}) (type solid)) (fill no) (layer "{layer}") (uuid "{new_uuid()}"))'

def poly(pts, layer, w=0.12, fill=True):
    p = ' '.join(f'(xy {x:.3f} {y:.3f})' for x, y in pts)
    return f'\t(fp_poly (pts {p}) (stroke (width {w}) (type solid)) (fill {"yes" if fill else "no"}) (layer "{layer}") (uuid "{new_uuid()}"))'

def circle(cx, cy, r, layer, w=0.12):
    return f'\t(fp_circle (center {cx:.3f} {cy:.3f}) (end {cx + r:.3f} {cy:.3f}) (stroke (width {w}) (type solid)) (fill no) (layer "{layer}") (uuid "{new_uuid()}"))'

def pad_smd(num, x, y, sx, sy, shape='roundrect', rr=0.25, paste=True):
    layers = '"F.Cu" "F.Paste" "F.Mask"' if paste else '"F.Cu" "F.Mask"'
    extra = f' (roundrect_rratio {rr})' if shape == 'roundrect' else ''
    return f'\t(pad "{num}" smd {shape} (at {x:.3f} {y:.3f}) (size {sx} {sy}) (layers {layers}){extra} (uuid "{new_uuid()}"))'

def pad_tht(num, x, y, sx, sy, dx, dy=None, shape='oval'):
    drill = f'(drill oval {dx} {dy})' if dy else f'(drill {dx})'
    return f'\t(pad "{num}" thru_hole {shape} (at {x:.3f} {y:.3f}) (size {sx} {sy}) {drill} (layers "*.Cu" "*.Mask") (remove_unused_layers no) (uuid "{new_uuid()}"))'

def footprint(name, descr, tags, attr, body, ref_at, val_at):
    return f'''(footprint "{name}"
\t(version 20241229)
\t(generator "pcbnew")
\t(generator_version "9.0")
\t(layer "F.Cu")
\t(descr "{descr}")
\t(tags "{tags}")
{prop("Reference", "REF**", ref_at[0], ref_at[1], "F.SilkS")}
{prop("Value", name, val_at[0], val_at[1], "F.Fab")}
{prop("Datasheet", "", 0, 0, "F.Fab", True)}
{prop("Description", descr, 0, 0, "F.Fab", True)}
\t(attr {attr})
''' + '\n'.join(body) + f'''
\t(fp_text user "${{REFERENCE}}" (at 0 0 0) (layer "F.Fab") (uuid "{new_uuid()}")
\t\t(effects (font (size 0.8 0.8) (thickness 0.12)))
\t)
)
'''

def sfw15r():
    """Amphenol SFW15R-1STE1LF: 15 pos, 1.0 mm pitch, bottom contact, right angle SMT.
    Land pattern from Amphenol drawing 10172241 sheet 5: contact pads 0.6x2.0 pitch 1.0; mounting pads 0.7x4.2,
    centres at 1x(n+1)=16.0 apart, top edge 1.0 below contact-pad top edge; body 20.8 wide, 5.5 deep,
    rear body edge 1.5 below contact-pad top edge. Origin = body centre. Cable enters from +Y (bottom in KiCad view).
    Pin 1 on the left when viewed from top with the cable toward the viewer."""
    n = 15
    body_w, body_d = 20.8, 5.5
    pad_top = 0.0                      # contact pad top edge (rear)
    body_y0 = pad_top + 1.5            # body rear edge
    body_y1 = body_y0 + body_d         # body front edge (cable side)
    cy = (body_y0 + body_y1) / 2
    def Y(y): return y - cy
    b = []
    for i in range(n):
        x = (i - (n - 1) / 2) * 1.0
        b.append(pad_smd(str(i + 1), x, Y(pad_top + 1.0), 0.6, 2.0))
    mp_x = (n + 1) / 2 * 1.0           # 8.0
    for x in (-mp_x, mp_x):
        b.append(pad_smd('', x, Y(pad_top + 1.0 + 2.1), 0.7, 4.2, rr=0.2))
    hw = body_w / 2
    # Fab outline
    b.append(rect(-hw, Y(body_y0), hw, Y(body_y1), 'F.Fab', 0.1))
    b.append(poly([(-hw + 0.0, Y(body_y0)), (-hw + 1.2, Y(body_y0)), (-hw + 0.6, Y(body_y0) + 1.0)], 'F.Fab', 0.1))
    # Silk: side edges and front edge, keep clear of pads
    b.append(line(-hw - 0.11, Y(body_y0 + 2.6 + 1.0), -hw - 0.11, Y(body_y1) - 0.4, 'F.SilkS'))
    b.append(line(hw + 0.11, Y(body_y0 + 2.6 + 1.0), hw + 0.11, Y(body_y1) - 0.4, 'F.SilkS'))
    # pin 1 marker: triangle above pad 1
    x1 = -(n - 1) / 2
    b.append(poly([(x1 - 0.4, Y(pad_top) - 0.5), (x1 + 0.4, Y(pad_top) - 0.5), (x1, Y(pad_top) - 0.1)], 'F.SilkS', 0.1))
    # Courtyard
    b.append(rect(-mp_x - 0.35 - 0.5, Y(pad_top) - 0.5, mp_x + 0.35 + 0.5, Y(body_y1) + 0.5, 'F.CrtYd', 0.05))
    return footprint('Amphenol_SFW15R-1STE1LF',
                     'Amphenol/FCI SFW15R-1STE1LF, 15 pos 1.00 mm FFC/FPC ZIF, bottom contact, right angle SMT (same part as Raspberry Pi Camera Module v2.1), drawing 10172241',
                     'FFC FPC 1.0mm 15 bottom-contact Raspberry Pi camera', 'smd', b, (0, Y(pad_top) - 1.6), (0, Y(body_y1) + 1.6))

def rca_psg01546():
    """Multicomp Pro PSG01546 RCA/phono jack, 3 pin, right angle, PCB mount (Farnell datasheet 3029582).
    Pin hole layout (bottom view): pin1 slot 1.3x2.5 at 5.0 from body front edge; two ground tabs 2.5x1.0 at 8.8,
    +/-5.0 apart. Body 9.7 wide, 12.0 deep behind the front edge; barrel dia 8.3 protrudes 9.0 in front.
    Origin = pin 1 (signal). Barrel points to -X."""
    b = []
    b.append(pad_tht('1', 0, 0, 2.3, 3.5, 1.3, 2.5))          # signal: slot 1.3 x 2.5 (long axis Y)
    b.append(pad_tht('2', 3.8, -5.0, 3.5, 2.0, 2.5, 1.0))     # ground tab
    b.append(pad_tht('2', 3.8, 5.0, 3.5, 2.0, 2.5, 1.0))      # ground tab
    fx0, fx1 = -5.0, 7.0                                     # body front edge x=-5, rear edge x=+7 (12.0 deep)
    hw = 9.7 / 2
    b.append(rect(fx0, -hw, fx1, hw, 'F.Fab', 0.1))
    b.append(rect(fx0 - 9.0, -8.3 / 2, fx0, 8.3 / 2, 'F.Fab', 0.1))   # barrel
    for sy in (-hw - 0.11, hw + 0.11):      # side lines, interrupted around the ground tabs at x=3.8
        b.append(line(fx0 + 0.4, sy, 1.6, sy, 'F.SilkS'))
        b.append(line(6.0, sy, fx1 + 0.11, sy, 'F.SilkS'))
    b.append(line(fx1 + 0.11, -hw - 0.11, fx1 + 0.11, hw + 0.11, 'F.SilkS'))
    b.append(rect(fx0 - 9.0 - 0.5, -hw - 0.5, fx1 + 0.5, hw + 0.5, 'F.CrtYd', 0.05))
    return footprint('RCA_Multicomp_PSG01546_Horizontal',
                     'RCA/phono jack, 3 pin, right angle, PCB mount, Multicomp Pro PSG01546 (generic 9.7 mm wide RCA jack pattern), Farnell datasheet 3029582',
                     'RCA phono CVBS jack right angle', 'through_hole', b, (1, -7.0), (1, 7.0))

def ti_x1son_dpy():
    """TI DPY0002A (X1SON, 1.0 x 0.6 mm), land pattern: 2 pads 0.3 x 0.5, 0.7 mm apart (TPD1E10B06 datasheet)."""
    b = []
    b.append(pad_smd('1', -0.35, 0, 0.3, 0.5, rr=0.25))
    b.append(pad_smd('2', 0.35, 0, 0.3, 0.5, rr=0.25))
    b.append(rect(-0.5, -0.3, 0.5, 0.3, 'F.Fab', 0.1))
    b.append(line(-0.6, -0.45, 0.6, -0.45, 'F.SilkS'))
    b.append(line(-0.6, 0.45, 0.6, 0.45, 'F.SilkS'))
    b.append(rect(-0.75, -0.55, 0.75, 0.55, 'F.CrtYd', 0.05))
    return footprint('TI_X1SON-2_DPY0002A_1.0x0.6mm',
                     'TI X1SON-2 DPY0002A 1.0x0.6 mm (TPD1E10B06DPY), land pattern per datasheet SLLSEB1G',
                     'X1SON DFN1006 ESD TVS', 'smd', b, (0, -1.3), (0, 1.3))

if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for fn, gen in [('Amphenol_SFW15R-1STE1LF', sfw15r), ('RCA_Multicomp_PSG01546_Horizontal', rca_psg01546), ('TI_X1SON-2_DPY0002A_1.0x0.6mm', ti_x1son_dpy)]:
        with open(os.path.join(OUT, fn + '.kicad_mod'), 'w') as f:
            f.write(gen())
        print('wrote', fn)
