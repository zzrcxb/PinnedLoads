
/*
 * Copyright (c) 2010-2014, 2017-2020 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_LSQ_UNIT_IMPL_HH__
#define __CPU_O3_LSQ_UNIT_IMPL_HH__

#include <map>
#include <cmath>
#include "arch/generic/debugfaults.hh"
#include "arch/x86/ldstflags.hh"
#include "arch/locked_mem.hh"
#include "base/str.hh"
#include "base/intmath.hh"
#include "config/the_isa.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/lsq_unit.hh"
#include "debug/Activity.hh"
#include "debug/HtmCpu.hh"
#include "debug/IEW.hh"
#include "debug/LSQUnit.hh"
#include "debug/O3PipeView.hh"
#include "debug/ROBDump.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

using bridge::GCONFIGS;
using namespace std;

template<class Impl>
LSQUnit<Impl>::WritebackEvent::WritebackEvent(const DynInstPtr &_inst,
        PacketPtr _pkt, LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete),
      inst(_inst), pkt(_pkt), lsqPtr(lsq_ptr)
{
    assert(_inst->savedReq);
    _inst->savedReq->writebackScheduled();
}

template<class Impl>
void
LSQUnit<Impl>::WritebackEvent::process()
{
    assert(!lsqPtr->cpu->switchedOut());

    lsqPtr->writeback(inst, pkt);

    assert(inst->savedReq);
    inst->savedReq->writebackDone();
    delete pkt;
}

template<class Impl>
const char *
LSQUnit<Impl>::WritebackEvent::description() const
{
    return "Store writeback";
}

template <class Impl>
bool
LSQUnit<Impl>::recvTimingResp(PacketPtr pkt)
{
    auto senderState = dynamic_cast<LSQSenderState*>(pkt->senderState);
    LSQRequest* req = senderState->request();
    assert(req != nullptr);
    bool ret = true;
    /* Check that the request is still alive before any further action. */
    if (senderState->alive()) {
        ret = req->recvTimingResp(pkt);
    } else {
        senderState->outstanding--;
    }
    return ret;

}

template <class Impl>
void
LSQUnit<Impl>::reIssueDelayedReqs() {
    assert(GCONFIGS.hw == utils::DOM);
    std::vector<LSQRequest*> toErase;

    for (auto *req : delayedReqs) {
        auto &inst = req->instruction();
        if (inst->isReachedESP() ||
            inst->isSquashed()) {
            if (inst->isSquashed() ||
                req->isReIssued()) {
                DSTATE(ReIssueSquashed, inst);
                toErase.push_back(req);
            } else {
                req->resetSpeculative();
                req->reIssue();
                if (req->isReIssued()) {
                    toErase.push_back(req);
                }
            }
        }
    }

    for (auto *req : toErase) {
        delayedReqs.erase(req);
    }
}

template<class Impl>
void
LSQUnit<Impl>::completeDataAccess(PacketPtr pkt)
{
    LSQSenderState *state = dynamic_cast<LSQSenderState *>(pkt->senderState);
    DynInstPtr inst = state->inst;

    // hardware transactional memory
    // sanity check
    if (pkt->isHtmTransactional() && !inst->isSquashed()) {
        assert(inst->getHtmTransactionUid() == pkt->getHtmTransactionUid());
    }

    // if in a HTM transaction, it's possible
    // to abort within the cache hierarchy.
    // This is signalled back to the processor
    // through responses to memory requests.
    if (pkt->htmTransactionFailedInCache()) {
        // cannot do this for write requests because
        // they cannot tolerate faults
        const HtmCacheFailure htm_rc =
            pkt->getHtmTransactionFailedInCacheRC();
        if(pkt->isWrite()) {
            DPRINTF(HtmCpu,
                "store notification (ignored) of HTM transaction failure "
                "in cache - addr=0x%lx - rc=%s - htmUid=%d\n",
                pkt->getAddr(), htmFailureToStr(htm_rc),
                pkt->getHtmTransactionUid());
        } else {
            HtmFailureFaultCause fail_reason =
                HtmFailureFaultCause::INVALID;

            if (htm_rc == HtmCacheFailure::FAIL_SELF) {
                fail_reason = HtmFailureFaultCause::SIZE;
            } else if (htm_rc == HtmCacheFailure::FAIL_REMOTE) {
                fail_reason = HtmFailureFaultCause::MEMORY;
            } else if (htm_rc == HtmCacheFailure::FAIL_OTHER) {
                // these are likely loads that were issued out of order
                // they are faulted here, but it's unlikely that these will
                // ever reach the commit head.
                fail_reason = HtmFailureFaultCause::OTHER;
            } else {
                panic("HTM error - unhandled return code from cache (%s)",
                      htmFailureToStr(htm_rc));
            }

            inst->fault =
            std::make_shared<GenericHtmFailureFault>(
                inst->getHtmTransactionUid(),
                fail_reason);

            DPRINTF(HtmCpu,
                "load notification of HTM transaction failure "
                "in cache - pc=%s - addr=0x%lx - "
                "rc=%u - htmUid=%d\n",
                inst->pcState(), pkt->getAddr(),
                htmFailureToStr(htm_rc), pkt->getHtmTransactionUid());
        }
    }

    cpu->ppDataAccessComplete->notify(std::make_pair(inst, pkt));

    /* Notify the sender state that the access is complete (for ownership
     * tracking). */
    state->complete();

    assert(!cpu->switchedOut());
    if (!inst->isSquashed()) {
        if (state->needWB) {
            // Only loads, store conditionals and atomics perform the writeback
            // after receiving the response from the memory
            assert(inst->isLoad() || inst->isStoreConditional() ||
                   inst->isAtomic());

            // hardware transactional memory
            if (pkt->htmTransactionFailedInCache()) {
                state->request()->mainPacket()->setHtmTransactionFailedInCache(
                    pkt->getHtmTransactionFailedInCacheRC() );
            }

            DSTATE(WritebackBegin, inst);
            writeback(inst, state->request()->mainPacket());
            if (inst->isStore() || inst->isAtomic()) {
                auto ss = dynamic_cast<SQSenderState*>(state);
                ss->writebackDone();
                completeStore(ss->idx);
            }
        } else if (inst->isStore()) {
            // This is a regular store (i.e., not store conditionals and
            // atomics), so it can complete without writing back
            completeStore(dynamic_cast<SQSenderState*>(state)->idx);
        }
    }
}

template <class Impl>
LSQUnit<Impl>::LSQUnit(uint32_t lqEntries, uint32_t sqEntries)
    : lsqID(-1), storeQueue(sqEntries+1), loadQueue(lqEntries+1),
      loads(0), stores(0), storesToWB(0),
      htmStarts(0), htmStops(0),
      lastRetiredHtmUid(0),
      cacheBlockMask(0), stalled(false),
      isStoreBlocked(false), storeInFlight(false), hasPendingRequest(false),
      pendingRequest(nullptr), stats(nullptr)
{
}

