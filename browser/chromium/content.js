// FluxDM content script: two small jobs on every page.
// 1. Alt+click on a link: the browser downloads it itself (FluxDM lets that one through).
// 2. Lists the page's links to files, for "Download all links" in the popup.

document.addEventListener(
  "click",
  (e) => {
    if (!e.altKey) return;
    const a = e.target && e.target.closest ? e.target.closest("a[href]") : null;
    if (a) chrome.runtime.sendMessage({ type: "skipNext" }).catch(() => {});
  },
  true
);

const FILE_RE =
  /\.(zip|rar|7z|tar|gz|tgz|bz2|xz|zst|iso|img|dmg|exe|msi|msix|appx|apk|deb|rpm|pkg|jar|pdf|epub|mobi|docx?|xlsx?|pptx?|odt|csv|mp3|m4a|flac|wav|ogg|opus|aac|mp4|mkv|avi|mov|webm|m4v|wmv|jpg|jpeg|png|gif|webp|svg|psd|torrent)$/i;

chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  if (!msg || msg.type !== "links") return false;
  const seen = new Set();
  for (const a of document.querySelectorAll("a[href]")) {
    let u;
    try {
      u = new URL(a.href, location.href);
    } catch (e) {
      continue;
    }
    if (u.protocol === "magnet:" || (/^(https?|ftp):$/.test(u.protocol) && FILE_RE.test(u.pathname))) {
      u.hash = "";
      seen.add(u.href);
    }
  }
  sendResponse([...seen].slice(0, 500));
  return false;
});
