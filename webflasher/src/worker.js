/**
 * PaperDarkRoom web flasher — Cloudflare Worker.
 *
 * Serves the static page (via the [assets] binding) plus three API routes, all
 * sourced from the repo's own GitHub Releases:
 *
 *   GET /api/firmwares          list of *-merged.bin, newest version first
 *   GET /fw/<key>               stream a firmware bin out of its release
 *   GET /manifest/<name>.json   ESP Web Tools manifest for one firmware
 *
 * The release assets *are* the source of truth: publishing a build is
 * `gh release create` and nothing else. This used to read an R2 bucket that had
 * to be hand-synced after every release, which is exactly the step that got
 * forgotten — a version would exist on GitHub and not on the flasher page.
 *
 * Only `*-merged.bin` is exposed. `*-launcher.bin` is an app-slot image meant to
 * be handed to the on-device Launcher; writing it at 0x0 over USB would brick the
 * boot chain, so it must never show up as a flashable option.
 */

const REPO = "kyya/PaperDarkRoom";
const RELEASES_API = `https://api.github.com/repos/${REPO}/releases?per_page=100`;

// Tags are `v<semver>` and assets keep their file name, so a firmware name is
// enough to rebuild its download URL — the flash path never has to touch the
// API (and so never inherits its rate limit).
const downloadUrl = (version, key) =>
  `https://github.com/${REPO}/releases/download/v${version}/${key}`;

// GitHub rejects API requests that arrive without one.
const UA = "paperdarkroom-flasher (+https://github.com/kyya/PaperDarkRoom)";

const MERGED_SUFFIX = "-merged.bin";
// adarkroom-0.12.0-merged.bin              -> "0.12.0"
// adarkroom-1.2.3-rc.1+build.5-merged.bin  -> "1.2.3-rc.1+build.5"
// Prerelease (`-rc.1`) and build metadata (`+build.5`) are independent optional
// segments, the same shape splitSemver() takes apart below. Treating the suffix
// as one `[-+]...` blob rejected a version carrying both.
const VERSION_RE =
  /-(\d+(?:\.\d+)*(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?)-merged\.bin$/;

// A firmware name carries its version and the bytes behind it never change, so
// the image is safe to keep forever. The listing is the one thing that has to
// stay fresh — a short window is enough to blunt refresh storms without hiding
// a new release for long.
const IMMUTABLE = "public, max-age=31536000, immutable";
const LISTING_TTL = "public, max-age=300";
// Served when GitHub could not be reached (see listFirmwares). Deliberately
// shorter than the fresh listing so the page recovers on the next refresh
// instead of pinning a stale answer in browser caches for the full window.
const STALE_TTL = "public, max-age=60";
// The "last known good" copy behind that fallback. Long, because a week-old
// listing is only wrong about releases published since — still far better than
// an empty page.
const LAST_GOOD_TTL = "public, max-age=604800";
// The manifest is derived purely from the file name, but it 404s on a missing
// asset, so keep it short enough that deleting a release takes effect.
const MANIFEST_TTL = "public, max-age=86400";
const EXISTS_TTL = "public, max-age=300";

const json = (body, cacheControl, extraHeaders) =>
  new Response(typeof body === "string" ? body : JSON.stringify(body), {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": cacheControl,
      ...extraHeaders,
    },
  });

const notFound = () => new Response("Not found", { status: 404 });

/**
 * Headers GitHub wants on API calls.
 *
 * Unauthenticated requests get 60/h per client IP, and a Worker leaves through
 * Cloudflare's shared egress addresses — an IP whose whole hourly budget some
 * other tenant may already have spent. This is not theoretical: the first
 * deploy of this file drew a flat `403 API rate limit exceeded for
 * 172.70.206.73` with `x-ratelimit-remaining: 0`, on a repo we had made zero
 * requests to. That is what the stale fallback in listFirmwares() exists for.
 * If it stops being enough, `wrangler secret put GITHUB_TOKEN` moves the limit
 * to 5000/h against the token instead of the IP, and this picks it up with no
 * other change.
 */
const apiHeaders = (env) => {
  const headers = {
    "user-agent": UA,
    accept: "application/vnd.github+json",
  };
  if (env.GITHUB_TOKEN) headers.authorization = `Bearer ${env.GITHUB_TOKEN}`;
  return headers;
};

