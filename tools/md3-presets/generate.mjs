// Generates src/ThemePresets.generated.h from material-color-utilities.
// Usage (needs `npm i @material/material-color-utilities` next to this file):
//   node --import ./hook.mjs generate.mjs > ../../src/ThemePresets.generated.h
import {
  Hct, argbFromHex, hexFromArgb,
  SchemeFidelity, SchemeTonalSpot, SchemeNeutral, SchemeMonochrome,
} from '@material/material-color-utilities';

// The curated seed list. Add a row and regenerate to offer another 主题色.
// ASCII only - the localised names live in SettingsPage.cpp.
const seeds = [
  ['blue (default)', '#1A73E8'],
  ['teal',           '#009688'],
  ['green',          '#43A047'],
  ['amber',          '#FFB300'],
  ['orange',         '#FB8C00'],
  ['red',            '#E53935'],
  ['purple',         '#8E24AA'],
  ['blue grey',      '#455A64'],
];

// The four "safe for a classroom projector" palette styles, in index order.
const styles = [
  ['fidelity',   SchemeFidelity],
  ['tonal spot', SchemeTonalSpot],
  ['neutral',    SchemeNeutral],
  ['monochrome', SchemeMonochrome],
];

// Field order MUST match ThemePresets::Scheme in the emitted header.
const roles = [
  'primary', 'onPrimary', 'primaryContainer', 'onPrimaryContainer',
  'secondary', 'onSecondary', 'secondaryContainer', 'onSecondaryContainer',
  'tertiary', 'onTertiary', 'tertiaryContainer', 'onTertiaryContainer',
  'error', 'onError', 'errorContainer', 'onErrorContainer',
  'surfaceDim', 'surfaceBright', 'surfaceContainerLowest', 'surfaceContainerLow',
  'surfaceContainer', 'surfaceContainerHigh', 'surfaceContainerHighest',
  'onSurface', 'onSurfaceVariant', 'outline', 'outlineVariant',
  'inverseSurface', 'inverseOnSurface', 'inversePrimary', 'scrim',
];

const rgb = (argb) => {
  const h = hexFromArgb(argb).slice(1);
  return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16));
};
const hex2 = (n) => n.toString(16).toUpperCase().padStart(2, '0');
const cell = (a) => `{0x${hex2(a[0])}, 0x${hex2(a[1])}, 0x${hex2(a[2])}}`;

let o = '';
o += '// ===========================================================================\n';
o += '//  GENERATED FILE - DO NOT EDIT BY HAND.\n';
o += '//  MD3 colour schemes behind the 「主题色 / 调色板风格」 presets.\n';
o += '//  Source: @material/material-color-utilities, contrast level 0, one scheme\n';
o += '//  per (seed x style x mode). Regenerate with tools/md3-presets/generate.mjs.\n';
o += '// ===========================================================================\n';
o += '#pragma once\n\nnamespace ThemePresets {\n\n';
o += 'struct Rgb { unsigned char r, g, b; };\n\n';
o += '// Field order is the array order below: the roles are positionally laid out.\n';
o += 'struct Scheme {\n';
for (const r of roles) o += `    Rgb ${r};\n`;
o += '};\n\n';
o += `inline constexpr int kSeedCount  = ${seeds.length};\n`;
o += `inline constexpr int kStyleCount = ${styles.length};\n`;
o += 'inline constexpr int kModeCount  = 2;   // 0 = light, 1 = dark\n\n';
o += '// Seeds (index order):\n';
seeds.forEach(([name, hex], i) => { o += `//   ${i}  ${name}  ${hex}\n`; });
o += '// Styles: 0 fidelity  1 tonal spot  2 neutral  3 monochrome\n\n';
o += 'inline constexpr Scheme kSchemes[kSeedCount][kStyleCount][kModeCount] = {\n';
seeds.forEach(([sname, shex], si) => {
  o += `  { // seed ${si}: ${sname} ${shex}\n`;
  styles.forEach(([stname, Ctor], sti) => {
    o += `    { // style ${sti}: ${stname}\n`;
    [false, true].forEach((dark, mi) => {
      const s = new Ctor(Hct.fromInt(argbFromHex(shex)), dark, 0);
      o += `      { // ${dark ? 'dark' : 'light'}\n`;
      for (const r of roles) o += `        ${cell(rgb(s[r]))},\n`;
      o += '      },\n';
    });
    o += '    },\n';
  });
  o += '  },\n';
});
o += '};\n\n}   // namespace ThemePresets\n';
process.stdout.write(o);