template<class Impl>
void
LSQUnit<Impl>::init(O3CPU *cpu_ptr, IEW *iew_ptr, DerivO3CPUParams *params,
        LSQ *lsq_ptr, unsigned id)
{
    lsqID = id;

    cpu = cpu_ptr;
    iewStage = iew_ptr;

    lsq = lsq_ptr;

    cpu->addStatGroup(csprintf("lsq%i", lsqID).c_str(), &stats);

    DPRINTF(LSQUnit, "Creating LSQUnit%i object.\n",lsqID);

    depCheckShift = params->LSQDepCheckShift;
    checkLoads = params->LSQCheckLoads;
    needsTSO = params->needsTSO;

    if (GCONFIGS.delayInvAck) {
        L1StartIndexBit = static_cast<uint>(log2(GCONFIGS.cachelineSize));
        L1DNumSetBit = static_cast<uint>(floorLog2(
            (GCONFIGS.L1DSize / GCONFIGS.L1DAssoc) / GCONFIGS.cachelineSize
        ));
        L2NumSetBit = static_cast<uint>(floorLog2(
            (GCONFIGS.L2Size / GCONFIGS.L2Assoc) / GCONFIGS.cachelineSize
        ));
        numL2Bit = static_cast<uint>(log2(GCONFIGS.numL2));
        // L2StartIndexBit = L1StartIndexBit + numL2Bit;
        // these two should be the same if we switch to Haswell's hash function
        L2StartIndexBit = L1StartIndexBit;

        L1DConflictSize = GCONFIGS.L1DAssoc;

        if (GCONFIGS.L2VPartition > 1) {
            if (GCONFIGS.L2Assoc % GCONFIGS.L2VPartition != 0) {
                cerr << ZWARN
                    << "L2 associativity cannot be divided by L2 VPartition! "
                    << "Fallback to Strict mode."
                    << endl;
                nsrPolicy = STRICT;
            } else {
                L2ConflictSize = GCONFIGS.L2Assoc / GCONFIGS.L2VPartition;
                nsrPolicy = PARTITIONED;

                assert(GCONFIGS.L1DCSTRecordCnt <= L1DConflictSize);
                assert(GCONFIGS.L2CSTRecordCnt <= L2ConflictSize);
            }
        } else {
            nsrPolicy = STRICT;
        }

        map<NSRPolicy, string> _policies = {
            {STRICT, "Strict"},
            {PARTITIONED, "Partitioned"}};

        if (!bridge::LSQ_PROMPTED) {
            cerr << ZINFO
                 << "NSR Mode: " << _policies.at(nsrPolicy);
            if (nsrPolicy == PARTITIONED) {
                cerr << " virtual partition size: " << L2ConflictSize << endl;
                cerr << ZINFO
                     << "L1DCST (#Entries X #Records): " << GCONFIGS.L1DCSTEntryCnt
                     << "X" << GCONFIGS.L1DCSTRecordCnt << endl
                     << ZINFO
                     << "L2CST (#Entries X #Records): " << GCONFIGS.L2CSTEntryCnt
                     << "X" << GCONFIGS.L2CSTRecordCnt << endl
                     << ZINFO
                     << "Use CAM: " << GCONFIGS.entryUseCAM << endl
                     << ZINFO
                     << "CST Addr field: " << GCONFIGS.CSTRecordSize << " bits"
                     << "; Compressed: " << GCONFIGS.compressCST;
            }
            cerr << endl;
            bridge::LSQ_PROMPTED = true;
        }
    }

    resetState();
}


template<class Impl>
void
LSQUnit<Impl>::resetState()
{
    loads = stores = storesToWB = 0;

    // hardware transactional memory
    // nesting depth
    htmStarts = htmStops = 0;

    storeWBIt = storeQueue.begin();

    retryPkt = NULL;
    memDepViolator = NULL;

    stalled = false;

    cacheBlockMask = ~(cpu->cacheLineSize() - 1);

    stats.lockedLQEntryNum.init(0, loadQueue.capacity() - 1, 1).flags(Stats::pdf);
    stats.L1DCSTEntries.init(0, GCONFIGS.L1DCSTEntryCnt, 1).flags(Stats::pdf);
    stats.L2CSTEntries.init(0, GCONFIGS.L2CSTEntryCnt, 1).flags(Stats::pdf);
    stats.CLTEntries.init(0, GCONFIGS.CLTSize, 1).flags(Stats::pdf);

    if (nsrPolicy == PARTITIONED) {
        L1DCST = make_shared<CacheShadowTable>(GCONFIGS.L1DCSTEntryCnt,
                    GCONFIGS.L1DCSTRecordCnt, cacheBlockMask);
        L2CST = make_shared<CacheShadowTable>(GCONFIGS.L2CSTEntryCnt,
                    GCONFIGS.L2CSTRecordCnt, cacheBlockMask);
    }
}

template<class Impl>
std::string
LSQUnit<Impl>::name() const
{
    if (Impl::MaxThreads == 1) {
        return iewStage->name() + ".lsq";
    } else {
        return iewStage->name() + ".lsq.thread" + std::to_string(lsqID);
    }
}

template <class Impl>
LSQUnit<Impl>::LSQUnitStats::LSQUnitStats(Stats::Group *parent)
    : Stats::Group(parent),
      ADD_STAT(forwLoads, "Number of loads that had data forwarded from"
          " stores"),
      ADD_STAT(squashedLoads, "Number of loads squashed"),
      ADD_STAT(ignoredResponses, "Number of memory responses ignored"
          " because the instruction is squashed"),
      ADD_STAT(memOrderViolation, "Number of memory ordering violations"),
      ADD_STAT(squashedStores, "Number of stores squashed"),
      ADD_STAT(rescheduledLoads, "Number of loads that were rescheduled"),
      ADD_STAT(blockedByCache, "Number of times an access to memory failed"
          " due to the cache being blocked"),
      ADD_STAT(unSquashables, "Number of un-squashable LQ entries"),
      ADD_STAT(preLocks, "Number of pre-locked LQ entries"),
      ADD_STAT(NSRConflictChecks, "Number of NSR conflict checks"),
      ADD_STAT(NSRConflicts, "Number of NSR conflicts"),
      ADD_STAT(NSRL1Conflicts, "Number of NSR L1 conflicts"),
      ADD_STAT(NSRLLCConflicts, "Number of NSR LLC conflicts"),
      ADD_STAT(NSRLockingReq, "Number of NSR locking req sent from LSQUnit"),
      ADD_STAT(NSRUnlockingReq, "Number of NSR unlocking req sent from LSQUnit"),
      ADD_STAT(squashedLocked, "Number of squashed un-squashable entries, should be 0"),
      ADD_STAT(eagerTranslated, "Number of eagerly translated stores"),
      ADD_STAT(delayedFwd, "Number of delayed load forwarding"),
      ADD_STAT(lockedLQEntryNum, "Distribution of number of locked LQ entries"),
      ADD_STAT(L1DCSTOverflow, "Number of L1D CST overflow"),
      ADD_STAT(L1DCSTTagMiss, "Number of L1D CST tag miss"),
      ADD_STAT(L1DCSTHashCollision, "Number of L1D CST hash collision"),
      ADD_STAT(L1DCSTTagMissConf, "Number of L1D CST tag miss causes conflicts"),
      ADD_STAT(L1DCSTHashColConf, "Number of L1D CST hash collision causes conflicts"),
      ADD_STAT(L1DCSTEntries, "Number of L1D CST entries"),
      ADD_STAT(L2CSTOverflow, "Number of L2 CST overflow"),
      ADD_STAT(L2CSTTagMiss, "Number of L2 CST tag miss"),
      ADD_STAT(L2CSTHashCollision, "Number of L2 CST hash collision"),
      ADD_STAT(L2CSTTagMissConf, "Number of L2 CST tag miss causes conflicts"),
      ADD_STAT(L2CSTHashColConf, "Number of L2 CST hash collision causes conflicts"),
      ADD_STAT(L2CSTEntries, "Number of L2 CST entries"),
      ADD_STAT(CLTOverflow, "Number of CLT overflow"),
      ADD_STAT(CLTEntries, "Number of CLT entries"),
      ADD_STAT(NSRUnlockVP, "Cannot Lock due to not reaching VP"),
      ADD_STAT(NSRUnlockException, "Cannot Lock due to exception"),
      ADD_STAT(NSRUnlockAddr, "Cannot Lock due to address not valid"),
      ADD_STAT(NSRUnlockDataRecv, "Cannot Lock due to not receiving data"),
      ADD_STAT(NSRUnlockConflicts, "Cannot Lock due to resource conflicts"),
      ADD_STAT(NSRUnlockRMW, "Cannot Lock due to RMW"),
      ADD_STAT(NSRUnlockSB, "Cannot Lock due to store buffer is full")
{
}

template<class Impl>
void
LSQUnit<Impl>::setDcachePort(RequestPort *dcache_port)
{
    dcachePort = dcache_port;
}

template<class Impl>
void
LSQUnit<Impl>::drainSanityCheck() const
{
    for (int i = 0; i < loadQueue.capacity(); ++i)
        assert(!loadQueue[i].valid());

    assert(storesToWB == 0);
    assert(!retryPkt);
}

template<class Impl>
void
LSQUnit<Impl>::takeOverFrom()
{
    resetState();
}

template <class Impl>
void
LSQUnit<Impl>::insert(const DynInstPtr &inst)
{
    assert(inst->isMemRef());

    assert(inst->isLoad() || inst->isStore() || inst->isAtomic());

    if (inst->isLoad()) {
        insertLoad(inst);
    } else {
        insertStore(inst);
    }

    inst->setInLSQ();
}

