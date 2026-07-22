/*
  ESP32-S3-Touch-AMOLED-1.75 battery case, parametric draft.

  Units: millimeters.
  Target: Waveshare ESP32-S3-Touch-AMOLED-1.75 with a thin 1S LiPo battery.

  Export from OpenSCAD:
    - Set `part = "front"` and export STL for the front bezel.
    - Set `part = "back"` and export STL for the rear battery shell.
    - Set `part = "assembly"` to inspect the fit.

  This is a first-print mechanical draft. Verify connector/button positions
  against your real board before committing to a final enclosure.
*/

$fn = 160;

part = "assembly"; // "assembly", "front", "back", "battery"

// Official/estimated dimensions from Waveshare drawings.
screen_glass_od = 48.96;
visible_lcd_od = 43.76;
board_od = 46.00;
official_shell_od = 51.00;

// Print tuning.
xy_clearance = 0.35;
z_clearance = 0.35;
wall = 1.55;
front_lip = 1.15;
front_height = 3.00;
back_height = 12.80;       // Increase for thicker batteries.
case_od = 53.60;           // Slightly larger than official shell for battery room.
corner_chamfer = 0.80;

// Battery bay. Good first target: 302030/402030 style LiPo, 200-300 mAh.
battery_x = 32.00;
battery_y = 25.00;
battery_z = 4.80;
battery_wire_channel_w = 4.00;

// Board stack clearances.
board_seat_depth = 2.20;
electronics_clear_z = 6.20;
rear_floor = 1.40;

// M2 screw/post positions, estimated from the rear drawing.
screw_d = 2.20;
screw_head_d = 4.20;
post_d = 5.20;
post_h = 5.30;
screw_pts = [
    [0.00, 20.50],
    [-13.75, -18.70],
    [13.75, -18.70]
];

// External cutouts. Adjust after measuring your printed board in the shell.
usb_cutout_w = 10.80;
usb_cutout_h = 5.80;
usb_y = 0.00;
button_cutout_w = 3.10;
button_cutout_h = 7.60;

module rounded_cylinder(d, h, chamfer = 0.5) {
    hull() {
        translate([0, 0, chamfer])
            cylinder(d = d - 2 * chamfer, h = h - 2 * chamfer);
        translate([0, 0, h / 2])
            cylinder(d = d, h = max(0.01, h - 2 * chamfer));
    }
}

module ring(od, id, h) {
    difference() {
        cylinder(d = od, h = h);
        translate([0, 0, -0.05])
            cylinder(d = id, h = h + 0.10);
    }
}

module screw_posts() {
    for (p = screw_pts) {
        translate([p[0], p[1], rear_floor])
            difference() {
                cylinder(d = post_d, h = post_h);
                translate([0, 0, -0.05])
                    cylinder(d = screw_d, h = post_h + 0.10);
            }
    }
}

module front_bezel() {
    difference() {
        union() {
            rounded_cylinder(case_od, front_height, corner_chamfer);
            translate([0, 0, front_height - front_lip])
                ring(case_od - 2.0, screen_glass_od + xy_clearance, front_lip);
        }

        // LCD visible opening with a small retaining shoulder over the glass edge.
        translate([0, 0, -0.05])
            cylinder(d = visible_lcd_od + 0.80, h = front_height + 0.10);

        // Back relief for the round glass and PCB top stack.
        translate([0, 0, front_height - front_lip - 0.05])
            cylinder(d = screen_glass_od + xy_clearance, h = front_lip + 0.15);

        // Screw clearance from rear into front lip, optional heat-set/m2 self tap.
        for (p = screw_pts) {
            translate([p[0], p[1], -0.05])
                cylinder(d = screw_d, h = front_height + 0.10);
        }
    }
}

module rear_battery_shell() {
    difference() {
        union() {
            rounded_cylinder(case_od, back_height, corner_chamfer);
            screw_posts();
        }

        // Main open cavity. The rear part must be a cup, not a capped block.
        translate([0, 0, rear_floor])
            cylinder(d = case_od - 2 * wall, h = back_height - rear_floor + 0.30);

        // Board pocket.
        translate([0, 0, back_height - board_seat_depth])
            cylinder(d = board_od + 2 * xy_clearance, h = board_seat_depth + 0.10);

        // Battery pocket in the rear floor, shifted away from USB side.
        translate([0, -1.80, rear_floor - 0.05])
            rounded_box([battery_x, battery_y, battery_z + z_clearance], 1.20);

        // Wire channel from battery bay toward BAT connector at board top.
        translate([0, 13.00, rear_floor - 0.05])
            cube([battery_wire_channel_w, 16.00, battery_z + 0.80], center = true);

        // USB-C side opening, placed on +X side.
        translate([case_od / 2 - 0.90, usb_y, back_height - 5.90])
            cube([2.20, usb_cutout_w, usb_cutout_h], center = true);

        // Two side button windows, one on each side. Tune angles after test fit.
        rotate([0, 0, 92])
            translate([case_od / 2 - 0.80, 0, back_height - 6.10])
                cube([2.20, button_cutout_h, button_cutout_w], center = true);
        rotate([0, 0, -92])
            translate([case_od / 2 - 0.80, 0, back_height - 6.10])
                cube([2.20, button_cutout_h, button_cutout_w], center = true);

        // Bottom pin-header/service opening.
        translate([0, -case_od / 2 + 1.20, back_height - 4.30])
            cube([18.00, 2.40, 4.80], center = true);

        // Screw through-holes and head relief from the rear.
        for (p = screw_pts) {
            translate([p[0], p[1], -0.05])
                cylinder(d = screw_d, h = back_height + 0.10);
            translate([p[0], p[1], 0.00])
                cylinder(d = screw_head_d, h = 1.20);
        }
    }
}

module rounded_box(size, radius) {
    x = size[0];
    y = size[1];
    z = size[2];
    hull() {
        for (sx = [-1, 1], sy = [-1, 1]) {
            translate([sx * (x / 2 - radius), sy * (y / 2 - radius), 0])
                cylinder(r = radius, h = z);
        }
    }
}

module battery_mock() {
    color([0.85, 0.85, 0.90, 0.55])
        translate([0, -1.80, rear_floor + 0.05])
            rounded_box([battery_x, battery_y, battery_z], 1.20);
}

module board_mock() {
    color([0.0, 0.25, 0.8, 0.35])
        translate([0, 0, back_height - board_seat_depth + 0.20])
            cylinder(d = board_od, h = 1.20);
    color([0.02, 0.02, 0.02, 0.45])
        translate([0, 0, back_height + 0.80])
            cylinder(d = screen_glass_od, h = 1.20);
}

if (part == "front") {
    front_bezel();
} else if (part == "back") {
    rear_battery_shell();
} else if (part == "battery") {
    battery_mock();
} else {
    color([0.04, 0.04, 0.04, 0.90]) rear_battery_shell();
    translate([0, 0, back_height])
        color([0.01, 0.01, 0.01, 0.85]) front_bezel();
    battery_mock();
    board_mock();
}
