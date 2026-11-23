// SPRUT-M + Raspberry Pi 4 case for 3D printing, stacked ("этажерка").
//
// Inside: ONE MODULE assembled on the table and dropped in from the top: Raspberry Pi 4 + four printed 17 mm standoffs
// + a printed carrier plate (over the USB/LAN blocks) + the SPRUT-M board (rev C, 42 x 55, buffer section not broken off).
// The board is turned so that its camera connector J1 is parallel to the Pi CSI connector and right next to it:
// the FFC leaves J1 towards the CSI and bends straight down into it.
// Outside: USB-C power and micro-HDMI0 on the front, LAN on the right, microSD slot on the left, and on the left wall
// three 3.5 mm 4-pole (TRRS) panel jacks (PJ-392 type, from stock), inserted from inside: IN, TV, GX16.
//
// Pi 4B dimensions: part outlines from the official DXF (RP-008342-DS, layers PARTS_TOP / PARTS_BOTTOM), heights from
// the official drawing (RP-008343-DS). Board 85 x 56, holes 3.5 from the edges, 58 x 49. Outlines (x, y; y = 0 is the
// USB-C edge): USB-C 6.88..15.52 / -1.25..6.15 (h 3.2); micro-HDMI 22.40..29.60 and 35.90..43.10 / -1.43..6.52 (h 3.0);
// audio 50.5..57.5 / 0..12.5 + barrel 51..57 / -2.5..0 (h 6.0); CSI 45.05..49.00 / 0.3..22.7 (h 5.5), its cable slot
// 45.05..45.95 / 3.5..19.5; DSI 1.5..5.45 / 16.8..39.2 (h 5.5); GPIO 7.1..57.9 / 50..55 (h 8.5); PoE header
// 59..64 / 43.86..48.86; SoC 21.75..36.75 / 25..40 (h 2.4); Ethernet 66.65..88.0 / 37.99..53.5 (h 13.5); USB
// 70.3..88.0 / 1.65..15.57 and 70.5..88.0 / 20.43..34.25 (h 16.0); microSD card (bottom) -2.26..1.68 / 22.51..33.51.
//
// Coordinates: X to the right, Y to the back, Z up, mm. Origin: front-left corner of the Pi 4 PCB (top view, GPIO
// header at the back, USB-C at the front left); Z = 0 is the bottom of the case.
// SPRUT-M board coordinates are those of docs/mech/enclosure-dims.md (top view, origin top-left, Y down).
//
// Export: openscad -D 'part="base"' -o base.stl sprutm-pi4-case.scad   (parts: base, lid, plate; assembly, exploded,
// section for pictures; chk1..chk8 interference checks, each must be empty or zero volume)
//
// Nothing hangs over the Pi inside the walls (the gap between the Pi's connectors and the walls is only 1.5..3.5 mm):
// the module drops straight down (check chk10), the heads of the Pi screws sit in pockets of the floor bosses (no
// sideways play), two posts of the lid hold the plate down, and the lid is screwed to four ears OUTSIDE the walls.
//
// Fasteners (all from stock): Pi to the standoffs 4 x M2.5 x 8 from under the Pi; plate to the standoffs 4 x M2.5 x 8
// from the top; SPRUT-M board to the plate 3 x M2.5 x 8 (its holes H2, H3, H4 drilled from 2.2 to 2.6 mm; unplated,
// 5 mm keep-out); lid 4 x M3 x 10 countersunk (DIN 965). All screws cut their own thread in the printed pilot holes.

part = "assembly";
$fn = 48;

// ---------------------------------------------------------------- print allowance
// Fuzzy skin (slicer "outer walls / contour only", thickness 0.3) roughens the outer contour, and in the layers of a
// hole through a wall that contour runs along the hole's sides: every wall opening is made 2 x fuzzy larger, and the
// sliding fits (lid lip, plate) get fuzzy more clearance. Set fuzzy = 0 for a print without fuzzy skin.
fuzzy = 0.3;
clr = 0.3;				// basic fit clearance

