/**
 * PaperDarkRoom web flasher — Cloudflare Worker.
 *
 * Serves the static page (via the [assets] binding) plus three API routes over
 * the `paperdarkroom-fw` R2 bucket:
 *
 *   GET /api/firmwares          list of *-merged.bin, newest version first
 *   GET /fw/<key>               stream a firmware bin out of R2
 *   GET /manifest/<name>.json   ESP Web Tools manifest for one firmware
 *
 * Only `*-merged.bin` is exposed. `*-launcher.bin` is an app-slot image meant to
 * be handed to the on-device Launcher; writing it at 0x0 over USB would brick the
 * boot chain, so it must never show up as a flashable option.
 */

const MERGED_SUFFIX = "-merged.bin";
// adarkroom-0.12.0-merged.bin -> "0.12.0"
const VERSION_RE = /-(\d+(?:\.\d+)*(?:[-+][0-9A-Za-z.-]+)?)-merged\.bin$/;

// A firmware name carries its version and the bytes behind it never change, so
// the image is safe to keep forever. The listing is the one thing that has to
// stay fresh — a short window is enough to blunt refresh storms without hiding
// a new upload for long.
const IMMUTABLE = "public, max-age=31536000, immutable";
const LISTING_TTL = "public, max-age=60";
// The manifest is derived purely from the file name, but it 404s on a missing
// object, so keep it short enough that deleting a firmware takes effect.
const MANIFEST_TTL = "public, max-age=86400";

const json = (body, cacheControl) =>
  new Response(JSON.stringify(body), {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": cacheControl,
    },
  });

const parseVersion = (key) => {
  const m = VERSION_RE.exec(key);
  return m ? m[1] : null;
};

/** `1.2.3-rc.1+build` -> core numbers plus the prerelease tag, if any. */
const splitSemver = (v) => {
  const [core, pre = null] = v.split("+")[0].split(/-(.*)/s);
  return { core: core.split(".").map(Number), pre };
};

/**
 * Descending semver precedence. Build metadata is ignored.
 *
 * The subtlety is semver rule 11: a release outranks every prerelease of the
 * same core version (`1.0.0-beta` < `1.0.0`). Comparing the dotted segments
 * uniformly gets that backwards — the release simply runs out of segments, the
 * missing one reads as empty, and the prerelease sorts first and gets labelled
 * "最新". So prerelease has to be a separate step, not another segment.
 */
const compareVersionDesc = (a, b) => {
  const va = splitSemver(a);
  const vb = splitSemver(b);

  for (let i = 0; i < Math.max(va.core.length, vb.core.length); i++) {
    const na = va.core[i] ?? 0;
    const nb = vb.core[i] ?? 0;
    if (na !== nb) return nb - na;
  }

  // Same core version: a release comes first.
  if (va.pre === null || vb.pre === null) {
    if (va.pre === vb.pre) return 0;
    return va.pre === null ? -1 : 1;
  }

  // Both prereleases: compare dot-separated identifiers. Numeric ones compare
  // as numbers and rank below alphanumeric ones; a shorter run of identifiers
  // ranks below a longer one that matches so far.
  const ia = va.pre.split(".");
  const ib = vb.pre.split(".");
  for (let i = 0; i < Math.max(ia.length, ib.length); i++) {
    const x = ia[i];
    const y = ib[i];
    if (x === undefined) return 1;
    if (y === undefined) return -1;
    if (x === y) continue;
    const nx = /^\d+$/.test(x);
    const ny = /^\d+$/.test(y);
    if (nx && ny) return Number(y) - Number(x);
    if (nx !== ny) return nx ? 1 : -1;
    return x < y ? 1 : -1;
  }
  return 0;
};

async function listFirmwares(env) {
  const out = [];
  let cursor;
  do {
    const page = await env.FW.list({ cursor, limit: 1000 });
    for (const obj of page.objects) {
      if (!obj.key.endsWith(MERGED_SUFFIX)) continue;
      const version = parseVersion(obj.key);
      if (!version) continue;
      out.push({
        name: obj.key,
        version,
        size: obj.size,
        uploaded: obj.uploaded.toISOString(),
      });
    }
    cursor = page.truncated ? page.cursor : undefined;
  } while (cursor);

  out.sort(
    (a, b) =>
      compareVersionDesc(a.version, b.version) ||
      b.uploaded.localeCompare(a.uploaded),
  );
  return out;
}

const firmwareHeaders = (obj) => {
  const headers = new Headers();
  obj.writeHttpMetadata(headers);
  headers.set("content-type", "application/octet-stream");
  headers.set("content-length", String(obj.size));
  headers.set("etag", obj.httpEtag);
  headers.set("cache-control", IMMUTABLE);
  return headers;
};

async function serveFirmware(request, env, ctx, key) {
  if (!key.endsWith(MERGED_SUFFIX)) return new Response("Not found", { status: 404 });

  // HEAD only needs the metadata; pulling the body just to throw it away would
  // be a wasted R2 read.
  if (request.method === "HEAD") {
    const head = await env.FW.head(key);
    if (!head) return new Response("Not found", { status: 404 });
    return new Response(null, { headers: firmwareHeaders(head) });
  }

  // These images are ~1.4 MB and every player downloads the same handful, so
  // park them in the edge cache: repeat downloads are then served without an R2
  // read at all. Cache API keys on the request, and only GET may be stored.
  const cache = caches.default;
  const cached = await cache.match(request);
  if (cached) return cached;

  const obj = await env.FW.get(key);
  if (!obj) return new Response("Not found", { status: 404 });

  const response = new Response(obj.body, { headers: firmwareHeaders(obj) });
  // Don't make the user wait on the cache write.
  ctx.waitUntil(cache.put(request, response.clone()));
  return response;
}

async function serveManifest(env, name) {
  // Accept both `adarkroom-0.12.0-merged.json` and `...-merged.bin.json`.
  const key = name.endsWith(".bin") ? name : `${name}.bin`;
  const version = parseVersion(key);
  if (!version) return new Response("Not found", { status: 404 });

  const head = await env.FW.head(key);
  if (!head) return new Response("Not found", { status: 404 });

  return json(
    {
      name: "PaperDarkRoom",
      version,
      // The device speaks no Improv Serial, so skip the probe entirely instead
      // of making every user sit through the post-install wait.
      new_install_improv_wait_time: 0,
      // Show the "Erase device" checkbox rather than silently full-erasing.
      // First install / major upgrades want it checked (nvs must be re-paired);
      // minor updates can leave it off.
      new_install_prompt_erase: true,
      builds: [
        {
          // merged image already contains bootloader + partition table + app,
          // laid out at their real offsets, so it is written whole at 0x0.
          chipFamily: "ESP32-S3",
          parts: [{ path: `/fw/${key}`, offset: 0 }],
        },
      ],
    },
    MANIFEST_TTL,
  );
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const { pathname } = url;

    if (request.method !== "GET" && request.method !== "HEAD") {
      return new Response("Method not allowed", { status: 405 });
    }

    if (pathname === "/api/firmwares") {
      return json(await listFirmwares(env), LISTING_TTL);
    }

    if (pathname.startsWith("/fw/")) {
      return serveFirmware(
        request,
        env,
        ctx,
        decodeURIComponent(pathname.slice("/fw/".length)),
      );
    }

    if (pathname.startsWith("/manifest/") && pathname.endsWith(".json")) {
      const name = decodeURIComponent(
        pathname.slice("/manifest/".length, -".json".length),
      );
      return serveManifest(env, name);
    }

    return new Response("Not found", { status: 404 });
  },
};
