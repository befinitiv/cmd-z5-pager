"""A minimal MeshCore repeater on the emulator's radio (UDP multicast), for
tests/emul/run_app.sh: it answers logins (password "test"; others fail),
status requests, telemetry requests (also flooded ones, with the path back)
and CLI commands ("> ok: <command>"), and forwards traces that name it,
adding its SNR. It does not repeat anything else.

Pure Python (Ed25519 key, X25519 shared secret as MeshCore: the Edwards key
converted to Montgomery form); AES and HMAC from pycryptodome.
Usage: fake_repeater.py <seconds> [port]
"""

import hashlib
import hmac
import os
import socket
import struct
import sys
import time

from Crypto.Cipher import AES

# Ed25519 / X25519 ---------------------------------------------------------------

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
I = pow(2, (P - 1) // 4, P)


def xrecover(y):
    xx = (y * y - 1) * pow(D * y * y + 1, P - 2, P)
    x = pow(xx, (P + 3) // 8, P)
    if (x * x - xx) % P != 0:
        x = x * I % P
    if x % 2 != 0:
        x = P - x
    return x


BY = 4 * pow(5, P - 2, P) % P
B = (xrecover(BY), BY)


def edwards(p, q):
    (x1, y1), (x2, y2) = p, q
    x3 = (x1 * y2 + x2 * y1) * pow(1 + D * x1 * x2 * y1 * y2, P - 2, P)
    y3 = (y1 * y2 + x1 * x2) * pow(1 - D * x1 * x2 * y1 * y2, P - 2, P)
    return (x3 % P, y3 % P)


def scalarmult(p, e):
    q = (0, 1)
    while e:
        if e & 1:
            q = edwards(q, p)
        p = edwards(p, p)
        e >>= 1
    return q


def encode_point(p):
    x, y = p
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def keypair(seed):
    h = hashlib.sha512(seed).digest()
    a = bytearray(h[:32])
    a[0] &= 248
    a[31] &= 63
    a[31] |= 64
    scalar = int.from_bytes(a, "little")
    return encode_point(scalarmult(B, scalar)), bytes(a)


def x25519(k, u):
    x1, x2, z2, x3, z3, swap = u, 1, 0, u, 1, 0
    for t in reversed(range(255)):
        kt = (k >> t) & 1
        swap ^= kt
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = kt
        a, b = x2 + z2, x2 - z2
        aa, bb = a * a % P, b * b % P
        e = aa - bb
        c, d = x3 + z3, x3 - z3
        da, cb = d * a % P, c * b % P
        x3 = (da + cb) ** 2 % P
        z3 = x1 * (da - cb) ** 2 % P
        x2 = aa * bb % P
        z2 = e * (aa + 121665 * e) % P
    if swap:
        x2, z2 = x3, z3
    return x2 * pow(z2, P - 2, P) % P


def shared_secret(my_scalar, their_pub):
    y = int.from_bytes(their_pub, "little") & ((1 << 255) - 1)
    u = (1 + y) * pow(1 - y, P - 2, P) % P
    k = bytearray(my_scalar)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    return x25519(int.from_bytes(k, "little"), u).to_bytes(32, "little")


def encrypt(secret, data):
    data = data + b"\0" * (-len(data) % 16)
    c = AES.new(secret[:16], AES.MODE_ECB).encrypt(data)
    return hmac.new(secret, c, hashlib.sha256).digest()[:2] + c


def decrypt(secret, data):
    mac, c = data[:2], data[2:]
    if len(c) == 0 or len(c) % 16 or hmac.new(secret, c, hashlib.sha256).digest()[:2] != mac:
        return None
    return AES.new(secret[:16], AES.MODE_ECB).decrypt(c)


# Packets ------------------------------------------------------------------------

REQ, RESPONSE, TXT_MSG, ACK, ADVERT, GRP_TXT, GRP_DATA, ANON_REQ, PATH, TRACE = range(10)
FLOOD, DIRECT = 1, 2


def decode(frame):
    hdr = frame[0]
    route, ptype = hdr & 3, (hdr >> 2) & 15
    i = 1
    if route in (0, 3):
        i += 4
    plen = frame[i]
    n = (plen & 63) * ((plen >> 6) + 1)
    path = frame[i + 1:i + 1 + n]
    return route, ptype, plen, path, frame[i + 1 + n:]


def encode(route, ptype, path, payload, plen=None):
    return bytes([ptype << 2 | route, len(path) if plen is None else plen]) + bytes(path) + payload


class Repeater:
    def __init__(self, sock, group):
        self.sock, self.group = sock, group
        self.pub, self.scalar = keypair(b"cmd-z5 fake repeater seed 000001")
        self.hash = self.pub[0]
        self.clients = {}  # hash -> (pub, secret, path back or None)
        self.instance = struct.unpack("<I", os.urandom(4))[0]
        self.cfg = None
        self.log = []

    def send(self, frame):
        # After the request's airtime (the datagram comes when it starts).
        time.sleep(1.5)
        freq, bw, sf, cr, sync = self.cfg
        env = b"CZRF" + struct.pack("<IIIBBBB", self.instance, freq, bw, sf, cr, sync, len(frame))
        self.sock.sendto(env + frame, self.group)
        print("sent", frame[:2].hex(), len(frame), round(time.time() % 100, 2), flush=True)

    def reply(self, client, ptype, data, route, in_path):
        pub, secret, back = client
        if route == FLOOD:
            # The path back to us, with the answer in it, by flood.
            body = bytes([len(in_path)]) + bytes(in_path) + bytes([ptype]) + data
            payload = bytes([pub[0], self.hash]) + encrypt(secret, body)
            self.send(encode(FLOOD, PATH, b"", payload))
        else:
            payload = bytes([pub[0], self.hash]) + encrypt(secret, data)
            self.send(encode(DIRECT, ptype, back or b"", payload))

    def handle(self, frame, cfg):
        self.cfg = cfg
        try:
            route, ptype, plen, path, payload = decode(frame)
        except IndexError:
            return
        if ptype == TRACE and route == DIRECT:
            flags = payload[8]
            hashes = payload[9:]
            n = plen & 63
            if n < len(hashes) and hashes[n] == self.hash:
                self.log.append("trace")
                self.send(encode(DIRECT, TRACE, bytes(path) + bytes([40]), payload, n + 1))
            return
        if route == DIRECT and (plen & 63) > 0:
            return
        if ptype == ANON_REQ and payload[0] == self.hash:
            pub = payload[1:33]
            secret = shared_secret(self.scalar, pub)
            data = decrypt(secret, payload[33:])
            if data is None:
                return
            pw = data[4:].rstrip(b"\0")
            client = (pub, secret, None)
            self.clients[pub[0]] = client
            self.log.append("login " + pw.decode(errors="replace"))
            now = struct.pack("<I", int(time.time()))
            if pw == b"test":
                # LOGIN_OK, keep-alive 0, admin, ACL permissions, 4 random, firmware level
                resp = now + bytes([0, 0, 1, 3]) + os.urandom(4) + bytes([2])
                self.reply(client, RESPONSE, resp, route, path)
            return
        if ptype in (REQ, TXT_MSG) and payload[0] == self.hash and payload[1] in self.clients:
            client = self.clients[payload[1]]
            data = decrypt(client[1], payload[2:])
            if data is None:
                return
            if ptype == REQ:
                tag, kind = data[:4], data[4]
                if kind == 1:
                    self.log.append("status")
                    # batt mV, queue, noise, rssi, counters ...: 52 bytes as in MeshCore
                    resp = tag + struct.pack("<HHhh", 4100, 0, -110, -80) + bytes(44)
                    self.reply(client, RESPONSE, resp, route, path)
                elif kind == 3:
                    self.log.append("telemetry" + (" flood" if route == FLOOD else ""))
                    resp = tag + bytes([1, 116, 0x01, 0x9a])  # 4.10 V
                    self.reply(client, RESPONSE, resp, route, path)
            elif (data[4] >> 2) == 1:
                cmd = data[5:].rstrip(b"\0").decode(errors="replace")
                self.log.append("cli " + cmd)
                text = ("> ok: " + cmd).encode()
                ts = struct.pack("<I", int(time.time()))
                self.reply(client, TXT_MSG, ts + bytes([1 << 2]) + text, DIRECT, path)


def main():
    seconds = float(sys.argv[1])
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 4403
    group = ("239.67.90.53", port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(("", port))
    lo = socket.inet_aton("127.0.0.1")
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group[0]) + lo)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, lo)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    sock.settimeout(0.2)
    r = Repeater(sock, group)
    print("pub", r.pub.hex(), flush=True)
    end = time.time() + seconds
    while time.time() < end:
        try:
            buf = sock.recv(512)
        except socket.timeout:
            continue
        if len(buf) < 20 or buf[:4] != b"CZRF":
            continue
        inst, freq, bw, sf, cr, sync, n = struct.unpack("<IIIBBBB", buf[4:20])
        if inst == r.instance:
            continue
        print("heard", buf[20:22].hex(), n, round(time.time() % 100, 2), flush=True)
        r.handle(buf[20:20 + n], (freq, bw, sf, cr, sync))
        while r.log:
            print(r.log.pop(0), flush=True)


if __name__ == "__main__":
    main()
