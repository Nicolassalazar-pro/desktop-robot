#include "drive_session.h"

constexpr uint8_t DriveSession::kNeutralFramesRequired;
constexpr uint32_t DriveSession::kNeutralFrameMaxGapMs;
constexpr uint32_t DriveSession::kControlTimeoutMs;
constexpr int16_t DriveSession::kCommandMin;
constexpr int16_t DriveSession::kCommandMax;

DriveSession::DriveSession()
    : state_(State::SafeOff),
      hasOwner_(false),
      owner_(0),
      generation_(0),
      hasSequence_(false),
      lastSequence_(0),
      neutralFrames_(0),
      hasNeutralTime_(false),
      lastNeutralTime_(0),
      timeoutArmed_(false),
      lastControlTime_(0) {}

DriveSession::Decision DriveSession::connect(uint32_t client, uint32_t now) {
  const State previousState = state_;
  if (state_ == State::Shutdown) {
    return decide(Action::PowerDown, 0, 0, Reason::ShutdownLocked,
                  previousState);
  }
  if (state_ == State::Manual) {
    return decide(Action::PowerDown, 0, 0, Reason::ManualMode, previousState);
  }
  if (hasOwner_ && owner_ != client) {
    state_ = State::WaitNeutral;
    resetNeutral();
    return decide(Action::PowerDown, 0, 0, Reason::OwnerBusy, previousState);
  }

  beginOwnedSession(client, now);
  return decide(Action::PowerDown, 0, 0, Reason::Connected, previousState);
}

DriveSession::Decision DriveSession::disconnect(uint32_t client) {
  const State previousState = state_;
  if (state_ == State::Shutdown) {
    return decide(Action::PowerDown, 0, 0, Reason::ShutdownLocked,
                  previousState);
  }
  if (!hasOwner_ || owner_ != client) {
    if (hasOwner_ && state_ != State::Manual) {
      state_ = State::WaitNeutral;
      resetNeutral();
    }
    return decide(Action::PowerDown, 0, 0, Reason::NonOwner, previousState);
  }

  clearOwner();
  state_ = State::SafeOff;
  return decide(Action::PowerDown, 0, 0, Reason::Disconnected, previousState);
}

DriveSession::Decision DriveSession::frame(
    uint32_t client, uint8_t seq, int16_t l, int16_t r, uint32_t now,
    bool calibrationEligible) {
  const State previousState = state_;
  if (state_ == State::Shutdown) {
    return decide(Action::PowerDown, 0, 0, Reason::ShutdownLocked,
                  previousState);
  }
  if (state_ == State::Manual) {
    return decide(Action::PowerDown, 0, 0, Reason::ManualMode, previousState);
  }
  if (!hasOwner_ || owner_ != client) {
    return failSafe(Reason::NonOwner, previousState);
  }
  if (!commandIsValid(l) || !commandIsValid(r)) {
    return failSafe(Reason::MalformedCommand, previousState);
  }
  if (!sequenceIsFresh(seq)) {
    return failSafe(Reason::StaleSequence, previousState);
  }
  if (!calibrationEligible) {
    return failSafe(Reason::CalibrationIneligible, previousState);
  }

  acceptSequence(seq);
  timeoutArmed_ = true;
  lastControlTime_ = now;

  const bool neutral = (l == 0) && (r == 0);
  if (state_ == State::WaitNeutral || state_ == State::SafeOff) {
    state_ = State::WaitNeutral;
    if (!neutral) {
      resetNeutral();
      return decide(Action::PowerDown, 0, 0, Reason::NeutralRequired,
                    previousState);
    }

    const bool gapTooLarge =
        hasNeutralTime_ &&
        static_cast<uint32_t>(now - lastNeutralTime_) >
            kNeutralFrameMaxGapMs;
    neutralFrames_ =
        gapTooLarge ? 1 : static_cast<uint8_t>(neutralFrames_ + 1);
    hasNeutralTime_ = true;
    lastNeutralTime_ = now;

    if (neutralFrames_ >= kNeutralFramesRequired) {
      state_ = State::ArmedOff;
      return decide(Action::PowerDown, 0, 0, Reason::NeutralReady,
                    previousState);
    }
    return decide(Action::PowerDown, 0, 0,
                  gapTooLarge ? Reason::NeutralGapRestart
                              : Reason::NeutralProgress,
                  previousState);
  }

  resetNeutral();
  if (state_ == State::ArmedOff) {
    if (neutral) {
      return decide(Action::PowerDown, 0, 0, Reason::ArmedOffNeutral,
                    previousState);
    }
    state_ = State::Active;
    return decide(Action::Drive, static_cast<int8_t>(l),
                  static_cast<int8_t>(r), Reason::DriveAccepted,
                  previousState);
  }

  if (state_ == State::Active) {
    if (neutral) {
      state_ = State::NeutralHold;
      return decide(Action::Neutral, 0, 0, Reason::NeutralHoldEntered,
                    previousState);
    }
    return decide(Action::Drive, static_cast<int8_t>(l),
                  static_cast<int8_t>(r), Reason::DriveAccepted,
                  previousState);
  }

  if (state_ == State::NeutralHold) {
    if (neutral) {
      return decide(Action::Neutral, 0, 0, Reason::NeutralHeld,
                    previousState);
    }
    state_ = State::Active;
    return decide(Action::Drive, static_cast<int8_t>(l),
                  static_cast<int8_t>(r), Reason::DriveAccepted,
                  previousState);
  }

  return failSafe(Reason::NeutralRequired, previousState);
}

