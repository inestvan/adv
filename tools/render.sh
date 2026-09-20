#!/bin/sh
# Regenerate docs/render-*.png and docs/layer-*.png from hw/adv7280m-csi2.kicad_pcb (needs kicad-cli, node + playwright for SVG->PNG).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT/hw"
S=${S:-/tmp/adv-render}; mkdir -p "$S"
kicad-cli pcb render --side top --quality high --background opaque --width 1600 --height 2000 --zoom 1.0 -o ../docs/render-top.png adv7280m-csi2.kicad_pcb >/dev/null 2>&1
kicad-cli pcb render --side bottom --quality high --background opaque --width 1600 --height 2000 --zoom 1.0 -o ../docs/render-bottom.png adv7280m-csi2.kicad_pcb >/dev/null 2>&1
for L in F.Cu B.Cu In1.Cu In2.Cu; do kicad-cli pcb export svg --layers "$L,Edge.Cuts" --page-size-mode 2 --exclude-drawing-sheet -o "$S/L_$L.svg" adv7280m-csi2.kicad_pcb >/dev/null 2>&1; done
kicad-cli pcb export svg --layers "F.Cu,F.SilkS,F.Mask,Edge.Cuts" --page-size-mode 2 --exclude-drawing-sheet -o "$S/top.svg" adv7280m-csi2.kicad_pcb >/dev/null 2>&1
SVG2PNG=${SVG2PNG:-$S/svg2png.mjs}
cd "$S" && for f in top L_F.Cu L_B.Cu L_In1.Cu L_In2.Cu; do node "$SVG2PNG" $f.svg $f.png 1830 2400; done
D="$ROOT/docs"
cp top.png "$D/layer-top.png"; cp L_B.Cu.png "$D/layer-bottom.png"; cp L_In1.Cu.png "$D/layer-in1-gnd.png"; cp L_In2.Cu.png "$D/layer-in2-pwr.png"
echo rendered
