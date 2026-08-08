//! Roundtrip and error-path tests against a real libshuttle_c.
//!
//! Everything here runs in one process: the created handle becomes the
//! producer, a second handle opened on the same name becomes the consumer. The
//! channel is still strictly one producer and one consumer — only the two ends
//! happen to live in the same address space, which the ABI does not care
//! about.
//!
//! Build the library and point the linker at it:
//!
//! ```sh
//! cmake -B build -S . && cmake --build build --target shuttle_c
//! SHUTTLE_LIB_DIR=$PWD/build cargo test --manifest-path bindings/rust/Cargo.toml
//! ```

use std::sync::atomic::{AtomicU32, Ordering};

use shuttle::{Channel, Consumer, CreateFlags, Error, Producer, Stats, Written};

// `_SC_PAGESIZE`, which is NOT the same number on the two platforms. Hard-coded
// here rather than pulled from a `libc` crate because this repo adds no
// dependencies; `sysconf` itself already comes in with std.
#[cfg(target_os = "macos")]
const SC_PAGESIZE: i32 = 29;
#[cfg(not(target_os = "macos"))]
const SC_PAGESIZE: i32 = 30;

extern "C" {
    #[link_name = "sysconf"]
    fn libc_sysconf(name: i32) -> i64;
}

/// The alignment unit `CreateFlags::ALIGNED_SPANS` promises. Asked of the OS
/// rather than assumed: the promise is "the system page", and that is 16 KiB on
/// Apple Silicon, not the 4 KiB an x86 habit would hard-code.
fn page_size() -> usize {
    // SAFETY: sysconf takes no pointer and writes nothing.
    let n = unsafe { libc_sysconf(SC_PAGESIZE) };
    assert!(n >= 4096, "implausible page size {}", n);
    let n = n as usize;
    assert!(n & (n - 1) == 0, "page size {} is not a power of two", n);
    n
}

const CAPACITY: usize = 1 << 20;
// Larger than read_owned's starting buffer (64 KiB), so the growth path is
// actually exercised.
const MAX_PAYLOAD: usize = 1 << 17;

static COUNTER: AtomicU32 = AtomicU32::new(0);

/// A fresh name that fits the macOS 30-char shm-name limit.
fn unique_name() -> String {
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    format!("/sh-rs-{}-{}", std::process::id() % 100_000, n)
}

/// A created producer plus an opened consumer on the same channel. The name is
/// returned so the test can unlink it.
struct Pair {
    name: String,
    producer: Producer,
    consumer: Consumer,
}

impl Pair {
    fn new() -> Pair {
        Pair::with_flags(CreateFlags::NONE)
    }

    fn with_flags(flags: CreateFlags) -> Pair {
        let name = unique_name();
        let producer = Channel::create_with(&name, CAPACITY, MAX_PAYLOAD, flags)
            .expect("create")
            .into_producer();
        let consumer = Channel::open(&name).expect("open").into_consumer();
        Pair {
            name,
            producer,
            consumer,
        }
    }
}

impl Drop for Pair {
    fn drop(&mut self) {
        let _ = Channel::unlink(&self.name);
    }
}

// --- roundtrip -------------------------------------------------------------

#[test]
fn copy_write_zero_copy_read_is_byte_exact() {
    let mut p = Pair::new();
    let payload: Vec<u8> = (0..=255u8).cycle().take(10_240).collect();

    p.producer.write(&payload).expect("write");

    let msg = p.consumer.acquire_read().expect("acquire_read");
    assert_eq!(msg.len(), payload.len());
    // Compared in place: to_vec is never called on this path.
    assert_eq!(msg.as_slice(), &payload[..]);
    msg.release().expect("release_read");
}

#[test]
fn zero_copy_write_copy_read_is_byte_exact() {
    let mut p = Pair::new();
    let payload: Vec<u8> = (0..4096u32).map(|i| (i * 7) as u8).collect();

    let mut res = p.producer.acquire_write(payload.len()).expect("acquire_write");
    res.as_mut_slice().copy_from_slice(&payload); // written into the segment
    res.commit_all().expect("commit_write");

    assert_eq!(p.consumer.read_owned().expect("read_owned"), payload);
}

