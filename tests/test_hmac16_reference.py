"""Golden vectors for HMAC-16 (used on Tag beacons and OTA beacons).

Locks down the truncation strategy: firmware takes the FIRST 2 bytes of the
HMAC-SHA256 output as a little-endian uint16. Any change to that convention
breaks Scanner verification silently in the field, so it is worth pinning.
"""
import hmac
import hashlib
import struct


def hmac16(key: bytes, data: bytes) -> int:
    """Reference impl matching apps/tag/components/bmt_auth/bmt_auth.c."""
    full = hmac.new(key, data, hashlib.sha256).digest()
    return struct.unpack("<H", full[:2])[0]


# 16-byte example master key (arbitrary constant used only in tests).
KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")


def test_deterministic():
    """Same key + same data => same 16-bit tag, always."""
    assert hmac16(KEY, b"hello") == hmac16(KEY, b"hello")


def test_empty_input_is_valid():
    """Empty payload still produces a well-defined tag."""
    tag = hmac16(KEY, b"")
    assert 0 <= tag <= 0xFFFF


def test_different_keys_produce_different_tags():
    other_key = bytes.fromhex("0f0e0d0c0b0a09080706050403020100")
    assert hmac16(KEY, b"same-data") != hmac16(other_key, b"same-data")


def test_truncation_takes_first_two_bytes_little_endian():
    """This is the strategy the firmware uses. If someone switches to
    'last 2 bytes' or big-endian, this test fires."""
    data = b"canary"
    full = hmac.new(KEY, data, hashlib.sha256).digest()
    expected = full[0] | (full[1] << 8)
    assert hmac16(KEY, data) == expected


def test_golden_vector():
    """One frozen (key, data) -> tag value. Bumping this test means
    someone changed the algorithm - make sure it was on purpose."""
    tag = hmac16(KEY, b"BMT/tag/24byte-adv-payload-example")
    # Regenerate this constant only if the algorithm intentionally changed:
    #   python3 -c "import tests.test_hmac16_reference as t; \
    #               print(hex(t.hmac16(t.KEY, b'BMT/tag/24byte-adv-payload-example')))"
    assert tag == 0x6147
