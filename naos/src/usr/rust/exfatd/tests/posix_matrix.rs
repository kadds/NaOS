//! Contract tests for the ADR appendix A capability matrix rows that must
//! fail deterministically: hard link / symlink / readlink / chmod / chown
//! return EOPNOTSUPP; cross-mount rename/link never reaches this worker
//! (vfsd rejects with EXDEV above us — documented in `worker.rs`).

use exfatd::block::RamBlockDevice;
use exfatd::errno::Errno;
use exfatd::volume::VolumeAdapter;
use exfatd::worker::FatWorker;

const SECTORS: u64 = 34816;

fn worker() -> FatWorker<RamBlockDevice> {
    let mut dev = RamBlockDevice::new(vec![0u8; (SECTORS * 512) as usize], 512);
    let mut adapter = VolumeAdapter::new(dev.clone()).unwrap();
    fatfs::format_volume(
        &mut adapter,
        fatfs::FormatVolumeOptions::new()
            .bytes_per_sector(512)
            .total_sectors(SECTORS as u32),
    )
    .unwrap();
    drop(adapter);
    FatWorker::mount(dev).unwrap()
}

fn errno_of<T>(r: Result<T, exfatd::errno::FsError>) -> i32 {
    r.err()
        .expect("operation unexpectedly succeeded")
        .errno
        .to_i32()
}

#[test]
fn hard_link_is_eopnotsupp() {
    let w = worker();
    w.create_file("/f1").unwrap();
    assert_eq!(
        errno_of(w.hard_link("/f1", "/f2")),
        Errno::EOpNotSupp.to_i32()
    ); // 95
}

#[test]
fn symlink_and_readlink_are_eopnotsupp() {
    let w = worker();
    assert_eq!(
        errno_of(w.symlink("/target", "/link")),
        Errno::EOpNotSupp.to_i32()
    );
    assert_eq!(
        errno_of(w.readlink("/whatever")),
        Errno::EOpNotSupp.to_i32()
    );
}

#[test]
fn chmod_chown_are_eopnotsupp_access_stubs_allow() {
    let w = worker();
    w.create_file("/f").unwrap();
    assert_eq!(errno_of(w.chmod("/f", 0o644)), Errno::EOpNotSupp.to_i32());
    assert_eq!(errno_of(w.chown("/f", 0, 0)), Errno::EOpNotSupp.to_i32());
    // access: single-user uid/gid 0 stub — allow if the node exists.
    assert!(w.access("/f", 0).is_ok());
    assert_eq!(errno_of(w.access("/missing", 0)), Errno::ENoent.to_i32());
}

#[test]
fn path_normalization_rejects_parent_components() {
    let w = worker();
    // `..` must never escape the mount root (VFS resolves above us).
    assert_eq!(errno_of(w.lookup("/a/../b")), Errno::EInval.to_i32());
    assert_eq!(errno_of(w.lookup("../escape")), Errno::EInval.to_i32());
}

#[test]
fn malformed_boot_sector_is_rejected_without_panicking() {
    let raw = RamBlockDevice::new(vec![0xa5; 34_816 * 512], 512);
    assert!(FatWorker::mount(raw).is_err());
}

#[test]
fn errno_values_match_linux_abi() {
    assert_eq!(Errno::ENoent.to_i32(), 2);
    assert_eq!(Errno::EExist.to_i32(), 17);
    assert_eq!(Errno::EXdev.to_i32(), 18);
    assert_eq!(Errno::EIsDir.to_i32(), 21);
    assert_eq!(Errno::ENotEmpty.to_i32(), 39);
    assert_eq!(Errno::ERofs.to_i32(), 30);
    assert_eq!(Errno::EOpNotSupp.to_i32(), 95);
}