template <class Impl>
void
LSQUnit<Impl>::insertLoad(const DynInstPtr &load_inst)
{
    assert(!loadQueue.full());
    assert(loads < loadQueue.capacity());

    DPRINTF(LSQUnit, "Inserting load PC %s, idx:%i [sn:%lli]\n",
            load_inst->pcState(), loadQueue.tail(), load_inst->seqNum);

    /* Grow the queue. */
    loadQueue.advance_tail();

    load_inst->sqIt = storeQueue.end();

    assert(!loadQueue.back().valid());
    loadQueue.back().set(load_inst);
    load_inst->lqIdx = loadQueue.tail();
    load_inst->lqIt = loadQueue.getIterator(load_inst->lqIdx);

    ++loads;

    // hardware transactional memory
    // transactional state and nesting depth must be tracked
    // in the in-order part of the core.
    if (load_inst->isHtmStart()) {
        htmStarts++;
        DPRINTF(HtmCpu, ">> htmStarts++ (%d) : htmStops (%d)\n",
                htmStarts, htmStops);

        const int htm_depth = htmStarts - htmStops;
        const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
        auto htm_uid = htm_cpt->getHtmUid();

        // for debugging purposes
        if (!load_inst->inHtmTransactionalState()) {
            htm_uid = htm_cpt->newHtmUid();
            DPRINTF(HtmCpu, "generating new htmUid=%u\n", htm_uid);
            if (htm_depth != 1) {
                DPRINTF(HtmCpu,
                    "unusual HTM transactional depth (%d)"
                    " possibly caused by mispeculation - htmUid=%u\n",
                    htm_depth, htm_uid);
            }
        }
        load_inst->setHtmTransactionalState(htm_uid, htm_depth);
    }

    if (load_inst->isHtmStop()) {
        htmStops++;
        DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops++ (%d)\n",
                htmStarts, htmStops);

        if (htmStops==1 && htmStarts==0) {
            DPRINTF(HtmCpu,
            "htmStops==1 && htmStarts==0. "
            "This generally shouldn't happen "
            "(unless due to misspeculation)\n");
        }
    }
}

template <class Impl>
void
LSQUnit<Impl>::insertStore(const DynInstPtr& store_inst)
{
    // Make sure it is not full before inserting an instruction.
    assert(!storeQueue.full());
    assert(stores < storeQueue.capacity());

    DPRINTF(LSQUnit, "Inserting store PC %s, idx:%i [sn:%lli]\n",
            store_inst->pcState(), storeQueue.tail(), store_inst->seqNum);
    storeQueue.advance_tail();

    store_inst->sqIdx = storeQueue.tail();
    store_inst->lqIdx = loadQueue.moduloAdd(loadQueue.tail(), 1);
    store_inst->lqIt = loadQueue.end();

    storeQueue.back().set(store_inst);

    ++stores;
}

template <class Impl>
typename Impl::DynInstPtr
LSQUnit<Impl>::getMemDepViolator()
{
    DynInstPtr temp = memDepViolator;

    memDepViolator = NULL;

    return temp;
}

template <class Impl>
unsigned
LSQUnit<Impl>::numFreeLoadEntries()
{
        //LQ has an extra dummy entry to differentiate
        //empty/full conditions. Subtract 1 from the free entries.
        DPRINTF(LSQUnit, "LQ size: %d, #loads occupied: %d\n",
                1 + loadQueue.capacity(), loads);
        return loadQueue.capacity() - loads;
}

template <class Impl>
unsigned
LSQUnit<Impl>::numFreeStoreEntries()
{
        //SQ has an extra dummy entry to differentiate
        //empty/full conditions. Subtract 1 from the free entries.
        DPRINTF(LSQUnit, "SQ size: %d, #stores occupied: %d\n",
                1 + storeQueue.capacity(), stores);
        return storeQueue.capacity() - stores;

 }

template <class Impl>
void
LSQUnit<Impl>::updateCLT(PacketPtr pkt)
{
    assert(pkt->isUpdateCLT());
    Addr addr = pkt->getAddr() & cacheBlockMask;

    if (pkt->isInsertCLT()) {
        if (!IN_SET(addr, unlockableLines)) {
            if (unlockableLines.size() < GCONFIGS.CLTSize) {
                unlockableLines.insert(addr);
                DPRINTF(NSR, "Add %#x to CLT\n", addr);
            } else {
                stats.CLTOverflow += 1;
                CLTOverflowed = true;
                DPRINTF(NSR, "CLT overflow, line: %#x\n", addr);
            }
        }
    } else if (pkt->isEraseCLT()) {
        unlockableLines.erase(addr);
        CLTOverflowed = unlockableLines.size() >= GCONFIGS.CLTSize;
        DPRINTF(NSR, "Erase %#x from CLT\n", addr);
    }
    stats.CLTEntries.sample(unlockableLines.size());
}

template <class Impl>
void
LSQUnit<Impl>::checkSnoop(PacketPtr pkt)
{
    // Should only ever get invalidations in here
    assert(pkt->isInvalidate());

    DPRINTF(LSQUnit, "Got snoop for address %#x\n", pkt->getAddr());

    for (int x = 0; x < cpu->numContexts(); x++) {
        ThreadContext *tc = cpu->getContext(x);
        bool no_squash = cpu->thread[x]->noSquashFromTC;
        cpu->thread[x]->noSquashFromTC = true;
        TheISA::handleLockedSnoop(tc, pkt, cacheBlockMask);
        cpu->thread[x]->noSquashFromTC = no_squash;
    }

    if (loadQueue.empty())
        return;

    auto iter = loadQueue.begin();

    Addr invalidate_addr = pkt->getAddr() & cacheBlockMask;
    DPRINTF(NSR, "Trying to invalidate line: %p\n", invalidate_addr);
    if (IN_MAP(invalidate_addr, YNLC_map)) {
        DPRINTF(NSR, "[ERROR] trying to invalidate a locked line: %p\n",
                invalidate_addr);
    }
    assert(!IN_MAP(invalidate_addr, YNLC_map));

    DynInstPtr ld_inst = iter->instruction();
    assert(ld_inst);
    LSQRequest *req = iter->request();

    // Check that this snoop didn't just invalidate our lock flag
    if (ld_inst->effAddrValid() &&
        req->isCacheBlockHit(invalidate_addr, cacheBlockMask)
        && ld_inst->memReqFlags & Request::LLSC)
        TheISA::handleLockedSnoopHit(ld_inst.get());

    bool force_squash = false;
    bool seen_SOS = false;
    for (; iter != loadQueue.end(); iter++) {
        ld_inst = iter->instruction();
        assert(ld_inst);
        req = iter->request();

        if (ld_inst->isUnSquashable() || ld_inst->isCommitted() ||
            ld_inst->isDataPrefetch()) {
            // un-squashable, data prefetch, and committed loads are ordered
            continue;
        } else if (!seen_SOS) {
            // after skipping all un-squashable, data prefetch, and committed
            // loads, the first one is the source of speculation
            seen_SOS = true;
            continue;
        } else if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()) {
            continue;
        }

        DPRINTF(LSQUnit, "-- inst [sn:%lli] to pktAddr:%#x\n",
                    ld_inst->seqNum, invalidate_addr);

        if (force_squash ||
            req->isCacheBlockHit(invalidate_addr, cacheBlockMask)) {
            if (needsTSO) {
                // If we have a TSO system, as all loads must be ordered with
                // all other loads, this load as well as *all* subsequent loads
                // need to be squashed to prevent possible load reordering.
                force_squash = true;
            }
            if (ld_inst->possibleLoadViolation() || force_squash) {
                DPRINTF(LSQUnit, "Conflicting load at addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Mark the load for re-execution
                ld_inst->fault = std::make_shared<ReExec>();
                req->setStateToFault();

                CSPRINT(ReExec, ld_inst, "invalid line: %p\n", invalidate_addr);
            } else {
                DPRINTF(LSQUnit, "HitExternal Snoop for addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Make sure that we don't lose a snoop hitting a LOCKED
                // address since the LOCK* flags don't get updated until
                // commit.
                if (ld_inst->memReqFlags & Request::LLSC)
                    TheISA::handleLockedSnoopHit(ld_inst.get());

                // If a older load checks this and it's true
                // then we might have missed the snoop
                // in which case we need to invalidate to be sure
                ld_inst->hitExternalSnoop(true);
            }
        }
    }
    return;
}

template <class Impl>
Fault
LSQUnit<Impl>::checkViolations(typename LoadQueue::iterator& loadIt,
        const DynInstPtr& inst)
{
    Addr inst_eff_addr1 = inst->effAddr >> depCheckShift;
    Addr inst_eff_addr2 = (inst->effAddr + inst->effSize - 1) >> depCheckShift;

    /** @todo in theory you only need to check an instruction that has executed
     * however, there isn't a good way in the pipeline at the moment to check
     * all instructions that will execute before the store writes back. Thus,
     * like the implementation that came before it, we're overly conservative.
     */
    while (loadIt != loadQueue.end()) {
        DynInstPtr ld_inst = loadIt->instruction();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()) {
            ++loadIt;
            continue;
        }

        Addr ld_eff_addr1 = ld_inst->effAddr >> depCheckShift;
        Addr ld_eff_addr2 =
            (ld_inst->effAddr + ld_inst->effSize - 1) >> depCheckShift;

        if (inst_eff_addr2 >= ld_eff_addr1 && inst_eff_addr1 <= ld_eff_addr2) {
            if (inst->isLoad()) {
                // If this load is to the same block as an external snoop
                // invalidate that we've observed then the load needs to be
                // squashed as it could have newer data
                if (ld_inst->hitExternalSnoop()) {
                    if (!memDepViolator ||
                            ld_inst->seqNum < memDepViolator->seqNum) {
                        DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] "
                                "and [sn:%lli] at address %#x\n",
                                inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                        ld_inst->setMemViolation();
                        memDepViolator = ld_inst;
                        assert(!ld_inst->isUnSquashable());

                        ++stats.memOrderViolation;

                        return std::make_shared<GenericISA::M5PanicFault>(
                            "Detected fault with inst [sn:%lli] and "
                            "[sn:%lli] at address %#x\n",
                            inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                    }
                }

                // Otherwise, mark the load has a possible load violation
                // and if we see a snoop before it's commited, we need to squash
                ld_inst->possibleLoadViolation(true);
                DPRINTF(LSQUnit, "Found possible load violation at addr: %#x"
                        " between instructions [sn:%lli] and [sn:%lli]\n",
                        inst_eff_addr1, inst->seqNum, ld_inst->seqNum);
            } else {
                // A load/store incorrectly passed this store.
                // Check if we already have a violator, or if it's newer
                // squash and refetch.
                if (memDepViolator && ld_inst->seqNum > memDepViolator->seqNum)
                    break;

                DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] and "
                        "[sn:%lli] at address %#x\n",
                        inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                ld_inst->setMemViolation();
                memDepViolator = ld_inst;
                assert(!ld_inst->isUnSquashable());

                ++stats.memOrderViolation;

                return std::make_shared<GenericISA::M5PanicFault>(
                    "Detected fault with "
                    "inst [sn:%lli] and [sn:%lli] at address %#x\n",
                    inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
            }
        }

        ++loadIt;
    }
    return NoFault;
}




