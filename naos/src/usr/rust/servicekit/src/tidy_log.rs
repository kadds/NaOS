//! Logging over the bootstrap stdout Stream client.
//!
//! Every early service logs through one route: duplicate the cached stdout
//! stream handle, copy the line into a reusable MemoryObject region, submit a
//! `Stream.write` request naming that region as resource slot 0, wait for
//! `SIGNAL_COMPLETED`, and take the reply. This module is the shared
//! implementation of that loop (previously copied into `vfsd/src/main.rs`
//! and `ramdiskd/src/main.rs`).
//!
//! The kernel-facing operations live behind [`StreamOps`] so host unit tests
//! drive the exact same chunking/emit logic against a fake sink instead of
//! real syscalls. [`NativeStreamOps`] is the production implementation.

use crate::memory::MemoryObject;
use core::cell::RefCell;
use core::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};

use alloc::format;
use alloc::string::String;
use naos_idl::stream;
use naos_idl::{CallError, ProtocolClientEndpoint, ResourceTable};
use naos_sys as sys;

/// Fixed wire overhead of one `Stream.write` request: the generated header
/// naming the caller's region and window (`stream::WRITE_REQUEST_HEADER_BYTES`).
pub const STREAM_WRITE_HEADER_BYTES: usize = stream::WRITE_REQUEST_HEADER_BYTES;

/// Largest payload one `Stream.write` round trip carries. The payload itself
/// lives in the caller's MemoryObject region, so this is a chunking policy
/// rather than a wire limit: a message larger than this is split by
/// [`stream_chunks`], which bounds the region the logger grows.
pub const STREAM_WRITE_MAX_PAYLOAD: usize = 65536 - STREAM_WRITE_HEADER_BYTES;

/// Split `message` into consecutive chunks each no larger than
/// [`STREAM_WRITE_MAX_PAYLOAD`], in order. Empty input yields one empty
/// chunk so a zero-length write still round trips (matching the historical
/// `write_request { size: 0 }` behavior of the pre-consolidation services).
pub fn stream_chunks(message: &[u8]) -> impl Iterator<Item = &[u8]> {
    if message.is_empty() {
        EitherChunks::Single(core::iter::once(&[][..]))
    } else {
        EitherChunks::Chunked(message.chunks(STREAM_WRITE_MAX_PAYLOAD))
    }
}

/// `_s_log` accepts a NUL-terminated C string and the kernel reads at most
/// 4096 bytes including that terminator.  Keep this framing beside the
/// logger instead of duplicating a service-local `native_log` module.
pub const NATIVE_LOG_MAX_PAYLOAD: usize = 4095;
pub const NATIVE_LOG_BUFFER_BYTES: usize = NATIVE_LOG_MAX_PAYLOAD + 1;

pub fn native_log_frame(message: &[u8], buffer: &mut [u8; NATIVE_LOG_BUFFER_BYTES]) -> usize {
    let length = message.len().min(NATIVE_LOG_MAX_PAYLOAD);
    buffer[..length].copy_from_slice(&message[..length]);
    buffer[length] = 0;
    length
}

pub fn native_log_chunks(message: &[u8]) -> impl Iterator<Item = &[u8]> {
    if message.is_empty() {
        NativeLogChunks::Empty(core::iter::once(&[][..]))
    } else {
        NativeLogChunks::Message(message.chunks(NATIVE_LOG_MAX_PAYLOAD))
    }
}