// ---------------------------------------------------------------- dimensions
wall = 2.0; floor_t = 2.0; lid_t = 2.0;
in_x0 = -3.5;				// microSD card sticks out ~2.5 mm on the left
in_x1 = 88.6;				// Ethernet / USB and the SPRUT-M board all reach x 88.0 (+ clr + fuzzy)
in_y0 = -4.0;				// audio jack sticks out to -2.7; the SPRUT-M board starts at -3.5
in_y1 = 57.5;				// GPIO header at the back edge
top_z = 46;				// top of the base walls; the lid sits on it
pi_boss_h = 5.5;			// Pi 4 bottom side needs >= 3; tall enough for an M2.5 x 8 screw (7 mm pilot)
pi_bot = floor_t + pi_boss_h; pi_t = 1.4; pi_top = pi_bot + pi_t;
spacer_h = 18.5;			// Pi top -> plate underside: clears USB (16.0), Ethernet (13.5), GPIO with Dupont (~14),
					// and the roofs of the wall niches (up to 26.6)
plate_t = 2.5; plate_z0 = pi_top + spacer_h; plate_z1 = plate_z0 + plate_t;
bb_h = 3;				// SPRUT-M board above the plate (only solder blobs underneath, ~1 mm)
brd_z0 = plate_z1 + bb_h; brd_t = 1.6; brd_z1 = brd_z0 + brd_t;

pi_holes = [[3.5, 3.5], [61.5, 3.5], [3.5, 52.5], [61.5, 52.5]];	// Pi 4, M2.5
usbc_x = 11.2; hdmi0_x = 26.0; hdmi1_x = 39.5; audio_x = 54.0; lan_y = 45.75; sd_y = 28.0;
csi = [45.5, 11.5];			// the CSI cable slot (x 45.05..45.95), centred on it; the connector body reaches x 49.0

// SPRUT-M placement: not rotated (board X -> case X, board Y -> case Y). J1 (board X 0..5.5, Y 6.1..23.9) is the
// board's left edge, its slot runs along Y like the Pi CSI slot and is centred on it (board Y 15 -> case Y 11.5);
// its left edge (J1) is 0.5 mm right of the CSI cable slot: the FFC leaves J1 and goes straight down along the board
// edge into the CSI connector (the board is 18 mm above the CSI connector, nothing in the way). So the board ends at
// x 88.0 like the Ethernet jack, and the right wall can stand right next to both.
brd_x0 = 46; brd_y0 = csi[1] - 15;
function B(p) = [brd_x0 + p[0], brd_y0 + p[1]];
brd_screw = [[39.5, 27.5], [3.2, 51.8], [39.5, 37.0]];	// H2, H3, H4: M2.5 x 8 into the plate bosses (holes drilled 2.6)
brd_rest = [[8.0, 3.2]];				// H1: a support pin only (next to the plate screw head at Pi hole 61.5 / 3.5)

// panel jacks on the left wall: PJ-392 type, hole 6.2 (+ fuzzy). Inserted FROM INSIDE: the round flange sits inside
// against the wall, the nut outside, so jack + cable can be soldered on the table first.
jack_y = [14, 28, 42]; jack_z = 37; jack_d = 6.2; jack_flange_d = 10; jack_lbl = ["IN", "TV", "GX16"];

standoff_d = 6;				// printed standoffs, Pi top -> plate underside (spacer_h)
pi_head_d = 5.6; pi_head_h = 2.8;	// pockets in the floor bosses for the heads of the Pi screws (M2.5 pan head ~4.5 x 1.7)

// lid: 4 ears outside the walls (M3 x 10 countersunk, pilot 2.5), 2 posts pressing on the plate (0.3 mm gap)
ear_d = 8; ear_h = 8;
lid_posts = [[26, 2], [26, 54]]; lid_post_d = 5;

out_x0 = in_x0 - wall; out_x1 = in_x1 + wall; out_y0 = in_y0 - wall; out_y1 = in_y1 + wall;
ears = [[out_x0 - ear_d / 2, out_y0 + 5], [out_x0 - ear_d / 2, out_y1 - 5], [out_x1 + ear_d / 2, out_y0 + 5], [out_x1 + ear_d / 2, out_y1 - 5]];

