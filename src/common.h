#ifndef _COMMON_H_
#define _COMMON_H_
#include <Arduino.h>

#ifndef PACKET_HEADER_1
#define PACKET_HEADER_1 0x55
#endif

#ifndef PACKET_HEADER_2
#define PACKET_HEADER_2 0xAA
#endif

enum class RxState : uint8_t
{
    WAIT_HEADER_1,
    WAIT_HEADER_2,
    WAIT_LENGTH,
    WAIT_PAYLOAD,
    WAIT_CHECK_1,
    WAIT_CHECK_2
};

// ============================================================================
// Serialization helpers
// Note: memcpy-based packing is fast, but requires compatible sender/receiver
// type layout and endianness.
// ============================================================================
template <typename... T>
inline void packData(uint8_t*& buffer, const T&... args)
{
    ((memcpy(buffer, &args, sizeof(T)), buffer += sizeof(T)), ...);
}

template <typename... T>
inline void unpackData(const uint8_t*& buffer, T&... args)
{
    ((memcpy(&args, buffer, sizeof(T)), buffer += sizeof(T)), ...);
}

#endif