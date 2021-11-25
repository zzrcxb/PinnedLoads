# Copyright (c) 2012, 2017-2018 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from __future__ import print_function
from __future__ import absolute_import

import os

from m5 import fatal
import m5.objects
from m5.objects import *

def config_etrace(cpu_cls, cpu_list, options):
    if issubclass(cpu_cls, m5.objects.DerivO3CPU):
        # Assign the same file name to all cpus for now. This must be
        # revisited when creating elastic traces for multi processor systems.
        for cpu in cpu_list:
            # Attach the elastic trace probe listener. Set the protobuf trace
            # file names. Set the dependency window size equal to the cpu it
            # is attached to.
            cpu.traceListener = m5.objects.ElasticTrace(
                                instFetchTraceFile = options.inst_trace_file,
                                dataDepTraceFile = options.data_trace_file,
                                depWindowSize = 3 * cpu.numROBEntries)
            # Make the number of entries in the ROB, LQ and SQ very
            # large so that there are no stalls due to resource
            # limitation as such stalls will get captured in the trace
            # as compute delay. For replay, ROB, LQ and SQ sizes are
            # modelled in the Trace CPU.
            cpu.numROBEntries = 512
            cpu.LQEntries = 128
            cpu.SQEntries = 128
    else:
        fatal("%s does not support data dependency tracing. Use a CPU model of"
              " type or inherited from DerivO3CPU.", cpu_cls)


def config_O3CPU(cpu_cls, cpu_list, options):
    if not issubclass(cpu_cls, m5.objects.DerivO3CPU):
        return

    for cpu in cpu_list:
        cpu.maxInsts = options.maxinsts if options.maxinsts else 0
        cpu.maxTicks = options.rel_max_tick if options.rel_max_tick else 0
        cpu.HWName = options.hw
        cpu.threatModelName = options.threat_model
        cpu.needsTSO = options.needsTSO
        cpu.delayInvAck = options.delay_inv_ack
        cpu.delayWB = options.delay_wb
        if cpu.delayInvAck:
            assert(cpu.needsTSO)

        cpu.L1DSize  = MemorySize(options.l1d_size)
        cpu.L1DAssoc = options.l1d_assoc
        cpu.L2Size   = MemorySize(options.l2_size)
        cpu.L2Assoc  = options.l2_assoc
        cpu.numL2    = options.num_l2caches
        cpu.L2VPartition     = options.l2_vpartition
        cpu.eagerTranslation = options.eager_translation
        cpu.cachelineSize    = options.cacheline_size

        def parse_cst_config(cs):
            entry, record = cs.replace('x', 'X').split('X')
            return int(entry), int(record)

        cpu.L1DCSTEntryCnt, cpu.L1DCSTRecordCnt = parse_cst_config(options.l1d_cst)
        cpu.L2CSTEntryCnt, cpu.L2CSTRecordCnt = parse_cst_config(options.l2_cst)
        cpu.entryUseCAM = options.entry_cam
        cpu.CLTSize = options.clt_size
        cpu.CSTRecord = options.cst_record

        cpu.collectPerfStats = options.perf_counter
        cpu.lowerSeqNum = options.dstate_start
        cpu.upperSeqNum = options.dstate_end
        cpu.specBreakdown = options.spec_breakdown
