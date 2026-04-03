from cobs import cobs
from typing import Union, Tuple


MAX_DATA_LENGTH = 64


def compute_checksum(data: bytes) -> int:
    checksum = 0x55
    for b in data:
        checksum ^= b
    return checksum


def build_packet_cobs(packet_id: int, payload: bytes) -> bytes:
    """Construct a COBS-encoded packet with checksum."""
    if len(payload) > MAX_DATA_LENGTH:
        raise ValueError("Payload too large")

    raw = bytes([packet_id]) + payload
    checksum = compute_checksum(raw)
    raw += bytes([checksum])
    encoded = cobs.encode(raw) + b'\x00'  # COBS delimiter
    return encoded

def build_packet(packet_id: int, payload: bytes) -> bytes:
    """Construct a COBS-encoded packet with checksum."""
    if len(payload) > MAX_DATA_LENGTH:
        raise ValueError("Payload too large")

    raw = bytes([packet_id]) + payload
    checksum = compute_checksum(raw)
    raw += bytes([checksum])
    raw = b'\x55' + bytes([len(raw)]) + raw

    return raw


def decode_packet_cobs(encoded: bytes) -> Union[Tuple[int, bytes], None]:
    """Decode a packet and verify checksum. Returns (id, payload) or None on failure."""
    try:
        decoded = cobs.decode(encoded)
        if len(decoded) < 2:
            return None

        payload, received_checksum = decoded[:-1], decoded[-1]
        calculated_checksum = compute_checksum(payload)
        if received_checksum != calculated_checksum:
            print("Checksum mismatch")
            return None

        packet_id = payload[0]
        return packet_id, payload[1:]

    except Exception as e:
        print(f"Decode error: {e}")
        return None


def decode_packet(buffer: bytes) -> Union[Tuple[int, bytes], None]:
    """Decode a packet and verify checksum. Returns (id, payload) or None on failure."""
    try:
        payload, received_checksum = buffer[:-1], buffer[-1]
        calculated_checksum = compute_checksum(payload)
        if received_checksum != calculated_checksum:
            print("Checksum mismatch")
            return None

        packet_id = payload[0]
        return packet_id, payload[1:]

    except Exception as e:
        print(f"Decode error: {e}")
        return None