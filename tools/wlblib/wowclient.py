"""A minimal WoW 3.3.5a (build 12340) client for e2e cases that need a real human player.

Logs a scripted human into the world server the way the game client does (no playerbots AI on
it, so the server and playerbots treat it as a real player): world auth with a session key the
test writes into the account, ARC4-drop1024 header encryption, character enum/create, login, and
a few packets (group invite, chat). Everything the server sends is decoded into `chats` (and
`group_invites` / `auth_code`); the rest is read and dropped.

The authserver's SRP6 step is skipped: the test puts a known session key in `account.session_key`
(`prepare_account`), exactly what a successful authserver login leaves there.
"""

from __future__ import annotations

import hashlib
import hmac
import os
import socket
import struct
import subprocess
import threading
import time
import zlib

from wlblib.common import MYSQL_CNF

BUILD = 12340

# Opcodes (AzerothCore Opcodes.h).
SMSG_AUTH_CHALLENGE = 0x1EC
CMSG_AUTH_SESSION = 0x1ED
SMSG_AUTH_RESPONSE = 0x1EE
CMSG_CHAR_CREATE = 0x036
CMSG_CHAR_ENUM = 0x037
SMSG_CHAR_CREATE = 0x03A
SMSG_CHAR_ENUM = 0x03B
CMSG_PLAYER_LOGIN = 0x03D
SMSG_LOGIN_VERIFY_WORLD = 0x236
CMSG_GROUP_INVITE = 0x06E
SMSG_GROUP_LIST = 0x07D
CMSG_MESSAGECHAT = 0x095
SMSG_MESSAGECHAT = 0x096
MSG_MINIMAP_PING = 0x1D5
SMSG_TIME_SYNC_REQ = 0x390
CMSG_TIME_SYNC_RESP = 0x391

AUTH_OK = 0x0C
CHAR_CREATE_SUCCESS = 0x2F

# ChatMsg (SharedDefines.h).
CHAT_MSG_SAY = 0x01
CHAT_MSG_PARTY = 0x02
CHAT_MSG_RAID = 0x03
CHAT_MSG_WHISPER = 0x07
CHAT_MSG_PARTY_LEADER = 0x33
CHAT_TYPE_NAMES = {CHAT_MSG_SAY: "say", CHAT_MSG_PARTY: "party", CHAT_MSG_RAID: "raid",
                   CHAT_MSG_WHISPER: "whisper", CHAT_MSG_PARTY_LEADER: "party"}
LANG_ORCISH = 1
LANG_COMMON = 7
ALLIANCE_RACES = frozenset({1, 3, 4, 7, 11})  # Human, Dwarf, Night Elf, Gnome, Draenei.

SERVER_ENCRYPTION_KEY = bytes.fromhex("CC98AE04E897EACA12DDC09342915357")
SERVER_DECRYPTION_KEY = bytes.fromhex("C2B3723CC6AED9B5343C53EE2F4367CE")


class _Arc4:
    def __init__(self, key: bytes):
        s = list(range(256))
        j = 0
        for i in range(256):
            j = (j + s[i] + key[i % len(key)]) & 0xFF
            s[i], s[j] = s[j], s[i]
        self._s, self._i, self._j = s, 0, 0
        self.process(bytes(1024))  # WoW uses ARC4-drop1024.

    def process(self, data: bytes) -> bytes:
        s, i, j = self._s, self._i, self._j
        out = bytearray(len(data))
        for n, b in enumerate(data):
            i = (i + 1) & 0xFF
            j = (j + s[i]) & 0xFF
            s[i], s[j] = s[j], s[i]
            out[n] = b ^ s[(s[i] + s[j]) & 0xFF]
        self._i, self._j = i, j
        return bytes(out)


def _cstr(data: bytes, pos: int) -> tuple[str, int]:
    end = data.index(b"\0", pos)
    return data[pos:end].decode("utf-8", "replace"), end + 1


