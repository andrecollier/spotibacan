"""Audio bridge for a 3DS client.

One endless HTTP stream of raw PCM, paced in real time, from one of three
sources. The console never decodes anything: NDSP takes STEREO_PCM16 directly.

    python3 server/bridge.py --source local --path ~/Music
    python3 server/bridge.py --source radio --url http://example.com/stream
    python3 server/bridge.py --source spotify          # needs Spotify Premium

The stream never stops. When the source is idle, paused or reconnecting, the
broadcaster emits silence on schedule, so the console's ring buffer is always
fed and the client needs no underrun handling.

Endpoints:
    GET /stream   endless raw PCM (s16le, stereo, --rate Hz)
    GET /now      {"title","artist","album","playing"}
    GET /health   rate, listeners, chunk counters, source state
"""
import argparse
import errno
import json
import os
import queue
import random
import shutil
import signal
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

SPOTIFY_RATE = 44100      # librespot's pipe backend always emits 44.1 kHz stereo
CHANNELS = 2
AUDIO_SUFFIXES = {".mp3", ".m4a", ".aac", ".flac", ".ogg", ".opus", ".wav", ".wma", ".aiff"}

STATE_FILE = Path(os.environ.get("BRIDGE_STATE", "/tmp/3ds-spotify-now.json"))
LOG_DIR = Path(__file__).resolve().parent.parent / "logs"
source = {"state": "starting", "kind": "", "restarts": -1, "error": None}

# Timestamp of the file this process actually loaded. A running server that
# predates an edit looks identical to one that has it, which has cost us more
# debugging today than any real bug.
CODE_STAMP = time.strftime("%H:%M:%S", time.localtime(Path(__file__).stat().st_mtime))

ART_SIZE = 128                      # power of two: the GPU requires it
art = {"tiled": None, "track": None}
art_lock = threading.Lock()

# Transport state for the local library. "step" is how far the playlist moves
# when the current track ends: 1 normally, -1 after a previous, 0 to replay.
control = {"proc": None, "step": None, "goto": None, "paused": False,
           "seek": None, "base": 0.0, "started": 0.0, "paused_at": None,
           "duration": 0.0}
library = {"tracks": [], "root": None}
control_lock = threading.Lock()
BROADCASTER = None


def read_state():
    try:
        return json.loads(STATE_FILE.read_text())
    except Exception:
        return {"title": "", "artist": "", "album": "", "playing": False}


def write_state(**fields):
    state = read_state()
    state.update(fields)
    tmp = STATE_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state))
    tmp.replace(STATE_FILE)


def log_handle(name):
    LOG_DIR.mkdir(exist_ok=True)
    handle = (LOG_DIR / f"{name}.log").open("ab", buffering=0)
    handle.write(f"\n--- started {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n".encode())
    return handle


def last_error(name="librespot"):
    """The most recent ERROR line a child logged, for /health and the console."""
    try:
        lines = (LOG_DIR / f"{name}.log").read_text().splitlines()
    except OSError:
        return f"no {name} log"
    errors = [l for l in lines if "ERROR" in l or "error" in l.lower()]
    return errors[-1].split("] ", 1)[-1] if errors else "no error line logged"


def morton(x, y):
    """Offset of (x, y) inside an 8x8 tile, as the PICA200 stores it: the bits
    of x and y interleaved. citro3d has no linear-to-tiled helper, so the
    conversion happens here rather than on the console's CPU."""
    return ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1)
            | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))


TILE = [[morton(x, y) for x in range(8)] for y in range(8)]


def tile_rgb565(linear, size):
    """Reorder a linear RGB565 image into the GPU's 8x8 tiles."""
    out = bytearray(len(linear))
    tiles_per_row = size // 8
    for y in range(size):
        row = TILE[y & 7]
        tile_y = (y >> 3) * tiles_per_row
        base = y * size * 2
        for x in range(size):
            dst = ((tile_y + (x >> 3)) * 64 + row[x & 7]) * 2
            src = base + x * 2
            out[dst] = linear[src]
            out[dst + 1] = linear[src + 1]
    return bytes(out)


