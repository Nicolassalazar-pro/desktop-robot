"""Static contracts for the hardware-free MG90D microsecond drive path."""

from __future__ import annotations

import importlib.util
import re
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
# The embedded phone UI now lives in its own translation unit. The JS-side
# opcode declarations and the telemetry decode used to live in main.cpp;
# they were moved here without updating this test, which is what left
# test_telemetry_opcode_frame_and_big_endian_decode_match failing silently.
WEB_UI = (ROOT / "src" / "override_html.cpp").read_text(encoding="utf-8")
HARDWARE = (ROOT / "HARDWARE.md").read_text(encoding="utf-8")

_APPLY_SPEC = importlib.util.spec_from_file_location(
    "apply_calibration_contract", ROOT / "tools" / "apply_calibration.py"
)
assert _APPLY_SPEC is not None and _APPLY_SPEC.loader is not None
APPLY = importlib.util.module_from_spec(_APPLY_SPEC)
sys.modules[_APPLY_SPEC.name] = APPLY
_APPLY_SPEC.loader.exec_module(APPLY)
CALIBRATION_HEADER = (ROOT / "src" / "drive_calibration.h").read_text(
    encoding="utf-8"
)
CALIBRATION_SOURCE = (ROOT / "src" / "drive_calibration.cpp").read_text(
    encoding="utf-8"
)
PLATFORMIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")
SERIAL_BUFFER = (ROOT / "src" / "serial_line_buffer.h").read_text(
    encoding="utf-8"
)


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"Unclosed function: {signature}")


def map_throttle(throttle: int, motor: tuple[int, int, int, int, int]) -> int:
    stop, fwd_start, rev_start, fwd_max, rev_max = motor
    if throttle == 0:
        return stop
    start = fwd_start if throttle > 0 else rev_start
    maximum = fwd_max if throttle > 0 else rev_max
    magnitude = abs(throttle)
    scaled = (maximum - start) * (magnitude - 1)
    adjustment = (
        (scaled + 49) // 99 if scaled >= 0 else -((-scaled + 49) // 99)
    )
    return start + adjustment