enum NativeLogChunks<'a> {
    Empty(core::iter::Once<&'a [u8]>),
    Message(core::slice::Chunks<'a, u8>),
}

impl<'a> Iterator for NativeLogChunks<'a> {
    type Item = &'a [u8];

    fn next(&mut self) -> Option<Self::Item> {
        match self {
            Self::Empty(iter) => iter.next(),
            Self::Message(iter) => iter.next(),
        }
    }
}

enum EitherChunks<'a> {
    Single(core::iter::Once<&'a [u8]>),
    Chunked(core::slice::Chunks<'a, u8>),
}

impl<'a> Iterator for EitherChunks<'a> {
    type Item = &'a [u8];

    fn next(&mut self) -> Option<Self::Item> {
        match self {
            EitherChunks::Single(iter) => iter.next(),
            EitherChunks::Chunked(chunks) => chunks.next(),
        }
    }
}

/// Why one emit round trip failed. Diagnostics only; logging is best-effort
/// and never panics or blocks boot on its own failures.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WriteFailure {
    /// No stdout handle installed yet (`tidy_log::init` not called).
    Uninitialized,
    /// `_na_handle_duplicate` failed or returned an invalid handle.
    DuplicateFailed(sys::Status),
    /// `Stream.write` invocation could not be submitted (`CallError` code).
    SubmitFailed(i64),
    /// Wait failed, timed out, or observed `PEER_CLOSED` without `COMPLETED`.
    WaitFailed,
    /// `take_write` rejected the reply.
    TakeFailed,
}

/// Map a generated transport error to its flat diagnostic code (mirrors
/// ramdiskd's `call_error_code`: status when available, else -1).
pub fn call_error_code(error: CallError) -> i64 {
    match error {
        CallError::Status(status) => status as i64,
        _ => -1,
    }
}

/// Kernel-facing operations the logger needs from the bootstrap stdout
/// stream, split out so host tests can fake them without syscalls.
pub trait StreamOps {
    /// Duplicate a stream handle (kernel HANDLE_DUPLICATE).
    fn duplicate(&self, handle: sys::Handle) -> Result<sys::Handle, WriteFailure>;

    /// One submit_write/take_write round trip carrying `data`.
    fn write(&self, endpoint: sys::Handle, data: &[u8]) -> Result<(), WriteFailure>;

    /// Release a handle previously returned by [`StreamOps::duplicate`].
    fn close(&self, handle: sys::Handle);
}

/// Production [`StreamOps`] over the real kernel surface.
pub struct NativeStreamOps;

/// Reusable region backing one thread's `Stream.write` calls.
///
/// A log line is copied into the region and only its window is named by the
/// control message, so a per-call object would pay a MEMORY_CREATE/MAP (NaOS)
/// or memfd syscall for every line.  The object is created on first use,
/// grown when a chunk exceeds it, and never shrunk.  It is per-thread rather
/// than per-process because emission runs from whichever runtime thread hits
/// a `log` call, and sharing one object would either need a lock held across
/// the invocation or a `Sync` region type; the region is neither.
struct StreamRegion {
    object: Option<MemoryObject>,
    window: usize,
}

impl StreamRegion {
    const fn new() -> Self {
        Self {
            object: None,
            window: 0,
        }
    }

    /// Region holding at least `bytes`, growing the object when needed.
    fn acquire(&mut self, bytes: usize) -> Result<&MemoryObject, WriteFailure> {
        let needed = bytes.max(1);
        if self.object.is_none() || self.window < needed {
            let allocation = crate::memory::page_aligned_size(needed)
                .map_err(|_| WriteFailure::SubmitFailed(-1))?;
            let object =
                MemoryObject::new(allocation).map_err(|_| WriteFailure::SubmitFailed(-1))?;
            // The logger writes a whole chunk before each call, so the mapping
            // must be writable and shared for the service to observe it.
            object
                .map_persistent(allocation, true)
                .map_err(|_| WriteFailure::SubmitFailed(-1))?;
            self.object = Some(object);
            self.window = allocation;
        }
        self.object
            .as_ref()
            .ok_or(WriteFailure::SubmitFailed(-1))
    }
}

std::thread_local! {
    static STREAM_REGION: RefCell<StreamRegion> = const { RefCell::new(StreamRegion::new()) };
}

impl StreamOps for NativeStreamOps {
    fn duplicate(&self, handle: sys::Handle) -> Result<sys::Handle, WriteFailure> {
        let mut duplicate = sys::HANDLE_INVALID;
        // SAFETY: syscall wrapper with out-pointer contract.
        let status = unsafe { sys::_na_handle_duplicate(handle, 0, &mut duplicate) };
        if status != sys::STATUS_OK || duplicate == sys::HANDLE_INVALID {
            return Err(WriteFailure::DuplicateFailed(status));
        }
        Ok(duplicate)
    }

    fn write(&self, endpoint: sys::Handle, data: &[u8]) -> Result<(), WriteFailure> {
        // SAFETY: `endpoint` is a freshly duplicated client end uniquely
        // owned for this call; the wrapper below closes it on drop.
        let client = unsafe { ProtocolClientEndpoint::from_raw(endpoint) };
        // The window is the chunk itself; the object may be larger from an
        // earlier line, and a window inside the object is all the service
        // needs.
        STREAM_REGION.with(|cell| {
            let mut region = cell.borrow_mut();
            let object = region.acquire(data.len())?;
            if !data.is_empty() && object.write_region(0, data).is_err() {
                return Err(WriteFailure::SubmitFailed(-1));
            }
            let Some(owner) = object.native_handle() else {
                // A Linux host has no capability table to transfer the region
                // through; the bootstrap stdout stream is a NaOS-only path, so
                // report the failure instead of inventing a descriptor here.
                return Err(WriteFailure::SubmitFailed(-1));
            };
            let mut resources = ResourceTable::new();
            let Ok(buffer) = resources.push_duplicate(owner) else {
                return Err(WriteFailure::SubmitFailed(-1));
            };
            let request = stream::write_request {
                size: data.len() as u64,
                flags: 0,
                buffer,
            };
            // The request envelope is the generated fixed header only; the
            // payload travel is the region.
            let mut request_wire = [0_u8; STREAM_WRITE_HEADER_BYTES];
            let mut invocation = match stream::submit_write(
                &client,
                &request,
                resources,
                &mut request_wire,
                0,
            ) {
                Ok(invocation) => invocation,
                Err(error) => return Err(WriteFailure::SubmitFailed(call_error_code(error))),
            };

            let completed = crate::wait_for_completion(invocation.get(), u64::MAX);
            if !completed {
                return Err(WriteFailure::WaitFailed);
            }

            let mut response_wire = [0_u8; 32];
            match stream::take_write(&mut invocation, &mut response_wire) {
                // A short write would silently truncate a diagnostic; the
                // chunk is the whole line, so anything else is a protocol
                // failure.
                Ok(response) if response.count == data.len() as u64 => Ok(()),
                Ok(_) => Err(WriteFailure::TakeFailed),
                Err(_) => Err(WriteFailure::TakeFailed),
            }
        })
    }

    fn close(&self, handle: sys::Handle) {
        // SAFETY: plain close syscall; invalid handles are rejected, not fatal.
        unsafe { sys::_na_handle_close(handle) };
    }
}
/// Zero-allocation lowercase hex rendering of a `u64`, always 16 digits
/// (replaces ramdiskd's manual hex loop and vfsd's trace_hex).
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Hex(pub u64);

impl core::fmt::Display for Hex {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        const DIGITS: &[u8; 16] = b"0123456789abcdef";
        let mut buffer = [0_u8; 16];
        for (index, digit) in buffer.iter_mut().enumerate() {
            let shift = (15 - index) * 4;
            *digit = DIGITS[((self.0 >> shift) & 0xf) as usize];
        }
        formatter.write_str(core::str::from_utf8(&buffer).expect("ascii"))
    }
}

