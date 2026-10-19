// check_js.js - syntax-check the JavaScript embedded in source files.
//
// Usage: node tools/check_js.js
//
// Checks:
//   - every <script> block in resources/launcher.html
//   - every C++ raw string in src/shim.h (the injected page script)
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
let failures = 0;

function check(label, code) {
  try {
    new Function(code);
    console.log(`OK    ${label} (${code.length} chars)`);
  } catch (err) {
    failures += 1;
    console.log(`FAIL  ${label}: ${err.message}`);
  }
}

// launcher.html script blocks
const launcherPath = path.join(root, 'resources', 'launcher.html');
const html = fs.readFileSync(launcherPath, 'utf8');
const blocks = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)];
if (!blocks.length) console.log('warn: no <script> blocks in launcher.html');
blocks.forEach((m, i) => check(`launcher.html script #${i}`, m[1]));

// shim.h raw strings
const shimPath = path.join(root, 'src', 'shim.h');
const shim = fs.readFileSync(shimPath, 'utf8');
const raw = /R"([A-Za-z_]*)\(([\s\S]*?)\)\1"/g;
let m;
let i = 0;
while ((m = raw.exec(shim)) !== null) {
  check(`shim.h raw string #${i}`, m[2]);
  i += 1;
}
if (i === 0) console.log('warn: no raw strings found in shim.h');

if (failures) {
  console.log(`\n${failures} failure(s)`);
  process.exit(1);
}
console.log('\nall JS parses cleanly');
