// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_HDNVP_FRAME_PROTOCOL_H
#define NATRON_HDNVP_FRAME_PROTOCOL_H
#include <cstdint>
#include <cstddef>
namespace NatronHdNvp {
// No C++ objects or SDK ABI cross the process boundary. A ready frame owns
// the mapping until the reader acknowledges it; cancellation is independent.
struct alignas(8) FrameHeader {
    uint32_t magic=0x4e485646,version=1;
    volatile int32_t cancelled=0,ready=0;
    uint64_t revision=0,pixels=0;
    uint32_t width=0,height=0;
    uint64_t reserved[3]{};
};
static_assert(sizeof(FrameHeader)==64);
constexpr size_t components[]{4,1,1,1,3,3};
inline size_t bytes(size_t pixels){return sizeof(FrameHeader)+pixels*13*sizeof(uint32_t);}
inline size_t offset(size_t aov,size_t pixels){size_t words=0;for(size_t i=0;i<aov;++i)words+=components[i];return sizeof(FrameHeader)+words*pixels*sizeof(uint32_t);}
}
#endif
