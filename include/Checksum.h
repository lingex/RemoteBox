#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Computes a CRC32 checksum (uppercase 8-char hex) over a canonical form of the
// document. Object keys are emitted in sorted order and the top-level "chk"
// field is excluded, so the value is stable regardless of member order.
// Returns an empty string if any object exceeds the internal key limit or the
// document nests deeper than the internal depth limit.
String commandChecksum(const JsonDocument &doc);
