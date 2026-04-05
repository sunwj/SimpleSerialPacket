
from __future__ import annotations

import enum
import importlib.util
import struct
import time
from dataclasses import dataclass
from typing import Callable, Optional

import serial

PACKET_HEADER_1 = 0x55
PACKET_HEADER_2 = 0xAA
CRC16_POLY = 0x1021
CRC16_INIT_VALUE = 0xFFFF


class IntegrityMode(str, enum.Enum):
    CRC16_BITWISE = "crc16_bitwise"
    CRC16_TABLE = "crc16_table"
    FLETCHER16 = "fletcher16"


class BackendPreference(str, enum.Enum):
    AUTO = "auto"
    PYTHON = "python"
    LIBSCRC = "libscrc"
    CRCMOD = "crcmod"


@dataclass(frozen=True)
class BackendInfo:
    crc_backend: str
    cobs_backend: str


def _module_available(name: str) -> bool:
    return importlib.util.find_spec(name) is not None


# -----------------------------------------------------------------------------
# Pure Python fallback implementations
# -----------------------------------------------------------------------------
def _build_crc16_table() -> list[int]:
    table: list[int] = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ CRC16_POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
        table.append(crc)
    return table


_CRC16_TABLE = _build_crc16_table()


def compute_crc16_bitwise_py(data: bytes) -> int:
    crc = CRC16_INIT_VALUE
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ CRC16_POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def compute_crc16_table_py(data: bytes) -> int:
    crc = CRC16_INIT_VALUE
    for byte in data:
        idx = ((crc >> 8) ^ byte) & 0xFF
        crc = ((crc << 8) ^ _CRC16_TABLE[idx]) & 0xFFFF
    return crc


def compute_fletcher16_py(data: bytes) -> int:
    sum1 = 0
    sum2 = 0
    for byte in data:
        sum1 += byte
        if sum1 >= 255:
            sum1 -= 255
        sum2 += sum1
        if sum2 >= 255:
            sum2 -= 255
    return ((sum2 & 0xFF) << 8) | (sum1 & 0xFF)


def cobs_encode_py(data: bytes) -> bytes:
    out = bytearray()
    code_index = 0
    out.append(0)
    code = 1
    for byte in data:
        if byte == 0:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
        else:
            out.append(byte)
            code += 1
            if code == 0xFF:
                out[code_index] = code
                code_index = len(out)
                out.append(0)
                code = 1
    out[code_index] = code
    return bytes(out)


def cobs_decode_py(data: bytes) -> bytes:
    out = bytearray()
    idx = 0
    n = len(data)
    while idx < n:
        code = data[idx]
        idx += 1
        if code == 0:
            raise ValueError("invalid COBS frame: zero code")
        end = idx + code - 1
        if end > n:
            raise ValueError("invalid COBS frame: code exceeds buffer")
        out.extend(data[idx:end])
        idx = end
        if code < 0xFF and idx < n:
            out.append(0)
    return bytes(out)


# -----------------------------------------------------------------------------
# Optional optimized backends
# -----------------------------------------------------------------------------
def _try_make_libscrc_crc() -> Optional[Callable[[bytes], int]]:
    if not _module_available("libscrc"):
        return None
    import libscrc  # type: ignore

    # Prefer exact CCITT-FALSE / IBM-3740 style variants.
    for name in ("ccitt_false", "ibm3740", "ccitt"):
        fn = getattr(libscrc, name, None)
        if fn is None:
            continue

        try:
            test = int(fn(b"123456789")) & 0xFFFF
        except Exception:
            continue

        # Match our implementation: poly 0x1021, init 0xFFFF, no xorout/reflection.
        if test == 0x29B1:
            return lambda data, _fn=fn: int(_fn(data)) & 0xFFFF
    return None


def _try_make_crcmod_crc() -> Optional[Callable[[bytes], int]]:
    if not _module_available("crcmod"):
        return None
    import crcmod  # type: ignore

    fn = crcmod.mkCrcFun(0x11021, initCrc=0xFFFF, rev=False, xorOut=0x0000)
    test = int(fn(b"123456789")) & 0xFFFF
    if test != 0x29B1:
        return None
    return lambda data, _fn=fn: int(_fn(data)) & 0xFFFF


def _select_crc16_table_backend(preference: BackendPreference) -> tuple[str, Callable[[bytes], int]]:
    if preference in (BackendPreference.AUTO, BackendPreference.LIBSCRC):
        fn = _try_make_libscrc_crc()
        if fn is not None:
            return "libscrc", fn

    if preference in (BackendPreference.AUTO, BackendPreference.CRCMOD):
        fn = _try_make_crcmod_crc()
        if fn is not None:
            return "crcmod", fn

    return "python", compute_crc16_table_py


def _select_cobs_backends() -> tuple[str, Callable[[bytes], bytes], Callable[[bytes], bytes]]:
    if _module_available("cobs"):
        from cobs import cobs  # type: ignore
        return "cobs", cobs.encode, cobs.decode
    return "python", cobs_encode_py, cobs_decode_py


_CRC16_TABLE_BACKEND_NAME, _CRC16_TABLE_BACKEND = _select_crc16_table_backend(BackendPreference.AUTO)
_COBS_BACKEND_NAME, _COBS_ENCODE_BACKEND, _COBS_DECODE_BACKEND = _select_cobs_backends()


def available_backends() -> BackendInfo:
    return BackendInfo(crc_backend=_CRC16_TABLE_BACKEND_NAME, cobs_backend=_COBS_BACKEND_NAME)


def configure_backends(preference: BackendPreference = BackendPreference.AUTO) -> BackendInfo:
    global _CRC16_TABLE_BACKEND_NAME, _CRC16_TABLE_BACKEND
    _CRC16_TABLE_BACKEND_NAME, _CRC16_TABLE_BACKEND = _select_crc16_table_backend(preference)
    return available_backends()


