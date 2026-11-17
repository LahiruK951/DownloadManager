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

load();
