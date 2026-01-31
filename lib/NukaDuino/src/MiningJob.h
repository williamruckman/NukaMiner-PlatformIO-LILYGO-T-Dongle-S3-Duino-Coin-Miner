#pragma GCC optimize("-Ofast")

#ifndef MINING_JOB_H
#define MINING_JOB_H

#include <Arduino.h>
#include <assert.h>
#include <string.h>
#include <Ticker.h>
#include <WiFiClient.h>

#include "DSHA1.h"
#include "Counter.h"
#include "Settings.h"

// https://github.com/esp8266/Arduino/blob/master/cores/esp8266/TypeConversion.cpp
const char base36Chars[36] PROGMEM = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z'
};

const uint8_t base36CharValues[75] PROGMEM{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0,                                                                        // 0 to 9
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 0, 0, 0, 0, 0, 0, // Upper case letters
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35                    // Lower case letters
};

#define SPC_TOKEN ' '
#define END_TOKEN '\n'
#define SEP_TOKEN ','
#define IOT_TOKEN '@'

struct MiningConfig {
    String host = "";
    int port = 0;
    String DUCO_USER = "";
    String RIG_IDENTIFIER = "";
    String MINER_KEY = "";
    String GROUP_ID = "";
    String MINER_VER = SOFTWARE_VERSION;
    #if defined(ESP8266)
        // "High-band" 8266 diff
        String START_DIFF = "ESP8266H";
    #elif defined(CONFIG_FREERTOS_UNICORE)
        // Single core 32 diff
        String START_DIFF = "ESP32S";
    #else
        // Normal 32 diff
        String START_DIFF = "ESP32";
    #endif

    MiningConfig(String DUCO_USER, String RIG_IDENTIFIER, String MINER_KEY, String GROUP_ID = "")
            : DUCO_USER(DUCO_USER), RIG_IDENTIFIER(RIG_IDENTIFIER), MINER_KEY(MINER_KEY), GROUP_ID(GROUP_ID) {}
};

class MiningJob {

public:
    MiningConfig *config;
    int core = 0;

    MiningJob(int core, MiningConfig *config) {
        this->core = core;
        this->config = config;
        this->client_buffer = "";
        dsha1 = new DSHA1();
        dsha1->warmup();
        generateRigIdentifier();
    }

    void blink(uint8_t count, uint8_t pin = LED_BUILTIN) {
        #if defined(LED_BLINKING)
            uint8_t state = HIGH;

            for (int x = 0; x < (count << 1); ++x) {
                digitalWrite(pin, state ^= HIGH);
                delay(50);
            }
        #else
            digitalWrite(LED_BUILTIN, HIGH);
        #endif
    }

    // NOTE: Per-instance stopwatch (NOT static). A static stopwatch would be
    // shared between cores/instances and can dramatically increase how often
    // we yield/delay, reducing hashrate.
    bool max_micros_elapsed(unsigned long current, unsigned long max_elapsed) {
        // max_elapsed==0 is used as a "reset" by upstream code
        if (max_elapsed == 0) {
            _micros_start = current;
            return true;
        }

        if ((current - _micros_start) > max_elapsed) {
            _micros_start = current;
            return true;
        }
        return false;
    }

    void handleSystemEvents(void) {
        #if defined(ESP32) && CORE == 2
          esp_task_wdt_reset();
        #endif
        // Keep this extremely light — calling this often directly impacts
        // hashrate. Yield without sleeping a full RTOS tick.
        delay(0);
        yield();
        // OTA is not used by NukaMiner; keep upstream call disabled to avoid
        // requiring ArduinoOTA.
    }

