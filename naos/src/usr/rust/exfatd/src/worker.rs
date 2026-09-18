//! POSIX-matrix wrapper around the FAT library (VFS ADR appendix A).
//!
//! Every method corresponds to a row of the appendix A capability matrix;
//! unsupported rows fail deterministically with `EOPNOTSUPP` (contract
//! tested in `tests/posix_matrix.rs`) rather than degrading silently.
//!
//! Timestamp precision (appendix A row "时间戳精度", FAT column): mtime
//! resolution is 2 s with an optional 10 ms high-resolution byte, creation
//! time carries 10 ms; atime has date-only resolution. Callers must not
//! expect ns timestamps from this backend. The time source used for new
//! entries is [`FixedTimeSource`] until the runtime clock lands.
//!
//! st_dev/st_ino (row "st_dev/st_ino 唯一性"): pseudo device numbers are
//! derived from the lease's `medium_id`; pseudo inode numbers are stable
//! FNV-1a hashes of the absolute path (FAT has no native inode numbers).
//! A rename changes the pseudo inode — acceptable for v1, documented here.

use alloc::format as aformat;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

use fatfs::{DateTime, DirEntry, FatType, FileSystem, FsOptions, Time, TimeProvider};

use crate::block::{BlockClient, BlockError};
use crate::errno::{Errno, FsError};
use crate::volume::VolumeAdapter;

/// Deterministic time source used until the NaOS runtime clock is wired.
///
/// 2026-08-24 00:00:00 UTC. Swapping this for a real clock is a one-line
/// change behind the fatfs `TimeProvider` seam.
#[derive(Clone, Copy, Debug)]
pub struct FixedTimeSource;

static FIXED_TIME_SOURCE: FixedTimeSource = FixedTimeSource;

impl TimeProvider for FixedTimeSource {
    fn get_current_date(&self) -> fatfs::Date {
        fatfs::Date {
            year: 2026,
            month: 8,
            day: 24,
        }
    }

    fn get_current_date_time(&self) -> DateTime {
        DateTime {
            date: fatfs::Date {
                year: 2026,
                month: 8,
                day: 24,
            },
            time: Time {
                hour: 0,
                min: 0,
                sec: 0,
                millis: 0,
            },
        }
    }
}

/// Kind of a looked-up node.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NodeKind {
    File,
    Dir,
}

/// Result of `lookup`: the subset of POSIX stat this backend can honestly
/// report.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NodeStat {
    pub ino: u64,
    pub size: u64,
    pub kind: NodeKind,
}

/// One directory-cursor entry.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DirEntryInfo {
    pub name: String,
    pub ino: u64,
    pub size: u64,
    pub kind: NodeKind,
}

/// Directory cursor: a snapshot of directory contents with a forward-only
/// position, mirroring the VFS Directory revision cursor model.
///
/// FAT cannot provide an offset cookie that stays valid across concurrent
/// mutation, so the cursor pins a consistent snapshot taken at open time;
/// entries created or removed afterwards are not reflected.
pub struct DirCursor {
    entries: Vec<DirEntryInfo>,
    pos: usize,
}

impl DirCursor {
    pub fn next_entry(&mut self) -> Option<DirEntryInfo> {
        let e = self.entries.get(self.pos).cloned();
        if e.is_some() {
            self.pos += 1;
        }
        e
    }

    pub fn remaining(&self) -> usize {
        self.entries.len() - self.pos
    }
}

/// The FAT-family filesystem worker.
pub struct FatWorker<D: BlockClient> {
    fs: FileSystem<VolumeAdapter<D>>,
    /// Cheap handle copy used for flush/FUA barriers (channel clients are
    /// capability handles; the RAM reference device derives Clone).
    client: D,
    medium_id: u64,
    read_only: bool,
}

// `FatWorker::mount` and `FatWorker::format` always install the immutable
// `FixedTimeSource` below and never expose the library's policy
// trait objects.  The worker is transferred once to the dedicated blocking
// executor used by the Linux daemon; it is not shared concurrently.  The
// explicit bound keeps the transfer honest for the block client.
unsafe impl<D: BlockClient + Send> Send for FatWorker<D> {}

