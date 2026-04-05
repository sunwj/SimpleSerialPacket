# SimpleSerialPacket

SimpleSerialPacket is a lightweight serial packet protocol for microcontrollers and host PCs.
It provides three packet formats:

1. **Fixed-length packets**
   - Format: `HEADER_1 | HEADER_2 | PAYLOAD | CHECK16`
   - Best when packet size is known at compile time.
   - Lowest parsing overhead and very predictable timing.

2. **Variable-length packets with explicit length**
   - Format: `HEADER_1 | HEADER_2 | LENGTH | PAYLOAD | CHECK16`
   - Best when packet sizes vary and you want low overhead.

3. **Variable-length packets using COBS**
   - Format on wire: `COBS(PAYLOAD | CHECK16) | 0x00`
   - Best when you want delimiter-based framing and clean resynchronization.

The integrity check is selectable at compile time in firmware and at runtime in Python:

- `CRC16_TABLE`
- `CRC16_BITWISE`
- `FLETCHER16`

The default and recommended mode is `CRC16_TABLE`.

---

## Repository layout

```text
firmware/
  common.h
  crc.h
  cobs.h
  cobs.cpp
  simple_serial_communication.h
  serial_communication.h
  main.cpp

python_api/
  simple_serial_packet.py
  main.py
  requirements.txt
```

---

## Packet formats

### Fixed-length

```text
0x55 0xAA [payload bytes...] [check low] [check high]
```

### Variable-length with explicit length

```text
0x55 0xAA [length] [payload bytes...] [check low] [check high]
```

- `length` is one byte, so the maximum payload length is 255 bytes.

### Variable-length with COBS

```text
COBS(payload + check16) 0x00
```

- The `0x00` byte is the frame delimiter.
- Payload may contain any byte value, including `0x00`.

---

## Firmware usage

### 1. Fixed-length packets

Include the fixed-length API:

```cpp
#include "simple_serial_communication.h"
```

#### Send a fixed-length packet from typed values

```cpp
send_packet_var_args<IntegrityMode::CRC16_TABLE>(Serial, uint32_t(100), int16_t(-12), uint8_t(0x5A));
```

#### Send a fixed-length packet from a runtime buffer

```cpp
uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};
send_packet_buffer<IntegrityMode::CRC16_TABLE>(Serial, payload, sizeof(payload));
```

#### Send a fixed-length packet from a compile-time array

```cpp
constexpr uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
send_packet_buffer<sizeof(payload), IntegrityMode::CRC16_TABLE>(Serial, payload);
```

#### Receive a fixed-length packet into a buffer

```cpp
using Rx = FixedPacketReceiver<8, IntegrityMode::CRC16_TABLE, 20000>;
Rx rx;

void loop()
{
    uint8_t payload[8];
    if (receive_packet_buffer(Serial, rx, payload))
    {
        // payload[0..7] now contains a valid packet
    }
}
```

#### Receive a fixed-length packet into typed variables

```cpp
using Rx = FixedPacketReceiver<sizeof(uint32_t) + sizeof(int16_t) + sizeof(uint8_t),
                               IntegrityMode::CRC16_TABLE,
                               20000>;
Rx rx;

void loop()
{
    uint32_t seq;
    int16_t value;
    uint8_t flags;

    if (receive_packet_var_args<IntegrityMode::CRC16_TABLE, 20000>(Serial, rx, seq, value, flags))
    {
        // seq, value, flags were unpacked from the payload
    }
}
```

#### Use the budgeted receive version

```cpp
uint8_t payload[8];
if (receive_packet_buffer_with_budget(Serial, rx, payload, 8))
{
    // Valid packet received, but this call processed at most 8 bytes
}
```

Use the budgeted version when serial parsing must not consume too much time in a single control-loop iteration.

---

### 2. Variable-length packets with explicit length

Include the variable-length API:

```cpp
#include "serial_communication.h"
```

#### Build and send a variable-length packet

```cpp
SerialPacketTransfer<64, IntegrityMode::CRC16_TABLE, 20000> tx;

tx.begin_packet();
uint8_t opcode = 0x01;
float value = 3.14f;
uint16_t counter = 42;

tx.add_to_packet(opcode);
tx.add_to_packet(value);
tx.add_to_packet(counter);
tx.send_packet(Serial);
```

#### Build and send from a raw byte buffer

```cpp
SerialPacketTransfer<64, IntegrityMode::CRC16_TABLE, 20000> tx;
uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};

tx.begin_packet();
tx.add_to_packet(payload, sizeof(payload));
tx.send_packet(Serial);
```

#### Receive a variable-length packet

```cpp
using HeaderRx = SerialPacketTransfer<64, IntegrityMode::CRC16_TABLE, 20000>;
HeaderRx rx;

void loop()
{
    if (rx.poll(Serial))
    {
        const uint8_t* payload = rx.get_received_payload();
        uint16_t len = rx.get_received_length();
        // process payload[0..len-1]
    }
}
```

#### Receive with a byte budget

```cpp
if (rx.poll_with_budget(Serial, 12))
{
    const uint8_t* payload = rx.get_received_payload();
    uint16_t len = rx.get_received_length();
}
```

#### Typed helper for sending

```cpp
send_var_packet<64, IntegrityMode::CRC16_TABLE>(Serial, uint8_t(0x02), int16_t(-100), float(1.5f));
```

#### Typed helper for receiving

```cpp
SerialPacketTransfer<64, IntegrityMode::CRC16_TABLE, 20000> rx;
uint8_t opcode;
int16_t value;
float gain;

if (receive_var_packet<64, IntegrityMode::CRC16_TABLE, 20000>(Serial, rx, opcode, value, gain))
{
    // values unpacked from packet
}
```

---

