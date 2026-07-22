# ESP32-S3-Touch-AMOLED-1.75 Battery Case

This folder contains a first-print parametric enclosure draft for the Waveshare
ESP32-S3-Touch-AMOLED-1.75.

## Files

- `ESP32_S3_Touch_AMOLED_1_75_battery_case.scad` - OpenSCAD source.

## Suggested Battery

Start with a protected 1S LiPo pouch cell:

- Nominal voltage: 3.7 V
- Full charge: 4.2 V
- Connector: MX1.25 2P, polarity verified before plugging in
- Capacity target: 200-300 mAh
- Practical size target: about 30 x 20-25 x 3-4.5 mm

The default model reserves a `32 x 25 x 4.8 mm` battery pocket.

## How To Export STL

Install OpenSCAD, open the `.scad` file, then set:

```scad
part = "front";
```

Render with F6 and export STL as the front bezel.

Then set:

```scad
part = "back";
```

Render with F6 and export STL as the rear battery shell.

Use:

```scad
part = "assembly";
```

only for visual checking.

## Important Parameters

Change these first:

```scad
battery_x = 32.00;
battery_y = 25.00;
battery_z = 4.80;
back_height = 12.80;
case_od = 53.60;
xy_clearance = 0.35;
```

If the battery is thicker, increase both `battery_z` and `back_height`.

## Print Notes

- Material: PLA+ or PETG for first prototypes.
- Layer height: 0.12-0.20 mm.
- Wall count: 3.
- Infill: 20-35%.
- Print the front bezel face-down if your bed surface is clean.
- Print the rear shell with the back face on the bed.
- Use M2 screws. Start with M2 x 6 or M2 x 8 depending on the printed height.

## Fit Checks Before Final Print

The following items are estimated from product drawings and must be checked on
your real board:

- USB-C opening position.
- Side button positions.
- Bottom pin-header/service opening.
- Three M2 post positions.
- Battery connector and wire route.
- Whether the selected LiPo protection PCB hits the rear shell.

Do one low-quality draft print before printing the final shell.
