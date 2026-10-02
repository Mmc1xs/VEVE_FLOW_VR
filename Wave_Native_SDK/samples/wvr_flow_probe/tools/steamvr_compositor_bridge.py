import argparse
import socket
import struct
import subprocess
import sys
import threading
import time


FLOW_MAGIC = b"FLOWH264"
FLOW_VERSION = 3
BGRA_MAGIC = b"FLOWBGRA"


def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise EOFError("socket closed")
        data.extend(chunk)
    return bytes(data)


def send_blob(conn, blob):
    conn.sendall(struct.pack(">I", len(blob)))
    conn.sendall(blob)


def start_code_len(data, start):
    return 3 if data[start + 2] == 1 else 4


def nal_type(nal):
    offset = start_code_len(nal, 0)
    if offset >= len(nal):
        return -1
    return nal[offset] & 0x1F


def find_start_code(data, start):
    i = start
    while i + 3 < len(data):
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                return i
            if i + 4 < len(data) and data[i + 2] == 0 and data[i + 3] == 1:
                return i
        i += 1
    return -1


class AnnexBReader:
    def __init__(self, stream):
        self.stream = stream
        self.buffer = bytearray()
        self.eof = False

    def read_nal(self):
        while True:
            first = find_start_code(self.buffer, 0)
            if first > 0:
                del self.buffer[:first]
                first = 0

            if first == 0:
                second = find_start_code(self.buffer, start_code_len(self.buffer, 0))
                if second > 0:
                    nal = bytes(self.buffer[:second])
                    del self.buffer[:second]
                    return nal
                if self.eof and self.buffer:
                    nal = bytes(self.buffer)
                    self.buffer.clear()
                    return nal
                if self.eof:
                    return None

            chunk = self.stream.read(65536)
            if chunk:
                self.buffer.extend(chunk)
            else:
                self.eof = True