/// Format `{tag}: {message}\n` — the standard service info-line shape.
pub fn format_line(tag: &str, message: &str) -> String {
    format!("{tag}: {message}\n")
}

/// Format `{tag}: fatal: {context}\n` — the standard boot-failure line.
pub fn format_fatal(tag: &str, context: &str) -> String {
    format!("{tag}: fatal: {context}\n")
}

/// Linux file/stderr sink used by the std daemon entry points.  The service
/// logic only receives a small cloned writer; it does not know whether the
/// bytes end up in a file or on stderr.
#[cfg(target_os = "linux")]
pub mod linux {
    use alloc::format;
    use std::fs::{File, OpenOptions};
    use std::io::{self, Write};
    use std::path::Path;
    use std::sync::{Arc, Mutex, OnceLock};

    enum Destination {
        File(File),
        Stderr,
    }

    /// Thread-safe line logger.  A supplied path is opened in append mode so
    /// independent daemon restarts do not destroy earlier diagnostics.
    pub(crate) struct Logger {
        tag: &'static str,
        destination: Arc<Mutex<Destination>>,
    }

    struct Facade;

    static FACADE: Facade = Facade;
    static LOGGER: OnceLock<Logger> = OnceLock::new();

    impl log::Log for Facade {
        fn enabled(&self, metadata: &log::Metadata<'_>) -> bool {
            metadata.level() <= log::Level::Info
        }

