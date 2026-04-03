#ifndef _SERIAL_COMMUNICATION_H_
#define _SERIAL_COMMUNICATION_H_

#include <Arduino.h>
#include "cobs.h"

#ifndef _HEADER_CONST_
#define _HEADER_CONST_
static constexpr uint8_t HEADER = 0x55;
#endif
static constexpr uint16_t maxDataLength = 64;
static constexpr uint16_t maxCOBSBufferLength = maxDataLength + 3;          // 1 id, 1 cobs overhead, 1 checksum
static constexpr uint16_t maxPacketBufferLength = maxDataLength + 2;        // 1 id, 1 checksum

// one instance can only be used for either send or receive
class SerialPacketTransferCOBS
{
    public:
    SerialPacketTransferCOBS(HardwareSerial& _serial): serial(_serial)
    {
        endDataPtr = packetBuffer + 1 + maxDataLength;                // point to checksum byte
        endCOBSPtr = packetBuffer + maxCOBSBufferLength;
        dataPtr = packetBuffer;                                       // for receive packet usage
    }

    inline void begin_packet(uint8_t id)
    {
        packetLength = 0;
        dataPtr = packetBuffer;
        *dataPtr++ = id;
    }

    template <typename T>
    inline bool add_to_packet(const T& data)
    {
        return add_to_packet(&data, sizeof(T));
    }

    bool add_to_packet(const void* data, uint8_t length)
    {
        if (dataPtr + length > endDataPtr) return false;

        memcpy(dataPtr, data, length);
        dataPtr += length;

        return true;
    }

    uint16_t end_packet()
    {
        if (dataPtr == packetBuffer) return 0;
        
        uint8_t checksum = 0x55;
        for (uint8_t* ptr = packetBuffer; ptr < dataPtr; ++ptr) checksum ^= *ptr;
        *dataPtr++ = checksum;

        packetLength = static_cast<uint16_t>(dataPtr - packetBuffer);
        return packetLength;
    }

    void send_packet()
    {
        if (packetLength < 2) return;

        uint8_t codingBuffer[maxCOBSBufferLength];
        uint16_t encodedLength = cobs_encode(packetBuffer, packetLength, codingBuffer);
        serial.write(codingBuffer, encodedLength);
        serial.write(uint8_t(0));
    }

    uint16_t receive_packet()
    {
        while (serial.available())
        {
            uint8_t data = serial.read();
            if (data == 0)
            {
                uint16_t decodeLength = cobs_decode(packetBuffer, static_cast<uint16_t>(dataPtr - packetBuffer));
                dataPtr = packetBuffer;
                if (decodeLength < 2) return 0;

                uint8_t checksum = 0x55;
                for (uint16_t i = 0; i < decodeLength - 1; ++i) checksum ^= packetBuffer[i];
                if (checksum != packetBuffer[decodeLength - 1]) return 0;

                return decodeLength;
            }

            if (dataPtr == endCOBSPtr)
            {
                dataPtr = packetBuffer;
                return 0;
            }
            *dataPtr++ = data;
        }
        return 0;
    }

    uint8_t get_id() const {return packetBuffer[0];}
    uint8_t* get_data() const {return (uint8_t*)(packetBuffer + 1);}

    private:
    HardwareSerial& serial;
    uint8_t packetBuffer[maxCOBSBufferLength] = {0};

    uint8_t* dataPtr = nullptr;
    uint8_t* endDataPtr = nullptr;
    uint8_t* endCOBSPtr = nullptr;
    uint16_t packetLength = 0;
};


class SerialPacketTransfer
{
    public:
    SerialPacketTransfer(HardwareSerial& _serial): serial(_serial)
    {
        endDataPtr = packetBuffer + 1 + maxDataLength;                // point to checksum byte
        dataPtr = packetBuffer;                                       // for receive packet usage
    }

    inline void begin_packet(uint8_t id)
    {
        packetLength = 0;
        dataPtr = packetBuffer;
        *dataPtr++ = id;
    }

    template <typename T>
    inline bool add_to_packet(const T& data)
    {
        return add_to_packet(&data, sizeof(T));
    }

    bool add_to_packet(const void* data, uint8_t length)
    {
        if (dataPtr + length > endDataPtr) return false;

        memcpy(dataPtr, data, length);
        dataPtr += length;

        return true;
    }

    uint16_t end_packet()
    {
        if (dataPtr == packetBuffer) return 0;
        
        uint8_t checksum = 0x55;
        for (uint8_t* ptr = packetBuffer; ptr < dataPtr; ++ptr) checksum ^= *ptr;
        *dataPtr++ = checksum;

        packetLength = dataPtr - packetBuffer;
        return packetLength;
    }

    void send_packet()
    {
        if (packetLength < 2) return;

        serial.write(HEADER);
        serial.write(packetLength);
        serial.write(packetBuffer, packetLength);
    }

    uint16_t receive_packet()
    {
        while (serial.available())
        {
            uint8_t data = serial.read();
            switch (readLength)
            {
                case 0:
                    if (data == HEADER) ++readLength;
                    break;

                case 1:
                    packetLength = data;
                    if (packetLength < 2 || packetLength > maxPacketBufferLength)
                    {
                        dataPtr = packetBuffer;
                        packetLength = 0;
                        readLength = 0;
                        break;
                    }
                    ++readLength;
                    break;

                default:
                    *dataPtr++ = data;
                    ++readLength;

                    if (readLength == packetLength + 2)
                    {
                        dataPtr = packetBuffer;
                        readLength = 0;

                        uint8_t checkSum = 0x55;
                        for (uint16_t i = 0; i < packetLength - 1; ++i) checkSum ^= packetBuffer[i];
                        if (checkSum == packetBuffer[packetLength - 1]) return packetLength;
                        return 0;
                    }
                    break;
            }
        }
        return 0;
    }

    uint8_t get_id() const {return packetBuffer[0];}
    uint8_t* get_data() const {return (uint8_t*)(packetBuffer + 1);}

    private:
    HardwareSerial& serial;
    uint8_t packetBuffer[maxPacketBufferLength] = {0};

    uint8_t* dataPtr = nullptr;
    uint8_t* endDataPtr = nullptr;
    uint8_t packetLength = 0;
    uint8_t readLength = 0;
};

#endif