    // Mine a single share cycle.
    // Returns true if a share was accepted ("GOOD"), false on failure
    // (connect/job failures or rejected share).
    bool mine() {
        if (!connectToNode()) return false;
        if (!askForJob()) return false;

        dsha1->reset().write((const unsigned char *)getLastBlockHash().c_str(), getLastBlockHash().length());

        const uint32_t start_time = micros();
        max_micros_elapsed(start_time, 0);

        #if defined(LED_BLINKING)
            #if defined(BLUSHYBOX)
              for (int i = 0; i < 72; i++) {
                analogWrite(LED_BUILTIN, i);
                delay(1);
              }
            #else
              digitalWrite(LED_BUILTIN, LOW);
            #endif
        #endif

        bool accepted = false;

        uint32_t limiterIter = 0;
        for (Counter<10> counter; counter < difficulty; ++counter, ++limiterIter) {
            DSHA1 ctx = *dsha1;
            ctx.write((const unsigned char *)counter.c_str(), counter.strlen()).finalize(hashArray);

            // Micro-yield: give lower-priority system work a chance even at 100%.
            // Keep overhead tiny by only yielding every 512 iterations.
            if ((limiterIter & 0x1FFu) == 0u) {
                yield();
            }

            // Hard idle guarantee on CPU0 (core==0) to prevent IDLE0 task watchdog resets
            // when both miners run at 100%. We only do this on the CPU0 miner so Core2 speed
            // remains essentially unaffected.
            if (core == 0) {
                // Guarantee the CPU0 IDLE task runs often enough to satisfy the task watchdog.
                // This keeps WiFi/Web responsive and prevents IDLE0 WDT resets at full load.
                const uint32_t nowMs = millis();
                if (_idleKickMs == 0 || (uint32_t)(nowMs - _idleKickMs) >= 15) {
                    _idleKickMs = nowMs;
                    delay(1); // yield one RTOS tick so IDLE0 can run
                }
            }

#ifndef CONFIG_FREERTOS_UNICORE

                #if defined(ESP32)
                    // Yielding too frequently hurts hashrate. 25ms keeps WiFi/RTOS happy
                    // without taking a big bite out of the inner hash loop.
                    #define SYSTEM_TIMEOUT 250000 // 25ms for ESP32
                #else
                    #define SYSTEM_TIMEOUT 500000 // 50ms for 8266
                #endif
                if (max_micros_elapsed(micros(), SYSTEM_TIMEOUT)) {
                    handleSystemEvents();
                }
            #endif

            if (memcmp(getExpectedHash(), hashArray, 20) == 0) {
                const uint32_t elapsed_time = micros() - start_time;
                const float elapsed_time_s = elapsed_time * .000001f;
                share_count++;

                #if defined(LED_BLINKING)
                    #if defined(BLUSHYBOX)
                        for (int i = 72; i > 0; i--) {
                          analogWrite(LED_BUILTIN, i);
                          delay(1);
                        }
                    #else
                        digitalWrite(LED_BUILTIN, HIGH);
                    #endif
                #endif

                if (String(core) == "0") {
                    hashrate = counter / elapsed_time_s;
                    submit(counter, hashrate, elapsed_time_s);
                } else {
                    hashrate_core_two = counter / elapsed_time_s;
                    submit(counter, hashrate_core_two, elapsed_time_s);
                }

                accepted = (client_buffer == "GOOD");

                #if defined(BLUSHYBOX)
                    gauge_set(hashrate + hashrate_core_two);
                #endif

                break;
            }
        }

        return accepted;
    }


private:
    String client_buffer;
    uint8_t hashArray[20];
    String last_block_hash;
    String expected_hash_str;
    uint8_t expected_hash[20];
    DSHA1 *dsha1;
    uint32_t _micros_start = 0;
    uint32_t _limitWindowStartMs = 0;
    uint32_t _idleKickMs = 0;
    WiFiClient client;
    String chipID = "";

    #if defined(ESP8266)
        #if defined(BLUSHYBOX)
          String MINER_BANNER = "Official BlushyBox Miner (ESP8266)";
        #else
          String MINER_BANNER = "Official ESP8266 Miner";
        #endif
    #elif defined(CONFIG_FREERTOS_UNICORE)
        String MINER_BANNER = "Official ESP32-S2 Miner";
    #else
        #if defined(BLUSHYBOX)
          String MINER_BANNER = "Official BlushyBox Miner (ESP32)";
        #else
          String MINER_BANNER = "Official ESP32 Miner";
        #endif
    #endif

    // Converts a hex string into a byte array.
    // IMPORTANT: Duino-Coin nodes can occasionally return partial lines if the
    // connection is interrupted or read timeouts occur. The upstream miner used
    // an assert() here, but that causes reboot loops on ESP32 when a truncated
    // job line is received. We validate instead and let the caller retry.
    uint8_t *hexStringToUint8Array(const String &hexString, uint8_t *uint8Array, const uint32_t arrayLength) {
        // Nodes can occasionally return partial lines; avoid assert/reboot loops.
        if (hexString.length() < arrayLength * 2) {
            return nullptr;
        }
        const char *hexChars = hexString.c_str();
        for (uint32_t i = 0; i < arrayLength; ++i) {
            uint8Array[i] = (pgm_read_byte(base36CharValues + hexChars[i * 2] - '0') << 4) +
                            pgm_read_byte(base36CharValues + hexChars[i * 2 + 1] - '0');
        }
        return uint8Array;
    }

