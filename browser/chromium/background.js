// FluxDM browser integration (Chrome / Edge / Brave / Firefox 128+, Manifest V3).
// Firefox runs this same file as a background script (see browser/firefox/manifest.json).
// Catches browser downloads and hands them to FluxDM through the native host,
// together with the cookies, referrer and user agent the server may require.

const HOST = "com.fluxdm.nativehost";
const DEFAULTS = { enabled: true, minSizeKB: 0, skipExtensions: "", skipSites: "" };

// URLs we re-download in the browser after FluxDM was unavailable (don't catch them twice).
const bypass = new Set();

// Alt+click on a link (reported by content.js): the browser keeps the next download.
let skipUntil = 0;

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

async function handOff(url, referrer, fileName, postData) {
  return sendToHost({
    cmd: "download",
    url,
    referrer: referrer || "",
    cookies: await cookieHeader(url),
    userAgent: navigator.userAgent,
    fileName: fileName || "",
    postData: postData || "",
  });
}

// Forms the browser submitted in the last minute, by URL: some servers only send the file as the
// answer to a POST, so FluxDM has to submit the same form again (FDM does this too).
const recentPosts = new Map();
chrome.webRequest.onBeforeRequest.addListener(
  (d) => {
    if (d.method !== "POST" || !d.requestBody) return;
    let body = "";
    if (d.requestBody.formData) {
      const p = new URLSearchParams();
      for (const [k, values] of Object.entries(d.requestBody.formData)) for (const v of values) p.append(k, v);
      body = p.toString();
    } else if (d.requestBody.raw && d.requestBody.raw[0] && d.requestBody.raw[0].bytes) {
      body = new TextDecoder().decode(d.requestBody.raw[0].bytes);
    }
    if (!body || body.length > 65536) return;
    recentPosts.set(d.url, { body, time: Date.now() });
    for (const [u, v] of recentPosts) if (Date.now() - v.time > 60000) recentPosts.delete(u);
  },
  { urls: ["<all_urls>"], types: ["main_frame", "sub_frame"] },
  ["requestBody"]
);

function postBodyFor(url) {
  const p = recentPosts.get(url);
  return p && Date.now() - p.time < 60000 ? p.body : "";
}

// "example.com" matches example.com and its subdomains; checked against the file's address and
// the page it came from.
function skippedBySite(urls, list) {
  const sites = list.split(/[\s,;]+/).map((s) => s.replace(/^\*?\.?/, "").toLowerCase()).filter(Boolean);
  if (sites.length === 0) return false;
  return urls.some((u) => {
    let host;
    try {
      host = new URL(u).hostname.toLowerCase();
    } catch (e) {
      return false;
    }
    return sites.some((s) => host === s || host.endsWith("." + s));
  });
}

// Size before deciding, when the browser doesn't know it yet (asks the server; 3 s at most).
async function remoteSize(url) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 3000);
  try {
    const r = await fetch(url, { method: "HEAD", credentials: "include", signal: ctl.signal });
    const n = parseInt(r.headers.get("content-length") || "", 10);
    return Number.isFinite(n) ? n : -1;
  } catch (e) {
    return -1;
  } finally {
    clearTimeout(timer);
  }
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
  if (Date.now() < skipUntil) { // Alt+click: the user wants the browser to download this one
    skipUntil = 0;
    return;
  }
  if (skippedByExtension(url, s.skipExtensions)) return;
  if (skippedBySite([url, item.referrer || ""], s.skipSites)) return;
  if (s.minSizeKB > 0) {
    const size = item.totalBytes > 0 ? item.totalBytes : await remoteSize(url);
    if (size >= 0 && size < s.minSizeKB * 1024) return;
  }

  // Stop the browser's own copy first so the file isn't downloaded twice.
  try {
    await chrome.downloads.cancel(item.id);
  } catch (e) {}

  const reply = await handOff(url, item.referrer, baseName(item.filename), postBodyFor(item.url) || postBodyFor(url));
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

// ---------------------------------------------------------------- media detection
// Remember HLS playlists, DASH manifests and media files each tab loads, so the popup can offer them
// (FDM's "download video from this page"). Kept in session storage: the service worker
// may be restarted at any time.

const MEDIA_RE = /\.(m3u8|mpd|mp4|webm|m4v|mov|mp3|m4a|flac)(\?|#|$)/i;
const MANIFESTS = ["m3u8", "mpd"]; // streams: the manifest is what FluxDM downloads
const MAX_PER_TAB = 30;

function mediaKind(url) {
  const m = MEDIA_RE.exec(new URL(url).pathname + "?");
  return m ? m[1].toLowerCase() : null;
}

async function tabMedia(tabId) {
  const key = "media:" + tabId;
  return (await chrome.storage.session.get(key))[key] || [];
}

async function setTabMedia(tabId, list) {
  await chrome.storage.session.set({ ["media:" + tabId]: list });
  const n = list.length;
  chrome.action.setBadgeText({ tabId, text: n ? String(n) : "" });
  chrome.action.setBadgeBackgroundColor({ tabId, color: "#2f7cf6" });
}

chrome.webRequest.onBeforeRequest.addListener(
  (details) => {
    if (details.tabId < 0) return;
    let kind;
    try {
      kind = mediaKind(details.url);
    } catch (e) {
      return;
    }
    if (!kind) return;
    // Segment-style requests (byte ranges of the same file) are deduplicated by URL without query.
    const plain = details.url.split("#")[0];
    tabMedia(details.tabId).then((list) => {
      if (list.some((m) => m.url === plain)) return;
      // On HLS/DASH pages the player fetches fMP4/ts segments by XHR: the manifest is the useful entry.
      if (!MANIFESTS.includes(kind) && details.type === "xmlhttprequest" && list.some((m) => MANIFESTS.includes(m.kind))) return;
      list.unshift({ url: plain, kind, time: Date.now() });
      setTabMedia(details.tabId, list.slice(0, MAX_PER_TAB));
    });
  },
  { urls: ["<all_urls>"], types: ["xmlhttprequest", "media", "other"] }
);

chrome.tabs.onUpdated.addListener((tabId, info) => {
  if (info.status === "loading" && info.url) setTabMedia(tabId, []);
});
chrome.tabs.onRemoved.addListener((tabId) => chrome.storage.session.remove("media:" + tabId));

// Popup → background
chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  if (msg && msg.type === "media") {
    tabMedia(msg.tabId).then(sendResponse);
    return true;
  }
  if (msg && msg.type === "download") {
    handOff(msg.url, msg.referrer, "").then(sendResponse);
    return true;
  }
  if (msg && msg.type === "skipNext") {
    skipUntil = Date.now() + 3000;
    return false;
  }
  if (msg && msg.type === "downloadMany") {
    // One Add dialog in FluxDM for all of them.
    sendToHost({ cmd: "downloadMany", urls: msg.urls, referrer: msg.referrer || "", userAgent: navigator.userAgent }).then(sendResponse);
    return true;
  }
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
