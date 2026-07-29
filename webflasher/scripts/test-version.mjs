// Version parsing/ordering checks for the firmware listing. Run: npm test
//
// This is the bit of the Worker with no other guard around it, and review has
// already caught two bugs in it (a prerelease sorting above its own release,
// and a version with build metadata being rejected outright), so the cases
// live here rather than in a scratch file.

import { parseVersion, compareVersionDesc } from "../src/worker.js";

let failures = 0;

const check = (ok, label, detail) => {
  console.log(`${ok ? "PASS" : "FAIL"}  ${label}`);
  if (!ok) {
    failures++;
    if (detail) console.log(`      ${detail}`);
  }
};

const parses = (file, expected) =>
  check(
    parseVersion(file) === expected,
    `parse ${file} -> ${expected}`,
    `got ${JSON.stringify(parseVersion(file))}`,
  );

const orders = (input, expected, label) => {
  const got = [...input].sort(compareVersionDesc);
  check(
    JSON.stringify(got) === JSON.stringify(expected),
    label,
    `got      ${got.join(" > ")}\n      expected ${expected.join(" > ")}`,
  );
};

// --- parseVersion ---------------------------------------------------------
parses("adarkroom-0.12.0-merged.bin", "0.12.0");
parses("adarkroom-1.2.3-rc.1-merged.bin", "1.2.3-rc.1");
parses("adarkroom-1.2.3+build.5-merged.bin", "1.2.3+build.5");
parses("adarkroom-1.2.3-rc.1+build.5-merged.bin", "1.2.3-rc.1+build.5");
// launcher images must never be offered for a whole-chip USB flash
parses("adarkroom-0.12.0-launcher.bin", null);
parses("adarkroom-merged.bin", null);

// --- compareVersionDesc ---------------------------------------------------
orders(["1.0.0-beta", "1.0.0"], ["1.0.0", "1.0.0-beta"], "release outranks its prerelease");
orders(
  ["1.0.0-beta", "0.12.0", "1.0.0", "0.12.0-rc.1"],
  ["1.0.0", "1.0.0-beta", "0.12.0", "0.12.0-rc.1"],
  "mixed releases and prereleases",
);
// semver.org rule 11's own precedence chain, descending
orders(
  [
    "1.0.0-alpha", "1.0.0", "1.0.0-beta.11", "1.0.0-rc.1",
    "1.0.0-alpha.1", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-alpha.beta",
  ],
  [
    "1.0.0", "1.0.0-rc.1", "1.0.0-beta.11", "1.0.0-beta.2",
    "1.0.0-beta", "1.0.0-alpha.beta", "1.0.0-alpha.1", "1.0.0-alpha",
  ],
  "semver rule 11 precedence chain",
);
orders(["0.9.0", "0.10.0", "0.2.0"], ["0.10.0", "0.9.0", "0.2.0"], "numeric, not lexical");
orders(["1.0", "1.0.1", "1"], ["1.0.1", "1.0", "1"], "short cores pad with 0");
// build metadata carries no precedence (semver rule 10)
orders(
  ["1.2.3-rc.1+build.5", "1.2.3"],
  ["1.2.3", "1.2.3-rc.1+build.5"],
  "build metadata does not lift a prerelease above its release",
);
orders(
  ["1.2.3-rc.1+build.5", "1.2.3-rc.2"],
  ["1.2.3-rc.2", "1.2.3-rc.1+build.5"],
  "build metadata ignored when ranking prereleases",
);
orders(
  ["1.2.3+build.1", "1.2.4"],
  ["1.2.4", "1.2.3+build.1"],
  "build metadata ignored against a higher core",
);

console.log(failures ? `\n${failures} failure(s)` : "\nall version checks pass");
process.exit(failures ? 1 : 0);
