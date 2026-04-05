from __future__ import annotations

import argparse
import sys
import time

from simple_serial_packet import (
    VariableLengthSerialPacketCOBS,
    FixedLengthSerialPacket,
    VariableLengthSerialPacket,
    IntegrityMode,
    SerialConfig,
)

FIXED_FMT = "<IhB"
FIXED_SWITCH = (0xFFFFFFFF, 0x1234, 0xA0)
HEADER_SWITCH_PAYLOAD = bytes([0x7E, 0xCA, 0xFE])


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_fixed(api: FixedLengthSerialPacket) -> None:
    print("[1/3] Testing fixed-length SimpleSerialPacket...")
    seq, value, flags = 123456789, -3210, 0x5C
    api.send_struct(FIXED_FMT, seq, value, flags)
    rsp_seq, rsp_value, rsp_flags = api.recv_struct(FIXED_FMT)
    expect(rsp_seq == (seq + 1) & 0xFFFFFFFF, f"fixed seq mismatch: {rsp_seq}")
    expect(rsp_value == -value, f"fixed value mismatch: {rsp_value}")
    expect(rsp_flags == (flags ^ 0x5A), f"fixed flags mismatch: {rsp_flags}")
    print("    ok")


def switch_to_header(api: FixedLengthSerialPacket) -> None:
    print("    switching device to header-length mode...")
    api.send_struct(FIXED_FMT, *FIXED_SWITCH)
    ack = api.recv_payload(9)
    expect(ack == b"HDR_READY", f"unexpected fixed switch ack: {ack!r}")


def test_header(api: VariableLengthSerialPacket) -> None:
    print("[2/3] Testing variable-length header mode...")
    payload = bytes([0x01, 0x00, 0x7F, 0x80, 0xFF, 0x34, 0x12])
    api.send_payload(payload)
    rsp = api.recv_payload()
    expect(rsp[0] == 0x81, f"unexpected header response opcode: {rsp[0]:#x}")
    expect(rsp[1:] == bytes((b ^ 0xA5) for b in payload[1:]), f"header payload mismatch: {rsp!r}")
    print("    ok")


def switch_to_cobs(api: VariableLengthSerialPacket) -> None:
    print("    switching device to COBS mode...")
    api.send_payload(HEADER_SWITCH_PAYLOAD)
    ack = api.recv_payload()
    expect(ack == b"COBS_READY", f"unexpected header switch ack: {ack!r}")


def test_cobs(api: VariableLengthSerialPacketCOBS) -> None:
    print("[3/3] Testing variable-length COBS mode...")
    payload = bytes([0x02, 0x00, 0x11, 0x00, 0x22, 0x33, 0x00, 0x44])
    api.send_payload(payload)
    rsp = api.recv_payload()
    expect(rsp[0] == 0x82, f"unexpected COBS response opcode: {rsp[0]:#x}")
    expected_tail = bytes(reversed(payload[1:]))
    expect(rsp[1:] == expected_tail, f"COBS payload mismatch: {rsp!r}")
    print("    ok")


def wait_for_ready(port: str, baudrate: int, timeout: float) -> None:
    import serial

    with serial.Serial(port=port, baudrate=baudrate, timeout=timeout, write_timeout=timeout) as ser:
        deadline = time.time() + timeout
        line = bytearray()
        while time.time() < deadline:
            ch = ser.read(1)
            if not ch:
                continue
            if ch == b"\n":
                text = line.decode(errors="replace").strip()
                if text == "READY":
                    return
                line.clear()
            else:
                line.extend(ch)
        raise TimeoutError("device did not print READY")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate SimpleSerialPacket firmware and Python APIs")
    parser.add_argument("--port", required=True, help="serial port, for example COM5 or /dev/ttyACM0")
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    wait_for_ready(args.port, args.baudrate, args.timeout)
    print("Device is ready")

    fixed = FixedLengthSerialPacket(SerialConfig(args.port, args.baudrate, args.timeout, args.timeout), IntegrityMode.CRC16_TABLE)
    try:
        fixed.reset()
        time.sleep(0.1)
        test_fixed(fixed)
        switch_to_header(fixed)
    finally:
        fixed.close()

    time.sleep(0.2)
    header = VariableLengthSerialPacket(SerialConfig(args.port, args.baudrate, args.timeout, args.timeout), IntegrityMode.CRC16_TABLE)
    try:
        header.reset()
        time.sleep(0.05)
        test_header(header)
        switch_to_cobs(header)
    finally:
        header.close()

    time.sleep(0.2)
    cobs = VariableLengthSerialPacketCOBS(SerialConfig(args.port, args.baudrate, args.timeout, args.timeout), IntegrityMode.CRC16_TABLE)
    try:
        cobs.reset()
        time.sleep(0.05)
        test_cobs(cobs)
    finally:
        cobs.close()

    print("All tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
