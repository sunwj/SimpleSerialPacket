#ifndef _COBS_H_
#define _COBS_H_

#include <memory.h>
#include <stdint.h>

uint16_t cobs_encode(const uint8_t* dataBuffer, uint16_t length, uint8_t* cobsBuffer);
uint16_t cobs_decode(const uint8_t* cobsBuffer, uint16_t length, uint8_t* dataBuffer);
uint16_t cobs_decode(uint8_t* cobsBuffer, uint16_t length);

#endif