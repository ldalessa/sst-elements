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

#ifndef _H_VANADIS_SYSCALL_CHECKPOINT
#define _H_VANADIS_SYSCALL_CHECKPOINT

#include "os/voscallev.h"
#include "os/vosbittype.h"

namespace SST {
namespace Vanadis {

class VanadisSyscallCheckpointEvent : public VanadisSyscallEvent {
public:
    VanadisSyscallCheckpointEvent() : VanadisSyscallEvent() {}
    VanadisSyscallCheckpointEvent(uint32_t core, uint32_t thr, VanadisOSBitType bittype)
        : VanadisSyscallEvent(core, thr, bittype) {}

    VanadisSyscallOp getOperation() override { return SYSCALL_OP_CHECKPOINT; }

private:

    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        VanadisSyscallEvent::serialize_order(ser);
    }
    ImplementSerializable(SST::Vanadis::VanadisSyscallCheckpointEvent);
};

} // namespace Vanadis
} // namespace SST

#endif
