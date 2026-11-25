# SPDX-License-Identifier: MIT
#
# XTEST extension protocol request builders

import struct
from dataclasses import dataclass

# XTEST minor opcodes
XTestGetVersion = 0
XTestFakeInput = 2

# core event types usable with FakeInput
KeyPress = 2
KeyRelease = 3
ButtonPress = 4
ButtonRelease = 5
MotionNotify = 6


@dataclass
class GetVersionRequest:
    opcode: int
    major: int = 2
    minor: int = 2

    def to_bytes(self, byte_order: str = "<") -> bytes:
        return struct.pack(
            f"{byte_order}BBHBxH", self.opcode, XTestGetVersion, 2, self.major, self.minor
        )


@dataclass
class FakeInputRequest:
    """XTestFakeInput: inject a synthetic core input event."""

    opcode: int
    type: int
    detail: int = 0
    time: int = 0
    root: int = 0
    root_x: int = 0
    root_y: int = 0
    deviceid: int = 0

    def to_bytes(self, byte_order: str = "<") -> bytes:
        # The request is a 36-byte core event layout with the device id last.
        return struct.pack(
            f"{byte_order}BBHBBxxII8xhh7xB",
            self.opcode,
            XTestFakeInput,
            9,
            self.type,
            self.detail,
            self.time,
            self.root,
            self.root_x,
            self.root_y,
            self.deviceid,
        )
