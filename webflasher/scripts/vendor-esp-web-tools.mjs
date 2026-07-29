// Copy the prebuilt esp-web-tools browser bundle into public/ so the page never
// touches a third-party CDN (unpkg/jsdelivr are unreliable from mainland China).
//
// `esp-web-tools/dist/web/` is the vendor's own rollup output: `install-button.js`
// plus hashed chunks, all referenced by *relative* specifiers, including the
// dynamic `import()`s that pull in the per-chip esptool stub flashers. Copying it
// verbatim keeps that code-splitting intact — re-bundling it ourselves would have
// to reproduce the dynamic-import graph for no benefit.
//
// Run after every `npm install`/upgrade: `npm run vendor`.

import { cp, rm, readdir } from "node:fs/promises";
import { fileURLToPath } from "node:url";

const root = new URL("../", import.meta.url);
const src = new URL("node_modules/esp-web-tools/dist/web/", root);
const dest = new URL("public/vendor/esp-web-tools/", root);

await rm(fileURLToPath(dest), { recursive: true, force: true });
await cp(fileURLToPath(src), fileURLToPath(dest), { recursive: true });

const files = await readdir(fileURLToPath(dest));
console.log(`[vendor] copied ${files.length} files -> public/vendor/esp-web-tools/`);
