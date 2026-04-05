#include <Arduino.h>
#include "simple_serial_communication.h"
#include "serial_communication.h"

namespace
{
HardwareSerial comSerial(1);
constexpr uint32_t kBaudRate = 115200;
constexpr uint32_t kTimeoutMicros = 20000;
constexpr size_t kVarMaxPayload = 64;

using FixedRx = FixedPacketReceiver<sizeof(uint32_t) + sizeof(int16_t) + sizeof(uint8_t),
                                    IntegrityMode::CRC16_TABLE,
                                    kTimeoutMicros>;
using HeaderRx = SerialPacketTransfer<kVarMaxPayload, IntegrityMode::CRC16_TABLE, kTimeoutMicros>;
using CobsRx = SerialPacketTransferCOBS<kVarMaxPayload, IntegrityMode::CRC16_TABLE, kTimeoutMicros>;

enum class TestMode : uint8_t
{
    FIXED = 0,
    HEADER_VAR = 1,
    COBS_VAR = 2,
};

FixedRx g_fixed_rx;
HeaderRx g_header_rx;
CobsRx g_cobs_rx;
TestMode g_mode = TestMode::FIXED;

constexpr uint32_t kFixedSwitchSeq = 0xFFFFFFFFu;
constexpr int16_t kFixedSwitchValue = 0x1234;
constexpr uint8_t kFixedSwitchFlags = 0xA0;

void send_text_header(const char* text)
{
    HeaderRx tx;
    tx.begin_packet();
    tx.add_to_packet(text, static_cast<uint16_t>(strlen(text)));
    tx.send_packet(comSerial);
}

void send_text_fixed(const char* text)
{
    const uint8_t* data = reinterpret_cast<const uint8_t*>(text);
    send_packet_buffer<IntegrityMode::CRC16_TABLE>(comSerial, data, static_cast<uint16_t>(strlen(text)));
}

void handle_fixed()
{
    uint32_t seq = 0;
    int16_t value = 0;
    uint8_t flags = 0;

    if (!receive_packet_var_args<IntegrityMode::CRC16_TABLE, kTimeoutMicros>(comSerial, g_fixed_rx, seq, value, flags))
        return;
    else
        Serial.printf("%d, %d, %d\n", seq, value, flags);

    if (seq == kFixedSwitchSeq && value == kFixedSwitchValue && flags == kFixedSwitchFlags)
    {
        Serial.println("Switching test mode to header mode ...");
        send_text_fixed("HDR_READY");
        g_mode = TestMode::HEADER_VAR;
        return;
    }

    const uint32_t rsp_seq = seq + 1u;
    const int16_t rsp_value = static_cast<int16_t>(-value);
    const uint8_t rsp_flags = static_cast<uint8_t>(flags ^ 0x5A);
    send_packet_var_args<IntegrityMode::CRC16_TABLE>(comSerial, rsp_seq, rsp_value, rsp_flags);
}

void handle_header_var()
{
    if (!g_header_rx.poll(comSerial))
        return;

    const uint8_t* rx = g_header_rx.get_received_payload();
    const uint16_t len = g_header_rx.get_received_length();
    if (len == 0)
        return;

    if (len == 3 && rx[0] == 0x7E && rx[1] == 0xCA && rx[2] == 0xFE)
    {
        send_text_header("COBS_READY");
        g_mode = TestMode::COBS_VAR;
        return;
    }

    HeaderRx tx;
    tx.begin_packet();
    const uint8_t opcode = static_cast<uint8_t>(rx[0] | 0x80);
    tx.add_to_packet(opcode);
    for (uint16_t i = 1; i < len; ++i)
    {
        const uint8_t transformed = static_cast<uint8_t>(rx[i] ^ 0xA5);
        tx.add_to_packet(transformed);
    }
    tx.send_packet(comSerial);
}

void handle_cobs_var()
{
    if (!g_cobs_rx.poll(comSerial))
        return;

    const uint8_t* rx = g_cobs_rx.get_received_payload();
    const uint16_t len = g_cobs_rx.get_received_length();
    if (len == 0)
        return;

    CobsRx tx;
    tx.begin_packet();
    const uint8_t opcode = static_cast<uint8_t>(rx[0] | 0x80);
    tx.add_to_packet(opcode);
    for (int i = static_cast<int>(len) - 1; i >= 1; --i)
        tx.add_to_packet(rx + i, 1);
    tx.send_packet(comSerial);
}
}

void setup()
{
    Serial.begin(115200);
    comSerial.begin(kBaudRate, SERIAL_8N1, 5, 4);
    while (!comSerial) { }
    delay(2000);
    comSerial.println("READY");
}

void loop()
{
    switch (g_mode)
    {
        case TestMode::FIXED:
            handle_fixed();
            break;
        case TestMode::HEADER_VAR:
            handle_header_var();
            break;
        case TestMode::COBS_VAR:
            handle_cobs_var();
            break;
        default:
            g_mode = TestMode::FIXED;
            break;
    }
}
