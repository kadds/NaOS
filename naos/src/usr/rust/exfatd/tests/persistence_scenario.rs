//! Phase 3 persistence acceptance scenario.
//!
//! This is the executable host-side equivalent of the guest sequence.  The
//! file-backed case below crosses a close/reopen boundary and uses `sync_all`
//! for FUA, while deliberately making no claim about a QEMU disk controller.

use exfatd::block::{BlockClient, FileBlockDevice, RamBlockDevice};
use exfatd::volume::VolumeAdapter;
use exfatd::worker::FatWorker;

const SECTORS: u64 = 34_816;
const PATH: &str = "/phase3-persistence.txt";
const PAYLOAD: &[u8] = b"NaOS Phase 3 durable worker payload\n";

fn formatted_device() -> RamBlockDevice {
    let dev = RamBlockDevice::new(vec![0; (SECTORS * 512) as usize], 512);
    let mut adapter = VolumeAdapter::new(dev.clone()).expect("adapter");
    fatfs::format_volume(
        &mut adapter,
        fatfs::FormatVolumeOptions::new()
            .bytes_per_sector(512)
            .total_sectors(SECTORS as u32),
    )
    .expect("format");
    dev
}

#[test]
fn mount_write_fsync_unmount_remount_reads_back() {
    let dev = formatted_device();
    let fua_before = dev.fua_flushes();

    // The scope is the worker lifetime.  `unmount` is explicit: it performs
    // the worker's final FUA barrier before releasing the worker state.
    {
        let worker = FatWorker::mount(dev.clone()).expect("initial mount");
        let mut file = worker.create_file(PATH).expect("create");
        assert_eq!(file.write_at(0, PAYLOAD).expect("write"), PAYLOAD.len());
        file.fsync().expect("file fsync");
        drop(file);
        worker.sync(true).expect("volume sync");
        worker.unmount().expect("unmount");
    }
    assert!(dev.fua_flushes() > fua_before);

    // A fresh worker and a fresh block-client instance are the
    // remount/recovery boundary.  Reconstructing the client from the
    // persisted image makes this test exercise on-disk bytes rather than
    // merely sharing the original in-process `Rc` handle.
    let persisted_image = dev.snapshot();
    let remount_dev = RamBlockDevice::new(persisted_image, 512);
    let worker = FatWorker::mount(remount_dev).expect("remount");
    let mut file = worker.open_file(PATH).expect("reopen");
    let mut actual = vec![0; PAYLOAD.len()];
    assert_eq!(
        file.read_at(0, &mut actual).expect("readback"),
        PAYLOAD.len()
    );
    assert_eq!(actual, PAYLOAD);
}

#[test]
fn phase3_worker_reports_lbd_geometry_and_flush_domain() {
    let dev = formatted_device();
    let worker = FatWorker::mount(dev.clone()).expect("mount");
    let info = dev.get_info().expect("LBD info");
    assert_eq!(worker.dev(), info.medium_id ^ 0x4e41_4f53_4641_5433);
    worker.sync(false).expect("plain flush");
    assert_eq!(dev.plain_flushes(), 1);
    worker.unmount().expect("unmount");
}

#[test]
fn fresh_file_block_clients_recover_after_worker_restart() {
    use std::time::{SystemTime, UNIX_EPOCH};

    let path = std::env::temp_dir().join(format!(
        "exfatd-phase3-{}-{}.img",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock")
            .as_nanos()
    ));
    let result = (|| {
        let dev = FileBlockDevice::create(&path, SECTORS * 512, 512, 0x5048_335f_4649_4c45)
            .expect("create file-backed block device");
        let mut adapter = VolumeAdapter::new(dev.clone()).expect("adapter");
        fatfs::format_volume(
            &mut adapter,
            fatfs::FormatVolumeOptions::new()
                .bytes_per_sector(512)
                .total_sectors(SECTORS as u32),
        )
        .expect("format");
        adapter.sync_fua().expect("persist format");
        drop(adapter);

        {
            let worker = FatWorker::mount(dev).expect("initial mount");
            let mut file = worker.create_file(PATH).expect("create");
            file.write_at(0, PAYLOAD).expect("write");
            file.fsync().expect("fsync");
            drop(file);
            worker.unmount().expect("unmount");
        }

        let reopened = FileBlockDevice::open(&path, 512, true, 0x5048_335f_4649_4c45)
            .expect("reopen file-backed block device");
        assert_eq!(
            reopened.write(0, &[0; 512]),
            Err(exfatd::block::BlockError::ReadOnly)
        );
        let worker = FatWorker::mount(reopened).expect("remount");
        let mut file = worker.open_file(PATH).expect("reopen file");
        let mut actual = vec![0; PAYLOAD.len()];
        file.read_at(0, &mut actual).expect("readback");
        assert_eq!(actual, PAYLOAD);
        Ok::<(), Box<dyn std::error::Error>>(())
    })();
    let _ = std::fs::remove_file(&path);
    result.expect("file-backed persistence scenario");
}
