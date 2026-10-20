/*
  =====================================================================
  event_log.h - In-DRAM circular event log for the motor monitor
  =====================================================================
  Stores the most recent system events (boot, Wi-Fi, soft-start, relay
  energize, trips, resets, wireless commands) in a fixed-size ring
  buffer living in DRAM. No filesystem is used (PROGMEM-only firmware).

  - NEWEST event index: count()-1   (oldest event index: 0)
  - Each event carries a monotonically increasing sequence number,
    the uptime in milliseconds at which it happened, a type code, and
    a short printf-formatted description string.
  - Oldest events are silently overwritten when the buffer is full
    (the newest history is the most useful for the dashboard and the
    CSV print report).
  =====================================================================
*/

#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <Arduino.h>
#include <stdarg.h>

#define EVENT_LOG_CAPACITY 48
#define EVENT_TEXT_LEN     48

enum LogType : uint8_t {
  EV_NONE = 0,
  EV_BOOT,          // boot / firmware revision
  EV_WIFI_OK,       // STA link established
  EV_WIFI_FAIL,     // STA link failed -> AP fallback engaged
  EV_AP_ON,         // softAP started
  EV_MOTOR_START,   // soft-start sequence initiated (button or web)
  EV_MOTOR_ON,      // relay energized, motor running
  EV_MOTOR_OFF,     // motor stopped via button or web
  EV_TRIP,          // fault trip, motor latched OFF
  EV_RESET,         // fault latch cleared
  EV_INFO           // misc
};

struct LogEvent {
  uint32_t    uptime;                   // ms since boot when it happened
  uint16_t    seq;                      // monotonic sequence number
  uint8_t     type;                     // LogType
  char        text[EVENT_TEXT_LEN];     // short human-readable description
};

class EventLog {
public:
  void begin() {
    head = 0;
    size_ = 0;
    seq = 0;
  }

  // printf-style append. Overwrites oldest when full. Safe against
  // malformed format/argument mixes (vsnprintf clamps output).
  void add(uint8_t type, const char* fmt, ...) {
    LogEvent& e = buf[head];
    e.type = type;
    e.uptime = millis();

    va_list args;
    va_start(args, fmt);
    vsnprintf(e.text, EVENT_TEXT_LEN, fmt ? fmt : "(no text)", args);
    va_end(args);
    e.text[EVENT_TEXT_LEN - 1] = '\0';   // vsnprintf already clamps, this is belt & braces

    e.seq = ++seq;

    head = (head + 1) % EVENT_LOG_CAPACITY;
    if (size_ < EVENT_LOG_CAPACITY) size_++;
  }

  uint8_t count() const { return size_; }

  // Index 0 = oldest retained event, index count()-1 = newest.
  const LogEvent* at(uint8_t idx) const {
    if (idx >= size_) return nullptr;
    return &buf[(head + idx) % EVENT_LOG_CAPACITY];
  }

  void clear() { begin(); }

private:
  LogEvent buf[EVENT_LOG_CAPACITY];     // zero-initialized by static storage
  uint8_t head = 0;
  uint8_t size_ = 0;
  uint16_t seq = 0;
};

EventLog eventLog;

// Single-label mapper shared by /api/status, /api/log (CSV) and the
// printable HTML report, so every surface uses identical type names.
inline const char* logTypeLabel(uint8_t t) {
  switch (t) {
    case EV_BOOT:        return "BOOT";
    case EV_WIFI_OK:     return "WIFI";
    case EV_WIFI_FAIL:   return "WIFI";
    case EV_AP_ON:       return "AP";
    case EV_MOTOR_START: return "START";
    case EV_MOTOR_ON:    return "ON";
    case EV_MOTOR_OFF:   return "OFF";
    case EV_TRIP:        return "TRIP";
    case EV_RESET:       return "RESET";
    default:             return "INFO";
  }
}

// "H:MM:SS" uptime string for CSV/report tables (hours grow past 99).
inline String uptimeToString(uint32_t ms) {
  uint32_t s = ms / 1000UL;
  uint32_t h = s / 3600UL;
  uint8_t m = (s % 3600UL) / 60UL;
  uint8_t sec = s % 60UL;
  char b[16];
  snprintf(b, sizeof(b), "%lu:%02u:%02u", (unsigned long)h, m, sec);
  return String(b);
}

#endif // EVENT_LOG_H
