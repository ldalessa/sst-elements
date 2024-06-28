// Copyright 2009-2024 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2024, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef _H_VANADIS_DECODER
#define _H_VANADIS_DECODER

#include "datastruct/cqueue.h"
#include "decoder/visaopts.h"
#include "inst/fpregmode.h"
#include "inst/isatable.h"
#include "inst/vinst.h"
#include "lsq/vlsq.h"
#include "os/vcpuos.h"
#include "vbranch/vbranchbasic.h"
#include "vbranch/vbranchunit.h"
#include "velf/velfinfo.h"
#include "vinsloader.h"
#include "vfpflags.h"

#include <cinttypes>
#include <cstdint>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/subcomponent.h>

#define VANADIS_DECODER_ELI_STATISTICS                                                                \
    { "uop_cache_hit", "Count number of times the instruction micro-op cache is hit", "hits", 1 },    \
        { "predecode_cache_hit",                                                                      \
          "Count number of times the predecode cache is hit when decoding an "                        \
          "instruction",                                                                              \
          "hits", 1 },                                                                                \
        { "predecode_cache_miss",                                                                     \
          "Count number of times the predecode cache misses, this forces a load "                     \
          "from the instruction cache interface",                                                     \
          "misses", 1 },                                                                              \
        { "decode_faults",                                                                            \
          "Count number of times decode operation fails to generate valid "                           \
          "micro-ops",                                                                                \
          "uops", 1 },                                                                                \
        { "ins_bytes_loaded", "Count the number of bytes loaded for decode operations", "bytes", 1 }, \
        { "uop_delayed_rob_full", "Number of times a micro-op cannot be added to the ROB because it is full.", "cycles", 1 }, \
    {                                                                                                 \
        "uops_generated",                                                                             \
            "Count number of micro-ops generated by decoder that are transfered to "                  \
            "the pipeline for execution",                                                             \
            "uops", 1                                                                                 \
    }

namespace SST {
namespace Vanadis {

class VanadisDecoder : public SST::SubComponent
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Vanadis::VanadisDecoder)

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS({ "os_handler", "Handler for SYSCALL instructions",
                                          "SST::Vanadis::VanadisCPUOSHandler" },
                                        { "branch_unit", "Branch prediction unit", "SST::Vanadis::VanadisBranchUnit" })

    SST_ELI_DOCUMENT_PARAMS(
            //{ "decode_q_len", "Number of entries in the decoded, but pending issue queue" },
                            { "icache_line_width", "Number of bytes in an icache line", "64"},
                            { "uop_cache_entries",
                              "Number of instructions to cache in the micro-op cache (this is full "
                              "instructions, not microops but usually 1:1 ratio", "128" },
                            { "predecode_cache_entries",
                              "Number of cache lines to store in the local L0 cache for instructions "
                              "pending decoding.", "4" },
                            { "loader_mode",
                              "Operation of the loader, 0 = LRU (more accurate), 1 = INFINITE cache (faster simulation)", "0"})

    SST_ELI_DOCUMENT_STATISTICS( 
				VANADIS_DECODER_ELI_STATISTICS
				)

    VanadisDecoder(ComponentId_t id, Params& params) : SubComponent(id)
    {
        ip      = 0;
        tls_ptr = 0;

        thread_rob = nullptr;
		  fpflags = nullptr;

        icache_line_width = params.find<uint64_t>("icache_line_width", 64);

        const size_t uop_cache_size          = params.find<size_t>("uop_cache_entries", 128);
        const size_t predecode_cache_entries = params.find<size_t>("predecode_cache_entries", 4);

        ins_loader = new VanadisInstructionLoader(uop_cache_size, predecode_cache_entries, icache_line_width);

        const uint32_t loader_mode = params.find<uint32_t>("loader_mode", 0);
        switch(loader_mode) {
        case 0:
            ins_loader->setLoaderMode(VanadisInstructionLoaderMode::LRU_CACHE_MODE);
            break;
        case 1:
            ins_loader->setLoaderMode(VanadisInstructionLoaderMode::INFINITE_CACHE_MODE);
            break;
        default:
            ins_loader->setLoaderMode(VanadisInstructionLoaderMode::LRU_CACHE_MODE);
            break;
        }

        branch_predictor = loadUserSubComponent<SST::Vanadis::VanadisBranchUnit>("branch_unit");
        os_handler       = loadUserSubComponent<SST::Vanadis::VanadisCPUOSHandler>("os_handler");

        hw_thr = 0;

        os_handler->setThreadLocalStoragePointer(&tls_ptr);

        canIssueStores = true;
        canIssueLoads  = true;

        stat_uop_hit          = registerStatistic<uint64_t>("uop_cache_hit", "1");
        stat_predecode_hit    = registerStatistic<uint64_t>("predecode_cache_hit", "1");
        stat_predecode_miss   = registerStatistic<uint64_t>("predecode_cache_miss", "1");
        stat_uop_generated    = registerStatistic<uint64_t>("uops_generated", "1");
        stat_decode_fault     = registerStatistic<uint64_t>("decode_faults", "1");
        stat_ins_bytes_loaded = registerStatistic<uint64_t>("ins_bytes_loaded", "1");
        stat_uop_delayed_rob_full = registerStatistic<uint64_t>("uop_delayed_rob_full", "1");
    }

