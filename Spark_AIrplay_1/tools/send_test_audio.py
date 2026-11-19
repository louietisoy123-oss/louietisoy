#!/usr/bin/env python3
"""
send_test_audio.py - AirPlay 1 (RAOP) Audio Streamer Test Rig for RT-Spark

Drives the RT-Spark (STM32F407) AirPlay 1 receiver through a full sender-side
session, mirroring what an iPhone/iTunes does on the wire:

    discovery -> OPTIONS -> ANNOUNCE (RSA-OAEP key exchange) -> SETUP ->
    RECORD -> RTP/ALAC streaming (AES-128-CBC, 352 frames / 8 ms) -> TEARDOWN

Every wire format below was verified against the firmware sources (2026-11-17):
  * aes_frame.c  : cipher_len = payload & ~0x0F, trailing 0..15 bytes plaintext
  * rtp_parser.c : 12-byte header, payload type 0x60 (marker bit stripped)
  * rtp_resend.c : 0xD5 request {seq,count}; 0xD6 response = 4-byte wrapper +
                   the FULL original RTP packet (header + payload)
  * rtp_timing.c : 32-byte 0xD2 request (t1 @ [24:32]); 0xD3 response carries
                   origin/receive/transmit as big-endian NTP 64-bit timestamps
  * alac.c       : uncompressed stereo frame = 3-bit channels(=1) + 4-bit 0 +
                   12-bit 0 + hassize(0) + uncompressed_bytes(0) +
                   isnotcompressed(1) + 352 x (16-bit L, 16-bit R) -> 1411 bytes
  * sdp.c/alac_config.c : fmtp keeps the "96" payload type -> 12 integers
  * raop.c       : SETUP answers server_port=; RECORD needs key + fmtp

Refactor notes (replaces the original hard-coded-IP script):
  * Board discovery: --ip, SoftAP heuristic (192.168.4.0/24), one-shot mDNS
    query answered unicast by mdns_responder.c, or the boot log on the serial
    monitor ("IP Address: ...") -- all candidates verified by TCP probe.
  * WAV streaming (--wave): real-music stutter testing, not just the tone.
  * Timing worker: answers the board's 0xD2 requests with 0xD3 (the board
    free-runs, but a well-behaved sender answers; exercises rtp_timing).
  * Control worker: services 0xD5 resend requests with 0xD6 responses.
  * Pacing: hybrid sleep + busy-wait with drift reporting; the firmware's
    jitter buffer backpressures, so keep sending at real-time rate.
  * Board health telemetry from the serial monitor (underruns / dma_misses /
    conceals / resends counters printed by the RTP thread every 5 s).

Examples:
    python send_test_audio.py                          # tone, auto-discover
    python send_test_audio.py --wave ..\\lukalukanightfever.wav
    python send_test_audio.py --pc                     # stream this PC's audio output
    python send_test_audio.py --pc --duration 0        # ...until Ctrl-C
    python send_test_audio.py --list-pc-devices        # pick a loopback source
    python send_test_audio.py --ip 192.168.1.56 --serial none --duration 30
"""

import sys
import os
import time
import math
import struct
import socket
import base64
import random
import threading
import re
import argparse
import wave

if sys.platform == "win32":
    import ctypes
    try:
        ctypes.windll.winmm.timeBeginPeriod(1)  # 1 ms scheduler for RTP pacing
    except Exception:
        pass

try:
    from cryptography.hazmat.primitives.asymmetric import padding
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    print("Error: python cryptography package not found. Run: pip install cryptography")
    sys.exit(1)

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

# --------------------------------------------------------------------------
# Well-known Apple AirPort Express RAOP RSA Private Key (AirPlay 1)
# --------------------------------------------------------------------------
RAOP_KEY_PEM = (
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
    "wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
    "wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
    "/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
    "UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
    "BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
    "LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
    "NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
    "lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
    "aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
    "a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
    "oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
    "oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
    "k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
    "AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
    "cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
    "54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
    "17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
    "1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
    "LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
    "2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
    "-----END RSA PRIVATE KEY-----\n"
)

# --------------------------------------------------------------------------
# Streaming constants (all cross-checked against the firmware)
# --------------------------------------------------------------------------
FRAME_SAMPLES    = 352                    # ALAC frame_length (RAOP default)
SAMPLE_RATE      = 44100
PACKET_INTERVAL  = FRAME_SAMPLES / SAMPLE_RATE   # ~7.98 ms between packets
ALAC_FRAME_BYTES = 1411                   # 23 header bits + 352 * 32 bits
CIPHER_BYTES     = ALAC_FRAME_BYTES & ~0x0F      # 1408 (88 AES-CBC blocks)
PLAIN_BYTES      = ALAC_FRAME_BYTES - CIPHER_BYTES  # 3 trailing plaintext bytes
RTP_PT_AUDIO     = 0x60                   # payload type (0xE0 = marker on first)
CLIENT_CTRL_PORT = 6001                   # we receive 0xD5 resend requests here
CLIENT_TIME_PORT = 6002                   # we receive 0xD2 timing requests here
PRIME_BURST      = 8                      # packets sent immediately at start
RESYNC_MS        = 50.0                   # behind schedule by this much -> re-anchor, never burst
RESEND_HISTORY   = 256                    # packets kept for resend servicing
NTP_EPOCH_DELTA  = 2208988800             # seconds between 1900 and 1970

# --------------------------------------------------------------------------
# BitWriter + uncompressed ALAC frame encoder
# --------------------------------------------------------------------------
class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.cur = 0
        self.bits = 0

    def write(self, val, n):
        for i in range(n - 1, -1, -1):
            b = (val >> i) & 1
            self.cur = (self.cur << 1) | b
            self.bits += 1
            if self.bits == 8:
                self.buf.append(self.cur)
                self.cur = 0
                self.bits = 0

    def finish(self):
        if self.bits > 0:
            self.cur <<= (8 - self.bits)
            self.buf.append(self.cur)
            self.cur = 0
            self.bits = 0
        return bytes(self.buf)