#[test]
fn partial_commit_publishes_only_what_was_written() {
    let mut p = Pair::new();
    let payload = b"only these bytes";

    let mut res = p.producer.acquire_write(MAX_PAYLOAD).expect("acquire_write");
    res.as_mut_slice()[..payload.len()].copy_from_slice(payload);
    res.commit(payload.len()).expect("commit_write");

    let msg = p.consumer.acquire_read().expect("acquire_read");
    assert_eq!(msg.as_slice(), payload);
}

#[test]
fn fifo_order_and_sizes_hold() {
    let mut p = Pair::new();
    let messages: Vec<Vec<u8>> = (1..25u8).map(|i| vec![i; i as usize * 37]).collect();

    for m in &messages {
        p.producer.write(m).expect("write");
    }
    for m in &messages {
        let msg = p.consumer.acquire_read().expect("acquire_read");
        assert_eq!(msg.as_slice(), &m[..]);
    }
}

#[test]
fn empty_messages_roundtrip() {
    let mut p = Pair::new();
    p.producer.write(&[]).expect("write");

    let msg = p.consumer.acquire_read().expect("acquire_read");
    assert!(msg.is_empty());
    assert_eq!(msg.as_slice(), &[] as &[u8]);
    drop(msg);

    p.producer.write(&[]).expect("write");
    assert_eq!(p.consumer.read_owned().expect("read_owned"), Vec::<u8>::new());
}

#[test]
fn read_owned_grows_past_its_starting_buffer() {
    let mut p = Pair::new();
    // The opener cannot know max_payload — the ABI has no getter — so
    // read_owned starts at 64 KiB and doubles on TooBig. That retry is safe
    // because an oversized copy read leaves the message queued.
    let payload: Vec<u8> = (0..MAX_PAYLOAD).map(|i| (i % 251) as u8).collect();
    p.producer.write(&payload).expect("write");
    assert_eq!(p.consumer.read_owned().expect("read_owned"), payload);
}

#[test]
fn undersized_read_into_leaves_the_message_queued() {
    let mut p = Pair::new();
    let payload = vec![0xABu8; 2048];
    p.producer.write(&payload).expect("write");

    let mut small = [0u8; 16];
    assert_eq!(p.consumer.read_into(&mut small), Err(Error::TooBig));

    let mut big = vec![0u8; 4096];
    let n = p.consumer.read_into(&mut big).expect("read_into");
    assert_eq!(&big[..n], &payload[..]);
}

#[test]
fn drop_releases_the_borrow() {
    let mut p = Pair::new();
    p.producer.write(b"first").expect("write");
    p.producer.write(b"second").expect("write");

    {
        let msg = p.consumer.acquire_read().expect("acquire_read");
        assert_eq!(msg.as_slice(), b"first");
    } // Drop performs shuttle_release_read

    // A second borrow is only possible because the first one released.
    let msg = p.consumer.acquire_read().expect("acquire_read");
    assert_eq!(msg.as_slice(), b"second");
}

// --- error paths -----------------------------------------------------------

#[test]
fn open_missing_is_not_found() {
    let err = Channel::open("/sh-rs-nonexistent").unwrap_err();
    assert_eq!(err, Error::NotFound);
    assert_eq!(err.code(), -4);
}

#[test]
fn unlink_missing_is_not_found() {
    assert_eq!(Channel::unlink("/sh-rs-nonexistent"), Err(Error::NotFound));
}

#[test]
fn oversized_write_is_too_big() {
    let mut p = Pair::new();
    let payload = vec![0u8; MAX_PAYLOAD + 1];
    let err = p.producer.write(&payload).unwrap_err();
    assert_eq!(err, Error::TooBig);
    assert_eq!(err.code(), -11);
}

#[test]
fn oversized_reservation_is_too_big() {
    let mut p = Pair::new();
    assert!(matches!(
        p.producer.acquire_write(MAX_PAYLOAD + 1),
        Err(Error::TooBig)
    ));
}

#[test]
fn capacity_below_payload_plus_frame_is_rejected() {
    let err = Channel::create(&unique_name(), 64, 1 << 16).unwrap_err();
    assert_eq!(err, Error::CapacityTooSmall);
}

