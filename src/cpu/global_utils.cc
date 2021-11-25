#include "cpu/global_utils.hh"

utils::CustomConfigs bridge::GCONFIGS;
uint64_t bridge::DEADLOCK_THRESHOLD = 100000; // cycles
uint64_t bridge::TICKS_PER_CYCLE;
bool bridge::CPU_PROMPTED = false;
bool bridge::DECODE_PROMPTED = false;
bool bridge::LSQ_PROMPTED = false;