def encode_alac_uncompressed_frame(samples_l, samples_r):
    """
    Packs 352 stereo samples into an uncompressed ALAC frame (1411 bytes).

    Bitstream layout (matches alac.c case-1 'not compressed' branch):
      3 bits channels=1 (2 channels) | 4 bits 0 | 12 bits 0 | hassize=0 |
      uncompressed_bytes=0 (2 bits) | isnotcompressed=1 | 352 x (16-bit L,
      16-bit R), 7 padding bits at the tail.
    """
    assert len(samples_l) == FRAME_SAMPLES and len(samples_r) == FRAME_SAMPLES
    bw = BitWriter()
    bw.write(1, 3)   # channels = 1 (meaning 2 channels)
    bw.write(0, 4)   # 4 bits 0
    bw.write(0, 12)  # 12 bits 0
    bw.write(0, 1)   # hassize = 0 (352 samples per setinfo_max_samples_per_frame)
    bw.write(0, 2)   # uncompressed_bytes = 0
    bw.write(1, 1)   # isnotcompressed = 1
    for l, r in zip(samples_l, samples_r):
        bw.write(l & 0xFFFF, 16)
        bw.write(r & 0xFFFF, 16)
    frame = bw.finish()
    assert len(frame) == ALAC_FRAME_BYTES
    return frame

# --------------------------------------------------------------------------
# Audio sources
# --------------------------------------------------------------------------
def tone_source(duration_sec=10.0, sample_rate=SAMPLE_RATE):
    """
    Precomputed stereo musical chord arpeggio (C5 -> E5 -> G5 -> C6 bell).
    Returns (frames_l, frames_r) flat sample lists; the caller chunks them.
    """
    print("      Synthesizing stereo chord arpeggio (C5 -> E5 -> G5 -> C6 bell)...")
    num_samples = int(duration_sec * sample_rate)
    notes = [
        (0.00, 0.75, 523.25, 0.8, 0.3),   # C5 (panned left)
        (0.75, 1.50, 659.25, 0.4, 0.7),   # E5 (panned right)
        (1.50, 2.25, 783.99, 0.7, 0.4),   # G5 (panned left-center)
        (2.25, 3.00, 1046.50, 0.6, 0.6),  # C6 (centered bell)
    ]
    amplitude = 12000.0
    two_pi = 2.0 * math.pi

    frames_l = [0] * num_samples
    frames_r = [0] * num_samples
    for i in range(num_samples):
        t = i / sample_rate
        val_l = 0.0
        val_r = 0.0
        for start_t, end_t, freq, pan_l, pan_r in notes:
            if start_t <= t < end_t:
                dt = t - start_t
                note_dur = end_t - start_t
                env = min(1.0, dt / 0.01) * math.exp(-3.0 * (dt / note_dur))
                wave_s = math.sin(two_pi * freq * dt)
                val_l += wave_s * env * pan_l
                val_r += wave_s * env * pan_r
            elif end_t <= t < (end_t + 0.5):
                dt = t - start_t
                tail_dt = t - end_t
                decay = math.exp(-3.0) * math.exp(-8.0 * tail_dt)
                wave_s = math.sin(two_pi * freq * dt)
                val_l += wave_s * decay * pan_l
                val_r += wave_s * decay * pan_r
        frames_l[i] = int(max(-32760, min(32760, val_l * amplitude)))
        frames_r[i] = int(max(-32760, min(32760, val_r * amplitude)))
    return frames_l, frames_r


def wav_source(path):
    """
    Opens a 44.1 kHz / 16-bit / stereo WAV for lazy 352-frame chunk streaming.
    Returns (chunks_iterable, total_frames).
    """
    try:
        w = wave.open(path, "rb")
    except Exception as e:
        raise SystemExit(f"Error: cannot open WAV '{path}': {e}")
    if (w.getnchannels() != 2 or w.getsampwidth() != 2 or
            abs(w.getframerate() - SAMPLE_RATE) > 1):
        raise SystemExit(
            f"Error: '{path}' is {w.getframerate()} Hz / {w.getsampwidth() * 8}-bit / "
            f"{w.getnchannels()} ch. The receiver decodes 44100 Hz / 16-bit / stereo.\n"
            f"       Convert first:  ffmpeg -i \"{path}\" -ar 44100 -ac 2 -sample_fmt s16 out.wav")
    total = w.getnframes()
    print(f"      WAV: {os.path.basename(path)} — {total} frames "
          f"({total / SAMPLE_RATE:.1f} s @ 44.1 kHz stereo)")
    import array

    def chunks():
        remaining = total
        while remaining >= FRAME_SAMPLES:
            raw = w.readframes(FRAME_SAMPLES)
            if len(raw) < FRAME_SAMPLES * 4:
                break
            remaining -= FRAME_SAMPLES
            smp = array.array("h")
            smp.frombytes(raw)          # little-endian int16 pairs (L, R)
            yield smp[0::2], smp[1::2]
        w.close()
    return chunks(), total

# --------------------------------------------------------------------------
# Live PC audio source: Windows WASAPI loopback -> 44.1 kHz stereo int16
# --------------------------------------------------------------------------
def list_pc_devices():
    """Enumerate WASAPI loopback (system output capture) devices."""
    try:
        import soundcard as sc
    except ImportError:
        raise SystemExit("Error: --pc / --list-pc-devices need soundcard: "
                         "pip install soundcard numpy")
    try:
        default = sc.default_speaker().name
    except Exception:
        default = "(unknown)"
    print(f"Default output: {default}")
    for i, m in enumerate(sc.all_microphones(include_loopback=True)):
        if not m.isloopback:
            continue
        mark = "  <- default output" if m.name == default else ""
        print(f"  [{i}] {m.name}{mark}")


