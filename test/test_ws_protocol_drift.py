"""Drift checks for load-bearing WebSocket protocol constants.

Two sources of drift are covered here:

1. The web app opcodes are duplicated on both sides of the wire. The C++
   robot uses ``kOpControl`` .. ``kOpSound`` in ``src/main.cpp``; the
   embedded phone UI declares ``OP_CONTROL`` .. ``OP_SOUND`` in
   ``src/override_html.cpp``. If these drift, the phone silently sends bytes
   the robot ignores, with no error surfaced anywhere -- the exact silent
   failure mode this test exists to prevent.

2. Three timeout constants (``kWsOwnerSilenceTimeoutMs``,
   ``kIngressDrainTimeoutMs``, ``kControlTimeoutMs``) drive the transport
   ownership state machine. They have no header pin and no other test. This
   file locks their documented values so a bare-number tweak in the source
   surfaces here.

Failure messages are written to tell the maintainer what to do next.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
WEB_UI = (ROOT / "src" / "override_html.cpp").read_text(encoding="utf-8")
DRIVE_SESSION_H = (ROOT / "src" / "drive_session.h").read_text(encoding="utf-8")


_CPP_OPCODE_RE = re.compile(
    r"constexpr\s+uint8_t\s+(kOp\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;"
)
_JS_OPCODE_LINE_RE = re.compile(
    r"const\s+(OP_[A-Z_]+=0x[0-9a-fA-F]+(?:\s*,\s*OP_[A-Z_]+=0x[0-9a-fA-F]+)*)\s*;"
)
_JS_OPCODE_PAIR_RE = re.compile(r"(OP_[A-Z_]+)=(0x[0-9a-fA-F]+)")


# Mapping between the C++ symbol and its JS counterpart. The pairing is what
# is contractually stable; the underlying numeric values are the free
# variable this test polices.
CPP_TO_JS = {
    "kOpControl": "OP_CONTROL",
    "kOpTelemetry": "OP_TELEMETRY",
    "kOpTrim": "OP_TRIM",
    "kOpMood": "OP_MOOD",
    "kOpGesture": "OP_GESTURE",
    "kOpSound": "OP_SOUND",
}


def _parse_cpp_opcodes(source: str) -> dict[str, int]:
    return {
        match.group(1): int(match.group(2), 16)
        for match in _CPP_OPCODE_RE.finditer(source)
    }


def _parse_js_opcodes(source: str) -> dict[str, int]:
    line_match = _JS_OPCODE_LINE_RE.search(source)
    if line_match is None:
        return {}
    return {
        pair.group(1): int(pair.group(2), 16)
        for pair in _JS_OPCODE_PAIR_RE.finditer(line_match.group(1))
    }


class WsProtocolDriftTests(unittest.TestCase):
    def test_cpp_and_js_opcodes_agree(self) -> None:
        cpp = _parse_cpp_opcodes(MAIN)
        js = _parse_js_opcodes(WEB_UI)

        # Sanity: both sides declare exactly the set the app protocol
        # promises. A missing declaration is itself drift -- silence a
        # channel and the phone will send bytes for a feature the robot
        # will never see.
        self.assertEqual(
            set(cpp), set(CPP_TO_JS),
            "src/main.cpp opcode set drifted from CPP_TO_JS mapping in this "
            "test. Update CPP_TO_JS if the protocol legitimately added or "
            "removed a channel.",
        )
        self.assertEqual(
            set(js), set(CPP_TO_JS.values()),
            "src/override_html.cpp opcode set drifted from CPP_TO_JS "
            "mapping. Update the JS declaration or update this test.",
        )

        for cpp_name, js_name in CPP_TO_JS.items():
            with self.subTest(cpp=cpp_name, js=js_name):
                self.assertEqual(
                    cpp[cpp_name], js[js_name],
                    f"{cpp_name} is {cpp[cpp_name]:#04x} in "
                    f"src/main.cpp but {js_name} is {js[js_name]:#04x} in "
                    f"src/override_html.cpp. The phone will send bytes the "
                    f"robot silently ignores, or vice versa. Bring both "
                    f"sides back into agreement in the same change.",
                )

    def test_documented_opcode_values_are_locked(self) -> None:
        # The wire protocol has a documented shape (see the comment block
        # around src/main.cpp line 5321 and docs). Renumbering a channel
        # breaks every deployed phone tab, so pin the actual numbers, not
        # just "they agree".
        expected = {
            "kOpControl":   0x01,
            "kOpTelemetry": 0x12,
            "kOpTrim":      0x03,
            "kOpMood":      0x04,
            "kOpGesture":   0x05,
            "kOpSound":     0x06,
        }
        cpp = _parse_cpp_opcodes(MAIN)
        for name, value in expected.items():
            with self.subTest(name=name):
                self.assertEqual(
                    cpp.get(name), value,
                    f"{name} must be {value:#04x} on the wire "
                    f"(binary protocol §4 Phase 1). Found "
                    f"{cpp.get(name)!r} in src/main.cpp.",
                )

    def test_ws_owner_silence_timeout_matches_ws_reaper_test(self) -> None:
        # test/ws_owner_reaper_test.cpp mirrors this constant so its host
        # test can compile without pulling in Arduino headers. If the
        # firmware value drifts, the host test starts asserting the wrong
        # policy in silence. The mirror is only OK if it matches.
        match = re.search(
            r"constexpr\s+uint32_t\s+kWsOwnerSilenceTimeoutMs\s*=\s*"
            r"(\d+)\s*;",
            MAIN,
        )
        self.assertIsNotNone(
            match,
            "kWsOwnerSilenceTimeoutMs disappeared from src/main.cpp. If "
            "the reaper policy is being retired, delete "
            "test/ws_owner_reaper_test.cpp in the same change.",
        )
        assert match is not None
        firmware_value = int(match.group(1))

        reaper_test = (
            ROOT / "test" / "ws_owner_reaper_test.cpp"
        ).read_text(encoding="utf-8")
        mirror = re.search(
            r"constexpr\s+uint32_t\s+kWsOwnerSilenceTimeoutMs\s*=\s*"
            r"(\d+)\s*;",
            reaper_test,
        )
        self.assertIsNotNone(
            mirror,
            "The host reaper test lost its mirror of "
            "kWsOwnerSilenceTimeoutMs. Restore it and re-run.",
        )
        assert mirror is not None
        self.assertEqual(
            int(mirror.group(1)), firmware_value,
            f"kWsOwnerSilenceTimeoutMs is {firmware_value} in src/main.cpp "
            f"but {mirror.group(1)} in test/ws_owner_reaper_test.cpp. "
            f"Update the mirror.",
        )

        # Also pin the documented value so a bare-number retune is noticed.
        self.assertEqual(
            firmware_value, 8000,
            "kWsOwnerSilenceTimeoutMs was 8000 ms by design (generous "
            "enough to survive a brief radio stall, short enough that the "
            "QR screen comes back while the user is still standing there). "
            "If you're changing it deliberately, update this test and the "
            "comment in src/main.cpp in the same commit.",
        )

    def test_ingress_drain_timeout_matches_documented_value(self) -> None:
        match = re.search(
            r"constexpr\s+uint32_t\s+kIngressDrainTimeoutMs\s*=\s*"
            r"(\d+)\s*;",
            MAIN,
        )
        self.assertIsNotNone(
            match,
            "kIngressDrainTimeoutMs disappeared from src/main.cpp.",
        )
        assert match is not None
        # Documented: comfortably longer than any real callback, and well
        # under the 5 s task watchdog it exists to avoid tripping.
        self.assertEqual(
            int(match.group(1)), 750,
            f"kIngressDrainTimeoutMs was 750 ms by design (well under the "
            f"5 s task watchdog). Found {match.group(1)}. If this is a "
            f"deliberate change, update the comment and this test.",
        )

    def test_control_timeout_matches_drive_session_header(self) -> None:
        match = re.search(
            r"static\s+constexpr\s+uint32_t\s+kControlTimeoutMs\s*=\s*"
            r"(\d+)\s*;",
            DRIVE_SESSION_H,
        )
        self.assertIsNotNone(
            match,
            "DriveSession::kControlTimeoutMs disappeared from "
            "src/drive_session.h.",
        )
        assert match is not None
        self.assertEqual(
            int(match.group(1)), 300,
            f"DriveSession::kControlTimeoutMs was 300 ms by design "
            f"(matches movement_expression.h refresh contract). Found "
            f"{match.group(1)}. If deliberate, update the comment and "
            f"this test.",
        )


if __name__ == "__main__":
    unittest.main()