### 3. Variable-length packets with COBS

#### Build and send a COBS packet

```cpp
SerialPacketTransferCOBS<64, IntegrityMode::CRC16_TABLE, 20000> tx;

tx.begin_packet();
uint8_t payload[] = {0x03, 0x00, 0x11, 0x00, 0x22};
tx.add_to_packet(payload, sizeof(payload));
tx.send_packet(Serial);
```

#### Receive a COBS packet

```cpp
using CobsRx = SerialPacketTransferCOBS<64, IntegrityMode::CRC16_TABLE, 20000>;
CobsRx rx;

void loop()
{
    if (rx.poll(Serial))
    {
        const uint8_t* payload = rx.get_received_payload();
        uint16_t len = rx.get_received_length();
        // process payload[0..len-1]
    }
}
```

#### Typed helper for sending

```cpp
send_cobs_packet<64, IntegrityMode::CRC16_TABLE>(Serial, uint8_t(0x04), uint16_t(1000), uint8_t(0x00));
```

#### Typed helper for receiving

```cpp
SerialPacketTransferCOBS<64, IntegrityMode::CRC16_TABLE, 20000> rx;
uint8_t opcode;
uint16_t count;
uint8_t marker;

if (receive_cobs_packet<64, IntegrityMode::CRC16_TABLE, 20000>(Serial, rx, opcode, count, marker))
{
    // values unpacked from packet
}
```

---

## Python API usage

Install dependencies:

```bash
pip install -r requirements.txt
```

Optional faster backends:

```bash
pip install cobs libscrc
```

The Python API lives in `python_api/simple_serial_packet.py`.

### Common setup

```python
from simple_serial_packet import (
    SerialConfig,
    IntegrityMode,
    FixedLengthSerialPacket,
    VariableLengthSerialPacket,
    VariableLengthSerialPacketCOBS,
)

config = SerialConfig(port="COM5", baudrate=115200, timeout=1.0, write_timeout=1.0)
```

---

### 1. Fixed-length Python API

#### Send a raw payload

```python
api = FixedLengthSerialPacket(config, IntegrityMode.CRC16_TABLE)
api.send_payload(b"\x01\x02\x03\x04")
api.close()
```

#### Receive a raw payload of known size

```python
api = FixedLengthSerialPacket(config, IntegrityMode.CRC16_TABLE)
payload = api.recv_payload(4)
print(payload)
api.close()
```

#### Send typed data with `struct`

```python
api = FixedLengthSerialPacket(config, IntegrityMode.CRC16_TABLE)
api.send_struct("<IhB", 100, -12, 0x5A)
api.close()
```

#### Receive typed data with `struct`

```python
api = FixedLengthSerialPacket(config, IntegrityMode.CRC16_TABLE)
seq, value, flags = api.recv_struct("<IhB")
print(seq, value, flags)
api.close()
```

#### Encode without sending

```python
api = FixedLengthSerialPacket(config, IntegrityMode.CRC16_TABLE)
packet = api.encode_payload(b"\xAA\xBB\xCC")
print(packet)
api.close()
```

---

### 2. Variable-length Python API with explicit length

#### Send a variable-length payload

```python
api = VariableLengthSerialPacket(config, IntegrityMode.CRC16_TABLE)
api.send_payload(b"\x10\x20\x30\x40\x50")
api.close()
```

#### Receive a variable-length payload

```python
api = VariableLengthSerialPacket(config, IntegrityMode.CRC16_TABLE)
payload = api.recv_payload()
print(payload)
api.close()
```

#### Encode without sending

```python
api = VariableLengthSerialPacket(config, IntegrityMode.CRC16_TABLE)
packet = api.encode_payload(b"hello")
print(packet)
api.close()
```

---

### 3. Variable-length Python API with COBS

#### Send a COBS payload

```python
api = VariableLengthSerialPacketCOBS(config, IntegrityMode.CRC16_TABLE)
api.send_payload(b"\x01\x00\x02\x00\x03")
api.close()
```

#### Receive a COBS payload

```python
api = VariableLengthSerialPacketCOBS(config, IntegrityMode.CRC16_TABLE)
payload = api.recv_payload()
print(payload)
api.close()
```

#### Encode without sending

```python
api = VariableLengthSerialPacketCOBS(config, IntegrityMode.CRC16_TABLE)
packet = api.encode_payload(b"\x00\x11\x22\x00")
print(packet)
api.close()
```

---

## End-to-end validation

### Firmware side

`firmware/main.cpp` contains a validation program that switches through:

1. fixed-length mode
2. variable-length header mode
3. variable-length COBS mode

### Host side

Run the Python validation script:

```bash
python main.py --port COM5 --baudrate 115200 --timeout 3.0
```

On Linux/macOS:

```bash
python main.py --port /dev/ttyACM0 --baudrate 115200 --timeout 3.0
```

---

## Integrity mode selection

### Firmware

Fixed-length example:

```cpp
using Rx = FixedPacketReceiver<8, IntegrityMode::FLETCHER16, 20000>;
```

Variable-length example:

```cpp
SerialPacketTransfer<64, IntegrityMode::CRC16_BITWISE, 20000> rx;
```

### Python

```python
api = VariableLengthSerialPacket(config, IntegrityMode.FLETCHER16)
```

Both sides must use the same integrity mode.

---

## Notes

- Fixed-length mode is the fastest and most deterministic.
- Header-length mode is the best general-purpose variable-length mode.
- COBS mode is the best choice when payloads may contain arbitrary bytes and delimiter-based framing is preferred.
- The firmware header-length protocol currently uses a one-byte payload length.
- `packData()` and `unpackData()` use `memcpy`, so both ends must agree on data layout, endianness, and floating-point representation when sending native types directly.