def pc_source(args):
    """
    Live capture of whatever this PC is playing (WASAPI loopback via the
    `soundcard` library), delivered at 44.1 kHz stereo int16. soundcard was
    chosen over PyAudioWPatch after its PortAudio backend segfaulted reading
    a virtual-cable loopback pin; soundcard handles loopback pins and sample
    rate conversion (48 kHz mix format -> 44.1 kHz) reliably.

    Architecture (mirrors what a real AirPlay sender does):
      * a background thread runs the BLOCKING loopback read — WASAPI delivers
        nothing until some app renders, and can stall for seconds at stream
        start or when apps switch sample rates;
      * the consumer paces by WALL CLOCK and pads with silence whenever the
        capture is behind, so the board's jitter buffer never runs dry:
        content gaps play as silence (correct) instead of starving the DAC
        (which counted ~150k underruns before this change).

    Returns (chunks_iterable, None) — unbounded; end with Ctrl-C or --duration.
    """
    try:
        import soundcard as sc
        import numpy as np
    except ImportError:
        raise SystemExit("Error: --pc needs soundcard + numpy: "
                         "pip install soundcard numpy")
    import collections

    if args.pc_device is not None:
        mics = [m for m in sc.all_microphones(include_loopback=True) if m.isloopback]
        if args.pc_device >= len(mics):
            raise SystemExit(f"Error: loopback index {args.pc_device} out of range "
                             f"(have {len(mics)}; use --list-pc-devices)")
        mic = mics[args.pc_device]
    else:
        try:
            spk = sc.default_speaker()
            mic = sc.get_microphone(spk.id, include_loopback=True)
        except Exception as e:
            raise SystemExit(f"Error: no default output to loop back: {e}")
    print(f"      PC loopback: {mic.name}")
    print(f"           captured at {SAMPLE_RATE} Hz stereo (soundcard rate-converts "
          f"the device mix format)")

    buf = collections.deque()        # interleaved int16, capture-thread side
    buf_lock = threading.Lock()
    capturing = [True]

    def capture_worker():
        try:
            with mic.recorder(samplerate=SAMPLE_RATE, channels=2,
                              blocksize=FRAME_SAMPLES) as rec:
                while capturing[0]:
                    data = rec.record(numframes=FRAME_SAMPLES)   # float [frames, ch]
                    smp = np.clip(data[:, :2] * 32767.0, -32767, 32767) \
                            .astype("<i2").reshape(-1)
                    if len(smp):
                        with buf_lock:
                            buf.extend(smp.tolist())
        except Exception as e:
            print(f"      [PC capture] error: {e}")
            capturing[0] = False

    cap_thread = threading.Thread(target=capture_worker, daemon=True)
    cap_thread.start()

    BLOCK = FRAME_SAMPLES * 2          # interleaved samples per RTP block
    PRIME_BLOCKS = 3                   # ~24 ms queued before (re)starting playback
    MAX_BLOCKS = 16                    # ~128 ms cap: drop oldest beyond this
    silence = ([0] * FRAME_SAMPLES, [0] * FRAME_SAMPLES)

    def chunks():
        # The caller (stream_audio) paces calls at real time, one per block.
        # This is a jitter buffer between the capture clock and the send clock:
        #   * whole blocks only — a partial block is NEVER zero-padded, since
        #     splicing zeros into live audio is itself an audible dropout;
        #   * emit a full silence block only when the queue is genuinely empty
        #     (idle loopback, or capture stalled), then re-prime before resuming;
        #   * cap the queue so a capture burst cannot grow latency without bound.
        playing = False
        try:
            while True:
                with buf_lock:
                    excess = len(buf) - MAX_BLOCKS * BLOCK
                    if excess > 0:
                        excess -= excess % 2       # keep L/R pairing
                        for _ in range(excess):
                            buf.popleft()
                    if not playing and len(buf) >= PRIME_BLOCKS * BLOCK:
                        playing = True
                    if playing and len(buf) >= BLOCK:
                        blk = [buf.popleft() for _ in range(BLOCK)]
                    else:
                        playing = False
                        blk = None
                if blk is None:
                    yield silence
                else:
                    yield blk[0::2], blk[1::2]
        finally:
            capturing[0] = False
    return chunks(), None

# --------------------------------------------------------------------------
# Board discovery: --ip / SoftAP heuristic / mDNS / serial boot log
# --------------------------------------------------------------------------
def local_ipv4s():
    """All IPv4 addresses of this host (best effort)."""
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                ips.add(ip)
    except Exception:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))          # no traffic sent; reveals default route IP
        ips.add(s.getsockname()[0])
        s.close()
    except Exception:
        pass
    return ips


def rtsp_probe(ip, port, timeout=2.5):
    """Open a connection, complete an OPTIONS exchange, verify 200 OK, and
    RETURN THE OPEN SOCKET.

    The board's RTSP server accepts exactly ONE sender; a bare TCP probe would
    occupy that slot (and any follow-up connection gets 453-refused until the
    server notices the probe's FIN). Adopting the verified connection avoids
    the double-connect race entirely."""
    try:
        sock = socket.create_connection((ip, port), timeout=timeout)
        sock.settimeout(timeout)
        req = ("OPTIONS * RTSP/1.0\r\n"
               "CSeq: 1\r\n"
               "User-Agent: RT-Spark-Test/1.0 (discovery probe)\r\n"
               "\r\n")
        sock.sendall(req.encode("ascii"))
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = sock.recv(1024)
            if not chunk:
                break
            resp += chunk
        first_line = resp.split(b"\r\n", 1)[0]
        if b" 200 " in first_line:
            sock.settimeout(10.0)
            return sock
        sock.close()
    except OSError:
        pass
    return None


