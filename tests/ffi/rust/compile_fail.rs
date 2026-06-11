// G6.2 negative proof: this file MUST NOT COMPILE. The payload slice is
// tied to the Borrowed's lifetime; using it after the borrow drops (which
// performs shuttle_release_read) is use-after-release and the borrow
// checker must reject it (E0597: borrowed value does not live long enough).
// The test driver compiles this expecting failure with that diagnostic.
mod shuttle;

fn main() {
    let mut c = shuttle::Consumer::open("/cf").unwrap();
    let stale;
    {
        let b = c.acquire_read().unwrap();
        stale = b.as_slice();
    } // b drops: release_read runs; `stale` must not survive this
    println!("{}", stale[0]); // E0597 expected here
}