// ---------------------------------------------------------------- helpers
module rrect(w, h, r) { rr = min(r, min(w, h) / 2 - 0.05); offset(rr) square([w - 2 * rr, h - 2 * rr], center = true); }
module rrect_box(w, h, r) { translate([w / 2, h / 2]) rrect(w, h, r); }
// holes through the front wall (w along X, h along Z) and the left / right walls (w along Y, h along Z), + fuzzy
module hole_front(x, z, w, h, r = 1) { translate([x, in_y0 + 4, z]) rotate([90, 0, 0]) linear_extrude(wall + 6) rrect(w + 2 * fuzzy, h + 2 * fuzzy, r); }
module hole_right(y, z, w, h, r = 1) { translate([in_x1 - 4, y, z]) rotate([0, 90, 0]) linear_extrude(wall + 6) rrect(h + 2 * fuzzy, w + 2 * fuzzy, r); }

// ---------------------------------------------------------------- niches
// The walls stand off for the SPRUT-M board (3.5 mm in front of the Pi; on the right it ends with the Ethernet jack at
// x 88.0), which would leave LAN 2.6 mm and USB-C / HDMI 4.7 mm deep inside the outer surface. At the Pi level the wall is therefore replaced by a thin FRAME
// right at the Pi PCB edge (PCB edge + clearance + fuzzy), with a niche open to the outside in front of it. The
// connectors overhang the PCB edge (LAN 3.0 mm, USB-C 1.25, micro-HDMI 1.43), so they go THROUGH the frame: each in a
// slot open to the top, because the module is lowered in from above. Result: LAN stands 1.2 mm out of the frame (its
// front 2.6 mm inside the outer surface), USB-C / HDMI are flush with it (0.2..0.35 mm inside) and the openings around them take the plug mouldings.
fr_gap = clr + fuzzy;			// PCB edge -> frame inner face (in the slot layers the inner face gets fuzzy too)
lan_in = 85 + fr_gap;			// right frame inner face (Pi PCB edge at x 85); LAN front at x 88.0
fr_r = 1.2;				// right frame thickness (3 lines of 0.4)
front_in = 0 - fr_gap;			// front frame inner face (Pi PCB edge at y 0); USB-C front -1.25, micro-HDMI -1.43
fr_f = 1.0;				// front frame thickness
r_niche = [36.1, in_y1 - 0.5, 6, 23.0];	// y0, y1, z0, z1 of the LAN niche pocket (USB 3.0 port ends at y 34.25)
r_roof = plate_z0 - 0.8;		// top of the solid over the LAN niche: the plate (to x 88.3) is lowered past it
f_niche = [3, 33.6, 4, 16.2];	// x0, x1, z0, z1 of the USB-C + HDMI niche pocket
sl_c = clr + fuzzy;			// slot clearance around a connector
module niche_right_solid() {
	translate([lan_in, r_niche[0] - fr_r, 0]) cube([in_x1 + 0.5 - lan_in, in_y1 - r_niche[0] + fr_r, r_roof]);
}
module niche_right_pocket() {		// 45 degree ceiling, rising towards the outside (prints without supports)
	x = lan_in + fr_r; d = out_x1 + 1 - x;
	hull() {
		translate([x, r_niche[0], r_niche[2]]) cube([d, r_niche[1] - r_niche[0], r_niche[3] - r_niche[2]]);
		translate([out_x1, r_niche[0], r_niche[2]]) cube([1, r_niche[1] - r_niche[0], r_niche[3] - r_niche[2] + out_x1 - x]);
	}
}
module niche_front_solid() {		// 45 degrees back out above the pocket, under the plate
	x0 = f_niche[0] - 1.2; xl = f_niche[1] - f_niche[0] + 2.4; zc = f_niche[3] + 1.6;	// 1.1 mm clear of micro-HDMI 1
	hull() {
		translate([x0, in_y0 - 0.5, 0]) cube([xl, front_in - in_y0 + 0.5, zc]);
		translate([x0, in_y0 - 0.5, 0]) cube([xl, 0.5, zc + front_in - in_y0]);
	}
}
module niche_front_pocket() {
	y = front_in - fr_f; d = y - out_y0;
	hull() {
		translate([f_niche[0], out_y0 - 1, f_niche[2]]) cube([f_niche[1] - f_niche[0], y - out_y0 + 1, f_niche[3] - f_niche[2]]);
		translate([f_niche[0], out_y0 - 1, f_niche[2]]) cube([f_niche[1] - f_niche[0], 1, f_niche[3] - f_niche[2] + d]);
	}
}
// open-top slots for the connectors through the frames (x0, x1, y0, y1 of the connector, its bottom = Pi top)
module conn_slot(x0, x1, y0, y1) { translate([x0 - sl_c, y0 - sl_c, pi_top - 0.3]) cube([x1 - x0 + 2 * sl_c, y1 - y0 + 2 * sl_c, top_z]); }
module conn_slots() {
	conn_slot(85 - 1, 88.0, 37.99, 53.50);		// Ethernet (from inside the PCB edge out to its front)
	conn_slot(6.88, 15.52, -1.25, 0 + 1);		// USB-C
	conn_slot(22.40, 29.60, -1.43, 0 + 1);		// micro-HDMI 0
}
// microSD: the wall and the floor under the card are cut away, so the card end can be gripped from the side and below
module sd_notch() { translate([out_x0 - 1, sd_y - 8, -1]) cube([1 - out_x0 + 1 + 1, 16, pi_bot - 0.3 + 1]); }
module hole_left(y, z, w, h, r = 1) { translate([in_x0 + 1, y, z]) rotate([0, -90, 0]) linear_extrude(wall + 3) rrect(h + 2 * fuzzy, w + 2 * fuzzy, r); }
// labels engraved lbl_depth into the outside of the left wall, readable from the left. For white letters: print
// part "labels" (exactly fills the engraving) in white with a multi-material printer, or fill the engraving with paint.
// Letters 5 mm high so that the bold strokes are ~0.9 mm, wider than a 0.4 mm nozzle line.
lbl_depth = 1.0;
module text_left(s, y, z, size, h) {	// h: extrusion from the engraving bottom outwards
	multmatrix([[0, 0, -1, out_x0 + lbl_depth], [-1, 0, 0, y], [0, 1, 0, z], [0, 0, 0, 1]])
		linear_extrude(h) text(s, size = size, font = "Liberation Sans:style=Bold", halign = "center", valign = "center");
}
module labels(h) {
	for (i = [0 : 2]) text_left(jack_lbl[i], jack_y[i], jack_z - 8, jack_lbl[i] == "GX16" ? 4.2 : 5, h);
}
// an ear on the outside of the left / right wall, top at top_z, 45 degrees underneath (prints without supports)
module ear(p) {
	wx = p[0] < 40 ? out_x0 : out_x1;
	hull() {
		translate([p[0], p[1], top_z - ear_h]) cylinder(d = ear_d, h = ear_h);
		translate([wx - 0.25, p[1] - ear_d / 2, top_z - ear_h - ear_d]) cube([0.5, ear_d, ear_h + ear_d]);
	}
}