def mdns_query_ip(timeout=4.0):
    """
    One-shot mDNS query for _raop._tcp.local. mdns_responder.c matches the raw
    bytes for "_raop" and replies UNICAST to our source port with its static
    response packet (PTR/TXT/SRV/A). Returns the board IP from the A record.
    """
    qname = b"\x05_raop\x04_tcp\x05local\x00"
    txn = random.randint(0, 0xFFFF)
    query = struct.pack("!HHHHHH", txn, 0x0100, 1, 0, 0, 0)  # ID, RD query, 1 question
    query += qname + struct.pack("!HH", 12, 1)               # PTR, IN

    def read_name(buf, off):
        labels = []
        while True:
            if off >= len(buf):
                return b"", off
            n = buf[off]
            if n == 0:
                off += 1
                break
            if n & 0xC0 == 0xC0:             # compression pointer
                off += 2
                break
            labels.append(buf[off + 1: off + 1 + n])
            off += 1 + n
        return b".".join(labels), off

    found = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("", 0))
        s.settimeout(timeout)
        try:
            s.sendto(query, ("224.0.0.251", 5353))
        except OSError as e:
            print(f"      [mDNS] query send failed ({e})")
            s.close()
            return None
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                data, _addr = s.recvfrom(4096)
            except socket.timeout:
                break
            if len(data) < 12:
                continue
            qd, an = struct.unpack_from("!HH", data, 4)
            off = 12
            for _ in range(qd):              # skip questions
                _, off = read_name(data, off)
                off += 4
            for _ in range(an):
                _, off = read_name(data, off)
                if off + 10 > len(data):
                    break
                rtype, _rclass, _ttl, rdlen = struct.unpack_from("!HHIH", data, off)
                off += 10
                rdata = data[off: off + rdlen]
                off += rdlen
                if rtype == 1 and rdlen == 4:
                    found.add(".".join(str(b) for b in rdata))
        s.close()
    except OSError as e:
        print(f"      [mDNS] discovery failed ({e})")
    if found:
        print(f"      [mDNS] found RT-Spark at {sorted(found)[0]}")
        return sorted(found)[0]
    return None


def serial_log_ip(logger, timeout=6.0):
    """Scan the serial monitor capture for the board's boot-time IP lines."""
    pat = re.compile(r"IP Address:\s*(\d+\.\d+\.\d+\.\d+)|"
                     r"Station Online! IP:\s*(\d+\.\d+\.\d+\.\d+)")
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in list(logger.lines):
            m = pat.search(line)
            if m:
                ip = m.group(1) or m.group(2)
                if ip and ip != "0.0.0.0":
                    print(f"      [Serial] boot log reports board IP {ip}")
                    return ip
        time.sleep(0.3)
    return None


def _private_subnet_of(ip):
    """The /24 subnet of a private-range IPv4, else None."""
    if not ip.startswith(("192.168.", "10.", "172.")):
        return None
    parts = ip.split(".")
    return ".".join(parts[:3])


def arp_candidates(subnet):
    """IPs our host recently talked to on `subnet` (Windows 'arp -a')."""
    cands = []
    try:
        import subprocess
        out = subprocess.run(["arp", "-a"], capture_output=True, text=True,
                             timeout=5).stdout
        for m in re.finditer(r"(\d+\.\d+\.\d+\.\d+)\s+([0-9a-f-]{17})", out,
                             re.IGNORECASE):
            ip = m.group(1)
            if ip.startswith(subnet + ".") and ip != subnet + ".255":
                cands.append(ip)
    except Exception:
        pass
    return cands


def subnet_scan(subnet, port, timeout=1.2, workers=100):
    """Concurrent bare-TCP probe of a /24 on the RTSP port (outbound,
    firewall-safe). Returns discovered IPs (verification happens later via
    rtsp_probe, which must not be run 100× — it would spam the board's
    single-slot RTSP server)."""
    import concurrent.futures

    def bare_probe(ip):
        try:
            with socket.create_connection((ip, port), timeout=timeout):
                return ip
        except OSError:
            return None

    ips = [f"{subnet}.{i}" for i in range(1, 255)]
    hits = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        futures = {ex.submit(bare_probe, ip): ip for ip in ips}
        for fut in concurrent.futures.as_completed(futures):
            r = fut.result()
            if r:
                hits.append(r)
    return sorted(hits, key=lambda x: int(x.rsplit(".", 1)[1]))


def discover_board(args, logger):
    """Build an ordered candidate list, return (ip, adopted_socket|None).

    Order: --ip > SoftAP heuristic > ARP-cache entries > one-shot mDNS >
    serial boot log > full /24 subnet scan (last resort)."""
    candidates = []

    if args.ip:
        candidates.append(args.ip)
    else:
        # 1. SoftAP mode: our host has a 192.168.4.x address -> the board is 192.168.4.1
        subnets = []
        for ip in local_ipv4s():
            if ip.startswith("192.168.4."):
                candidates.append("192.168.4.1")
            sn = _private_subnet_of(ip)
            if sn and sn not in subnets:
                subnets.append(sn)

        # 2. ARP cache entries on our LAN (free, reliable if we talked before)
        for sn in subnets:
            candidates.extend(arp_candidates(sn))

        # 3. mDNS one-shot query (mdns_responder.c replies unicast; may be
        #    blocked by Windows Firewall for inbound UDP — keep as mid-order)
        ip = mdns_query_ip()
        if ip:
            candidates.append(ip)

        # 4. Boot log captured by the serial monitor
        if logger is not None and args.serial != "none":
            ip = serial_log_ip(logger, timeout=3.0)
            if ip:
                candidates.append(ip)

        # 5. Last resort: concurrent /24 scan (outbound probes always pass)
        for sn in subnets:
            for ip in subnet_scan(sn, args.port):
                candidates.append(ip)

    seen = set()
    for ip in candidates:
        if ip in seen:
            continue
        seen.add(ip)
        print(f"      Probing RTSP on {ip}:{args.port} ...")
        sock = rtsp_probe(ip, args.port)
        if sock is not None:
            return ip, sock
        print("        no answer")
    return None, None