        fn log(&self, record: &log::Record<'_>) {
            if self.enabled(record.metadata()) {
                if let Some(logger) = LOGGER.get() {
                    let _ = logger.line(&format!("{}: {}", record.level(), record.args()));
                }
            }
        }

        fn flush(&self) {}
    }

    /// Install the process-global `log` facade backend.  Linux daemons call
    /// this once during bootstrap; all later diagnostics use
    /// `log::{trace,debug,info,warn,error}!`.
    pub fn init(tag: &'static str, path: Option<&Path>) -> io::Result<()> {
        let logger = Logger::new(tag, path)?;
        LOGGER.set(logger).map_err(|_| {
            io::Error::new(io::ErrorKind::AlreadyExists, "tidy_log already initialized")
        })?;
        log::set_logger(&FACADE).map_err(|_| io::Error::other("log facade already initialized"))?;
        log::set_max_level(log::LevelFilter::Info);
        Ok(())
    }

    impl Logger {
        pub(crate) fn new(tag: &'static str, path: Option<&Path>) -> io::Result<Self> {
            let destination = match path {
                Some(path) => {
                    Destination::File(OpenOptions::new().create(true).append(true).open(path)?)
                }
                None => Destination::Stderr,
            };
            Ok(Self {
                tag,
                destination: Arc::new(Mutex::new(destination)),
            })
        }

        pub(crate) fn line(&self, message: &str) -> io::Result<()> {
            let mut destination = self
                .destination
                .lock()
                .map_err(|_| io::Error::other("tidy_log mutex poisoned"))?;
            match &mut *destination {
                Destination::File(file) => {
                    writeln!(file, "{}: {message}", self.tag)?;
                    file.flush()
                }
                Destination::Stderr => {
                    let mut stderr = io::stderr().lock();
                    writeln!(stderr, "{}: {message}", self.tag)
                }
            }
        }
    }
}

impl log::Log for Logger {
    fn enabled(&self, metadata: &log::Metadata<'_>) -> bool {
        metadata.level() <= log::Level::Info
    }

    fn log(&self, record: &log::Record<'_>) {
        if self.enabled(record.metadata()) {
            let stream_line = format!("{}: {}: {}\n", self.tag(), record.level(), record.args());
            // `_s_log` already prepends the current process name. Do not put
            // the service tag into that channel a second time: the serial
            // line should be `exfatd: INFO: ...`, not
            // `exfatd: exfatd: INFO: ...`.
            let native_line = format!("{}: {}\n", record.level(), record.args());
            self.emit_with_kernel(
                &NativeStreamOps,
                stream_line.as_bytes(),
                native_line.as_bytes(),
            );
        }
    }