def ffmpeg_command(width, height, fps, args):
    encoder_args = [
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-profile:v", "baseline",
        "-x264-params", "repeat-headers=1",
        "-pix_fmt", "yuv420p",
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

    return [
        "ffmpeg",
        "-hide_banner",
        "-loglevel", "warning",
        "-f", "rawvideo",
        "-pix_fmt", "bgra",
        "-video_size", f"{width}x{height}",
        "-framerate", str(fps),
        "-i", "pipe:0",
        "-an",
        *encoder_args,
        "-g", str(fps),
        "-keyint_min", str(fps),
        "-f", "h264",
        "pipe:1",
    ]


def accept_one(host, port, label):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((host, port))
    server.listen(1)
    print(f"{label} listening on {host}:{port}")
    conn, addr = server.accept()
    print(f"{label} connected: {addr}")
    server.close()
    return conn


def listen_one(host, port, label):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((host, port))
    server.listen(1)
    print(f"{label} listening on {host}:{port}", flush=True)
    return server


def accept_worker(server, label, result):
    try:
        conn, addr = server.accept()
        print(f"{label} connected: {addr}", flush=True)
        result["conn"] = conn
    except Exception as exc:
        result["error"] = exc
    finally:
        server.close()


def read_bgra_header(conn):
    header = recv_exact(conn, 20)
    magic, width, height, fps = struct.unpack(">8sIII", header)
    if magic != BGRA_MAGIC:
        raise ValueError(f"bad BGRA magic: {magic!r}")
    print(f"BGRA source header: {width}x{height} fps={fps}")
    return width, height, fps


def raw_frame_pump(source_conn, width, height, latest, label):
    payload_size = width * height * 4
    frames = 0
    try:
        while not latest["stop"]:
            frame_header = recv_exact(source_conn, 20)
            length, frame_id, present_count = struct.unpack(">IQQ", frame_header)
            if length != payload_size:
                raise ValueError(f"unexpected BGRA frame size {length}, expected {payload_size}")
            payload = recv_exact(source_conn, length)
            frames += 1
            with latest["condition"]:
                latest["frame"] = payload
                latest["source_frames"] += 1
                latest["frame_id"] = frame_id
                latest["present_count"] = present_count
                latest["source_label"] = label
                latest["condition"].notify_all()
            if frames <= 3 or frames % 30 == 0:
                print(f"raw frames received={frames} total={latest['source_frames']} source={label} lastSteamFrame={frame_id} present={present_count}", flush=True)
    except Exception as exc:
        if not latest["stop"]:
            print(f"raw pump stopped source={label}: {exc}", file=sys.stderr, flush=True)
    finally:
        with latest["condition"]:
            latest["condition"].notify_all()


def source_accept_loop(server, latest):
    source_index = 0
    while not latest["stop"]:
        try:
            conn, addr = server.accept()
        except OSError:
            break
        source_index += 1
        label = f"src{source_index}@{addr[1]}"
        print(f"SteamVR BGRA connected: {addr} as {label}", flush=True)
        threading.Thread(
            target=source_connection_pump,
            args=(conn, latest, label),
            daemon=True,
        ).start()


def source_connection_pump(conn, latest, label):
    try:
        width, height, fps = read_bgra_header(conn)
        with latest["condition"]:
            if latest["width"] is None:
                latest["width"] = width
                latest["height"] = height
                latest["fps"] = fps
                latest["condition"].notify_all()
            elif latest["width"] != width or latest["height"] != height or latest["fps"] != fps:
                print(f"ignoring mismatched BGRA source={label}: {width}x{height} fps={fps}", file=sys.stderr, flush=True)
                return
        raw_frame_pump(conn, width, height, latest, label)
    except Exception as exc:
        if not latest["stop"]:
            print(f"source connection stopped source={label}: {exc}", file=sys.stderr, flush=True)
    finally:
        try:
            conn.close()
        except Exception:
            pass


def ffmpeg_frame_writer(proc, fps, max_frames, latest):
    frames = 0
    started = time.perf_counter()
    try:
        with latest["condition"]:
            while latest["frame"] is None and not latest["stop"]:
                latest["condition"].wait(timeout=0.5)

        while (max_frames <= 0 or frames < max_frames) and not latest["stop"]:
            with latest["condition"]:
                frame = latest["frame"]
                source_frames = latest["source_frames"]
                frame_id = latest["frame_id"]
                source_label = latest["source_label"]
            if frame is None:
                break
            proc.stdin.write(frame)
            frames += 1
            if frames <= 3 or frames % fps == 0:
                print(f"ffmpeg frames fed={frames} sourceFrames={source_frames} source={source_label} lastSteamFrame={frame_id}", flush=True)

            target = started + frames / fps
            sleep_for = target - time.perf_counter()
            if sleep_for > 0:
                time.sleep(sleep_for)
    except Exception as exc:
        print(f"ffmpeg writer stopped: {exc}", file=sys.stderr, flush=True)
    finally:
        latest["stop"] = True
        try:
            proc.stdin.close()
        except Exception:
            pass


def bridge(flow_conn, latest, args):
    with latest["condition"]:
        while latest["width"] is None and not latest["stop"]:
            latest["condition"].wait(timeout=0.5)
        if latest["width"] is None:
            raise RuntimeError("no BGRA source connected")
        width = latest["width"]
        height = latest["height"]
        fps = latest["fps"]

    proc = subprocess.Popen(
        ffmpeg_command(width, height, fps, args),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    writer = threading.Thread(
        target=ffmpeg_frame_writer,
        args=(proc, fps, args.frames, latest),
        daemon=True,
    )
    writer.start()

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
                flow_conn.sendall(FLOW_MAGIC)
                flow_conn.sendall(struct.pack(">IIII", FLOW_VERSION, width, height, fps))
                send_blob(flow_conn, sps)
                send_blob(flow_conn, pps)
                sent_header = True
                print(f"FLOWH264 header sent: {width}x{height} fps={fps} sps={len(sps)} pps={len(pps)}")

            pts_us = sent_frames * 1_000_000 // fps
            encoded_ready_ms = time.time_ns() // 1_000_000
            send_start_ms = encoded_ready_ms
            flow_conn.sendall(struct.pack(">Iqqq", len(nal), pts_us, encoded_ready_ms, send_start_ms))
            flow_conn.sendall(nal)
            flow_conn.sendall(struct.pack(">q", time.time_ns() // 1_000_000))
            sent_frames += 1

            if sent_frames % fps == 0:
                elapsed = time.perf_counter() - started
                print(f"sent {sent_frames} compositor frames in {elapsed:.1f}s")

        if sent_header and args.frames > 0:
            flow_conn.sendall(struct.pack(">i", -1))
        print(f"bridge finished frames={sent_frames}")
    finally:
        latest["stop"] = True
        with latest["condition"]:
            latest["condition"].notify_all()
        try:
            proc.terminate()
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        stderr = proc.stderr.read().decode(errors="replace").strip()
        if stderr:
            print(stderr, file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Bridge SteamVR driver BGRA preview frames to Flow H.264 socket stream.")
    parser.add_argument("--flow-host", default="0.0.0.0")
    parser.add_argument("--flow-port", type=int, default=8001)
    parser.add_argument("--source-host", default="127.0.0.1")
    parser.add_argument("--source-port", type=int, default=9101)
    parser.add_argument("--encoder", choices=["x264", "nvenc"], default="nvenc")
    parser.add_argument("--bitrate", default="45M")
    parser.add_argument("--frames", type=int, default=0, help="Number of encoded frames to send; 0 streams until interrupted.")
    args = parser.parse_args()

    flow_server = listen_one(args.flow_host, args.flow_port, "Flow H264")
    source_server = listen_one(args.source_host, args.source_port, "SteamVR BGRA")
    latest = {
        "condition": threading.Condition(),
        "width": None,
        "height": None,
        "fps": None,
        "frame": None,
        "source_frames": 0,
        "frame_id": 0,
        "present_count": 0,
        "source_label": "none",
        "stop": False,
    }
    source_thread = threading.Thread(target=source_accept_loop, args=(source_server, latest), daemon=True)
    source_thread.start()

    flow_result = {}
    flow_thread = threading.Thread(target=accept_worker, args=(flow_server, "Flow H264", flow_result))
    flow_thread.start()
    flow_thread.join()

    if "error" in flow_result:
        raise flow_result["error"]

    flow_conn = flow_result["conn"]
    try:
        bridge(flow_conn, latest, args)
    finally:
        latest["stop"] = True
        try:
            source_server.close()
        except Exception:
            pass
        flow_conn.close()


if __name__ == "__main__":
    main()
