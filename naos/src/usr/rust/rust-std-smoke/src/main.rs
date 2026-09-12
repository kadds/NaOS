#![cfg_attr(target_os = "naos", feature(thread_local))]

#[cfg(target_os = "naos")]
#[tokio::main]
async fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(servicekit::run("rust-smoke-suite", naos_entry::entry).await as u8)
}

#[cfg(not(target_os = "naos"))]
fn main() {}

extern crate alloc;

#[cfg(target_os = "naos")]
#[path = "native_smoke.rs"]
mod native_smoke;

#[cfg(target_os = "naos")]
mod naos_entry {

    use super::native_smoke;

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

    /// Exercise the NaOS Tokio reactor bootstrap. `enable_io` constructs the
    /// custom Mio selector and channel-backed waker; the yield verifies that the
    /// runtime can drive a future without depending on Linux sockets or proc
    /// macros.
    async fn run_tokio_smoke() {
        tokio::task::yield_now().await;
        marker(b"rust-smoke-suite: tokio naos reactor ready\0");
    }

    /// `HashMap` uses the same NaOS `std::sys::random` path as Tokio's runtime.
    /// Keeping this before filesystem initialization makes the random ABI
    /// independently observable in the early-service bootstrap environment.
    fn run_random_smoke() {
        let mut map = std::collections::HashMap::new();
        map.insert(1_u64, 2_u64);
        assert_eq!(map.get(&1), Some(&2));
        marker(b"rust-smoke-suite: std random ready\0");
    }

    /// Exercise the upstream `byteorder` crate through its standard-library IO
    /// extension traits. This must compile and run on NaOS without a crate-local
    /// `restricted_std` opt-in or a NaOS-specific byteorder fork.
    fn run_byteorder_smoke() {
        use byteorder::{LittleEndian, ReadBytesExt, WriteBytesExt};
        use std::io::Cursor;

        let mut bytes = [0_u8; 8];
        {
            let mut writer = Cursor::new(&mut bytes[..]);
            writer
                .write_u32::<LittleEndian>(0x1234_5678)
                .expect("byteorder write");
            writer
                .write_u32::<LittleEndian>(0x90ab_cdef)
                .expect("byteorder write");
        }
        let mut reader = Cursor::new(bytes);
        assert_eq!(
            reader.read_u32::<LittleEndian>().expect("byteorder read"),
            0x1234_5678
        );
        assert_eq!(
            reader.read_u32::<LittleEndian>().expect("byteorder read"),
            0x90ab_cdef
        );
        marker(b"rust-smoke-suite: byteorder std ready\0");
    }