template <class Impl>
Fault
LSQUnit<Impl>::executeLoad(const DynInstPtr &inst)
{
    using namespace TheISA;
    // Execute a specific load.
    Fault load_fault = NoFault;

    DPRINTF(LSQUnit, "Executing load PC %s, [sn:%lli]\n",
            inst->pcState(), inst->seqNum);

    assert(!inst->isSquashed());

    load_fault = inst->initiateAcc();

    if (load_fault == NoFault && !inst->readMemAccPredicate()) {
        assert(inst->readPredicate());
        inst->setExecuted();
        inst->completeAcc(nullptr);
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        return NoFault;
    }

    if (inst->isTranslationDelayed() && load_fault == NoFault)
        return load_fault;

    if (load_fault != NoFault && inst->translationCompleted() &&
        inst->savedReq->isPartialFault() && !inst->savedReq->isComplete()) {
        assert(inst->savedReq->isSplit());
        // If we have a partial fault where the mem access is not complete yet
        // then the cache must have been blocked. This load will be re-executed
        // when the cache gets unblocked. We will handle the fault when the
        // mem access is complete.
        return NoFault;
    }

    // If the instruction faulted or predicated false, then we need to send it
    // along to commit without the instruction completing.
    if (load_fault != NoFault || !inst->readPredicate()) {
        // Send this instruction to commit, also make sure iew stage
        // realizes there is activity.  Mark it as executed unless it
        // is a strictly ordered load that needs to hit the head of
        // commit.
        if (!inst->readPredicate())
            inst->forwardOldRegs();
        DPRINTF(LSQUnit, "Load [sn:%lli] not executed from %s\n",
                inst->seqNum,
                (load_fault != NoFault ? "fault" : "predication"));
        if (!(inst->hasRequest() && inst->strictlyOrdered()) ||
            inst->isAtCommit()) {
            inst->setExecuted();
        }
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
    } else {
        if (inst->effAddrValid()) {
            auto it = inst->lqIt;
            ++it;

            if (checkLoads)
                return checkViolations(it, inst);
        }
    }

    return load_fault;
}

template <class Impl>
Fault
LSQUnit<Impl>::executeStore(const DynInstPtr &store_inst)
{
    using namespace TheISA;
    // Make sure that a store exists.
    assert(stores != 0);

    int store_idx = store_inst->sqIdx;

    DPRINTF(LSQUnit, "Executing store PC %s [sn:%lli]\n",
            store_inst->pcState(), store_inst->seqNum);

    assert(!store_inst->isSquashed());

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = store_inst->lqIt;

    Fault store_fault = store_inst->initiateAcc();

    if (store_inst->isTranslationDelayed() &&
        store_fault == NoFault)
        return store_fault;

    if (!store_inst->readPredicate()) {
        DPRINTF(LSQUnit, "Store [sn:%lli] not executed from predication\n",
                store_inst->seqNum);
        store_inst->forwardOldRegs();
        return store_fault;
    }

    if (storeQueue[store_idx].size() == 0) {
        DPRINTF(LSQUnit,"Fault on Store PC %s, [sn:%lli], Size = 0\n",
                store_inst->pcState(), store_inst->seqNum);

        return store_fault;
    }

    assert(store_fault == NoFault);

    if (store_inst->isStoreConditional() || store_inst->isAtomic()) {
        // Store conditionals and Atomics need to set themselves as able to
        // writeback if we haven't had a fault by here.
        storeQueue[store_idx].canWB() = true;

        ++storesToWB;
    }

    return checkViolations(loadIt, store_inst);

}

template <class Impl>
void
LSQUnit<Impl>::commitLoad()
{
    assert(loadQueue.front().valid());

    DPRINTF(LSQUnit, "Committing head load instruction, PC %s\n",
            loadQueue.front().instruction()->pcState());

    if (loadQueue.front().instruction()->hasPendingUnlockReq()) {
        loadQueue.front().instruction()->resetPendingUnlockReq();
    }

    if (nsrPolicy == PARTITIONED) {
        removeEntryConflicts(loadQueue.head());
    }

    loadQueue.front().clear();
    loadQueue.pop_front();

    --loads;
}

template <class Impl>
void
LSQUnit<Impl>::commitLoads(InstSeqNum &youngest_inst)
{
    assert(loads == 0 || loadQueue.front().valid());

    while (loads != 0 &&
           loadQueue.front().instruction()->seqNum <= youngest_inst) {
        bool canCommit = true;
        if (GCONFIGS.delayInvAck) {
            canCommit = tryUnlockLQEntry(loadQueue.head(), false);
        }

        if (canCommit) {
            DSTATE(CommitLoad, loadQueue.front().instruction());
            commitLoad();
        } else {
            DPRINTF(LSQUnit, "Cannot commit head load instruction, PC %#x\n",
            loadQueue.front().instruction()->instAddr());
            break;
        }
    }
}