// Exported for scripts/test-version.mjs; the Worker itself uses only the
// default export.
export const parseVersion = (key) => {
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
export const compareVersionDesc = (a, b) => {
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

/**
 * Cache API keys.
 *
 * The Cache API needs a Request as a key, and these are internal entries with
 * no route behind them, so they get their own `/__cache/` prefix on this
 * origin. They hold nothing that /api/firmwares does not already hand out
 * publicly, so a stray direct hit on one of these paths is harmless.
 */
const cacheKey = (request, name) =>
  new Request(`${new URL(request.url).origin}/__cache/${name}`);

/**
 * Last successful listing, kept in the isolate.
 *
 * The `last-good` cache entry is the copy that survives the isolate, and it
 * does work here (put/match verified against this worker on workers.dev). This
 * is just the cheaper first look: one variable, no await, and it still answers
 * in the contexts where the Cache API is documented as a no-op (dashboard
 * editor, playground previews).
 */
let lastGoodListing = null;

async function fetchListing(env) {
  const res = await fetch(RELEASES_API, { headers: apiHeaders(env) });
  if (!res.ok) throw new Error(`GitHub releases API: HTTP ${res.status}`);
  const releases = await res.json();

  const out = [];
  for (const release of releases) {
    for (const asset of release.assets ?? []) {
      if (!asset.name.endsWith(MERGED_SUFFIX)) continue;
      const version = parseVersion(asset.name);
      if (!version) continue;
      out.push({
        name: asset.name,
        version,
        size: asset.size,
        // Assets are uploaded once and never rewritten, so these agree; prefer
        // updated_at so a re-uploaded asset reports when it actually landed.
        uploaded: asset.updated_at || asset.created_at,
      });
    }
  }

  out.sort(
    (a, b) =>
      compareVersionDesc(a.version, b.version) ||
      b.uploaded.localeCompare(a.uploaded),
  );
  return JSON.stringify(out);
}

async function listFirmwares(request, env, ctx) {
  const cache = caches.default;
  const fresh = cacheKey(request, "firmwares");
  const lastGood = cacheKey(request, "firmwares-last-good");

  const hit = await cache.match(fresh);
  if (hit) return hit;

  try {
    const body = await fetchListing(env);
    lastGoodListing = body;
    ctx.waitUntil(cache.put(fresh, json(body, LISTING_TTL)));
    ctx.waitUntil(cache.put(lastGood, json(body, LAST_GOOD_TTL)));
    return json(body, LISTING_TTL);
  } catch (err) {
    // Rate limit, GitHub 5xx, network blip: serve the last good listing rather
    // than an error page. A slightly old list of firmwares still flashes fine —
    // every entry in it is still downloadable.
    const stale =
      lastGoodListing ?? (await cache.match(lastGood).then((r) => r?.text()));
    if (stale) return json(stale, STALE_TTL, { "x-firmwares-stale": "1" });
    // Nothing to fall back on: a cold isolate that has never seen a good
    // listing. Say so rather than serve an empty list, which the page would
    // render as "暂无可刷入的固件".
    return new Response(JSON.stringify({ error: String(err) }), {
      status: 502,
      headers: {
        "content-type": "application/json; charset=utf-8",
        "cache-control": "no-store",
      },
    });
  }
}

const firmwareHeaders = (upstream) => {
  const headers = new Headers();
  headers.set("content-type", "application/octet-stream");
  // Both come from the asset store behind the release redirect; content-length
  // is what the flasher shows as progress, so pass it through rather than let
  // the response go chunked.
  const length = upstream.headers.get("content-length");
  if (length) headers.set("content-length", length);
  const etag = upstream.headers.get("etag");
  if (etag) headers.set("etag", etag);
  headers.set("cache-control", IMMUTABLE);
  return headers;
};

async function serveFirmware(request, ctx, key) {
  if (!key.endsWith(MERGED_SUFFIX)) return notFound();
  const version = parseVersion(key);
  if (!version) return notFound();
  // The key lands in the last path segment of the upstream URL, so one carrying
  // a slash could aim the fetch at an entirely different release or repo path.
  if (key.includes("/")) return notFound();

  const url = downloadUrl(version, key);

  // HEAD only needs the metadata, and GitHub answers it on the asset itself
  // (through the 302), so there is no body to throw away.
  if (request.method === "HEAD") {
    const head = await fetch(url, { method: "HEAD", headers: { "user-agent": UA } });
    if (!head.ok) return notFound();
    return new Response(null, { headers: firmwareHeaders(head) });
  }

  // These images are ~1.4 MB and every player downloads the same handful, so
  // park them in the edge cache: repeat downloads are then served without
  // touching GitHub at all. Cache API keys on the request, and only GET may be
  // stored.
  const cache = caches.default;
  const cached = await cache.match(request);
  if (cached) return cached;

  const upstream = await fetch(url, { headers: { "user-agent": UA } });
  if (upstream.status === 404) return notFound();
  // Anything else that went wrong is GitHub's, not the user's — don't cache it
  // and don't dress it up as a missing firmware.
  if (!upstream.ok) {
    return new Response("Upstream error", { status: 502 });
  }

  const response = new Response(upstream.body, { headers: firmwareHeaders(upstream) });
  // Don't make the user wait on the cache write.
  ctx.waitUntil(cache.put(request, response.clone()));
  return response;
}

/**
 * Does this exact asset exist on its release? One HEAD, briefly remembered —
 * ESP Web Tools fetches the manifest on every install click, and the answer
 * only changes when a release is published or pulled.
 */
async function firmwareExists(request, ctx, version, key) {
  const cache = caches.default;
  const marker = cacheKey(request, `exists/${encodeURIComponent(key)}`);

  const hit = await cache.match(marker);
  if (hit) return (await hit.text()) === "1";

  const head = await fetch(downloadUrl(version, key), {
    method: "HEAD",
    headers: { "user-agent": UA },
  });
  ctx.waitUntil(
    cache.put(
      marker,
      new Response(head.ok ? "1" : "0", {
        headers: { "cache-control": EXISTS_TTL },
      }),
    ),
  );
  return head.ok;
}

async function serveManifest(request, ctx, name) {
  // Accept both `adarkroom-0.12.0-merged.json` and `...-merged.bin.json`.
  const key = name.endsWith(".bin") ? name : `${name}.bin`;
  const version = parseVersion(key);
  if (!version) return notFound();
  if (key.includes("/")) return notFound();

  if (!(await firmwareExists(request, ctx, version, key))) return notFound();

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
      return listFirmwares(request, env, ctx);
    }

    if (pathname.startsWith("/fw/")) {
      return serveFirmware(
        request,
        ctx,
        decodeURIComponent(pathname.slice("/fw/".length)),
      );
    }

    if (pathname.startsWith("/manifest/") && pathname.endsWith(".json")) {
      const name = decodeURIComponent(
        pathname.slice("/manifest/".length, -".json".length),
      );
      return serveManifest(request, ctx, name);
    }

    return notFound();
  },
};
