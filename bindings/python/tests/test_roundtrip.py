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
import mmap
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


# --- v1.2 surface: statistics ----------------------------------------------


@pytest.fixture
def stats_pair():
    """A pair on a channel created with the counters (layout version 2)."""
    name = unique_name()
    producer = Channel.create(name, CAPACITY, MAX_PAYLOAD,
                              flags=shuttle_ipc.CREATE_STATS)
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


def test_stats_roundtrip_counts_payload_bytes(stats_pair):
    producer, consumer = stats_pair
    messages = [os.urandom(n) for n in (1, 100, 4096, 5000)]

    zero = producer.get_stats()
    assert zero == shuttle_ipc.Stats(0, 0, 0, 0, 0)

    for msg in messages:
        producer.write(msg)
    mid = producer.get_stats()
    assert mid.msgs_written == len(messages)
    assert mid.bytes_written == sum(len(m) for m in messages)
    # Nothing has been released yet, so the read side is still at zero.
    assert mid.msgs_read == 0 and mid.bytes_read == 0

    for msg in messages:
        with consumer.acquire_read() as view:
            assert bytes(view) == msg

    # Read from the OTHER handle: the counters live in the segment, not in the
    # handle that wrote them.
    final = consumer.get_stats()
    assert final.msgs_read == len(messages)
    assert final.bytes_read == sum(len(m) for m in messages)
    # Payload bytes on both sides — the 8-byte frame header is excluded, so the
    # two totals are directly comparable.
    assert final.bytes_read == final.bytes_written
    assert final.msgs_dropped == 0


def test_stats_on_a_v1_segment_raises_no_stats(pair):
    producer, _ = pair
    with pytest.raises(shuttle_ipc.NoStats) as excinfo:
        producer.get_stats()
    assert excinfo.value.code == shuttle_ipc.ERR_NO_STATS == -15


# --- v1.3 surface: drop-newest ---------------------------------------------


def test_drop_newest_drops_instead_of_blocking():
    """The one lossy path, and it must be visible to the caller."""
    name = unique_name()
    # Small ring: a couple of large messages fill it.
    producer = Channel.create(name, 1 << 16, 1 << 15,
                              flags=shuttle_ipc.CREATE_STATS)
    consumer = Channel.open(name)
    try:
        payload = b"x" * (1 << 15)
        written = dropped = 0
        for _ in range(8):
            if producer.write(payload, drop_newest=True):
                written += 1
            else:
                dropped += 1
        assert written >= 1, "nothing fit at all — the test proves nothing"
        assert dropped >= 1, "the ring never filled — no drop was exercised"

        stats = producer.get_stats()
        assert stats.msgs_dropped == dropped
        assert stats.msgs_written == written
        # A drop disturbs nothing already queued: the messages that WERE
        # accepted are all still there, intact and in order.
        for _ in range(written):
            assert consumer.read() == payload

        # An oversized payload stays an error, not a silent drop: it could
        # never fit in any ring state, so it is a caller bug.
        with pytest.raises(shuttle_ipc.TooBig):
            producer.write(b"y" * ((1 << 15) + 1), drop_newest=True)
    finally:
        consumer.close()
        producer.close()
        Channel.unlink(name)


def test_drop_newest_rejects_a_blocking_policy_too():
    name = unique_name()
    with Channel.create(name, CAPACITY, MAX_PAYLOAD) as producer:
        with pytest.raises(ValueError):
            producer.write(b"a", drop_newest=True, nonblock=True)
        with pytest.raises(ValueError):
            producer.write(b"a", drop_newest=True, timeout=0.1)
    Channel.unlink(name)


# --- v1.4 surface: page-aligned spans --------------------------------------


def test_aligned_spans_give_page_aligned_borrows():
    """The whole point of the flag: an address a no-copy API will accept."""
    name = unique_name()
    page = mmap.PAGESIZE
    producer = Channel.create(name, 64 * page, 4096,
                              flags=shuttle_ipc.CREATE_ALIGNED_SPANS)
    consumer = Channel.open(name)
    try:
        # Consecutive frames are page + round_up(len, page) apart — the padded
        # stride, which is what keeps the NEXT payload aligned too.
        strides = []
        for length in (1, 4096):
            producer.write(b"s" * length)
            borrow = consumer.acquire_read()
            strides.append(borrow.address)
            borrow.release()
            assert strides[-1] % page == 0
        assert strides[1] - strides[0] == page + page  # len 1 -> two pages

        for length in (0, 1, 100, 4096):
            payload = os.urandom(length)
            # The producer's reserved span is aligned too, not just the read.
            reservation = producer.acquire_write(length)
            assert reservation.address % page == 0
            if length:
                reservation.view[:] = payload
            reservation.commit(length)

            borrow = consumer.acquire_read()
            try:
                assert borrow.address % page == 0, (
                    "payload at {:#x} is not page-aligned".format(
                        borrow.address))
                assert bytes(borrow.view) == payload
            finally:
                borrow.release()
            # The address is a borrow-lifetime property like the view is.
            with pytest.raises(shuttle_ipc.ShuttleError):
                borrow.address
    finally:
        consumer.close()
        producer.close()
        Channel.unlink(name)