#[test]
fn creating_twice_is_exists() {
    let name = unique_name();
    let _first = Channel::create(&name, CAPACITY, MAX_PAYLOAD).expect("create");
    assert_eq!(
        Channel::create(&name, CAPACITY, MAX_PAYLOAD).unwrap_err(),
        Error::Exists
    );
    let _ = Channel::unlink(&name);
}

#[test]
fn malformed_names_are_invalid_args() {
    assert_eq!(
        Channel::open("no-leading-slash").unwrap_err(),
        Error::InvalidArgs
    );
    // An interior NUL never reaches the C layer; it is rejected here.
    assert_eq!(Channel::open("/has\0nul").unwrap_err(), Error::InvalidArgs);
}

#[test]
fn try_read_on_an_empty_channel_would_block() {
    let mut p = Pair::new();
    assert!(matches!(p.consumer.try_acquire_read(), Err(Error::WouldBlock)));
    assert!(matches!(p.consumer.try_read_owned(), Err(Error::WouldBlock)));
}

#[test]
fn error_codes_round_trip_through_the_enum() {
    for code in -15..=-1 {
        assert_eq!(Error::from_code(code).code(), code);
        // ...and every one of them is a NAMED variant, not the catch-all.
        assert_ne!(Error::from_code(code), Error::Unknown(code));
    }
    // An unnamed code is carried, not guessed at.
    assert_eq!(Error::from_code(-99), Error::Unknown(-99));
    assert_eq!(Error::from_code(-99).code(), -99);
    assert!(!Error::NotFound.to_string().is_empty());
}

// --- v1.1 surface ----------------------------------------------------------

#[test]
fn huge_pages_create_is_advisory_and_roundtrips() {
    let mut p = Pair::with_flags(CreateFlags::HUGEPAGES);
    p.producer.write(b"huge pages are advisory").expect("write");
    let msg = p.consumer.acquire_read().expect("acquire_read");
    assert_eq!(msg.as_slice(), b"huge pages are advisory");
}

#[test]
fn keepalive_does_not_disturb_the_stream() {
    let mut p = Pair::new();
    p.producer.write(b"one").expect("write");
    p.producer.keepalive();
    p.consumer.keepalive();
    assert_eq!(p.consumer.read_owned().expect("read_owned"), b"one".to_vec());
}

#[test]
fn create_flags_compose() {
    assert_eq!(CreateFlags::NONE.bits(), 0);
    assert_eq!(CreateFlags::HUGEPAGES.bits(), 1);
    assert_eq!((CreateFlags::NONE | CreateFlags::HUGEPAGES).bits(), 1);
    // The whole create-flag namespace, in the header's order.
    assert_eq!(CreateFlags::HUGETLB_2MB.bits(), 0x2);
    assert_eq!(CreateFlags::HUGETLB_1GB.bits(), 0x4);
    assert_eq!(CreateFlags::STATS.bits(), 0x8);
    assert_eq!(CreateFlags::ALIGNED_SPANS.bits(), 0x10);
    assert_eq!(
        (CreateFlags::STATS | CreateFlags::ALIGNED_SPANS).bits(),
        0x18
    );
}

// --- v1.2 surface: statistics ----------------------------------------------

#[test]
fn stats_roundtrip_counts_payload_bytes() {
    let mut p = Pair::with_flags(CreateFlags::STATS);
    let messages: Vec<Vec<u8>> = vec![vec![1u8; 1], vec![2u8; 100], vec![3u8; 5000]];
    let total: u64 = messages.iter().map(|m| m.len() as u64).sum();

    assert_eq!(p.producer.stats().expect("stats"), Stats::default());

    for m in &messages {
        p.producer.write(m).expect("write");
    }
    let mid = p.producer.stats().expect("stats");
    assert_eq!(mid.msgs_written, messages.len() as u64);
    assert_eq!(mid.bytes_written, total);
    // Nothing released yet.
    assert_eq!((mid.msgs_read, mid.bytes_read), (0, 0));

    for m in &messages {
        let msg = p.consumer.acquire_read().expect("acquire_read");
        assert_eq!(msg.as_slice(), &m[..]);
    }

    // Read through the OTHER handle: the counters live in the segment.
    let end = p.consumer.stats().expect("stats");
    assert_eq!(end.msgs_read, messages.len() as u64);
    // Payload bytes on both sides, so the two totals are comparable.
    assert_eq!(end.bytes_read, end.bytes_written);
    assert_eq!(end.msgs_dropped, 0);
}

