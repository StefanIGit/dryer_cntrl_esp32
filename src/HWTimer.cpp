#include "HWTimer.h"

bool HWTimer::update() {
  if (!running)
    return false;

  uint64_t elapsed = millis() - startTimeMs;
  if (elapsed >= targetMs) {
    running = false;
    elapsedMs = targetMs;
    if (userCallback)
      userCallback();
    return true;
  }

  elapsedMs = elapsed;
  return false;
}

void HWTimer::start(uint8_t hours, TimerCallback cb) {
  stop();
  elapsedMs = 0;
  targetMs = static_cast<uint64_t>(hours) * 60ULL * 60ULL * 1000ULL;
  userCallback = cb;
  startTimeMs = millis();
  running = true;
}

void HWTimer::stop() {
  running = false;
  elapsedMs = 0;
  targetMs = 0;
  startTimeMs = 0;
  userCallback = nullptr;
}

String HWTimer::getTimeLeft() {
  if (!running)
    return String("00:00");

  uint64_t remainingMs = (targetMs > elapsedMs) ? (targetMs - elapsedMs) : 0;
  uint32_t minutesLeft = remainingMs / 60000;
  uint8_t hours = minutesLeft / 60;
  uint8_t minutes = minutesLeft % 60;

  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", hours, minutes);
  return String(buf);
}