def test_unaligned_channel_is_not_page_aligned():
    """The negative control: without the flag the payload lands 8 bytes in.

    Without this, the assertion above could pass on a build where the payload
    happened to be aligned for unrelated reasons.
    """
    name = unique_name()
    producer = Channel.create(name, 64 * mmap.PAGESIZE, 4096)
    consumer = Channel.open(name)
    try:
        addresses = []
        for _ in range(2):
            producer.write(b"z" * 64)
            borrow = consumer.acquire_read()
            addresses.append(borrow.address)
            borrow.release()
        # The classic v1 data_offset (1280) is not a page multiple, so a
        # borrow on a default channel is not page-aligned...
        assert addresses[0] % mmap.PAGESIZE != 0
        # ...and consecutive frames are 8 + len apart, not a padded stride.
        assert addresses[1] - addresses[0] == 8 + 64
    finally:
        consumer.close()
        producer.close()
        Channel.unlink(name)


def test_aligned_capacity_floor_is_the_padded_stride():
    """FR-4 is measured in the channel's own geometry."""
    page = mmap.PAGESIZE
    # One frame for a (page + 1)-byte payload costs page + 2*page.
    with pytest.raises(shuttle_ipc.CapacityTooSmall):
        Channel.create(unique_name(), 3 * page - 1, page + 1,
                       flags=shuttle_ipc.CREATE_ALIGNED_SPANS)
    # The same numbers are fine without the flag: the floor moved because the
    # framing did.
    name = unique_name()
    Channel.create(name, 3 * page - 1, page + 1).close()
    Channel.unlink(name)


def test_aligned_and_stats_compose():
    """0x18: counters on an aligned channel still count PAYLOAD bytes."""
    name = unique_name()
    flags = shuttle_ipc.CREATE_ALIGNED_SPANS | shuttle_ipc.CREATE_STATS
    producer = Channel.create(name, 64 * mmap.PAGESIZE, 4096, flags=flags)
    consumer = Channel.open(name)
    try:
        lengths = (1, 4095, 4096)
        for n in lengths:
            producer.write(b"q" * n)
            with consumer.acquire_read() as view:
                assert len(view) == n
        stats = producer.get_stats()
        assert stats.msgs_written == stats.msgs_read == len(lengths)
        # Not the padded stride, which would be far larger.
        assert stats.bytes_written == sum(lengths)
        assert stats.bytes_read == sum(lengths)
    finally:
        consumer.close()
        producer.close()
        Channel.unlink(name)


# --- v1.4 surface: file-backed channels ------------------------------------


@pytest.fixture
def seg_path(tmp_path):
    """An absolute path for a file-backed segment, removed afterward."""
    path = str(tmp_path / "chan.seg")
    yield path
    try:
        Channel.unlink_file(path)
    except shuttle_ipc.NotFound:
        pass


def test_file_backed_roundtrip(seg_path):
    """The whole feature: a channel whose segment is a file on disk."""
    producer = Channel.create_file(seg_path, CAPACITY, MAX_PAYLOAD)
    consumer = Channel.open_file(seg_path)
    try:
        assert os.path.exists(seg_path)
        # The file is sized to hold the geometry, which is where a capacity
        # larger than RAM would come from.
        assert os.path.getsize(seg_path) >= CAPACITY

        payload = os.urandom(9999)
        producer.write(payload)
        with consumer.acquire_read() as view:  # zero-copy, out of a file
            assert bytes(view) == payload

        # ...and the borrow path on the producer side too.
        blob = os.urandom(4096)
        with producer.acquire_write(len(blob)) as buf:
            buf[:] = blob
        assert consumer.read() == blob
    finally:
        consumer.close()
        producer.close()
    Channel.unlink_file(seg_path)
    assert not os.path.exists(seg_path)


def test_file_backed_stats_compose(seg_path):
    """0x28: file+stats. The backing changes nothing about the counters."""
    producer = Channel.create_file(seg_path, CAPACITY, MAX_PAYLOAD,
                                   flags=shuttle_ipc.CREATE_STATS)
    consumer = Channel.open_file(seg_path)
    try:
        lengths = (1, 1000, 65536)
        for n in lengths:
            producer.write(b"f" * n)
            with consumer.acquire_read() as view:
                assert len(view) == n
        stats = consumer.get_stats()
        assert stats.msgs_written == stats.msgs_read == len(lengths)
        assert stats.bytes_written == stats.bytes_read == sum(lengths)
    finally:
        consumer.close()
        producer.close()


def test_open_file_missing_raises_not_found(tmp_path):
    with pytest.raises(shuttle_ipc.NotFound):
        Channel.open_file(str(tmp_path / "nope.seg"))


