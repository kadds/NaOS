//! Mutation cycles on an in-memory volume: format a RAM device through the
//! upstream formatter (fatfs `format_volume` driven by our own volume
//! adapter), then run create/write/rename/unlink/mkdir/rmdir cycles, plus
//! read-only lease and fsync→FUA behavior.

use exfatd::block::RamBlockDevice;
use exfatd::volume::VolumeAdapter;
use exfatd::worker::FatWorker;

const SECTORS: u64 = 34816; // same geometry as the committed fixture

/// Extract the POSIX errno from an operation result without requiring
/// `Debug` on the payload.
fn fs_errno<T>(r: Result<T, exfatd::errno::FsError>) -> i32 {
    r.err()
        .expect("operation unexpectedly succeeded")
        .errno
        .to_i32()
}

fn formatted_device() -> RamBlockDevice {
    let mut dev = RamBlockDevice::new(vec![0u8; (SECTORS * 512) as usize], 512);
    // Format through OUR adapter: exercises the byte↔sector mapping in both
    // directions before any filesystem code runs.
    let mut adapter = VolumeAdapter::new(dev.clone()).unwrap();
    fatfs::format_volume(
        &mut adapter,
        fatfs::FormatVolumeOptions::new()
            .bytes_per_sector(512)
            .total_sectors(SECTORS as u32),
    )
    .unwrap();
    drop(adapter);
    dev
}

#[test]
fn format_worker_creates_a_fat32_volume() {
    // 34 MiB is enough for the FAT32 minimum cluster count with 512-byte
    // clusters, while keeping the host test reasonably small.
    let dev = RamBlockDevice::new(vec![0u8; 69_632 * 512], 512);
    let worker = FatWorker::format(dev.clone()).unwrap();
    assert!(worker.is_fat32());
    worker.sync(true).unwrap();

    // A fresh format must be mountable again from the same block lease.
    let remounted = FatWorker::mount(dev).unwrap();
    assert!(remounted.is_fat32());
    assert_eq!(
        remounted.lookup("/").unwrap().kind,
        exfatd::worker::NodeKind::Dir
    );
}

#[test]
fn formats_in_memory_volume_and_mounts() {
    let dev = formatted_device();
    let worker = FatWorker::mount(dev).unwrap();
    // Empty FAT32 root contains only the (optional) volume label entry.
    let mut cursor = worker.open_dir_cursor("/").unwrap();
    while cursor.next_entry().is_some() {}
}

#[test]
fn create_write_rename_unlink_cycle() {
    let worker = FatWorker::mount(formatted_device()).unwrap();

    // create + pwrite across several clusters
    let mut file = worker.create_file("/a.txt").unwrap();
    let payload: Vec<u8> = (0..10_000u32).map(|i| (i % 251) as u8).collect();
    assert_eq!(file.write_at(0, &payload).unwrap(), payload.len());
    file.flush().unwrap(); // persist FAT directory entry (length)
    worker.sync(true).unwrap();

    // re-open, pread back through the whole write path
    drop(file);
    let stat = worker.lookup("/a.txt").unwrap();
    assert_eq!(stat.size, 10_000);
    let mut reopened = worker.open_file("/a.txt").unwrap();
    assert_eq!(reopened.size().unwrap(), 10_000);
    let mut out = vec![0u8; 5_000];
    assert_eq!(reopened.read_at(500, &mut out).unwrap(), 5_000);
    assert_eq!(&out[..100], &payload[500..600]);

    // O_CREAT|O_EXCL semantics on an existing file
    assert_eq!(fs_errno(worker.create_file("/a.txt")), 17); // EEXIST

    // rename, then verify old name is gone
    worker.rename("/a.txt", "/b-renamed.txt").unwrap();
    assert!(worker.lookup("/b-renamed.txt").is_ok());
    assert_eq!(
        worker.lookup("/a.txt").unwrap_err().errno.to_i32(),
        2 // ENOENT
    );
    worker.rename("/b-renamed.txt", "/b-renamed.txt").unwrap(); // self rename
    worker.truncate("/b-renamed.txt").unwrap();
    assert_eq!(worker.lookup("/b-renamed.txt").unwrap().size, 0);

    // unlink files but never directories
    worker.unlink("/b-renamed.txt").unwrap();
    assert_eq!(
        worker.lookup("/b-renamed.txt").unwrap_err().errno.to_i32(),
        2
    );
    worker.mkdir("/dir1").unwrap();
    assert_eq!(
        worker.unlink("/dir1").unwrap_err().errno.to_i32(),
        21 // EISDIR
    );
}