#[test]
fn stats_on_a_v1_segment_is_no_stats() {
    let p = Pair::new();
    assert_eq!(p.producer.stats(), Err(Error::NoStats));
    assert_eq!(Error::NoStats.code(), -15);
    assert_eq!(Error::NoHugePages.code(), -14);
}

// --- v1.3 surface: drop-newest ---------------------------------------------

#[test]
fn write_or_drop_drops_instead_of_blocking() {
    let name = unique_name();
    // A ring only a couple of large messages wide.
    let mut producer = Channel::create_with(&name, 1 << 16, 1 << 15, CreateFlags::STATS)
        .expect("create")
        .into_producer();
    let mut consumer = Channel::open(&name).expect("open").into_consumer();

    let payload = vec![0xCDu8; 1 << 15];
    let (mut written, mut dropped) = (0u64, 0u64);
    for _ in 0..8 {
        match producer.write_or_drop(&payload).expect("write_or_drop") {
            Written::Yes => written += 1,
            Written::Dropped => dropped += 1,
        }
    }
    assert!(written >= 1, "nothing fit at all — the test proves nothing");
    assert!(dropped >= 1, "the ring never filled — no drop was exercised");

    let stats = producer.stats().expect("stats");
    assert_eq!(stats.msgs_dropped, dropped);
    assert_eq!(stats.msgs_written, written);

    // A drop disturbs nothing already queued.
    for _ in 0..written {
        let msg = consumer.acquire_read().expect("acquire_read");
        assert_eq!(msg.as_slice(), &payload[..]);
    }

    // An oversized payload is still an error, never a silent drop.
    let too_big = vec![0u8; (1 << 15) + 1];
    assert_eq!(producer.write_or_drop(&too_big), Err(Error::TooBig));

    let _ = Channel::unlink(&name);
}

// --- v1.4 surface: page-aligned spans --------------------------------------

#[test]
fn aligned_spans_give_page_aligned_borrows() {
    let page = page_size();
    let name = unique_name();
    let mut producer =
        Channel::create_with(&name, 64 * page, 4096, CreateFlags::ALIGNED_SPANS)
            .expect("create")
            .into_producer();
    let mut consumer = Channel::open(&name).expect("open").into_consumer();

    for &len in &[0usize, 1, 100, 4096] {
        let payload: Vec<u8> = (0..len).map(|i| (i * 31 + 7) as u8).collect();
        // The reserved span is aligned too, not only the borrow.
        let mut res = producer.acquire_write(len).expect("acquire_write");
        assert_eq!(
            res.as_ptr() as usize % page,
            0,
            "reservation at {:p} is not page-aligned",
            res.as_ptr()
        );
        res.as_mut_slice().copy_from_slice(&payload);
        res.commit_all().expect("commit");

        let msg = consumer.acquire_read().expect("acquire_read");
        assert_eq!(
            msg.as_ptr() as usize % page,
            0,
            "payload at {:p} is not page-aligned",
            msg.as_ptr()
        );
        assert_eq!(msg.as_slice(), &payload[..]);
    }
    let _ = Channel::unlink(&name);
}

#[test]
fn an_unflagged_channel_is_not_page_aligned() {
    // The negative control. Without it, the assertions above could pass on a
    // build where the payload happened to land on a page for other reasons.
    let page = page_size();
    let mut p = Pair::new();
    let mut addresses = Vec::new();
    for _ in 0..2 {
        p.producer.write(&[0x5Au8; 64]).expect("write");
        let msg = p.consumer.acquire_read().expect("acquire_read");
        addresses.push(msg.as_ptr() as usize);
    }
    // The classic v1 data_offset (1280) is not a page multiple...
    assert_ne!(addresses[0] % page, 0);
    // ...and consecutive frames are 8 + len apart, not a padded stride.
    assert_eq!(addresses[1] - addresses[0], 8 + 64);
}

