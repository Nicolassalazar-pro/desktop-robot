"""Tests for deriving drive calibration from a measured dead band."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = (
    Path(__file__).resolve().parents[1] / "tools" / "apply_calibration.py"
)
SPEC = importlib.util.spec_from_file_location("apply_calibration", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
APPLY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = APPLY
SPEC.loader.exec_module(APPLY)

LOW = APPLY.DEFAULT_DEADZONE_LOW_US
HIGH = APPLY.DEFAULT_DEADZONE_HIGH_US


class DeriveTests(unittest.TestCase):
    def test_measured_band_produces_the_expected_ten_values(self) -> None:
        stop = APPLY.derive_stop(LOW, HIGH)
        self.assertEqual((LOW, HIGH, stop), (1423, 1533, 1478))
        plan = APPLY.build_plan(LOW, HIGH, stop, True, False)
        self.assertEqual(
            plan["left"], APPLY.MotorCalibration(1478, 1534, 1422, 2000, 1000)
        )
        self.assertEqual(
            plan["right"], APPLY.MotorCalibration(1478, 1422, 1534, 1000, 2000)
        )

    def test_stop_sits_inside_the_band_and_the_firmware_stop_window(
        self,
    ) -> None:
        stop = APPLY.derive_stop(LOW, HIGH)
        self.assertTrue(LOW <= stop <= HIGH)
        self.assertTrue(APPLY.STOP_MIN_US <= stop <= APPLY.STOP_MAX_US)
        # Equal margin to both edges is what keeps a batch-mate motor quiet.
        self.assertEqual(stop - LOW, HIGH - stop)

    def test_every_polarity_pairing_validates(self) -> None:
        stop = APPLY.derive_stop(LOW, HIGH)
        for left_high in (True, False):
            for right_high in (True, False):
                with self.subTest(left=left_high, right=right_high):
                    plan = APPLY.build_plan(
                        LOW, HIGH, stop, left_high, right_high
                    )
                    for side in APPLY.SIDES:
                        self.assertIsNone(APPLY.validate_motor(plan[side]))
                        self.assertEqual(
                            APPLY.check_deadzone_escape(
                                plan[side], LOW, HIGH
                            ),
                            [],
                        )

    def test_starts_escape_the_band_in_tick_space(self) -> None:
        stop = APPLY.derive_stop(LOW, HIGH)
        plan = APPLY.build_plan(LOW, HIGH, stop, True, False)
        low_ticks = APPLY.quantized_ticks(LOW)
        high_ticks = APPLY.quantized_ticks(HIGH)
        motor = plan["left"]
        self.assertTrue(
            low_ticks <= APPLY.quantized_ticks(motor.stop) <= high_ticks
        )
        self.assertGreater(APPLY.quantized_ticks(motor.fwdstart), high_ticks)
        self.assertLess(APPLY.quantized_ticks(motor.revstart), low_ticks)

    def test_a_start_that_quantizes_back_into_the_band_is_flagged(
        self,
    ) -> None:
        # 1532 and 1533 are one microsecond apart but share tick 1255, so a
        # band ending at 1532 derives a forward start the shaft cannot see.
        self.assertEqual(
            APPLY.quantized_ticks(1532), APPLY.quantized_ticks(1533)
        )
        motor = APPLY.derive_motor(1423, 1532, 1478, True)
        self.assertEqual(motor.fwdstart, 1533)
        warnings = APPLY.check_deadzone_escape(motor, 1423, 1532)
        self.assertEqual(len(warnings), 1)
        self.assertIn("fwdstart", warnings[0])
        self.assertIn("cannot move the shaft", warnings[0])

    def test_the_measured_band_edge_is_a_real_tick_boundary(self) -> None:
        # 1533 -> no motion and 1534 -> motion is only meaningful because the
        # two requests land on different ticks.
        self.assertNotEqual(
            APPLY.quantized_ticks(HIGH), APPLY.quantized_ticks(HIGH + 1)
        )
        self.assertNotEqual(
            APPLY.quantized_ticks(LOW), APPLY.quantized_ticks(LOW - 1)
        )

    def test_stop_outside_the_band_is_flagged_as_creep(self) -> None:
        warnings = APPLY.check_deadzone_escape(
            APPLY.MotorCalibration(1550, 1560, 1422, 2000, 1000), LOW, HIGH
        )
        self.assertTrue(any("creep" in warning for warning in warnings))

    def test_validation_matches_firmware_rejections(self) -> None:
        cases = {
            "stop must be 1450..1550us": APPLY.MotorCalibration(
                1440, 1600, 1400, 2000, 1000
            ),
            "starts must be on opposite sides of stop": APPLY.MotorCalibration(
                1478, 1534, 1600, 2000, 2000
            ),
            "forward max must be at or beyond forward start":
                APPLY.MotorCalibration(1478, 1534, 1422, 1500, 1000),
            "pulse must be 1000..2000us": APPLY.MotorCalibration(
                1478, 2100, 1422, 2000, 1000
            ),
        }
        for expected, motor in cases.items():
            with self.subTest(expected=expected):
                self.assertEqual(APPLY.validate_motor(motor), expected)

    def test_quantization_matches_the_firmware_formula(self) -> None:
        self.assertEqual(APPLY.quantized_ticks(1478), 1210)
        self.assertEqual(APPLY.quantized_ticks(1534), 1256)
        self.assertEqual(APPLY.quantized_ticks(1422), 1164)
        self.assertAlmostEqual(APPLY.emitted_us(1478), 1477.05, places=2)


class CommandPlanTests(unittest.TestCase):
    def test_plan_emits_all_ten_cal_set_commands(self) -> None:
        stop = APPLY.derive_stop(LOW, HIGH)
        plan = APPLY.build_plan(LOW, HIGH, stop, True, False)
        commands = APPLY.plan_commands(plan)
        self.assertEqual(len(commands), 10)
        self.assertEqual(commands[0], "cal set left stop 1478")
        self.assertEqual(commands[9], "cal set right revmax 2000")
        for side in APPLY.SIDES:
            for field in APPLY.FIELD_ORDER:
                self.assertTrue(
                    any(cmd.startswith(f"cal set {side} {field} ")
                        for cmd in commands),
                    f"missing {side} {field}",
                )

    def test_plan_never_stages_a_save(self) -> None:
        plan = APPLY.build_plan(LOW, HIGH, 1478, True, False)
        for command in APPLY.plan_commands(plan):
            self.assertNotIn("save", command)
            self.assertNotIn("confirm", command)

    def test_dry_run_does_not_touch_a_port(self) -> None:
        self.assertEqual(APPLY.main(["--dry-run"]), 0)

    def test_inverted_band_is_rejected(self) -> None:
        self.assertEqual(
            APPLY.main(["--dry-run", "--deadzone-low", "1533",
                        "--deadzone-high", "1423"]),
            2,
        )


class PortIdentityTests(unittest.TestCase):
    def test_production_is_the_default_target(self) -> None:
        ports = [
            APPLY.PortIdentity("COM3", None),
            APPLY.PortIdentity("COM4", "A0:F2:62:E4:C3:44"),
            APPLY.PortIdentity("COM9", "DC:B4:D9:23:22:A0"),
        ]
        self.assertEqual(APPLY.select_port(ports), "COM4")
        self.assertEqual(APPLY.select_port(ports, target="testrig"), "COM9")

    def test_unknown_boards_and_com3_are_refused(self) -> None:
        with self.assertRaises(RuntimeError):
            APPLY.select_port(
                [APPLY.PortIdentity("COM12", "0123456789AB")]
            )
        with self.assertRaises(RuntimeError):
            APPLY.select_port(
                [APPLY.PortIdentity("COM4", "A0:F2:62:E4:C3:44")], "COM3"
            )
        with self.assertRaises(RuntimeError):
            APPLY.select_port(
                [APPLY.PortIdentity("COM9", "DC:B4:D9:23:22:A0")],
                target="production",
            )


if __name__ == "__main__":
    unittest.main()
