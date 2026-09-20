"""Build fab/BOM_JLCPCB.csv and fab/CPL_JLCPCB.csv from docs/bom.csv and the kicad-cli position file (fab/pos-kicad.csv).
Parts with Assemble=no or without an LCSC code are left out (hand-soldered by the owner)."""
import csv, re, os
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
rows = list(csv.DictReader(open(os.path.join(ROOT, 'docs', 'bom.csv'), encoding='utf-8')))
out, skipped = [], []
for r in rows:
    exp = []
    for x in [x.strip() for x in r['Reference'].split(',')]:
        m = re.match(r'([A-Z]+)(\d+)-\1(\d+)$', x)
        exp += [f"{m.group(1)}{i}" for i in range(int(m.group(2)), int(m.group(3)) + 1)] if m else [x]
    if r['Assemble'] == 'no' or not r['LCSC']:
        skipped.append((','.join(exp), r['Value'], r['LCSC'] or '-')); continue
    out.append({'Comment': r['Value'], 'Designator': ','.join(exp), 'Footprint': r['Footprint'].split(':')[-1], 'LCSC Part #': r['LCSC']})
with open(os.path.join(ROOT, 'fab', 'BOM_JLCPCB.csv'), 'w', newline='', encoding='utf-8') as f:
    w = csv.DictWriter(f, fieldnames=['Comment', 'Designator', 'Footprint', 'LCSC Part #']); w.writeheader(); w.writerows(out)
asm = set()
for o in out: asm.update(o['Designator'].split(','))
print('BOM lines', len(out), 'assembled refs', len(asm))
print('not assembled by JLCPCB:', skipped)
pos = list(csv.DictReader(open(os.path.join(ROOT, 'fab', 'pos-kicad.csv'), encoding='utf-8')))
cpl = []
for p in pos:
    if p['Ref'] not in asm: continue
    cpl.append({'Designator': p['Ref'], 'Mid X': f"{float(p['PosX']):.4f}mm", 'Mid Y': f"{float(p['PosY']):.4f}mm",
                'Layer': 'Top' if p['Side'] == 'top' else 'Bottom', 'Rotation': f"{float(p['Rot']) % 360:.0f}"})
with open(os.path.join(ROOT, 'fab', 'CPL_JLCPCB.csv'), 'w', newline='', encoding='utf-8') as f:
    w = csv.DictWriter(f, fieldnames=['Designator', 'Mid X', 'Mid Y', 'Layer', 'Rotation']); w.writeheader(); w.writerows(cpl)
print('CPL rows', len(cpl), 'missing', asm - set(c['Designator'] for c in cpl))