#[test]
fn aligned_frames_advance_by_the_padded_stride() {
    let page = page_size();
    let name = unique_name();
    let mut producer =
        Channel::create_with(&name, 64 * page, 4096, CreateFlags::ALIGNED_SPANS)
            .expect("create")
            .into_producer();
    let mut consumer = Channel::open(&name).expect("open").into_consumer();

    let mut addresses = Vec::new();
    for _ in 0..2 {
        producer.write(&[0x11u8; 1]).expect("write");
        let msg = consumer.acquire_read().expect("acquire_read");
        assert_eq!(msg.as_ptr() as usize % page, 0);
        addresses.push(msg.as_ptr() as usize);
    }
    // A 1-byte message occupies a whole header page plus a whole payload page:
    // the documented worst case, 2*page - 9 bytes of padding.
    assert_eq!(addresses[1] - addresses[0], 2 * page);
    let _ = Channel::unlink(&name);
}

#[test]
fn the_aligned_capacity_floor_is_the_padded_stride() {
    let page = page_size();
    // One frame for a (page + 1)-byte payload costs page + 2*page.
    assert_eq!(
        Channel::create_with(
            &unique_name(),
            3 * page - 1,
            page + 1,
            CreateFlags::ALIGNED_SPANS
        )
        .unwrap_err(),
        Error::CapacityTooSmall
    );
    // The same numbers are fine unflagged: the floor moved because the framing
    // did, not because the channel is small.
    let name = unique_name();
    drop(Channel::create(&name, 3 * page - 1, page + 1).expect("create"));
    let _ = Channel::unlink(&name);
}

#[test]
fn aligned_and_stats_compose() {
    let page = page_size();
    let name = unique_name();
    let flags = CreateFlags::ALIGNED_SPANS | CreateFlags::STATS;
    let mut producer = Channel::create_with(&name, 64 * page, 4096, flags)
        .expect("create")
        .into_producer();
    let mut consumer = Channel::open(&name).expect("open").into_consumer();

    let lengths = [1usize, 4095, 4096];
    for &n in &lengths {
        producer.write(&vec![0xEEu8; n]).expect("write");
        let msg = consumer.acquire_read().expect("acquire_read");
        assert_eq!(msg.len(), n);
        assert_eq!(msg.as_ptr() as usize % page, 0);
    }
    let stats = producer.stats().expect("stats");
    let total: u64 = lengths.iter().map(|n| *n as u64).sum();
    assert_eq!(stats.msgs_written, lengths.len() as u64);
    assert_eq!(stats.msgs_read, lengths.len() as u64);
    // Payload bytes, not the far larger padded stride.
    assert_eq!(stats.bytes_written, total);
    assert_eq!(stats.bytes_read, total);
    let _ = Channel::unlink(&name);
}

// --- v1.4 surface: file-backed channels ------------------------------------

/// A temp directory that removes itself, so a failing test cannot leave a
/// segment file behind. Built by hand rather than with a crate: this repo adds
/// no dependencies.
struct TempDir {
    path: std::path::PathBuf,
}

impl TempDir {
    fn new() -> TempDir {
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "shuttle-rs-fb-{}-{}",
            std::process::id(),
            n
        ));
        std::fs::create_dir_all(&path).expect("temp dir");
        TempDir { path }
    }

    /// An absolute path for a segment file inside this directory.
    fn seg(&self, tag: &str) -> String {
        self.path
            .join(format!("{}.seg", tag))
            .to_str()
            .expect("utf-8 temp path")
            .to_string()
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.path);
    }
}

