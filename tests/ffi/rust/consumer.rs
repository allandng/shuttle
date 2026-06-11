// G6.2 Rust consumer: drains N seeded messages over the zero-copy borrow
// path and verifies byte-exact FIFO IN PLACE (no to_vec — copying would
// silently defeat the zero-copy contract).
//
// usage: rust_consumer </shm-name> <nmsgs> <seed> <max_payload>
mod shuttle;

fn splitmix(x: u64) -> u64 {
    let x = x.wrapping_add(0x9E3779B97F4A7C15);
    let mut z = x;
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D049BB133111EB);
    z ^ (z >> 31)
}

fn msg_len(seed: u64, i: u64, max_payload: u64) -> u64 {
    splitmix(seed ^ i) % (max_payload + 1)
}

fn fill_byte(msg: u64, j: u64) -> u8 {
    (msg.wrapping_mul(1315423911)
        .wrapping_add(j.wrapping_mul(151))
        .wrapping_add(j >> 8)) as u8
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 5 {
        eprintln!("usage: {} </name> <nmsgs> <seed> <maxp>", args[0]);
        std::process::exit(2);
    }
    let name = &args[1];
    let nmsgs: u64 = args[2].parse().unwrap();
    let seed: u64 = args[3].parse().unwrap();
    let maxp: u64 = args[4].parse().unwrap();

    let mut c = match shuttle::Consumer::open(name) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("rust_consumer: open failed err={}", e);
            std::process::exit(1);
        }
    };

    for i in 0..nmsgs {
        let b = match c.acquire_read() {
            Ok(b) => b,
            Err(e) => {
                eprintln!("rust_consumer: acquire {} rc={}", i, e);
                std::process::exit(1);
            }
        };
        let want = msg_len(seed, i, maxp);
        if b.len() as u64 != want {
            eprintln!("rust_consumer: msg {} len {} != {} (FIFO?)", i,
                      b.len(), want);
            std::process::exit(1);
        }
        let s = b.as_slice(); // zero-copy view into the segment
        let mut bad = 0u64;
        for (j, &byte) in s.iter().enumerate() {
            if byte != fill_byte(i, j as u64) {
                bad += 1;
            }
        }
        if bad != 0 {
            eprintln!("rust_consumer: msg {} has {} corrupt bytes", i, bad);
            std::process::exit(1);
        }
        // b drops here: shuttle_release_read. The borrow checker forbids
        // `s` (or `b`) escaping this scope — that proof is compile_fail.rs.
    }
    println!(
        "rust_consumer: {} msgs byte-exact over zero-copy borrow path",
        nmsgs
    );
}