// ---------------------------------------------------------------- base
module base() {
	difference() {
		union() {
			difference() {
				translate([out_x0, out_y0, 0]) linear_extrude(top_z) rrect_box(out_x1 - out_x0, out_y1 - out_y0, 3);
				translate([in_x0, in_y0, floor_t]) cube([in_x1 - in_x0, in_y1 - in_y0, top_z]);
			}
			for (p = pi_holes) translate([p[0], p[1], floor_t - 0.01]) cylinder(d = 7, h = pi_boss_h + 0.01);
			for (p = ears) ear(p);
			niche_right_solid();
			niche_front_solid();
		}
		niche_right_pocket();
		niche_front_pocket();
		conn_slots();
		sd_notch();
		// pockets for the heads of the Pi screws (screwed from under the Pi into the standoffs)
		for (p = pi_holes) translate([p[0], p[1], pi_bot - pi_head_h]) cylinder(d = pi_head_d, h = pi_head_h + 1);
		// lid screws M3 x 10: pilot 2.5, 9 deep, in the ears
		for (p = ears) translate([p[0], p[1], top_z - 9]) cylinder(d = 2.5, h = 10);
		// front: USB-C and micro-HDMI0, openings in the frame for the plug mouldings (USB-C up to ~12.4 x 7.4,
		// micro-HDMI up to ~11.4 x 8.4), so a plug goes fully in although the receptacles are flush with the frame
		hole_front(usbc_x, pi_top + 1.6, 13.0, 8.0, 2);
		hole_front(hdmi0_x, pi_top + 1.5, 12.0, 9.0, 1.5);
		// right: LAN goes through the frame in its slot (conn_slots), nothing else to cut
		// left: the three jacks, with labels under them
		for (i = [0 : 2]) translate([in_x0 + 1, jack_y[i], jack_z]) rotate([0, -90, 0]) cylinder(d = jack_d + 2 * fuzzy, h = wall + 3);
		labels(lbl_depth + 1);
		// back wall vents at the Pi level, floor vents under the SoC
		for (x = [6 : 6 : 56]) translate([x, in_y1 - 1, pi_top + 3]) rotate([-90, 0, 0]) linear_extrude(wall + 3) rrect(2.4 + 2 * fuzzy, 10, 1.2);
		for (y = [16 : 5 : 44]) translate([14, y, -1]) linear_extrude(floor_t + 2) rrect(32, 2.4, 1.2);
	}
}