#[test]
fn file_backed_channel_roundtrips() {
    let dir = TempDir::new();
    let path = dir.seg("roundtrip");
    let mut producer = Channel::create_file(&path, CAPACITY, MAX_PAYLOAD)
        .expect("create_file")
        .into_producer();
    let mut consumer = Channel::open_file(&path).expect("open_file").into_consumer();

    // The segment really is that file, sized for the geometry it claims.
    let meta = std::fs::metadata(&path).expect("stat");
    assert!(meta.len() >= CAPACITY as u64);

    let payload: Vec<u8> = (0..9999u32).map(|i| (i % 251) as u8).collect();
    producer.write(&payload).expect("write");
    {
        // Zero-copy, straight out of a file mapping.
        let msg = consumer.acquire_read().expect("acquire_read");
        assert_eq!(msg.as_slice(), &payload[..]);
    }
    // ...and the producer's borrow path over the same backing.
    {
        let mut res = producer.acquire_write(4096).expect("acquire_write");
        res.as_mut_slice().fill(0x5A);
        res.commit_all().expect("commit");
    }
    assert_eq!(consumer.read_owned().expect("read"), vec![0x5Au8; 4096]);

    drop(consumer);
    drop(producer);
    Channel::unlink_file(&path).expect("unlink_file");
    assert!(!std::path::Path::new(&path).exists());
    // Unlinking twice is NotFound, exactly as for a name.
    assert!(matches!(
        Channel::unlink_file(&path),
        Err(Error::NotFound)
    ));
}

#[test]
fn file_backed_and_stats_compose() {
    let dir = TempDir::new();
    let path = dir.seg("stats");
    let mut producer = Channel::create_file_with(&path, CAPACITY, MAX_PAYLOAD, CreateFlags::STATS)
        .expect("create_file")
        .into_producer();
    let mut consumer = Channel::open_file(&path).expect("open_file").into_consumer();

    let lengths = [1usize, 1000, 65536];
    for &n in &lengths {
        producer.write(&vec![0xC3u8; n]).expect("write");
        let msg = consumer.acquire_read().expect("acquire_read");
        assert_eq!(msg.len(), n);
    }
    let stats = consumer.stats().expect("stats");
    let total: u64 = lengths.iter().map(|n| *n as u64).sum();
    assert_eq!(stats.msgs_written, lengths.len() as u64);
    assert_eq!(stats.bytes_written, total);
    assert_eq!(stats.bytes_read, total);
    let _ = Channel::unlink_file(&path);
}

#[test]
fn open_file_missing_is_not_found() {
    let dir = TempDir::new();
    assert!(matches!(
        Channel::open_file(&dir.seg("nope")),
        Err(Error::NotFound)
    ));
    assert!(matches!(
        Channel::unlink_file(&dir.seg("nope")),
        Err(Error::NotFound)
    ));
}

#[test]
fn creating_a_file_backed_channel_twice_is_exists() {
    let dir = TempDir::new();
    let path = dir.seg("twice");
    let _first = Channel::create_file(&path, CAPACITY, MAX_PAYLOAD).expect("create_file");
    let size = std::fs::metadata(&path).expect("stat").len();
    assert!(matches!(
        Channel::create_file(&path, CAPACITY, MAX_PAYLOAD),
        Err(Error::Exists)
    ));
    // The existing file is untouched — that refusal is the stale-file recovery
    // point, not a truncate-and-retry.
    assert_eq!(std::fs::metadata(&path).expect("stat").len(), size);
    let _ = Channel::unlink_file(&path);
}

#[test]
fn non_absolute_paths_are_invalid_args() {
    for bad in ["relative/chan.seg", "chan.seg", ""] {
        assert!(matches!(
            Channel::create_file(bad, CAPACITY, MAX_PAYLOAD),
            Err(Error::InvalidArgs)
        ));
        assert!(matches!(Channel::open_file(bad), Err(Error::InvalidArgs)));
        assert!(matches!(Channel::unlink_file(bad), Err(Error::InvalidArgs)));
    }
}

#[test]
fn file_backed_rejects_hugetlb_bits() {
    // A hugetlbfs backing and a caller-chosen path name two different segments.
    let dir = TempDir::new();
    for flags in [CreateFlags::HUGETLB_2MB, CreateFlags::HUGETLB_1GB] {
        let path = dir.seg("hugetlb");
        assert!(matches!(
            Channel::create_file_with(&path, CAPACITY, MAX_PAYLOAD, flags),
            Err(Error::InvalidArgs)
        ));
        assert!(!std::path::Path::new(&path).exists());
    }
}