    void generateRigIdentifier() {
        String AutoRigName = "";

        #if defined(ESP8266)
            chipID = String(ESP.getChipId(), HEX);

            if (strcmp(config->RIG_IDENTIFIER.c_str(), "Auto") != 0)
                return;

            AutoRigName = "ESP8266-" + chipID;
            AutoRigName.toUpperCase();
            config->RIG_IDENTIFIER = AutoRigName.c_str();
        #else
            uint64_t chip_id = ESP.getEfuseMac();
            uint16_t chip = (uint16_t)(chip_id >> 32); // Prepare to print a 64 bit value into a char array
            char fullChip[23];
            snprintf(fullChip, 23, "%04X%08X", chip,
                    (uint32_t)chip_id); // Store the (actually) 48 bit chip_id into a char array

            chipID = String(fullChip);

            if (strcmp(config->RIG_IDENTIFIER.c_str(), "Auto") != 0)
                return;
            // Autogenerate ID if required
            AutoRigName = "ESP32-" + String(fullChip);
            AutoRigName.toUpperCase();
            config->RIG_IDENTIFIER = AutoRigName.c_str();
        #endif 
        #if defined(SERIAL_PRINTING)
          NM_log("Core [" + String(core) + "] - Rig identifier: "
                          + config->RIG_IDENTIFIER);
        #endif
    }

    bool connectToNode() {
        if (client.connected()) return true;

        // Make stream reads less prone to returning partial lines.
        client.setTimeout(15000);

        uint32_t stopWatch = millis();
        #if defined(SERIAL_PRINTING)
          NM_log("Core [" + String(core) + "] - Connecting to a Duino-Coin node...");
        #endif

        int attempts = 0;
        while (!client.connect(config->host.c_str(), config->port)) {
            attempts++;
            if (max_micros_elapsed(micros(), 100000)) {
                handleSystemEvents();
            }
            if (attempts >= 3 || (millis() - stopWatch) > 15000) {
                #if defined(SERIAL_PRINTING)
                  NM_log("Core [" + String(core) + "] - Failed to connect to node (timeout)");
                #endif
                client.stop();
                return false;
            }
            delay(250);
        }

        // Reduce latency for small request/response packets (helps dashboard ping).
        client.setNoDelay(true);

        // Wait for server greeting/version
        if (!waitForClientData()) {
            client.stop();
            return false;
        }

        #if defined(SERIAL_PRINTING)
          NM_log("Core [" + String(core) + "] - Connected. Node reported version: "
                          + client_buffer);
        #endif

        blink(BLINK_CLIENT_CONNECT);

        return true;
    }

    void submit(unsigned long counter, float hashrate, float elapsed_time_s) {
        // Duino-Coin PC miners can "group" multiple workers (threads) into a single
        // dashboard entry by appending a shared group-id to the share submission line.
        // When GROUP_ID is set and shared across workers, the dashboard shows one miner
        // with N threads instead of N separate miners.
        String submitLine = String(counter) +
                     SEP_TOKEN + String(hashrate) +
                     SEP_TOKEN + MINER_BANNER +
                     SPC_TOKEN + config->MINER_VER +
                     SEP_TOKEN + config->RIG_IDENTIFIER +
                     // Field after identifier: device ID (optional but used by many miners)
                     SEP_TOKEN + "DUCOID" + String(chipID) +
                     // Last field: group-id used by the Duino-Coin dashboard to collapse
                     // multiple workers into a single "threads" entry (PC miner behavior).
                     SEP_TOKEN + config->GROUP_ID +
                     END_TOKEN;
        client.print(submitLine);

        unsigned long ping_start = millis();
        waitForClientData();
        ping = millis() - ping_start;

        if (client_buffer == "GOOD") {
          accepted_share_count++;
        }

        #if defined(SERIAL_PRINTING)
          NM_log("Core [" + String(core) + "] - " +
                          client_buffer +
                          " share #" + String(share_count) +
                          " (" + String(counter) + ")" +
                          " hashrate: " + String(hashrate / 1000, 2) + " kH/s (" +
                          String(elapsed_time_s) + "s) " + 
                          "Ping: " + String(ping) + "ms " +
                          "(" + node_id + ")\n");
        #endif
    }

    bool parse() {
        // Create a non-constant copy of the input string
        char *job_str_copy = strdup(client_buffer.c_str());

        if (job_str_copy) {
            String tokens[3];
            char *token = strtok(job_str_copy, ",");
            for (int i = 0; token != NULL && i < 3; i++) {
                tokens[i] = token;
                token = strtok(NULL, ",");
            }

            // Ensure we actually got all 3 tokens
            if (tokens[0].length() == 0 || tokens[1].length() == 0 || tokens[2].length() == 0) {
                free(job_str_copy);
                return false;
            }

            // Nodes may include a trailing '\r'
            tokens[0].trim();
            tokens[1].trim();
            tokens[2].trim();

            last_block_hash = tokens[0];
            expected_hash_str = tokens[1];
            // Expected hash is 20 bytes => 40 hex chars
            if (hexStringToUint8Array(expected_hash_str, expected_hash, 20) == nullptr) {
                free(job_str_copy);
                return false;
            }

            int diff = tokens[2].toInt();
            if (diff <= 0) {
                free(job_str_copy);
                return false;
            }
            difficulty = diff * 100 + 1;

            // Free the memory allocated by strdup
            free(job_str_copy);

            return true;
        }
        else {
            // Handle memory allocation failure
            return false;
        }
    }

