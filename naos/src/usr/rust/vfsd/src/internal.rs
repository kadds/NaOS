//! Small VFS-local values used by the private worker protocols.
//!
//! Wire structs, codecs, endpoint descriptors, and dispatchers are generated
//! from `idl/internal` for the `naos-idl/internal` feature. This module only
//! keeps topology's local node key and the mutation operation vocabulary.

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, PartialOrd, Ord)]
pub struct NodeKey {
    pub node_id: u64,
    pub generation: u64,
}

impl NodeKey {
    pub const ZERO: Self = Self {
        node_id: 0,
        generation: 0,
    };

    pub fn is_zero(self) -> bool {
        self == Self::ZERO
    }
}

pub mod namespace_binding {
    pub const OP_RENAME: u32 = 0;
    pub const OP_LINK: u32 = 1;
    pub const OP_SYMLINK: u32 = 2;
    pub const OP_CREATE: u32 = 3;
    pub const OP_MKDIR: u32 = 4;
    pub const OP_UNLINK: u32 = 5;
    pub const OP_RMDIR: u32 = 6;
}
