// SD card logging: one CSV of aircraft sightings per UTC day and a system log, both in /adsb on the card.
// A background task owns the card. Producers append to RAM buffers (never blocking on the card); the task writes them in batches, reopening the file
// each time so a card pulled at any moment loses at most the last few seconds. A missing or removed card is retried every 10 s and logging resumes by
// itself; the oldest day files are deleted when less than 10 % of the card is free.
#pragma once
#include <Arduino.h>
#include "adsb.h"

namespace sdlog {

enum State : uint8_t { S_OFF, S_NO_CARD, S_OK, S_ERROR, S_EJECTED };

struct Status {
  State state;
  uint64_t totalBytes, freeBytes;   // card capacity and free space (0 when unknown)
  uint32_t rowsToday, bytesToday;   // sightings written to today's file since the radar started
  uint32_t dropped;                 // rows lost because the buffer was full (card absent for long)
  char file[24];                    // today's sightings file name
};

void begin();                                   // start the card task
void sightings(const adsb::Snapshot &s);        // call after every fresh fetch
void sys(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // one line in the system log
Status status();
void eject();                                   // flush and unmount: safe to pull the card out
void ejectAndWait(uint32_t ms);                 // the same, and wait (up to ms) until it is done: used just before a restart
void mount();                                   // after eject()
void simulateRemoval(bool on);                  // dev (serial 'U'): behave as if the card were pulled

// For the log viewer
int listFiles(char names[][24], uint32_t *sizes, int max);               // newest first; returns the count
bool readTail(const char *name, char *buf, size_t cap, size_t &len);     // last bytes of a file (cut at a line start)

}  // namespace sdlog