    bool askForJob() {
        if (!client.connected()) return false;

        NM_log("Core [" + String(core) + "] - Asking for a new job for user: " 
                        + String(config->DUCO_USER));

        #if defined(USE_DS18B20)
            sensors.requestTemperatures(); 
            float temp = sensors.getTempCByIndex(0);
            #if defined(SERIAL_PRINTING)
              NM_log("DS18B20 reading: " + String(temp) + "°C");
            #endif
        
            client.print("JOB," +
                         String(config->DUCO_USER) +
                         SEP_TOKEN + config->START_DIFF + 
                         SEP_TOKEN + String(config->MINER_KEY) + 
                         SEP_TOKEN + "Temp:" + String(temp) + "*C" +
                         END_TOKEN);
        #elif defined(USE_DHT)
            float temp = dht.readTemperature();
            float hum = dht.readHumidity();
            #if defined(SERIAL_PRINTING)
              NM_log("DHT reading: " + String(temp) + "°C");
              NM_log("DHT reading: " + String(hum) + "%");
            #endif

            client.print("JOB," +
                         String(config->DUCO_USER) +
                         SEP_TOKEN + config->START_DIFF + 
                         SEP_TOKEN + String(config->MINER_KEY) + 
                         SEP_TOKEN + "Temp:" + String(temp) + "*C" +
                         IOT_TOKEN + "Hum:" + String(hum) + "%" +
                         END_TOKEN);
        #elif defined(USE_HSU07M)
            float temp = read_hsu07m();
            #if defined(SERIAL_PRINTING)
              NM_log("HSU reading: " + String(temp) + "°C");
            #endif

            client.print("JOB," +
                         String(config->DUCO_USER) +
                         SEP_TOKEN + config->START_DIFF + 
                         SEP_TOKEN + String(config->MINER_KEY) + 
                         SEP_TOKEN + "Temp:" + String(temp) + "*C" +
                         END_TOKEN);
        #elif defined(USE_INTERNAL_SENSOR)
            float temp = 0;
            temp_sensor_read_celsius(&temp);
            #if defined(SERIAL_PRINTING)
              NM_log("Internal temp sensor reading: " + String(temp) + "°C");
            #endif

            client.print("JOB," +
                         String(config->DUCO_USER) +
                         SEP_TOKEN + config->START_DIFF + 
                         SEP_TOKEN + String(config->MINER_KEY) + 
                         SEP_TOKEN + "CPU Temp:" + String(temp) + "*C" +
                         END_TOKEN);
        #else
            client.print("JOB," +
                         String(config->DUCO_USER) +
                         SEP_TOKEN + config->START_DIFF + 
                         SEP_TOKEN + String(config->MINER_KEY) + 
                         END_TOKEN);
        #endif

        if (!waitForClientData()) return false;
        #if defined(SERIAL_PRINTING)
          NM_log("Core [" + String(core) + "] - Received job with size of "
                          + String(client_buffer.length()) 
                          + " bytes " + client_buffer);
        #endif

        if (!parse()) {
            #if defined(SERIAL_PRINTING)
              NM_log("Core [" + String(core) + "] - Invalid/truncated job received, retrying...");
            #endif
            return false;
        }
        #if defined(SERIAL_PRINTING)
          NM_log("Core [" + String(core) + "] - Parsed job: " 
                          + getLastBlockHash() + " " 
                          + getExpectedHashStr() + " " 
                          + String(getDifficulty()));
        #endif
    
        return true;
    }

    // Returns true if a full line was read, false on timeout/disconnect.
    bool waitForClientData() {
        client_buffer = "";
        const uint32_t stopWatch = millis();
        while (client.connected()) {
            if (client.available()) {
                client_buffer = client.readStringUntil(END_TOKEN);
                if (client_buffer.length() == 1 && client_buffer[0] == END_TOKEN)
                    client_buffer = "???\n"; // Should never happen
                return true;
            }
            if (max_micros_elapsed(micros(), 100000)) {
                handleSystemEvents();
            }
            if ((millis() - stopWatch) > 15000) {
                return false;
            }
        }
        return false;
    }

    const String &getLastBlockHash() const { return last_block_hash; }
    const String &getExpectedHashStr() const { return expected_hash_str; }
    const uint8_t *getExpectedHash() const { return expected_hash; }
    unsigned int getDifficulty() const { return difficulty; }
};

#endif
