"""Splits a stream of length-prefixed frames (common/net/frame_codec.h) without copying the
whole buffer for every frame: bytes are appended to one buffer, frames are read at an offset,
and the consumed part is dropped once per chunk received.
"""


class FrameReader:
    HEADER = 4  # Big-endian payload length.

    def __init__(self):
        self._buf = bytearray()
        self._pos = 0

    def feed(self, data):
        """Adds received bytes; returns the complete frames now available."""
        self._buf += data
        frames = []
        buf, pos = self._buf, self._pos
        while len(buf) - pos >= self.HEADER:
            size = int.from_bytes(buf[pos:pos + self.HEADER], "big")
            end = pos + self.HEADER + size
            if len(buf) < end:
                break
            frames.append(bytes(buf[pos + self.HEADER:end]))
            pos = end
        if pos:
            del buf[:pos]  # Once per chunk, not once per frame.
        self._pos = 0
        return frames
