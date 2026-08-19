#![feature(restricted_std)]
#![feature(thread_local)]
#![no_main]

extern crate alloc;

mod native_smoke;

use naos_runtime::{Bootstrap, InitialStack};
use std::cell::Cell;
use std::env;
use std::io::ErrorKind;
use std::process;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Condvar, Mutex, Once, RwLock};
use std::thread;
use std::time::{Duration, Instant, SystemTime};

static TLS_DROPS: AtomicU32 = AtomicU32::new(0);

struct TlsDrop;

impl Drop for TlsDrop {
    fn drop(&mut self) {
        TLS_DROPS.fetch_add(1, Ordering::Release);
    }
}

thread_local! {
    static TLS_VALUE: Cell<u64> = const { Cell::new(0x11) };
    static TLS_DROP: TlsDrop = const { TlsDrop };
}

fn marker(message: &'static [u8]) {
    native_smoke::log(message);
}

fn run() {
    marker(b"rust-smoke-suite: std env start\0");
    assert!(env::args_os().next().is_some());
    let _env: Vec<_> = env::vars_os().collect();
    unsafe { env::set_var("NAOS_STD_SMOKE", "enabled") };
    assert_eq!(
        env::var_os("NAOS_STD_SMOKE").as_deref(),
        Some(std::ffi::OsStr::new("enabled"))
    );
    unsafe { env::remove_var("NAOS_STD_SMOKE") };
    assert!(env::var_os("NAOS_STD_SMOKE").is_none());
    marker(b"rust-smoke-suite: std env ready\0");
    let _now = SystemTime::now();
    let start = Instant::now();
    assert_ne!(process::id(), 0);

    TLS_VALUE.with(|value| assert_eq!(value.replace(0x22), 0x11));
    TLS_DROP.with(|_| {});
    unsafe { env::remove_var("RUST_MIN_STACK") };
    marker(b"rust-smoke-suite: std tls ready\0");

    let child_value = Arc::new(AtomicU32::new(0));
    let child_value_ref = Arc::clone(&child_value);
    let parked = match thread::Builder::new()
        .name("naos-std-child".to_owned())
        .spawn(move || {
            TLS_VALUE.with(|tls| assert_eq!(tls.replace(0x33), 0x11));
            TLS_DROP.with(|_| {});
            child_value_ref.store(0x33, Ordering::Release);
            thread::yield_now();
            thread::park_timeout(Duration::from_millis(100));
            thread::sleep(Duration::from_micros(1));
        }) {
        Ok(thread) => thread,
        Err(error) => {
            marker(b"rust-smoke-suite: std thread spawn failed\0");
            core::mem::forget(error);
            std::process::exit(2);
        }
    };
    marker(b"rust-smoke-suite: std thread spawned\0");
    assert_eq!(parked.thread().name(), Some("naos-std-child"));
    parked.thread().unpark();
    parked.join().expect("std smoke park");
    marker(b"rust-smoke-suite: std thread joined\0");

    assert_eq!(child_value.load(Ordering::Acquire), 0x33);
    TLS_VALUE.with(|value| assert_eq!(value.get(), 0x22));
    assert!(start.elapsed() >= Duration::from_nanos(0));
    assert!(TLS_DROPS.load(Ordering::Acquire) >= 1);

    let mutex_value = Arc::new(Mutex::new(0_u32));
    let mutex_child = Arc::clone(&mutex_value);
    thread::spawn(move || {
        *mutex_child.lock().unwrap() = 7;
    })
    .join()
    .expect("std smoke mutex");
    marker(b"rust-smoke-suite: std mutex ready\0");
    assert_eq!(*mutex_value.lock().unwrap(), 7);

    let condition = Arc::new((Mutex::new(false), Condvar::new()));
    let condition_child = Arc::clone(&condition);
    let waiter = thread::spawn(move || {
        let (lock, condvar) = &*condition_child;
        let mut ready = lock.lock().unwrap();
        while !*ready {
            ready = condvar.wait(ready).unwrap();
        }
    });
    {
        let (lock, condvar) = &*condition;
        *lock.lock().unwrap() = true;
        condvar.notify_one();
    }
    waiter.join().expect("std smoke condvar");
    marker(b"rust-smoke-suite: std condvar ready\0");

    let once = Arc::new(Once::new());
    let once_child = Arc::clone(&once);
    let once_count = Arc::new(AtomicU32::new(0));
    let once_count_child = Arc::clone(&once_count);
    let once_thread = thread::spawn(move || {
        once_child.call_once(|| {
            once_count_child.fetch_add(1, Ordering::AcqRel);
        });
    });
    once.call_once(|| {
        once_count.fetch_add(1, Ordering::AcqRel);
    });
    once_thread.join().expect("std smoke once");
    assert_eq!(once_count.load(Ordering::Acquire), 1);
    marker(b"rust-smoke-suite: std once ready\0");

    let rwlock = Arc::new(RwLock::new(0_u32));
    let rwlock_child = Arc::clone(&rwlock);
    thread::spawn(move || {
        *rwlock_child.write().unwrap() = 9;
    })
    .join()
    .expect("std smoke rwlock");
    marker(b"rust-smoke-suite: std rwlock ready\0");
    assert_eq!(*rwlock.read().unwrap(), 9);

    assert_eq!(
        std::fs::File::open("/unsupported").unwrap_err().kind(),
        ErrorKind::Unsupported
    );
    assert_eq!(
        std::net::TcpStream::connect(("127.0.0.1", 1))
            .unwrap_err()
            .kind(),
        ErrorKind::Unsupported
    );
    assert_eq!(
        process::Command::new("unsupported")
            .status()
            .unwrap_err()
            .kind(),
        ErrorKind::Unsupported
    );
    marker(b"rust-smoke-suite: std unsupported ready\0");
    let peer_closed = std::os::naos::io::from_status(13);
    assert_eq!(peer_closed.kind(), ErrorKind::BrokenPipe);
    assert_eq!(
        std::os::naos::io::StatusError::from_io(&peer_closed)
            .unwrap()
            .status(),
        13
    );
    marker(b"rust-smoke-suite: std ready\0");
    std::process::exit(0);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn naos_app_main(
    stack: *const InitialStack,
    bootstrap: *const Bootstrap,
) -> i64 {
    if stack.is_null() || bootstrap.is_null() {
        return 1;
    }
    if native_smoke::run(stack, bootstrap) != 0 {
        process::exit(3);
    }
    run();
    0
}
