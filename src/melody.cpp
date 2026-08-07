#include "melody.h"

namespace Melody {

bool validate(const Note* notes, uint8_t count) {
  if (notes == nullptr || count == 0) return false;
  for (uint8_t i = 0; i < count; ++i) {
    if (notes[i].ms == 0) return false;
    if (notes[i].hz == 0) continue;   // rest
    if (notes[i].hz < kMinHz || notes[i].hz > kMaxHz) return false;
  }
  return true;
}

Player::Player()
    : notes_(nullptr),
      count_(0),
      index_(0),
      noteStartedMs_(0),
      active_(false),
      name_("") {}

bool Player::start(const Note* notes, uint8_t count, uint32_t nowMs) {
  if (!validate(notes, count)) return false;
  notes_ = notes;
  count_ = count;
  index_ = 0;
  noteStartedMs_ = nowMs;
  active_ = true;
  return true;
}

void Player::stop() {
  active_ = false;
  notes_ = nullptr;
  count_ = 0;
  index_ = 0;
}

uint16_t Player::service(uint32_t nowMs) {
  if (!active_ || notes_ == nullptr) return 0;

  // Advance through however many notes have elapsed. A loop rather than a
  // single step, because a stalled tick must not stretch the melody — it
  // should skip ahead and stay in time with the movement it accompanies.
  while (active_) {
    const uint32_t elapsed = nowMs - noteStartedMs_;   // unsigned: wrap is safe
    if (elapsed < notes_[index_].ms) break;
    noteStartedMs_ += notes_[index_].ms;
    ++index_;
    if (index_ >= count_) {
      stop();
      return 0;
    }
  }
  if (!active_) return 0;
  return notes_[index_].hz;
}

bool Player::active() const { return active_; }
uint8_t Player::noteIndex() const { return index_; }
const char* Player::name() const { return name_; }
void Player::setName(const char* name) { name_ = name == nullptr ? "" : name; }

// ---------------------------------------------------------------------------
// The six fixed voice tables (kFrustrated/kExcited/kScared/kChirp/kQuery/
// kAlarm) and the voiceForName / voiceCount / voiceNameAt helpers used to
// live here. They were removed when the seeded mood-melody generator
// (melody_gen.{h,cpp}) took over: generated phrases give the robot a much
// larger and non-repeating expressive vocabulary in the same three
// documented moods (happy/sad/confused).
// ---------------------------------------------------------------------------

}  // namespace Melody
