//! Transport-independent FAT service state and operations.
//!
//! Linux UDS and NaOS channel adapters deliberately use different wire
//! plumbing, but they must not grow different filesystem semantics.  This
//! module owns the worker and the operations both adapters expose.

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::string::String;

use crate::block::BlockClient;
use crate::errno::FsError;
use crate::worker::{DirCursor, FatWorker, NodeKind, NodeStat};
use naos_idl::{directory, file};

pub const OPEN_READ: u64 = 1;
pub const OPEN_WRITE: u64 = 2;
pub const OPEN_APPEND: u64 = 8;
pub const OPEN_DIRECTORY: u64 = 16;
pub const OPEN_CREATE: u64 = 1;
pub const OPEN_EXCL: u64 = 128;
pub const OPEN_TRUNC: u64 = 256;
pub const CREATE_DIRECTORY: u64 = 1;

/// The common filesystem state used by every process transport.
pub struct FatService<D: BlockClient> {
    worker: FatWorker<D>,
    open_descriptions: BTreeMap<u64, OpenDescription>,
    next_description: u64,
}

/// Transport-independent state for one application-facing open description.
/// Endpoint handles are owned by the selected transport; path, offset and
/// open flags are filesystem semantics and therefore live here.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OpenDescription {
    pub path: String,
    pub offset: i64,
    pub flags: u64,
}

impl<D: BlockClient + Clone> FatService<D> {
    pub fn new(worker: FatWorker<D>) -> Self {
        Self {
            worker,
            open_descriptions: BTreeMap::new(),
            next_description: 1,
        }
    }

    pub fn worker(&self) -> &FatWorker<D> {
        &self.worker
    }

    pub fn into_worker(self) -> FatWorker<D> {
        self.worker
    }

    pub fn device_id(&self) -> u64 {
        self.worker.dev()
    }

    pub fn allocate_resource_id(&mut self) -> u64 {
        let id = self.next_description;
        self.next_description = self.next_description.saturating_add(1);
        id
    }

    pub fn open_description(&mut self, path: String, flags: u64) -> u64 {
        let id = self.allocate_resource_id();
        self.open_description_with_id(id, path, flags);
        id
    }

    /// Install an open description under the transport-issued endpoint id.
    /// NaOS uses the kernel object id while Linux uses a servicekit-local id;
    /// the filesystem state must not invent a second identity for the same
    /// endpoint.
    pub fn open_description_with_id(&mut self, id: u64, path: String, flags: u64) {
        self.open_descriptions.insert(
            id,
            OpenDescription {
                path,
                offset: 0,
                flags,
            },
        );
        self.next_description = self.next_description.max(id.saturating_add(1));
    }

    pub fn description(&self, id: u64) -> Option<&OpenDescription> {
        self.open_descriptions.get(&id)
    }

    pub fn remove_description(&mut self, id: u64) -> bool {
        self.open_descriptions.remove(&id).is_some()
    }

    pub fn set_flags(&mut self, id: u64, flags: u64) -> bool {
        let Some(description) = self.open_descriptions.get_mut(&id) else {
            return false;
        };
        description.flags = flags;
        true
    }

    pub fn set_offset(&mut self, id: u64, offset: i64) -> bool {
        let Some(description) = self.open_descriptions.get_mut(&id) else {
            return false;
        };
        description.offset = offset;
        true
    }

    pub fn has_open_path(&self, path: &str) -> bool {
        self.open_descriptions
            .values()
            .any(|description| description.path == path)
    }

    /// Keep all open descriptions valid after a successful rename. FAT
    /// reopens by path, so this table is the shared equivalent of an OS open
    /// description table for both transports.
    pub fn rewrite_open_paths(&mut self, old: &str, new: &str) {
        for description in self.open_descriptions.values_mut() {
            let descendant = description.path.len() > old.len()
                && description.path.starts_with(old)
                && description.path.as_bytes().get(old.len()) == Some(&b'/');
            if description.path == old || descendant {
                let suffix = String::from(&description.path[old.len()..]);
                description.path.clear();
                description.path.push_str(new);
                description.path.push_str(&suffix);
            }
        }
    }

    pub fn lookup(&self, path: &str) -> Result<NodeStat, FsError> {
        self.worker.lookup(path)
    }

    pub fn open_dir_cursor(&self, path: &str) -> Result<DirCursor, FsError> {
        self.worker.open_dir_cursor(path)
    }

    pub fn mkdir(&self, path: &str) -> Result<(), FsError> {
        self.worker.mkdir(path)
    }

    pub fn create_file(&self, path: &str) -> Result<(), FsError> {
        self.worker.create_file(path).map(|_| ())
    }

    pub fn rmdir(&self, path: &str) -> Result<(), FsError> {
        self.worker.rmdir(path)
    }

    pub fn unlink(&self, path: &str) -> Result<(), FsError> {
        self.worker.unlink(path)
    }

