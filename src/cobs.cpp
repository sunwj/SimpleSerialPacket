#include "cobs.h"
#include <memory.h>

uint16_t cobs_encode(const uint8_t* __restrict dataBuffer, uint16_t length, uint8_t* __restrict cobsBuffer)
{
    const uint8_t* endPtr = dataBuffer + length;
    uint8_t* outputPtr = cobsBuffer;
    uint8_t code = 1;
    uint8_t* codePtr = outputPtr++;

    while (dataBuffer < endPtr)
    {
        uint8_t byte = *dataBuffer++;
        if (byte == 0)
        {
            *codePtr = code;
            code = 1;
            codePtr = outputPtr++;
        }
        else
        {
            *outputPtr++ = byte;
            ++code;

            if (code == 0xFF)
            {
                *codePtr = code;
                code = 1;
                codePtr = outputPtr++;
            }
        }
    }
    *codePtr = code;

    return static_cast<uint16_t>(outputPtr - cobsBuffer);
}

uint16_t cobs_decode(const uint8_t* __restrict cobsBuffer, uint16_t length, uint8_t* __restrict dataBuffer)
{
    const uint8_t* endPtr = cobsBuffer + length;
    uint8_t* outputPtr = dataBuffer;

    while (cobsBuffer < endPtr)
    {
        uint8_t code = *cobsBuffer++;
        if (code == 0 || (cobsBuffer + code - 1) > endPtr) return 0;
        for (uint8_t i = 1; i < code; ++i) *outputPtr++ = *cobsBuffer++;
        if (code < 0xFF && cobsBuffer < endPtr) *outputPtr++ = 0;
    }

    return static_cast<uint16_t>(outputPtr - dataBuffer);
}

uint16_t cobs_decode_inplace(uint8_t* cobsBuffer, uint16_t length)
{
    uint8_t* readPtr = cobsBuffer;
    uint8_t* writePtr = cobsBuffer;
    uint8_t* endPtr = cobsBuffer + length;

    while (readPtr < endPtr)
    {
        uint8_t code = *readPtr++;
        if (code == 0 || (readPtr - 1 + code) > endPtr) return 0;
        for (uint8_t i = 1; i < code; ++i) *writePtr++ = *readPtr++;
        if (code < 0xFF && readPtr < endPtr) *writePtr++ = 0;
    }
    return static_cast<uint16_t>(writePtr - cobsBuffer);
}