// ---------------------------------------------------------------- lid
module lid() {
	lip = 1.2; lip_h = 3; lc = clr + fuzzy;
	difference() {
		union() {
			translate([out_x0, out_y0, top_z]) linear_extrude(lid_t) rrect_box(out_x1 - out_x0, out_y1 - out_y0, 3);
			for (p = ears) translate([p[0], p[1], top_z]) hull() {
				cylinder(d = ear_d, h = lid_t);
				translate([(p[0] < 40 ? 1 : -1) * ear_d / 2 - 0.25, -ear_d / 2, 0]) cube([0.5, ear_d, lid_t]);
			}
			// locating lip inside the walls
			difference() {
				translate([in_x0 + lc, in_y0 + lc, top_z - lip_h]) cube([in_x1 - in_x0 - 2 * lc, in_y1 - in_y0 - 2 * lc, lip_h]);
				translate([in_x0 + lc + lip, in_y0 + lc + lip, top_z - lip_h - 1]) cube([in_x1 - in_x0 - 2 * (lc + lip), in_y1 - in_y0 - 2 * (lc + lip), lip_h + 2]);
			}
			// posts holding the plate down (0.3 mm above it, so the lid always closes)
			for (p = lid_posts) translate([p[0], p[1], plate_z1 + 0.3]) cylinder(d = lid_post_d, h = top_z - plate_z1 - 0.3 + 0.01);
		}
		// M3 screw holes in the ears, countersunk from the top
		for (p = ears) {
			translate([p[0], p[1], top_z - 1]) cylinder(d = 3.4, h = lid_t + 2);
			translate([p[0], p[1], top_z + lid_t - 1.7]) cylinder(d1 = 3.4, d2 = 6.6, h = 1.71);
		}
		// vents over the Pi SoC and the SPRUT-M board
		for (y = [12 : 5 : 47]) translate([47, y, top_z - 1]) linear_extrude(lid_t + 2) rrect(54, 2.4, 1.2);
	}
}

// ---------------------------------------------------------------- carrier plate (printed flat, bosses up)
module plate() {
	pc = clr + fuzzy + 0.2;
	difference() {
		union() {
			// right edge only 0.3 from the wall: the board bosses at H2 / H4 (x 83..88) must stand fully on the plate
			translate([in_x0 + pc, in_y0 + pc, plate_z0]) cube([in_x1 - 0.3 - in_x0 - pc, in_y1 - in_y0 - 2 * pc, plate_t]);
			for (p = brd_screw) translate([B(p)[0], B(p)[1], plate_z1 - 0.01]) cylinder(d = 5, h = bb_h + 0.01);
			for (p = brd_rest) translate([B(p)[0], B(p)[1], plate_z1 - 0.01]) cylinder(d = 3, h = bb_h + 0.01);
		}
		// M2.5 x 8 from the top into the standoffs on the Pi holes
		for (p = pi_holes) translate([p[0], p[1], plate_z0 - 1]) cylinder(d = 2.8, h = plate_t + 2);
		// SPRUT-M board screws M2.5 x 8: pilot 2.1 through boss and plate (the tip may stick out 1 mm underneath)
		for (p = brd_screw) translate([B(p)[0], B(p)[1], plate_z0 - 1]) cylinder(d = 2.1, h = plate_t + bb_h + 2);
		// FFC: comes off J1 to the left and goes straight down into the Pi CSI connector (slot along Y)
		translate([csi[0], csi[1], plate_z0 - 1]) linear_extrude(plate_t + 2) rrect(6, 21, 1.5);
		// wires from the Pi GPIO 5 V / GND (pins 2 and 6, back-left of the header)
		translate([12, 46, plate_z0 - 1]) linear_extrude(plate_t + 2) rrect(10, 8, 2);
		// zip-tie slots (pairs, 8 mm apart) to hold the inline connectors of the cables on the free left part
		for (y = [8, 50]) for (x = [22, 30]) translate([x, y, plate_z0 - 1]) linear_extrude(plate_t + 2) rrect(1.8, 4.2, 0.6);
		// vents over the SoC (left of the board), and under the board between its screws
		for (y = [16 : 5 : 41]) translate([16, y, plate_z0 - 1]) linear_extrude(plate_t + 2) rrect(24, 2.4, 1.2);
		for (y = [8 : 5 : 43]) translate([70, y, plate_z0 - 1]) linear_extrude(plate_t + 2) rrect(24, 2.4, 1.2);
	}
}

