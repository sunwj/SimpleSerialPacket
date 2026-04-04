#ifndef _SIMPLE_SERIAL_COMMUNICATION_H_
#define _SIMPLE_SERIAL_COMMUNICATION_H_

#include <Arduino.h>
#include <string.h>
#include "common.h"
#include "crc.h"

// ============================================================================
// Receiver
// PayloadSize is fixed at compile time.
// Mode is compile-time selected; zero runtime overhead.
// TimeoutMicros = 0 disables timeout reset.
// ============================================================================
template <uint16_t PayloadSize,
          IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0>
class FixedPacketReceiver
{
public:
    static constexpr uint16_t payload_size = PayloadSize;
    static constexpr IntegrityMode integrity_mode = Mode;

    FixedPacketReceiver()
    {
        reset();
    }

    inline void reset()
    {
        state = RxState::WAIT_HEADER_1;
        index = 0;
        rx_check = 0;
        integrity_reset<Mode>(check_state);
        last_byte_time = 0;
    }

    inline RxState state() const { return state; }
    inline uint16_t bytes_received() const { return index; }

    inline bool poll(HardwareSerial& serial, uint8_t* payload)
    {
        return poll_with_budget(serial, payload, 0);
    }

    // maxBytesToProcess = 0 means "process everything available"
    inline bool poll_with_budget(HardwareSerial& serial, uint8_t* payload, uint16_t maxBytesToProcess)
    {
        uint16_t processed = 0;

        while (serial.available() > 0)
        {
            if (maxBytesToProcess != 0 && processed >= maxBytesToProcess)
                break;

            const uint8_t data = static_cast<uint8_t>(serial.read());
            ++processed;

            if constexpr (TimeoutMicros > 0)
            {
                const uint32_t now = micros();
                if (last_byte_time != 0 && static_cast<uint32_t>(now - last_byte_time) > TimeoutMicros)
                {
                    reset();
                }
                last_byte_time = now;
            }

            switch (state)
            {
                case RxState::WAIT_HEADER_1:
                    if (data == SERIAL_PACKET_HEADER_1)
                        state = RxState::WAIT_HEADER_2;
                    break;

                case RxState::WAIT_HEADER_2:
                    if (data == SERIAL_PACKET_HEADER_2)
                    {
                        state = RxState::WAIT_PAYLOAD;
                        index = 0;
                        rx_check = 0;
                        integrity_reset<Mode>(check_state);
                    }
                    else
                    {
                        // Faster resync when headers overlap, e.g. 55 55 AA
                        state = (data == SERIAL_PACKET_HEADER_1)
                               ? RxState::WAIT_HEADER_2
                               : RxState::WAIT_HEADER_1;
                    }
                    break;

                case RxState::WAIT_PAYLOAD:
                    payload[index_++] = data;
                    integrity_update<Mode>(check_state, data);

                    if (index == PayloadSize)
                        state = RxState::WAIT_CHECK_1;
                    break;

                case RxState::WAIT_CHECK_1:
                    rx_check = data;
                    state = RxState::WAIT_CHECK_2;
                    break;

                case RxState::WAIT_CHECK_2:
                {
                    rx_check |= static_cast<uint16_t>(data) << 8;
                    const uint16_t expected = integrity_value<Mode>(check_state);
                    state = RxState::WAIT_HEADER_1;

                    if (rx_check == expected)
                        return true;

                    break;
                }

                default:
                    reset();
                    break;
            }
        }

        return false;
    }

private:
    RxState state;
    uint16_t index;
    uint16_t rx_check;
    IntegrityState<Mode> check_state;
    uint32_t last_byte_time;
};