    /// Phase-3 std::fs coverage: every diagnostic goes through the native
    /// `_s_log` path AND the stdout Stream (dual-channel rule) via `marker`.
    fn run_fs_smoke() {
        use std::fs;
        use std::io::{Read, Seek, SeekFrom, Write};
        use std::os::naos::fs::{MetadataExt, symlink as naos_symlink};

        marker(b"rust-smoke-suite: std fs start\0");

        // Fresh scratch directory; tolerate leftovers from an earlier boot.
        let base = std::path::Path::new("/rust-smoke-fs");
        let _ = fs::remove_dir_all(base);
        fs::create_dir(base).expect("fs create_dir");
        let dir_meta = fs::metadata(base).expect("fs metadata(dir)");
        assert!(dir_meta.is_dir(), "created path must be a directory");
        marker(b"rust-smoke-suite: std fs create-dir ready\0");

        // Regular file lifecycle: create, write, sync, read, seek, size.
        let file_path = base.join("hello.txt");
        {
            let mut file = fs::OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .truncate(true)
                .open(&file_path)
                .expect("fs open read-write");
            file.write_all(b"hello naos").expect("fs write_all");
            file.sync_all().expect("fs sync_all");
            file.seek(SeekFrom::Start(0)).expect("fs seek start");
            let mut back = String::new();
            file.read_to_string(&mut back).expect("fs read_to_string");
            assert_eq!(back, "hello naos", "read-back must match written bytes");
        }
        let file_meta = fs::metadata(&file_path).expect("fs metadata(file)");
        assert!(file_meta.is_file());
        assert_eq!(file_meta.len(), 10, "stat size must reflect written bytes");
        marker(b"rust-smoke-suite: std fs write-read-sync ready\0");

        // Symlinks: symlink_metadata reports LNK, read_link returns the target,
        // plain metadata follows to the regular file.
        let link_path = base.join("hello.lnk");
        naos_symlink("hello.txt", &link_path).expect("fs symlink");
        let link_meta = fs::symlink_metadata(&link_path).expect("fs symlink_metadata");
        assert!(link_meta.is_symlink(), "symlink_metadata must report LNK");
        assert_eq!(
            fs::read_link(&link_path).expect("fs read_link"),
            std::path::Path::new("hello.txt")
        );
        assert!(
            fs::metadata(&link_path)
                .expect("fs metadata(link)")
                .is_file()
        );
        marker(b"rust-smoke-suite: std fs symlink ready\0");

        // Rename: source disappears with ENOENT and the negated errno is
        // preserved in raw_os_error.
        let renamed = base.join("renamed.txt");
        fs::rename(&file_path, &renamed).expect("fs rename");
        marker(b"rust-smoke-suite: std fs rename ready\0");
        let gone = fs::metadata(&file_path).unwrap_err();
        assert_eq!(gone.kind(), ErrorKind::NotFound, "ENOENT maps to NotFound");
        assert_eq!(gone.raw_os_error(), Some(-2), "raw errno preservation");
        // Hard link: both names share one inode (nlink >= 2).
        let linked = base.join("linked.txt");
        fs::hard_link(&renamed, &linked).expect("fs hard_link");
        let link_count = fs::metadata(&linked)
            .expect("fs metadata(hardlink)")
            .nlink();
        assert!(
            link_count >= 2,
            "hard link must raise nlink, got {link_count}"
        );
        assert_eq!(
            fs::metadata(&renamed)
                .expect("fs metadata(hardlink src)")
                .ino(),
            fs::metadata(&linked)
                .expect("fs metadata(hardlink dst)")
                .ino(),
            "hard links must share one inode",
        );
        marker(b"rust-smoke-suite: std fs rename-hardlink ready\0");

        // Directory iteration: cursor-based list with typed entries.
        let mut seen = Vec::new();
        for entry in fs::read_dir(base).expect("fs read_dir") {
            let entry = entry.expect("read_dir entry");
            seen.push(entry.file_name().to_string_lossy().into_owned());
            let file_type = entry.file_type().expect("entry file type");
            // The earlier rename intentionally leaves hello.lnk dangling; std's
            // DirEntry::metadata follows links, so only non-links are expected
            // to have successful follow-up metadata here.
            if !file_type.is_symlink() {
                let _ = entry.metadata().expect("entry metadata").is_file();
            }
        }
        for expected in ["hello.lnk", "renamed.txt", "linked.txt"] {
            assert!(
                seen.iter().any(|name| name == expected),
                "missing {expected} in {seen:?}"
            );
        }
        marker(b"rust-smoke-suite: std fs read-dir ready\0");

        // chdir/current_dir round trip through the runtime cwd binding.
        let previous = env::current_dir().expect("current_dir");
        env::set_current_dir(base).expect("set_current_dir");
        let now = env::current_dir().expect("current_dir after chdir");
        assert!(
            now.ends_with(base.file_name().unwrap_or(base.as_os_str())),
            "cwd must move into the scratch dir, got {now:?}"
        );
        let relative = fs::File::create("relative.txt").expect("relative create after chdir");
        drop(relative);
        assert!(
            fs::metadata("relative.txt").is_ok(),
            "relative open uses new cwd"
        );
        env::set_current_dir(previous).expect("restore cwd");
        marker(b"rust-smoke-suite: std fs chdir ready\0");

        // Cleanup proves remove_dir_all walks the same protocol surface.
        fs::remove_dir_all(base).expect("fs remove_dir_all");
        marker(b"rust-smoke-suite: std fs cleanup ready\0");
        assert_eq!(
            fs::metadata(base).unwrap_err().kind(),
            ErrorKind::NotFound,
            "scratch dir must be gone"
        );
    }

    fn run() -> ! {
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

        // With the Phase-3 PAL wiring, missing paths surface as typed ENOENT
        // protocol failures mapped to NotFound with the negated errno preserved.
        let missing = std::fs::metadata("/definitely-missing");
        assert_eq!(
            missing.unwrap_err().kind(),
            ErrorKind::NotFound,
            "missing path must map to NotFound"
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
        run_fs_smoke();
        run_byteorder_smoke();
        marker(b"rust-smoke-suite: std fs ready\0");
        marker(b"rust-smoke-suite: std ready\0");
        std::process::exit(0);
    }

    pub(super) async fn entry(context: servicekit::Context) -> i64 {
        if native_smoke::run(context) != 0 {
            process::exit(3);
        }
        run_random_smoke();
        run_tokio_smoke().await;
        // MOVE the bootstrap root/cwd Directory bindings into the standard
        // library before any std::fs use (std::os::naos::init_fs). The runtime
        // owns this transition; the smoke itself never sees its bootstrap.
        let Some((root, cwd)) = servicekit::take_root_and_current() else {
            marker(b"rust-smoke-suite: std fs namespace context unavailable\0");
            process::exit(4);
        };
        if std::os::naos::init_fs(root, cwd).is_err() {
            marker(b"rust-smoke-suite: std fs namespace init failed\0");
            process::exit(4);
        }
        run();
    }
}