fn map_block_error(err: BlockError) -> FsError {
    let errno = match err {
        BlockError::OutOfRange => Errno::EIo,
        BlockError::ReadOnly => Errno::ERofs,
        BlockError::Io => Errno::EIo,
    };
    FsError::new(errno)
}

/// Map the io error surface of the FAT library onto POSIX errno rows.
fn map_io_error(err: &std::io::Error) -> FsError {
    use std::io::ErrorKind as K;
    let msg = aformat!("{}", err);
    let errno = match err.kind() {
        K::NotFound => Errno::ENoent,
        K::AlreadyExists => Errno::EExist,
        K::PermissionDenied => Errno::ERofs,
        K::InvalidInput | K::InvalidData => Errno::EInval,
        // The library encodes several POSIX conditions as ErrorKind::Other
        // with distinguishing messages; match those before defaulting.
        _ => {
            if msg.contains("No space left") {
                Errno::ENospc
            } else if msg.contains("Directory not empty") {
                Errno::ENotEmpty
            } else if msg.contains("too long") || msg.contains("is empty") {
                Errno::ENameTooLong
            } else {
                Errno::EIo
            }
        }
    };
    FsError::new(errno)
}

/// Stable pseudo-inode: FNV-1a over the normalized absolute path.
fn pseudo_ino(path: &str) -> u64 {
    let mut hash: u64 = 0xcbf29ce484222325;
    for b in path.as_bytes() {
        hash ^= u64::from(*b);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}

/// Normalize a POSIX path into a FAT-relative path ("" == root).
/// A single trailing slash is allowed (POSIX names directories so);
/// any `.`, `..` or empty interior component is rejected.
fn normalize_mutation(path: &str) -> Result<String, FsError> {
    let rel = path.trim_start_matches('/');
    let rel = rel.strip_suffix('/').unwrap_or(rel);
    if rel.is_empty() {
        return Ok(String::new());
    }
    if rel
        .split('/')
        .any(|c| c.is_empty() || c == ".." || c == ".")
    {
        return Err(FsError::new(Errno::EInval));
    }
    Ok(rel.to_string())
}

/// Normalize a lookup path into a FAT-relative path ("" == root).
///
/// Lookup operations accept POSIX single-dot components and repeated
/// separators. Parent traversal remains the namespace router's
/// responsibility, so double-dot components are still rejected here.
/// Mutation paths use the stricter normalize_mutation helper.
fn normalize_lookup(path: &str) -> Result<String, FsError> {
    let mut normalized = String::new();
    for component in path.trim_start_matches('/').split('/') {
        if component.is_empty() || component == "." {
            continue;
        }
        if component == ".." {
            return Err(FsError::new(Errno::EInval));
        }
        if !normalized.is_empty() {
            normalized.push('/');
        }
        normalized.push_str(component);
    }
    Ok(normalized)
}

/// Split a normalized relative path into `(parent, final_name)`.
/// The parent is "" when the path is a single component (root child).
fn split_parent(rel: &str) -> (String, String) {
    match rel.rsplit_once('/') {
        Some((parent, name)) => (parent.to_string(), name.to_string()),
        None => (String::new(), rel.to_string()),
    }
}

/// Find a directory entry by name within `dir` (LFN-aware, ASCII
/// case-insensitive on short names, matching FAT semantics).
fn find_entry<'a, 'b, D: BlockClient>(
    dir: &'b fatfs::Dir<'a, VolumeAdapter<D>>,
    name: &str,
) -> Result<Option<DirEntry<'a, VolumeAdapter<D>>>, FsError> {
    for e in dir.iter() {
        let entry = e.map_err(|err| map_io_error(&err))?;
        let fname = entry.file_name();
        if fname == "." || fname == ".." {
            continue;
        }
        if fname == name
            || entry
                .short_file_name_as_bytes()
                .eq_ignore_ascii_case(name.as_bytes())
        {
            return Ok(Some(entry));
        }
    }
    Ok(None)
}