    fn flush(&self) {}
}
/// Init-once logger over the bootstrap stdout stream.
///
/// Internal backend for the process-global logger.  Daemons do not construct
/// or retain this type; use [`init`] and the standard `log` facade instead.
pub(crate) struct Logger {
    tag: AtomicUsize,
    tag_length: AtomicUsize,
    stdout: AtomicU64,
    kernel_mirror: AtomicBool,
}

static GLOBAL_LOGGER: Logger = Logger::new();

impl Logger {
    /// Create the process-wide logger for `tag` (e.g. `"vfsd"`, `"ramdiskd"`).
    const fn new() -> Self {
        Self {
            tag: AtomicUsize::new(0),
            tag_length: AtomicUsize::new(0),
            stdout: AtomicU64::new(sys::HANDLE_INVALID),
            kernel_mirror: AtomicBool::new(false),
        }
    }

    #[cfg(test)]
    fn with_tag(tag: &'static str) -> Self {
        let logger = Self::new();
        logger.set_tag(tag);
        logger
    }

    fn set_tag(&self, tag: &'static str) {
        self.tag.store(tag.as_ptr() as usize, Ordering::Relaxed);
        self.tag_length.store(tag.len(), Ordering::Relaxed);
    }

    fn tag(&self) -> &'static str {
        let pointer = self.tag.load(Ordering::Relaxed);
        let length = self.tag_length.load(Ordering::Relaxed);
        if pointer == 0 {
            return "service";
        }
        // `set_tag` only accepts a `'static` string, so this pointer remains
        // valid for the lifetime of the process.
        unsafe {
            core::str::from_utf8_unchecked(core::slice::from_raw_parts(
                pointer as *const u8,
                length,
            ))
        }
    }

    fn configure(&self, tag: &'static str, stdout_handle: sys::Handle) {
        self.set_tag(tag);
        self.stdout.store(stdout_handle, Ordering::Relaxed);
        self.kernel_mirror.store(true, Ordering::Relaxed);
    }

    /// Install the bootstrap stdout stream handle for a test logger.
    #[cfg(test)]
    fn init(&self, stdout_handle: sys::Handle) {
        self.stdout.store(stdout_handle, Ordering::Relaxed);
    }

    /// Register this process-wide static logger as the standard Rust `log`
    /// facade backend.  Kept separate from [`Self::init`] so host unit tests
    /// can exercise stream output with stack-local logger fakes.
    fn install(&'static self) {
        let _ = log::set_logger(self);
        log::set_max_level(log::LevelFilter::Info);
    }

    /// Kernel-log mirror hook (dual-channel rule escape hatch): when
    /// enabled, every emitted line is additionally copied into the native
    /// `_s_log` diagnostic channel so boot markers survive in the serial
    /// capture even when stdout is not the serial device. OFF by default.
    #[cfg(test)]
    fn set_kernel_log_mirror(&self, enabled: bool) {
        self.kernel_mirror.store(enabled, Ordering::Relaxed);
    }

    /// Emit raw bytes via the production stream ops.
    fn log_bytes(&self, bytes: &[u8]) {
        let native_bytes = self.strip_tag_prefix(bytes);
        self.emit_with_kernel(&NativeStreamOps, bytes, native_bytes);
    }

    fn strip_tag_prefix<'a>(&self, bytes: &'a [u8]) -> &'a [u8] {
        let tag = self.tag().as_bytes();
        bytes
            .strip_prefix(tag)
            .and_then(|rest| rest.strip_prefix(b": "))
            .unwrap_or(bytes)
    }

    /// Emit `{tag}: {message}\n`.
    /// Emit the standard boot-failure line, then exit nonzero. Never silent:
    /// every early-service failure path funnels here.
    fn fatal(&self, context: &str) -> ! {
        self.log_bytes(format_fatal(self.tag(), context).as_bytes());
        unsafe { sys::_s_exit(1) }
    }

    /// Core emit pipeline: optional kernel mirror, duplicate the cached
    /// stdout handle, write the message in payload-capped chunks, release
    /// the duplicate. Best-effort: individual chunk failures are ignored so
    /// a wedged console cannot wedge boot.
    ///
    /// Generic over [`StreamOps`] so tests run this unmodified against a
    /// fake sink.
    #[cfg(test)]
    fn emit<S: StreamOps>(&self, stream_ops: &S, message: &[u8]) {
        self.emit_with_kernel(stream_ops, message, message);
    }

    /// Emit one representation to the service stream and another to
    /// `_s_log`. The kernel representation intentionally omits the service
    /// tag because the kernel adds the process name itself.
    fn emit_with_kernel<S: StreamOps>(
        &self,
        stream_ops: &S,
        message: &[u8],
        kernel_message: &[u8],
    ) {
        if self.kernel_mirror.load(Ordering::Relaxed) {
            kernel_log_bytes(kernel_message);
        }
        let handle = self.stdout.load(Ordering::Relaxed);
        if handle == sys::HANDLE_INVALID {
            return;
        }
        let Ok(duplicate) = stream_ops.duplicate(handle) else {
            return;
        };
        for chunk in stream_chunks(message) {
            let _ = stream_ops.write(duplicate, chunk);
        }
        stream_ops.close(duplicate);
    }
}

