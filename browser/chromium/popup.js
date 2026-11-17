const DEFAULTS = { enabled: true, minSizeKB: 0, skipExtensions: "" };
const $ = (id) => document.getElementById(id);

async function load() {
  const s = Object.assign({}, DEFAULTS, await chrome.storage.sync.get(DEFAULTS));
  $("enabled").checked = s.enabled;
  $("minSizeKB").value = s.minSizeKB;
  $("skipExtensions").value = s.skipExtensions;

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
  });
}

$("enabled").addEventListener("change", save);
$("minSizeKB").addEventListener("change", save);
$("skipExtensions").addEventListener("change", save);
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
    kind.textContent = m.kind === "m3u8" ? "stream" : m.kind;
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

load();
loadMedia();