# --------------------------------------------------------------------------
# Serial monitor: echoes board telemetry (incl. the RTP health counters)
# --------------------------------------------------------------------------
class SerialLogger(threading.Thread):
    """Background COM reader. Captures every line (for discovery) and echoes
    the ones that matter: stream health, RX drops, AirPlay session events."""

    INTERESTING = re.compile(
        r"health:|RX drop|AirPlay|RTSP|RTP|ANNOUNCE|SETUP|RECORD|TEARDOWN|"
        r"Volume|Metadata|stats|heap|alloc|crypto|rsa|ERROR|WARN|RSSI|RW007|"
        r"DISCONNECT|CONNECT|DHCP|Wi-Fi", re.IGNORECASE)

    def __init__(self, port=None, baudrate=115200):
        super().__init__(daemon=True)
        self.port = port
        self.baudrate = baudrate
        self.running = True
        self.ser = None
        self.lines = []          # capture buffer (discovery reads this)

    def run(self):
        if serial is None:
            print("[SerialMonitor] pyserial not installed — monitor disabled")
            return
        if self.port is None:
            print("[SerialMonitor] No serial port available — monitor disabled")
            return
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=0.1)
            print(f"[SerialMonitor] Attached to {self.port} at {self.baudrate} baud")
            while self.running:
                raw = self.ser.readline()
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace").rstrip()
                if text:
                    self.lines.append(text)
                    if len(self.lines) > 500:
                        del self.lines[:100]
                    if self.INTERESTING.search(text):
                        print(f"[RT-Spark] {text}")
        except Exception as e:
            print(f"[SerialMonitor] Note: could not open {self.port} ({e})")

    def stop(self):
        self.running = False
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass


def pick_serial_port(requested):
    """'none' disables. 'auto' prefers COM7 (monitor_port in platformio.ini),
    then any ST-LINK VCP, else the first available port."""
    if requested == "none" or serial is None:
        return None
    if requested != "auto":
        return requested
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.device.upper() == "COM7":
            return p.device
    for p in ports:
        if getattr(p, "vid", None) == 0x0483:  # STMicroelectronics
            return p.device
    return ports[0].device if ports else None

# --------------------------------------------------------------------------
# RTSP sender session
# --------------------------------------------------------------------------
class RaopSender:
    def __init__(self, target_ip, rtsp_port=5000, host_ip=None):
        self.target_ip = target_ip
        self.rtsp_port = rtsp_port
        self.host_ip = host_ip
        self.sock = None
        self.cseq = 0
        self.priv_key = None
        self.sock_lock = threading.Lock()   # request() vs keepalive thread vs teardown

    def load_key(self):
        self.priv_key = serialization.load_pem_private_key(
            RAOP_KEY_PEM.encode("ascii"), password=None)
        print("[1/6] RSA 2048-bit AirPort Express key loaded.")

    def adopt(self, sock, ip):
        """Use the discovery probe's already-verified connection (the board's
        RTSP server is single-client; re-connecting would race its cleanup)."""
        self.sock = sock
        self.target_ip = ip
        self.cseq = 1            # the probe already sent OPTIONS with CSeq 1
        print("      Adopted the verified probe connection.")

    def connect(self):
        if self.sock is not None:
            print(f"[3/6] RTSP session to {self.target_ip}:{self.rtsp_port} already open.")
            return
        print(f"[3/6] Connecting to RTSP server {self.target_ip}:{self.rtsp_port}...")
        self.sock = socket.create_connection((self.target_ip, self.rtsp_port), timeout=10.0)
        self.sock.settimeout(10.0)
        print("      RTSP TCP connected.")

    def _next_cseq(self):
        self.cseq += 1
        return self.cseq

    def request(self, method, uri, headers="", body=""):
        with self.sock_lock:
            req = f"{method} {uri} RTSP/1.0\r\nCSeq: {self._next_cseq()}\r\n{headers}"
            if body:
                req += f"Content-Length: {len(body)}\r\n"
            req += "\r\n" + body
            self.sock.sendall(req.encode("utf-8"))

            resp = b""
            while b"\r\n\r\n" not in resp:
                chunk = self.sock.recv(1024)
                if not chunk:
                    break
                resp += chunk
        text = resp.decode("utf-8", errors="replace")
        m = re.search(r"RTSP/1\.0 (\d+)", text)
        status = int(m.group(1)) if m else 0
        return status, text

    def options(self):
        status, text = self.request(
            "OPTIONS", "*",
            "User-Agent: RT-Spark-Test/1.0 (send_test_audio.py)\r\n")
        if status != 200:
            raise RuntimeError(f"OPTIONS failed:\n{text}")
        print("      OPTIONS 200 OK.")

    def announce(self, aes_key, aes_iv):
        pub = self.priv_key.public_key()
        enc_key = pub.encrypt(
            aes_key,
            padding.OAEP(mgf=padding.MGF1(algorithm=hashes.SHA1()),
                         algorithm=hashes.SHA1(), label=None))
        sdp = (
            "v=0\r\n"
            f"o=iTunes {self.cseq} 0 IN IP4 {self.host_ip}\r\n"
            "s=iTunes\r\n"
            f"c=IN IP4 {self.target_ip}\r\n"
            "t=0 0\r\n"
            "m=audio 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 AppleLossless\r\n"
            # 12 integers: the "96" stays (sdp.c keeps it; alac_config expects
            # v[0] = payload type and rejects any count other than 12).
            "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n"
            f"a=rsaaeskey:{base64.b64encode(enc_key).decode('ascii')}\r\n"
            f"a=aesiv:{base64.b64encode(aes_iv).decode('ascii')}\r\n"
        )
        status, text = self.request(
            "ANNOUNCE", f"rtsp://{self.target_ip}/stream",
            "Content-Type: application/sdp\r\n", sdp)
        if status != 200:
            raise RuntimeError(f"ANNOUNCE failed:\n{text}")
        print("      ANNOUNCE 200 OK (board RSA-OAEP-decrypted the AES key).")

    def setup(self):
        status, text = self.request(
            "SETUP", f"rtsp://{self.target_ip}/stream",
            f"Transport: RTP/AVP/UDP;unicast;mode=record;"
            f"control_port={CLIENT_CTRL_PORT};timing_port={CLIENT_TIME_PORT}\r\n")
        if status != 200:
            raise RuntimeError(f"SETUP failed:\n{text}")
        m = re.search(r"server_port=(\d+)", text)
        if not m:
            raise RuntimeError(f"SETUP response missing server_port:\n{text}")
        m_ctl = re.search(r"control_port=(\d+)", text)
        print(f"      SETUP 200 OK. Audio UDP target port: {m.group(1)}"
              + (f", board control port: {m_ctl.group(1)}" if m_ctl else ""))
        return int(m.group(1)), (int(m_ctl.group(1)) if m_ctl else None)

    def record(self):
        status, text = self.request(
            "RECORD", f"rtsp://{self.target_ip}/stream",
            "Range: npt=0-\r\nRTP-Info: seq=1;rtptime=0\r\n")
        if status != 200:
            raise RuntimeError(f"RECORD failed:\n{text}")
        print("      RECORD 200 OK. Board RTP decoder is live (jitter buffer priming).")

    def set_volume(self, db):
        body = f"volume: {db:.6f}\r\n"
        status, _text = self.request(
            "SET_PARAMETER", f"rtsp://{self.target_ip}/stream",
            "Content-Type: text/parameters\r\n", body)
        if status != 200:
            print(f"      Warning: SET_PARAMETER volume -> {status}")
        else:
            print(f"      Volume set to {db:.1f} dB.")

    def keepalive(self):
        try:
            # Short timeout: at stream end the board's RTSP thread can be slow
            # (the RTP thread preempts it); a long wait here would delay the
            # lock-protected teardown and keep the session running while the
            # board's ring drains (which inflated the underrun telemetry).
            self.sock.settimeout(2.0)
            self.request("GET_PARAMETER", f"rtsp://{self.target_ip}/stream")
        except Exception:
            pass
        finally:
            try:
                self.sock.settimeout(10.0)
            except Exception:
                pass

    def teardown(self):
        if not self.sock:
            return
        try:
            self.request("TEARDOWN", f"rtsp://{self.target_ip}/stream")
            print("      TEARDOWN sent. Session released.")
        except Exception:
            pass
        finally:
            try:
                self.sock.close()
            except Exception:
                self.sock = None