/// Install the process-global logger backend.  Callers use the standard
/// `log` facade after this function returns; the backend object remains
/// private to tidy_log.
pub fn init(tag: &'static str, stdout_handle: sys::Handle) {
    GLOBAL_LOGGER.configure(tag, stdout_handle);
    GLOBAL_LOGGER.install();
}

/// Emit bytes through the process-global logger, primarily for panic/abort
/// paths that cannot construct a formatted `log::Record`.
pub fn log_bytes(bytes: &[u8]) {
    GLOBAL_LOGGER.log_bytes(bytes);
}

/// Emit a fatal message through the installed backend and terminate the
/// current NaOS service.  This keeps the abort path on the same logger while
/// leaving the backend object private.
pub fn fatal(context: &str) -> ! {
    GLOBAL_LOGGER.fatal(context)
}

/// Copy `message` NUL-padded into the fixed native kernel-log buffer
/// (`_s_log` takes a C string).
pub fn kernel_log(message: &str) {
    kernel_log_bytes(message.as_bytes());
}

/// Host test builds replace the native `_s_log` side channel with a no-op:
/// the raw syscall trap must never fire under the std test harness.
#[cfg(not(test))]
fn kernel_log_bytes(bytes: &[u8]) {
    let mut buffer = [0_u8; NATIVE_LOG_BUFFER_BYTES];
    for chunk in native_log_chunks(bytes) {
        native_log_frame(chunk, &mut buffer);
        // SAFETY: `native_log_frame` wrote the terminating NUL.
        unsafe { sys::_s_log(buffer.as_ptr()) };
    }
}

#[cfg(test)]
fn kernel_log_bytes(_bytes: &[u8]) {}

#[cfg(test)]
mod tests {
    use super::*;
    use core::cell::RefCell;
    use std::vec::Vec;

    struct FakeStreamOps {
        duplicates: RefCell<Vec<sys::Handle>>,
        writes: RefCell<Vec<Vec<u8>>>,
        closes: RefCell<Vec<sys::Handle>>,
        next_duplicate: Result<sys::Handle, WriteFailure>,
        fail_writes_from: core::cell::Cell<Option<usize>>,
    }

    impl FakeStreamOps {
        fn new() -> Self {
            Self {
                duplicates: RefCell::new(Vec::new()),
                writes: RefCell::new(Vec::new()),
                closes: RefCell::new(Vec::new()),
                next_duplicate: Ok(0x5150),
                fail_writes_from: core::cell::Cell::new(None),
            }
        }

        fn written(&self) -> Vec<u8> {
            self.writes.borrow().concat()
        }
    }