def compute_crc16_table(data: bytes) -> int:
    return _CRC16_TABLE_BACKEND(data)


def compute_integrity16(data: bytes, mode: IntegrityMode) -> int:
    if mode == IntegrityMode.CRC16_BITWISE:
        return compute_crc16_bitwise_py(data)
    if mode == IntegrityMode.CRC16_TABLE:
        return compute_crc16_table(data)
    if mode == IntegrityMode.FLETCHER16:
        return compute_fletcher16_py(data)
    raise ValueError(f"unsupported mode: {mode}")


def cobs_encode(data: bytes) -> bytes:
    return _COBS_ENCODE_BACKEND(data)


def cobs_decode(data: bytes) -> bytes:
    return _COBS_DECODE_BACKEND(data)


@dataclass
class SerialConfig:
    port: str
    baudrate: int = 115200
    timeout: float = 1.0
    write_timeout: float = 1.0


class BaseSerialPacket:
    def __init__(self, config: SerialConfig):
        self.config = config
        self.ser = serial.Serial(
            port=config.port,
            baudrate=config.baudrate,
            timeout=config.timeout,
            write_timeout=config.write_timeout,
        )

    def close(self) -> None:
        self.ser.close()

    def reset(self) -> None:
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def read_exactly(self, size: int) -> bytes:
        data = self.ser.read(size)
        if len(data) != size:
            raise TimeoutError(f"expected {size} bytes, got {len(data)}")
        return data

    def read_until_delimiter(self, delimiter: bytes = b"\x00", max_size: int = 4096) -> bytes:
        buf = bytearray()
        while True:
            chunk = self.ser.read(1)
            if not chunk:
                raise TimeoutError("timeout waiting for delimited frame")
            if chunk == delimiter:
                return bytes(buf)
            buf.extend(chunk)
            if len(buf) > max_size:
                raise ValueError("frame exceeded max_size while waiting for delimiter")

    def __enter__(self) -> "BaseSerialPacket":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()


class FixedLengthSerialPacket(BaseSerialPacket):
    def __init__(self, config: SerialConfig, mode: IntegrityMode = IntegrityMode.CRC16_TABLE):
        super().__init__(config)
        self.mode = mode

    def encode_payload(self, payload: bytes) -> bytes:
        check = compute_integrity16(payload, self.mode)
        return bytes([PACKET_HEADER_1, PACKET_HEADER_2]) + payload + struct.pack("<H", check)

    def send_payload(self, payload: bytes) -> None:
        self.ser.write(self.encode_payload(payload))

    def recv_payload(self, payload_size: int) -> bytes:
        while True:
            first = self.read_exactly(1)[0]
            if first != PACKET_HEADER_1:
                continue
            second = self.read_exactly(1)[0]
            if second != PACKET_HEADER_2:
                continue
            payload = self.read_exactly(payload_size)
            rx_check, = struct.unpack("<H", self.read_exactly(2))
            expected = compute_integrity16(payload, self.mode)
            if rx_check != expected:
                raise ValueError("fixed frame integrity check failed")
            return payload

    def send_struct(self, fmt: str, *values: object) -> None:
        self.send_payload(struct.pack(fmt, *values))

    def recv_struct(self, fmt: str) -> tuple[object, ...]:
        payload = self.recv_payload(struct.calcsize(fmt))
        return struct.unpack(fmt, payload)


class VariableLengthSerialPacket(BaseSerialPacket):
    def __init__(self, config: SerialConfig, mode: IntegrityMode = IntegrityMode.CRC16_TABLE):
        super().__init__(config)
        self.mode = mode

    def encode_payload(self, payload: bytes) -> bytes:
        if len(payload) > 255:
            raise ValueError("header-mode payload too large for 1-byte length")
        check = compute_integrity16(payload, self.mode)
        return bytes([PACKET_HEADER_1, PACKET_HEADER_2, len(payload)]) + payload + struct.pack("<H", check)

    def send_payload(self, payload: bytes) -> None:
        self.ser.write(self.encode_payload(payload))

    def recv_payload(self) -> bytes:
        while True:
            first = self.read_exactly(1)[0]
            if first != PACKET_HEADER_1:
                continue
            second = self.read_exactly(1)[0]
            if second != PACKET_HEADER_2:
                continue
            length = self.read_exactly(1)[0]
            payload = self.read_exactly(length)
            rx_check, = struct.unpack("<H", self.read_exactly(2))
            expected = compute_integrity16(payload, self.mode)
            if rx_check != expected:
                raise ValueError("header-mode integrity check failed")
            return payload


class VariableLengthSerialPacketCOBS(BaseSerialPacket):
    def __init__(self, config: SerialConfig, mode: IntegrityMode = IntegrityMode.CRC16_TABLE):
        super().__init__(config)
        self.mode = mode

    def encode_payload(self, payload: bytes) -> bytes:
        check = compute_integrity16(payload, self.mode)
        raw = payload + struct.pack("<H", check)
        return cobs_encode(raw) + b"\x00"

    def send_payload(self, payload: bytes) -> None:
        self.ser.write(self.encode_payload(payload))

    def recv_payload(self) -> bytes:
        encoded = self.read_until_delimiter(b"\x00")
        if not encoded:
            return b""
        raw = cobs_decode(encoded)
        if len(raw) < 2:
            raise ValueError("COBS frame too short")
        payload = raw[:-2]
        rx_check, = struct.unpack("<H", raw[-2:])
        expected = compute_integrity16(payload, self.mode)
        if rx_check != expected:
            raise ValueError("COBS integrity check failed")
        return payload