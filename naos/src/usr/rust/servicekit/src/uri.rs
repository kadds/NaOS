//! Well-known service names shared by providers and clients.
//!
//! A service URI is a protocol namespace identifier, not a transport path.
//! Keeping the names here means changing the namespace layout does not require
//! synchronised string edits in every daemon.

/// Root of the service namespace.
pub const SERVICE_ROOT: &str = "naos://service";
/// Prefix accepted by the service-directory locator.
pub const SERVICE_PREFIX: &str = "naos://service/";
/// Kernel-published one-shot executable image consumed by the boot manager.
pub const BOOT_MODULE_ROOTFSD: &str = "naos://service/boot/module/rootfsd/0";
/// Kernel-published one-shot executable image consumed after root commit.
pub const BOOT_MODULE_INIT: &str = "naos://service/boot/module/init/0";
/// Kernel-published one-shot prepared block image consumed by blockd.
pub const BOOT_ROOT_IMAGE: &str = "naos://service/boot/root-image/0";
/// Kernel-owned terminal-driver factory.
pub const TERMINAL_DRIVER_FACTORY: &str = "naos://service/terminal/driver/0";
/// Kernel-owned console frontend capability.
pub const CONSOLE_FRONTEND: &str = "naos://service/console/0";
/// Kernel-owned input event source capability.
pub const INPUT_EVENT_SOURCE: &str = "naos://service/input/0";
/// Prefix for block-device providers.
pub const BLOCK_PREFIX: &str = "naos://service/block";
/// Prefix for filesystem providers.
pub const FS_PREFIX: &str = "naos://service/fs";
/// The in-memory block-device provider's factory endpoint.
pub const BLOCK_RAMDISK: &str = "naos://service/block/ramdiskd/0";
/// The ordinary VFS namespace endpoint.
pub const FS_VFS: &str = "naos://service/fs/vfs/0";
/// The VFS administrative endpoint used by mount managers.
pub const FS_VFS_ADMIN: &str = "naos://service/fs/vfs/0/admin";
/// The FAT-family filesystem endpoint.
pub const FS_EXFAT: &str = "naos://service/fs/exfat/0";