// ---------------------------------------------------------------- standoff (print 4): Pi top -> plate underside
module standoff() { difference() { cylinder(d = standoff_d, h = spacer_h); translate([0, 0, -1]) cylinder(d = 2.1, h = spacer_h + 2); } }
module standoffs() { for (p = pi_holes) translate([p[0], p[1], pi_top]) standoff(); }

// ---------------------------------------------------------------- mock-ups (official Pi 4 drawing)
module pi4_mock() {	// outlines from the official DXF, heights from the official drawing
	module box(x0, x1, y0, y1, h, z = pi_top) translate([x0, y0, z]) cube([x1 - x0, y1 - y0, h]);
	color("ForestGreen") translate([0, 0, pi_bot]) linear_extrude(pi_t) rrect_box(85, 56, 3);
	color("Silver") {
		box(6.88, 15.52, -1.25, 6.15, 3.2);				// USB-C
		box(22.40, 29.60, -1.43, 6.52, 3.0);				// micro-HDMI 0
		box(35.90, 43.10, -1.43, 6.52, 3.0);				// micro-HDMI 1
		box(66.65, 88.00, 37.99, 53.50, 13.5);				// Ethernet
		box(70.30, 88.00, 1.65, 15.57, 16.0);				// USB (2.0)
		box(70.50, 88.00, 20.43, 34.25, 16.0);				// USB (3.0)
	}
	color("Black") {
		box(50.5, 57.5, 0, 12.5, 6.0); box(51.0, 57.0, -2.5, 0, 6.0);	// audio jack
		box(7.1, 57.9, 50, 55, 8.5);					// GPIO header
		box(59.0, 64.0, 43.86, 48.86, 8.5);				// PoE header
		box(-2.26, 13.14, 22.11, 34.10, 1.4, pi_bot - 1.4);		// microSD holder + card (bottom)
	}
	color("Beige") {
		box(45.05, 49.00, 0.3, 22.7, 5.5);				// CSI connector
		box(1.5, 5.45, 16.8, 39.2, 5.5);				// DSI connector
	}
	color("DimGray") box(21.75, 36.75, 25, 40, 2.4);			// SoC
	color("Silver") translate([29.25 - 12.5, 32.5 - 12.5, pi_top + 2.4]) cube([25, 25, 10]);	// heatsink 25x25x10 from stock
}
module sprutm_mock() {
	color("RoyalBlue") translate([brd_x0, brd_y0, brd_z0]) cube([42, 55, brd_t]);
	color("Beige") translate([B([0, 6.1])[0], B([0, 6.1])[1], brd_z1]) cube([5.5, 17.8, 3]);	// J1 FFC connector
	color("Black") translate([B([20, 10.5])[0] - 2.5, B([20, 10.5])[1] - 2.5, brd_z1]) cube([5, 5, 1]);	// ADV7280
}
module jack_mock() {	// PJ-392 from inside: flange inside, body + lugs, nut outside
	color("DimGray") for (y = jack_y) {
		translate([in_x0, y, jack_z]) rotate([0, 90, 0]) cylinder(d = jack_flange_d, h = 3);
		translate([in_x0 + 3, y, jack_z]) rotate([0, 90, 0]) cylinder(d = 7, h = 18);
		translate([out_x0 - 2, y, jack_z]) rotate([0, 90, 0]) cylinder(d = 9.2, h = 2, $fn = 6);
	}
}
module ffc_mock() {	// leaves J1 towards -X at the top of the board, bends down into the CSI connector
	color("Orange") {
		translate([csi[0] - 0.15, csi[1] - 8, pi_top + 3]) cube([0.3, 16, brd_z1 + 1.5 - pi_top - 3]);
		translate([csi[0] - 0.15, csi[1] - 8, brd_z1 + 1.5]) cube([brd_x0 - 0.01 - csi[0], 16, 0.3]);	// up to J1
	}
}

