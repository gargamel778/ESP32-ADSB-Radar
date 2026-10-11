// Breadcrumb for post-mortems: the name of the long-running step each task is in, kept in RTC memory across a watchdog or panic reset.
// At boot the system log says which step was active when the radar last reset abnormally.
#pragma once
#include <Arduino.h>

namespace phase {

inline RTC_NOINIT_ATTR char gData[3][20];   // 0 = data task (core 0), 1 = SD task (core 0), 2 = main loop

inline void set(int who, const char *name) { strlcpy(gData[who], name, sizeof(gData[0])); }
inline const char *get(int who) { return gData[who]; }
inline void clear() { memset(gData, 0, sizeof(gData)); }

// Set just before an intentional restart (app::restart): this chip reports some software restarts as a watchdog reset (reason 7), so the next boot
// uses this mark, not the reset reason, to tell a planned restart from a crash.
constexpr uint32_t kPlanned = 0xA11CE5ED;
inline RTC_NOINIT_ATTR uint32_t gPlanned;
inline bool takePlanned() { const bool p = gPlanned == kPlanned; gPlanned = 0; return p; }

}  // namespace phase
