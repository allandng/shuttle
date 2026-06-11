// G6.3 Rust probe: an induced error must arrive as the documented integer
// code through the safe wrapper — Err(-4), never a panic. A panic would
// abort this process nonzero; clean exit 0 is the no-panic proof.
mod shuttle;

const ERR_NOT_FOUND: i32 = -4;

fn main() {
    match shuttle::Consumer::open("/shnx.does-not-exist") {
        Ok(_) => {
            eprintln!("rust: nonexistent segment unexpectedly opened");
            std::process::exit(1);
        }
        Err(e) if e == ERR_NOT_FOUND => {
            println!("rust: kErrNotFound surfaced as {}, no panic", e);
        }
        Err(e) => {
            eprintln!("rust: wrong error code {}", e);
            std::process::exit(1);
        }
    }
}
