#ifndef HWTIMER_H
#define HWTIMER_H

#include <Arduino.h>

typedef void (*TimerCallback)();

class HWTimer {
  public:
    void start(uint8_t hours, TimerCallback cb);
    void stop();
    bool update();
    String getTimeLeft();

  private:
    static constexpr uint64_t tickMs = 60000; // 1 minute
    TimerCallback userCallback = nullptr;
    volatile bool running = false;
    uint64_t elapsedMs = 0;
    uint64_t targetMs = 0;
    uint64_t startTimeMs = 0;
};

#endif
