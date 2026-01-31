#include "Settings.h"

unsigned int hashrate = 0;
unsigned int hashrate_core_two = 0;
unsigned int difficulty = 0;
unsigned long share_count = 0;
unsigned long accepted_share_count = 0;
String WALLET_ID = "";
String node_id = "";
unsigned int ping = 0;

uint8_t NM_hash_limit_pct_job0 = 100;
uint8_t NM_hash_limit_pct_job1 = 100;

// Alias (job0) for older code paths
uint8_t NM_hash_limit_pct = 100;