    impl StreamOps for FakeStreamOps {
        fn duplicate(&self, handle: sys::Handle) -> Result<sys::Handle, WriteFailure> {
            self.duplicates.borrow_mut().push(handle);
            self.next_duplicate
        }

        fn write(&self, _endpoint: sys::Handle, data: &[u8]) -> Result<(), WriteFailure> {
            let mut writes = self.writes.borrow_mut();
            if let Some(from) = self.fail_writes_from.get() {
                if writes.len() >= from {
                    return Err(WriteFailure::WaitFailed);
                }
            }
            writes.push(data.to_vec());
            Ok(())
        }

        fn close(&self, handle: sys::Handle) {
            self.closes.borrow_mut().push(handle);
        }
    }

    #[test]
    fn native_log_framing_reserves_the_terminator() {
        let mut buffer = [0_u8; NATIVE_LOG_BUFFER_BYTES];
        let length = native_log_frame(b"ramdiskd: PASS\n", &mut buffer);
        assert_eq!(&buffer[..length], b"ramdiskd: PASS\n");
        assert_eq!(buffer[length], 0);
    }

    #[test]
    fn native_log_chunks_reassemble_without_truncation() {
        let message = vec![b'x'; NATIVE_LOG_MAX_PAYLOAD * 2 + 1];
        let chunks: Vec<&[u8]> = native_log_chunks(&message).collect();
        assert_eq!(chunks.len(), 3);
        assert_eq!(
            chunks.iter().map(|chunk| chunk.len()).sum::<usize>(),
            message.len()
        );
        assert_eq!(
            chunks.into_iter().flatten().copied().collect::<Vec<u8>>(),
            message
        );
    }

    #[test]
    fn small_messages_stay_one_chunk() {
        let message = b"vfsd: boot archive 4096 bytes\n";
        let chunks: Vec<&[u8]> = stream_chunks(message).collect();
        assert_eq!(chunks, vec![&message[..]]);
    }

    #[test]
    fn empty_message_yields_single_empty_chunk() {
        let chunks: Vec<&[u8]> = stream_chunks(b"").collect();
        assert_eq!(chunks, vec![&b""[..]]);
    }

    #[test]
    fn oversized_message_splits_at_exact_boundary() {
        let message: Vec<u8> = (0..STREAM_WRITE_MAX_PAYLOAD as u64 + 1)
            .map(|index| index as u8)
            .collect();
        let chunks: Vec<&[u8]> = stream_chunks(&message).collect();
        assert_eq!(chunks.len(), 2);
        assert_eq!(chunks[0].len(), STREAM_WRITE_MAX_PAYLOAD);
        assert_eq!(chunks[1].len(), 1);
        assert_eq!(
            chunks[0]
                .iter()
                .chain(chunks[1].iter())
                .copied()
                .collect::<Vec<u8>>(),
            message
        );
    }

    #[test]
    fn multi_chunk_reassembly_is_lossless() {
        let message: Vec<u8> = (0..3 * STREAM_WRITE_MAX_PAYLOAD + 7)
            .map(|index| (index % 251) as u8)
            .collect();
        let chunks: Vec<&[u8]> = stream_chunks(&message).collect();
        assert_eq!(
            chunks.iter().map(|chunk| chunk.len()).sum::<usize>(),
            message.len()
        );
        assert_eq!(
            chunks
                .iter()
                .copied()
                .flatten()
                .copied()
                .collect::<Vec<u8>>(),
            message
        );
    }

    #[test]
    fn emit_duplicates_once_and_reassembles_exactly() {
        let logger = Logger::with_tag("testd");
        logger.init(0xABCD);
        let fake = FakeStreamOps::new();
        let message: Vec<u8> = b"x".repeat(STREAM_WRITE_MAX_PAYLOAD + 3);
        logger.emit(&fake, &message);
        assert_eq!(fake.duplicates.borrow().as_slice(), &[0xABCD]);
        assert_eq!(fake.written(), message);
        assert_eq!(fake.closes.borrow().as_slice(), &[0x5150]);
    }

