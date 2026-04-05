#ifndef _SERIAL_COMMUNICATION_H_
#define _SERIAL_COMMUNICATION_H_

#include <Arduino.h>
#include <string.h>
#include "common.h"
#include "crc.h"
#include "cobs.h"

// ============================================================================
// Header + Length + Payload + Check16
// Packet format:
//   HEADER1 | HEADER2 | LENGTH | PAYLOAD | CHECK_LO | CHECK_HI
//
// LENGTH is payload length only, not including header/length/check bytes.
// ============================================================================
template <uint16_t MaxPayloadLength,
          IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0>
class SerialPacketTransfer
{
public:
    static constexpr uint16_t max_payload_length = MaxPayloadLength;
    static constexpr uint16_t check_length = 2;
    static constexpr IntegrityMode integrity_mode = Mode;

    SerialPacketTransfer() = default;

    inline void reset_rx()
    {
        rx_state = RxState::WAIT_HEADER_1;
        rx_length = 0;
        rx_index = 0;
        rx_check_rx = 0;
        integrity_reset<Mode>(rx_check_state);
        rx_last_byte_time = 0;
    }

    inline void begin_packet()
    {
        tx_length = 0;
    }

    template <typename T>
    inline bool add_to_packet(const T& value)
    {
        return add_to_packet(&value, static_cast<uint16_t>(sizeof(T)));
    }

    inline bool add_to_packet(const void* data, uint16_t length)
    {
        if (data == nullptr) return false;
        if (tx_length + length > MaxPayloadLength) return false;

        memcpy(tx_payload + tx_length, data, length);
        tx_length = static_cast<uint16_t>(tx_length + length);
        return true;
    }

    inline uint16_t get_payload_length() const
    {
        return tx_length;
    }

    inline const uint8_t* get_tx_payload() const
    {
        return tx_payload;
    }

    inline uint8_t* get_received_payload()
    {
        return rx_payload;
    }

    inline const uint8_t* get_received_payload() const
    {
        return rx_payload;
    }

    inline uint16_t get_received_length() const
    {
        return rx_length;
    }

    inline void send_packet(HardwareSerial& serial)
    {
        if (tx_length > MaxPayloadLength) return;

        const uint16_t check = compute_integrity16<Mode>(tx_payload, tx_length);

        uint8_t header[3];
        header[0] = PACKET_HEADER_1;
        header[1] = PACKET_HEADER_2;
        header[2] = static_cast<uint8_t>(tx_length);

        uint8_t check_bytes[2];
        check_bytes[0] = static_cast<uint8_t>(check & 0xFF);
        check_bytes[1] = static_cast<uint8_t>((check >> 8) & 0xFF);

        serial.write(header, 3);
        if (tx_length > 0)
            serial.write(tx_payload, tx_length);
        serial.write(check_bytes, 2);
    }

    inline bool poll(HardwareSerial& serial)
    {
        return poll_with_budget(serial, 0);
    }

    inline bool poll_with_budget(HardwareSerial& serial, uint16_t maxBytesToProcess)
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
                if (rx_last_byte_time != 0 &&
                    static_cast<uint32_t>(now - rx_last_byte_time) > TimeoutMicros)
                {
                    reset_rx();
                }
                rx_last_byte_time = now;
            }

            switch (rx_state)
            {
                case RxState::WAIT_HEADER_1:
                    if (data == PACKET_HEADER_1)
                        rx_state = RxState::WAIT_HEADER_2;
                    break;

                case RxState::WAIT_HEADER_2:
                    if (data == PACKET_HEADER_2)
                    {
                        rx_state = RxState::WAIT_LENGTH;
                    }
                    else
                    {
                        rx_state = (data == PACKET_HEADER_1)
                                  ? RxState::WAIT_HEADER_2
                                  : RxState::WAIT_HEADER_1;
                    }
                    break;

                case RxState::WAIT_LENGTH:
                    rx_length = data;
                    rx_index = 0;
                    rx_check_rx = 0;
                    integrity_reset<Mode>(rx_check_state);

                    if (rx_length > MaxPayloadLength)
                    {
                        reset_rx();
                        break;
                    }

                    if (rx_length == 0)
                        rx_state = RxState::WAIT_CHECK_1;
                    else
                        rx_state = RxState::WAIT_PAYLOAD;
                    break;

                case RxState::WAIT_PAYLOAD:
                    rx_payload[rx_index++] = data;
                    integrity_update<Mode>(rx_check_state, data);

                    if (rx_index >= rx_length)
                        rx_state = RxState::WAIT_CHECK_1;
                    break;

                case RxState::WAIT_CHECK_1:
                    rx_check_rx = data;
                    rx_state = RxState::WAIT_CHECK_2;
                    break;

                case RxState::WAIT_CHECK_2:
                {
                    rx_check_rx |= static_cast<uint16_t>(data) << 8;
                    const uint16_t expected = integrity_value<Mode>(rx_check_state);
                    rx_state = RxState::WAIT_HEADER_1;

                    if (rx_check_rx == expected)
                        return true;

                    break;
                }

                default:
                    reset_rx();
                    break;
            }
        }

        return false;
    }