# --------------------------------------------------------------------------
# UDP workers: resend servicing + timing responses
# --------------------------------------------------------------------------
def ntp_now():
    """64-bit NTP timestamp (seconds since 1900, fraction), big-endian."""
    return struct.pack("!Q", int((time.time() + NTP_EPOCH_DELTA) * 2 ** 32))


def make_control_workers(target_ip, board_ctl_port, sent_packets, stats):
    """One thread answers 0xD5 resend requests (0xD6 responses) and 0xD2
    timing requests (0xD3 responses). Returns (thread, stop_flag, saw_control)."""
    stop = threading.Event()

    ctl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ctl.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ctl.bind(("", CLIENT_CTRL_PORT))
    ctl.settimeout(0.1)
    tm = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tm.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tm.bind(("", CLIENT_TIME_PORT))
    tm.settimeout(0.1)

    saw_control = threading.Event()

    def note_rx():
        # Anything the BOARD transmits to us (resend/timing requests) proves its
        # uplink is alive; the stream_audio progress line reports how stale it is.
        stats["board_rx"] = stats.get("board_rx", 0) + 1
        stats["last_board_rx"] = time.time()

    def worker():
        while not stop.is_set():
            try:
                data, addr = ctl.recvfrom(256)
                saw_control.set()
                note_rx()
                if len(data) >= 8 and (data[1] & 0x7F) == 0x55:   # resend request
                    first, count = struct.unpack("!HH", data[4:8])
                    print(f"      [Resend request] seq {first} x{count} from {addr[0]}")
                    served = 0
                    for s in range(first, first + max(1, count)):
                        pkt = sent_packets.get(s & 0xFFFF)
                        if pkt:
                            resp = b"\x80\xd6\x00\x01" + pkt  # 4B wrapper + full packet
                            ctl.sendto(resp, (target_ip, board_ctl_port))
                            served += 1
                    stats["resent"] += served
            except socket.timeout:
                pass
            except OSError:
                pass
            try:
                data, addr = tm.recvfrom(64)
                saw_control.set()
                note_rx()
                if len(data) >= 32 and (data[1] & 0x7F) == 0x52:  # timing request
                    resp = bytearray(32)
                    resp[0], resp[1], resp[2], resp[3] = 0x80, 0xD3, 0x00, 0x07
                    resp[8:16] = data[24:32]                      # origin = their t1
                    resp[16:24] = ntp_now()                       # receive
                    resp[24:32] = ntp_now()                       # transmit
                    tm.sendto(bytes(resp), addr)
            except socket.timeout:
                pass
            except OSError:
                pass
        ctl.close()
        tm.close()

    th = threading.Thread(target=worker, daemon=True)
    th.start()
    return th, stop, saw_control

# --------------------------------------------------------------------------
# RTP streaming loop
# --------------------------------------------------------------------------
class KeepaliveThread(threading.Thread):
    """Sends GET_PARAMETER every ~2 s on the RTSP socket so the board's 30 s
    idle timeout never fires mid-stream. Runs in its own thread WITH the
    pacing loop never touching the socket: an RTT spike (the board's RTSP
    thread is lower priority than the streaming path) delays this thread, not
    the audio pacer — a blocking keepalive in the send loop once caused a
    1 s sender stall that would starve the board on long WAV streams."""

    def __init__(self, sender, period_s=2.0):
        super().__init__(daemon=True)
        self.sender = sender
        self.period = period_s
        self.stop_flag = threading.Event()
        self.lock = threading.Lock()

    def run(self):
        while not self.stop_flag.wait(self.period):
            with self.lock:
                try:
                    self.sender.keepalive()
                except Exception:
                    return

    def stop(self):
        self.stop_flag.set()


