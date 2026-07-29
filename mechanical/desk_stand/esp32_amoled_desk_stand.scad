/*
  Desk stand for the ESP32-S3-Touch-AMOLED-1.75 (cased version).

  A leaning backboard rather than a ring: the puck drops onto a shelf and
  rests in a shallow circular recess, so nothing overlaps the glass and the
  device lifts straight out. The 3000 mAh pack lives inside the backboard
  behind that recess, the 2030 cavity speaker fires forward out of the
  plinth, and the USB-C port - which points down - passes through a slot in
  the shelf into a channel that exits at the back of the base. The plug goes
  in with the device seated; nothing has to be lifted to charge.

  Units: millimetres. Export from OpenSCAD:
    part = "body"    -> the stand
    part = "cover"   -> the battery hatch
    part = "assembly" / "section" are for looking at, not for printing.

  Dimensions marked CHECK are estimates from product photos. Measure them on
  the parts in hand before the final print.
*/

$fn = 120;

part = "assembly"; // "assembly", "section", "body", "cover"

/* ---------------------------------------------------------- Measured parts */

// Waveshare cased device: 51.00 outer diameter, 12.10 deep.
device_od = 51.00;
device_thick = 12.10;

// USB-C at the bottom of the puck. CHECK both against your cable.
usb_slot_w = 14.00;    // plug body width plus room for the moulding
usb_slot_d = 10.00;    // how far the plug sticks out below the case

// 655063 pouch cell, 3000 mAh: 6.5 x 50 x 63, MX1.25-2P on a 50 mm lead.
batt_t = 6.50;
batt_w = 50.00;
batt_h = 63.00;

// 2030 cavity speaker, 8R 2W: 20 x 30 x 6.8, PH1.25-2P on a 120 mm lead.
spk_w = 30.00;
spk_h = 20.00;
spk_t = 6.80;

/* -------------------------------------------------------------- Print tuning */

fit = 0.60;          // slip fit around the device
part_fit = 1.00;     // the pouch cell swells slightly in service
wall = 2.60;
floor_t = 3.00;
tilt = 20;           // degrees back from vertical
recess_d = 5.00;     // how deep the puck sits into the backboard
shelf_out = 14.00;   // how far the shelf reaches forward
shelf_t = 4.00;      // thickness of the shelf slab
shelf_lip = 3.50;    // front kerb that stops the puck sliding off
cover_fit = 0.35;
screw_d = 2.10;      // M2 self-tapping into printed bosses

/* ------------------------------------------------------------- Derived sizes */

recess_od = device_od + 2 * fit;

batt_bay_t = batt_t + part_fit;
batt_bay_w = batt_w + part_fit;
batt_bay_h = batt_h + part_fit;

spk_bay_w = spk_w + part_fit;
spk_bay_h = spk_h + part_fit;
spk_bay_t = spk_t + part_fit;

// Board thickness: puck recess, a rib, the pack, and the hatch face.
board_t = recess_d + wall + batt_bay_t + wall;

// The puck rests on the shelf, so the recess centre sits one radius above it.
seat_z = shelf_t;                                      // top face of the shelf
recess_cz = seat_z + recess_od / 2;                    // centre of the puck
batt_bottom = max(recess_cz + recess_od / 2 - batt_bay_h + 6, seat_z + 3);

board_w = max(recess_od + 2 * wall, batt_bay_w + 2 * wall);
board_h = max(recess_cz + recess_od / 2 + 6, batt_bottom + batt_bay_h + wall + 3);

plinth_h = spk_bay_h + floor_t + 3.0;
plinth_d = 44.00;                    // depth in front of the backboard foot
plinth_back = 20.00;                 // depth behind it, for stability and cable
base_w = board_w;
board_foot_y = plinth_d;

echo(str("footprint ", base_w, " x ",
         plinth_d + board_t * cos(tilt) + board_h * sin(tilt),
         " mm, height ", plinth_h + board_h * cos(tilt)));

/* -------------------------------------------------------------------- Helpers */

/* Always centred in X. y_from_zero puts the front face on y = 0 instead of
   centring it - the previous version shifted X as well, which slid the plinth
   half a width sideways and left it barely touching the backboard. */
module rounded_box(size, r, y_from_zero = false) {
    dy = y_from_zero ? size[1] / 2 : 0;
    translate([0, dy, 0]) hull() {
        for (x = [r - size[0] / 2, size[0] / 2 - r],
             y = [r - size[1] / 2, size[1] / 2 - r])
            translate([x, y, 0]) cylinder(h = size[2], r = r);
    }
}

