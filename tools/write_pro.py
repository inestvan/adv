"""Write hw/adv7280m-csi2.kicad_pro (pcbnew.SaveBoard overwrites the project with defaults, so run this last)."""
import json, os
HW = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'hw'))
p = os.path.join(HW, 'adv7280m-csi2.kicad_pro')
d = json.load(open(p)) if os.path.exists(p) else {}
d.setdefault('meta', {"filename": "adv7280m-csi2.kicad_pro", "version": 3})
d['board'] = d.get('board', {})
d['board']['design_settings'] = {
  "defaults": {"board_outline_line_width": 0.1, "copper_line_width": 0.2, "silk_line_width": 0.12, "silk_text_size_h": 0.8, "silk_text_size_v": 0.8, "silk_text_thickness": 0.12},
  "rules": {"min_clearance": 0.127, "min_connection": 0.0, "min_copper_edge_clearance": 0.3, "min_hole_clearance": 0.2, "min_hole_to_hole": 0.5,
            "min_microvia_diameter": 0.2, "min_microvia_drill": 0.1, "min_resolved_spokes": 1, "min_silk_clearance": 0.0, "min_text_height": 0.6,
            "min_text_thickness": 0.08, "min_through_hole_diameter": 0.2, "min_track_width": 0.127, "min_via_annular_width": 0.1, "min_via_diameter": 0.45,
            "solder_mask_to_copper_clearance": 0.0, "use_height_for_length_calcs": True},
  "diff_pair_dimensions": [{"gap": 0.15, "via_gap": 0.25, "width": 0.18}],
  "track_widths": [0.0, 0.15, 0.18, 0.2, 0.3, 0.4],
  "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.5, "drill": 0.3}, {"diameter": 0.45, "drill": 0.2}]
}
def nc(name, clr, tw, dpw, dpg, prio):
    return {"name": name, "clearance": clr, "track_width": tw, "via_diameter": 0.5, "via_drill": 0.3, "diff_pair_gap": dpg, "diff_pair_via_gap": 0.25,
            "diff_pair_width": dpw, "microvia_diameter": 0.3, "microvia_drill": 0.1, "bus_width": 12, "line_style": 0, "wire_width": 6,
            "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)", "priority": prio}
d['net_settings'] = {
  "classes": [nc("Default", 0.2, 0.2, 0.2, 0.25, 2147483647), nc("MIPI", 0.15, 0.18, 0.18, 0.15, 0), nc("Power", 0.2, 0.4, 0.2, 0.25, 1)],
  "meta": {"version": 4}, "net_colors": None, "netclass_assignments": None,
  "netclass_patterns": [{"netclass": "MIPI", "pattern": "/CSI_*"}, {"netclass": "Power", "pattern": "+3V3"}, {"netclass": "Power", "pattern": "+1V8*"},
                        {"netclass": "Power", "pattern": "PVDD"}, {"netclass": "Power", "pattern": "GND"}]
}
json.dump(d, open(p, 'w'), indent=2)
print('wrote', p)
