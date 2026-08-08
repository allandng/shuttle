"""Roundtrip and error-path tests for shuttle_ipc against a real
libshuttle_c.

Everything here runs in one process: the creator handle becomes the producer on
its first write, a second handle opened on the same name becomes the consumer on
its first read. That is legal — roles are bound lazily per handle, and the
channel is still strictly one producer and one consumer.

The library is located the normal way (explicit path, then SHUTTLE_C_LIB, then
the platform search); if none of those find it the whole module skips rather
than failing, since a missing build is not a binding bug.

    cmake -B build -S . && cmake --build build --target shuttle_c
    SHUTTLE_C_LIB=build/libshuttle_c.so pytest bindings/python/tests
"""
import os

import pytest

import shuttle_ipc
from shuttle_ipc import Channel

try:
    LIBRARY = shuttle_ipc.load_library()
except shuttle_ipc.LibraryNotFoundError as exc:  # pragma: no cover
    pytest.skip("libshuttle_c unavailable: {}".format(exc),
                allow_module_level=True)

CAPACITY = 1 << 20
# Deliberately larger than the adaptive copy-read's starting buffer (64 KiB),
# so test_read_sizes_itself_on_an_opened_handle actually exercises the growth.
MAX_PAYLOAD = 1 << 17

_counter = [0]


def unique_name():
    """A fresh name that fits the macOS 30-char shm-name limit."""
    _counter[0] += 1
    return "/sh-py-{}-{}".format(os.getpid() % 100000, _counter[0])


@pytest.fixture
def pair():
    """A (producer, consumer) handle pair on one freshly created channel."""
    name = unique_name()
    producer = Channel.create(name, CAPACITY, MAX_PAYLOAD)
    consumer = Channel.open(name)
    try:
        yield producer, consumer
    finally:
        consumer.close()
        producer.close()
        try:
            Channel.unlink(name)
        except shuttle_ipc.NotFound:
            pass


# --- roundtrip -------------------------------------------------------------


def test_copy_write_zero_copy_read_is_byte_exact(pair):
    producer, consumer = pair
    payload = bytes(range(256)) * 40  # 10240 bytes, every byte value present

    producer.write(payload)

    with consumer.acquire_read() as view:
        assert isinstance(view, memoryview)
        assert view.readonly, "a consumer must not be handed a writable ring"
        assert len(view) == len(payload)
        # Indexed in place: no bytes() copy is taken before this comparison.
        assert all(view[i] == payload[i] for i in range(len(payload)))
        assert bytes(view) == payload


def test_zero_copy_write_copy_read_is_byte_exact(pair):
    producer, consumer = pair
    payload = os.urandom(4096)

    with producer.acquire_write(len(payload)) as buf:
        assert not buf.readonly
        buf[:] = payload  # written straight into the segment

    assert consumer.read() == payload


def test_zero_copy_both_sides(pair):
    producer, consumer = pair
    payload = os.urandom(1024)

    reservation = producer.acquire_write(MAX_PAYLOAD)
    reservation.view[:len(payload)] = payload
    reservation.commit(len(payload))  # publish only what we filled

    borrow = consumer.acquire_read()
    assert len(borrow) == len(payload)
    assert borrow.tobytes() == payload
    borrow.release()


def test_fifo_order_and_sizes(pair):
    producer, consumer = pair
    messages = [bytes([i & 0xFF]) * (i * 37) for i in range(1, 25)]

    for msg in messages:
        producer.write(msg)
    for msg in messages:
        with consumer.acquire_read() as view:
            assert bytes(view) == msg


def test_empty_message_roundtrips(pair):
    producer, consumer = pair
    producer.write(b"")
    with consumer.acquire_read() as view:
        assert len(view) == 0
    producer.write(b"")
    assert consumer.read() == b""


def test_read_sizes_itself_on_an_opened_handle(pair):
    producer, consumer = pair
    # The opener cannot know max_payload (the ABI has no getter), so read()
    # starts at 64 KiB and grows on MSG_TOO_LARGE — the message stays queued.
    assert consumer.max_payload is None
    payload = os.urandom(MAX_PAYLOAD)
    producer.write(payload)
    assert consumer.read() == payload


def test_explicit_max_len_too_small_leaves_message_queued(pair):
    producer, consumer = pair
    payload = os.urandom(2048)
    producer.write(payload)

    with pytest.raises(shuttle_ipc.TooBig):
        consumer.read(max_len=16)
    # Still queued: the same message reads back intact.
    assert consumer.read(max_len=4096) == payload


# --- borrow guard ----------------------------------------------------------


def test_borrow_is_dead_after_release(pair):
    producer, consumer = pair
    producer.write(b"transient")

    borrow = consumer.acquire_read()
    view = borrow.view
    assert bytes(view) == b"transient"
    borrow.release()

    assert borrow.released
    with pytest.raises(shuttle_ipc.ShuttleError):
        borrow.view
    # The memoryview handed out earlier is released too, not left aliasing
    # bytes the producer may now overwrite.
    with pytest.raises(ValueError):
        view[0]


def test_release_is_idempotent(pair):
    producer, consumer = pair
    producer.write(b"x")
    borrow = consumer.acquire_read()
    borrow.release()
    borrow.release()


def test_reservation_is_dead_after_commit(pair):
    producer, consumer = pair
    reservation = producer.acquire_write(8)
    reservation.view[:] = b"abcdefgh"
    reservation.commit()

    with pytest.raises(shuttle_ipc.ShuttleError):
        reservation.view
    assert consumer.read() == b"abcdefgh"


