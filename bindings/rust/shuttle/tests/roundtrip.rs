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

use shuttle::{Channel, Consumer, CreateFlags, Error, Producer};

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
    for code in -13..=-1 {
        assert_eq!(Error::from_code(code).code(), code);
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
}