    virtual ~VanadisDecoder()
    {
        delete ins_loader;
        delete os_handler;
        delete branch_predictor;
    }

    virtual void markLoadFencing() { canIssueLoads = false; }

    virtual void markStoreFencing() { canIssueStores = false; }

    virtual void clearLoadFencing() { canIssueLoads = true; }

    virtual void clearStoreFencing() { canIssueStores = true; }

    virtual void clearFencing()
    {
        clearLoadFencing();
        clearStoreFencing();
    }

    virtual void markFencing()
    {
        markLoadFencing();
        markStoreFencing();
    }

    void setInsCacheLineWidth(const uint64_t ic_width)
    {
        icache_line_width = ic_width;
        ins_loader->setCacheLineWidth(ic_width);
    }

    void setFPFlags(VanadisFloatingPointFlags* new_fpflags) {
		fpflags = new_fpflags;
	 }

    bool acceptCacheResponse(SST::Output* output, SST::Interfaces::StandardMem::Request* req)
    {
        return ins_loader->acceptResponse(output, req);
    }

    uint64_t getInsCacheLineWidth() const { return icache_line_width; }

    virtual VanadisFPRegisterMode getFPRegisterMode() const = 0;

    virtual const char*                  getISAName() const                        = 0;
    virtual uint16_t                     countISAIntReg() const                    = 0;
    virtual uint16_t                     countISAFPReg() const                     = 0;
    virtual void                         tick(SST::Output* output, uint64_t cycle) = 0;
    virtual const VanadisDecoderOptions* getDecoderOptions() const                 = 0;

    uint64_t getInstructionPointer() const { return ip; }

    void setInstructionPointer(const uint64_t newIP)
    {
        ip = newIP;

        // Do we need to clear here or not?
    }

    virtual void setStackPointer( SST::Output* output, VanadisISATable* isa_tbl, VanadisRegisterFile* regFile, const uint64_t stack_start_address ) {assert(0);}
    virtual void setThreadPointer( SST::Output* output, VanadisISATable* isa_tbl, VanadisRegisterFile* regFile, const uint64_t stack_start_address ) {}
    virtual void setArg1Register( SST::Output* output, VanadisISATable* isa_tbl, VanadisRegisterFile* regFile, const uint64_t value ) {assert(0);}
    virtual void setFuncPointer( SST::Output* output, VanadisISATable* isa_tbl, VanadisRegisterFile* regFile, const uint64_t value ) {}
    virtual void setReturnRegister( SST::Output* output, VanadisISATable* isa_tbl, VanadisRegisterFile* regFile, const uint64_t value ) {assert(0);} 
    virtual void setSuccessRegister( SST::Output* output, VanadisISATable* isa_tbl, VanadisRegisterFile* regFile, const uint64_t value ) {}

    void setInstructionPointerAfterMisspeculate(SST::Output* output, const uint64_t newIP)
    {
        ip = newIP;

        output->verbose(CALL_INFO, 16, 0, "[decoder] -> clear decode-q and set new ip: 0x%" PRI_ADDR "\n", newIP);

        // Clear out the decode queue, need to restart
        // decoded_q->clear();

        clearDecoderAfterMisspeculate(output);
    }

    void setThreadLocalStoragePointer(uint64_t new_tls) { tls_ptr = new_tls; }

    uint64_t getThreadLocalStoragePointer() const { return tls_ptr; }

    // VanadisCircularQueue<VanadisInstruction*>* getDecodedQueue() { return
    // decoded_q; }

    virtual void setThreadROB(VanadisCircularQueue<VanadisInstruction*>* thr_rob) { thread_rob = thr_rob; }

    void     setCore(const uint32_t num ) { core = num; }
    uint32_t getCore() const { return core; }

    void     setHardwareThread(const uint32_t thr) { hw_thr = thr; }
    uint32_t getHardwareThread() const { return hw_thr; }

    VanadisInstructionLoader* getInstructionLoader() { return ins_loader; }
    VanadisBranchUnit*        getBranchPredictor() { return branch_predictor; }

    virtual VanadisCPUOSHandler* getOSHandler() { return os_handler; }

protected:
    virtual void clearDecoderAfterMisspeculate(SST::Output* output) {};

    uint64_t ip;
    uint64_t icache_line_width;
    uint32_t hw_thr;
    uint32_t core;

    uint64_t tls_ptr;

    bool                                       wantDelegatedLoad;
    VanadisCircularQueue<VanadisInstruction*>* thread_rob;

    // VanadisCircularQueue<VanadisInstruction*>* decoded_q;

    VanadisInstructionLoader* ins_loader;
    VanadisBranchUnit*        branch_predictor;
    VanadisCPUOSHandler*      os_handler;
	VanadisFloatingPointFlags* fpflags;

    bool canIssueStores;
    bool canIssueLoads;

    Statistic<uint64_t>* stat_uop_hit;
    Statistic<uint64_t>* stat_uop_delayed_rob_full;
    Statistic<uint64_t>* stat_predecode_hit;
    Statistic<uint64_t>* stat_predecode_miss;
    Statistic<uint64_t>* stat_decode_fault;
    Statistic<uint64_t>* stat_uop_generated;
    Statistic<uint64_t>* stat_ins_bytes_loaded;
};

} // namespace Vanadis
} // namespace SST

#endif