    pub fn truncate(&self, path: &str) -> Result<(), FsError> {
        self.worker.truncate(path)
    }

    pub fn truncate_to(&self, path: &str, length: u64) -> Result<(), FsError> {
        self.worker.truncate_to(path, length)
    }

    pub fn rename(&self, old_path: &str, new_path: &str) -> Result<(), FsError> {
        self.worker.rename(old_path, new_path)
    }

    pub fn access(&self, path: &str, mode: u32) -> Result<(), FsError> {
        self.worker.access(path, mode)
    }

    pub fn sync(&self, fua: bool) -> Result<(), FsError> {
        self.worker.sync(fua)
    }

    pub fn open_file(&self, path: &str) -> Result<crate::worker::FileHandle<'_, D>, FsError> {
        self.worker.open_file(path)
    }

    pub fn read_at(&self, path: &str, offset: u64, data: &mut [u8]) -> Result<usize, FsError> {
        let mut file = self.worker.open_file(path)?;
        file.read_at(offset, data)
    }

    pub fn write_at(&self, path: &str, offset: u64, data: &[u8]) -> Result<usize, FsError> {
        let mut file = self.worker.open_file(path)?;
        let count = file.write_at(offset, data)?;
        file.flush()?;
        Ok(count)
    }

    /// Encode one directory page into the caller's region window using the
    /// same bounded record format on both transports.  `out` is the mapped
    /// request region, so the records never pass through an intermediate
    /// buffer.  Returns `(next offset, record count, record bytes)`.
    pub fn list(
        &self,
        path: &str,
        offset: u64,
        requested_bytes: u64,
        out: &mut [u8],
    ) -> Result<(u64, u64, u64), FsError> {
        let mut cursor = self.worker.open_dir_cursor(path)?;
        let budget = if requested_bytes == 0 {
            65_536
        } else {
            (requested_bytes as usize).min(65_536)
        }
        .min(out.len());
        let mut position = 0_u64;
        let mut count = 0_u64;
        let mut written = 0usize;
        while let Some(entry) = cursor.next_entry() {
            if position < offset {
                position += 1;
                continue;
            }
            let name = entry.name.as_bytes();
            let required = 16 + name.len() + 1;
            if written + required > budget {
                break;
            }
            out[written..written + 8].copy_from_slice(&entry.ino.to_le_bytes());
            out[written + 8..written + 12].copy_from_slice(
                &(if entry.kind == NodeKind::Dir {
                    1_u32
                } else {
                    0_u32
                })
                .to_le_bytes(),
            );
            out[written + 12..written + 16]
                .copy_from_slice(&((name.len() + 1) as u32).to_le_bytes());
            out[written + 16..written + 16 + name.len()].copy_from_slice(name);
            out[written + 16 + name.len()] = 0;
            written += required;
            count += 1;
            position += 1;
        }
        Ok((offset + count, count, written as u64))
    }
}

/// Decode a protocol path, accepting the C ABI's optional trailing NUL.
pub fn path(bytes: &[u8]) -> Result<String, i32> {
    let bytes = match bytes.iter().position(|byte| *byte == 0) {
        Some(nul) if bytes[nul + 1..].iter().any(|byte| *byte != 0) => return Err(22),
        Some(nul) => &bytes[..nul],
        None => bytes,
    };
    core::str::from_utf8(bytes)
        .map(str::to_owned)
        .map_err(|_| 22)
}

pub fn stat_value(stat: NodeStat, device: u64) -> directory::Stat {
    directory::Stat {
        device,
        inode: stat.ino,
        links: 1,
        mode: match stat.kind {
            NodeKind::Dir => 0o040000 | 0o755,
            NodeKind::File => 0o100000 | 0o644,
        },
        uid: 0,
        gid: 0,
        padding: 0,
        device_id: 0,
        size: stat.size as i64,
        block_size: 512,
        blocks: stat.size.div_ceil(512) as i64,
        access_seconds: 0,
        access_nanoseconds: 0,
        modify_seconds: 0,
        modify_nanoseconds: 0,
        change_seconds: 0,
        change_nanoseconds: 0,
        unused0: 0,
        unused1: 0,
        unused2: 0,
    }
}

pub fn file_stat(stat: NodeStat, device: u64) -> file::Stat {
    let value = stat_value(stat, device);
    file::Stat {
        device: value.device,
        inode: value.inode,
        links: value.links,
        mode: value.mode,
        uid: value.uid,
        gid: value.gid,
        padding: value.padding,
        device_id: value.device_id,
        size: value.size,
        block_size: value.block_size,
        blocks: value.blocks,
        access_seconds: value.access_seconds,
        access_nanoseconds: value.access_nanoseconds,
        modify_seconds: value.modify_seconds,
        modify_nanoseconds: value.modify_nanoseconds,
        change_seconds: value.change_seconds,
        change_nanoseconds: value.change_nanoseconds,
        unused0: value.unused0,
        unused1: value.unused1,
        unused2: value.unused2,
    }
}
