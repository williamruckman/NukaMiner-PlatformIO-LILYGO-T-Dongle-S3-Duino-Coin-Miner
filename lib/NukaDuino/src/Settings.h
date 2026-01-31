#pragma once
// Minimal settings/globals required by MiningJob.h (adapted for NukaMiner)

#include <Arduino.h>

#define SOFTWARE_VERSION "4.3-nukaminer"

// Enable some diagnostics (optional)
#define SERIAL_PRINTING
// #define LED_BLINKING   // We'll manage LED ourselves in app if desired

// MiningJob expects these blink presets in some upstream variants.
// Provide safe defaults.
#ifndef BLINK_CLIENT_CONNECT
#define BLINK_CLIENT_CONNECT 2
#endif

// Globals used by MiningJob (declared extern here, defined in Settings.cpp)
extern unsigned int hashrate;
extern unsigned int hashrate_core_two;
extern unsigned int difficulty;
extern unsigned long share_count;
extern unsigned long accepted_share_count;
extern String WALLET_ID;
extern String node_id;
extern unsigned int ping;

// Hashrate limiter (0-100). 100 = unlimited.
// job0 = primary miner (always available)
// job1 = optional second miner task ("Core 2" in UI)
extern uint8_t NM_hash_limit_pct_job0;
extern uint8_t NM_hash_limit_pct_job1;

// Backwards compatibility: older code uses NM_hash_limit_pct (maps to job0).
extern uint8_t NM_hash_limit_pct;

// NukaMiner log hook (implemented in src/main.cpp). This allows the miner
// library to mirror Serial output into the Web UI live console.
void NM_log(const String &line);