template <class Impl>
void
LSQUnit<Impl>::commitStores(InstSeqNum &youngest_inst)
{
    assert(stores == 0 || storeQueue.front().valid());

    /* Forward iterate the store queue (age order). */
    for (auto& x : storeQueue) {
        assert(x.valid());
        // Mark any stores that are now committed and have not yet
        // been marked as able to write back.
        if (!x.canWB()) {
            if (x.instruction()->seqNum > youngest_inst) {
                break;
            }
            DPRINTF(LSQUnit, "Marking store as able to write back, PC "
                    "%s [sn:%lli]\n",
                    x.instruction()->pcState(),
                    x.instruction()->seqNum);

            x.canWB() = true;

            DSTATE(CanWB, x.instruction());

            ++storesToWB;
        }
    }
}

template <class Impl>
void
LSQUnit<Impl>::writebackBlockedStore()
{
    assert(isStoreBlocked);
    storeWBIt->request()->sendPacketToCache();
    if (storeWBIt->request()->isSent()){
        storePostSend();
    }
}

template <class Impl>
void
LSQUnit<Impl>::writebackStores()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Writing back  blocked store\n");
        writebackBlockedStore();
    }

    while (storesToWB > 0 &&
           storeWBIt.dereferenceable() &&
           storeWBIt->valid() &&
           storeWBIt->canWB() &&
           ((!needsTSO) || (!storeInFlight)) &&
           lsq->cachePortAvailable(false)) {

        CSPRINT(TryWB, storeWBIt->instruction(), "blocked: %d; addr: %#x\n",
                isStoreBlocked, storeWBIt->instruction()->physEffAddr);

        if (isStoreBlocked) {
            DPRINTF(LSQUnit, "Unable to write back any more stores, cache"
                    " is blocked!\n");
            break;
        }

        // Store didn't write any data so no need to write it back to
        // memory.
        if (storeWBIt->size() == 0) {
            /* It is important that the preincrement happens at (or before)
             * the call, as the the code of completeStore checks
             * storeWBIt. */
            completeStore(storeWBIt++);
            continue;
        }

        if (storeWBIt->instruction()->isDataPrefetch()) {
            storeWBIt++;
            continue;
        }

        assert(storeWBIt->hasRequest());
        assert(!storeWBIt->committed());

        DynInstPtr inst = storeWBIt->instruction();
        LSQRequest* req = storeWBIt->request();

        // Process store conditionals or store release after all previous
        // stores are completed
        if ((req->mainRequest()->isLLSC() ||
             req->mainRequest()->isRelease()) &&
             (storeWBIt.idx() != storeQueue.head())) {
            DPRINTF(LSQUnit, "Store idx:%i PC:%s to Addr:%#x "
                "[sn:%lli] is %s%s and not head of the queue\n",
                storeWBIt.idx(), inst->pcState(),
                req->request()->getPaddr(), inst->seqNum,
                req->mainRequest()->isLLSC() ? "SC" : "",
                req->mainRequest()->isRelease() ? "/Release" : "");
            break;
        }

        storeWBIt->committed() = true;

        assert(!inst->memData);
        inst->memData = new uint8_t[req->_size];

        if (storeWBIt->isAllZeros())
            memset(inst->memData, 0, req->_size);
        else
            memcpy(inst->memData, storeWBIt->data(), req->_size);

        if (DTRACE(MemDbg)) {
            assert(!inst->isSquashed());

            uint64_t data;
            switch (inst->effSize) {
            case 1:
                data = *((uint8_t*)inst->memData);
                break;
            case 2:
                data = *((uint16_t*)inst->memData);
                break;
            case 4:
                data = *((uint32_t*)inst->memData);
                break;
            case 8:
                data = *((uint64_t*)inst->memData);
                break;
            default:
                break;
            }
            CCSPRINT(MemDbg, Mem, inst,
                     "%c: eff: %#x, phy: %#x, size: %u, data: %lu, atomic: %d, sc: %d\n",
                     inst->typeCode,
                     inst->effAddr, inst->physEffAddr,
                     inst->effSize, data, inst->isAtomic(),
                     inst->isStoreConditional());
        }

        if (req->senderState() == nullptr) {
            SQSenderState *state = new SQSenderState(storeWBIt);
            state->isLoad = false;
            state->needWB = false;
            state->inst = inst;

            req->senderState(state);
            if (inst->isStoreConditional() || inst->isAtomic()) {
                /* Only store conditionals and atomics need a writeback. */
                state->needWB = true;
            }
        }
        req->buildPackets();

        DPRINTF(LSQUnit, "D-Cache: Writing back store idx:%i PC:%s "
                "to Addr:%#x, data:%#x [sn:%lli]\n",
                storeWBIt.idx(), inst->pcState(),
                req->request()->getPaddr(), (int)*(inst->memData),
                inst->seqNum);

        // @todo: Remove this SC hack once the memory system handles it.
        if (inst->isStoreConditional()) {
            // Disable recording the result temporarily.  Writing to
            // misc regs normally updates the result, but this is not
            // the desired behavior when handling store conditionals.
            inst->recordResult(false);
            bool success = TheISA::handleLockedWrite(inst.get(),
                    req->request(), cacheBlockMask);
            inst->recordResult(true);
            req->packetSent();

            if (!success) {
                req->complete();
                // Instantly complete this store.
                DPRINTF(LSQUnit, "Store conditional [sn:%lli] failed.  "
                        "Instantly completing it.\n",
                        inst->seqNum);
                PacketPtr new_pkt = new Packet(*req->packet());
                WritebackEvent *wb = new WritebackEvent(inst,
                        new_pkt, this);
                cpu->schedule(wb, curTick() + 1);
                completeStore(storeWBIt);
                if (!storeQueue.empty())
                    storeWBIt++;
                else
                    storeWBIt = storeQueue.end();
                continue;
            }
        }

        if (req->request()->isLocalAccess()) {
            assert(!inst->isStoreConditional());
            assert(!inst->inHtmTransactionalState());
            ThreadContext *thread = cpu->tcBase(lsqID);
            PacketPtr main_pkt = new Packet(req->mainRequest(),
                                            MemCmd::WriteReq);
            main_pkt->dataStatic(inst->memData);
            req->request()->localAccessor(thread, main_pkt);
            delete main_pkt;
            completeStore(storeWBIt);
            storeWBIt++;
            continue;
        }
        /* Send to cache */
        req->sendPacketToCache();

        /* If successful, do the post send */
        if (req->isSent()) {
            storePostSend();
        } else {
            DPRINTF(LSQUnit, "D-Cache became blocked when writing [sn:%lli], "
                    "will retry later\n",
                    inst->seqNum);
        }
    }
    assert(stores >= 0 && storesToWB >= 0);
}