if (part == "base") base();
else if (part == "lid") translate([0, 0, top_z + lid_t]) rotate([180, 0, 0]) lid();
else if (part == "plate") translate([0, 0, -plate_z0]) plate();
else if (part == "labels") labels(lbl_depth);
else if (part == "standoff") standoff();
// quick fit test: only the bottom of the case up to the plate level (floor, Pi bosses, all Pi openings), to check
// that the Pi sits on its bosses and every connector is centred in its opening before printing the whole case
else if (part == "fittest") intersection() { base(); translate([out_x0 - 20, out_y0 - 1, -1]) cube([out_x1 - out_x0 + 40, out_y1 - out_y0 + 2, plate_z0 + 1]); }		// same coordinates as "base": load both as one object
else if (part == "assembly") {
	color("LightGray") base();
	color("White") labels(lbl_depth);
	color("Khaki") plate();
	color("Tan") standoffs();
	pi4_mock(); sprutm_mock(); jack_mock(); ffc_mock();
}
else if (part == "exploded") {
	color("LightGray") base();
	color("White") labels(lbl_depth);
	// the module above the case, as it is lowered in
	translate([0, 0, 40]) { color("Khaki") plate(); color("Tan") standoffs(); pi4_mock(); sprutm_mock(); ffc_mock(); }
	color("SlateGray") translate([0, 0, 80]) lid();
}

// ---------------------------------------------------------------- interference checks (each must be empty / zero volume)
if (part == "chk1") intersection() { base(); plate(); }
if (part == "chk2") intersection() { base(); pi4_mock(); }
if (part == "chk3") intersection() { plate(); pi4_mock(); }
if (part == "chk4") intersection() { base(); sprutm_mock(); }
if (part == "chk5") intersection() { jack_mock(); union() { plate(); sprutm_mock(); } }
if (part == "chk6") intersection() { lid(); union() { sprutm_mock(); jack_mock(); plate(); } }
if (part == "chk7") intersection() { jack_mock(); base(); }
if (part == "chk8") intersection() { ffc_mock(); union() { base(); plate(); sprutm_mock(); } }
if (part == "chk9") intersection() { base(); labels(lbl_depth); }
// the module (Pi, standoffs, plate, SPRUT-M board, FFC) must drop straight down into the case: its footprint, swept
// from the Pi's seat to above the walls, must not touch the base
// each part of the module sweeps from its own seat upwards (the plate and the board never go below their seats, so
// they may be wider than the niches lower down)
module sweep_from(z) { translate([0, 0, z]) linear_extrude(top_z + 5 - z) projection() children(); }
if (part == "chk10") intersection() {
	base();
	union() {
		// the Pi: its PCB from its own underside, the connectors (which overhang the PCB edge and pass through the
		// wall frames in slots) from the PCB top, the microSD card from its underside
		sweep_from(pi_bot + 0.01) translate([0, 0, pi_bot]) linear_extrude(pi_t) rrect_box(85, 56, 3);
		sweep_from(pi_bot - 1.4 + 0.01) translate([-2.26, 22.11, pi_bot - 1.4]) cube([15.4, 12, 1.4]);
		sweep_from(pi_top + 0.01) { pi4_mock(); standoffs(); ffc_mock(); }
		sweep_from(plate_z0 + 0.01) plate();
		sweep_from(brd_z0 + 0.01) sprutm_mock();
	}
}

// ---------------------------------------------------------------- cutaway for explaining the inside
sec_y = 20;
module cut() { translate([-50, sec_y, -10]) cube([200, 200, 100]); }
if (part == "section") {
	color("LightGray") intersection() { base(); cut(); }
	color("Khaki") intersection() { plate(); cut(); }
	color("SlateGray") intersection() { lid(); cut(); }
	intersection() { union() { pi4_mock(); sprutm_mock(); jack_mock(); } cut(); }
}