def prepare_account(db_prefix: str, account: str, session_key: bytes) -> None:
    """Gives `account` (already created) the session key a client login would leave, a Windows
    client OS, and no IP or country lock."""
    sql = (f"UPDATE {db_prefix}auth.account SET session_key = X'{session_key.hex()}', os = 'Win', "
           f"locked = 0, lock_country = '00' WHERE username = '{account.upper()}'")
    subprocess.run(["mysql", f"--defaults-file={MYSQL_CNF}", "-uroot", "-e", sql], check=True)


class WowClient:
    """One logged-in human. Not thread-safe except for reading the recorded lists under `lock`."""

    def __init__(self, port: int, account: str, session_key: bytes, host: str = "127.0.0.1"):
        self.account = account.upper()
        self._key = session_key
        self._sock = socket.create_connection((host, port), timeout=10)
        self._encrypt: _Arc4 | None = None
        self._decrypt: _Arc4 | None = None
        self._buf = b""
        self.lock = threading.Lock()
        self.chats: list[dict] = []  # {"type", "from_guid", "text", "t"}
        self.group_lists = 0
        self.pings: list[dict] = []  # {"from_guid", "x", "y", "t"}: party pings it saw
        self.guid = 0
        self.lang = LANG_COMMON
        self._reader: threading.Thread | None = None
        self._stop = False

    # ---- framing ----------------------------------------------------------------------------
    def _recv_exact(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("world server closed the connection")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def read_packet(self) -> tuple[int, bytes]:
        head = self._recv_exact(4)
        if self._decrypt:
            head = self._decrypt.process(head)
        if head[0] & 0x80:  # A large packet: 3-byte size.
            extra = self._recv_exact(1)
            if self._decrypt:
                extra = self._decrypt.process(extra)
            head = head + extra
            size = ((head[0] & 0x7F) << 16) | (head[1] << 8) | head[2]
            opcode = struct.unpack_from("<H", head, 3)[0]
        else:
            size = struct.unpack_from(">H", head, 0)[0]
            opcode = struct.unpack_from("<H", head, 2)[0]
        return opcode, self._recv_exact(size - 2)

    def send(self, opcode: int, payload: bytes = b"") -> None:
        head = struct.pack(">H", len(payload) + 4) + struct.pack("<I", opcode)
        if self._encrypt:
            head = self._encrypt.process(head)
        self._sock.sendall(head + payload)

    def _wait_for(self, opcode: int, timeout_s: float = 15.0) -> bytes:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            op, data = self.read_packet()
            if op == opcode:
                return data
            self._handle(op, data)
        raise TimeoutError(f"no opcode 0x{opcode:X} within {timeout_s} s")

    # ---- login ------------------------------------------------------------------------------
    def authenticate(self, realm_id: int = 1) -> None:
        op, data = self.read_packet()
        if op != SMSG_AUTH_CHALLENGE:
            raise ConnectionError(f"expected SMSG_AUTH_CHALLENGE, got 0x{op:X}")
        server_seed = data[4:8]
        client_seed = os.urandom(4)
        digest = hashlib.sha1(self.account.encode() + bytes(4) + client_seed + server_seed +
                              self._key).digest()
        addons = zlib.compress(struct.pack("<II", 0, int(time.time())))
        payload = (struct.pack("<II", BUILD, 0) + self.account.encode() + b"\0" +
                   struct.pack("<I", 0) + client_seed + struct.pack("<IIIQ", 0, 0, realm_id, 3) +
                   digest + struct.pack("<I", 8) + addons)
        self.send(CMSG_AUTH_SESSION, payload)
        self._encrypt = _Arc4(hmac.new(SERVER_DECRYPTION_KEY, self._key, hashlib.sha1).digest())
        self._decrypt = _Arc4(hmac.new(SERVER_ENCRYPTION_KEY, self._key, hashlib.sha1).digest())
        data = self._wait_for(SMSG_AUTH_RESPONSE)
        if data[0] != AUTH_OK:
            raise ConnectionError(f"world auth refused (code 0x{data[0]:X})")

    def characters(self) -> list[tuple[int, str]]:
        self.send(CMSG_CHAR_ENUM)
        data = self._wait_for(SMSG_CHAR_ENUM)
        chars, pos = [], 1
        for _ in range(data[0]):
            guid = struct.unpack_from("<Q", data, pos)[0]
            name, pos = _cstr(data, pos + 8)
            pos += 9 + 4 + 4 + 12 + 4 + 4 + 4 + 1 + 12 + 23 * 9
            chars.append((guid, name))
        return chars

    def create_character(self, name: str, race: int, cls: int) -> None:
        self.send(CMSG_CHAR_CREATE, name.encode() + b"\0" + bytes([race, cls, 0, 0, 0, 0, 0, 0, 0]))
        data = self._wait_for(SMSG_CHAR_CREATE)
        if data[0] != CHAR_CREATE_SUCCESS:
            raise ConnectionError(f"character create refused (code 0x{data[0]:X})")

    def enter_world(self, name: str, race: int, cls: int) -> None:
        """Logs `name` in (creating it first if this account lacks it), then reads in the
        background."""
        self.lang = LANG_COMMON if race in ALLIANCE_RACES else LANG_ORCISH
        chars = dict((n, g) for g, n in self.characters())
        if name not in chars:
            self.create_character(name, race, cls)
            chars = dict((n, g) for g, n in self.characters())
        self.guid = chars[name]
        self.send(CMSG_PLAYER_LOGIN, struct.pack("<Q", self.guid))
        self._wait_for(SMSG_LOGIN_VERIFY_WORLD, 30.0)
        self._sock.settimeout(1.0)
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    # ---- in the world -----------------------------------------------------------------------
    def invite(self, name: str) -> None:
        self.send(CMSG_GROUP_INVITE, name.encode() + b"\0" + struct.pack("<I", 0))

    def say(self, chat_type: int, text: str) -> None:
        self.send(CMSG_MESSAGECHAT, struct.pack("<II", chat_type, self.lang) + text.encode() + b"\0")

    def whisper(self, to: str, text: str) -> None:
        self.send(CMSG_MESSAGECHAT, struct.pack("<II", CHAT_MSG_WHISPER, self.lang) +
                  to.encode() + b"\0" + text.encode() + b"\0")

    def ping(self, x: float, y: float) -> None:
        """Pings the minimap at world coordinates (x, y), as a click on the minimap does: the
        server shows it to the rest of the party."""
        self.send(MSG_MINIMAP_PING, struct.pack("<ff", x, y))

    def close(self) -> None:
        self._stop = True
        try:
            self._sock.close()
        except OSError:
            pass
        if self._reader:
            self._reader.join(timeout=3)

    def _read_loop(self) -> None:
        while not self._stop:
            try:
                op, data = self.read_packet()
            except socket.timeout:
                continue
            except (OSError, ConnectionError, ValueError):
                return
            self._handle(op, data)

    def _handle(self, op: int, data: bytes) -> None:
        if op == SMSG_TIME_SYNC_REQ and len(data) >= 4:
            counter = struct.unpack_from("<I", data)[0]
            self.send(CMSG_TIME_SYNC_RESP, struct.pack("<II", counter, int(time.monotonic() * 1000)
                                                       & 0xFFFFFFFF))
        elif op == MSG_MINIMAP_PING and len(data) >= 16:
            guid, x, y = struct.unpack_from("<Qff", data)
            with self.lock:
                self.pings.append({"from_guid": guid, "x": x, "y": y, "t": time.monotonic()})
        elif op == SMSG_GROUP_LIST:
            with self.lock:
                self.group_lists += 1
        elif op == SMSG_MESSAGECHAT and len(data) >= 17:
            chat_type = data[0]
            sender = struct.unpack_from("<Q", data, 5)[0]
            pos = 17
            try:
                if chat_type in CHAT_TYPE_NAMES:
                    pos += 8  # Receiver guid.
                    size = struct.unpack_from("<I", data, pos)[0]
                    text = data[pos + 4:pos + 4 + size - 1].decode("utf-8", "replace")
                else:
                    return
            except struct.error:
                return
            with self.lock:
                self.chats.append({"type": CHAT_TYPE_NAMES[chat_type], "from_guid": sender,
                                   "text": text, "t": time.monotonic()})
