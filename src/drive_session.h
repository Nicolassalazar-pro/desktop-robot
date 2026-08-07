#pragma once

#include <stdint.h>

class DriveSession {
 public:
  enum class State : uint8_t {
    SafeOff,
    WaitNeutral,
    ArmedOff,
    Active,
    NeutralHold,
    Manual,
    Shutdown,
  };

  enum class Action : uint8_t {
    None,
    PowerDown,
    Neutral,
    Drive,
  };

  enum class Reason : uint8_t {
    None,
    Connected,
    OwnerBusy,
    Disconnected,
    NonOwner,
    MalformedCommand,
    StaleSequence,
    CalibrationIneligible,
    NeutralProgress,
    NeutralGapRestart,
    NeutralReady,
    NeutralRequired,
    ArmedOffNeutral,
    DriveAccepted,
    NeutralHoldEntered,
    NeutralHeld,
    ControlTimeout,
    ManualEntered,
    AlreadyManual,
    ManualExited,
    ManualNotActive,
    ManualMode,
    ShutdownEntered,
    ShutdownLocked,
    Reset,
  };

  struct Decision {
    Action action;
    int8_t l;
    int8_t r;
    State state;
    Reason reason;
    bool stateChanged;
  };

  static constexpr uint8_t kNeutralFramesRequired = 5;
  static constexpr uint32_t kNeutralFrameMaxGapMs = 90;
  static constexpr uint32_t kControlTimeoutMs = 300;

  // WebSocket control values are signed percentages in the repository's
  // existing protocol. Values outside [-100, 100] are malformed.
  static constexpr int16_t kCommandMin = -100;
  static constexpr int16_t kCommandMax = 100;

  DriveSession();

  Decision connect(uint32_t client, uint32_t now);
  Decision disconnect(uint32_t client);
  Decision frame(uint32_t client, uint8_t seq, int16_t l, int16_t r,
                 uint32_t now, bool calibrationEligible);
  Decision tick(uint32_t now);
  Decision enterManual();
  Decision exitManual();
  Decision shutdown();
  Decision reset();

  State state() const;
  bool hasOwner() const;
  uint32_t owner() const;
  uint32_t generation() const;
  uint32_t clientOwner() const;
  uint32_t sessionGeneration() const;

 private:
  Decision decide(Action action, int8_t l, int8_t r, Reason reason,
                  State previousState) const;
  Decision failSafe(Reason reason, State previousState);
  void clearOwner();
  void clearSequence();
  void resetNeutral();
  void beginOwnedSession(uint32_t client, uint32_t now);
  bool sequenceIsFresh(uint8_t seq) const;
  void acceptSequence(uint8_t seq);
  bool commandIsValid(int16_t value) const;

  State state_;
  bool hasOwner_;
  uint32_t owner_;
  uint32_t generation_;
  bool hasSequence_;
  uint8_t lastSequence_;
  uint8_t neutralFrames_;
  bool hasNeutralTime_;
  uint32_t lastNeutralTime_;
  bool timeoutArmed_;
  uint32_t lastControlTime_;
};
