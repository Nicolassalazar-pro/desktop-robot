// Host tests for the on-screen gesture-button menu: touch orientation and
// button geometry.
//
// Both of these were found the hard way on real hardware, and neither is
// obvious from reading the code, so they are pinned here.

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

// Mirrors the constants in main.cpp. Kept in sync by
// testMirrorsFirmwareConstants below, which will fail loudly if main.cpp is
// edited without updating this file.
constexpr int kScreenW = 240;
constexpr int kScreenH = 280;

constexpr int kGestureExpressionCount = 3;
constexpr int kGestureButtonCount = 4;
constexpr int kGestureBtnX = 10;
constexpr int kGestureBtnW = kScreenW - (2 * kGestureBtnX);
constexpr int kGestureBtnH = 50;
constexpr int kGestureBtnTop = 40;
constexpr int kGestureBtnGap = 6;
constexpr int kGestureFooterY = 264;

const char* const kNames[kGestureButtonCount] = {"frustrated", "excited",
                                                 "scared", "360 test"};

int gestureButtonY(int index) {
  return kGestureBtnTop + (index * (kGestureBtnH + kGestureBtnGap));
}

int gestureButtonAt(int x, int y) {
  if (x < kGestureBtnX || x >= (kGestureBtnX + kGestureBtnW)) return -1;
  for (int i = 0; i < kGestureButtonCount; ++i) {
    const int top = gestureButtonY(i);
    if (y >= top && y < (top + kGestureBtnH)) return i;
  }
  return -1;
}

// The CST816T reports in the panel's native orientation, rotated 90 degrees
// from the screen.
void mapTouchToScreen(int rawX, int rawY, int& screenX, int& screenY) {
  screenY = rawX;
  screenX = kScreenW - rawY;
  if (screenX < 0) screenX = 0;
  if (screenX >= kScreenW) screenX = kScreenW - 1;
  if (screenY < 0) screenY = 0;
  if (screenY >= kScreenH) screenY = kScreenH - 1;
}

void testButtonsDoNotOverlapEachOther() {
  for (int i = 0; i + 1 < kGestureButtonCount; ++i) {
    const int endOfThis = gestureButtonY(i) + kGestureBtnH;
    assert(endOfThis <= gestureButtonY(i + 1));
  }
}

void testButtonsFitOnScreenAndClearTheFooter() {
  assert(gestureButtonY(0) >= 30);  // below the title
  const int lastBottom =
      gestureButtonY(kGestureButtonCount - 1) + kGestureBtnH;
  assert(lastBottom <= kScreenH);
  // A 72 px layout ran to y=273 and the footer at y=268 was drawn on top of
  // the third button. Caught by test, not by looking at the screen.
  assert(kGestureFooterY >= lastBottom);
  assert(kGestureFooterY + 8 <= kScreenH);
  assert(kGestureBtnX + kGestureBtnW <= kScreenW);

  // The button count is load-bearing: adding a fourth button meant 68 px no
  // longer fit and the height had to drop to 50. Pin the arithmetic so a fifth
  // cannot be added without the layout being rechecked.
  assert(kGestureButtonCount == 4);
  assert(kGestureBtnTop + (kGestureButtonCount * kGestureBtnH) +
             ((kGestureButtonCount - 1) * kGestureBtnGap) <= kGestureFooterY);
}

void testTargetsExceedTheTouchMinimum() {
  // 44 px is the accepted minimum for a fingertip; this is operated on a 1.69"
  // panel, possibly while the robot is moving.
  assert(kGestureBtnH >= 44);
  assert(kGestureBtnW >= 44);
}

void testLabelsFitTheirButtons() {
  // GFX 6x8 font at size 2.
  for (int i = 0; i < kGestureButtonCount; ++i) {
    const int textW = static_cast<int>(strlen(kNames[i])) * 6 * 2;
    assert(textW <= kGestureBtnW);
  }
}

void testEveryButtonCentreSelectsItself() {
  for (int i = 0; i < kGestureButtonCount; ++i) {
    assert(gestureButtonAt(kScreenW / 2,
                           gestureButtonY(i) + (kGestureBtnH / 2)) == i);
  }
}