def stream_audio(sender, audio_udp_port, source, total_frames, aes_key, aes_iv,
                 sent_packets, stats, ka_thread):
    """Real-time paced RTP/ALAC sender. The firmware jitter buffer is
    backpressured by design, so pacing fidelity is what keeps it smooth.
    total_frames=None streams unbounded (live PC audio) until Ctrl-C."""
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (sender.target_ip, audio_udp_port)

    seq = 1
    timestamp = 0
    ssrc = 0x11223344
    total_packets = (total_frames // FRAME_SAMPLES) if total_frames is not None else None

    if total_packets is not None:
        print(f"[5/6] Streaming {total_packets} packets "
              f"({total_frames / SAMPLE_RATE:.2f} s audio) to {dest[0]}:{dest[1]}...")
    else:
        print(f"[5/6] Streaming live PC audio (unbounded, Ctrl-C to stop) "
              f"to {dest[0]}:{dest[1]}...")

    start_time = None
    drift_ms = 0.0
    max_drift_ms = 0.0
    max_send_ms = 0.0      # longest single sendto(): a stall here is the OS/NIC, not the board
    slow_sends = 0         # sendto() calls that took > 10 ms
    base_i = PRIME_BURST   # packet index the current pacing schedule is anchored to
    resyncs = 0            # times the schedule was re-anchored after a stall

    try:
        for i, (chunk_l, chunk_r) in enumerate(source):
            frame = encode_alac_uncompressed_frame(chunk_l, chunk_r)
            cipher = Cipher(algorithms.AES(aes_key), modes.CBC(aes_iv))
            enc = cipher.encryptor()
            payload = enc.update(frame[:CIPHER_BYTES]) + enc.finalize() + frame[CIPHER_BYTES:]

            pt = 0xE0 if i == 0 else RTP_PT_AUDIO   # marker bit on first packet
            packet = struct.pack("!BBHII", 0x80, pt, seq & 0xFFFF,
                                 timestamp & 0xFFFFFFFF, ssrc) + payload
            sent_packets[seq & 0xFFFF] = packet
            stale = (seq - RESEND_HISTORY) & 0xFFFF
            if stale in sent_packets:
                del sent_packets[stale]
            t_send = time.perf_counter()
            udp.sendto(packet, dest)
            send_ms = (time.perf_counter() - t_send) * 1000.0
            if send_ms > max_send_ms:
                max_send_ms = send_ms
            if send_ms > 10.0:
                slow_sends += 1

            seq += 1
            timestamp += FRAME_SAMPLES

            # Pacing: burst PRIME_BURST packets to fill the jitter buffer, then
            # drift-corrected real-time sending (sleep + busy-wait tail).
            if i == PRIME_BURST - 1:
                start_time = time.time()
                base_i = PRIME_BURST
            elif i >= PRIME_BURST:
                target = start_time + (i + 1 - base_i) * PACKET_INTERVAL
                sleep_dur = target - time.time()
                if sleep_dur > 0.002:
                    time.sleep(sleep_dur - 0.0015)
                while time.time() < target:
                    pass
                drift_ms = ((time.time() - start_time) -
                            (i + 1 - base_i) * PACKET_INTERVAL) * 1000.0
                max_drift_ms = max(max_drift_ms, abs(drift_ms))
                if drift_ms > RESYNC_MS:
                    # A stalled sendto() (OS/NIC/hotspot adapter) left us behind
                    # schedule. NEVER catch up by bursting: the board's reorder
                    # window is only a few packets, so a burst overruns it and
                    # discards audio. Drop the lag and resume real-time pacing.
                    resyncs += 1
                    start_time = time.time()
                    base_i = i + 1
                    drift_ms = 0.0

            if (stats.get("stall_at") is not None and not stats.get("stall_done")
                    and (i + 1) * FRAME_SAMPLES / SAMPLE_RATE >= stats["stall_at"]):
                stats["stall_done"] = True
                print(f"      [TEST] simulating a stall: no RTP for {stats['stall_for']:.0f} s "
                      f"(RTSP stays alive, no FLUSH)")
                time.sleep(stats["stall_for"])
                start_time = time.time()          # resume real-time pacing, no catch-up burst
                base_i = i + 1
                drift_ms = 0.0
                print("      [TEST] stall over, resuming RTP")

            done = i + 1
            if done % 100 == 0 or (total_packets is not None and done == total_packets):
                audio_t = done * FRAME_SAMPLES / SAMPLE_RATE
                wall_t = (time.time() - start_time) if start_time else 0.0
                last_rx = stats.get("last_board_rx")
                rx_age = f"{time.time() - last_rx:5.1f}s ago" if last_rx else "never"
                print(f"      {done} pkts ({audio_t:6.2f} s audio, "
                      f"{wall_t:6.2f} s wall, pacing {drift_ms:+7.1f} ms, "
                      f"sendto max {max_send_ms:.1f} ms, slow {slow_sends}, "
                      f"board->PC {stats.get('board_rx', 0)} pkts, last {rx_age})")
    finally:
        udp.close()

    stats["max_drift_ms"] = max_drift_ms
    stats["resyncs"] = resyncs
    wall = time.time() - start_time if start_time else 0.0
    print(f"      Streaming finished ({wall:.2f} s wall, max pacing drift "
          f"{max_drift_ms:.1f} ms, sendto max {max_send_ms:.1f} ms, "
          f"{slow_sends} slow sends, {resyncs} resyncs).")

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------
def parse_args():
    ap = argparse.ArgumentParser(
        description="AirPlay 1 (RAOP) test sender for the RT-Spark receiver.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--ip", help="Board IP (default: auto-discover)")
    ap.add_argument("--port", type=int, default=5000, help="RTSP port")
    ap.add_argument("--duration", type=float, default=10.0,
                    help="Tone duration in seconds; 0 for WAV = whole file, "
                         "0 for --pc = until Ctrl-C")
    ap.add_argument("--volume-db", type=float, default=0.0,
                    help="SET_PARAMETER volume in dB (-30..0)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--wave", metavar="FILE",
                     help="Stream a 44.1 kHz/16-bit/stereo WAV instead of the tone")
    src.add_argument("--pc", action="store_true",
                     help="Stream this PC's live audio output (WASAPI loopback)")
    ap.add_argument("--pc-device", type=int, metavar="INDEX",
                    help="WASAPI loopback device index (see --list-pc-devices)")
    ap.add_argument("--stall-at", type=float, metavar="SEC",
                    help="TEST: stop sending RTP at this audio second (RTSP stays alive, no FLUSH), "
                         "to exercise the board's stall watchdog")
    ap.add_argument("--stall-for", type=float, default=12.0, metavar="SEC",
                    help="TEST: how long the simulated stall lasts")
    ap.add_argument("--serial", default="auto",
                    help="Serial monitor port: COMx, 'auto', or 'none'")
    ap.add_argument("--list-ports", action="store_true", help="List COM ports and exit")
    ap.add_argument("--list-pc-devices", action="store_true",
                    help="List WASAPI loopback devices and exit")
    return ap.parse_args()


def main():
    args = parse_args()

    if args.list_ports:
        if serial is None:
            print("pyserial not installed")
            return 0
        for p in serial.tools.list_ports.comports():
            print(f"{p.device}: {p.description}")
        return 0

    if args.list_pc_devices:
        list_pc_devices()
        return 0

    print("=" * 64)
    print(" RT-Spark AirPlay 1 (RAOP) Audio Stream Verification")
    print("=" * 64)

    # Serial monitor starts FIRST so the boot log is captured for discovery
    port_name = pick_serial_port(args.serial)
    logger = SerialLogger(port_name) if port_name else SerialLogger(None)
    logger.start()
    time.sleep(0.3)

    return_code = 1
    sender = RaopSender(args.ip or "", args.port)
    stats = {"resent": 0, "max_drift_ms": 0.0,
             "stall_at": args.stall_at, "stall_for": args.stall_for}
    workers_stop = None
    saw_control = None
    ka_thread = None
    try:
        sender.load_key()

        # Discovery (skipped when --ip given; --ip is still probed once)
        print("[2/6] Discovering the RT-Spark board...")
        board_ip, probe_sock = discover_board(args, logger)
        if not board_ip:
            print("\nERROR: no RT-Spark receiver found. Check that:")
            print("  * the board is powered and connected to the same network")
            print("    (its LCD shows the IP), or connected to this PC via USB")
            print("  * if the board is in hotspot mode, connect this PC to the")
            print("    'RT-Spark-Audio' Wi-Fi (password 12345678) and re-run")
            print("  * or pass the IP explicitly:  python send_test_audio.py --ip <board-ip>")
            return 1
        if probe_sock is not None:
            sender.adopt(probe_sock, board_ip)
        print(f"[2/6] Board found at {board_ip}:{args.port}")

        # Local IP for the SDP o=/c= lines. The firmware learns the real peer
        # from RECORD's getpeername(), so this is informational only.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect((board_ip, args.port))
        sender.host_ip = probe.getsockname()[0]
        probe.close()
        print(f"      Local host IP for SDP: {sender.host_ip}")

        # Audio source (validated BEFORE the handshake so a bad file fails fast)
        if args.wave:
            source, total_frames = wav_source(args.wave)
            if args.duration and args.duration > 0:
                cap = int(args.duration * SAMPLE_RATE)
                total_frames = min(total_frames, cap)

                def capped(it, limit_chunks):
                    n = 0
                    for chunk in it:
                        if n >= limit_chunks:
                            return
                        yield chunk
                        n += 1
                source = capped(source, total_frames // FRAME_SAMPLES)
        elif args.pc:
            source, total_frames = pc_source(args)
            if args.duration and args.duration > 0:
                cap = int(args.duration * SAMPLE_RATE)
                total_frames = cap

                def capped(it, limit_chunks):
                    n = 0
                    for chunk in it:
                        if n >= limit_chunks:
                            return
                        yield chunk
                        n += 1
                source = capped(source, total_frames // FRAME_SAMPLES)
        else:
            dur = args.duration if args.duration > 0 else 10.0
            fl, fr = tone_source(dur)
            total_frames = len(fl) - (len(fl) % FRAME_SAMPLES)
            fl, fr = fl[:total_frames], fr[:total_frames]

            def chunks():
                for off in range(0, total_frames, FRAME_SAMPLES):
                    yield fl[off: off + FRAME_SAMPLES], fr[off: off + FRAME_SAMPLES]
            source = chunks()

        # Session key material
        aes_key = os.urandom(16)
        aes_iv = os.urandom(16)
        print(f"      Session AES Key: {aes_key.hex()}")
        print(f"      Session AES IV:  {aes_iv.hex()}")

        # RTSP handshake
        sender.connect()
        sender.options()
        sender.announce(aes_key, aes_iv)
        audio_udp_port, board_ctl_port = sender.setup()
        sender.record()
        sender.set_volume(args.volume_db)

        # Control workers: answer the board's 0xD5 resends and 0xD2 timing
        print("[4/6] Preparing control-channel workers...")
        sent_packets = {}
        _th, workers_stop, saw_control = make_control_workers(
            board_ip, board_ctl_port or CLIENT_CTRL_PORT, sent_packets, stats)

        # RTSP keepalive in its own thread (never blocks the audio pacer)
        ka_thread = KeepaliveThread(sender, period_s=2.0)
        ka_thread.start()

        # Stream
        stream_audio(sender, audio_udp_port, source, total_frames,
                     aes_key, aes_iv, sent_packets, stats, ka_thread)

        # Let the DAC drain the buffered tail before teardown
        time.sleep(0.5)
        return_code = 0
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
    except Exception as e:
        print(f"\nERROR: {e}")
        return_code = 1
    finally:
        print("[6/6] Releasing the audio session...")
        if ka_thread is not None:
            ka_thread.stop()
            ka_thread.join(timeout=3.0)
        sender.teardown()
        if workers_stop:
            workers_stop.set()
        time.sleep(0.3)

        if saw_control is not None and saw_control.is_set():
            print(f"      Control channel OK (resends serviced: {stats['resent']})")
        else:
            print("      Note: no 0xD5/0xD2 control datagrams seen from the board.")
            print("            (Windows Firewall may block inbound UDP — allow Python")
            print("             on private networks for full resend/timing coverage.)")

        health = [ln for ln in logger.lines if "health:" in ln]
        if health:
            print("      Board health telemetry:")
            for ln in health[-3:]:
                print(f"        {ln.split('health:')[-1].strip()}")
        logger.stop()

    print("=" * 64)
    print(" Audio test stream complete!")
    print("=" * 64)
    return return_code


if __name__ == "__main__":
    sys.exit(main())