private:
    uint8_t tx_payload[MaxPayloadLength] = {0};
    uint16_t tx_length = 0;

    uint8_t rx_payload[MaxPayloadLength] = {0};
    uint16_t rx_length = 0;
    uint16_t rx_index = 0;
    uint16_t rx_check_rx = 0;
    IntegrityState<Mode> rx_check_state{};
    uint32_t rx_last_byte_time = 0;
    RxState rx_state = RxState::WAIT_HEADER_1;
};

// ============================================================================
// COBS variable-length packets
// Wire format:
//   COBS( PAYLOAD | CHECK16 ) | 0x00
//
// No explicit header or length.
// Packet end is 0x00 delimiter.
// ============================================================================
template <uint16_t MaxPayloadLength,
          IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0>
class SerialPacketTransferCOBS
{
public:
    static constexpr uint16_t max_payload_length = MaxPayloadLength;
    static constexpr uint16_t check_length = 2;

    // Worst-case COBS encoded size for N bytes is N + ceil(N / 254) + 1.
    // Here N = payload + 2 check bytes.
    static constexpr uint16_t raw_frame_max = static_cast<uint16_t>(MaxPayloadLength + check_length);
    static constexpr uint16_t cobs_encoded_max = static_cast<uint16_t>(raw_frame_max + ((raw_frame_max + 253) / 254));

    SerialPacketTransferCOBS() = default;

    inline void reset_rx()
    {
        rx_encoded_length = 0;
        rx_length = 0;
        rx_last_byte_time = 0;
        rx_discard_until_delimiter = false;
    }

    inline void begin_packet()
    {
        tx_length = 0;
    }

    template <typename T>
    inline bool add_to_packet(const T& value)
    {
        return add_to_packet(&value, static_cast<uint16_t>(sizeof(T)));
    }

    inline bool add_to_packet(const void* data, uint16_t length)
    {
        if (data == nullptr) return false;
        if (tx_length + length > MaxPayloadLength) return false;

        memcpy(tx_payload + tx_length, data, length);
        tx_length = static_cast<uint16_t>(tx_length + length);
        return true;
    }

    inline uint16_t get_payload_length() const
    {
        return tx_length;
    }

    inline const uint8_t* get_tx_payload() const
    {
        return tx_payload;
    }

    inline uint8_t* get_received_payload()
    {
        return rx_payload;
    }

    inline const uint8_t* get_received_payload() const
    {
        return rx_payload;
    }

    inline uint16_t get_received_length() const
    {
        return rx_length;
    }

    inline void send_packet(HardwareSerial& serial)
    {
        if (tx_length > MaxPayloadLength) return;

        const uint16_t check = compute_integrity16<Mode>(tx_payload, tx_length);

        uint8_t raw[raw_frame_max];
        if (tx_length > 0)
            memcpy(raw, tx_payload, tx_length);

        raw[tx_length] = static_cast<uint8_t>(check & 0xFF);
        raw[tx_length + 1] = static_cast<uint8_t>((check >> 8) & 0xFF);

        uint8_t encoded[cobs_encoded_max];
        const uint16_t encoded_length = cobs_encode(raw, static_cast<uint16_t>(tx_length + 2), encoded);

        serial.write(encoded, encoded_length);
        serial.write(static_cast<uint8_t>(0));
    }

    inline bool poll(HardwareSerial& serial)
    {
        return poll_with_budget(serial, 0);
    }

