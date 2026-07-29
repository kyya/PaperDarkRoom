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

const json = (body, init = {}) =>
  new Response(JSON.stringify(body), {
    ...init,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      ...init.headers,
    },
  });

const parseVersion = (key) => {
  const m = VERSION_RE.exec(key);
  return m ? m[1] : null;
};

/** Descending semver-ish compare; numeric segments compared as numbers. */
const compareVersionDesc = (a, b) => {
  const sa = a.split(/[.\-+]/);
  const sb = b.split(/[.\-+]/);
  for (let i = 0; i < Math.max(sa.length, sb.length); i++) {
    const na = Number(sa[i]);
    const nb = Number(sb[i]);
    if (Number.isNaN(na) || Number.isNaN(nb)) {
      const ca = (sa[i] ?? "").localeCompare(sb[i] ?? "");
      if (ca !== 0) return -ca;
    } else if (na !== nb) {
      return nb - na;
    }
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

async function serveFirmware(env, key) {
  if (!key.endsWith(MERGED_SUFFIX)) return new Response("Not found", { status: 404 });
  const obj = await env.FW.get(key);
  if (!obj) return new Response("Not found", { status: 404 });

  const headers = new Headers();
  obj.writeHttpMetadata(headers);
  headers.set("content-type", "application/octet-stream");
  headers.set("content-length", String(obj.size));
  headers.set("etag", obj.httpEtag);
  headers.set("cache-control", "public, max-age=3600");
  return new Response(obj.body, { headers });
}

async function serveManifest(env, name) {
  // Accept both `adarkroom-0.12.0-merged.json` and `...-merged.bin.json`.
  const key = name.endsWith(".bin") ? name : `${name}.bin`;
  const version = parseVersion(key);
  if (!version) return new Response("Not found", { status: 404 });

  const head = await env.FW.head(key);
  if (!head) return new Response("Not found", { status: 404 });

  return json({
    name: "PaperDarkRoom",
    version,
    // The device speaks no Improv Serial, so skip the probe entirely instead of
    // making every user sit through the post-install wait.
    new_install_improv_wait_time: 0,
    // Show the "Erase device" checkbox rather than silently full-erasing. First
    // install / major upgrades want it checked (nvs must be re-paired); minor
    // updates can leave it off.
    new_install_prompt_erase: true,
    builds: [
      {
        // merged image already contains bootloader + partition table + app,
        // laid out at their real offsets, so it is written whole at 0x0.
        chipFamily: "ESP32-S3",
        parts: [{ path: `/fw/${key}`, offset: 0 }],
      },
    ],
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const { pathname } = url;

    if (request.method !== "GET" && request.method !== "HEAD") {
      return new Response("Method not allowed", { status: 405 });
    }

    if (pathname === "/api/firmwares") {
      return json(await listFirmwares(env));
    }

    if (pathname.startsWith("/fw/")) {
      return serveFirmware(env, decodeURIComponent(pathname.slice("/fw/".length)));
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