template <class Impl>
void
LSQUnit<Impl>::squash(const InstSeqNum &squashed_num)
{
    DPRINTF(LSQUnit, "Squashing until [sn:%lli]!"
            "(Loads:%i Stores:%i)\n", squashed_num, loads, stores);

    while (loads != 0 &&
            loadQueue.back().instruction()->seqNum > squashed_num) {
        DPRINTF(LSQUnit,"Load Instruction PC %s squashed, "
                "[sn:%lli]\n",
                loadQueue.back().instruction()->pcState(),
                loadQueue.back().instruction()->seqNum);

        if (isStalled() && loadQueue.tail() == stallingLoadIdx) {
            stalled = false;
            stallingStoreIsn = 0;
            stallingLoadIdx = 0;
        }

        // hardware transactional memory
        // Squashing instructions can alter the transaction nesting depth
        // and must be corrected before fetching resumes.
        if (loadQueue.back().instruction()->isHtmStart())
        {
            htmStarts = (--htmStarts < 0) ? 0 : htmStarts;
            DPRINTF(HtmCpu, ">> htmStarts-- (%d) : htmStops (%d)\n",
              htmStarts, htmStops);
        }
        if (loadQueue.back().instruction()->isHtmStop())
        {
            htmStops = (--htmStops < 0) ? 0 : htmStops;
            DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops-- (%d)\n",
              htmStarts, htmStops);
        }
        if (loadQueue.back().instruction()->isUnSquashable()) {
            warn_once("Squashing an un-squashable LQ entry! seqNum: %llu\n",
                      loadQueue.back().instruction()->seqNum);
            tryUnlockLQEntry(loadQueue.tail(), true);
            stats.squashedLocked++;
        }
        if (nsrPolicy == PARTITIONED) {
            removeEntryConflicts(loadQueue.tail());
        }

        // remove req from delayed buffer
        if (GCONFIGS.hw == utils::DOM) {
            delayedReqs.erase(loadQueue.back().request());
        }

        // Clear the smart pointer to make sure it is decremented.
        loadQueue.back().instruction()->setSquashed();
        loadQueue.back().clear();

        --loads;

        loadQueue.pop_back();
        ++stats.squashedLoads;
    }

    // hardware transactional memory
    // scan load queue (from oldest to youngest) for most recent valid htmUid
    auto scan_it = loadQueue.begin();
    uint64_t in_flight_uid = 0;
    while (scan_it != loadQueue.end()) {
        if (scan_it->instruction()->isHtmStart() &&
            !scan_it->instruction()->isSquashed()) {
            in_flight_uid = scan_it->instruction()->getHtmTransactionUid();
            DPRINTF(HtmCpu, "loadQueue[%d]: found valid HtmStart htmUid=%u\n",
                scan_it._idx, in_flight_uid);
        }
        scan_it++;
    }
    // If there's a HtmStart in the pipeline then use its htmUid,
    // otherwise use the most recently committed uid
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    if (htm_cpt) {
        const uint64_t old_local_htm_uid = htm_cpt->getHtmUid();
        uint64_t new_local_htm_uid;
        if (in_flight_uid > 0)
            new_local_htm_uid = in_flight_uid;
        else
            new_local_htm_uid = lastRetiredHtmUid;

        if (old_local_htm_uid != new_local_htm_uid) {
            DPRINTF(HtmCpu, "flush: lastRetiredHtmUid=%u\n",
                lastRetiredHtmUid);
            DPRINTF(HtmCpu, "flush: resetting localHtmUid=%u\n",
                new_local_htm_uid);

            htm_cpt->setHtmUid(new_local_htm_uid);
        }
    }

    if (memDepViolator && squashed_num < memDepViolator->seqNum) {
        memDepViolator = NULL;
    }

    while (stores != 0 &&
           storeQueue.back().instruction()->seqNum > squashed_num) {
        // Instructions marked as can WB are already committed.
        if (storeQueue.back().canWB()) {
            break;
        }

        DPRINTF(LSQUnit,"Store Instruction PC %s squashed, "
                "idx:%i [sn:%lli]\n",
                storeQueue.back().instruction()->pcState(),
                storeQueue.tail(), storeQueue.back().instruction()->seqNum);

        // I don't think this can happen.  It should have been cleared
        // by the stalling load.
        if (isStalled() &&
            storeQueue.back().instruction()->seqNum == stallingStoreIsn) {
            panic("Is stalled should have been cleared by stalling load!\n");
            stalled = false;
            stallingStoreIsn = 0;
        }

        // Clear the smart pointer to make sure it is decremented.
        storeQueue.back().instruction()->setSquashed();

        // Must delete request now that it wasn't handed off to
        // memory.  This is quite ugly.  @todo: Figure out the proper
        // place to really handle request deletes.
        storeQueue.back().clear();
        --stores;

        storeQueue.pop_back();
        ++stats.squashedStores;
    }

    vector<int16_t> toErase;
    for (auto &p : eagerTransBuffer) {
        auto idx = p.first;
        auto &inst = storeQueue[idx].instruction();
        if (inst && inst->seqNum > squashed_num) {
            toErase.push_back(idx);
        }
    }

    for (auto &i : toErase) {
        eagerTransBuffer.erase(i);
    }
}

template <class Impl>
void
LSQUnit<Impl>::storePostSend()
{
    if (isStalled() &&
        storeWBIt->instruction()->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%i\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        // the stallingLoadIdx could be invalid because
        // it has been replayed due to a strictly ordered load
        if (loadQueue[stallingLoadIdx].instruction()) {
            iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
        }
    }

    if (!storeWBIt->instruction()->isStoreConditional()) {
        // The store is basically completed at this time. This
        // only works so long as the checker doesn't try to
        // verify the value in memory for stores.
        storeWBIt->instruction()->setCompleted();

        if (cpu->checker) {
            cpu->checker->verify(storeWBIt->instruction());
        }
    }

    if (needsTSO) {
        DSTATE(StInflight, storeWBIt->instruction());
        storeInFlight = true;
    }

    storeWBIt++;
}

template <class Impl>
void
LSQUnit<Impl>::writeback(const DynInstPtr &inst, PacketPtr pkt)
{
    iewStage->wakeCPU();

    // Squashed instructions do not need to complete their access.
    if (inst->isSquashed()) {
        assert (!inst->isStore() || inst->isStoreConditional());
        ++stats.ignoredResponses;
        return;
    }

    if (!inst->isExecuted()) {
        inst->setExecuted();

        if (inst->fault == NoFault) {
            inst->setDataReceived();
            // Complete access to copy data to proper place.
            inst->completeAcc(pkt);
        } else {
            // If the instruction has an outstanding fault, we cannot complete
            // the access as this discards the current fault.

            // If we have an outstanding fault, the fault should only be of
            // type ReExec or - in case of a SplitRequest - a partial
            // translation fault

            // Unless it's a hardware transactional memory fault
            auto htm_fault = std::dynamic_pointer_cast<
                GenericHtmFailureFault>(inst->fault);

            if (!htm_fault) {
                assert(dynamic_cast<ReExec*>(inst->fault.get()) != nullptr ||
                       inst->savedReq->isPartialFault());

            } else if (!pkt->htmTransactionFailedInCache()) {
                // Situation in which the instruction has a hardware transactional
                // memory fault but not the packet itself. This can occur with
                // ldp_uop microops since access is spread over multiple packets.
                DPRINTF(HtmCpu,
                        "%s writeback with HTM failure fault, "
                        "however, completing packet is not aware of "
                        "transaction failure. cause=%s htmUid=%u\n",
                        inst->staticInst->getName(),
                        htmFailureToStr(htm_fault->getHtmFailureFaultCause()),
                        htm_fault->getHtmUid());
            }

            DPRINTF(LSQUnit, "Not completing instruction [sn:%lli] access "
                    "due to pending fault.\n", inst->seqNum);
        }
    }

    // Need to insert instruction into queue to commit
    iewStage->instToCommit(inst);

    iewStage->activityThisCycle();

    // see if this load changed the PC
    iewStage->checkMisprediction(inst);
}

