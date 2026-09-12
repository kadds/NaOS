//! RAM filesystem backend (USERSPACE_FILESYSTEM_ADR §5.2).
//!
//! Single-user (uid/gid 0) in-memory inode tree backing the File/Directory
//! protocol server. Semantics follow POSIX where the ADR demands them:
//! per-open independent cursors, last-component NOFOLLOW resolution with a
//! symlink budget, POSIX rename error precedence, hard links via shared
//! inodes, resumable directory cursors, and a hard cap on total stored bytes.
//!
//! Node IDs are `(slot | generation << 32)`; the slot generation is bumped
//! every time a slot is recycled so a stale ID can never alias a new node.

use alloc::boxed::Box;
use alloc::collections::{BTreeMap, VecDeque};
use alloc::vec;
use alloc::vec::Vec;
use core::ops::Bound;

use crate::errno::Errno;

pub const MAX_PATH_BYTES: usize = 4095;
pub const MAX_COMPONENT_BYTES: usize = 255;
/// Symlink traversals allowed during one path resolution before `ELOOP`.
pub const SYMLINK_BUDGET: usize = 8;

pub type NodeId = u64;

const GENERATION_SHIFT: u32 = 32;

fn make_node_id(slot: usize, generation: u32) -> NodeId {
    (slot as NodeId) | ((generation as NodeId) << GENERATION_SHIFT)
}

fn slot_of(id: NodeId) -> usize {
    (id & 0xffff_ffff) as usize
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FileKind {
    Regular,
    Directory,
    Symlink,
}

/// Immutable snapshot of node metadata for stat-style responses.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Metadata {
    pub node_id: NodeId,
    pub kind: FileKind,
    pub size: u64,
    pub nlink: u64,
}

enum NodeKind {
    Regular(Vec<u8>),
    Directory(BTreeMap<Vec<u8>, NodeId>),
    Symlink(Vec<u8>),
}

struct NodeBody {
    nlink: u64,
    /// Number of open descriptions retaining this inode after its directory
    /// entries are removed.  An unlinked inode remains usable until this
    /// reaches zero, matching POSIX open-after-unlink semantics.
    open_refs: u64,
    kind: NodeKind,
}

impl NodeKind {
    fn file_kind(&self) -> FileKind {
        match self {
            NodeKind::Regular(_) => FileKind::Regular,
            NodeKind::Directory(_) => FileKind::Directory,
            NodeKind::Symlink(_) => FileKind::Symlink,
        }
    }

    fn byte_len(&self) -> u64 {
        match self {
            NodeKind::Regular(data) | NodeKind::Symlink(data) => data.len() as u64,
            NodeKind::Directory(_) => 0,
        }
    }
}

struct Slot {
    generation: u32,
    node: Option<NodeBody>,
}

/// One open description. Each `open()` yields an independent one; offsets and
/// directory cursors are private to it.
struct OpenDesc {
    node: NodeId,
    offset: u64,
    /// Directory listing cursor: entries strictly after this name come next.
    dir_cursor: Option<Vec<u8>>,
    dir_done: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SeekFrom {
    Set(i64),
    Current(i64),
    End(i64),
}

pub struct RamFs {
    slots: Vec<Slot>,
    free_slots: Vec<usize>,
    open_descriptions: BTreeMap<u64, OpenDesc>,
    next_open_id: u64,
    used_bytes: u64,
    max_bytes: u64,
}

impl RamFs {
    pub fn new(max_bytes: u64) -> Self {
        let mut fs = Self {
            slots: Vec::new(),
            free_slots: Vec::new(),
            open_descriptions: BTreeMap::new(),
            next_open_id: 1,
            used_bytes: 0,
            max_bytes,
        };
        // Slot 0 is the root directory and is never freed.
        fs.slots.push(Slot {
            generation: 0,
            node: Some(NodeBody {
                nlink: 2,
                open_refs: 0,
                kind: NodeKind::Directory(BTreeMap::new()),
            }),
        });
        fs
    }

    pub fn root(&self) -> NodeId {
        make_node_id(0, 0)
    }

    pub fn used_bytes(&self) -> u64 {
        self.used_bytes
    }

    pub fn max_bytes(&self) -> u64 {
        self.max_bytes
    }

    // ---- node table ------------------------------------------------------

    fn node(&self, id: NodeId) -> Result<&NodeBody, Errno> {
        let slot = self.slots.get(slot_of(id)).ok_or(Errno::ENoent)?;
        if slot.generation as u64 != id >> GENERATION_SHIFT {
            return Err(Errno::ENoent);
        }
        slot.node.as_ref().ok_or(Errno::ENoent)
    }

    fn node_kind(&self, id: NodeId) -> Result<&NodeKind, Errno> {
        Ok(&self.node(id)?.kind)
    }

    fn node_mut(&mut self, id: NodeId) -> Result<&mut NodeBody, Errno> {
        let slot = self.slots.get_mut(slot_of(id)).ok_or(Errno::ENoent)?;
        if slot.generation as u64 != id >> GENERATION_SHIFT {
            return Err(Errno::ENoent);
        }
        slot.node.as_mut().ok_or(Errno::ENoent)
    }

    pub fn metadata_of(&self, id: NodeId) -> Result<Metadata, Errno> {
        let body = self.node(id)?;
        Ok(Metadata {
            node_id: id,
            kind: body.kind.file_kind(),
            size: body.kind.byte_len(),
            nlink: body.nlink,
        })
    }

    fn alloc_slot(&mut self, nlink: u64, kind: NodeKind) -> NodeId {
        if let Some(slot_index) = self.free_slots.pop() {
            // The generation bump happens BEFORE the ID is reused, so an ID
            // handed out earlier can never alias the new node.
            let slot = &mut self.slots[slot_index];
            slot.generation = slot.generation.wrapping_add(1);
            slot.node = Some(NodeBody {
                nlink,
                open_refs: 0,
                kind,
            });
            make_node_id(slot_index, slot.generation)
        } else {
            let slot_index = self.slots.len();
            self.slots.push(Slot {
                generation: 0,
                node: Some(NodeBody {
                    nlink,
                    open_refs: 0,
                    kind,
                }),
            });
            make_node_id(slot_index, 0)
        }
    }

    /// Release a node whose last reference went away. Returns freed bytes.
    fn free_node(&mut self, id: NodeId) -> u64 {
        let slot_index = slot_of(id);
        let slot = &mut self.slots[slot_index];
        let bytes = slot
            .node
            .take()
            .map(|body| body.kind.byte_len())
            .unwrap_or(0);
        slot.generation = slot.generation.wrapping_add(1);
        slot.node = None;
        self.free_slots.push(slot_index);
        bytes
    }

    /// Reap an inode whose last directory link and open description are gone.
    /// The caller accounts the returned storage bytes in `used_bytes`.
    fn reap_if_unreferenced(&mut self, id: NodeId) -> u64 {
        let should_reap = self
            .node(id)
            .map(|body| body.nlink == 0 && body.open_refs == 0)
            .unwrap_or(false);
        if should_reap { self.free_node(id) } else { 0 }
    }

    /// Validate a file length before converting it to the length type used by
    /// `Vec`. Rust allocations cannot represent lengths above `isize::MAX`,
    /// even on targets where `usize` is wider than that limit.
    fn storage_len(&self, size: u64) -> Result<usize, Errno> {
        if size > isize::MAX as u64 {
            return Err(Errno::EFbig);
        }
        usize::try_from(size).map_err(|_| Errno::EFbig)
    }

    fn check_charge(&self, old_len: u64, new_len: u64) -> Result<(), Errno> {
        if new_len > old_len {
            let growth = new_len - old_len;
            let total = self
                .used_bytes
                .checked_add(growth)
                .ok_or(Errno::EOverflow)?;
            if total > self.max_bytes {
                return Err(Errno::ENospc);
            }
        } else {
            let freed = old_len - new_len;
            self.used_bytes.checked_sub(freed).ok_or(Errno::EOverflow)?;
        }
        Ok(())
    }

    /// Account for changing one stored object from `old_len` to `new_len`.
    /// Callers must perform any fallible allocation before committing the
    /// resulting length, so a failed operation leaves both state and budget
    /// unchanged.
    fn charge_bytes(&mut self, old_len: u64, new_len: u64) -> Result<(), Errno> {
        self.check_charge(old_len, new_len)?;
        self.used_bytes = if new_len > old_len {
            self.used_bytes + (new_len - old_len)
        } else {
            self.used_bytes - (old_len - new_len)
        };
        Ok(())
    }