// ============================================================================
// TX helpers
// One contiguous write for lower overhead.
// ============================================================================
template <IntegrityMode Mode = IntegrityMode::CRC16_TABLE, typename... T>
inline void send_packet_var_args(HardwareSerial& serial, const T&... args)
{
    constexpr uint16_t payloadSize = (sizeof(T) + ...);
    constexpr uint16_t packetSize = 2 + payloadSize + 2;

    uint8_t packet[packetSize];
    packet[0] = PACKET_HEADER_1;
    packet[1] = PACKET_HEADER_2;

    uint8_t* ptr = &packet[2];
    packData(ptr, args...);

    const uint16_t check = compute_integrity16<Mode>(&packet[2], payloadSize);
    packet[2 + payloadSize]     = static_cast<uint8_t>(check & 0xFF);
    packet[2 + payloadSize + 1] = static_cast<uint8_t>((check >> 8) & 0xFF);

    serial.write(packet, packetSize);
}

template <IntegrityMode Mode = IntegrityMode::CRC16_TABLE>
inline void send_packet_buffer(HardwareSerial& serial, const uint8_t* payload, uint16_t length)
{
    uint8_t headers[2] = { PACKET_HEADER_1, PACKET_HEADER_2 };
    const uint16_t check = compute_integrity16<Mode>(payload, length);
    uint8_t checkBytes[2] = {
        static_cast<uint8_t>(check & 0xFF),
        static_cast<uint8_t>((check >> 8) & 0xFF)
    };

    serial.write(headers, 2);
    serial.write(payload, length);
    serial.write(checkBytes, 2);
}

// Compile-time-length version: fastest path, one write
template <uint16_t PayloadSize, IntegrityMode Mode = IntegrityMode::CRC16_TABLE>
inline void send_packet_buffer(HardwareSerial& serial, const uint8_t (&payload)[PayloadSize])
{
    constexpr uint16_t packetSize = 2 + PayloadSize + 2;
    uint8_t packet[packetSize];

    packet[0] = SERIAL_PACKET_HEADER_1;
    packet[1] = SERIAL_PACKET_HEADER_2;
    memcpy(&packet[2], payload, PayloadSize);

    const uint16_t check = compute_integrity16<Mode>(&packet[2], PayloadSize);
    packet[2 + PayloadSize] = static_cast<uint8_t>(check & 0xFF);
    packet[2 + PayloadSize + 1] = static_cast<uint8_t>((check >> 8) & 0xFF);

    serial.write(packet, packetSize);
}

// ============================================================================
// RX wrappers
// ============================================================================
template <uint16_t PayloadSize,
          IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0>
inline bool receive_packet_buffer(
    HardwareSerial& serial,
    FixedPacketReceiver<PayloadSize, Mode, TimeoutMicros>& receiver,
    uint8_t* payload)
{
    return receiver.poll(serial, payload);
}

template <uint16_t PayloadSize,
          IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0>
inline bool receive_packet_buffer_with_budget(
    HardwareSerial& serial,
    FixedPacketReceiver<PayloadSize, Mode, TimeoutMicros>& receiver,
    uint8_t* payload,
    uint16_t maxBytesToProcess)
{
    return receiver.poll_with_budget(serial, payload, maxBytesToProcess);
}

template <IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0,
          typename... T>
inline bool receive_packet_var_args(
    HardwareSerial& serial,
    FixedPacketReceiver<(sizeof(T) + ...), Mode, TimeoutMicros>& receiver,
    T&... args)
{
    constexpr uint16_t payloadSize = (sizeof(T) + ...);
    uint8_t payload[payloadSize];

    if (!receiver.poll(serial, payload))
        return false;

    const uint8_t* ptr = payload;
    unpackData(ptr, args...);
    return true;
}

template <IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0,
          typename... T>
inline bool receive_packet_var_args_with_budget(
    HardwareSerial& serial,
    FixedPacketReceiver<(sizeof(T) + ...), Mode, TimeoutMicros>& receiver,
    uint16_t maxBytesToProcess,
    T&... args)
{
    constexpr uint16_t payloadSize = (sizeof(T) + ...);
    uint8_t payload[payloadSize];

    if (!receiver.poll_with_budget(serial, payload, maxBytesToProcess))
        return false;

    const uint8_t* ptr = payload;
    unpackData(ptr, args...);
    return true;
}

#endif