/* Backboard space: built upright, then leaned back as a whole. Local +Z runs
   up the board, local +Y from the front face towards the hatch. */
module in_board_space() {
    translate([0, board_foot_y, plinth_h])
        rotate([-tilt, 0, 0])
            children();
}

/* ---------------------------------------------------------------- The parts */

module plinth_solid() {
    rounded_box([base_w, plinth_d + plinth_back, plinth_h], 6, y_from_zero = true);
}

module backboard_solid() {
    in_board_space()
        translate([0, board_t / 2, 0])
            rounded_box([board_w, board_t, board_h], 5);
}

/* Shallow circular seat for the puck. */
module device_recess() {
    in_board_space()
        translate([0, recess_d / 2 - 0.05, recess_cz])
            rotate([90, 0, 0])
                cylinder(h = recess_d + 0.1, d = recess_od, center = true);
}

/* Shelf: a slab in front of the board that the puck stands on. */
module shelf() {
    in_board_space()
        difference() {
            union() {
                translate([0, -shelf_out / 2 + 0.5, shelf_t / 2])
                    cube([recess_od + 2 * wall, shelf_out + 1, shelf_t], center = true);
                // Front kerb.
                translate([0, -shelf_out + 1.3, shelf_t + shelf_lip / 2 - 0.1])
                    cube([recess_od + 2 * wall, wall, shelf_lip], center = true);
            }
            // The USB-C plug drops through here.
            translate([0, -usb_slot_d / 2 + 1.0, shelf_t / 2])
                cube([usb_slot_w, usb_slot_d, shelf_t * 3 + shelf_lip * 2], center = true);
        }

    // Gusset under the shelf: a 14 mm horizontal overhang wants support, a
    // 45 degree ramp does not.
    in_board_space()
        for (x = [-1, 1])
            translate([x * (recess_od / 2 + wall - 3), -0.5, 0])
                rotate([90, 0, 90])
                    linear_extrude(height = 6, center = true)
                        polygon([[0, 0], [0, -shelf_out], [-shelf_out, 0]]);
}

/* Cable run: under the shelf, down the front of the board, into the plinth,
   and out of the back. */
module usb_channel() {
    in_board_space()
        translate([0, -usb_slot_d / 2 + 1.0, -12])
            cube([usb_slot_w, usb_slot_d, 30], center = true);

    // Vertical shaft through the plinth roof.
    translate([-usb_slot_w / 2, plinth_d - usb_slot_d - 6, floor_t])
        cube([usb_slot_w, usb_slot_d + 10, plinth_h]);
    // Horizontal run out of the back.
    translate([-usb_slot_w / 2, plinth_d - usb_slot_d - 6, floor_t])
        cube([usb_slot_w, plinth_back + usb_slot_d + 10, 11]);
}

module battery_bay(clearance = 0) {
    in_board_space()
        translate([0, recess_d + wall + batt_bay_t / 2,
                   batt_bottom + batt_bay_h / 2])
            cube([batt_bay_w + clearance, batt_bay_t + clearance,
                  batt_bay_h + clearance], center = true);
}

/* Hatch opening in the back face. */
module battery_hatch_cut() {
    in_board_space()
        translate([0, board_t - wall / 2, batt_bottom + batt_bay_h / 2])
            cube([batt_bay_w, wall * 2 + 0.2, batt_bay_h], center = true);
}

module hatch_screws(cut = false) {
    for (x = [-1, 1], z = [batt_bottom + 6, batt_bottom + batt_bay_h - 6])
        in_board_space()
            translate([x * (batt_bay_w / 2 + 3.0), board_t - 5, z])
                rotate([90, 0, 0]) {
                    if (cut) cylinder(h = 24, d = screw_d, center = true);
                    else cylinder(h = 10, d = screw_d + 3.6, center = true);
                }
}

module speaker_cavity() {
    // Pocket behind the front wall of the plinth.
    translate([0, wall + spk_bay_t / 2, floor_t + spk_bay_h / 2])
        cube([spk_bay_w, spk_bay_t, spk_bay_h], center = true);

    // Slot grille through that front wall.
    for (i = [-3 : 3])
        translate([i * 4.2, -1, floor_t + spk_bay_h / 2])
            rotate([-90, 0, 0])
                hull() for (z = [-spk_bay_h / 2 + 4.5, spk_bay_h / 2 - 4.5])
                    translate([0, z, 0])
                        cylinder(h = wall + 3, r = 1.15);

