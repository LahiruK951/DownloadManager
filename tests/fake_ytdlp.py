"""Stand-in for yt-dlp in FluxDM's tests: same command line, same output format, no network.

The video id in the URL (?v=...) picks the behaviour:
  ok    video + audio streams, then a merge into one .mp4
  gone  "ERROR: [youtube] gone: Video unavailable"
  slow  one stream, slowly (for stop/resume); continues an existing .part file
"""
import os
import sys
import time


def arg(name, default=None):
    a = sys.argv
    return a[a.index(name) + 1] if name in a else default


def main():
    url = sys.argv[-1]
    vid = url.split("v=")[-1] if "v=" in url else "x"
    folder = arg("-P", ".")
    title = "Test Clip"
    print(f"[youtube] {vid}: Downloading webpage", flush=True)
    if vid == "gone":
        print(f"ERROR: [youtube] {vid}: Video unavailable", file=sys.stderr, flush=True)
        return 1

    def stream(name, size, steps, delay=0.0):
        path = os.path.join(folder, name)
        part = path + ".part"
        start = os.path.getsize(part) if os.path.exists(part) else 0
        if start:
            print(f"[download] Resuming download at byte {start}", flush=True)
        print(f"[download] Destination: {path}", flush=True)
        with open(part, "ab") as f:
            done = start
            while done < size:
                n = min(size - done, size // steps + 1)
                f.write(b"v" * n)
                f.flush()
                done += n
                print(f"FLUXDM-PROGRESS {done} {size} NA 123456.0", flush=True)
                time.sleep(delay)
        os.replace(part, path)
        return path

    if vid == "slow":
        final = stream(f"{title} [{vid}].mp4", 400_000, 40, 0.1)
    else:
        v = stream(f"{title} [{vid}].f137.mp4", 300_000, 5)
        a = stream(f"{title} [{vid}].f140.m4a", 50_000, 3)
        final = os.path.join(folder, f"{title} [{vid}].mp4")
        print(f'[Merger] Merging formats into "{final}"', flush=True)
        with open(final, "wb") as out:
            for p in (v, a):
                out.write(open(p, "rb").read())
                os.remove(p)
    print(f"FLUXDM-FILE {final}", flush=True)
    return 0


sys.exit(main())
