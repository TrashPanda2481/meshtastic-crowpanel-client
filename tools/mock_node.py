#!/usr/bin/env python3
"""
Mock Meshtastic node -- a fake node that speaks the Meshtastic TCP API so the
CrowPanel client can be developed/tested without live mesh traffic.

It listens on :4403, replies to want_config with a config dump (my_info + a couple
of NodeInfos + config_complete), periodically pushes synthetic TEXT packets, AND
decodes anything the client SENDS (framed ToRadio) so we can verify canned-message
sending end to end.

Run:  tools\\venv\\Scripts\\python -u tools\\mock_node.py
"""
import socket
import time
from meshtastic.protobuf import mesh_pb2, portnums_pb2

HOST = "0.0.0.0"
PORT = 4403
START1, START2 = 0x94, 0xC3

MY_NUM = 0x11223344
NODES = [
    (0xA1B2C3D4, "North Ridge", "NRdg"),
    (0x0A0B0C0D, "Base Camp",   "Base"),
]
MESSAGES = [
    "Hello from the mock node!",
    "Trail is clear at the summit.",
    "Radio check - how copy?",
    "Weather turning, heading back.",
]
TEXT_INTERVAL = 12  # seconds between synthetic texts


def frame(payload: bytes) -> bytes:
    n = len(payload)
    return bytes([START1, START2, (n >> 8) & 0xFF, n & 0xFF]) + payload


def fr_my_info() -> bytes:
    fr = mesh_pb2.FromRadio()
    fr.my_info.my_node_num = MY_NUM
    return fr.SerializeToString()


def fr_node_info(num, long_name, short_name) -> bytes:
    fr = mesh_pb2.FromRadio()
    ni = fr.node_info
    ni.num = num
    ni.user.id = "!%08x" % num
    ni.user.long_name = long_name
    ni.user.short_name = short_name
    ni.last_heard = int(time.time())
    return fr.SerializeToString()


def fr_config_complete(cid=1) -> bytes:
    fr = mesh_pb2.FromRadio()
    fr.config_complete_id = cid
    return fr.SerializeToString()


def fr_text(from_num, channel, text) -> bytes:
    fr = mesh_pb2.FromRadio()
    p = fr.packet
    setattr(p, "from", from_num)   # 'from' is a Python keyword
    p.to = 0xFFFFFFFF
    p.channel = channel
    p.id = int(time.time()) & 0x7FFFFFFF
    p.rx_time = int(time.time())
    p.decoded.portnum = portnums_pb2.TEXT_MESSAGE_APP
    p.decoded.payload = text.encode("utf-8")
    return fr.SerializeToString()


def parse_frames(buf: bytes):
    """Return (list_of_payloads, remaining_buf) for complete 0x94 0xc3 frames."""
    out = []
    i = 0
    while i + 4 <= len(buf):
        if buf[i] == START1 and buf[i + 1] == START2:
            ln = (buf[i + 2] << 8) | buf[i + 3]
            if i + 4 + ln <= len(buf):
                out.append(buf[i + 4:i + 4 + ln])
                i += 4 + ln
                continue
            break  # incomplete frame, wait for more
        i += 1
    return out, buf[i:]


def handle_toradio(payload: bytes):
    try:
        tr = mesh_pb2.ToRadio()
        tr.ParseFromString(payload)
    except Exception as e:
        print(f"[mock] RX unparseable ToRadio ({len(payload)}B): {e}", flush=True)
        return
    w = tr.WhichOneof("payload_variant")
    if w == "packet":
        pkt = tr.packet
        if pkt.WhichOneof("payload_variant") == "decoded" and pkt.decoded.portnum == portnums_pb2.TEXT_MESSAGE_APP:
            txt = bytes(pkt.decoded.payload).decode("utf-8", "replace")
            print(f"[mock] *** RX from CrowPanel: ch{pkt.channel}  text={txt!r} ***", flush=True)
        else:
            print(f"[mock] RX packet from client (port {pkt.decoded.portnum})", flush=True)
    elif w == "want_config_id":
        print(f"[mock] RX want_config_id={tr.want_config_id}", flush=True)
    elif w is not None:
        print(f"[mock] RX ToRadio variant={w}", flush=True)


def serve_client(conn, addr):
    print(f"[mock] client connected: {addr}", flush=True)
    conn.settimeout(1.0)

    # send the config dump straight away (client sends want_config on connect)
    conn.sendall(frame(fr_my_info()))
    for num, ln, sn in NODES:
        conn.sendall(frame(fr_node_info(num, ln, sn)))
    conn.sendall(frame(fr_config_complete(1)))
    print(f"[mock] sent config dump (my_info + {len(NODES)} nodes + config_complete)", flush=True)

    i = 0
    last = 0.0
    inbuf = b""
    try:
        while True:
            try:
                chunk = conn.recv(512)
                if chunk == b"":
                    print("[mock] client closed", flush=True)
                    break
                inbuf += chunk
                frames, inbuf = parse_frames(inbuf)
                for p in frames:
                    handle_toradio(p)
            except socket.timeout:
                pass
            now = time.time()
            if now - last > TEXT_INTERVAL:
                last = now
                sender = NODES[i % len(NODES)][0]
                text = MESSAGES[i % len(MESSAGES)]
                conn.sendall(frame(fr_text(sender, 0, text)))
                print(f"[mock] sent TEXT from !{sender:08x}: {text}", flush=True)
                i += 1
    except Exception as e:
        print("[mock] client error:", e, flush=True)
    finally:
        conn.close()


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(1)
    print(f"[mock] listening on {HOST}:{PORT}  (my_node_num=0x{MY_NUM:08x})", flush=True)
    while True:
        conn, addr = s.accept()
        serve_client(conn, addr)


if __name__ == "__main__":
    main()