DriveSession::Decision DriveSession::tick(uint32_t now) {
  const State previousState = state_;
  if (state_ == State::Shutdown) {
    return decide(Action::PowerDown, 0, 0, Reason::ShutdownLocked,
                  previousState);
  }
  if (!hasOwner_ || state_ == State::Manual || !timeoutArmed_) {
    return decide(Action::None, 0, 0, Reason::None, previousState);
  }
  if (static_cast<uint32_t>(now - lastControlTime_) < kControlTimeoutMs) {
    return decide(Action::None, 0, 0, Reason::None, previousState);
  }

  state_ = State::WaitNeutral;
  timeoutArmed_ = false;
  resetNeutral();
  return decide(Action::PowerDown, 0, 0, Reason::ControlTimeout,
                previousState);
}

DriveSession::Decision DriveSession::enterManual() {
  const State previousState = state_;
  if (state_ == State::Shutdown) {
    return decide(Action::PowerDown, 0, 0, Reason::ShutdownLocked,
                  previousState);
  }
  if (state_ == State::Manual) {
    return decide(Action::PowerDown, 0, 0, Reason::AlreadyManual,
                  previousState);
  }

  clearOwner();
  state_ = State::Manual;
  return decide(Action::PowerDown, 0, 0, Reason::ManualEntered, previousState);
}

DriveSession::Decision DriveSession::exitManual() {
  const State previousState = state_;
  if (state_ == State::Shutdown) {
    return decide(Action::PowerDown, 0, 0, Reason::ShutdownLocked,
                  previousState);
  }

  clearOwner();
  state_ = State::SafeOff;
  return decide(Action::PowerDown, 0, 0,
                previousState == State::Manual ? Reason::ManualExited
                                               : Reason::ManualNotActive,
                previousState);
}

DriveSession::Decision DriveSession::shutdown() {
  const State previousState = state_;
  clearOwner();
  state_ = State::Shutdown;
  return decide(Action::PowerDown, 0, 0,
                previousState == State::Shutdown ? Reason::ShutdownLocked
                                                 : Reason::ShutdownEntered,
                previousState);
}

DriveSession::Decision DriveSession::reset() {
  const State previousState = state_;
  state_ = State::SafeOff;
  hasOwner_ = false;
  owner_ = 0;
  generation_ = 0;
  clearSequence();
  resetNeutral();
  timeoutArmed_ = false;
  lastControlTime_ = 0;
  return decide(Action::PowerDown, 0, 0, Reason::Reset, previousState);
}

DriveSession::State DriveSession::state() const {
  return state_;
}

bool DriveSession::hasOwner() const {
  return hasOwner_;
}

uint32_t DriveSession::owner() const {
  return owner_;
}

uint32_t DriveSession::generation() const {
  return generation_;
}

uint32_t DriveSession::clientOwner() const {
  return owner_;
}

uint32_t DriveSession::sessionGeneration() const {
  return generation_;
}

DriveSession::Decision DriveSession::decide(Action action, int8_t l, int8_t r,
                                            Reason reason,
                                            State previousState) const {
  const Decision decision = {action, l, r, state_, reason,
                             state_ != previousState};
  return decision;
}

DriveSession::Decision DriveSession::failSafe(Reason reason,
                                              State previousState) {
  if (hasOwner_) {
    state_ = State::WaitNeutral;
  } else {
    state_ = State::SafeOff;
  }
  resetNeutral();
  return decide(Action::PowerDown, 0, 0, reason, previousState);
}

void DriveSession::clearOwner() {
  hasOwner_ = false;
  owner_ = 0;
  clearSequence();
  resetNeutral();
  timeoutArmed_ = false;
  lastControlTime_ = 0;
}

void DriveSession::clearSequence() {
  hasSequence_ = false;
  lastSequence_ = 0;
}

void DriveSession::resetNeutral() {
  neutralFrames_ = 0;
  hasNeutralTime_ = false;
  lastNeutralTime_ = 0;
}

void DriveSession::beginOwnedSession(uint32_t client, uint32_t now) {
  hasOwner_ = true;
  owner_ = client;
  ++generation_;
  clearSequence();
  resetNeutral();
  timeoutArmed_ = true;
  lastControlTime_ = now;
  state_ = State::WaitNeutral;
}

bool DriveSession::sequenceIsFresh(uint8_t seq) const {
  if (!hasSequence_) {
    return true;
  }
  const uint8_t delta = static_cast<uint8_t>(seq - lastSequence_);
  return delta >= 1 && delta <= 127;
}

void DriveSession::acceptSequence(uint8_t seq) {
  hasSequence_ = true;
  lastSequence_ = seq;
}

bool DriveSession::commandIsValid(int16_t value) const {
  return value >= kCommandMin && value <= kCommandMax;
}
