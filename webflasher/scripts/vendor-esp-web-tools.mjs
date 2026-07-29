// Copy the prebuilt esp-web-tools browser bundle into public/, then patch its
// user-visible English into Chinese.
//
// `esp-web-tools/dist/web/` is the vendor's own rollup output: `install-button.js`
// plus hashed chunks, all referenced by *relative* specifiers, including the
// dynamic `import()`s that pull in the per-chip esptool stub flashers. Copying it
// verbatim keeps that code-splitting intact — re-bundling it ourselves would have
// to reproduce the dynamic-import graph for no benefit.
//
// Upstream has no i18n: every string is hardcoded in a lit template. Since the
// version is pinned (10.4.0) the strings are fixed, so a build-time
// search-and-replace is deterministic. Two details make it safe:
//
//   * The minifier leaves template-literal contents alone, so the English still
//     carries lit's source indentation and line wrapping. Phrases are therefore
//     matched with `\s+` between words rather than as literal one-line strings.
//   * Every entry must match at least once or the build fails. That is the
//     tripwire for a future esp-web-tools bump silently dropping a translation.
//
// Run after every `npm install`/upgrade: `npm run vendor`.

import { cp, rm, readdir, readFile, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";

const root = new URL("../", import.meta.url);
const src = new URL("node_modules/esp-web-tools/dist/web/", root);
const dest = new URL("public/vendor/esp-web-tools/", root);

// Full sentences and unambiguous fragments. Matched anywhere, whitespace-flexibly.
// Longest source first at replace time, so "Preparing installation..." cannot be
// clipped by "Preparing installation".
const PHRASES = {
  // -- no-port-picked dialog (the one users hit when the port picker is empty)
  "No port selected": "未选择串口",
  "If you didn't select a port because you didn't see your device listed, try the following steps:":
    "如果因为列表里没有你的设备而无法选择串口，请依次检查：",
  "Make sure that the device is connected to this computer (the one that runs the browser that shows this website)":
    "确认设备已连接到这台电脑（正在显示本页面的这一台）",
  "Most devices have a tiny light when it is powered on. If yours has one, make sure it is on.":
    "多数设备通电后会亮一颗指示灯。如果你的设备有，确认它是亮的。",
  "Make sure that the USB cable you use can be used for data and is not a power-only cable.":
    "确认用的 USB 线支持数据传输，而不是只能充电的电源线。",
  "If you are using a Linux flavor, make sure that your user is part of the":
    "如果你用的是 Linux，确认当前用户已加入",
  "group so it has permission to access the device.": "用户组，否则没有访问设备的权限。",
  "You may need to log out & back in or reboot to activate the new group access.":
    "可能需要重新登录或重启，新的用户组权限才会生效。",
  "Make sure you have the right drivers installed. Below are the drivers for common chips used in ESP devices:":
    "确认已装好串口驱动。以下是 ESP 设备常见串口芯片的驱动（M5PaperS3 用 ESP32-S3 原生 USB，不需要装）：",
  "CP2102 drivers:": "CP2102 驱动：",
  "CH342, CH343, CH9102 drivers:": "CH342、CH343、CH9102 驱动：",
  "CH340, CH341 drivers:": "CH340、CH341 驱动：",
  "Windows & Mac": "Windows 与 Mac",
  "(download via blue button with": "（点击带",
  "icon)": "图标的蓝色按钮下载）",

  // -- install dialog: errors
  "Serial port is not readable/writable. Close any other application using it and try again.":
    "串口不可读写。请关闭占用该串口的其他程序后重试。",
  "Serial port is not ready. Close any other application using it and try again.":
    "串口未就绪。请关闭占用该串口的其他程序后重试。",
  "Failed to initialize. Try resetting your device or holding the BOOT button while clicking INSTALL.":
    "初始化失败。请复位设备，或按住 BOOT 键的同时点击刷入。",
  "Failed to download manifest": "固件清单下载失败",

  // -- install dialog: progress and outcomes
  "Preparing installation...": "正在准备刷写…",
  "Preparing installation": "正在准备刷写",
  "Installation prepared": "刷写准备完成",
  "Erasing flash (this may take a while)...": "正在擦除 flash（可能要等一会儿）…",
  "Erasing device...": "正在擦除设备…",
  "Device erased": "设备已擦除",
  "Erasing": "正在擦除",
  "Installing": "正在刷写",
  "Wrapping up": "收尾中",
  "Writing complete": "写入完成",
  "Writing progress:": "写入进度：",
  "Installation complete!": "刷写完成！",
  "Installation failed": "刷写失败",
  "All done!": "全部完成！",
  // progress body: "This will take ${...}.<br />Keep this page visible..."
  "This will take": "大约需要",
  "a minute": "1 分钟",
  "2 minutes": "2 分钟",
  "Keep this page visible to prevent slow down": "请保持本页面可见，切到后台会变慢",

  // -- install dialog: headings and menu entries
  "Confirm Installation": "确认刷写",
  "Erase User Data": "擦除用户数据",
  "Erase device": "擦除设备",
  "Do you want to reset your device and erase all user data from your device?":
    "要复位设备，并擦除设备上的全部用户数据吗？",
  "Logs & Console": "日志与控制台",
  "Download Logs": "下载日志",
  "Reset Device": "复位设备",
  "Visit Device": "访问设备",

  // -- install-button fallbacks
  "You can only install ESP devices on HTTPS websites or on the localhost.":
    "只能在 HTTPS 页面或 localhost 上刷写 ESP 设备。",
  "Your browser does not support installing things on ESP devices. Use Mozilla Firefox, Google Chrome or Microsoft Edge.":
    "此浏览器不支持给 ESP 设备刷写固件，请改用 Google Chrome 或 Microsoft Edge。",

  // -- console
  "Text copied to clipboard!": "已复制到剪贴板！",
  "Failed to copy text:": "复制失败：",
};

// Short labels that are also common substrings, so they are only replaced when
// they form a whole HTML text node (`>Next<`).
const LABELS = {
  Next: "下一步",
  Back: "返回",
  Close: "关闭",
  Cancel: "取消",
  Install: "刷入",
  Continue: "继续",
  Skip: "跳过",
  "Try Again": "重试",
};

// Strings carrying `${...}` interpolation. The capture keeps the expression
// (with whatever the minifier renamed it to) exactly as-is.
const PATTERNS = [
  [/Install\s+\$\{([^{}]+)\}/g, "刷入 ${$1}"],
  [/Update\s+\$\{([^{}]+)\}/g, "更新 ${$1}"],
  [
    /Do\s+you\s+want\s+to\s+erase\s+the\s+device\s+before\s+installing\s+\$\{([^{}]+)\}\?\s+All\s+data\s+on\s+the\s+device\s+will\s+be\s+lost\./g,
    "刷入 ${$1} 前要先擦除设备吗？设备上的数据会全部丢失。",
  ],
  [/Initialized\.\s+Found\s+\$\{([^{}]+)\}/g, "初始化完成，识别到 ${$1}"],
  [
    /Your\s+\$\{([^{}]+)\}\s+board\s+is\s+not\s+supported\./g,
    "不支持 ${$1} 开发板。",
  ],
  [
    /Downloading\s+firmware\s+\$\{([^{}]+)\}\s+failed:\s+\$\{([^{}]+)\}/g,
    "固件 ${$1} 下载失败：${$2}",
  ],
  // While erasing, the dialog's "Back" button becomes "Skip". Both are plain JS
  // strings in a ternary, not HTML text nodes, so LABELS can't reach them.
  [/"Skip"\s*:\s*"Back"/g, '"跳过":"返回"'],
];

const escapeRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
// Words may be split across lines in the bundle, so match runs of whitespace.
const flexible = (phrase) => new RegExp(escapeRe(phrase).replace(/\s+/g, "\\s+"), "g");

await rm(fileURLToPath(dest), { recursive: true, force: true });
await cp(fileURLToPath(src), fileURLToPath(dest), { recursive: true });

const files = (await readdir(fileURLToPath(dest))).filter((f) => f.endsWith(".js"));

// We always hand `.replace()` a function (so we can count hits), and a function
// replacer's return value is taken literally — `$1` is NOT expanded. Do it here.
const expand = (tpl, args) => tpl.replace(/\$(\d)/g, (_, d) => args[Number(d)]);

const jobs = [
  // Longest first so a shorter phrase can't eat a longer one's prefix.
  ...Object.entries(PHRASES)
    .sort((a, b) => b[0].length - a[0].length)
    .map(([en, zh]) => ({ label: en, re: flexible(en), to: () => zh })),
  ...Object.entries(LABELS).map(([en, zh]) => ({
    label: `>${en}<`,
    re: new RegExp("(>)\\s*" + escapeRe(en) + "\\s*(<)", "g"),
    to: (m, a, b) => a + zh + b,
  })),
  ...PATTERNS.map(([re, zh]) => ({
    label: String(re).slice(0, 46) + "…",
    re,
    to: (...args) => expand(zh, args),
  })),
];

const hits = new Map(jobs.map((j) => [j.label, 0]));

for (const file of files) {
  const path = new URL(file, dest);
  let text = await readFile(path, "utf8");
  let touched = false;
  for (const job of jobs) {
    job.re.lastIndex = 0;
    text = text.replace(job.re, (...args) => {
      hits.set(job.label, hits.get(job.label) + 1);
      touched = true;
      return job.to(...args);
    });
  }
  if (touched) await writeFile(path, text, "utf8");
}

const missed = [...hits].filter(([, n]) => n === 0).map(([label]) => label);
if (missed.length) {
  console.error(
    `[vendor] ${missed.length} translation(s) matched nothing — esp-web-tools ` +
      `probably changed its wording:\n  ` + missed.join("\n  "),
  );
  process.exit(1);
}

// A bad replacement would corrupt a template literal; catch it here, not in the
// browser. package.json has "type": "module", so these parse as ESM.
for (const file of files) {
  execFileSync(process.execPath, ["--check", fileURLToPath(new URL(file, dest))]);
}

const total = [...hits.values()].reduce((a, b) => a + b, 0);
console.log(
  `[vendor] copied ${files.length} files -> public/vendor/esp-web-tools/; ` +
    `localised ${hits.size} entries, ${total} replacements, all chunks parse`,
);
