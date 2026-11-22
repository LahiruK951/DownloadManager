const DEFAULTS = { enabled: true, minSizeKB: 0, skipExtensions: "", skipSites: "" };
const $ = (id) => document.getElementById(id);

async function load() {
  const s = Object.assign({}, DEFAULTS, await chrome.storage.sync.get(DEFAULTS));
  $("enabled").checked = s.enabled;
  $("minSizeKB").value = s.minSizeKB;
  $("skipExtensions").value = s.skipExtensions;
  $("skipSites").value = s.skipSites;

  const reply = await chrome.runtime.sendMessage({ type: "ping" });
  const status = $("status");
  if (reply && reply.ok) {
    status.textContent = reply.running ? "Connected" : "Ready";
    status.className = "status ok";
  } else {
    status.textContent = "Not found";
    status.className = "status bad";
    $("help").hidden = false;
  }
}

function save() {
  chrome.storage.sync.set({
    enabled: $("enabled").checked,
    minSizeKB: Math.max(0, parseInt($("minSizeKB").value, 10) || 0),
    skipExtensions: $("skipExtensions").value.trim(),
    skipSites: $("skipSites").value.trim(),
  });
}

$("enabled").addEventListener("change", save);
$("minSizeKB").addEventListener("change", save);
$("skipExtensions").addEventListener("change", save);
$("skipSites").addEventListener("change", save);
$("open").addEventListener("click", async () => {
  await chrome.runtime.sendMessage({ type: "show" });
  window.close();
});

async function loadMedia() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab) return;
  const list = await chrome.runtime.sendMessage({ type: "media", tabId: tab.id });
  if (!list || list.length === 0) return;
  const ul = $("mediaList");
  for (const m of list) {
    const li = document.createElement("li");
    const kind = document.createElement("span");
    kind.className = "kind";
    kind.textContent = m.kind === "m3u8" || m.kind === "mpd" ? "stream" : m.kind;
    const name = document.createElement("span");
    name.className = "name";
    let label = m.url;
    try {
      const u = new URL(m.url);
      label = decodeURIComponent(u.pathname.split("/").filter(Boolean).slice(-2).join("/")) || u.host;
    } catch (e) {}
    name.textContent = label;
    name.title = m.url;
    const btn = document.createElement("button");
    btn.textContent = "Download";
    btn.addEventListener("click", async () => {
      btn.disabled = true;
      const r = await chrome.runtime.sendMessage({ type: "download", url: m.url, referrer: tab.url });
      btn.textContent = r && r.ok ? "Sent" : "Failed";
    });
    li.append(kind, name, btn);
    ul.append(li);
  }
  $("media").hidden = false;
}

async function loadLinks() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab || !/^https?:/.test(tab.url || "")) return;
  let links = [];
  try {
    links = (await chrome.tabs.sendMessage(tab.id, { type: "links" })) || [];
  } catch (e) {
    return; // page opened before the extension was installed, or a browser page
  }
  if (links.length === 0) return;
  const btn = $("allLinks");
  btn.textContent = links.length === 1 ? "Download the 1 file link on this page" : `Download all ${links.length} file links on this page`;
  btn.addEventListener("click", async () => {
    btn.disabled = true;
    const r = await chrome.runtime.sendMessage({ type: "downloadMany", urls: links, referrer: tab.url });
    btn.textContent = r && r.ok ? "Sent to FluxDM — choose them there" : "FluxDM was not found";
  });
  $("links").hidden = false;
}

load();
loadMedia();
loadLinks();