def test_unlink_file_missing_raises_not_found(tmp_path):
    with pytest.raises(shuttle_ipc.NotFound):
        Channel.unlink_file(str(tmp_path / "nope.seg"))


def test_create_file_twice_raises_exists(seg_path):
    """And the existing file is left alone — the stale-file recovery point."""
    with Channel.create_file(seg_path, CAPACITY, MAX_PAYLOAD):
        size = os.path.getsize(seg_path)
        with pytest.raises(shuttle_ipc.Exists):
            Channel.create_file(seg_path, CAPACITY, MAX_PAYLOAD)
        assert os.path.getsize(seg_path) == size


@pytest.mark.parametrize("bad", ["relative/chan.seg", "chan.seg", ""])
def test_non_absolute_paths_raise_invalid_args(bad):
    """A channel's identity must not depend on anyone's working directory."""
    with pytest.raises(shuttle_ipc.InvalidArgs):
        Channel.create_file(bad, CAPACITY, MAX_PAYLOAD)
    with pytest.raises(shuttle_ipc.InvalidArgs):
        Channel.open_file(bad)
    with pytest.raises(shuttle_ipc.InvalidArgs):
        Channel.unlink_file(bad)


@pytest.mark.parametrize("bit", [shuttle_ipc.CREATE_HUGETLB_2MB,
                                 shuttle_ipc.CREATE_HUGETLB_1GB])
def test_file_backed_rejects_hugetlb_bits(seg_path, bit):
    """A hugetlbfs backing and a path name two different segments."""
    with pytest.raises(shuttle_ipc.InvalidArgs):
        Channel.create_file(seg_path, CAPACITY, MAX_PAYLOAD, flags=bit)
    assert not os.path.exists(seg_path)


def test_create_masks_the_file_backed_bit():
    """The documented asymmetry: 0x20 is not selectable through create().

    ``create`` takes an shm NAME and has nowhere to put a path, so the bit is
    masked off like any other it cannot implement — silently, per the
    unknown-bit rule, not as an error.
    """
    name = unique_name()
    with Channel.create(name, CAPACITY, MAX_PAYLOAD,
                        flags=shuttle_ipc._ffi.CREATE_FILE_BACKED) as producer:
        producer.write(b"an ordinary shm channel")
        with Channel.open(name) as consumer:
            assert consumer.read() == b"an ordinary shm channel"
    Channel.unlink(name)


def test_unknown_create_bits_are_masked_not_rejected():
    """An older library must tolerate a flag it does not implement."""
    name = unique_name()
    with Channel.create(name, CAPACITY, MAX_PAYLOAD, flags=1 << 30) as producer:
        producer.write(b"still works")
        with Channel.open(name) as consumer:
            assert consumer.read() == b"still works"
    Channel.unlink(name)


# --- v1.4 surface: the read-only lookahead ---------------------------------


def test_peek_next_reports_the_next_message_length(pair):
    """Empty -> None; one committed message -> its exact payload length."""
    producer, consumer = pair

    assert consumer.peek_next() is None      # nothing committed yet

    producer.write(b"x" * 1234)
    assert consumer.peek_next() == 1234
    # Read-only: asking twice does not consume, and the message is still there.
    assert consumer.peek_next() == 1234
    assert consumer.read() == b"x" * 1234
    assert consumer.peek_next() is None


def test_peek_next_sees_past_an_outstanding_borrow(pair):
    """The case with no other API.

    Borrows are strictly release-before-acquire — while message 1 is held,
    ``acquire_read`` hands back message 1 — so this is the only way to learn
    that message 2 has arrived, and how big it is, before releasing.
    """
    producer, consumer = pair
    producer.write(b"first")

    with consumer.acquire_read() as view:
        assert bytes(view) == b"first"
        assert consumer.peek_next() is None      # nothing behind it yet
        producer.write(b"second message")
        assert consumer.peek_next() == len(b"second message")
        # The borrow is untouched by the peek.
        assert bytes(view) == b"first"

    assert consumer.peek_next() == len(b"second message")
    assert consumer.read() == b"second message"
    assert consumer.peek_next() is None


def test_peek_next_survives_a_wrap(pair):
    """Draining a channel repeatedly wraps the ring; peek keeps up.

    The interesting internal state (the next message sitting at offset 0 while
    the borrow is still in the high region) is engineered precisely in the C++
    test; here the point is that the binding stays correct across whatever the
    ring does under a long run.
    """
    producer, consumer = pair
    lengths = [(i * 7919) % 30000 + 1 for i in range(200)]
    for n in lengths:
        producer.write(b"w" * n)
        assert consumer.peek_next() == n
        with consumer.acquire_read() as view:
            assert len(view) == n
    assert consumer.peek_next() is None


def test_peek_next_on_a_closed_channel_raises(pair):
    producer, consumer = pair
    consumer.close()
    with pytest.raises(shuttle_ipc.ShuttleError):
        consumer.peek_next()


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
