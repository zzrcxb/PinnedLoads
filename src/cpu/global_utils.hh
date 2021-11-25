#ifndef GLOBAL_UTILS_HH
#define GLOBAL_UTILS_HH

#include <iostream>
#include <cstdint>
#include <string>
#include <vector>

#include "debug/Tracer.hh"

#define IN_SET(ELEM, SET) (SET.find(ELEM) != SET.end())
#define IN_MAP(KEY, MAP) (MAP.find(KEY) != MAP.end())

#define IN_SETPTR(ELEM, SET) (SET->find(ELEM) != SET->end())
#define IN_MAPPTR(KEY, MAP) (MAP->find(KEY) != MAP->end())

#define DICT_GET(ELEM, DICT, DEFT) (IN_MAP(ELEM, DICT) ? DICT[ELEM] : DEFT)
#define DICTPTR_GET(ELEM, DICT, DEFT) (IN_MAPPTR(ELEM, DICT) ? (*DICT)[ELEM] : DEFT)

#define CHECK_SEQNUM (((bridge::GCONFIGS.hasLowerBound &&                                   \
                        bridge::GCONFIGS.youngestSeqNum >= bridge::GCONFIGS.lowerSeqNum) || \
                       !bridge::GCONFIGS.hasLowerBound) &&                                  \
                      ((bridge::GCONFIGS.hasUpperBound &&                                   \
                        bridge::GCONFIGS.youngestSeqNum <= bridge::GCONFIGS.upperSeqNum) || \
                       !bridge::GCONFIGS.hasUpperBound))

#define DSTATE(STATE, INST)                                                 \
    do {                                                                    \
        if (CHECK_SEQNUM) {                                                 \
            DPRINTF(Tracer, "C%d=> %#x+%lli(%c)@%lli: [%s]\n",              \
                    INST->getContextId(), INST->instAddr(),                 \
                    INST->microPC(), INST->typeCode, INST->seqNum, #STATE); \
        }                                                                   \
    } while (0)

#define CSPRINT(STATE, INST, x, ...)                                                     \
    do {                                                                                 \
        if (CHECK_SEQNUM) {                                                              \
            DPRINTF(Tracer, "C%d=> %#x+%lli(%c)@%lli: [%s]: " x,                         \
                    INST->getContextId(), INST->instAddr(),                              \
                    INST->microPC(), INST->typeCode, INST->seqNum, #STATE, __VA_ARGS__); \
        }                                                                                \
    } while (0)

#define CCSPRINT(FLAG, STATE, INST, x, ...)                                              \
    do {                                                                                 \
        if (CHECK_SEQNUM) {                                                              \
            DPRINTF(FLAG, "C%d=> %#x+%lli(%c)@%lli: [%s]: " x,                           \
                    INST->getContextId(), INST->instAddr(),                              \
                    INST->microPC(), INST->typeCode, INST->seqNum, #STATE, __VA_ARGS__); \
        }                                                                                \
    } while (0)

#define CPRINT(x, ...)               \
    do {                             \
        if (CHECK_SEQNUM) {          \
            DPRINTF(x, __VA_ARGS__); \
        }                            \
    } while (0)


namespace utils {
typedef enum {
    UNSAFE_CORE,
    DOM,
    FENCE,
    STT
} HWType;

typedef enum {
    Unsafe,
    Spectre,
    Comprehensive
} ThreatModelType;

typedef enum {
    ARM,
    X86
} SysISA;

typedef enum {
    STLD_FWD, // Control (Spectre) + st-ld fwd
    EXCEPTION, // Control (Spectre) + st-ld fwd + exception
    SBD_DISABLED // full comprehensive
} SpecBreakdown;

struct CustomConfigs {
    std::string HWName;
    std::string threatModelName;
    std::string ISAName;

    HWType hw;
    ThreatModelType threatModel;
    SysISA ISA;

    bool delayInvAck;
    bool delayWB;
    bool needsTSO;

    // stats
    bool saveSimStat;
    std::string simStatPath;
    std::string outputDir;
    uint64_t maxInsts;
    uint64_t maxTicks;

    // debug
    bool dumpROB;
    std::string ROBDumpPath;

    // NSR related config
    uint32_t L1DSize, L1DAssoc, L2Size, L2Assoc, numL2, L2VPartition, cachelineSize;
    bool eagerTranslation;

    // CST
    size_t L1DCSTEntryCnt, L1DCSTRecordCnt;
    size_t L2CSTEntryCnt, L2CSTRecordCnt;
    size_t CSTRecordSize;
    bool entryUseCAM, compressCST;

    size_t CLTSize;

    // debug
    bool collectPerfStats;
    uint64_t lowerSeqNum, upperSeqNum;  // range for print DSTATE information
    bool hasLowerBound, hasUpperBound;
    uint64_t youngestSeqNum;

    SpecBreakdown specBreakdown;
};

}  // namespace utils

namespace bridge {
    extern utils::CustomConfigs GCONFIGS;
    extern uint64_t DEADLOCK_THRESHOLD;
    extern uint64_t TICKS_PER_CYCLE;
    extern bool CPU_PROMPTED, DECODE_PROMPTED, LSQ_PROMPTED;
}  // namespace bridge

#endif
