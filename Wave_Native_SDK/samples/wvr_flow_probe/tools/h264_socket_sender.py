import argparse
import socket
import struct
import time
from pathlib import Path


MAGIC = b"FLOWH264"
VERSION = 2


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


def start_code_len(data, start):
    return 3 if data[start + 2] == 1 else 4


def nal_type(nal):
    offset = start_code_len(nal, 0)
    if offset >= len(nal):
        return -1
    return nal[offset] & 0x1F


def split_nals(data):
    nals = []
    start = find_start_code(data, 0)
    while start >= 0:
        next_start = find_start_code(data, start + start_code_len(data, start))
        end = next_start if next_start >= 0 else len(data)
        nals.append(data[start:end])
        start = next_start
    return nals


def payloads_from_h264(path):
    data = Path(path).read_bytes()
    nals = split_nals(data)
    sps = next((nal for nal in nals if nal_type(nal) == 7), None)
    pps = next((nal for nal in nals if nal_type(nal) == 8), None)
    frames = [nal for nal in nals if 1 <= nal_type(nal) <= 5]
    if sps is None or pps is None:
        raise RuntimeError("missing SPS/PPS in input stream")
    if not frames:
        raise RuntimeError("no VCL frames in input stream")
    return sps, pps, frames


def send_blob(conn, blob):
    conn.sendall(struct.pack(">I", len(blob)))
    conn.sendall(blob)


def serve(args):
    sps, pps, frames = payloads_from_h264(args.input)
    frame_interval = 1.0 / args.fps
    print(f"loaded {len(frames)} frames, sps={len(sps)} pps={len(pps)}")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port))
        server.listen(1)
        print(f"listening on {args.host}:{args.port}")
        conn, addr = server.accept()
        with conn:
            print(f"client connected: {addr}")
            conn.sendall(MAGIC)
            conn.sendall(struct.pack(">IIII", VERSION, args.width, args.height, args.fps))
            send_blob(conn, sps)
            send_blob(conn, pps)

            sent = 0
            base = time.perf_counter()
            for loop_index in range(args.loops):
                for frame in frames:
                    pts_us = sent * 1_000_000 // args.fps
                    conn.sendall(struct.pack(">Iqq", len(frame), pts_us, time.time_ns() // 1_000_000))
                    conn.sendall(frame)
                    sent += 1

                    target = base + sent * frame_interval
                    sleep_for = target - time.perf_counter()
                    if sleep_for > 0:
                        time.sleep(sleep_for)

            conn.sendall(struct.pack(">i", -1))
            print(f"sent {sent} frames")


def main():
    parser = argparse.ArgumentParser(description="Send Annex-B H.264 frames to Flow Probe.")
    parser.add_argument(
        "--input",
        default=str(Path(__file__).resolve().parents[1] / "app" / "src" / "main" / "assets" / "decoder_test.h264"),
    )
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8001)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--loops", type=int, default=5)
    serve(parser.parse_args())


if __name__ == "__main__":
    main()