    #[test]
    fn native_mirror_drops_the_service_tag() {
        let logger = Logger::with_tag("exfatd");
        assert_eq!(
            logger.strip_tag_prefix(b"exfatd: fatal: mount failed\n"),
            b"fatal: mount failed\n"
        );
        assert_eq!(
            logger.strip_tag_prefix(b"other: message\n"),
            b"other: message\n"
        );
    }

    #[test]
    fn emit_without_init_touches_nothing() {
        let logger = Logger::with_tag("testd");
        let fake = FakeStreamOps::new();
        logger.emit(&fake, b"hello\n");
        assert!(fake.duplicates.borrow().is_empty());
        assert!(fake.writes.borrow().is_empty());
        assert!(fake.closes.borrow().is_empty());
    }

    #[test]
    fn emit_with_failing_duplicate_aborts_before_any_write() {
        let logger = Logger::with_tag("testd");
        logger.init(7);
        let mut fake = FakeStreamOps::new();
        fake.next_duplicate = Err(WriteFailure::DuplicateFailed(sys::STATUS_ACCESS_DENIED));
        logger.emit(&fake, b"hello\n");
        assert!(fake.writes.borrow().is_empty());
        assert!(fake.closes.borrow().is_empty());
    }

    #[test]
    fn emit_is_best_effort_across_chunk_failures() {
        let logger = Logger::with_tag("testd");
        logger.init(9);
        let fake = FakeStreamOps::new();
        // First chunk succeeds, every later one fails; the loop keeps going
        // and the duplicate is still released.
        fake.fail_writes_from.set(Some(1));
        let message = [42_u8; STREAM_WRITE_MAX_PAYLOAD + 10];
        logger.emit(&fake, &message);
        assert_eq!(fake.writes.borrow().len(), 1);
        assert_eq!(fake.writes.borrow()[0].len(), STREAM_WRITE_MAX_PAYLOAD);
        assert_eq!(fake.closes.borrow().as_slice(), &[0x5150]);
    }

    #[test]
    fn kernel_mirror_off_by_default_and_toggles() {
        let logger = Logger::with_tag("mirror");
        logger.init(3);
        let fake = FakeStreamOps::new();
        logger.emit(&fake, b"line\n"); // must not panic either way on host
        logger.set_kernel_log_mirror(true);
        logger.emit(&fake, b"line2\n");
        assert_eq!(fake.writes.borrow().len(), 2);
    }

    #[test]
    fn hex_renders_fixed_width_lowercase() {
        let render = |value: u64| format!("{}", Hex(value));
        assert_eq!(render(0), "0000000000000000");
        assert_eq!(render(u64::MAX), "ffffffffffffffff");
        assert_eq!(render(0xdead_beef), "00000000deadbeef");
        assert_eq!(render((-2_i64) as u64), "fffffffffffffffe");
    }

    #[test]
    fn call_error_maps_status_and_swallows_the_rest() {
        assert_eq!(
            call_error_code(CallError::Status(sys::STATUS_PEER_CLOSED)),
            13
        );
        assert_eq!(call_error_code(CallError::InvalidInvocation), -1);
    }
}

#[cfg(all(test, target_os = "linux"))]
mod linux_tests {
    use super::linux;
    use alloc::format;
    use std::fs;
    use std::path::PathBuf;

    #[test]
    fn file_sink_appends_tagged_lines() {
        let path = PathBuf::from(std::env::temp_dir())
            .join(format!("naos-tidy-log-{}.log", std::process::id()));
        let logger = linux::Logger::new("hostd", Some(&path)).unwrap();
        logger.line("ready").unwrap();
        logger.line("stopped").unwrap();
        let contents = fs::read_to_string(&path).unwrap();
        assert_eq!(contents, "hostd: ready\nhostd: stopped\n");
        fs::remove_file(path).unwrap();
    }
}
