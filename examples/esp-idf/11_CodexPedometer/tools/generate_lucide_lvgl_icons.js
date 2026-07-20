const fs = require("fs");
const os = require("os");
const path = require("path");
const { spawnSync } = require("child_process");

const projectDir = path.resolve(__dirname, "..");
const svgDir = path.join(projectDir, "assets", "lucide");
const outDir = path.join(projectDir, "main");

const browserCandidates = [
  process.env.CHROME_PATH,
  process.env.EDGE_PATH,
  "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
  "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
].filter(Boolean);

const specs = [
  { name: "ui_icon_clock", svg: "clock.svg", size: 24, color: "#F7FBFF", pad: 1 },
  { name: "ui_icon_battery", svg: "battery-full.svg", size: 34, color: "#9DFF35", pad: 1 },
  { name: "ui_icon_steps", svg: "footprints.svg", size: 54, color: "#9DFF35", pad: 3 },
  { name: "ui_icon_codex", svg: "terminal.svg", size: 54, color: "#FFD166", pad: 3 },
  { name: "ui_icon_distance", svg: "map-pin.svg", size: 30, color: "#18D7F5", pad: 2 },
  { name: "ui_icon_calories", svg: "flame.svg", size: 30, color: "#9DFF35", pad: 2 },
  { name: "ui_icon_motion", svg: "heart-pulse.svg", size: 30, color: "#FF5A70", pad: 2 },
  { name: "ui_icon_codex_card", svg: "terminal.svg", size: 30, color: "#18D7F5", pad: 2 },
  { name: "ui_icon_battery_card", svg: "battery-full.svg", size: 30, color: "#9DFF35", pad: 2 },
];

function findBrowser() {
  for (const candidate of browserCandidates) {
    if (candidate && fs.existsSync(candidate)) {
      return candidate;
    }
  }
  throw new Error("Chrome/Edge executable not found");
}

function escapeForTemplate(value) {
  return value.replace(/\\/g, "\\\\").replace(/`/g, "\\`").replace(/\$\{/g, "\\${");
}

function renderSvg(browser, svgText, spec) {
  const cleanedSvg = svgText
    .replace(/stroke="currentColor"/g, `stroke="${spec.color}"`)
    .replace(/width="24"/, `width="${spec.size}"`)
    .replace(/height="24"/, `height="${spec.size}"`);

  const html = `<!doctype html>
<html>
<body>
<canvas id="canvas" width="${spec.size}" height="${spec.size}"></canvas>
<script>
const svg = \`${escapeForTemplate(cleanedSvg)}\`;
const size = ${spec.size};
const pad = ${spec.pad};
const canvas = document.getElementById("canvas");
const ctx = canvas.getContext("2d", { willReadFrequently: true });
const img = new Image();
img.onload = () => {
  ctx.clearRect(0, 0, size, size);
  ctx.drawImage(img, pad, pad, size - pad * 2, size - pad * 2);
  const pixels = Array.from(ctx.getImageData(0, 0, size, size).data);
  document.body.textContent = "__PIXELS_BEGIN__" + pixels.join(",") + "__PIXELS_END__";
};
img.onerror = () => {
  document.body.textContent = "__PIXELS_ERROR__";
};
img.src = "data:image/svg+xml;base64," + btoa(unescape(encodeURIComponent(svg)));
</script>
</body>
</html>`;

  const tempPath = path.join(os.tmpdir(), `lucide-${spec.name}-${Date.now()}.html`);
  fs.writeFileSync(tempPath, html, "utf8");

  const result = spawnSync(browser, [
    "--headless=new",
    "--disable-gpu",
    "--no-first-run",
    "--disable-extensions",
    "--virtual-time-budget=1500",
    "--dump-dom",
    `file:///${tempPath.replace(/\\/g, "/")}`,
  ], {
    encoding: "utf8",
    maxBuffer: 16 * 1024 * 1024,
  });

  fs.rmSync(tempPath, { force: true });

  if (result.status !== 0) {
    throw new Error(`${path.basename(browser)} failed for ${spec.name}: ${result.stderr}`);
  }

  const match = result.stdout.match(/__PIXELS_BEGIN__([0-9,]+)__PIXELS_END__/);
  if (!match) {
    throw new Error(`No pixel data returned for ${spec.name}`);
  }

  const rgba = match[1].split(",").map((n) => Number(n));
  if (rgba.length !== spec.size * spec.size * 4) {
    throw new Error(`Bad pixel count for ${spec.name}: ${rgba.length}`);
  }

  const bgra = [];
  for (let i = 0; i < rgba.length; i += 4) {
    bgra.push(rgba[i + 2], rgba[i + 1], rgba[i], rgba[i + 3]);
  }
  return bgra;
}

function formatBytes(bytes) {
  const lines = [];
  for (let i = 0; i < bytes.length; i += 16) {
    const chunk = bytes.slice(i, i + 16)
      .map((b) => `0x${b.toString(16).padStart(2, "0")}`)
      .join(", ");
    lines.push(`    ${chunk},`);
  }
  return lines.join("\n");
}

function main() {
  const browser = findBrowser();
  const generatedAt = new Date().toISOString();

  const header = `#pragma once

#include "lvgl.h"

extern const lv_image_dsc_t ui_icon_steps;
extern const lv_image_dsc_t ui_icon_codex;
extern const lv_image_dsc_t ui_icon_distance;
extern const lv_image_dsc_t ui_icon_calories;
extern const lv_image_dsc_t ui_icon_motion;
extern const lv_image_dsc_t ui_icon_clock;
extern const lv_image_dsc_t ui_icon_battery;
extern const lv_image_dsc_t ui_icon_codex_card;
extern const lv_image_dsc_t ui_icon_battery_card;
`;

  let source = `#include "ui_icons.h"

/* Generated from Lucide SVG icons (${generatedAt}). Source SVG files are in assets/lucide. */
`;

  for (const spec of specs) {
    const svgPath = path.join(svgDir, spec.svg);
    const svgText = fs.readFileSync(svgPath, "utf8");
    const bytes = renderSvg(browser, svgText, spec);
    const dataName = `${spec.name}_data`;

    source += `
static const uint8_t ${dataName}[] = {
${formatBytes(bytes)}
};

const lv_image_dsc_t ${spec.name} = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_ARGB8888,
        .flags = 0,
        .w = ${spec.size},
        .h = ${spec.size},
        .stride = ${spec.size * 4},
        .reserved_2 = 0,
    },
    .data_size = sizeof(${dataName}),
    .data = ${dataName},
    .reserved = NULL,
    .reserved_2 = NULL,
};
`;
  }

  fs.writeFileSync(path.join(outDir, "ui_icons.h"), header, "utf8");
  fs.writeFileSync(path.join(outDir, "ui_icons.c"), source, "utf8");
  console.log(`Generated ${specs.length} icons using ${browser}`);
}

main();