impl<D: BlockClient + Clone> FatWorker<D> {
    /// Format a fresh lease as FAT32 and mount it as a worker.
    ///
    /// FAT32 is the first persistent-worker format.  The boot ramdisk is
    /// sized above fatfs' minimum FAT32 geometry; formatting remains a
    /// worker operation so the filesystem, rather than the kernel, owns
    /// on-disk metadata creation.
    pub fn format(client: D) -> Result<Self, FsError> {
        let info = client.get_info().map_err(map_block_error)?;
        if info.read_only || info.block_count == 0 || info.block_count > u64::from(u32::MAX) {
            return Err(FsError::new(if info.read_only {
                Errno::ERofs
            } else {
                Errno::EInval
            }));
        }
        let mut adapter = VolumeAdapter::new(client.clone())
            .map_err(map_block_error)?
            .with_write_back();
        fatfs::format_volume(
            &mut adapter,
            fatfs::FormatVolumeOptions::new()
                .bytes_per_sector(info.sector_size as u16)
                .total_sectors(info.block_count as u32)
                .fat_type(FatType::Fat32),
        )
        .map_err(|e| map_io_error(&e))?;
        // Formatting writes metadata through the adapter; publish it before
        // opening a FileSystem so a subsequent mount sees a clean volume.
        adapter.flush_plain().map_err(map_block_error)?;
        drop(adapter);
        Self::mount(client)
    }

    /// Mount a formatted volume served by `client`.
    pub fn mount(client: D) -> Result<Self, FsError> {
        let adapter = VolumeAdapter::new(client.clone()).map_err(map_block_error)?;
        let info = *adapter.info();
        let fs = FileSystem::new(adapter, FsOptions::new().time_provider(&FIXED_TIME_SOURCE))
            .map_err(|e| map_io_error(&e))?;
        Ok(Self {
            fs,
            client,
            medium_id: info.medium_id,
            read_only: info.read_only,
        })
    }

    /// Pseudo device number derived from the lease medium id (appendix A).
    pub fn dev(&self) -> u64 {
        self.medium_id ^ 0x4e41_4f53_4641_5433 // "NAOSFAT3" mix
    }

    pub fn is_read_only(&self) -> bool {
        self.read_only
    }

    /// Whether this worker is serving the explicitly supported FAT32 format.
    pub fn is_fat32(&self) -> bool {
        self.fs.fat_type() == FatType::Fat32
    }