    // ---- path resolution -------------------------------------------------

    /// Split into components, enforcing the length limits up front. Absolute
    /// and relative paths are indistinguishable here: callers anchor at their
    /// own visible root (`base`), matching chroot semantics.
    fn split_path(path: &[u8]) -> Result<VecDeque<Vec<u8>>, Errno> {
        if path.is_empty() {
            return Err(Errno::ENoent);
        }
        if path.len() > MAX_PATH_BYTES {
            return Err(Errno::ENameTooLong);
        }
        let mut components = VecDeque::new();
        for component in path.split(|&byte| byte == b'/') {
            if component.is_empty() {
                continue;
            }
            if component.len() > MAX_COMPONENT_BYTES {
                return Err(Errno::ENameTooLong);
            }
            components.push_back(component.to_vec());
        }
        Ok(components)
    }

    /// Resolve components against `base`, following symlinks everywhere
    /// except the final component when `follow_final` is false. Symlink
    /// targets splice into the remaining component list relative to the
    /// directory containing the link (root for absolute targets).
    fn walk(
        &self,
        base: NodeId,
        components: &[Vec<u8>],
        follow_final: bool,
    ) -> Result<NodeId, Errno> {
        let mut budget = SYMLINK_BUDGET;
        let mut pending: VecDeque<Vec<u8>> = components.iter().cloned().collect();
        let mut stack: Vec<NodeId> = vec![base];
        let mut follow_final = follow_final;

        while !pending.is_empty() {
            let component = pending.pop_front().expect("checked non-empty");
            let current = *stack.last().expect("stack starts with base");

            if component == b"." {
                continue;
            }
            if component == b".." {
                if stack.len() > 1 {
                    stack.pop();
                }
                continue;
            }

            let children = match self.node_kind(current)? {
                NodeKind::Directory(children) => children,
                _ => return Err(Errno::ENotDir),
            };
            let child = *children.get(&component).ok_or(Errno::ENoent)?;

            if matches!(self.node_kind(child)?, NodeKind::Symlink(_))
                && (!pending.is_empty() || follow_final)
            {
                if budget == 0 {
                    return Err(Errno::ELoop);
                }
                budget -= 1;
                let target = match self.node_kind(child)? {
                    NodeKind::Symlink(target) => target.clone(),
                    _ => unreachable!("matched above"),
                };
                // Continue inside the resolved target; whatever follows the
                // link is no longer the final component.
                follow_final = true;
                let rest: Vec<Vec<u8>> = pending.drain(..).collect();
                pending = Self::split_path(&target)?;
                pending.extend(rest);
                if target.first() == Some(&b'/') {
                    // Absolute targets restart at the visible root.
                    stack.truncate(1);
                }
                continue;
            }
            stack.push(child);
        }
        stack.last().copied().ok_or(Errno::EIo)
    }

    fn lookup_node(&self, base: NodeId, path: &[u8], follow_final: bool) -> Result<NodeId, Errno> {
        let mut components = Self::split_path(path)?;
        if components.is_empty() {
            return Ok(base);
        }
        self.walk(base, components.make_contiguous(), follow_final)
    }

    /// Resolve to the parent directory plus the raw final name. Rejects
    /// trailing slashes and dot names: they name nothing createable.
    fn resolve_parent(&self, base: NodeId, path: &[u8]) -> Result<(NodeId, Vec<u8>), Errno> {
        if path.is_empty() {
            return Err(Errno::ENoent);
        }
        if path.len() > MAX_PATH_BYTES {
            return Err(Errno::ENameTooLong);
        }
        if path.ends_with(b"/") {
            return Err(Errno::EInval);
        }
        let mut components = Self::split_path(path)?;
        let name = components.back().ok_or(Errno::EInval)?.clone();
        if name == b"." || name == b".." || name.len() > MAX_COMPONENT_BYTES {
            return Err(if name.len() > MAX_COMPONENT_BYTES {
                Errno::ENameTooLong
            } else {
                Errno::EInval
            });
        }
        let parent = match components.len() {
            1 => base,
            _ => {
                let head = components.make_contiguous();
                let head: Vec<Vec<u8>> = head[..head.len() - 1].to_vec();
                self.walk(base, &head, true)?
            }
        };
        if !matches!(self.node_kind(parent)?, NodeKind::Directory(_)) {
            return Err(Errno::ENotDir);
        }
        Ok((parent, name))
    }

    // ---- lookups ---------------------------------------------------------

    pub fn lookup(&self, base: NodeId, path: &[u8], follow_final: bool) -> Result<Metadata, Errno> {
        self.metadata_of(self.lookup_node(base, path, follow_final)?)
    }

    // ---- mutation --------------------------------------------------------

    fn dir_children(&self, dir: NodeId) -> Result<&BTreeMap<Vec<u8>, NodeId>, Errno> {
        match self.node_kind(dir)? {
            NodeKind::Directory(children) => Ok(children),
            _ => Err(Errno::ENotDir),
        }
    }

    pub fn mkdir(&mut self, base: NodeId, path: &[u8]) -> Result<Metadata, Errno> {
        let (parent, name) = self.resolve_parent(base, path)?;
        self.insert_dir(parent, &name)
    }

    fn insert_dir(&mut self, parent: NodeId, name: &[u8]) -> Result<Metadata, Errno> {
        if !matches!(self.node_kind(parent)?, NodeKind::Directory(_)) {
            return Err(Errno::ENotDir);
        }
        if self.dir_children(parent)?.contains_key(name) {
            return Err(Errno::EExist);
        }
        let id = self.alloc_slot(
            2, // the entry in the parent plus the directory's own "."
            NodeKind::Directory(BTreeMap::new()),
        );
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => children.insert(name.to_vec(), id),
            _ => unreachable!("checked above"),
        };
        self.node_mut(parent)?.nlink += 1;
        self.metadata_of(id)
    }

    /// Create missing intermediate directories along `path`, returning the
    /// leaf metadata. Used by root namespace setup and test fixtures; be
    /// robust against paths containing missing intermediate directories.
    pub fn mkdir_p(&mut self, base: NodeId, path: &[u8]) -> Result<Metadata, Errno> {
        let components = Self::split_path(path)?;
        let mut current = base;
        for component in components.iter() {
            current = match self.dir_children(current)?.get(component).copied() {
                Some(child) => child,
                None => self.insert_dir(current, component)?.node_id,
            };
        }
        self.metadata_of(current)
    }

