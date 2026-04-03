#ifndef _SIMPLE_SERIAL_COMMUNICATION_H_
#define _SIMPLE_SERIAL_COMMUNICATION_H_

#include <Arduino.h>
#include "crc.h"

#define HEADER_1 0x55
#define HEADER_2 0xAA

enum RxState: uint8_t
{
    WAIT_HEADER_1, WAIT_HEADER_2, WAIT_PAYLOAD, WAIT_CRC_1, WAIT_CRC_2
};

template <typename... T>
inline void packData(uint8_t*& buffer, const T&... args)
{
    ((memcpy(buffer, &args, sizeof(args)), buffer += sizeof(args)), ...);
}

template <typename... T>
void send_packet_var_args(HardwareSerial& serial, const T&... args)
{
    constexpr uint16_t payLoadSize = (sizeof(T) + ...);
    constexpr uint16_t packetSize = 2 + payLoadSize + 2;
    static uint8_t buffer[packetSize];
    buffer[0] = HEADER_1;
    buffer[1] = HEADER_2;
    uint8_t* ptr = &buffer[2];

    packData(ptr, args...);

    uint16_t crc16 = compute_crc16(&buffer[2], payLoadSize);
    buffer[packetSize - 2] = uint8_t(crc16 & 0x00FF);
    buffer[packetSize - 1] = uint8_t((crc16 >> 8) & 0x00FF);

    serial.write(buffer, packetSize);
}

template <typename... T>
inline void unpackData(const uint8_t*& buffer, T&... args)
{
    ((memcpy(&args, buffer, sizeof(args)), buffer += sizeof(args)), ...);
}

template <typename... T>
bool receive_packet_var_args(HardwareSerial& serial, T&... args)
{
    constexpr uint16_t payLoadSize = (sizeof(T) + ...);
    constexpr uint16_t packetSize = payLoadSize + 2;
    static uint8_t buffer[payLoadSize];
    static uint16_t idx = 0;
    static RxState state = WAIT_HEADER_1;
    static uint16_t crc16RX = 0;

    while (serial.available())
    {
        uint8_t data = serial.read();

        switch (state)
        {
            case WAIT_HEADER_1:
                if (HEADER_1 == data) state = WAIT_HEADER_2;
            break;

            case WAIT_HEADER_2:
            {
                if (HEADER_2 == data)
                {
                    state = WAIT_PAYLOAD;
                    idx = 0;
                }
                else state = WAIT_HEADER_1;
            }
            break;

            case WAIT_PAYLOAD:
            {
                buffer[idx++] = data;
                if (idx == payLoadSize) state = WAIT_CRC_1;
            }
            break;

            case WAIT_CRC_1:
            {
                crc16RX = data;
                state = WAIT_CRC_2;
            }
            break;

            case WAIT_CRC_2:
            {
                crc16RX |= uint16_t(data) << 8;
                if (crc16RX != compute_crc16(buffer, payLoadSize))
                {
                    state = WAIT_HEADER_1;
                    crc16RX = 0;

                    return false;
                }

                uint8_t* ptr = buffer;
                unpackData(ptr, args...);

                state = WAIT_HEADER_1;
                crc16RX = 0;

                return true;
            }
            break;

            default:
            break;
        }

        return false;
    }
}

void send_packet_buffer(HardwareSerial& serial, const uint8_t* buffer, uint16_t length)
{
    const uint8_t headers[2] = {HEADER_1, HEADER_2};
    uint16_t crc16 = compute_crc16(buffer, length);
    uint8_t crc[2];
    crc[0] = uint8_t(crc16 & 0x00FF);
    crc[1] = uint8_t((crc16 >> 8) & 0x00FF);
    
    serial.write(headers, 2);
    serial.write(buffer, length);
    serial.write(crc, 2);
}

bool receive_packet_buffer(HardwareSerial& serial, uint8_t* buffer, uint16_t length)
{
    static RxState state = WAIT_HEADER_1;
    static uint16_t crc16RX = 0;
    static uint16_t idx = 0;

    while (serial.available())
    {
        uint8_t data = serial.read();
        
        switch (state)
        {
            case WAIT_HEADER_1:
                if (HEADER_1 == data) state = WAIT_HEADER_2;
            break;

            case WAIT_HEADER_2:
            {
                if (HEADER_2 == data)
                {
                    state = WAIT_PAYLOAD;
                    idx = 0;
                }
                else state = WAIT_HEADER_1;
            }
            break;

            case WAIT_PAYLOAD:
            {
                buffer[idx++] = data;
                if (idx == length) state = WAIT_CRC_1;
            }
            break;

            case WAIT_CRC_1:
            {
                crc16RX = data;
                state = WAIT_CRC_2;
            }
            break;

            case WAIT_CRC_2:
            {
                crc16RX |= uint16_t(data) << 8;
                if (crc16RX != compute_crc16(buffer, length))
                {
                    state = WAIT_HEADER_1;
                    crc16RX = 0;

                    return false;
                }

                state = WAIT_HEADER_1;
                crc16RX = 0;

                return true;
            }
            break;

            default:
            break;
        }

        return false;
    }
}

#endif