    /// Resolve a normalized parent path ("" == root) to its directory
    /// object.
    fn dir_for(&self, parent: &str) -> Result<fatfs::Dir<'_, VolumeAdapter<D>>, FsError> {
        if parent.is_empty() {
            Ok(self.fs.root_dir())
        } else {
            self.fs
                .root_dir()
                .open_dir(parent)
                .map_err(|e| map_io_error(&e))
        }
    }

    /// Appendix A row "lookup/read/write/目录游标": lookup.
    pub fn lookup(&self, path: &str) -> Result<NodeStat, FsError> {
        let rel = normalize_lookup(path)?;
        if rel.is_empty() {
            return Ok(NodeStat {
                ino: pseudo_ino("/"),
                size: 0,
                kind: NodeKind::Dir,
            });
        }
        let (parent, name) = split_parent(&rel);
        let dir = self.dir_for(&parent)?;
        let entry = find_entry(&dir, &name)?.ok_or(FsError::new(Errno::ENoent))?;
        Ok(NodeStat {
            ino: pseudo_ino(&rel),
            size: entry.len(),
            kind: if entry.is_dir() {
                NodeKind::Dir
            } else {
                NodeKind::File
            },
        })
    }

    /// Open a directory cursor (appendix A row "目录游标").
    pub fn open_dir_cursor(&self, path: &str) -> Result<DirCursor, FsError> {
        let rel = normalize_lookup(path)?;
        let mut entries = Vec::new();
        let prefix = if rel.is_empty() {
            String::from("/")
        } else {
            aformat!("/{}/", rel)
        };
        let dir = self.dir_for(&rel)?;
        for e in dir.iter() {
            let entry = e.map_err(|err| map_io_error(&err))?;
            // FAT subdirectories carry "." / ".." pseudo entries; POSIX
            // readdir on this backend hides them.
            let probe = entry.file_name();
            if probe == "." || probe == ".." {
                continue;
            }
            entries.push(dir_entry_info(&entry, &prefix));
        }
        Ok(DirCursor { entries, pos: 0 })
    }

    /// Appendix A row "mkdir/rmdir/unlink/rename（同 FS）".
    pub fn mkdir(&self, path: &str) -> Result<(), FsError> {
        let rel = normalize_mutation(path)?;
        if rel.is_empty() {
            return Err(FsError::new(Errno::EExist));
        }
        self.dir_for("")?
            .create_dir(&rel)
            .map(|_| ())
            .map_err(|e| map_io_error(&e))
    }

    pub fn rmdir(&self, path: &str) -> Result<(), FsError> {
        let rel = normalize_mutation(path)?;
        if rel.is_empty() {
            return Err(FsError::new(Errno::EInval));
        }
        self.dir_for("")?.remove(&rel).map_err(|e| map_io_error(&e))
    }

    pub fn unlink(&self, path: &str) -> Result<(), FsError> {
        let rel = normalize_mutation(path)?;
        if rel.is_empty() {
            return Err(FsError::new(Errno::EInval));
        }
        let (parent, name) = split_parent(&rel);
        let dir = self.dir_for(&parent)?;
        // Refuse directories so unlink never silently deletes a subtree.
        if find_entry(&dir, &name)?.is_some_and(|e| e.is_dir()) {
            return Err(FsError::new(Errno::EIsDir));
        }
        drop(dir);
        self.dir_for("")?.remove(&rel).map_err(|e| map_io_error(&e))
    }

    /// Truncate an existing regular file to zero bytes.  The caller owns the
    /// open-description policy; this worker operation only changes FAT
    /// metadata and persists the directory entry before returning.
    pub fn truncate(&self, path: &str) -> Result<(), FsError> {
        self.truncate_to(path, 0)
    }

    /// Truncate a regular file to an exact length. FAT's primitive truncates
    /// at the current cursor and cannot extend, so extension is materialized
    /// as zero-filled writes before the final metadata truncate.
    pub fn truncate_to(&self, path: &str, length: u64) -> Result<(), FsError> {
        if self.read_only {
            return Err(FsError::new(Errno::ERofs));
        }
        if length > u64::from(u32::MAX) {
            return Err(FsError::new(Errno::EInval));
        }
        let mut file = self.open_file(path)?;
        let current = file.size()?;
        if length > current {
            let zeros = [0_u8; 4096];
            let mut offset = current;
            while offset < length {
                let count = core::cmp::min((length - offset) as usize, zeros.len());
                file.write_at(offset, &zeros[..count])?;
                offset += count as u64;
            }
        }
        file.truncate_at(length)
    }

    /// Same-filesystem rename. Cross-mount rename/link is rejected with
    /// EXDEV by `vfsd` before any backend is consulted (appendix A rule);
    /// this worker therefore never sees it.
    pub fn rename(&self, old_path: &str, new_path: &str) -> Result<(), FsError> {
        let src = normalize_mutation(old_path)?;
        let dst = normalize_mutation(new_path)?;
        if src.is_empty() || dst.is_empty() {
            return Err(FsError::new(Errno::EInval));
        }
        let root = self.fs.root_dir();
        root.rename(&src, &root, &dst).map_err(|e| map_io_error(&e))
    }

    /// Appendix A rows "hard link"/"symlink/readlink": deterministic
    /// `EOPNOTSUPP` — FAT has neither native hard links nor reparse-based
    /// symlinks, and side-car emulation requires its own PRD.
    pub fn hard_link(&self, _old_path: &str, _new_path: &str) -> Result<(), FsError> {
        Err(FsError::new(Errno::EOpNotSupp))
    }

    pub fn symlink(&self, _target: &str, _link_path: &str) -> Result<(), FsError> {
        Err(FsError::new(Errno::EOpNotSupp))
    }

    pub fn readlink(&self, _path: &str) -> Result<Vec<u8>, FsError> {
        Err(FsError::new(Errno::EOpNotSupp))
    }

    /// Appendix A row "chmod/chown/access": chmod/chown are EOPNOTSUPP;
    /// access checks are stubbed allow-all for uid/gid 0.
    pub fn chmod(&self, _path: &str, _mode: u32) -> Result<(), FsError> {
        Err(FsError::new(Errno::EOpNotSupp))
    }

    pub fn chown(&self, _path: &str, _uid: u32, _gid: u32) -> Result<(), FsError> {
        Err(FsError::new(Errno::EOpNotSupp))
    }

    pub fn access(&self, path: &str, _mode: u32) -> Result<(), FsError> {
        // Single-user uid/gid 0 stub: existence check only.
        self.lookup(path).map(|_| ())
    }

    /// Open an existing file (read/write position starts at 0).
    ///
    /// Write authorization is enforced here for read-only leases up front;
    /// the volume adapter additionally rejects every write at the block
    /// boundary.
    pub fn open_file(&self, path: &str) -> Result<FileHandle<'_, D>, FsError> {
        let rel = normalize_lookup(path)?;
        if rel.is_empty() {
            return Err(FsError::new(Errno::EIsDir));
        }
        let file = self
            .fs
            .root_dir()
            .open_file(&rel)
            .map_err(|e| map_io_error(&e))?;
        Ok(FileHandle {
            file,
            client: self.client.clone(),
            read_only: self.read_only,
        })
    }

    /// Create an empty file, failing with EEXIST if it exists
    /// (O_CREAT|O_EXCL semantics; fatfs `create_file` alone would silently
    /// open an existing file).
    pub fn create_file(&self, path: &str) -> Result<FileHandle<'_, D>, FsError> {
        if self.read_only {
            return Err(FsError::new(Errno::ERofs));
        }
        let rel = normalize_mutation(path)?;
        if rel.is_empty() {
            return Err(FsError::new(Errno::EIsDir));
        }
        let (parent, name) = split_parent(&rel);
        let dir = self.dir_for(&parent)?;
        if find_entry(&dir, &name)?.is_some() {
            return Err(FsError::new(Errno::EExist));
        }
        drop(dir);
        let file = self
            .fs
            .root_dir()
            .create_file(&rel)
            .map_err(|e| map_io_error(&e))?;
        Ok(FileHandle {
            file,
            client: self.client.clone(),
            read_only: self.read_only,
        })
    }

    /// Persistence barrier for the whole volume.
    ///
    /// `fua == false` maps to the block protocol plain flush;
    /// `fua == true` maps to FUA and must not return before data survives
    /// restart (ADR appendix A "fsync 持久化" → "flush/FUA 映射").
    pub fn sync(&self, fua: bool) -> Result<(), FsError> {
        // A read-only LBD has no writable cache to drain.  Its flush right
        // is intentionally absent, so the mount-level sync is a successful
        // no-op rather than an attempted block flush.
        if self.read_only {
            return Ok(());
        }
        self.client.flush(fua).map_err(map_block_error)
    }

    /// Finish this worker instance and release its view of the LBD.
    ///
    /// FAT metadata is written through the volume adapter as operations are
    /// committed, but the worker must still issue a durability barrier before
    /// its capability is released.  Consuming `self` makes it impossible for
    /// callers to continue issuing filesystem operations after unmount and
    /// models the worker-side `DETACHED -> SHUTDOWN` handoff in the VFS
    /// lifecycle.  A real channel-backed client will close its moved LBD when
    /// the worker is dropped after this barrier.
    pub fn unmount(self) -> Result<(), FsError> {
        self.sync(true)?;
        drop(self);
        Ok(())
    }
}

