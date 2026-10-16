'use strict';
// Packages the launcher as a standalone app: node package.js [out dir]
// (default dist/). The CMake build runs it into <build>/launcher, beside the
// game, which the launcher then finds by itself.
const path = require('node:path');
const { packager } = require('@electron/packager');

const out = path.resolve(process.argv[2] || path.join(__dirname, 'dist'));

packager({
  dir: __dirname,
  name: 'OpenStrikersLauncher',
  out,
  overwrite: true,
  prune: true,
  asar: true,
  ignore: [/^\/dist($|\/)/, /^\/test($|\/)/, /^\/package\.js$/],
}).then((paths) => {
  for (const p of paths) console.log(`launcher: packaged into ${p}`);
}).catch((err) => {
  console.error(`launcher: packaging failed: ${err.message}`);
  process.exit(1);
});