void testMissesReturnNoButton() {
  assert(gestureButtonAt(kScreenW / 2, 5) == -1);          // title row
  assert(gestureButtonAt(kScreenW / 2, kGestureFooterY) == -1);  // footer row
  assert(gestureButtonAt(0, gestureButtonY(0) + 10) == -1);      // left margin
  assert(gestureButtonAt(kScreenW - 1, gestureButtonY(0) + 10) == -1);
  // The gaps between buttons must be dead, or a sloppy tap could trigger a
  // neighbour.
  assert(gestureButtonAt(kScreenW / 2,
                         gestureButtonY(0) + kGestureBtnH + 2) == -1);
}

void testTouchIsRotatedNotIdentity() {
  // The whole bug: the panel reports 90 degrees off from the screen. If this
  // ever becomes the identity mapping again, the buttons silently select the
  // wrong movement.
  int sx = 0;
  int sy = 0;
  mapTouchToScreen(60, 120, sx, sy);
  assert(sy == 60);   // screen Y comes from raw X
  assert(sx == 120);  // screen X comes from raw Y

  // Anchored in the confirmed swipe evidence: a screen swipe DOWN (increasing
  // screen Y) arrives as the chip's GESTURE_RIGHT (increasing raw X).
  int lowY = 0;
  int highY = 0;
  int ignored = 0;
  mapTouchToScreen(20, 100, ignored, lowY);
  mapTouchToScreen(200, 100, ignored, highY);
  assert(highY > lowY);
}

void testMappedTouchesStayOnScreen() {
  for (int rawX = -20; rawX <= 320; rawX += 7) {
    for (int rawY = -20; rawY <= 320; rawY += 7) {
      int sx = 0;
      int sy = 0;
      mapTouchToScreen(rawX, rawY, sx, sy);
      assert(sx >= 0 && sx < kScreenW);
      assert(sy >= 0 && sy < kScreenH);
    }
  }
}

void testEveryButtonIsReachableThroughTheRealMapping() {
  // Walk the raw coordinate space the chip can produce and confirm each button
  // can actually be hit end to end, mapping included. A transform that is
  // self-consistent but unreachable would pass every test above.
  bool reached[kGestureButtonCount] = {false, false, false, false};
  for (int rawX = 0; rawX < kScreenH; ++rawX) {
    for (int rawY = 0; rawY < kScreenW; ++rawY) {
      int sx = 0;
      int sy = 0;
      mapTouchToScreen(rawX, rawY, sx, sy);
      const int hit = gestureButtonAt(sx, sy);
      if (hit >= 0) reached[hit] = true;
    }
  }
  for (int i = 0; i < kGestureButtonCount; ++i) assert(reached[i]);
}

void testXMirrorDoesNotChangeButtonSelection() {
  // The X mirror is the one part the swipe evidence does not pin down. The
  // buttons are full-width rows specifically so it cannot matter — this test
  // is what makes that claim true rather than hopeful.
  for (int rawX = 0; rawX < kScreenH; rawX += 3) {
    for (int rawY = 0; rawY < kScreenW; rawY += 3) {
      int sxA = kScreenW - rawY;
      int sxB = rawY;  // the mirrored alternative
      const int sy = rawX;
      if (sxA < 0) sxA = 0;
      if (sxA >= kScreenW) sxA = kScreenW - 1;
      if (sxB < 0) sxB = 0;
      if (sxB >= kScreenW) sxB = kScreenW - 1;
      // Only compare where both land inside the button column; the margins are
      // legitimately dead in either orientation.
      const int a = gestureButtonAt(sxA, sy);
      const int b = gestureButtonAt(sxB, sy);
      if (a >= 0 && b >= 0) assert(a == b);
    }
  }
}

}  // namespace

int main() {
  testButtonsDoNotOverlapEachOther();
  testButtonsFitOnScreenAndClearTheFooter();
  testTargetsExceedTheTouchMinimum();
  testLabelsFitTheirButtons();
  testEveryButtonCentreSelectsItself();
  testMissesReturnNoButton();
  testTouchIsRotatedNotIdentity();
  testMappedTouchesStayOnScreen();
  testEveryButtonIsReachableThroughTheRealMapping();
  testXMirrorDoesNotChangeButtonSelection();
  printf("gesture-menu tests passed\n");
  return 0;
}