fn dir_entry_info<D: BlockClient>(
    entry: &DirEntry<'_, VolumeAdapter<D>>,
    parent_prefix: &str,
) -> DirEntryInfo {
    let name = entry.file_name();
    let full = aformat!("{}{}", parent_prefix, name);
    DirEntryInfo {
        ino: pseudo_ino(&full),
        size: entry.len(),
        kind: if entry.is_dir() {
            NodeKind::Dir
        } else {
            NodeKind::File
        },
        name,
    }
}

/// Handle to an open file: POSIX-ish positional I/O over the FAT library.
pub struct FileHandle<'a, D: BlockClient> {
    file: fatfs::File<'a, VolumeAdapter<D>>,
    client: D,
    read_only: bool,
}

impl<'a, D: BlockClient + Clone> FileHandle<'a, D> {
    /// Sequential read at the current position; fills `buf` unless EOF.
    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize, FsError> {
        let mut total = 0usize;
        while total < buf.len() {
            let n = std::io::Read::read(&mut self.file, &mut buf[total..])
                .map_err(|e| map_io_error(&e))?;
            if n == 0 {
                break;
            }
            total += n;
        }
        Ok(total)
    }

    /// Positional write (`pwrite`). Extends the file as needed.
    pub fn write_at(&mut self, offset: u64, buf: &[u8]) -> Result<usize, FsError> {
        std::io::Seek::seek(&mut self.file, std::io::SeekFrom::Start(offset))
            .map_err(|e| map_io_error(&e))?;
        let mut total = 0usize;
        while total < buf.len() {
            let n = std::io::Write::write(&mut self.file, &buf[total..])
                .map_err(|e| map_io_error(&e))?;
            if n == 0 {
                return Err(FsError::new(Errno::EIo));
            }
            total += n;
        }
        Ok(total)
    }