def extract_art(path):
    """Embedded cover art as tiled RGB565, letterboxed square. None if absent."""
    try:
        raw = subprocess.run(
            ["ffmpeg", "-v", "quiet", "-i", str(path), "-map", "0:v:0", "-frames:v", "1",
             "-vf", f"scale={ART_SIZE}:{ART_SIZE}:force_original_aspect_ratio=decrease,"
                    f"pad={ART_SIZE}:{ART_SIZE}:(ow-iw)/2:(oh-ih)/2",
             "-f", "rawvideo", "-pix_fmt", "rgb565le", "pipe:1"],
            capture_output=True, timeout=15).stdout
    except Exception:
        return None
    return tile_rgb565(raw, ART_SIZE) if len(raw) == ART_SIZE * ART_SIZE * 2 else None


class Broadcaster:
    """Paces audio to subscribers in real time, filling gaps with silence."""

    def __init__(self, rate, chunk_ms=120):
        self.rate = rate
        self.chunk = int(rate * chunk_ms / 1000) * 2 * CHANNELS
        self.period = chunk_ms / 1000
        self.silence = bytes(self.chunk)
        self.source = queue.Queue(maxsize=64)
        self.clients = []
        self.lock = threading.Lock()
        self.fed = self.filled = self.dropped = self.starved = 0

    def subscribe(self):
        q = queue.Queue(maxsize=48)
        with self.lock:
            self.clients.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)

    def feed(self, data):
        # Block briefly rather than drop: a file source decodes far faster than
        # real time, and silently discarding it would gut the audio.
        try:
            self.source.put(data, timeout=1.0)
        except queue.Full:
            self.dropped += 1

    def flush(self):
        """Drop buffered audio, e.g. when switching tracks or reconnecting."""
        try:
            while True:
                self.source.get_nowait()
        except queue.Empty:
            pass

    def run(self):
        pending = b""
        next_tick = time.monotonic()
        while True:
            while len(pending) < self.chunk:
                try:
                    pending += self.source.get_nowait()
                except queue.Empty:
                    break
            if len(pending) >= self.chunk:
                block, pending = pending[:self.chunk], pending[self.chunk:]
                self.fed += 1
            else:
                block = self.silence
                self.filled += 1
            with self.lock:
                targets = list(self.clients)
            for q in targets:
                try:
                    q.put_nowait(block)
                except queue.Full:
                    # A stalled listener must not stall the others, but the
                    # audio it misses has to show up somewhere: dropping it
                    # silently would look like clean playback from here while
                    # the console stutters.
                    self.starved += 1
            next_tick += self.period
            delay = next_tick - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                next_tick = time.monotonic()  # Fell behind; resynchronise.


def drain(stream, broadcaster, alive):
    """Move decoded PCM into the broadcaster until the child stops."""
    while alive():
        data = stream.read(4096)
        if not data:
            return
        broadcaster.feed(data)


def ffmpeg_args(rate, before_input, source_arg):
    return ["ffmpeg", "-loglevel", "warning", *before_input, "-i", source_arg,
            "-f", "s16le", "-ar", str(rate), "-ac", str(CHANNELS), "pipe:1"]


# --------------------------------------------------------------------------- sources

def run_spotify(args, broadcaster):
    """librespot advertises on the LAN; your phone hands it credentials, so
    this process never sees your password. Requires Spotify Premium."""
    hook = Path(__file__).resolve().parent / "onevent.py"
    spot = subprocess.Popen(
        ["librespot", "--name", args.name, "--backend", "pipe",
         "--device", "/dev/stdout", "--format", "S16",
         "--bitrate", args.bitrate, "--disable-audio-cache",
         "--device-type", "computer", "--onevent", str(hook)],
        stdout=subprocess.PIPE, stderr=log_handle("librespot"),
        env={**os.environ, "BRIDGE_STATE": str(STATE_FILE)})
    ff = subprocess.Popen(
        ffmpeg_args(args.rate, ["-f", "s16le", "-ar", str(SPOTIFY_RATE),
                                "-ac", str(CHANNELS)], "pipe:0"),
        stdin=spot.stdout, stdout=subprocess.PIPE, stderr=log_handle("ffmpeg"))
    spot.stdout.close()   # ffmpeg owns the read end now.
    try:
        drain(ff.stdout, broadcaster, lambda: spot.poll() is None and ff.poll() is None)
    finally:
        for proc in (spot, ff):
            if proc.poll() is None:
                proc.terminate()
    raise RuntimeError(f"librespot exited ({spot.poll()}): {last_error('librespot')}")