def test_second_borrow_before_release_is_rejected(pair):
    """One borrow per handle (docs/API.md, "Zero-copy borrow rules").

    Enforced by the binding on this side: a repeat shuttle_acquire_read is
    idempotent in the C layer and would silently orphan the first guard.
    """
    producer, consumer = pair
    producer.write(b"a")
    producer.write(b"b")

    first = consumer.acquire_read()
    try:
        with pytest.raises(shuttle_ipc.InvalidArgs):
            consumer.acquire_read()
    finally:
        first.release()
    # The second message is still there, and now borrowable.
    with consumer.acquire_read() as view:
        assert bytes(view) == b"b"


def test_second_reservation_before_commit_is_rejected(pair):
    producer, _ = pair
    first = producer.acquire_write(16)
    try:
        with pytest.raises(shuttle_ipc.InvalidArgs):
            producer.acquire_write(16)
    finally:
        first.commit(0)


# --- error paths -----------------------------------------------------------


def test_open_missing_raises_not_found():
    with pytest.raises(shuttle_ipc.NotFound) as excinfo:
        Channel.open("/sh-py-nonexistent")
    assert excinfo.value.code == shuttle_ipc.ERR_NOT_FOUND
    assert isinstance(excinfo.value, shuttle_ipc.ShuttleError)


def test_unlink_missing_raises_not_found():
    with pytest.raises(shuttle_ipc.NotFound) as excinfo:
        Channel.unlink("/sh-py-nonexistent")
    assert excinfo.value.code == -4


def test_oversized_write_raises_too_big(pair):
    producer, _ = pair
    with pytest.raises(shuttle_ipc.TooBig) as excinfo:
        producer.write(b"\0" * (MAX_PAYLOAD + 1))
    assert excinfo.value.code == shuttle_ipc.ERR_MSG_TOO_LARGE


def test_oversized_reservation_raises_too_big(pair):
    producer, _ = pair
    with pytest.raises(shuttle_ipc.TooBig):
        producer.acquire_write(MAX_PAYLOAD + 1)


def test_capacity_smaller_than_payload_plus_frame():
    with pytest.raises(shuttle_ipc.CapacityTooSmall) as excinfo:
        Channel.create(unique_name(), 64, 1 << 16)
    assert excinfo.value.code == shuttle_ipc.ERR_CAPACITY_TOO_SMALL


def test_create_twice_raises_exists():
    name = unique_name()
    first = Channel.create(name, CAPACITY, MAX_PAYLOAD)
    try:
        with pytest.raises(shuttle_ipc.Exists):
            Channel.create(name, CAPACITY, MAX_PAYLOAD)
    finally:
        first.close()
        Channel.unlink(name)


def test_bad_name_raises_invalid_args():
    with pytest.raises(shuttle_ipc.InvalidArgs):
        Channel.open("no-leading-slash")


def test_nonblocking_read_on_empty_channel(pair):
    _, consumer = pair
    with pytest.raises(shuttle_ipc.WouldBlock) as excinfo:
        consumer.acquire_read(nonblock=True)
    assert excinfo.value.code == shuttle_ipc.ERR_WOULD_BLOCK
    with pytest.raises(shuttle_ipc.WouldBlock):
        consumer.read(nonblock=True)


def test_timeout_gives_up_without_parking(pair):
    _, consumer = pair
    with pytest.raises(shuttle_ipc.WouldBlock):
        consumer.read(timeout=0.05)


def test_nonblock_and_timeout_are_mutually_exclusive(pair):
    _, consumer = pair
    with pytest.raises(ValueError):
        consumer.read(nonblock=True, timeout=1.0)


def test_use_after_close_raises(pair):
    producer, consumer = pair
    producer.write(b"before close")
    consumer.close()
    assert consumer.closed
    with pytest.raises(shuttle_ipc.ShuttleError):
        consumer.read(nonblock=True)
    consumer.close()  # idempotent


# --- v1.1 surface ----------------------------------------------------------


def test_huge_pages_create_is_advisory_and_roundtrips():
    """shuttle_create_ex with SHUTTLE_CREATE_HUGEPAGES must never be fatal."""
    name = unique_name()
    producer = Channel.create(name, CAPACITY, MAX_PAYLOAD, huge_pages=True)
    consumer = Channel.open(name)
    try:
        producer.write(b"huge pages are advisory")
        with consumer.acquire_read() as view:
            assert bytes(view) == b"huge pages are advisory"
    finally:
        consumer.close()
        producer.close()
        Channel.unlink(name)


def test_keepalive_does_not_disturb_the_stream(pair):
    producer, consumer = pair
    producer.write(b"one")
    producer.keepalive()
    consumer.keepalive()
    assert consumer.read() == b"one"


def test_context_managers_close_the_handles():
    name = unique_name()
    with Channel.create(name, CAPACITY, MAX_PAYLOAD) as producer:
        with Channel.open(name) as consumer:
            producer.write(b"scoped")
            assert consumer.read() == b"scoped"
        assert consumer.closed
    assert producer.closed
    shuttle_ipc.unlink(name)


# --- library discovery -----------------------------------------------------


def test_explicit_path_beats_the_environment():
    path = shuttle_ipc.resolve_library_path(LIBRARY.path)
    assert path == LIBRARY.path


def test_missing_library_raises_library_not_found(monkeypatch):
    monkeypatch.delenv("SHUTTLE_C_LIB", raising=False)
    monkeypatch.setattr(shuttle_ipc._ffi.ctypes.util, "find_library",
                        lambda name: None)
    with pytest.raises(shuttle_ipc.LibraryNotFoundError):
        shuttle_ipc.resolve_library_path()
