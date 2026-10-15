#!/bin/sh
# Stage 4/5d: Gerber + drill (JLCPCB conventions), JLCPCB BOM/CPL, schematic PDF/BOM. Run after the board is final and DRC-clean.
# KICAD_CLI / PY override the tools (Windows: KICAD_CLI="/c/Program Files/KiCad/10.0/bin/kicad-cli.exe" PY=python sh tools/fab.sh).
set -e
K=${KICAD_CLI:-kicad-cli}; PY=${PY:-python3}
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
rm -rf fab/gerber fab/adv7280m-csi2-gerber.zip; mkdir -p fab/gerber
cd hw
"$K" sch export pdf -o ../docs/schematic.pdf adv7280m-csi2.kicad_sch >/dev/null 2>&1
"$K" sch export bom --fields "Reference,Value,Footprint,LCSC,MPN,Assemble,Description,\${QUANTITY}" --labels "Reference,Value,Footprint,LCSC,MPN,Assemble,Description,Qty" --group-by "Value,Footprint,LCSC" --exclude-dnp -o ../docs/bom.csv adv7280m-csi2.kicad_sch >/dev/null 2>&1
"$K" pcb export gerbers --layers "F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts" --no-x2 --no-netlist --subtract-soldermask --use-drill-file-origin -o ../fab/gerber/ adv7280m-csi2.kicad_pcb >/dev/null 2>&1
"$K" pcb export drill --format excellon --excellon-units mm --excellon-zeros-format decimal --drill-origin absolute --excellon-separate-th --generate-map --map-format gerberx2 -o ../fab/gerber/ adv7280m-csi2.kicad_pcb >/dev/null 2>&1
"$K" pcb export pos --format csv --units mm --side front --use-drill-file-origin -o ../fab/pos-kicad.csv adv7280m-csi2.kicad_pcb >/dev/null 2>&1
cd ../fab/gerber
# JLCPCB expects inner layers named In1_Cu.g2 / In2_Cu.g3 (KiCad names them after the layer names GND / PWR)
mv adv7280m-csi2-GND.g1 adv7280m-csi2-In1_Cu.g2; mv adv7280m-csi2-PWR.g2 adv7280m-csi2-In2_Cu.g3
sed -i 's/adv7280m-csi2-GND.g1/adv7280m-csi2-In1_Cu.g2/; s/adv7280m-csi2-PWR.g2/adv7280m-csi2-In2_Cu.g3/' adv7280m-csi2-job.gbrjob
"$PY" -c "import os, zipfile; z = zipfile.ZipFile('../adv7280m-csi2-gerber.zip', 'w', zipfile.ZIP_DEFLATED); [z.write(f) for f in sorted(os.listdir('.'))]; z.close()"
cd "$ROOT" && "$PY" tools/jlc_bom.py && rm -f fab/pos-kicad.csv
ls -la fab/adv7280m-csi2-gerber.zip; echo fab done