def probe_tags(path):
    """Read title/artist/album with ffprobe, falling back to the file name."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json", "-show_format", str(path)],
            capture_output=True, text=True, timeout=10).stdout
        fmt = json.loads(out).get("format", {})
        tags = {k.lower(): v for k, v in fmt.get("tags", {}).items()}
        seconds = float(fmt.get("duration", 0) or 0)
    except Exception:
        tags, seconds = {}, 0.0
    return {"title": tags.get("title") or path.stem,
            "artist": tags.get("artist", ""),
            "album": tags.get("album", ""),
            "duration": round(seconds, 1)}


def load_art(track):
    cover = extract_art(track)
    with art_lock:
        if art["track"] == str(track):   # a later skip wins
            art["tiled"] = cover


def run_local(args, broadcaster):
    """Play a music folder on shuffle, for ever. -re paces ffmpeg to real time;
    without it a four-minute track decodes in seconds and floods the queue."""
    root = Path(args.path).expanduser()
    tracks = sorted(p for p in root.rglob("*") if p.suffix.lower() in AUDIO_SUFFIXES)
    if not tracks:
        raise RuntimeError(f"No audio files under {root}")
    print(f"Local library: {len(tracks)} tracks under {root}", flush=True)
    library["tracks"], library["root"] = tracks, root
    errors = log_handle("ffmpeg")
    random.shuffle(tracks)
    index = 0
    while True:
        track = tracks[index]
        tags = probe_tags(track)
        with control_lock:
            control["duration"] = tags["duration"]
        write_state(playing=True, **tags)
        # Clear the old cover at once: showing the previous track's art while
        # the new one decodes is worse than showing the placeholder.
        with art_lock:
            art["tiled"], art["track"] = None, str(track)
        with control_lock:
            begin = control["seek"] or 0.0
            control["seek"] = None
        pre = ["-re"] + (["-ss", f"{begin:.2f}"] if begin > 0 else [])
        ff = subprocess.Popen(ffmpeg_args(args.rate, pre, str(track)),
                              stdout=subprocess.PIPE, stderr=errors)
        # Cover extraction spawns its own ffmpeg and can take most of a second.
        # Off the critical path it costs nothing; on it, every skip waits.
        threading.Thread(target=load_art, args=(track,), daemon=True).start()
        with control_lock:
            control.update(proc=ff, step=None, paused=False, base=begin,
                           started=time.monotonic(), paused_at=None)
        try:
            drain(ff.stdout, broadcaster, lambda: ff.poll() is None)
        finally:
            if ff.poll() is None:
                ff.terminate()
        with control_lock:
            # step is None when the track simply ended; any value means the
            # user asked for it. Testing the value instead of its presence
            # missed "next", which shares step 1 with a natural end.
            seeking = control["seek"] is not None
            requested = seeking or control["step"] is not None or control["goto"] is not None
            target = control["goto"]
            step = control["step"] if control["step"] is not None else 1
            control.update(proc=None, step=None, goto=None, paused=False)
        if requested:
            broadcaster.flush()
        if seeking:
            continue                      # same track, new offset
        index = target % len(tracks) if target is not None else (index + step) % len(tracks)


def run_radio(args, broadcaster):
    """Relay an internet station, reconnecting on drop. The network paces it."""
    write_state(playing=True, title=args.station or args.url, artist="", album="")
    errors = log_handle("ffmpeg")
    ff = subprocess.Popen(
        ffmpeg_args(args.rate, ["-reconnect", "1", "-reconnect_streamed", "1",
                                "-reconnect_delay_max", "5"], args.url),
        stdout=subprocess.PIPE, stderr=errors)
    try:
        drain(ff.stdout, broadcaster, lambda: ff.poll() is None)
    finally:
        if ff.poll() is None:
            ff.terminate()
    raise RuntimeError(f"stream ended ({ff.poll()}): {last_error('ffmpeg')}")


SOURCES = {"spotify": run_spotify, "local": run_local, "radio": run_radio}


def supervise(args, broadcaster):
    """Restart the source when it stops. A source that dies instantly three
    times running is misconfigured, not flaky; retrying for ever would only
    bury the reason in the log."""
    runner = SOURCES[args.source]
    quick = 0
    while True:
        source["restarts"] += 1
        source.update(state="running", kind=args.source, error=None)
        began = time.monotonic()
        try:
            runner(args, broadcaster)
            reason = "source ended"
        except Exception as exc:
            reason = str(exc)
        lived = time.monotonic() - began
        broadcaster.flush()
        write_state(playing=False)
        quick = quick + 1 if lived < 20 else 0
        if quick >= 3:
            source.update(state="fatal", error=reason)
            print(f"Source keeps failing at startup. Giving up.\n  {reason}", flush=True)
            return
        source.update(state="restarting", error=reason)
        print(f"Source stopped after {lived:.0f}s: {reason}\nRestarting in 3s.", flush=True)
        time.sleep(3)


# --------------------------------------------------------------------------- http

def position():
    """Seconds into the current track. ffmpeg -re decodes in real time, so
    elapsed wall clock minus any paused stretch is the playback position."""
    with control_lock:
        if control["proc"] is None:
            return 0.0
        end = control["paused_at"] or time.monotonic()
        return round(control["base"] + max(0.0, end - control["started"]), 1)


def transport(action):
    """next/prev/toggle/pause/play for the local library.

    Pause suspends the decoder rather than tearing it down, so resuming picks
    up mid-track. The broadcaster keeps emitting silence meanwhile, so the
    console's stream never ends and it needs no special handling.
    """
    with control_lock:
        proc = control["proc"]
        if proc is None or proc.poll() is not None:
            return {"ok": False, "reason": "no track is playing"}
        if action.startswith("play/"):
            try:
                control["goto"] = int(action.split("/", 1)[1])
            except ValueError:
                return {"ok": False, "reason": "play needs a track number"}
            if control["paused"]:
                proc.send_signal(signal.SIGCONT)
                control["paused"] = False
            proc.terminate()
            if BROADCASTER is not None:
                BROADCASTER.flush()
            return {"ok": True, "goto": control["goto"]}
        if action.startswith("seek/"):
            try:
                where = max(0.0, float(action.split("/", 1)[1]))
            except ValueError:
                return {"ok": False, "reason": "seek needs seconds"}
            control["seek"] = where
            if control["paused"]:
                proc.send_signal(signal.SIGCONT)
                control["paused"] = False
            proc.terminate()
            if BROADCASTER is not None:
                BROADCASTER.flush()
            return {"ok": True, "seek": where}
        if action in ("next", "prev", "restart"):
            control["step"] = {"next": 1, "prev": -1, "restart": 0}[action]
            if control["paused"]:
                proc.send_signal(signal.SIGCONT)   # a stopped child cannot exit
                control["paused"] = False
            proc.terminate()
            # Drop the tail of the old track at once; waiting for the decoder
            # to die first leaves a second of it queued.
            if BROADCASTER is not None:
                BROADCASTER.flush()
            return {"ok": True, "action": action}
        if action in ("pause", "play", "toggle"):
            want_pause = action == "pause" or (action == "toggle" and not control["paused"])
            if want_pause == control["paused"]:
                return {"ok": True, "paused": control["paused"]}
            proc.send_signal(signal.SIGSTOP if want_pause else signal.SIGCONT)
            control["paused"] = want_pause
            now = time.monotonic()
            if want_pause:
                control["paused_at"] = now
            elif control["paused_at"]:
                control["started"] += now - control["paused_at"]
                control["paused_at"] = None
            # Without this the already-queued seconds keep playing after the
            # press, which reads as a pause that did not work.
            if want_pause and BROADCASTER is not None:
                BROADCASTER.flush()
            write_state(playing=not want_pause)
            return {"ok": True, "paused": want_pause}
    return {"ok": False, "reason": f"unknown action: {action}"}


def make_handler(broadcaster, rate):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass  # The console reconnects often; keep the console quiet.

        def _json(self, payload):
            body = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.startswith("/now"):
                payload = read_state()
                with art_lock:
                    payload["art"] = art["tiled"] is not None
                payload["art_size"] = ART_SIZE
                payload["position"] = position()
                with control_lock:
                    payload["duration"] = control["duration"]
                return self._json(payload)
            if self.path.startswith("/control/"):
                action = self.path.split("/control/", 1)[1].split("?")[0]
                return self._json(transport(action))
            if self.path.startswith("/library"):
                query = parse_qs(urlparse(self.path).query)
                needle = (query.get("q", [""])[0] or "").lower()
                offset = int(query.get("offset", ["0"])[0] or 0)
                limit = min(int(query.get("limit", ["20"])[0] or 20), 60)
                tracks, root = library["tracks"], library["root"]
                # Searching file paths rather than tags: reading tags for
                # thousands of files takes minutes, and paths usually carry the
                # artist and album as folder names anyway.
                if needle:
                    words = needle.split()
                    hits = [(i, t) for i, t in enumerate(tracks)
                            if all(w in str(t.relative_to(root)).lower() for w in words)]
                else:
                    hits = list(enumerate(tracks))
                window = hits[offset:offset + limit]
                return self._json({
                    "total": len(hits), "offset": offset,
                    "items": [{"i": i, "name": t.stem[:60],
                               "dir": t.parent.name[:40]} for i, t in window]})
            if self.path.startswith("/art"):
                with art_lock:
                    cover = art["tiled"]
                if cover is None:
                    return self.send_error(404)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(cover)))
                self.send_header("X-Art-Size", str(ART_SIZE))
                self.end_headers()
                return self.wfile.write(cover)
            if self.path.startswith("/health"):
                return self._json({"code_loaded": CODE_STAMP,
                                   "rate": rate, "channels": CHANNELS,
                                   "listeners": len(broadcaster.clients),
                                   "audio_chunks": broadcaster.fed,
                                   "silence_chunks": broadcaster.filled,
                                   "dropped_chunks": broadcaster.dropped,
                                   "listener_drops": broadcaster.starved,
                                   **{"source_" + k: v for k, v in source.items()}})
            if not self.path.startswith("/stream"):
                return self.send_error(404)
            self.send_response(200)
            self.send_header("Content-Type", "audio/L16")
            self.send_header("X-Sample-Rate", str(rate))
            self.send_header("X-Channels", str(CHANNELS))
            self.send_header("Connection", "close")
            self.end_headers()
            q = broadcaster.subscribe()
            try:
                while True:
                    self.wfile.write(q.get())
            except (BrokenPipeError, ConnectionResetError, socket.timeout, OSError):
                pass
            finally:
                broadcaster.unsubscribe(q)

    return Handler


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 1))   # TEST-NET-1: routes nowhere, sends nothing
        return s.getsockname()[0]
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", choices=sorted(SOURCES), default="local")
    ap.add_argument("--path", help="Music folder, for --source local")
    ap.add_argument("--url", help="Stream URL, for --source radio")
    ap.add_argument("--station", help="Display name for --source radio")
    ap.add_argument("--rate", type=int, default=22050,
                    help="Output sample rate; lower it if the console stutters")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--name", default="3DS", help="Name shown in Spotify Connect")
    ap.add_argument("--bitrate", default="160", choices=("96", "160", "320"))
    ap.add_argument("--silent", action="store_true",
                    help="Serve silence only; for testing the stream with no source")
    args = ap.parse_args()

    if not args.silent:
        if args.source == "local" and not args.path:
            ap.error("--source local needs --path")
        if args.source == "radio" and not args.url:
            ap.error("--source radio needs --url")
        for tool in ("ffmpeg", "ffprobe") + (("librespot",) if args.source == "spotify" else ()):
            if not shutil.which(tool):
                raise SystemExit(f"Missing {tool}: brew install {tool}")

    global BROADCASTER
    broadcaster = BROADCASTER = Broadcaster(args.rate)
    threading.Thread(target=broadcaster.run, daemon=True).start()
    if not args.silent:
        threading.Thread(target=supervise, args=(args, broadcaster), daemon=True).start()
    else:
        source.update(state="silent", kind="none")

    try:
        server = ThreadingHTTPServer(("0.0.0.0", args.port),
                                     make_handler(broadcaster, args.rate))
    except OSError as exc:
        if exc.errno != errno.EADDRINUSE:
            raise
        # Name the culprit instead of printing a traceback: an old copy of this
        # same bridge is the usual cause, and it answers requests as if nothing
        # were wrong.
        who = subprocess.run(["lsof", "-nP", f"-iTCP:{args.port}", "-sTCP:LISTEN"],
                             capture_output=True, text=True).stdout.strip()
        detail = "\n  ".join(who.splitlines()[1:]) or "could not identify the process"
        raise SystemExit(
            f"Port {args.port} is already in use, most likely an older bridge:\n"
            f"  {detail}\n"
            f"Stop it with:  pkill -f server/bridge.py\n"
            f"Or run this one elsewhere with --port")
    ip = local_ip()
    print(f"Bridge ready ({args.source}).  Stream: http://{ip}:{args.port}/stream")
    print(f"                              Now:    http://{ip}:{args.port}/now")
    print(f"PCM s16le {args.rate} Hz stereo = {args.rate*4*8/1000:.0f} kbps over the air")
    if args.source == "spotify" and not args.silent:
        print(f"Pick '{args.name}' in your Spotify app under Connect.")
    server.serve_forever()


if __name__ == "__main__":
    main()
