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

3. The binary frame SHAPES agree between firmware and web app. Opcodes only
   name a channel; every actual byte of every frame is also duplicated
   across the wire. The firmware validates control/mood/gesture/sound frame
   lengths with ``if (len != N)`` checks and builds a 30-byte telemetry
   frame at fixed byte offsets; the web app allocates a matching
   ``ArrayBuffer`` for outbound frames and asserts ``v.byteLength !== 30``
   for the inbound telemetry frame, then decodes it at hard-coded offsets.
   Any mismatch here is SILENT -- the phone sends bytes the firmware
   rejects (or worse, the firmware writes bytes the phone decodes as
   garbage), and nothing surfaces the error anywhere.

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

    # ------------------------------------------------------------------
    # Frame-shape drift. Not just "which byte means what channel", but
    # the actual sizes and offsets the two sides encode/decode at. If any
    # of these disagree, the phone sends or reads bytes nobody validates
    # and no error is surfaced anywhere.
    # ------------------------------------------------------------------

    def _cpp_int(self, source: str, pattern: str, name: str) -> int:
        match = re.search(pattern, source)
        self.assertIsNotNone(
            match,
            f"Expected pattern for {name} not found: {pattern}",
        )
        assert match is not None
        return int(match.group(1))

    def test_telemetry_frame_size_agrees_between_firmware_and_web(
        self,
    ) -> None:
        # The C++ side declares the frame size once and everything else
        # (the atomic snapshot buffer, the local stack frame in
        # orBuildTelemetry, the ws send) reads from it. The web app hard-
        # codes 30 in `v.byteLength !== 30`. This test is the only place
        # they meet.
        firmware_size = self._cpp_int(
            MAIN,
            r"constexpr\s+size_t\s+kTelemetryFrameSize\s*=\s*(\d+)\s*;",
            "kTelemetryFrameSize",
        )
        self.assertEqual(
            firmware_size, 30,
            f"kTelemetryFrameSize was 30 bytes by design (opcode + raw mV + "
            f"display mV + pct + mood + charging + eligible + drive state + "
            f"10 uint16 calibration fields). Found {firmware_size}. If this "
            f"is a deliberate change, update the web app's byteLength check "
            f"in src/override_html.cpp and this test in the same commit.",
        )

        # The web app rejects any frame that isn't exactly this size.
        # This regex is deliberately tight -- if the guard is rewritten
        # in a way that still checks 30 but doesn't match, we WANT this
        # test to fail so the maintainer explicitly re-audits both sides.
        js_match = re.search(
            r"v\.byteLength\s*!==\s*(\d+)",
            WEB_UI,
        )
        self.assertIsNotNone(
            js_match,
            "The web app used to reject telemetry frames whose byteLength "
            "did not match the firmware's kTelemetryFrameSize. That check "
            "is gone from src/override_html.cpp. Without it, a stale/short "
            "frame slips into the decoder and getUint16(28) reads past the "
            "end of the DataView.",
        )
        assert js_match is not None
        self.assertEqual(
            int(js_match.group(1)), firmware_size,
            f"src/main.cpp builds a {firmware_size}-byte telemetry frame "
            f"but src/override_html.cpp rejects everything except "
            f"{js_match.group(1)}. The phone will drop every telemetry "
            f"frame silently and the app will sit forever on 'live · "
            f"LOCKED'. Bring both sides back into agreement in the same "
            f"change.",
        )

    def test_telemetry_byte_offsets_written_by_firmware_match_web_decode(
        self,
    ) -> None:
        # The web app decodes the telemetry frame at hard-coded offsets:
        #
        #   getUint8(0)  == OP_TELEMETRY
        #   getUint16(3) == dspMv       (divided by 1000 for display volts)
        #   getUint8(5)  == pct
        #   getUint8(6)  == mood
        #   getUint8(7)  == charging
        #   getUint8(8)  == eligible
        #   getUint8(9)  == drive state
        #
        # The firmware writes at matching offsets in orBuildTelemetry
        # (main.cpp around line 8384). Any disagreement is silent
        # miscoding -- the operator sees the wrong battery percentage,
        # the wrong drive state, or the wrong charging light with no
        # error anywhere.
        #
        # We scrape both sides for these coordinates and pin the mapping.
        # The scrape is by SYMBOL (kOpTelemetry, out[N]) and by SHAPE
        # (v.getUint*(N)), not by line number, because main.cpp is
        # actively edited by another agent.

        # (byte offset, firmware code, web-side decoder call, label)
        expectations = [
            (0,  "out[0]  = kOpTelemetry;",  "getUint8(0)",   "opcode"),
            (5,  "out[5]  = g_batPercent;",   "getUint8(5)",   "battery pct"),
            (6,  "out[6]  = static_cast<uint8_t>(g_eyes.getMood());",
             "getUint8(6)",  "current mood"),
            (7,  "out[7]  = g_batCharging ? 1 : 0;",
             "getUint8(7)",  "charging flag"),
            (8,  "out[8]  = g_driveCalibration.eligible() ? 1U : 0U;",
             "getUint8(8)",  "calibration eligible"),
            (9,  "out[9]  = static_cast<uint8_t>(g_driveSession.state());",
             "getUint8(9)",  "drive session state"),
        ]

        for offset, firmware_assign, web_call, label in expectations:
            with self.subTest(offset=offset, label=label):
                self.assertIn(
                    firmware_assign, MAIN,
                    f"Expected firmware to write telemetry offset {offset} "
                    f"({label}) as `{firmware_assign}`. Not found in "
                    f"src/main.cpp. If the write was refactored, either "
                    f"restore the expected offset or update the web decode "
                    f"and this test.",
                )
                self.assertIn(
                    web_call, WEB_UI,
                    f"Expected web app to decode telemetry offset {offset} "
                    f"({label}) as `{web_call}` in src/override_html.cpp. "
                    f"Not found. If the decode moved, update the firmware "
                    f"write and this test in the same change.",
                )

        # The two-byte fields (bytes 1-2 raw mV, 3-4 dspMv) are big-
        # endian. Pin the shape on both sides.
        for offset, hi_assign, lo_assign, web_call, label in [
            (1, "out[1]  = static_cast<uint8_t>(rawMv >> 8);",
             "out[2]  = static_cast<uint8_t>(rawMv & 0xFF);",
             None, "raw mV"),
            (3, "out[3]  = static_cast<uint8_t>(dspMv >> 8);",
             "out[4]  = static_cast<uint8_t>(dspMv & 0xFF);",
             "getUint16(3)", "display mV"),
        ]:
            with self.subTest(offset=offset, label=label):
                self.assertIn(hi_assign, MAIN)
                self.assertIn(lo_assign, MAIN)
                if web_call is not None:
                    self.assertIn(
                        web_call, WEB_UI,
                        f"Web app must read {label} at offset {offset} as "
                        f"`{web_call}` (big-endian uint16). Not found.",
                    )

        # Calibration fields are ten big-endian uint16s at offsets 10..29.
        # Prove that the loop writes them (structurally) and that offset
        # 29 is the last byte written. If someone stops at 28 or 27, the
        # 30-byte size assertion above still passes but a couple of
        # calibration cells go unwritten.
        loop_pattern = (
            r"out\[10U\s*\+\s*i\s*\*\s*2U\]\s*="
        )
        self.assertRegex(
            MAIN, loop_pattern,
            "Firmware calibration loop that writes offsets 10..29 in the "
            "telemetry frame was removed or refactored. Restore the loop "
            "or update this test AND the (currently absent) web decode of "
            "these bytes together.",
        )
        # Sanity: the loop counter goes to 10U (five per motor × two).
        self.assertIn("for (size_t i = 0; i < 10U; ++i)", MAIN)

    def test_control_frame_length_is_four_on_both_sides(self) -> None:
        # Firmware refuses len != 4 for kOpControl, and the web app
        # allocates a 4-byte ArrayBuffer for sendCtrl. Any drift here is
        # a hard hang: the firmware fails safe (MalformedControl), the
        # phone doesn't notice, and the operator sees the robot refuse
        # to move without a clue why.
        firmware_guard = re.search(
            r"case\s+kOpControl:\s*\{[^}]*?len\s*!=\s*(\d+)U",
            MAIN,
            re.DOTALL,
        )
        self.assertIsNotNone(
            firmware_guard,
            "src/main.cpp's kOpControl handler used to guard `len != 4U`. "
            "That guard is missing. Restore it -- without it, a short "
            "frame reads uninitialised stack as the drive values.",
        )
        assert firmware_guard is not None
        self.assertEqual(
            int(firmware_guard.group(1)), 4,
            f"Firmware rejects control frames of every size except 4 B, "
            f"but this guard now requires {firmware_guard.group(1)} B. "
            f"Bring the web app's ArrayBuffer allocation into agreement.",
        )

        web_alloc = re.search(
            r"function\s+sendCtrl\s*\([^)]*\)\s*\{.*?"
            r"new\s+ArrayBuffer\((\d+)\).*?"
            r"d\.setUint8\(\s*0\s*,\s*OP_CONTROL\s*\)",
            WEB_UI,
            re.DOTALL,
        )
        self.assertIsNotNone(
            web_alloc,
            "src/override_html.cpp's sendCtrl used to allocate an "
            "ArrayBuffer whose size is what the firmware validates and "
            "then write OP_CONTROL into byte 0. That pair is not "
            "detectable in the source -- restore it, or update this "
            "test AND the firmware guard together.",
        )
        assert web_alloc is not None
        self.assertEqual(
            int(web_alloc.group(1)), 4,
            f"sendCtrl allocates a {web_alloc.group(1)}-byte buffer but "
            f"the firmware requires exactly 4. Every control frame will "
            f"be dropped as MalformedControl and the transport will fault.",
        )

    def test_three_byte_channels_agree_on_length(self) -> None:
        # Mood/Gesture/Sound all use the same 3-byte shape: op, id, seq.
        # The firmware validates each with `if (len != 3U)`. The web app
        # sends them via the shared send3 helper.
        for opcode_name in ("kOpMood", "kOpGesture", "kOpSound"):
            with self.subTest(opcode=opcode_name):
                guard = re.search(
                    rf"case\s+{opcode_name}:\s*\{{[^}}]*?len\s*!=\s*(\d+)U",
                    MAIN,
                    re.DOTALL,
                )
                self.assertIsNotNone(
                    guard,
                    f"src/main.cpp's {opcode_name} handler no longer "
                    f"guards its payload length. Without it the handler "
                    f"reads past the end of the ws frame.",
                )
                assert guard is not None
                self.assertEqual(
                    int(guard.group(1)), 3,
                    f"{opcode_name} guard requires "
                    f"{guard.group(1)} B, but the web app's shared "
                    f"send3(op, id) allocates 3 B. Bring both sides "
                    f"into agreement.",
                )

        # The web app's send3 must actually allocate 3 bytes. If someone
        # widens it silently, every mood/gesture/sound frame will fail
        # the firmware guard as ProtocolFailure and the transport will
        # fault-and-revoke.
        web_send3 = re.search(
            r"function\s+send3\(op,\s*id\)\s*\{[^}]*?"
            r"new\s+ArrayBuffer\((\d+)\)",
            WEB_UI,
            re.DOTALL,
        )
        self.assertIsNotNone(
            web_send3,
            "src/override_html.cpp's send3(op, id) helper is missing or "
            "was rewritten in a shape this test cannot recognise. It is "
            "load-bearing -- every mood, gesture, and sound frame ships "
            "through it.",
        )
        assert web_send3 is not None
        self.assertEqual(
            int(web_send3.group(1)), 3,
            f"send3 now allocates {web_send3.group(1)} bytes; firmware "
            f"guards demand exactly 3. Every mood/gesture/sound frame "
            f"will be rejected as ProtocolFailure and the socket will "
            f"be closed by the fault path.",
        )


if __name__ == "__main__":
    unittest.main()