    inline bool poll_with_budget(HardwareSerial& serial, uint16_t maxBytesToProcess)
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
                if (rx_last_byte_time != 0 &&
                    static_cast<uint32_t>(now - rx_last_byte_time) > TimeoutMicros)
                {
                    reset_rx();
                }
                rx_last_byte_time = now;
            }

            if (rx_discard_until_delimiter)
            {
                if (data == 0)
                    rx_discard_until_delimiter = false;
                continue;
            }

            if (data == 0)
            {
                if (rx_encoded_length == 0)
                    continue; // ignore empty frame delimiters

                const uint16_t decoded_length = cobs_decode(rx_encoded_buffer, rx_encoded_length, rx_decoded_buffer);

                rx_encoded_length = 0;

                if (decoded_length < 2)
                    continue;

                const uint16_t payload_length = static_cast<uint16_t>(decoded_length - 2);
                if (payload_length > MaxPayloadLength)
                    continue;

                uint16_t rx_check = rx_decoded_buffer[payload_length];
                rx_check |= static_cast<uint16_t>(rx_decoded_buffer[payload_length + 1]) << 8;

                const uint16_t expected = compute_integrity16<Mode>(rx_decoded_buffer, payload_length);

                if (rx_check != expected)
                    continue;

                rx_length = payload_length;
                if (payload_length > 0)
                    memcpy(rx_payload, rx_decoded_buffer, payload_length);

                return true;
            }

            if (rx_encoded_length >= cobs_encoded_max)
            {
                rx_encoded_length = 0;
                rx_discard_until_delimiter = true;
                continue;
            }

            rx_encoded_buffer[rx_encoded_length++] = data;
        }

        return false;
    }

private:
    uint8_t tx_payload[MaxPayloadLength] = {0};
    uint16_t tx_length = 0;

    uint8_t rx_payload[MaxPayloadLength] = {0};
    uint16_t rx_length = 0;

    uint8_t rx_encoded_buffer[cobs_encoded_max] = {0};
    uint8_t rx_decoded_buffer[raw_frame_max] = {0};
    uint16_t rx_encoded_length = 0;
    uint32_t rx_last_byte_time = 0;
    bool rx_discard_until_delimiter = false;
};

// ============================================================================
// Optional typed helpers for header-length mode
// ============================================================================
template <uint16_t MaxPayloadLength,
          IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0,
          typename... T>
inline bool send_var_packet(HardwareSerial& serial, const T&... args)
{
    constexpr uint16_t payloadSize = (sizeof(T) + ...);
    static_assert(payloadSize <= MaxPayloadLength, "payload exceeds MaxPayloadLength");

    SerialPacketTransfer<MaxPayloadLength, Mode, TimeoutMicros> tx;
    tx.begin_packet();
    (tx.add_to_packet(args) && ...);
    tx.send_packet(serial);
    return true;
}

template <uint16_t MaxPayloadLength,
          IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0,
          typename... T>
inline bool receive_var_packet(
    HardwareSerial& serial,
    SerialPacketTransfer<MaxPayloadLength, Mode, TimeoutMicros>& rx,
    T&... args)
{
    constexpr uint16_t payloadSize = (sizeof(T) + ...);

    if (!rx.poll(serial))
        return false;

    if (rx.received_length() != payloadSize)
        return false;

    const uint8_t* ptr = rx.received_payload();
    serial_unpack_data(ptr, args...);
    return true;
}

// ============================================================================
// Optional typed helpers for COBS mode
// ============================================================================
template <uint16_t MaxPayloadLength,
          IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0,
          typename... T>
inline bool send_cobs_packet(HardwareSerial& serial, const T&... args)
{
    constexpr uint16_t payloadSize = (sizeof(T) + ...);
    static_assert(payloadSize <= MaxPayloadLength, "payload exceeds MaxPayloadLength");

    SerialPacketTransferCOBS<MaxPayloadLength, Mode, TimeoutMicros> tx;
    tx.begin_packet();
    (tx.add_to_packet(args) && ...);
    tx.send_packet(serial);
    return true;
}

template <uint16_t MaxPayloadLength,
          IntegrityMode Mode = IntegrityMode::CRC16_TABLE,
          uint32_t TimeoutMicros = 0,
          typename... T>
inline bool receive_cobs_packet(
    HardwareSerial& serial,
    SerialPacketTransferCOBS<MaxPayloadLength, Mode, TimeoutMicros>& rx,
    T&... args)
{
    constexpr uint16_t payloadSize = (sizeof(T) + ...);

    if (!rx.poll(serial))
        return false;

    if (rx.received_length() != payloadSize)
        return false;

    const uint8_t* ptr = rx.received_payload();
    serial_unpack_data(ptr, args...);
    return true;
}

#endif