    /// Create (or replace the contents of) a regular file.
    pub fn create_file(
        &mut self,
        base: NodeId,
        path: &[u8],
        exclusive: bool,
        initial_data: &[u8],
    ) -> Result<Metadata, Errno> {
        let (parent, name) = self.resolve_parent(base, path)?;
        let existing = self.dir_children(parent)?.get(&name).copied();
        if let Some(existing) = existing {
            if exclusive || self.node_kind(existing)?.file_kind() != FileKind::Regular {
                return Err(Errno::EExist);
            }
            let current_len = self.node_kind(existing)?.byte_len();
            self.charge_bytes(current_len, initial_data.len() as u64)?;
            match &mut self.node_mut(existing)?.kind {
                NodeKind::Regular(stored) => {
                    stored.clear();
                    stored.extend_from_slice(initial_data);
                }
                _ => unreachable!("checked above"),
            }
            return self.metadata_of(existing);
        }
        self.charge_bytes(0, initial_data.len() as u64)?;
        let id = self.alloc_slot(1, NodeKind::Regular(initial_data.to_vec()));
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => children.insert(name, id),
            _ => unreachable!("checked above"),
        };
        self.metadata_of(id)
    }

    pub fn unlink(&mut self, base: NodeId, path: &[u8]) -> Result<(), Errno> {
        let (parent, name) = self.resolve_parent(base, path)?;
        let target = match self.dir_children(parent)?.get(&name).copied() {
            Some(target) => target,
            None => return Err(Errno::ENoent),
        };
        match self.node_kind(target)?.file_kind() {
            FileKind::Directory => return Err(Errno::EIsDir),
            FileKind::Regular | FileKind::Symlink => {}
        }
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => {
                children.remove(&name);
            }
            _ => unreachable!("checked above"),
        }
        let body = self.node_mut(target)?;
        body.nlink -= 1;
        let freed = self.reap_if_unreferenced(target);
        self.used_bytes -= freed;
        Ok(())
    }

    pub fn rmdir(&mut self, base: NodeId, path: &[u8]) -> Result<(), Errno> {
        let (parent, name) = self.resolve_parent(base, path)?;
        let target = match self.dir_children(parent)?.get(&name).copied() {
            Some(target) => target,
            None => return Err(Errno::ENoent),
        };
        if self.node_kind(target)?.file_kind() != FileKind::Directory {
            return Err(Errno::ENotDir);
        }
        if !self.dir_children(target)?.is_empty() {
            return Err(Errno::ENotEmpty);
        }
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => {
                children.remove(&name);
            }
            _ => unreachable!("checked above"),
        }
        self.node_mut(target)?.nlink = 0;
        let freed = self.reap_if_unreferenced(target);
        self.used_bytes -= freed;
        self.node_mut(parent)?.nlink -= 1;
        Ok(())
    }

    /// True when `candidate` lies inside the subtree rooted at `ancestor`.
    /// Bounded by the total slot count, so it terminates even on cycles.
    fn is_descendant(&self, ancestor: NodeId, candidate: NodeId) -> bool {
        if ancestor == candidate {
            return true;
        }
        let mut work = vec![ancestor];
        while let Some(dir) = work.pop() {
            if let Ok(children) = self.dir_children(dir) {
                for (_, &child) in children.iter() {
                    if child == candidate {
                        return true;
                    }
                    if matches!(self.node_kind(child), Ok(NodeKind::Directory(_))) {
                        work.push(child);
                    }
                }
            }
        }
        false
    }

    /// Rename across two independently resolved scopes: commits the POSIX
    /// error precedence and single-transaction entry move.
    fn rename_nodes(
        &mut self,
        old_parent: NodeId,
        old_name: &[u8],
        new_parent: NodeId,
        new_name: &[u8],
    ) -> Result<(), Errno> {
        let moved = match self.dir_children(old_parent)?.get(old_name).copied() {
            Some(moved) => moved,
            None => return Err(Errno::ENoent),
        };
        let moved_is_dir = self.node_kind(moved)?.file_kind() == FileKind::Directory;
        // POSIX: renaming a directory into its own subtree fails outright.
        if moved_is_dir && self.is_descendant(moved, new_parent) {
            return Err(Errno::EInval);
        }

        let replaced = self.dir_children(new_parent)?.get(new_name).copied();
        if replaced == Some(moved) {
            // Same inode: rename(2) succeeds without doing anything.
            return Ok(());
        }
        if let Some(replaced) = replaced {
            let replaced_is_dir = self.node_kind(replaced)?.file_kind() == FileKind::Directory;
            match (moved_is_dir, replaced_is_dir) {
                (true, true) => {
                    if !self.dir_children(replaced)?.is_empty() {
                        return Err(Errno::ENotEmpty);
                    }
                }
                (true, false) => return Err(Errno::ENotDir),
                (false, true) => return Err(Errno::EIsDir),
                (false, false) => {}
            }
        }

        match &mut self.node_mut(old_parent)?.kind {
            NodeKind::Directory(children) => {
                if children.remove(old_name).is_none() {
                    return Err(Errno::ENoent);
                }
            }
            _ => unreachable!("checked above"),
        }
        match &mut self.node_mut(new_parent)?.kind {
            NodeKind::Directory(children) => {
                children.insert(new_name.to_vec(), moved);
            }
            _ => unreachable!("checked above"),
        }

        if let Some(replaced) = replaced {
            let replaced_is_dir = self.node_kind(replaced)?.file_kind() == FileKind::Directory;
            if replaced_is_dir {
                self.node_mut(replaced)?.nlink = 0;
                let freed = self.reap_if_unreferenced(replaced);
                self.used_bytes -= freed;
                self.node_mut(new_parent)?.nlink -= 1;
            } else {
                let body = self.node_mut(replaced)?;
                body.nlink -= 1;
                let freed = self.reap_if_unreferenced(replaced);
                self.used_bytes -= freed;
            }
        }
        if moved_is_dir && old_parent != new_parent {
            self.node_mut(old_parent)?.nlink -= 1;
            self.node_mut(new_parent)?.nlink += 1;
        }
        Ok(())
    }

    pub fn link(&mut self, base: NodeId, old_path: &[u8], new_path: &[u8]) -> Result<(), Errno> {
        let (old_parent, old_name) = self.resolve_parent(base, old_path)?;
        let source = match self.dir_children(old_parent)?.get(&old_name).copied() {
            Some(source) => source,
            None => return Err(Errno::ENoent),
        };
        match self.node_kind(source)?.file_kind() {
            FileKind::Regular => {}
            FileKind::Directory => return Err(Errno::EIsDir),
            FileKind::Symlink => return Err(Errno::Eperm),
        }
        let (new_parent, new_name) = self.resolve_parent(base, new_path)?;
        if self.dir_children(new_parent)?.contains_key(&new_name) {
            return Err(Errno::EExist);
        }
        match &mut self.node_mut(new_parent)?.kind {
            NodeKind::Directory(children) => children.insert(new_name, source),
            _ => unreachable!("checked above"),
        };
        self.node_mut(source)?.nlink += 1;
        Ok(())
    }

    /// Hard link across two independently resolved scopes (rename_at's
    /// sibling contract): resolution of both sides happens before any
    /// mutation.
    pub fn link_scoped(
        &mut self,
        old_root: NodeId,
        old_current: NodeId,
        old_path: &[u8],
        new_root: NodeId,
        new_current: NodeId,
        new_path: &[u8],
    ) -> Result<(), Errno> {
        let (old_parent, old_name) = self.resolve_parent_scoped(old_root, old_current, old_path)?;
        let source = match self.dir_children(old_parent)?.get(&old_name).copied() {
            Some(source) => source,
            None => return Err(Errno::ENoent),
        };
        match self.node_kind(source)?.file_kind() {
            FileKind::Regular => {}
            FileKind::Directory => return Err(Errno::EIsDir),
            FileKind::Symlink => return Err(Errno::Eperm),
        }
        let (new_parent, new_name) = self.resolve_parent_scoped(new_root, new_current, new_path)?;
        if self.dir_children(new_parent)?.contains_key(&new_name) {
            return Err(Errno::EExist);
        }
        match &mut self.node_mut(new_parent)?.kind {
            NodeKind::Directory(children) => children.insert(new_name, source),
            _ => unreachable!("checked above"),
        };
        self.node_mut(source)?.nlink += 1;
        Ok(())
    }

    /// Create a regular file under a resolved scope.
    pub fn create_scoped(
        &mut self,
        root: NodeId,
        current: NodeId,
        path: &[u8],
        exclusive: bool,
    ) -> Result<Metadata, Errno> {
        let (parent, name) = self.resolve_parent_scoped(root, current, path)?;
        let existing = self.dir_children(parent)?.get(&name).copied();
        if let Some(existing) = existing {
            if exclusive || self.node_kind(existing)?.file_kind() != FileKind::Regular {
                return Err(Errno::EExist);
            }
            return self.metadata_of(existing);
        }
        self.charge_bytes(0, 0)?;
        let id = self.alloc_slot(1, NodeKind::Regular(Vec::new()));
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => children.insert(name, id),
            _ => unreachable!("checked above"),
        };
        self.metadata_of(id)
    }

    pub fn mkdir_scoped(
        &mut self,
        root: NodeId,
        current: NodeId,
        path: &[u8],
    ) -> Result<Metadata, Errno> {
        let (parent, name) = self.resolve_parent_scoped(root, current, path)?;
        self.insert_dir(parent, &name)
    }

    pub fn unlink_scoped(
        &mut self,
        root: NodeId,
        current: NodeId,
        path: &[u8],
    ) -> Result<(), Errno> {
        let (parent, name) = self.resolve_parent_scoped(root, current, path)?;
        let target = match self.dir_children(parent)?.get(&name).copied() {
            Some(target) => target,
            None => return Err(Errno::ENoent),
        };
        match self.node_kind(target)?.file_kind() {
            FileKind::Directory => return Err(Errno::EIsDir),
            FileKind::Regular | FileKind::Symlink => {}
        }
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => {
                children.remove(&name);
            }
            _ => unreachable!("checked above"),
        }
        let body = self.node_mut(target)?;
        body.nlink -= 1;
        let freed = self.reap_if_unreferenced(target);
        self.used_bytes -= freed;
        Ok(())
    }

    pub fn rmdir_scoped(
        &mut self,
        root: NodeId,
        current: NodeId,
        path: &[u8],
    ) -> Result<(), Errno> {
        let (parent, name) = self.resolve_parent_scoped(root, current, path)?;
        let target = match self.dir_children(parent)?.get(&name).copied() {
            Some(target) => target,
            None => return Err(Errno::ENoent),
        };
        if self.node_kind(target)?.file_kind() != FileKind::Directory {
            return Err(Errno::ENotDir);
        }
        if !self.dir_children(target)?.is_empty() {
            return Err(Errno::ENotEmpty);
        }
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => {
                children.remove(&name);
            }
            _ => unreachable!("checked above"),
        }
        self.node_mut(target)?.nlink = 0;
        let freed = self.reap_if_unreferenced(target);
        self.used_bytes -= freed;
        self.node_mut(parent)?.nlink -= 1;
        Ok(())
    }

    pub fn symlink_scoped(
        &mut self,
        root: NodeId,
        current: NodeId,
        target: &[u8],
        link_path: &[u8],
    ) -> Result<Metadata, Errno> {
        if target.is_empty() {
            return Err(Errno::ENoent);
        }
        if target.len() > MAX_PATH_BYTES {
            return Err(Errno::ENameTooLong);
        }
        let (parent, name) = self.resolve_parent_scoped(root, current, link_path)?;
        if self.dir_children(parent)?.contains_key(&name) {
            return Err(Errno::EExist);
        }
        self.charge_bytes(0, target.len() as u64)?;
        let id = self.alloc_slot(1, NodeKind::Symlink(target.to_vec()));
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => children.insert(name, id),
            _ => unreachable!("checked above"),
        };
        self.metadata_of(id)
    }

    pub fn rename_scoped(
        &mut self,
        old_root: NodeId,
        old_current: NodeId,
        old_path: &[u8],
        new_root: NodeId,
        new_current: NodeId,
        new_path: &[u8],
    ) -> Result<(), Errno> {
        let (old_parent, old_name) = self.resolve_parent_scoped(old_root, old_current, old_path)?;
        // Resolve the destination before any mutation so a bad path leaves
        // no side effects.
        let (new_parent, new_name) = self.resolve_parent_scoped(new_root, new_current, new_path)?;
        self.rename_nodes(old_parent, &old_name, new_parent, &new_name)
    }

    pub fn symlink(
        &mut self,
        base: NodeId,
        target: &[u8],
        link_path: &[u8],
    ) -> Result<Metadata, Errno> {
        if target.is_empty() {
            return Err(Errno::ENoent);
        }
        if target.len() > MAX_PATH_BYTES {
            return Err(Errno::ENameTooLong);
        }
        let (parent, name) = self.resolve_parent(base, link_path)?;
        if self.dir_children(parent)?.contains_key(&name) {
            return Err(Errno::EExist);
        }
        self.charge_bytes(0, target.len() as u64)?;
        let id = self.alloc_slot(1, NodeKind::Symlink(target.to_vec()));
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => children.insert(name, id),
            _ => unreachable!("checked above"),
        };
        self.metadata_of(id)
    }

    /// Copy the symlink target into `out`; returns its length. Does not
    /// follow any component of `path`.
    pub fn readlink(&self, base: NodeId, path: &[u8], out: &mut [u8]) -> Result<usize, Errno> {
        let id = self.lookup_node(base, path, false)?;
        match self.node_kind(id)? {
            NodeKind::Symlink(target) => {
                if out.len() < target.len() {
                    return Err(Errno::EInval);
                }
                out[..target.len()].copy_from_slice(target);
                Ok(target.len())
            }
            _ => Err(Errno::EInval),
        }
    }

    pub fn truncate_path(&mut self, base: NodeId, path: &[u8], size: u64) -> Result<(), Errno> {
        let id = self.lookup_node(base, path, true)?;
        self.truncate_node(id, size)
    }

    fn truncate_node(&mut self, id: NodeId, size: u64) -> Result<(), Errno> {
        let kind = self.node_kind(id)?.file_kind();
        if kind != FileKind::Regular {
            return Err(match kind {
                FileKind::Directory => Errno::EIsDir,
                FileKind::Symlink => Errno::EInval,
                FileKind::Regular => unreachable!("matched above"),
            });
        }
        let target_len = self.storage_len(size)?;
        let current_len = self.node_kind(id)?.byte_len();
        self.check_charge(current_len, size)?;
        match &mut self.node_mut(id)?.kind {
            NodeKind::Regular(stored) => {
                if target_len > stored.len() {
                    stored
                        .try_reserve_exact(target_len - stored.len())
                        .map_err(|_| Errno::ENomem)?;
                }
                stored.resize(target_len, 0);
            }
            _ => unreachable!("checked above"),
        }
        self.charge_bytes(current_len, size)?;
        Ok(())
    }

    // ---- open descriptions ----------------------------------------------

    pub fn open(&mut self, base: NodeId, path: &[u8], follow_final: bool) -> Result<u64, Errno> {
        let id = self.lookup_node(base, path, follow_final)?;
        let fd = self.next_open_id;
        self.next_open_id = self
            .next_open_id
            .checked_add(1)
            .filter(|next| *next < u64::from(u32::MAX))
            .ok_or(Errno::EMfile)?;
        self.open_descriptions.insert(
            fd,
            OpenDesc {
                node: id,
                offset: 0,
                dir_cursor: None,
                dir_done: false,
            },
        );
        self.node_mut(id)?.open_refs += 1;
        Ok(fd)
    }

    pub fn close(&mut self, fd: u64) -> Result<(), Errno> {
        let desc = self.open_descriptions.remove(&fd).ok_or(Errno::EBadf)?;
        let body = self.node_mut(desc.node)?;
        body.open_refs -= 1;
        let freed = self.reap_if_unreferenced(desc.node);
        self.used_bytes -= freed;
        Ok(())
    }

    fn desc_node(&self, fd: u64) -> Result<NodeId, Errno> {
        self.open_descriptions
            .get(&fd)
            .map(|d| d.node)
            .ok_or(Errno::EBadf)
    }

    fn desc_offset(&self, fd: u64) -> Result<u64, Errno> {
        self.open_descriptions
            .get(&fd)
            .map(|d| d.offset)
            .ok_or(Errno::EBadf)
    }

    fn desc_set_offset(&mut self, fd: u64, offset: u64) {
        if let Some(desc) = self.open_descriptions.get_mut(&fd) {
            desc.offset = offset;
        }
    }

    /// Read through an open description, advancing only its private cursor.
    pub fn read(&mut self, fd: u64, out: &mut [u8]) -> Result<usize, Errno> {
        let node = self.desc_node(fd)?;
        let offset = self.desc_offset(fd)? as usize;
        let available = match self.node_kind(node)? {
            NodeKind::Regular(data) => data.len(),
            NodeKind::Directory(_) => return Err(Errno::EIsDir),
            NodeKind::Symlink(_) => return Err(Errno::EInval),
        };
        if offset >= available || out.is_empty() {
            self.desc_set_offset(fd, (offset as u64).min(available as u64));
            return Ok(0);
        }
        let count = (available - offset).min(out.len());
        let data = match self.node_kind(node)? {
            NodeKind::Regular(data) => data,
            _ => unreachable!("matched above"),
        };
        out[..count].copy_from_slice(&data[offset..offset + count]);
        self.desc_set_offset(fd, offset as u64 + count as u64);
        Ok(count)
    }

    /// Write at the description's cursor, zero-filling gaps like POSIX write.
    pub fn write(&mut self, fd: u64, data: &[u8]) -> Result<usize, Errno> {
        let node = self.desc_node(fd)?;
        let kind = self.node_kind(node)?.file_kind();
        if kind != FileKind::Regular {
            return Err(match kind {
                FileKind::Directory => Errno::EIsDir,
                _ => Errno::EInval,
            });
        }
        let offset = self.desc_offset(fd)?;
        let end = offset.checked_add(data.len() as u64).ok_or(Errno::EFbig)?;
        let current = self.node_kind(node)?.byte_len();
        let target_len = self.storage_len(end)?;
        let growing = end > current;
        if growing {
            self.check_charge(current, end)?;
        }
        match &mut self.node_mut(node)?.kind {
            NodeKind::Regular(stored) => {
                if growing {
                    stored
                        .try_reserve_exact(target_len - stored.len())
                        .map_err(|_| Errno::ENomem)?;
                }
                let offset = usize::try_from(offset).map_err(|_| Errno::EFbig)?;
                let new_end = offset.checked_add(data.len()).ok_or(Errno::EFbig)?;
                if stored.len() < new_end {
                    stored.resize(new_end, 0);
                }
                stored[offset..new_end].copy_from_slice(data);
            }
            _ => unreachable!("checked above"),
        }
        if growing {
            self.charge_bytes(current, end)?;
        }
        self.desc_set_offset(fd, end);
        Ok(data.len())
    }

    pub fn seek(&mut self, fd: u64, position: SeekFrom) -> Result<u64, Errno> {
        let node = self.desc_node(fd)?;
        let kind = self.node_kind(node)?.file_kind();
        match kind {
            FileKind::Regular => {}
            FileKind::Directory => return Err(Errno::EIsDir),
            FileKind::Symlink => return Err(Errno::EInval),
        }
        let current = self.desc_offset(fd)?;
        let size = self.node_kind(node)?.byte_len();
        let target: i128 = match position {
            SeekFrom::Set(offset) => offset as i128,
            SeekFrom::Current(delta) => current as i128 + delta as i128,
            SeekFrom::End(delta) => size as i128 + delta as i128,
        };
        if target < 0 {
            return Err(Errno::EInval);
        }
        // Seeking past EOF is legal; a later write fills the gap.
        let offset = target as u64;
        self.desc_set_offset(fd, offset);
        Ok(offset)
    }

    pub fn ftruncate(&mut self, fd: u64, size: u64) -> Result<(), Errno> {
        let node = self.desc_node(fd)?;
        self.truncate_node(node, size)
    }

    pub fn fstat(&self, fd: u64) -> Result<Metadata, Errno> {
        self.metadata_of(self.desc_node(fd)?)
    }

    /// Emit up to `max_entries` directory entries strictly after the
    /// description's cursor, advancing that resumable cursor. Returns whether
    /// more entries remain. Order is ascending byte order; deletions of
    /// already-visited names never rewind or repeat the cursor.
    pub fn read_dir_batch(
        &mut self,
        fd: u64,
        max_entries: usize,
        out: &mut Vec<(Vec<u8>, Metadata)>,
    ) -> Result<bool, Errno> {
        out.clear();
        let node = self.desc_node(fd)?;
        let (done_before, cursor) = match self.open_descriptions.get(&fd) {
            Some(desc) => (desc.dir_done, desc.dir_cursor.clone()),
            None => return Err(Errno::EBadf),
        };
        if !matches!(self.node_kind(node)?, NodeKind::Directory(_)) {
            return Err(Errno::ENotDir);
        }
        if done_before {
            return Ok(false);
        }

        let children = match self.node_kind(node)? {
            NodeKind::Directory(children) => children,
            _ => unreachable!("checked above"),
        };
        let has_more = {
            // Confine the children borrow: the boxed iterator carries a
            // destructor, so it must not outlive this block into the
            // cursor update below.
            let mut ordered: Box<dyn Iterator<Item = (&Vec<u8>, &NodeId)>> = match &cursor {
                Some(after) => Box::new(
                    children
                        .range::<Vec<u8>, _>((Bound::Excluded(after.clone()), Bound::Unbounded)),
                ),
                None => Box::new(children.iter()),
            };
            for _ in 0..max_entries {
                let Some((name, &child)) = ordered.next() else {
                    break;
                };
                out.push((name.clone(), self.metadata_of(child)?));
            }
            ordered.next().is_some()
        };
        if let Some(desc) = self.open_descriptions.get_mut(&fd) {
            desc.dir_cursor = out.last().map(|(name, _)| name.clone());
            desc.dir_done = !has_more;
        }
        Ok(has_more)
    }

    // ---- protocol-server surface (USERSPACE_FILESYSTEM_ADR §5.2) ---------

    /// Core walker over an explicit directory stack; `..` pops until only
    /// the visible root remains. `walk` is the single-scope special case.
    fn walk_stack(
        &self,
        mut stack: Vec<NodeId>,
        components: &[Vec<u8>],
        follow_final: bool,
    ) -> Result<NodeId, Errno> {
        let mut budget = SYMLINK_BUDGET;
        let mut pending: VecDeque<Vec<u8>> = components.iter().cloned().collect();
        let mut follow_final = follow_final;

        while !pending.is_empty() {
            let component = pending.pop_front().expect("checked non-empty");
            let current = *stack.last().ok_or(Errno::EIo)?;

            if component == b"." {
                continue;
            }
            if component == b".." {
                if stack.len() > 1 {
                    stack.pop();
                }
                continue;
            }

            let children = match self.node_kind(current)? {
                NodeKind::Directory(children) => children,
                _ => return Err(Errno::ENotDir),
            };
            let child = *children.get(&component).ok_or(Errno::ENoent)?;

            if matches!(self.node_kind(child)?, NodeKind::Symlink(_))
                && (!pending.is_empty() || follow_final)
            {
                if budget == 0 {
                    return Err(Errno::ELoop);
                }
                budget -= 1;
                let target = match self.node_kind(child)? {
                    NodeKind::Symlink(target) => target.clone(),
                    _ => unreachable!("matched above"),
                };
                follow_final = true;
                let rest: Vec<Vec<u8>> = pending.drain(..).collect();
                pending = Self::split_path(&target)?;
                pending.extend(rest);
                if target.first() == Some(&b'/') {
                    // Absolute targets restart at the visible root.
                    stack.truncate(1);
                }
                continue;
            }
            stack.push(child);
        }
        stack.last().copied().ok_or(Errno::EIo)
    }

    /// Starting stack for a scoped lookup: absolute paths anchor at the
    /// visible root, relative paths at the current directory, and `..`
    /// clamps at the root in both cases (chroot semantics).
    fn scoped_stack(&self, root: NodeId, current: NodeId, absolute: bool) -> Vec<NodeId> {
        if absolute || current == root {
            vec![root]
        } else {
            vec![root, current]
        }
    }

    /// Scoped node lookup mirroring the kernel adapter's root/current pair.
    pub fn lookup_scoped(
        &self,
        root: NodeId,
        current: NodeId,
        path: &[u8],
        follow_final: bool,
    ) -> Result<NodeId, Errno> {
        if path.is_empty() {
            return Err(Errno::ENoent);
        }
        if path.len() > MAX_PATH_BYTES {
            return Err(Errno::ENameTooLong);
        }
        let absolute = path.first() == Some(&b'/');
        let mut components = Self::split_path(path)?;
        if components.is_empty() {
            return Ok(if absolute { root } else { current });
        }
        let stack = self.scoped_stack(root, current, absolute);
        self.walk_stack(stack, components.make_contiguous(), follow_final)
    }

    /// Scoped resolve-to-parent: returns `(parent_directory, final_name)`
    /// after walking every intermediate component with symlink following.
    pub fn resolve_parent_scoped(
        &self,
        root: NodeId,
        current: NodeId,
        path: &[u8],
    ) -> Result<(NodeId, Vec<u8>), Errno> {
        if path.is_empty() || path.len() > MAX_PATH_BYTES {
            return Err(if path.len() > MAX_PATH_BYTES {
                Errno::ENameTooLong
            } else {
                Errno::ENoent
            });
        }
        if path.ends_with(b"/") {
            return Err(Errno::EInval);
        }
        let absolute = path.first() == Some(&b'/');
        let mut components = Self::split_path(path)?;
        let name = components.back().ok_or(Errno::EInval)?.clone();
        if name == b"." || name == b".." {
            return Err(Errno::EInval);
        }
        let parent = match components.len() {
            1 => {
                if absolute {
                    root
                } else {
                    current
                }
            }
            _ => {
                let head = components.make_contiguous();
                let head: Vec<Vec<u8>> = head[..head.len() - 1].to_vec();
                let stack = self.scoped_stack(root, current, absolute);
                self.walk_stack(stack, &head, true)?
            }
        };
        if !matches!(self.node_kind(parent)?, NodeKind::Directory(_)) {
            return Err(Errno::ENotDir);
        }
        Ok((parent, name))
    }

    /// Open a description directly on a known node (used when the protocol
    /// server already resolved the final component itself).
    pub fn open_node(&mut self, id: NodeId) -> Result<u64, Errno> {
        self.metadata_of(id)?;
        let fd = self.next_open_id;
        self.next_open_id = self
            .next_open_id
            .checked_add(1)
            .filter(|next| *next < u64::from(u32::MAX))
            .ok_or(Errno::EMfile)?;
        self.open_descriptions.insert(
            fd,
            OpenDesc {
                node: id,
                offset: 0,
                dir_cursor: None,
                dir_done: false,
            },
        );
        self.node_mut(id)?.open_refs += 1;
        Ok(fd)
    }

    /// Positioned read that leaves the description cursor untouched.
    pub fn pread(&self, fd: u64, offset: u64, out: &mut [u8]) -> Result<usize, Errno> {
        let node = self.desc_node(fd)?;
        let data = match self.node_kind(node)? {
            NodeKind::Regular(data) => data,
            NodeKind::Directory(_) => return Err(Errno::EIsDir),
            NodeKind::Symlink(_) => return Err(Errno::EInval),
        };
        if offset >= data.len() as u64 || out.is_empty() {
            return Ok(0);
        }
        let start = offset as usize;
        let count = (data.len() - start).min(out.len());
        out[..count].copy_from_slice(&data[start..start + count]);
        Ok(count)
    }

    /// Positioned write that leaves the description cursor untouched and
    /// zero-fills gaps like `write`.
    pub fn pwrite(&mut self, fd: u64, offset: u64, data: &[u8]) -> Result<usize, Errno> {
        let node = self.desc_node(fd)?;
        if !matches!(self.node_kind(node)?, NodeKind::Regular(_)) {
            return Err(Errno::EIsDir);
        }
        let end = offset.checked_add(data.len() as u64).ok_or(Errno::EFbig)?;
        let current_len = self.node_kind(node)?.byte_len();
        let target_len = self.storage_len(end)?;
        let growing = end > current_len;
        if growing {
            self.check_charge(current_len, end)?;
        }
        match &mut self.node_mut(node)?.kind {
            NodeKind::Regular(stored) => {
                if growing {
                    stored
                        .try_reserve_exact(target_len - stored.len())
                        .map_err(|_| Errno::ENomem)?;
                }
                let start = usize::try_from(offset).map_err(|_| Errno::EFbig)?;
                let new_end = start.checked_add(data.len()).ok_or(Errno::EFbig)?;
                if stored.len() < new_end {
                    stored.resize(new_end, 0);
                }
                stored[start..new_end].copy_from_slice(data);
            }
            _ => unreachable!("checked above"),
        }
        if growing {
            self.charge_bytes(current_len, end)?;
        }
        Ok(data.len())
    }

    /// Immutable byte snapshot of the file behind an open description
    /// (`File.materialize` admission semantics, ADR §5.4).
    pub fn snapshot(&self, fd: u64) -> Result<Vec<u8>, Errno> {
        let node = self.desc_node(fd)?;
        match self.node_kind(node)? {
            NodeKind::Regular(data) => Ok(data.clone()),
            NodeKind::Directory(_) => Err(Errno::EIsDir),
            NodeKind::Symlink(_) => Err(Errno::EInval),
        }
    }

    /// Ordered children of a directory as `(name, metadata)` pairs in
    /// ascending byte order, starting at entry index `offset` and stopping
    /// once `budget` bytes of names have been collected.
    #[allow(clippy::type_complexity)]
    pub fn entries_page(
        &self,
        dir: NodeId,
        offset: u64,
        budget: usize,
    ) -> Result<(Vec<(Vec<u8>, Metadata)>, u64, bool), Errno> {
        let children = match self.node_kind(dir)? {
            NodeKind::Directory(children) => children,
            _ => return Err(Errno::ENotDir),
        };
        if offset > children.len() as u64 {
            return Err(Errno::EInval);
        }
        let mut page = Vec::new();
        let mut consumed = 0usize;
        let mut next = offset;
        let mut truncated = false;
        for (index, (name, &child)) in children.iter().enumerate() {
            if (index as u64) < offset {
                continue;
            }
            let record_bytes = name.len() + 1;
            if consumed + record_bytes > budget && !page.is_empty() {
                truncated = true;
                break;
            }
            page.push((name.clone(), self.metadata_of(child)?));
            consumed += record_bytes;
            next = index as u64 + 1;
        }
        Ok((page, next, truncated))
    }

    /// Path of `node` relative to the visible root; "/" when the node is the
    /// root itself. Bounded by the slot count so cycles terminate.
    pub fn path_of(&self, root: NodeId, node: NodeId) -> Option<Vec<u8>> {
        if root == node {
            return Some(b"/".to_vec());
        }
        let mut names: Vec<Vec<u8>> = Vec::new();
        let mut current = node;
        for _ in 0..self.slots.len() {
            let mut found = None;
            // Find the unique parent by scanning directories.
            'search: for index in 0..self.slots.len() {
                let candidate = make_node_id(index, self.slots[index].generation);
                if let Ok(NodeKind::Directory(children)) = self.node_kind(candidate) {
                    for (name, &child) in children.iter() {
                        if child == current {
                            found = Some((candidate, name.clone()));
                            break 'search;
                        }
                    }
                }
            }
            let (parent, name) = found?;
            names.push(name);
            current = parent;
            if current == root {
                let mut path = Vec::new();
                for name in names.iter().rev() {
                    path.push(b'/');
                    path.extend_from_slice(name);
                }
                return Some(path);
            }
        }
        None
    }

    /// Raw symlink target bytes for a known node (readlink semantics).
    pub fn read_target(&self, node: NodeId, out: &mut [u8]) -> Result<usize, Errno> {
        match self.node_kind(node)? {
            NodeKind::Symlink(target) => {
                if out.len() < target.len() {
                    return Err(Errno::EInval);
                }
                out[..target.len()].copy_from_slice(target);
                Ok(target.len())
            }
            _ => Err(Errno::EInval),
        }
    }

    /// Hard-link an already-resolved source node under `(parent, name)`
    /// (the AT_SYMLINK_FOLLOW branch of link_at).
    pub fn link_node(&mut self, parent: NodeId, source: NodeId, name: &[u8]) -> Result<(), Errno> {
        if self.node_kind(source)?.file_kind() != FileKind::Regular {
            return Err(Errno::Eperm);
        }
        if !matches!(self.node_kind(parent)?, NodeKind::Directory(_)) {
            return Err(Errno::ENotDir);
        }
        if self.dir_children(parent)?.contains_key(name) {
            return Err(Errno::EExist);
        }
        match &mut self.node_mut(parent)?.kind {
            NodeKind::Directory(children) => children.insert(name.to_vec(), source),
            _ => unreachable!("checked above"),
        };
        self.node_mut(source)?.nlink += 1;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    const CAP: u64 = 4096;

    fn fs() -> RamFs {
        RamFs::new(CAP)
    }

    fn create(fs: &mut RamFs, path: &[u8], data: &[u8]) -> Metadata {
        fs.create_file(fs.root(), path, false, data).unwrap()
    }

    /// Same-root rename convenience mirroring the old unscoped signature.
    fn rename(fs: &mut RamFs, base: NodeId, old_path: &[u8], new_path: &[u8]) -> Result<(), Errno> {
        fs.rename_scoped(base, base, old_path, base, base, new_path)
    }

    fn read_all(fs: &mut RamFs, fd: u64) -> Vec<u8> {
        let mut out = vec![0u8; 256];
        let mut collected = Vec::new();
        loop {
            let count = fs.read(fd, &mut out).unwrap();
            if count == 0 {
                break;
            }
            collected.extend_from_slice(&out[..count]);
        }
        collected
    }

    #[test]
    fn independent_open_descriptions_have_private_cursors() {
        let mut f = fs();
        create(&mut f, b"/a", b"hello");
        let first = f.open(f.root(), b"/a", true).unwrap();
        let second = f.open(f.root(), b"/a", true).unwrap();

        let mut buffer = [0u8; 2];
        assert_eq!(f.read(first, &mut buffer).unwrap(), 2);
        assert_eq!(&buffer, b"he");
        // The second description still starts at zero.
        assert_eq!(f.read(second, &mut buffer).unwrap(), 2);
        assert_eq!(&buffer, b"he");
        // Advancing the first does not disturb the second.
        assert_eq!(f.read(first, &mut buffer).unwrap(), 2);
        assert_eq!(&buffer, b"ll");
        assert_eq!(f.read(second, &mut buffer).unwrap(), 2);
        assert_eq!(&buffer, b"ll");
    }

    #[test]
    fn write_read_and_seek_semantics() {
        let mut f = fs();
        create(&mut f, b"/a", b"0123456789");
        let fd = f.open(f.root(), b"/a", true).unwrap();
        assert_eq!(f.seek(fd, SeekFrom::Set(2)).unwrap(), 2);
        assert_eq!(f.write(fd, b"ab").unwrap(), 2);
        assert_eq!(f.seek(fd, SeekFrom::Current(-4)).unwrap(), 0);
        assert_eq!(f.seek(fd, SeekFrom::End(-3)).unwrap(), 7);
        assert_eq!(read_all(&mut f, fd), b"789".to_vec());
        assert_eq!(
            f.seek(fd, SeekFrom::Current(-100)).unwrap_err(),
            Errno::EInval
        );
        assert_eq!(f.close(fd), Ok(()));
        assert_eq!(f.read(fd, &mut [0u8; 1]), Err(Errno::EBadf));
    }

    #[test]
    fn unlink_keeps_open_description_alive_until_close() {
        let mut f = fs();
        let original = create(&mut f, b"/victim", b"data");
        let fd = f.open(f.root(), b"/victim", true).unwrap();

        f.unlink(f.root(), b"/victim").unwrap();
        assert_eq!(f.lookup(f.root(), b"/victim", true), Err(Errno::ENoent));
        assert_eq!(f.open(f.root(), b"/victim", true), Err(Errno::ENoent));

        let stat = f.fstat(fd).unwrap();
        assert_eq!(stat.node_id, original.node_id);
        assert_eq!(stat.nlink, 0);
        assert_eq!(stat.size, 4);
        assert_eq!(f.read(fd, &mut [0u8; 4]).unwrap(), 4);
        f.seek(fd, SeekFrom::Set(0)).unwrap();
        assert_eq!(f.write(fd, b"DATA").unwrap(), 4);
        f.seek(fd, SeekFrom::Set(0)).unwrap();
        assert_eq!(read_all(&mut f, fd), b"DATA".to_vec());

        // The unlinked node cannot be recycled while the description is open.
        let another = create(&mut f, b"/another", b"");
        assert_ne!(another.node_id, original.node_id);

        f.close(fd).unwrap();
        let recycled = create(&mut f, b"/recycled", b"");
        assert_ne!(recycled.node_id, original.node_id);
        assert_eq!(f.metadata_of(original.node_id), Err(Errno::ENoent));
    }

    #[test]
    fn sparse_writes_fill_gaps_and_budget_tracks_bytes() {
        let mut f = fs();
        create(&mut f, b"/a", b"x");
        let fd = f.open(f.root(), b"/a", true).unwrap();
        f.seek(fd, SeekFrom::Set(10)).unwrap();
        f.write(fd, b"end").unwrap();
        f.seek(fd, SeekFrom::Set(0)).unwrap();
        assert_eq!(read_all(&mut f, fd), {
            // Position 0 keeps the original byte; the gap is zero-filled.
            let mut expected = vec![b'x', 0u8, 0u8];
            expected.extend_from_slice(&[0u8; 7]);
            expected.extend_from_slice(b"end");
            expected
        });
        assert_eq!(f.used_bytes(), 13);
        // Shrinking releases budget again.
        f.ftruncate(fd, 0).unwrap();
        assert_eq!(f.used_bytes(), 0);
    }

    #[test]
    fn byte_cap_enforced_with_enospc() {
        let mut f = RamFs::new(16);
        create(&mut f, b"/a", &[7u8; 12]);
        let fd = f.open(f.root(), b"/a", true).unwrap();
        // Overwriting inside the current size never trips the cap.
        assert_eq!(f.write(fd, &[7u8; 5]).unwrap(), 5);
        // Growth past the cap does.
        f.seek(fd, SeekFrom::End(0)).unwrap();
        assert_eq!(f.write(fd, &[7u8; 5]), Err(Errno::ENospc));
        assert_eq!(f.write(fd, &[7u8; 4]).unwrap(), 4);
    }

    #[test]
    fn truncate_rejects_unrepresentable_size_without_mutation() {
        let mut f = RamFs::new(u64::MAX);
        create(&mut f, b"/a", b"data");
        let fd = f.open(f.root(), b"/a", true).unwrap();

        assert_eq!(f.ftruncate(fd, u64::MAX), Err(Errno::EFbig));
        assert_eq!(
            f.truncate_path(f.root(), b"/a", u64::MAX),
            Err(Errno::EFbig)
        );
        assert_eq!(f.used_bytes(), 4);
        assert_eq!(f.metadata_of(f.desc_node(fd).unwrap()).unwrap().size, 4);
        assert_eq!(f.snapshot(fd).unwrap(), b"data");
    }

    #[test]
    fn truncate_rejects_budget_overflow_without_mutation() {
        let mut f = RamFs::new(8);
        create(&mut f, b"/a", b"data");
        let fd = f.open(f.root(), b"/a", true).unwrap();

        assert_eq!(f.ftruncate(fd, 9), Err(Errno::ENospc));
        assert_eq!(f.truncate_path(f.root(), b"/a", 9), Err(Errno::ENospc));
        assert_eq!(f.used_bytes(), 4);
        assert_eq!(f.metadata_of(f.desc_node(fd).unwrap()).unwrap().size, 4);
        assert_eq!(f.snapshot(fd).unwrap(), b"data");
    }

    #[test]
    fn write_rejects_unrepresentable_end_without_mutation() {
        let mut f = RamFs::new(u64::MAX);
        create(&mut f, b"/a", b"data");
        let fd = f.open(f.root(), b"/a", true).unwrap();
        f.seek(fd, SeekFrom::Set(i64::MAX)).unwrap();

        assert_eq!(f.write(fd, &[1]), Err(Errno::EFbig));
        assert_eq!(f.used_bytes(), 4);
        assert_eq!(f.metadata_of(f.desc_node(fd).unwrap()).unwrap().size, 4);
        assert_eq!(f.snapshot(fd).unwrap(), b"data");
        assert_eq!(f.desc_offset(fd).unwrap(), i64::MAX as u64);
    }

    #[test]
    fn write_rejects_budget_overflow_without_mutation() {
        let mut f = RamFs::new(8);
        create(&mut f, b"/a", b"data");
        let fd = f.open(f.root(), b"/a", true).unwrap();
        f.seek(fd, SeekFrom::End(0)).unwrap();

        assert_eq!(f.write(fd, &[1, 2, 3, 4, 5]), Err(Errno::ENospc));
        assert_eq!(f.used_bytes(), 4);
        assert_eq!(f.metadata_of(f.desc_node(fd).unwrap()).unwrap().size, 4);
        assert_eq!(f.snapshot(fd).unwrap(), b"data");
        assert_eq!(f.desc_offset(fd).unwrap(), 4);
    }

    #[test]
    fn mkdir_rmdir_and_nlink_bookkeeping() {
        let mut f = fs();
        let dir = f.mkdir(f.root(), b"/d").unwrap();
        assert_eq!(dir.nlink, 2);
        assert_eq!(f.lookup(f.root(), b"/", true).unwrap().nlink, 3);
        assert_eq!(f.mkdir(f.root(), b"/d"), Err(Errno::EExist));
        create(&mut f, b"/d/child", b"c");
        assert_eq!(f.rmdir(f.root(), b"/d"), Err(Errno::ENotEmpty));
        f.unlink(f.root(), b"/d/child").unwrap();
        f.rmdir(f.root(), b"/d").unwrap();
        assert_eq!(f.lookup(f.root(), b"/d", true), Err(Errno::ENoent));
        assert_eq!(f.lookup(f.root(), b"/", true).unwrap().nlink, 2);
    }

    #[test]
    fn unlink_error_precedence() {
        let mut f = fs();
        create(&mut f, b"/file", b"f");
        f.mkdir(f.root(), b"/dir").unwrap();
        assert_eq!(f.unlink(f.root(), b"/dir"), Err(Errno::EIsDir));
        assert_eq!(f.unlink(f.root(), b"/missing"), Err(Errno::ENoent));
        f.symlink(f.root(), b"file", b"/lnk").unwrap();
        // Unlinking a symlink removes the link, never the target.
        f.unlink(f.root(), b"/lnk").unwrap();
        assert_eq!(f.lookup(f.root(), b"/file", true).unwrap().size, 1);
    }

    #[test]
    fn hard_links_share_data_until_the_last_name_goes() {
        let mut f = fs();
        let original = create(&mut f, b"/a", b"data");
        f.link(f.root(), b"/a", b"/b").unwrap();
        assert_eq!(f.lookup(f.root(), b"/b", true).unwrap().nlink, 2);
        assert_eq!(
            f.lookup(f.root(), b"/b", true).unwrap().node_id,
            original.node_id
        );
        // Writes through one name are visible through the other.
        let fd = f.open(f.root(), b"/b", true).unwrap();
        f.write(fd, b"DATA").unwrap();
        let fd_a = f.open(f.root(), b"/a", true).unwrap();
        assert_eq!(read_all(&mut f, fd_a), b"DATA".to_vec());
        f.unlink(f.root(), b"/a").unwrap();
        assert_eq!(f.lookup(f.root(), b"/b", true).unwrap().nlink, 1);
        f.unlink(f.root(), b"/b").unwrap();
        assert_eq!(f.lookup(f.root(), b"/b", true), Err(Errno::ENoent));
        // Both open descriptions still retain the unlinked inode until they
        // are closed.
        assert_eq!(f.used_bytes(), 4);
        f.close(fd).unwrap();
        assert_eq!(f.used_bytes(), 4);
        f.close(fd_a).unwrap();
        assert_eq!(f.used_bytes(), 0);
        // Directories and symlinks cannot gain extra names.
        f.mkdir(f.root(), b"/dir").unwrap();
        assert_eq!(f.link(f.root(), b"/dir", b"/x"), Err(Errno::EIsDir));
    }

    #[test]
    fn symlinks_resolve_relative_absolute_and_nofollow() {
        let mut f = fs();
        f.mkdir_p(f.root(), b"/real/dir").unwrap();
        create(&mut f, b"/real/dir/file", b"F");
        f.symlink(f.root(), b"real/dir", b"/rel").unwrap();
        f.symlink(f.root(), b"/real/dir/file", b"/abs").unwrap();

        assert_eq!(f.lookup(f.root(), b"/rel/file", true).unwrap().size, 1);
        assert_eq!(f.lookup(f.root(), b"/abs", true).unwrap().size, 1);

        // NOFOLLOW on the final component returns the link itself.
        let meta = f.lookup(f.root(), b"/abs", false).unwrap();
        assert_eq!(meta.kind, FileKind::Symlink);
        let mut target = [0u8; 64];
        let length = f.readlink(f.root(), b"/abs", &mut target).unwrap();
        assert_eq!(&target[..length], b"/real/dir/file");

        // Intermediate components always follow.
        assert_eq!(
            f.lookup(f.root(), b"/rel/../dir/file", true).unwrap().size,
            1
        );
        // Dangling links vanish only when followed.
        f.symlink(f.root(), b"nowhere", b"/dangling").unwrap();
        assert_eq!(
            f.lookup(f.root(), b"/dangling", false).unwrap().kind,
            FileKind::Symlink
        );
        assert_eq!(f.lookup(f.root(), b"/dangling", true), Err(Errno::ENoent));
        // Reading through a dangling link fails at open time.
        assert_eq!(f.open(f.root(), b"/dangling", true), Err(Errno::ENoent));
    }

    #[test]
    fn symlink_loops_hit_the_budget() {
        let mut f = fs();
        f.symlink(f.root(), b"/two", b"/one").unwrap();
        f.symlink(f.root(), b"/one", b"/two").unwrap();
        assert_eq!(f.lookup(f.root(), b"/one", true), Err(Errno::ELoop));
        f.symlink(f.root(), b"self", b"/self").unwrap();
        assert_eq!(f.lookup(f.root(), b"/self", true), Err(Errno::ELoop));
    }

    #[test]
    fn dotdot_after_a_relative_symlink_resolves_in_the_target() {
        let mut f = fs();
        f.mkdir_p(f.root(), b"/a/inner").unwrap();
        f.mkdir_p(f.root(), b"/b").unwrap();
        create(&mut f, b"/b/marker", b"M");
        // /a/jump -> ../b ; then ".." must apply inside /b, not /a.
        f.symlink(f.root(), b"../b", b"/a/jump").unwrap();
        assert_eq!(f.lookup(f.root(), b"/a/jump/marker", true).unwrap().size, 1);

        // ".." after the link applies inside the resolved target (/b):
        // climbing once more reaches the visible root, where no `marker`
        // exists — matching POSIX resolution component-by-component.
        assert_eq!(
            f.lookup(f.root(), b"/a/jump/../marker", true),
            Err(Errno::ENoent)
        );
        // Descending through the target keeps working.
        f.mkdir_p(f.root(), b"/b/sub").unwrap();
        create(&mut f, b"/b/sub/deep", b"D");
        assert_eq!(
            f.lookup(f.root(), b"/a/jump/sub/deep", true).unwrap().size,
            1
        );
    }

    #[test]
    fn rename_edges_follow_posix_precedence() {
        let mut f = fs();
        let root = f.root();
        create(&mut f, b"/old", b"O");
        create(&mut f, b"/victim", b"V");
        f.mkdir_p(root, b"/src/dir").unwrap();
        create(&mut f, b"/src/dir/inner", b"I");

        // Plain file move over an existing file replaces it.
        rename(&mut f, root, b"/old", b"/victim").unwrap();
        assert_eq!(f.lookup(root, b"/old", true), Err(Errno::ENoent));
        assert_eq!(f.lookup(root, b"/victim", true).unwrap().size, 1);

        // File onto a directory fails with EISDIR; directory onto a file ENOTDIR.
        assert_eq!(
            rename(&mut f, root, b"/victim", b"/src"),
            Err(Errno::EIsDir)
        );
        assert_eq!(
            rename(&mut f, root, b"/src", b"/victim"),
            Err(Errno::ENotDir)
        );

        // Directory onto a non-empty directory fails with ENOTEMPTY.
        f.mkdir(root, b"/occupied").unwrap();
        create(&mut f, b"/occupied/keep", b"K");
        assert_eq!(
            rename(&mut f, root, b"/src", b"/occupied"),
            Err(Errno::ENotEmpty)
        );

        // Directory onto an empty directory replaces it, subtree intact.
        f.mkdir(root, b"/empty").unwrap();
        rename(&mut f, root, b"/src", b"/empty").unwrap();
        assert_eq!(f.lookup(root, b"/src", true), Err(Errno::ENoent));
        assert_eq!(f.lookup(root, b"/empty/dir/inner", true).unwrap().size, 1);
        assert_eq!(f.lookup(root, b"/empty", true).unwrap().nlink, 3);

        // Cross-directory moves fix up both parents' link counts.
        let dst = f.mkdir(root, b"/dst").unwrap();
        assert_eq!(dst.nlink, 2);
        rename(&mut f, root, b"/empty/dir", b"/dst/moved").unwrap();
        assert_eq!(f.lookup(root, b"/empty", true).unwrap().nlink, 2);
        assert_eq!(f.lookup(root, b"/dst", true).unwrap().nlink, 3);

        // Moving a directory into its own subtree is rejected outright.
        assert_eq!(
            rename(&mut f, root, b"/dst", b"/dst/moved/deeper"),
            Err(Errno::EInval)
        );
        // Renaming a node onto its own name is a no-op success.
        assert_eq!(rename(&mut f, root, b"/victim", b"victim"), Ok(()));
        // Missing sources fail before any mutation happens.
        assert_eq!(rename(&mut f, root, b"/missing", b"/x"), Err(Errno::ENoent));
    }
}
