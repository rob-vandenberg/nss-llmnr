#!/usr/bin/env python3
"""Fake multi-homed Windows host for testing: answers LLMNR queries for one
name from three source addresses (like three NICs on one LAN), with the
LAN address deliberately answering LAST."""
import socket, struct, sys, time

NAME = sys.argv[1].lower()
# (source address, delay seconds) - public NICs answer first
NICS = [("92.66.200.107", 0.000), ("92.66.200.108", 0.010), ("10.0.0.141", 0.040)]

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
rx.bind(("0.0.0.0", 5355))
mreq = struct.pack("4s4s", socket.inet_aton("224.0.0.252"), socket.inet_aton("10.0.0.141"))
rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

tx = {}
for ip, _ in NICS:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((ip, 5355))
    tx[ip] = s

print("responder ready", flush=True)
while True:
    data, (src, sport) = rx.recvfrom(1500)
    if len(data) < 12 or data[2] & 0x80:
        continue
    qid = data[:2]
    ln = data[12]
    qname = data[13:13 + ln].decode(errors="replace").lower()
    question = data[12:12 + 1 + ln + 1 + 4]
    print(f"query {qname!r} from {src}", flush=True)
    if qname != NAME:
        continue
    t0 = time.time()
    for ip, delay in NICS:
        while time.time() - t0 < delay:
            time.sleep(0.001)
        hdr = qid + b"\x80\x00" + struct.pack(">HHHH", 1, 1, 0, 0)
        ans = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 30, 4) + socket.inet_aton(ip)
        tx[ip].sendto(hdr + question + ans, (src, sport))