#[test]
fn create_with_masks_the_file_backed_bit() {
    // The documented asymmetry: 0x20 is not selectable through create_with,
    // which takes an shm name and has nowhere to put a path. It is masked off
    // silently, per the unknown-bit rule — not an error.
    let name = unique_name();
    let mut producer = Channel::create_with(
        &name,
        CAPACITY,
        MAX_PAYLOAD,
        CreateFlags::from_bits(shuttle_sys::SHUTTLE_CREATE_FILE_BACKED),
    )
    .expect("create")
    .into_producer();
    producer.write(b"an ordinary shm channel").expect("write");
    let mut consumer = Channel::open(&name).expect("open").into_consumer();
    let msg = consumer.acquire_read().expect("acquire_read");
    assert_eq!(msg.as_slice(), b"an ordinary shm channel");
    drop(msg);
    drop(consumer);
    drop(producer);
    let _ = Channel::unlink(&name);
}

#[test]
fn unknown_create_bits_are_masked_not_rejected() {
    // An older library must tolerate a flag it does not implement.
    let name = unique_name();
    let mut producer = Channel::create_with(
        &name,
        CAPACITY,
        MAX_PAYLOAD,
        CreateFlags::from_bits(1 << 30) | CreateFlags::NONE,
    )
    .expect("create")
    .into_producer();
    producer.write(b"still works").expect("write");
    let mut consumer = Channel::open(&name).expect("open").into_consumer();
    let msg = consumer.acquire_read().expect("acquire_read");
    assert_eq!(msg.as_slice(), b"still works");
    let _ = Channel::unlink(&name);
}

// --- v1.4 surface: the read-only lookahead ---------------------------------

#[test]
fn peek_next_reports_the_next_message_length() {
    let mut p = Pair::new();

    // Nothing committed: the ordinary "not yet", as Ok(None) rather than an
    // error — that is the whole reason the return is an Option.
    assert_eq!(p.consumer.peek_next().expect("peek"), None);

    p.producer.write(&vec![b'x'; 1234]).expect("write");
    assert_eq!(p.consumer.peek_next().expect("peek"), Some(1234));
    // Read-only: asking twice consumes nothing.
    assert_eq!(p.consumer.peek_next().expect("peek"), Some(1234));

    let msg = p.consumer.acquire_read().expect("acquire_read");
    assert_eq!(msg.len(), 1234);
    drop(msg);
    assert_eq!(p.consumer.peek_next().expect("peek"), None);
}

#[test]
fn peek_next_sees_past_a_live_borrow() {
    // The case with no other API, and the one that decides the signatures: a
    // live `Borrowed` holds the `Consumer` exclusively, so the peek has to be
    // reachable THROUGH it. Both spellings are exercised here.
    let mut p = Pair::new();
    p.producer.write(b"first").expect("write");

    let msg = p.consumer.acquire_read().expect("acquire_read");
    assert_eq!(msg.as_slice(), b"first");
    assert_eq!(msg.peek_next().expect("peek"), None);

    // A second message arrives while the first is still borrowed.
    p.producer.write(b"second message").expect("write");
    assert_eq!(msg.peek_next().expect("peek"), Some(14));
    // Peeking did not disturb the borrow: the slice is still the first message.
    assert_eq!(msg.as_slice(), b"first");
    // ...and it did not consume it either — this borrow still owns message 1.
    drop(msg);

    // With nothing borrowed, the same answer through the Consumer.
    assert_eq!(p.consumer.peek_next().expect("peek"), Some(14));
    let msg = p.consumer.acquire_read().expect("acquire_read");
    assert_eq!(msg.as_slice(), b"second message");
    drop(msg);
    assert_eq!(p.consumer.peek_next().expect("peek"), None);
}

#[test]
fn peek_next_keeps_up_across_wraps() {
    // Long enough to lap the ring many times: peek must track the wrapped
    // state (where the next message can sit at offset 0 while the borrow is
    // still in the high region) exactly as the acquire path does.
    let mut p = Pair::new();
    for i in 0..500usize {
        let n = (i * 7919) % 30_000 + 1;
        p.producer.write(&vec![b'w'; n]).expect("write");
        assert_eq!(p.consumer.peek_next().expect("peek"), Some(n as u64));
        let msg = p.consumer.acquire_read().expect("acquire_read");
        assert_eq!(msg.len(), n);
        assert_eq!(msg.peek_next().expect("peek"), None);
    }
    assert_eq!(p.consumer.peek_next().expect("peek"), None);
}
