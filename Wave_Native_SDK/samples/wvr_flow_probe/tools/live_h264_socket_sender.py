import argparse
import ipaddress
import socket
import struct
import subprocess
import sys
import threading
import time


MAGIC = b"FLOWH264"
VERSION = 3
DISCOVERY_MAGIC = b"FLOWH264_PC"


def start_code_len(data, start):
    return 3 if data[start + 2] == 1 else 4


def nal_type(nal):
    offset = start_code_len(nal, 0)
    if offset >= len(nal):
        return -1
    return nal[offset] & 0x1F


def find_start_code(data, start):
    # bytearray.find runs in C; a byte-by-byte Python loop capped the stream near 38 fps.
    i = data.find(b"\x00\x00\x01", start)
    if i < 0:
        return -1
    # Report a 4-byte start code (00 00 00 01) from its first zero.
    if i > start and data[i - 1] == 0:
        return i - 1
    return i


class AnnexBReader:
    def __init__(self, stream):
        self.stream = stream
        self.buffer = bytearray()
        self.eof = False
        # Where to resume searching for the next start code, so each byte is scanned once.
        self.scan_from = 0

    def read_nal(self):
        while True:
            first = find_start_code(self.buffer, 0)
            if first > 0:
                del self.buffer[:first]
                self.scan_from = max(0, self.scan_from - first)
                first = 0

            if first == 0:
                payload_start = start_code_len(self.buffer, 0)
                second = find_start_code(self.buffer, max(payload_start, self.scan_from))
                if second > 0:
                    nal = bytes(self.buffer[:second])
                    del self.buffer[:second]
                    self.scan_from = 0
                    return nal
                # Back off 3 bytes so a start code split across reads is still found.
                self.scan_from = max(payload_start, len(self.buffer) - 3)
                if self.eof and self.buffer:
                    nal = bytes(self.buffer)
                    self.buffer.clear()
                    self.scan_from = 0
                    return nal

            if self.eof:
                return None

            chunk = self.stream.read(65536)
            if chunk:
                self.buffer.extend(chunk)
            else:
                self.eof = True


def send_blob(conn, blob):
    conn.sendall(struct.pack(">I", len(blob)))
    conn.sendall(blob)


def ffmpeg_command(args):
    size = f"{args.width}x{args.height}"
    if args.source == "desktop":
        source_args = [
            "-f", "gdigrab",
            "-framerate", str(args.fps),
            "-video_size", size,
            "-i", "desktop",
        ]
    elif args.source == "ddagrab":
        source_args = [
            "-f", "lavfi",
            "-i", f"ddagrab=framerate={args.fps}:video_size={size}",
        ]
    else:
        source_args = [
            "-f", "lavfi",
            "-re",
            "-i", f"testsrc2=size={size}:rate={args.fps}",
        ]

    if args.encoder == "nvenc":
        encoder_args = [
            "-c:v", "h264_nvenc",
            "-preset", "p1",
            "-tune", "ull",
            "-profile:v", "baseline",
            "-bf", "0",
            "-rc", "cbr",
            "-rc-lookahead", "0",
            "-delay", "0",
            "-surfaces", "2",
            "-b:v", args.bitrate,
            "-maxrate", args.bitrate,
            "-bufsize", args.bitrate,
            "-zerolatency", "1",
            "-forced-idr", "1",
        ]
    elif args.encoder == "mf":
        encoder_args = [
            "-c:v", "h264_mf",
            "-b:v", args.bitrate,
        ]
    elif args.encoder == "amf":
        encoder_args = [
            "-c:v", "h264_amf",
            "-usage", "ultralowlatency",
            "-profile:v", "baseline",
            "-b:v", args.bitrate,
        ]
    elif args.encoder == "qsv":
        encoder_args = [
            "-c:v", "h264_qsv",
            "-preset", "veryfast",
            "-profile:v", "baseline",
            "-b:v", args.bitrate,
        ]
    else:
        encoder_args = [
            "-c:v", "libx264",
            "-preset", "ultrafast",
            "-tune", "zerolatency",
            "-profile:v", "baseline",
            "-x264-params", "repeat-headers=1",
        ]

    pixel_format_args = []
    if not (args.source == "ddagrab" and args.encoder == "nvenc"):
        pixel_format_args = ["-pix_fmt", "yuv420p"]

    filter_args = []
    if args.video_filter:
        filter_args = ["-vf", args.video_filter]

    gop_args = [
        "-g", str(args.fps),
        "-keyint_min", str(args.fps),
    ]
    if args.encoder == "x264":
        gop_args.extend(["-sc_threshold", "0"])

    return [
        "ffmpeg",
        "-hide_banner",
        "-loglevel", "warning",
        *source_args,
        "-an",
        *filter_args,
        *encoder_args,
        *pixel_format_args,
        *gop_args,
        "-f", "h264",
        "pipe:1",
    ]


def log(message):
    print(message, flush=True)


