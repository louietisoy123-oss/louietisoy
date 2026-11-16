#!/usr/bin/env python3
"""
send_test_audio.py - AirPlay 1 (RAOP) Audio Streamer Test for RT-Spark
Tests RTSP Handshake, RSA-OAEP Key Exchange, ALAC bitstream encoding,
AES-128-CBC encryption, and RTP audio streaming to RT-Spark (STM32F407).
"""

import sys
import os
import time
import math
import struct
import socket
import base64
import threading
import re

if sys.platform == 'win32':
    import ctypes
    try:
        ctypes.windll.winmm.timeBeginPeriod(1)
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
# BitWriter for uncompressed ALAC packaging
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
    Packs 352 stereo samples into an uncompressed ALAC frame.
    Bitstream specification:
      - 3 bits: channels = 1 (meaning 2 channels)
      - 4 bits: 0
      - 12 bits: 0
      - 1 bit:  hassize = 0
      - 2 bits: uncompressed_bytes = 0
      - 1 bit:  isnotcompressed = 1
      - For each sample (352 pairs):
          16 bits: sample_left (signed int16)
          16 bits: sample_right (signed int16)
    Total: 23 header bits + 352 * 32 = 11,287 bits = 1,411 bytes.
    """
    assert len(samples_l) == 352 and len(samples_r) == 352
    bw = BitWriter()
    bw.write(1, 3)   # channels = 1 (2 channels)
    bw.write(0, 4)   # 4 bits 0
    bw.write(0, 12)  # 12 bits 0
    bw.write(0, 1)   # hassize = 0 (uses default 352 samples)
    bw.write(0, 2)   # uncompressed_bytes = 0
    bw.write(1, 1)   # isnotcompressed = 1

    for l, r in zip(samples_l, samples_r):
        bw.write(l & 0xFFFF, 16)
        bw.write(r & 0xFFFF, 16)

    return bw.finish()

# --------------------------------------------------------------------------
# Audio Tone Generator (Musical Chord Arpeggio)
# --------------------------------------------------------------------------
def generate_audio_samples(duration_sec=3.0, sample_rate=44100):
    """
    Generates a 3-second stereo musical chord arpeggio (C major triad: C5, E5, G5, C6).
    Returns list of 352-sample chunks: [(samples_l, samples_r), ...]
    """
    num_samples = int(duration_sec * sample_rate)
    frames_l = []
    frames_r = []

    # Frequencies: C5 (523.25), E5 (659.25), G5 (783.99), C6 (1046.50)
    notes = [
        (0.00, 0.75, 523.25, 0.8, 0.3),   # C5 (panned left)
        (0.75, 1.50, 659.25, 0.4, 0.7),   # E5 (panned right)
        (1.50, 2.25, 783.99, 0.7, 0.4),   # G5 (panned left-center)
        (2.25, 3.00, 1046.50, 0.6, 0.6),  # C6 (centered bell)
    ]

    amplitude = 12000.0  # Safe headroom below int16 max (32767)

    for i in range(num_samples):
        t = i / sample_rate
        val_l = 0.0
        val_r = 0.0

        for start_t, end_t, freq, pan_l, pan_r in notes:
            if t >= start_t and t < end_t:
                dt = t - start_t
                note_dur = end_t - start_t
                # Smooth envelope (fast attack 10ms, exponential decay)
                attack = min(1.0, dt / 0.01)
                decay = math.exp(-3.0 * (dt / note_dur))
                env = attack * decay
                wave = math.sin(2.0 * math.pi * freq * dt)
                val_l += wave * env * pan_l
                val_r += wave * env * pan_r
            elif t >= end_t and (t - end_t) < 0.5:
                # Ringing tail
                dt = t - start_t
                tail_dt = t - end_t
                decay = math.exp(-3.0) * math.exp(-8.0 * tail_dt)
                wave = math.sin(2.0 * math.pi * freq * dt)
                val_l += wave * decay * pan_l
                val_r += wave * decay * pan_r

        # Soft clip to prevent any distortion
        s_l = int(max(-32760, min(32760, val_l * amplitude)))
        s_r = int(max(-32760, min(32760, val_r * amplitude)))
        frames_l.append(s_l)
        frames_r.append(s_r)

    # Split into 352-sample packets
    packets = []
    chunk_size = 352
    for offset in range(0, len(frames_l) - chunk_size, chunk_size):
        chunk_l = frames_l[offset : offset + chunk_size]
        chunk_r = frames_r[offset : offset + chunk_size]
        packets.append((chunk_l, chunk_r))

    return packets

# --------------------------------------------------------------------------
# Background Serial Monitor for COM7
# --------------------------------------------------------------------------
class SerialLogger(threading.Thread):
    def __init__(self, port="COM7", baudrate=115200):
        super().__init__(daemon=True)
        self.port = port
        self.baudrate = baudrate
        self.running = True
        self.ser = None

    def run(self):
        if serial is None:
            return
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=0.1)
            print(f"[SerialMonitor] Attached to {self.port} at {self.baudrate}")
            while self.running:
                line = self.ser.readline()
                if line:
                    try:
                        text = line.decode('utf-8', errors='replace').rstrip()
                        if text:
                            print(f"[RT-Spark UART] {text}")
                    except Exception:
                        pass
        except Exception as e:
            print(f"[SerialMonitor] Note: Could not open {self.port} ({e})")

    def stop(self):
        self.running = False
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass

# --------------------------------------------------------------------------
# Main Streaming Protocol Implementation
# --------------------------------------------------------------------------
def send_rtsp(sock, req_str):
    sock.sendall(req_str.encode('utf-8'))
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(1024)
        if not chunk:
            break
        resp += chunk
    return resp.decode('utf-8', errors='replace')

def main():
    target_ip = "192.168.1.56"
    target_rtsp_port = 5000
    host_ip = "192.168.1.2"

    print("================================================================")
    print(f" RT-Spark AirPlay 1 (RAOP) Audio Stream Verification")
    print(f" Target: {target_ip}:{target_rtsp_port} | Host: {host_ip}")
    print("================================================================")

    # 1. Start Serial Logger
    logger = SerialLogger("COM7", 115200)
    logger.start()
    time.sleep(0.3)

    # 2. Load AirPlay RSA Key
    priv_key = serialization.load_pem_private_key(RAOP_KEY_PEM.encode('ascii'), password=None)
    pub_key = priv_key.public_key()
    print("[1/6] RSA 2048-bit AirPort Express key loaded.")

    # 3. Generate random 16-byte AES Session Key & IV
    aes_key = os.urandom(16)
    aes_iv = os.urandom(16)
    print(f"[2/6] Session AES Key: {aes_key.hex()}")
    print(f"      Session AES IV:  {aes_iv.hex()}")

    # Encrypt AES Key using RSA-OAEP SHA-1
    enc_aes_key = pub_key.encrypt(
        aes_key,
        padding.OAEP(
            mgf=padding.MGF1(algorithm=hashes.SHA1()),
            algorithm=hashes.SHA1(),
            label=None
        )
    )
    b64_key = base64.b64encode(enc_aes_key).decode('ascii')
    b64_iv = base64.b64encode(aes_iv).decode('ascii')

    # 4. Connect RTSP over TCP
    print(f"[3/6] Connecting to RTSP server {target_ip}:{target_rtsp_port}...")
    rtsp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    rtsp_sock.settimeout(15.0)
    try:
        rtsp_sock.connect((target_ip, target_rtsp_port))
    except Exception as e:
        print(f"Error connecting to RTSP server: {e}")
        logger.stop()
        sys.exit(1)
    print("      RTSP TCP connected.")

    # OPTIONS
    resp = send_rtsp(rtsp_sock,
        "OPTIONS * RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "User-Agent: iTunes/10.6 (Windows; N)\r\n"
        "Client-Instance: 0123456789ABCDEF\r\n"
        "\r\n"
    )
    if "200 OK" not in resp:
        print(f"OPTIONS failed:\n{resp}")
        rtsp_sock.close()
        logger.stop()
        sys.exit(1)
    print("      OPTIONS 200 OK.")

    # ANNOUNCE with SDP
    sdp = (
        "v=0\r\n"
        f"o=iTunes 0 0 IN IP4 {host_ip}\r\n"
        "s=iTunes\r\n"
        f"c=IN IP4 {target_ip}\r\n"
        "t=0 0\r\n"
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 AppleLossless\r\n"
        "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n"
        f"a=rsaaeskey:{b64_key}\r\n"
        f"a=aesiv:{b64_iv}\r\n"
    )
    announce_req = (
        f"ANNOUNCE rtsp://{target_ip}/stream RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Content-Type: application/sdp\r\n"
        f"Content-Length: {len(sdp)}\r\n"
        "\r\n"
        f"{sdp}"
    )
    resp = send_rtsp(rtsp_sock, announce_req)
    if "200 OK" not in resp:
        print(f"ANNOUNCE failed:\n{resp}")
        rtsp_sock.close()
        logger.stop()
        sys.exit(1)
    print("      ANNOUNCE 200 OK (AES key decrypted by RT-Spark).")

    # SETUP
    ctl_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ctl_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        ctl_sock.bind(("", 6001))
        ctl_sock.setblocking(False)
    except Exception:
        pass

    tm_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tm_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        tm_sock.bind(("", 6002))
        tm_sock.setblocking(False)
    except Exception:
        pass

    setup_req = (
        f"SETUP rtsp://{target_ip}/stream RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Transport: RTP/AVP/UDP;unicast;mode=record;control_port=6001;timing_port=6002\r\n"
        "\r\n"
    )
    resp = send_rtsp(rtsp_sock, setup_req)
    if "200 OK" not in resp:
        print(f"SETUP failed:\n{resp}")
        rtsp_sock.close()
        logger.stop()
        sys.exit(1)

    m = re.search(r"server_port=(\d+)", resp)
    if not m:
        print(f"Could not find server_port in SETUP response:\n{resp}")
        rtsp_sock.close()
        logger.stop()
        sys.exit(1)
    audio_udp_port = int(m.group(1))

    m_ctl = re.search(r"control_port=(\d+)", resp)
    server_ctl_port = int(m_ctl.group(1)) if m_ctl else None
    print(f"      SETUP 200 OK. Audio UDP Destination Port: {audio_udp_port}")

    # RECORD
    record_req = (
        f"RECORD rtsp://{target_ip}/stream RTSP/1.0\r\n"
        "CSeq: 4\r\n"
        "Range: npt=0-\r\n"
        "RTP-Info: seq=1;rtptime=0\r\n"
        "\r\n"
    )
    resp = send_rtsp(rtsp_sock, record_req)
    if "200 OK" not in resp:
        print(f"RECORD failed:\n{resp}")
        rtsp_sock.close()
        logger.stop()
        sys.exit(1)
    print("      RECORD 200 OK. Hardware decoding & DAC pipeline active.")

    # SET_PARAMETER (Volume = 0.0 dB, 100%)
    vol_body = "volume: 0.000000\r\n"
    vol_req = (
        f"SET_PARAMETER rtsp://{target_ip}/stream RTSP/1.0\r\n"
        "CSeq: 5\r\n"
        "Content-Type: text/parameters\r\n"
        f"Content-Length: {len(vol_body)}\r\n"
        "\r\n"
        f"{vol_body}"
    )
    send_rtsp(rtsp_sock, vol_req)
    print("      Volume set to 0.0 dB (100%).")

    # 5. Generate Audio and Stream RTP Packets
    print("[4/6] Synthesizing stereo chord arpeggio (C5 -> E5 -> G5 -> C6 bell)...")
    packets = generate_audio_samples(duration_sec=3.0, sample_rate=44100)
    print(f"      Generated {len(packets)} audio packets ({len(packets) * 352 / 44100:.2f} seconds).")

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest_addr = (target_ip, audio_udp_port)

    # Dictionary to store recently sent packets for RTP resend servicing
    sent_packets = {}
    resend_running = True

    def resend_worker():
        ctl_sock.settimeout(0.05)
        while resend_running:
            try:
                data, addr = ctl_sock.recvfrom(256)
                if len(data) >= 8 and (data[1] & 0x7F) == 0x55 and server_ctl_port:
                    first_seq, count = struct.unpack("!HH", data[4:8])
                    print(f"      [Resend Request from RT-Spark] Missing seq {first_seq} (count={count})")
                    for s_idx in range(first_seq, first_seq + count):
                        s_key = s_idx & 0xFFFF
                        if s_key in sent_packets:
                            resend_pkt = b"\x80\xd6\x00\x01" + sent_packets[s_key]
                            ctl_sock.sendto(resend_pkt, (target_ip, server_ctl_port))
                            print(f"      [Resent packet] seq={s_key} to {target_ip}:{server_ctl_port}")
            except (socket.timeout, BlockingIOError, OSError):
                pass

    resend_th = threading.Thread(target=resend_worker, daemon=True)
    resend_th.start()

    print(f"[5/6] Streaming audio packets to {target_ip}:{audio_udp_port}...")
    seq = 1
    timestamp = 0
    ssrc = 0x11223344

    start_time = time.time()
    packet_interval = 352.0 / 44100.0  # ~7.98 ms

    for i, (chunk_l, chunk_r) in enumerate(packets):
        # 1. Uncompressed ALAC frame
        alac_frame = encode_alac_uncompressed_frame(chunk_l, chunk_r)
        assert len(alac_frame) == 1411

        # 2. AES-128-CBC Encrypt first 1408 bytes (88 blocks of 16)
        cipher = Cipher(algorithms.AES(aes_key), modes.CBC(aes_iv))
        enc = cipher.encryptor()
        encrypted_part = enc.update(alac_frame[:1408]) + enc.finalize()
        payload = encrypted_part + alac_frame[1408:]  # 3 plaintext bytes

        # 3. RTP Header (12 bytes)
        # Marker bit set on first packet (0xE0) or standard 0x60
        pt = 0xE0 if i == 0 else 0x60
        header = struct.pack("!BBHII", 0x80, pt, seq & 0xFFFF, timestamp & 0xFFFFFFFF, ssrc)
        packet = header + payload

        sent_packets[seq & 0xFFFF] = packet
        # Prune old packets from history
        if (seq - 64) & 0xFFFF in sent_packets:
            del sent_packets[(seq - 64) & 0xFFFF]

        udp_sock.sendto(packet, dest_addr)

        seq += 1
        timestamp += 352

        # Pacing: pre-send first 4 packets immediately to prime the jitter buffer,
        # then pace remainder with accurate microsecond hybrid wait
        if i >= 4:
            target_time = start_time + (i + 1 - 4) * packet_interval
            sleep_dur = target_time - time.time()
            if sleep_dur > 0.002:
                time.sleep(sleep_dur - 0.0015)
            while time.time() < target_time:
                pass

        if (i + 1) % 50 == 0 or (i + 1) == len(packets):
            print(f"      Sent {i + 1}/{len(packets)} packets ({(i + 1) * 352 / 44100:.2f}s audio)")

        # Send RTSP keepalive every ~1s (125 packets) to maintain active TCP session & fresh ARP cache
        if (i + 1) % 125 == 0 and (i + 1) < len(packets):
            cseq_kp = 5 + (i + 1) // 125
            get_param = (
                f"GET_PARAMETER rtsp://{target_ip}/stream RTSP/1.0\r\n"
                f"CSeq: {cseq_kp}\r\n"
                "\r\n"
            )
            send_rtsp(rtsp_sock, get_param)

    print(f"      Audio streaming completed in {time.time() - start_time:.2f}s.")
    resend_running = False

    # 6. Wait for DAC DMA to play buffered audio, then TEARDOWN
    time.sleep(0.3)
    print("[6/6] Sending TEARDOWN to gracefully release audio session...")
    teardown_req = (
        f"TEARDOWN rtsp://{target_ip}/stream RTSP/1.0\r\n"
        "CSeq: 8\r\n"
        "\r\n"
    )
    resp = send_rtsp(rtsp_sock, teardown_req)
    if "200 OK" in resp:
        print("      TEARDOWN 200 OK. Session released.")

    rtsp_sock.close()
    udp_sock.close()
    try:
        ctl_sock.close()
        tm_sock.close()
    except Exception:
        pass
    time.sleep(0.5)
    logger.stop()

    print("================================================================")
    print(" Audio test stream complete!")
    print("================================================================")

if __name__ == "__main__":
    main()
