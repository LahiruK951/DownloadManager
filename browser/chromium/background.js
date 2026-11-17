// FluxDM browser integration (Chrome / Edge / Brave, Manifest V3).
// Catches browser downloads and hands them to FluxDM through the native host,
// together with the cookies, referrer and user agent the server may require.

const HOST = "com.fluxdm.nativehost";
const DEFAULTS = { enabled: true, minSizeKB: 0, skipExtensions: "" };

// URLs we re-download in the browser after FluxDM was unavailable (don't catch them twice).
const bypass = new Set();

async function getSettings() {
  return Object.assign({}, DEFAULTS, await chrome.storage.sync.get(DEFAULTS));
}

function sendToHost(message) {
  return new Promise((resolve) => {
    chrome.runtime.sendNativeMessage(HOST, message, (reply) => {
      if (chrome.runtime.lastError) {
        resolve({ ok: false, error: chrome.runtime.lastError.message });
      } else {
        resolve(reply || { ok: false, error: "no reply" });
      }
    });
  });
}

async function cookieHeader(url) {
  try {
    const cookies = await chrome.cookies.getAll({ url });
    return cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  } catch (e) {
    return "";
  }
}

function baseName(path) {
  return (path || "").split(/[\\/]/).pop();
}

async function handOff(url, referrer, fileName) {
  return sendToHost({
    cmd: "download",
    url,
    referrer: referrer || "",
    cookies: await cookieHeader(url),
    userAgent: navigator.userAgent,
    fileName: fileName || "",
  });
}

function skippedByExtension(url, list) {
  const exts = list.split(/[\s,;]+/).map((e) => e.replace(/^\./, "").toLowerCase()).filter(Boolean);
  if (exts.length === 0) return false;
  try {
    const name = new URL(url).pathname.split("/").pop().toLowerCase();
    return exts.some((e) => name.endsWith("." + e));
  } catch (e) {
    return false;
  }
}

chrome.downloads.onCreated.addListener(async (item) => {
  const url = item.finalUrl || item.url;
  if (bypass.has(url)) {
    bypass.delete(url);
    return;
  }
  const s = await getSettings();
  if (!s.enabled) return;
  if (!/^(https?|ftp):/i.test(url)) return; // blob:, data:, file: stay in the browser
  if (item.byExtensionId === chrome.runtime.id) return;
  if (s.minSizeKB > 0 && item.totalBytes > 0 && item.totalBytes < s.minSizeKB * 1024) return;
  if (skippedByExtension(url, s.skipExtensions)) return;

  // Stop the browser's own copy first so the file isn't downloaded twice.
  try {
    await chrome.downloads.cancel(item.id);
  } catch (e) {}

  const reply = await handOff(url, item.referrer, baseName(item.filename));
  if (reply.ok) {
    try {
      await chrome.downloads.erase({ id: item.id });
    } catch (e) {}
  } else {
    // FluxDM not available: let the browser download it after all.
    bypass.add(url);
    chrome.downloads.download({ url });
  }
});

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: "fluxdm-link",
    title: "Download with FluxDM",
    contexts: ["link", "image", "video", "audio"],
  });
});

chrome.contextMenus.onClicked.addListener((info) => {
  const url = info.linkUrl || info.srcUrl;
  if (url && /^(https?|ftp|magnet):/i.test(url)) handOff(url, info.pageUrl, "");
});

// Popup → background
chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  if (msg && msg.type === "ping") {
    sendToHost({ cmd: "ping" }).then(sendResponse);
    return true;
  }
  if (msg && msg.type === "show") {
    sendToHost({ cmd: "show" }).then(sendResponse);
    return true;
  }
  return false;
});
