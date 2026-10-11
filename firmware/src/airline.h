// Airline names from callsigns: the first three letters of a callsign such as "ELY382" are the airline's ICAO designator.
// Table built by tools/build_airlines.py from OpenFlights airlines.dat (Open Database License, openflights.org).
#pragma once
#include <Arduino.h>

namespace airline {

const char *forCallsign(const char *callsign);   // "El Al" for "ELY382"; nullptr for registrations, military calls and unknown airlines

}  // namespace airline