class MotorMicrosecondContractTests(unittest.TestCase):
    def test_schema_three_keys_and_legacy_slot_erasure_are_locked(self) -> None:
        self.assertRegex(CALIBRATION_SOURCE, r'kSlotAKey\[\]\s*=\s*"us_a"')
        self.assertRegex(CALIBRATION_SOURCE, r'kSlotBKey\[\]\s*=\s*"us_b"')
        self.assertRegex(CALIBRATION_SOURCE, r"kSchema\s*=\s*3\s*;")
        self.assertRegex(CALIBRATION_SOURCE, r'kLegacySlotAKey\[\]\s*=\s*"slot_a"')
        self.assertRegex(CALIBRATION_SOURCE, r'kLegacySlotBKey\[\]\s*=\s*"slot_b"')

        load = function_body(CALIBRATION_SOURCE, "Result Store::load()")
        legacy_erase = load.index("nvs_erase_key(handle, kLegacySlotAKey)")
        schema_read = load.index("readSlot(handle, kSlotAKey")
        self.assertLess(legacy_erase, schema_read)
        self.assertIn("nvs_erase_key(handle, kLegacySlotBKey)", load)
        self.assertIn("keyPresence(handle, kLegacySlotAKey)", load)
        self.assertIn("keyPresence(handle, kLegacySlotBKey)", load)

        erase = function_body(CALIBRATION_SOURCE, "Result Store::eraseAndReset()")
        self.assertRegex(
            erase,
            r"kSlotAKey,\s*kSlotBKey,\s*kLegacySlotAKey,\s*kLegacySlotBKey",
        )

    def test_calibration_layout_bounds_and_direction_rules_are_locked(self) -> None:
        motor = re.search(
            r"struct MotorCalibration\s*\{(?P<body>.*?)\};",
            CALIBRATION_HEADER,
            re.DOTALL,
        )
        self.assertIsNotNone(motor)
        fields = re.findall(r"uint16_t\s+(\w+)\s*;", motor.group("body"))
        self.assertEqual(
            fields,
            ["stopUs", "fwdStartUs", "revStartUs", "fwdMaxUs", "revMaxUs"],
        )

        self.assertRegex(CALIBRATION_SOURCE, r"kPulseMinUs\s*=\s*1000\s*;")
        self.assertRegex(CALIBRATION_SOURCE, r"kPulseMaxUs\s*=\s*2000\s*;")
        self.assertRegex(CALIBRATION_SOURCE, r"kStopMinUs\s*=\s*1450\s*;")
        self.assertRegex(CALIBRATION_SOURCE, r"kStopMaxUs\s*=\s*1550\s*;")

        validate = function_body(
            CALIBRATION_SOURCE, "Result validateMotorImpl("
        )
        self.assertIn("motor.fwdStartUs == motor.stopUs", validate)
        self.assertIn("motor.revStartUs == motor.stopUs", validate)
        self.assertIn("forwardHigh == (motor.revStartUs > motor.stopUs)", validate)
        self.assertIn("quantizedPulseTicks(motor.fwdStartUs)", validate)
        self.assertIn("quantizedPulseTicks(motor.revStartUs)", validate)
        for rule in (
            "motor.fwdMaxUs < motor.fwdStartUs",
            "motor.revMaxUs > motor.revStartUs",
            "motor.fwdMaxUs > motor.fwdStartUs",
            "motor.revMaxUs < motor.revStartUs",
        ):
            self.assertIn(rule, validate)

    def test_legacy_draft_stays_ineligible_and_all_ten_fields_are_required(self) -> None:
        legacy = function_body(CALIBRATION_SOURCE, "Snapshot legacyDraft()")
        self.assertIn("snapshot.source = Source::LegacyDefaults", legacy)
        self.assertIn("snapshot.flags = 0", legacy)
        self.assertIn("snapshot.dirty = true", legacy)
        self.assertIn("snapshot.loadedFromNvs = false", legacy)

        eligible = function_body(CALIBRATION_SOURCE, "bool Store::eligible() const")
        for gate in ("Flag::Confirmed", "Flag::Locked", "Flag::Verified"):
            self.assertIn(gate, eligible)
        self.assertIn("!draft_.dirty", eligible)

        self.assertRegex(
            MAIN, r"kAllCalibrationFieldsTouched\s*=\s*0x03FFU\s*;"
        )
        self.assertIn(
            "side == DriveCalibration::Side::Right ? 5U : 0U", MAIN
        )
        self.assertIn(
            "g_calibrationTouchedMask != kAllCalibrationFieldsTouched", MAIN
        )
        self.assertIn("explicit cal set for all 10 fields", MAIN)
        save = function_body(CALIBRATION_SOURCE, "Result Store::saveConfirmed()")
        self.assertIn("touchedMask_ != kAllFieldsMask", save)
        self.assertIn("all calibration fields required", save)

    def test_ambiguous_generations_and_retained_baseline_fail_closed(self) -> None:
        load = function_body(CALIBRATION_SOURCE, "Result Store::load()")
        self.assertIn("GenerationOrder::Ambiguous", load)
        self.assertIn("ambiguous calibration generation", load)
        self.assertIn("divergent equal-generation calibration", load)
        self.assertIn("memcmp(&slotA.record, &slotB.record", load)
        self.assertIn("hasRetainedBaseline() const", CALIBRATION_SOURCE)
        self.assertIn("retainedGeneration() const", CALIBRATION_SOURCE)
        self.assertIn("retained NVS calibration reloads after reboot", MAIN)
        self.assertIn("all schema-3 and legacy records absent", MAIN)

    def test_telemetry_opcode_frame_and_big_endian_decode_match(self) -> None:
        self.assertRegex(MAIN, r"kTelemetryFrameSize\s*=\s*30\s*;")
        self.assertRegex(MAIN, r"kOpTelemetry\s*=\s*0x12\s*;")

        build = function_body(MAIN, "void orBuildTelemetry(uint8_t* out)")
        self.assertIn("out[0]  = kOpTelemetry", build)
        self.assertRegex(build, r"const uint16_t values\[\]\s*=\s*\{")
        self.assertIn("for (size_t i = 0; i < 10U; ++i)", build)
        self.assertIn("out[10U + i * 2U]", build)
        self.assertIn("static_cast<uint8_t>(values[i] >> 8)", build)
        self.assertIn("out[11U + i * 2U]", build)
        self.assertIn("static_cast<uint8_t>(values[i] & 0xFFU)", build)

        # The JS side of the contract now lives in src/override_html.cpp,
        # not main.cpp. The original test hard-coded MAIN and rotted
        # silently once the HTML was hoisted out (docs/key-issues/TEST.md
        # TEST-3). It is the pairing across the file boundary that
        # matters; who owns which side does not.
        self.assertIn("const OP_CONTROL=0x01,OP_TELEMETRY=0x12", WEB_UI)
        self.assertIn(
            "if(v.byteLength!==30||v.getUint8(0)!==OP_TELEMETRY)return",
            WEB_UI,
        )
        # The old CAL_KEYS assertion was:
        #   assertIn("CAL_KEYS.forEach((k,i)=>$(k).value=v.getUint16(10+i*2))")
        # It has been deliberately removed. The phone-side calibration
        # form was retired from the UI on purpose (calibration is now a
        # separate serial-only path), so the JS no longer decodes bytes
        # 10..29. The firmware still WRITES those ten uint16 fields on
        # every 5 Hz telemetry frame -- 20 wasted bytes per frame,
        # ~100 B/s -- which is a cleanup opportunity in src/main.cpp
        # (out of this test's scope). The wire-format assertion is
        # covered by build[] above, so the C++ half is still pinned.

    def test_wheel_side_to_gpio_mapping_matches_the_verified_wiring(
        self,
    ) -> None:
        # Verified on the assembled chassis 2026-08-03 by pulsing one pin at a
        # time. Swapping these silently inverts differential steering, so the
        # mapping is pinned here rather than left to a comment.
        self.assertRegex(MAIN, r"kServoLeftPin\s*=\s*18\s*;")
        self.assertRegex(MAIN, r"kServoRightPin\s*=\s*17\s*;")
        self.assertEqual(APPLY.SIDE_GPIO, {"left": 18, "right": 17})
        self.assertIn("GPIO18 is the LEFT wheel", HARDWARE)
        self.assertIn("GPIO17", HARDWARE)

    def test_servo_output_is_native_microseconds_and_has_one_adjacent_pair(self) -> None:
        self.assertRegex(MAIN, r"kServoTimerWidthBits\s*=\s*14\s*;")
        attach = function_body(MAIN, "void ensureServoAttached(")
        self.assertLess(
            attach.index("s.attach(pin, kServoMinPulseUs, kServoMaxPulseUs)"),
            attach.index("s.setTimerWidth(kServoTimerWidthBits)"),
        )
        self.assertIn("servoChannelConfigured(s, channel)", attach)
        configured = function_body(
            MAIN, "bool servoChannelConfigured("
        )
        self.assertIn(
            "servo.readTimerWidth() == kServoTimerWidthBits",
            configured,
        )

        writes = list(
            re.finditer(r"g_servo(?:Left|Right)\.writeMicroseconds\([^)]+\);", MAIN)
        )
        self.assertEqual([match.group(0) for match in writes], [
            "g_servoRight.writeMicroseconds(rightPulseUs);",
            "g_servoLeft.writeMicroseconds(leftPulseUs);",
        ])
        between = MAIN[writes[0].end() : writes[1].start()]
        self.assertEqual(between.strip(), "")
        self.assertNotRegex(MAIN, r"writeMicroseconds\s*\(\s*0(?:U|L|UL)?\s*\)")
        self.assertNotRegex(MAIN, r"g_servo(?:Left|Right)\.write\s*\(")
        self.assertNotRegex(MAIN, r"\bX2\b")
        self.assertNotRegex(
            CALIBRATION_HEADER + CALIBRATION_SOURCE,
            r"(?i)\bangle\b",
        )
        self.assertIn("platform              = espressif32@7.0.1", PLATFORMIO)
        self.assertIn(
            "madhephaestus/ESP32Servo@1.2.1", PLATFORMIO
        )
        self.assertIn("nominalReadbackPulseUs", MAIN)
        self.assertNotIn("actualPulseUs", MAIN)

        output = function_body(MAIN, "bool writeDriveOutputLocked(")
        self.assertIn("const bool coldStart", output)
        self.assertEqual(output.count("setBoostEn(false)"), 1)
        self.assertEqual(output.count("setBoostEn(true)"), 1)
        self.assertIn("delay(kServoPwmCommitMs)", output)
        self.assertIn("refreshDriveHardwareLease()", output)
        self.assertIn("servoPwmHardwareMatches", output)

    def test_independent_hardware_lease_cuts_en_without_the_loop(self) -> None:
        timer = function_body(MAIN, "void onDriveHardwareTimer(")
        self.assertIn("g_driveHardwareInhibit.store(true", timer)
        self.assertIn("g_driveHardwareTripPending.store(true", timer)
        self.assertIn("gpio_set_level", timer)
        self.assertIn("g_driveRailOn.store(false", timer)

        boost = function_body(MAIN, "static bool setBoostEn(bool on) {")
        self.assertIn("g_driveHardwareLeaseArmed.load", boost)
        self.assertIn("g_driveHardwareInhibit.load", boost)
        self.assertIn("portENTER_CRITICAL", boost)

        service = function_body(
            MAIN, "static void serviceDriveControl(uint32_t now) {"
        )
        self.assertIn("handleDriveHardwareTrip()", service)
        setup = function_body(MAIN, "void setup()")
        self.assertIn("enableLoopWDT()", setup)

    def test_mapping_endpoints_and_monotonicity_for_both_polarities(self) -> None:
        mapping = function_body(MAIN, "bool mapThrottleToPulseUs(")
        for contract in (
            "if (throttle == 0)",
            "pulseUs = motor.stopUs",
            "forward ? motor.fwdStartUs : motor.revStartUs",
            "forward ? motor.fwdMaxUs : motor.revMaxUs",
            "magnitude - 1U",
            "(scaled + 49) / 99",
            "-(((-scaled) + 49) / 99)",
        ):
            self.assertIn(contract, mapping)

        motors = (
            (1500, 1550, 1450, 2000, 1000),
            (1500, 1450, 1550, 1000, 2000),
        )
        for motor in motors:
            with self.subTest(motor=motor):
                stop, fwd_start, rev_start, fwd_max, rev_max = motor
                self.assertEqual(map_throttle(0, motor), stop)
                self.assertEqual(map_throttle(1, motor), fwd_start)
                self.assertEqual(map_throttle(-1, motor), rev_start)
                self.assertEqual(map_throttle(100, motor), fwd_max)
                self.assertEqual(map_throttle(-100, motor), rev_max)

                forward = [map_throttle(value, motor) for value in range(1, 101)]
                reverse = [
                    map_throttle(-magnitude, motor)
                    for magnitude in range(1, 101)
                ]
                if fwd_max > fwd_start:
                    self.assertEqual(forward, sorted(forward))
                else:
                    self.assertEqual(forward, sorted(forward, reverse=True))
                if rev_max > rev_start:
                    self.assertEqual(reverse, sorted(reverse))
                else:
                    self.assertEqual(reverse, sorted(reverse, reverse=True))
                self.assertTrue(all(1000 <= pulse <= 2000 for pulse in forward))
                self.assertTrue(all(1000 <= pulse <= 2000 for pulse in reverse))

    def test_expo_preserves_nonzero_sign_and_serial_rejects_nul(self) -> None:
        def apply_expo(value: int) -> int:
            if value == 0:
                return 0
            sign = 1 if value > 0 else -1
            magnitude = min(abs(value), 100)
            x = magnitude / 100.0
            y = x * (0.5 * x * x + 0.5)
            return sign * min(int(y * 100.0 + 0.5), 100)

        for value in range(-100, 101):
            with self.subTest(value=value):
                effective = apply_expo(value)
                self.assertEqual(effective == 0, value == 0)
                if value:
                    self.assertEqual(effective > 0, value > 0)

        serial = function_body(MAIN, "void drainSerial()")
        self.assertIn("g_serialInput.push", serial)
        self.assertIn("ERR invalid byte in command", serial)
        self.assertIn("SerialLineEvent::InvalidByte", serial)
        self.assertIn("byte == 0U", SERIAL_BUFFER)
        self.assertIn("byte > 0x7EU", SERIAL_BUFFER)
        self.assertIn("discarding_ = true", SERIAL_BUFFER)

    def test_14_bit_quantization_error_is_bounded(self) -> None:
        def readback(requested: int, bits: int = 14) -> int:
            ticks = int(requested / (20000 / (1 << bits)))
            return int(ticks * (20000 / (1 << bits)))

        errors = [
            requested - readback(requested)
            for requested in range(1000, 2001)
        ]
        self.assertLessEqual(max(errors), 2)
        self.assertEqual(readback(1500), 1499)
        self.assertEqual(readback(1454), readback(1455))


if __name__ == "__main__":
    unittest.main()
