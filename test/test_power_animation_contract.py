"""Static lifecycle contracts for Garage startup/sleep/shutdown animation."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
LOGO = (ROOT / "src" / "mascot" / "garage_logo.h").read_text(
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


class PowerAnimationContractTests(unittest.TestCase):
    def test_original_garage_asset_is_full_screen(self) -> None:
        self.assertRegex(LOGO, r"GARAGE_LOGO_W\s*=\s*240\s*;")
        self.assertRegex(LOGO, r"GARAGE_LOGO_H\s*=\s*280\s*;")
        self.assertIn("GARAGE_LOGO_ALPHA[67200]", LOGO)
        self.assertIn('#include "mascot/garage_logo.h"', MAIN)

    def test_backlight_uses_isolated_ledc_channel(self) -> None:
        self.assertRegex(MAIN, r"kBacklightChannel\s*=\s*4\s*;")
        backlight = function_body(MAIN, "static bool initBacklight() {")
        self.assertIn("ledcSetup", backlight)
        self.assertIn("ledcAttachPin(LCD_BL, kBacklightChannel)", backlight)
        self.assertIn("ledcWrite(kBacklightChannel, 0U)", backlight)
        self.assertRegex(MAIN, r"kLogoTransitionSteps\s*=\s*8\s*;")
        self.assertRegex(
            MAIN, r"kLogoTransitionFrameMs\s*=\s*15\s*;"
        )
        self.assertRegex(MAIN, r"kStartupLogoHoldMs\s*=\s*100\s*;")
        self.assertRegex(MAIN, r"kShutdownLogoHoldMs\s*=\s*100\s*;")
        self.assertRegex(MAIN, r"kBacklightFadeMs\s*=\s*90\s*;")

    def test_startup_is_logo_then_face(self) -> None:
        startup = function_body(
            MAIN, "static void runStartupLogoAnimation() {"
        )
        self.assertLess(
            startup.index("drawGarageLogoToCanvas()"),
            startup.index("blendGarageToTarget(faceTarget)"),
        )
        self.assertIn("fadeBacklightTo(kBacklightMax", startup)
        blend = function_body(MAIN, "static void blendGarageToTarget(")
        self.assertIn("DisplayTransition::startupPixel", blend)
        shutdown_blend = function_body(
            MAIN, "static void blendSourceToGarage("
        )
        self.assertIn("DisplayTransition::shutdownPixel", shutdown_blend)
        setup = function_body(MAIN, "void setup()")
        self.assertLess(
            setup.index("g_eyeEngineReady = true"),
            setup.index("runStartupLogoAnimation()"),
        )

    def test_shutdown_safes_motors_before_visual_and_latch(self) -> None:
        self.assertRegex(MAIN, r"kPowerOffHoldMs\s*=\s*3000\s*;")
        shutdown = function_body(
            MAIN, "[[noreturn]] static void runPowerOffSequence() {"
        )
        motor_off = shutdown.index('powerDownDriveMotors("system shutdown")')
        animation = shutdown.index("runSleepLogoAnimation()")
        latch = shutdown.index("digitalWrite(SYS_EN, LOW)")
        self.assertLess(motor_off, animation)
        self.assertLess(animation, latch)
        self.assertIn("stopWifiServer()", shutdown)
        self.assertIn("ESP.restart()", shutdown)
        self.assertLess(
            shutdown.index("disableLoopWDT()"),
            shutdown.index("while (digitalRead(SYS_OUT) != LOW)"),
        )

    def test_serial_sleep_and_wake_share_animation_language(self) -> None:
        # The `sleep`/`wake` serial commands were extracted into
        # cmdSleep/cmdWake handler functions when dispatchCommand was
        # split into a table (see src/serial_console.h).  The lifecycle
        # contract is unchanged; only the enclosing function-body address
        # moved.
        sleep_body = function_body(MAIN, "void cmdSleep(char* argument)")
        self.assertIn("runSleepLogoAnimation()", sleep_body)
        self.assertIn('resetDriveControl("serial sleep")', sleep_body)
        self.assertLess(
            sleep_body.index("g_paused = true"),
            sleep_body.index('resetDriveControl("serial sleep")'),
        )
        wake_body = function_body(MAIN, "void cmdWake(char* argument)")
        self.assertIn("runWakeLogoAnimation()", wake_body)
        output = function_body(MAIN, "bool writeDriveOutputLocked(")
        self.assertIn("g_systemShuttingDown || g_paused", output)
        service = function_body(
            MAIN, "static void serviceDriveControl(uint32_t now) {"
        )
        self.assertIn(
            "g_orState == ORS_IDLE || g_systemShuttingDown || g_paused",
            service,
        )


if __name__ == "__main__":
    unittest.main()