    /// Positional read (`pread`).
    pub fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> Result<usize, FsError> {
        std::io::Seek::seek(&mut self.file, std::io::SeekFrom::Start(offset))
            .map_err(|e| map_io_error(&e))?;
        self.read(buf)
    }

    /// Truncate this open file at offset zero and flush its directory entry.
    pub fn truncate(&mut self) -> Result<(), FsError> {
        self.truncate_at(0)
    }

    /// Truncate at the current logical file offset and flush the directory
    /// entry. The offset is clamped by fatfs only for seeks beyond EOF; the
    /// worker performs zero filling before calling this for extension.
    fn truncate_at(&mut self, offset: u64) -> Result<(), FsError> {
        if self.read_only {
            return Err(FsError::new(Errno::ERofs));
        }
        std::io::Seek::seek(&mut self.file, std::io::SeekFrom::Start(offset))
            .map_err(|e| map_io_error(&e))?;
        self.file.truncate().map_err(|e| map_io_error(&e))?;
        self.flush()
    }

    /// Flush the FAT directory entry (file length) and metadata so a
    /// concurrent `lookup` observes the written size; maps to the block
    /// protocol plain flush. POSIX `close` implies this.
    pub fn flush(&mut self) -> Result<(), FsError> {
        if self.read_only {
            return Ok(());
        }
        std::io::Write::flush(&mut self.file).map_err(|e| map_io_error(&e))
    }

    /// Current file length (via an end-of-file seek round trip).
    pub fn size(&mut self) -> Result<u64, FsError> {
        let saved = std::io::Seek::seek(&mut self.file, std::io::SeekFrom::Current(0))
            .map_err(|e| map_io_error(&e))?;
        let len = std::io::Seek::seek(&mut self.file, std::io::SeekFrom::End(0))
            .map_err(|e| map_io_error(&e))?;
        std::io::Seek::seek(&mut self.file, std::io::SeekFrom::Start(saved))
            .map_err(|e| map_io_error(&e))?;
        Ok(len)
    }

    /// POSIX fsync mapping: an explicit FUA barrier on the block client;
    /// must not return before written data survives restart.
    pub fn fsync(&mut self) -> Result<(), FsError> {
        if self.read_only {
            return Ok(());
        }
        // Commit the file's directory entry/length through fatfs before
        // issuing the device durability barrier.  The order is essential:
        // FUA cannot make metadata durable if it has not reached the volume
        // adapter yet.
        std::io::Write::flush(&mut self.file).map_err(|e| map_io_error(&e))?;
        self.client.flush(true).map_err(map_block_error)
    }
}