    // Lead-out into the wiring void, clear of the USB shaft.
    translate([spk_bay_w / 2 - 8, wall + spk_bay_t - 0.1, floor_t + 3])
        cube([8, 14, 8]);
}

/* Hollow interior of the plinth, so the leads have somewhere to live. */
module wire_void() {
    translate([0, wall + spk_bay_t + 8, floor_t])
        rounded_box([base_w - 2 * wall, plinth_d - spk_bay_t - 14,
                     plinth_h - floor_t - wall], 4, y_from_zero = true);
    // Openings in the board foot: the pack and speaker leads have to reach
    // the connectors on the back of the device.
    in_board_space()
        translate([0, recess_d / 2 + 1, seat_z + 7])
            cube([30, recess_d * 2 + 8, 16], center = true);
    in_board_space()
        translate([0, recess_d + wall + batt_bay_t / 2, batt_bottom - 3])
            cube([24, batt_bay_t, 16], center = true);
}

module battery_cover() {
    w = batt_bay_w - cover_fit;
    h = batt_bay_h - cover_fit;
    difference() {
        union() {
            rounded_box([w, h, wall], 3);
            for (x = [-1, 1], y = [-1, 1])
                translate([x * (batt_bay_w / 2 + 3.0), y * (batt_bay_h / 2 - 6), 0])
                    cylinder(h = wall, d = 7.2);
        }
        for (x = [-1, 1], y = [-1, 1])
            translate([x * (batt_bay_w / 2 + 3.0), y * (batt_bay_h / 2 - 6), -0.1])
                cylinder(h = wall + 0.2, d = screw_d + 0.5);
        // Vents: a pouch cell should never sit in a sealed box.
        for (i = [-2 : 2])
            translate([i * 9, 0, -0.1])
                hull() for (y = [-h / 2 + 14, h / 2 - 14])
                    translate([0, y, 0]) cylinder(h = wall + 0.2, r = 1.6);
    }
}

module stand_body() {
    difference() {
        union() {
            plinth_solid();
            backboard_solid();
            shelf();
            hatch_screws(cut = false);
        }
        device_recess();
        battery_bay();
        battery_hatch_cut();
        hatch_screws(cut = true);
        speaker_cavity();
        wire_void();
        usb_channel();

        // Anything below the bed is not a stand.
        translate([0, 0, -100]) cube([400, 400, 200], center = true);

        // Silicone feet, 9 mm pads.
        for (x = [-1, 1], y = [12, plinth_d + plinth_back - 12])
            translate([x * (base_w / 2 - 11), y, -0.1]) cylinder(h = 1.2, d = 9.4);
    }
}

/* --------------------------------------------------------------- Rendering */

module ghost_device() {
    color("SteelBlue", 0.45)
        in_board_space()
            translate([0, recess_d - device_thick / 2, recess_cz])
                rotate([90, 0, 0])
                    cylinder(h = device_thick, d = device_od, center = true);
}

module ghost_battery() {
    color("SeaGreen", 0.45)
        in_board_space()
            translate([0, recess_d + wall + batt_bay_t / 2,
                       batt_bottom + batt_bay_h / 2])
                cube([batt_w, batt_t, batt_h], center = true);
}

module ghost_speaker() {
    color("Orange", 0.55)
        translate([0, wall + spk_bay_t / 2, floor_t + spk_bay_h / 2])
            cube([spk_w, spk_t, spk_h], center = true);
}

module ghost_cover() {
    color("Gainsboro", 0.8)
        in_board_space()
            translate([0, board_t + 0.3, batt_bottom + batt_bay_h / 2])
                rotate([90, 0, 0]) battery_cover();
}

if (part == "xsec") {
    // True 2D slice through the centre plane: voids read as white, which a
    // shaded 3D cut does not reliably show.
    projection(cut = true) rotate([0, 90, 0]) stand_body();
} else if (part == "section") {
    // Body only: ghosts in a section just hide the cut face.
    difference() {
        stand_body();
        translate([-200, -100, -100]) cube([200, 400, 400]);
    }
} else if (part == "body") {
    stand_body();
} else if (part == "cover") {
    battery_cover();
} else {
    stand_body();
    ghost_device();
    ghost_battery();
    ghost_speaker();
    ghost_cover();
}