#[test]
fn mkdir_rmdir_cycle_with_errors() {
    let worker = FatWorker::mount(formatted_device()).unwrap();

    worker.mkdir("/etc").unwrap();
    worker.mkdir("/etc/nested").unwrap();
    let mut f = worker.create_file("/etc/nested/x.cfg").unwrap();
    f.write_at(0, b"k=v\n").unwrap();
    drop(f);

    // rmdir on non-empty directory fails ENOTEMPTY
    assert_eq!(worker.rmdir("/etc").unwrap_err().errno.to_i32(), 39);

    // deep listing through nested paths
    let mut cursor = worker.open_dir_cursor("/etc/nested").unwrap();
    let entry = cursor.next_entry().unwrap();
    assert_eq!(entry.name, "x.cfg"); // LFN entry preserves lowercase

    worker.unlink("/etc/nested/x.cfg").unwrap();
    worker.rmdir("/etc/nested").unwrap();
    worker.rmdir("/etc").unwrap();
    assert_eq!(
        worker.lookup("/etc").unwrap_err().errno.to_i32(),
        2 // ENOENT
    );

    // rmdir of the root is rejected EINVAL
    assert_eq!(worker.rmdir("/").unwrap_err().errno.to_i32(), 22);
    assert_eq!(worker.rmdir("").unwrap_err().errno.to_i32(), 22);
}

#[test]
fn fsync_maps_to_fua_flushes() {
    let dev = formatted_device();
    let plain_before = dev.plain_flushes();
    let fua_before = dev.fua_flushes();

    let worker = FatWorker::mount(dev.clone()).unwrap();
    let mut f = worker.create_file("/synced.bin").unwrap();
    f.write_at(0, b"persist me").unwrap();
    f.fsync().unwrap(); // per-file fsync → FUA barrier

    assert!(dev.fua_flushes() > fua_before);

    worker.sync(false).unwrap(); // plain barrier
    assert!(dev.plain_flushes() > plain_before);
}

#[test]
fn read_only_lease_rejects_mutations() {
    // Seed from an already-formatted image, then flip the lease read-only.
    let formatted = formatted_device();
    let ro_dev = RamBlockDevice::with_medium(formatted.snapshot(), 512, true, 7);
    assert!(
        exfatd::block::BlockClient::get_info(&ro_dev)
            .unwrap()
            .read_only
    );

    let worker = FatWorker::mount(ro_dev).unwrap();
    assert!(worker.is_read_only());
    assert_eq!(worker.dev(), 7 ^ 0x4e41_4f53_4641_5433);

    // Reads still work.
    assert!(worker.open_dir_cursor("/").is_ok());

    // Mutations fail deterministically with EROFS.
    assert_eq!(fs_errno(worker.create_file("/nope")), 30); // EROFS
    assert_eq!(worker.mkdir("/nope").unwrap_err().errno.to_i32(), 30);
    assert!(worker.unlink("/README.TXT").is_err());
}

#[test]
fn read_only_sync_is_a_noop_without_flush_right() {
    let formatted = formatted_device();
    let ro_dev = RamBlockDevice::with_medium(formatted.snapshot(), 512, true, 9);
    let worker = FatWorker::mount(ro_dev.clone()).unwrap();
    let plain_before = ro_dev.plain_flushes();
    let fua_before = ro_dev.fua_flushes();

    worker.sync(false).unwrap();
    worker.sync(true).unwrap();

    assert_eq!(ro_dev.plain_flushes(), plain_before);
    assert_eq!(ro_dev.fua_flushes(), fua_before);
}

#[test]
fn read_only_file_fsync_is_a_noop_without_flush_right() {
    let formatted = formatted_device();
    let seed = FatWorker::mount(formatted.clone()).unwrap();
    seed.create_file("/fsync.txt").unwrap();
    let ro_dev = RamBlockDevice::with_medium(formatted.snapshot(), 512, true, 10);
    let worker = FatWorker::mount(ro_dev.clone()).unwrap();
    let mut file = worker.open_file("/fsync.txt").unwrap();
    let fua_before = ro_dev.fua_flushes();
    file.fsync().unwrap();
    assert_eq!(ro_dev.fua_flushes(), fua_before);
}

#[test]
fn allocation_exhaustion_is_enospc_not_generic_io() {
    // Keep this volume intentionally tiny.  The formatter still creates a
    // valid FAT volume, but the first multi-cluster write exhausts it.
    let dev = RamBlockDevice::new(vec![0u8; 4_096 * 512], 512);
    let mut adapter = VolumeAdapter::new(dev.clone()).unwrap();
    fatfs::format_volume(
        &mut adapter,
        fatfs::FormatVolumeOptions::new()
            .bytes_per_sector(512)
            .total_sectors(4_096),
    )
    .unwrap();
    drop(adapter);

    let worker = FatWorker::mount(dev).unwrap();
    let mut file = worker.create_file("/full.bin").unwrap();
    let payload = vec![0x5a; 4 * 1024 * 1024];
    assert_eq!(
        file.write_at(0, &payload).unwrap_err().errno,
        exfatd::errno::Errno::ENospc
    );
}