def local_broadcast_targets(port):
    targets = {("255.255.255.255", port)}
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET, socket.SOCK_DGRAM):
            ip_text = info[4][0]
            if ip_text.startswith("127."):
                continue
            ip = ipaddress.ip_address(ip_text)
            network = ipaddress.ip_network(f"{ip_text}/24", strict=False)
            targets.add((str(network.broadcast_address), port))
    except OSError as exc:
        log(f"broadcast target lookup failed: {exc}")
    return sorted(targets)


def discovery_broadcaster(args, stop_event):
    payload = DISCOVERY_MAGIC + b" " + str(args.port).encode("ascii")
    targets = local_broadcast_targets(args.discovery_port)
    log("discovery targets: " + ", ".join([f"{host}:{port}" for host, port in targets]))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        while not stop_event.is_set():
            for target in targets:
                try:
                    udp.sendto(payload, target)
                except OSError as exc:
                    log(f"discovery broadcast failed target={target}: {exc}")
            stop_event.wait(1.0)


def serve_client(conn, addr, args):
    log(f"client connected: {addr}")
    proc = subprocess.Popen(
        ffmpeg_command(args),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    reader = AnnexBReader(proc.stdout)
    sps = None
    pps = None
    sent_header = False
    sent_frames = 0
    started = time.perf_counter()

    try:
        while args.frames <= 0 or sent_frames < args.frames:
            nal = reader.read_nal()
            if nal is None:
                break

            typ = nal_type(nal)
            if typ == 7:
                sps = nal
                continue
            if typ == 8:
                pps = nal
                continue
            if not (1 <= typ <= 5):
                continue

            if not sent_header:
                if sps is None or pps is None:
                    continue
                conn.sendall(MAGIC)
                conn.sendall(struct.pack(">IIII", VERSION, args.width, args.height, args.fps))
                send_blob(conn, sps)
                send_blob(conn, pps)
                sent_header = True
                log(f"stream header sent: source={args.source} size={args.width}x{args.height} fps={args.fps} sps={len(sps)} pps={len(pps)}")

            pts_us = sent_frames * 1_000_000 // args.fps
            encoded_ready_ms = time.time_ns() // 1_000_000
            send_start_ms = encoded_ready_ms
            conn.sendall(struct.pack(">Iqqq", len(nal), pts_us, encoded_ready_ms, send_start_ms))
            conn.sendall(nal)
            conn.sendall(struct.pack(">q", time.time_ns() // 1_000_000))
            sent_frames += 1

            if args.pace:
                target = started + sent_frames / args.fps
                sleep_for = target - time.perf_counter()
                if sleep_for > 0:
                    time.sleep(sleep_for)

            if sent_frames % args.fps == 0:
                elapsed = time.perf_counter() - started
                log(f"sent {sent_frames} live frames in {elapsed:.1f}s")

        if sent_header and args.frames > 0:
            conn.sendall(struct.pack(">i", -1))
        log(f"live sender finished frames={sent_frames}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
        stderr = proc.stderr.read().decode(errors="replace").strip()
        if stderr:
            print(stderr, file=sys.stderr)


def serve(args):
    discovery_stop = threading.Event()
    discovery_thread = None
    if args.discovery:
        discovery_thread = threading.Thread(
            target=discovery_broadcaster,
            args=(args, discovery_stop),
            name="FlowDiscovery",
            daemon=True,
        )
        discovery_thread.start()
        log(f"discovery broadcasting on UDP :{args.discovery_port}")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        try:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((args.host, args.port))
            server.listen(1)
            log(f"live sender listening on {args.host}:{args.port}")
            while True:
                conn, addr = server.accept()
                with conn:
                    try:
                        serve_client(conn, addr, args)
                    except (ConnectionError, OSError) as exc:
                        log(f"client disconnected: {exc}")
                        if args.frames > 0:
                            break
        finally:
            discovery_stop.set()
            if discovery_thread is not None:
                discovery_thread.join(timeout=1)


def main():
    # Windows socket errors are localized; a cp932/cp950 console would otherwise crash
    # the server while logging a disconnect instead of waiting for Flow to reconnect.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="Live-encode H.264 and send it to Flow Probe.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8001)
    parser.add_argument("--source", choices=["testsrc", "desktop", "ddagrab"], default="testsrc")
    parser.add_argument("--encoder", choices=["x264", "nvenc", "amf", "qsv", "mf"], default="x264")
    parser.add_argument("--bitrate", default="20M")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--frames", type=int, default=0, help="Number of frames to send; 0 streams until interrupted.")
    parser.add_argument("--video-filter", default="", help="Optional ffmpeg video filter, for example hflip,vflip.")
    parser.add_argument("--discovery-port", type=int, default=8002)
    parser.add_argument("--discovery", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--pace", action=argparse.BooleanOptionalAction, default=True)
    serve(parser.parse_args())


if __name__ == "__main__":
    main()