template <class Impl>
void
LSQUnit<Impl>::completeStore(typename StoreQueue::iterator store_idx)
{
    assert(store_idx->valid() && store_idx->dataAvail());
    store_idx->completed() = true;
    --storesToWB;
    // A bit conservative because a store completion may not free up entries,
    // but hopefully avoids two store completions in one cycle from making
    // the CPU tick twice.
    cpu->wakeCPU();
    cpu->activityThisCycle();

    /* We 'need' a copy here because we may clear the entry from the
     * store queue. */
    DynInstPtr store_inst = store_idx->instruction();
    if (store_idx == storeQueue.begin()) {
        do {
            storeQueue.front().clear();
            storeQueue.pop_front();
            --stores;
        } while (storeQueue.front().completed() &&
                 !storeQueue.empty());

        iewStage->updateLSQNextCycle = true;
    }

    DPRINTF(LSQUnit, "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            store_inst->seqNum, store_idx.idx() - 1, storeQueue.head() - 1);

#if TRACING_ON
    if (DTRACE(O3PipeView)) {
        store_inst->storeTick =
            curTick() - store_inst->fetchTick;
    }
#endif

    if (isStalled() &&
        store_inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%i\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }

    store_inst->setCompleted();

    if (needsTSO) {
        DSTATE(StoreComplete, store_inst);
        storeInFlight = false;
    }

    // Tell the checker we've completed this instruction.  Some stores
    // may get reported twice to the checker, but the checker can
    // handle that case.
    // Store conditionals cannot be sent to the checker yet, they have
    // to update the misc registers first which should take place
    // when they commit
    if (cpu->checker &&  !store_inst->isStoreConditional()) {
        cpu->checker->verify(store_inst);
    }
}

template <class Impl>
bool
LSQUnit<Impl>::trySendPacket(bool isLoad, PacketPtr data_pkt)
{
    bool ret = true;
    bool cache_got_blocked = false;

    auto state = dynamic_cast<LSQSenderState*>(data_pkt->senderState);

    if (!lsq->cacheBlocked() &&
        lsq->cachePortAvailable(isLoad)) {
        if (!dcachePort->sendTimingReq(data_pkt)) {
            ret = false;
            cache_got_blocked = true;
        }
    } else {
        ret = false;
    }

    if (ret) {
        if (!isLoad) {
            isStoreBlocked = false;
        }
        lsq->cachePortBusy(isLoad);
        state->outstanding++;
        state->request()->packetSent();
    } else {
        if (cache_got_blocked) {
            lsq->cacheBlocked(true);
            ++stats.blockedByCache;
        }
        if (!isLoad) {
            assert(state->request() == storeWBIt->request());
            isStoreBlocked = true;
        }
        state->request()->packetNotSent();
    }
    return ret;
}

template <class Impl>
bool
LSQUnit<Impl>::unlockBufferRetry() {
    bool allSent = true;
    std::vector<Addr> toErase;

    for (auto line : NSRUnlockBuffer) {
        bool notMemAddr = false;
        if (trySendNSR(line, false, notMemAddr)) {
            toErase.push_back(line);
        } else {
            allSent = false;
            assert(!notMemAddr);
            break;
        }
    }

    // clear those have been sent
    for (auto line : toErase) {
        NSRUnlockBuffer.erase(line);
    }

    return allSent;
}

template <class Impl>
bool
LSQUnit<Impl>::tryLockLQEntry(uint32_t idx, bool isPreLock) {
    auto entry = loadQueue.getIterator(idx);
    auto &inst = entry->instruction();
    bool allSent = true;

    CSPRINT(Lock, inst, "addr: %#x; prelock: %d\n", inst->physEffAddr, isPreLock);

    if (!entry->request()) {
        return false;
    }

    for (auto &r : entry->request()->_requests) {
        if (r->hasPaddr()) {
            Addr line = r->getPaddr() & cacheBlockMask;

            if (checkCLT(line)) {
                allSent = false;
                break;
            }

            if (IN_SET(line, YNLC_map)) {
                // already locked, just propagate
                CCSPRINT(NSR, LockedLinePropagation, inst, "; line: %#x\n", line);
                YNLC_map[line] = entry;
            } else if (IN_SET(line, NSRUnlockBuffer)) {
                // has pending unlock request, no need to send both
                CCSPRINT(NSR, UnlockBufferHit, inst, "; line: %#x\n", line);
                NSRUnlockBuffer.erase(line);
                YNLC_map[line] = entry;
            } else {
                // needs to send NSR Lock
                bool notMemAddr = false;
                CCSPRINT(NSR, LineLocking, inst, "; line: %#x\n", line);
                if (trySendNSR(line, true, notMemAddr, isPreLock)) {
                    // succeed
                    YNLC_map[line] = entry;
                } else {
                    allSent = false;
                    if (notMemAddr) {
                        // stop sending NSR for this instruction
                        // because it is not accessing
                        inst->setNotMemAddr();
                    }
                    break;
                }
            }
        } else {
            allSent = false;
            break;
        }
    }

    return allSent;
}

template <class Impl>
bool
LSQUnit<Impl>::tryUnlockLQEntry(uint32_t idx, bool addToRetry) {
    auto entry = loadQueue.getIterator(idx);
    auto &inst = entry->instruction();
    bool allSent = true;

    CSPRINT(Unlock, inst, "addr: %#x;\n", inst->physEffAddr);

    if (!entry->request()) {
        return false;
    }

    for (auto &r : entry->request()->_requests) {
        if (r->hasPaddr()) {
            Addr line = r->getPaddr() & cacheBlockMask;

            if (IN_MAP(line, YNLC_map) &&
                YNLC_map.at(line) == entry) {
                // the entry is the YNLC of line

                bool notMemAddr = false;
                if (trySendNSR(line, false, notMemAddr)) {
                    // succeed
                    YNLC_map.erase(line);
                } else {
                    allSent = false;
                    assert(!notMemAddr);
                    if (addToRetry) {
                        NSRUnlockBuffer.insert(line);
                        // because YNLC is going to commit soon, cannot keep it
                        YNLC_map.erase(line);
                    } else {
                        // set its instruction as unlock pending, before it retires
                        inst->setPendingUnlockReq();
                    }
                    break;
                }
            }
        } else {
            continue;
        }
    }

    return allSent;
}

template <class Impl>
bool
LSQUnit<Impl>::trySendNSR(Addr address, bool lock, bool &notMemAddr,
                          bool isPreLock) {
    bool ret;
    auto request = std::make_shared<Request>(
        address, 1, 0, Request::funcRequestorId);
    request->setContext(cpu->thread[lsqID]->contextId());

    Packet pkt(request, lock ? MemCmd::SetUnSquashable :
                               MemCmd::ClearUnSquashable);

    if (isPreLock) {
        pkt.setPreLock();
    }

    if (lsq->cachePortAvailable(true) &&
        dcachePort->sendTimingReq(&pkt)) {
        ret = true;
    } else {
        ret = false;
    }

    if (lock) {
        DPRINTF(NSR, "LSQUnit tries to lock %#x; succeed: %d\n", address, ret);
        if (ret) stats.NSRLockingReq++;
    } else {
        DPRINTF(NSR, "LSQUnit tries to unlock %#x; succeed: %d\n", address, ret);
        if (ret) stats.NSRUnlockingReq++;
    }

    if (!ret && pkt.notInMemAddr()) {
        notMemAddr = true;
    }

    return ret;
}

template <class Impl>
bool
LSQUnit<Impl>::markUnSquashables() {
    assert(GCONFIGS.threatModel == utils::Comprehensive);

    unlockBufferRetry();

    if (nsrPolicy == PARTITIONED && CSTBlocked) {
        return false;
    }

    auto storeCheck = [](LQIterator iter) {
        if (GCONFIGS.ISA == utils::X86) {
            uint32_t flags = iter->request()->mainRequest()->getFlags();
            bool storeCheck = flags &
                    (X86ISA::StoreCheck << X86ISA::FlagShift);
            return storeCheck;
        } else {
            return false;
        }
    };

    bool locked = true, hasPending = false;
    uint32_t locked_cnt = 0;
    for (auto iter = loadQueue.begin(); iter != loadQueue.end() && locked; iter++, locked_cnt++) {
        assert(iter->valid());
        DynInstPtr inst = iter->instruction();

        // ignore data prefetch and locked
        if (inst->isDataPrefetch() || inst->isUnSquashable() ||
            inst->isCommitted()) {
            locked = true;
            continue;
        }

        // start of locking condiction checking
        bool canLock = true;
        if (!inst->isReachedVP()) {
            canLock = false;
            stats.NSRUnlockVP += 1;
        }

        if (inst->isUnderExcept() || inst->getFault() != NoFault ||
            inst->hasMemViolation()) {
            canLock = false;
            stats.NSRUnlockException += 1;
            stats.NSRUnlockVP -= 1; // under exception implies not reaching VP
        }

        if (!inst->effAddrValid()) {
            canLock = false;
            stats.NSRUnlockAddr += 1;
            stats.NSRUnlockException -= 1; // effAddr not valid implies under exception
        }

        // check Store Queue space to avoid deadlock
        // it is an over-approximation of checking Store Buffer's room
        if (storeQueue.full() &&
            iter->instruction()->sqIt == storeQueue.getIterator(storeQueue.tail())) {
            canLock = false;
            stats.NSRUnlockSB += 1;
        }

        if (iter->hasRequest() &&
            iter->request()->mainRequest()->isLockedRMW()) {
            canLock = false;
            stats.NSRUnlockRMW += 1;
        }

        // policy dependent checking
        switch (nsrPolicy) {
            case STRICT:
                if (!inst->isDataReceived()) {
                    canLock = false;
                    stats.NSRUnlockDataRecv += 1;
                }
                break;

            case PARTITIONED:
                if (iter->hasRequest() && storeCheck(iter)) {
                    canLock = false;
                }

                if (checkEntryConflicts(iter.idx())) {
                    canLock = false;
                    stats.NSRUnlockConflicts += 1;
                    inst->setHasConflict();
                } else {
                    inst->resetHasConflict();
                }
                break;

            default:
                panic("Unknown NSR policy");
        }

        if (canLock) {
            switch (nsrPolicy) {
                case STRICT:
                    locked = tryLockLQEntry(iter.idx());
                    hasPending = hasPending || !locked;
                    break;

                case PARTITIONED:
                    locked = tryLockLQEntry(iter.idx(), true);
                    hasPending = hasPending || !locked;
                    break;

                default:
                    panic("Unknown NSR policy");
            }
        } else {
            locked = false;
        }

        if (locked) {
            if (!iter->isUnSquashable()) {
                iter->setUnSquashable();
                stats.unSquashables++;
            }
        }
    }

    stats.lockedLQEntryNum.sample(locked_cnt);

    return hasPending;
}

template <class Impl>
uint64_t
LSQUnit<Impl>::getL1DConflictID(Addr address, bool hash) const {
    assert(nsrPolicy == PARTITIONED);

    auto sid = addressToL1DCacheSet(address);
    if (!hash) {
        return sid;
    } else {
        return sid % GCONFIGS.L1DCSTEntryCnt;
    }
}

template <class Impl>
uint64_t
LSQUnit<Impl>::getL2ConflictID(Addr address, bool hash) const {
    assert(nsrPolicy == PARTITIONED);
    uint32_t set = addressToL2CacheSet(address);
    uint32_t slice = addressToLLCHaswell(address);
    uint64_t sid = ((uint64_t)slice << 32) + set;
    if (!hash) {
        return sid;
    } else {
        return (sid ^ (sid >> 8) ^ (sid >> 16)) % GCONFIGS.L2CSTEntryCnt;
    }
}

template <class Impl>
bool
LSQUnit<Impl>::checkEntryConflicts(uint32_t idx) {
    assert(nsrPolicy == PARTITIONED);
    bool conflict = false;
    auto entry = loadQueue.getIterator(idx);
    auto &inst = entry->instruction();

    if (!entry->request()) {
        return true;
    }

    for (auto &r : entry->request()->_requests) {
        if (r->hasPaddr()) {
            Addr line = r->getPaddr() & cacheBlockMask;
            if (checkConflicts(line, inst->seqNum)) {
                conflict = true;
                CSTBlocked = true;
                break;
            }
        } else {
            return true;
        }
    }

    if (DTRACE(ROBDump)) {
        inst->line_accessed = inst->physEffAddr & cacheBlockMask;
        inst->l1_set = addressToL1DCacheSet(inst->line_accessed);
        inst->l2_set = addressToL2CacheSet(inst->line_accessed);
        inst->l2_slice = addressToLLCHaswell(inst->line_accessed);
    }

    return conflict;
}

template <class Impl>
bool
LSQUnit<Impl>::checkConflicts(Addr line, InstSeqNum seqNum) {
    assert(nsrPolicy == PARTITIONED);
    bool useCAM = GCONFIGS.entryUseCAM;
    uint64_t l1dHash = getL1DConflictID(line, !useCAM), l2Hash = getL2ConflictID(line, !useCAM);
    uint64_t l1dSID = getL1DConflictID(line, false), l2SID = getL2ConflictID(line, false);
    stats.NSRConflictChecks += 1;

    bool l1dOverflow = false, l1dTagMiss = false, l1dHashCol = false;
    bool l1dNoConflict = L1DCST->check_and_insert(line, l1dHash, l1dSID, seqNum,
                                    l1dOverflow, l1dTagMiss, l1dHashCol);

    if (!l1dNoConflict) {
        stats.NSRL1Conflicts += 1;
    }

    if (l1dOverflow) {
        stats.L1DCSTOverflow += 1;
    } else {
        stats.L1DCSTEntries.sample(L1DCST->get_entry_num());
    }

    if (l1dTagMiss) {
        stats.L1DCSTTagMiss += 1;
        if (!l1dNoConflict) {
            stats.L1DCSTTagMissConf += 1;
        }
    }

    if (l1dHashCol) {
        stats.L1DCSTHashCollision += 1;
        if (!l1dNoConflict) {
            stats.L1DCSTHashColConf += 1;
        }
    }

    bool l2Overflow = false, l2TagMiss = false, l2HashCol = false;
    bool l2NoConflict = L2CST->check_and_insert(line, l2Hash, l2SID, seqNum,
                                    l2Overflow, l2TagMiss, l2HashCol);

    if (!l2NoConflict) {
        stats.NSRLLCConflicts += 1;
    }

    if (l2Overflow) {
        stats.L2CSTOverflow += 1;
    } else {
        stats.L2CSTEntries.sample(L2CST->get_entry_num());
    }

    if (l2TagMiss) {
        stats.L2CSTTagMiss += 1;
        if (!l2NoConflict) {
            stats.L2CSTTagMissConf += 1;
        }
    }

    if (l2HashCol) {
        stats.L2CSTHashCollision += 1;
        if (!l2NoConflict) {
            stats.L2CSTHashColConf += 1;
        }
    }

    if (!(l1dNoConflict && l2NoConflict)) {
        stats.NSRConflicts += 1;
        return true; // has conflicts
    } else {
        return false;
    }
}

template <class Impl>
void
LSQUnit<Impl>::removeEntryConflicts(uint32_t idx) {
    assert(nsrPolicy == PARTITIONED);
    auto entry = loadQueue.getIterator(idx);
    auto &inst = entry->instruction();

    if (!entry->request()) {
        return;
    }

    CSTBlocked = false;
    bool useCAM = GCONFIGS.entryUseCAM;
    for (auto &r : entry->request()->_requests) {
        if (r->hasPaddr()) {
            Addr line = r->getPaddr() & cacheBlockMask;
            L1DCST->commit_or_squash(line, getL1DConflictID(line, !useCAM), inst->seqNum);
            L2CST->commit_or_squash(line, getL2ConflictID(line, !useCAM), inst->seqNum);
        }
    }
    return;
}

template <class Impl>
void
LSQUnit<Impl>::issueEagerTranslated() {
    vector<int16_t> toErase;
    for (auto &p : eagerTransBuffer) {
        auto idx = p.first;
        auto waitingList = p.second;

        auto &inst = storeQueue[idx].instruction();
        if (!inst) {
            // the entry is squashed
            toErase.push_back(idx);
            continue;
        }
        assert(inst->isEagerTranslated());
        if (inst->numSrcRegs() == inst->readyRegs || inst->isSquashed()) {
            if (!inst->isSquashed()) {
                inst->setStoreData();
                assert(storeQueue[idx].dataAvail());
                for (auto &load_inst : *waitingList) {
                    if (!load_inst->isSquashed() && load_inst->lqIt->request()) {
                        read(load_inst->lqIt->request(), load_inst->lqIdx);
                    }
                }
            }
            toErase.push_back(idx);
        }
    }

    for (auto &i : toErase) {
        eagerTransBuffer.erase(i);
    }
}

template <class Impl>
Fault
LSQUnit<Impl>::setMemData(const DynInstPtr& inst, uint8_t *data) {
    auto idx = inst->sqIdx;
    auto size = storeQueue[idx].size();
    LSQRequest *req = storeQueue[idx].request();

    assert((!(req->request()->getFlags() & Request::CACHE_BLOCK_ZERO) &&
            !req->request()->isCacheMaintenance() &&
            !req->request()->isAtomic()));

    memcpy(storeQueue[idx].data(), data, size);
    req->resetEagerTranslated();
    storeQueue[idx].setDataAvail();

    return NoFault;
}

template <class Impl>
void
LSQUnit<Impl>::recvRetry()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: blocked store\n");
        writebackBlockedStore();
    }
}

template <class Impl>
void
LSQUnit<Impl>::dumpInsts() const
{
    cprintf("Load store queue: Dumping instructions.\n");
    cprintf("Load queue size: %i\n", loads);
    cprintf("Load queue: ");

    for (const auto& e: loadQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", stores);
    cprintf("Store queue: ");

    for (const auto& e: storeQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }

    cprintf("\n");
}

template <class Impl>
unsigned int
LSQUnit<Impl>::cacheLineSize()
{
    return cpu->cacheLineSize();
}

#endif//__CPU_O3_LSQ_UNIT_IMPL_HH__
