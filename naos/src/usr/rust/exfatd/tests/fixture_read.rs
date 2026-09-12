//! Fixture-based read tests: mount the build-generated FAT32 image (created by
//! `mkfs.vfat` + mtools, see tests/fixtures/gen_fat32.sh) through the
//! exfatd volume adapter and worker, and verify directory listing and file
//! reads against the known contents.

use exfatd::block::RamBlockDevice;
use exfatd::worker::{FatWorker, NodeKind};

fn fixture_path() -> std::path::PathBuf {
    static FIXTURE: std::sync::OnceLock<std::path::PathBuf> = std::sync::OnceLock::new();
    FIXTURE
        .get_or_init(|| {
            if let Some(path) = std::env::var_os("NAOS_FAT32_FIXTURE") {
                return path.into();
            }

            // Direct `cargo test` has no CMake build directory to receive the
            // generated image. Keep that developer path working without
            // writing generated data back into the source tree.
            let output = std::env::temp_dir().join(format!("naos-exfatd-fat32-{}", std::process::id()));
            let fixture = output.join("fat32-fixture.img.gz");
            if !fixture.is_file() {
                let script = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("tests/fixtures/gen_fat32.sh");
                let status = std::process::Command::new("sh")
                    .arg(script)
                    .arg(&output)
                    .status()
                    .expect("failed to run FAT32 fixture generator");
                assert!(status.success(), "FAT32 fixture generator failed: {status}");
            }
            fixture
        })
        .clone()
}

fn gunzip(data: &[u8]) -> Vec<u8> {
    use std::io::Read;
    let mut out = Vec::new();
    let mut decoder = flate2::read::GzDecoder::new(data);
    decoder.read_to_end(&mut out).unwrap();
    out
}

fn mount_fixture() -> FatWorker<RamBlockDevice> {
    let raw = std::fs::read(fixture_path()).unwrap();
    let img = gunzip(&raw);
    assert_eq!(img.len(), 69_632 * 512); // 69632 sectors of 512B
    FatWorker::mount(RamBlockDevice::new(img, 512)).unwrap()
}

#[test]
fn fixture_mounts_and_reports_fat32_geometry() {
    let worker = mount_fixture();
    assert!(!worker.is_read_only());
    assert_eq!(worker.dev(), 1 ^ 0x4e41_4f53_4641_5433);
}

#[test]
fn fixture_root_dir_listing() {
    let worker = mount_fixture();
    let mut cursor = worker.open_dir_cursor("/").unwrap();

    let mut names = Vec::new();
    while let Some(entry) = cursor.next_entry() {
        names.push((entry.name.clone(), entry.kind));
        if entry.name == "README.TXT" {
            assert_eq!(entry.size, 105);
            assert_eq!(entry.kind, NodeKind::File);
            // Pseudo inode is a stable function of the absolute path.
            assert_ne!(entry.ino, 0);
        }
        if entry.name == "docs" {
            assert_eq!(entry.kind, NodeKind::Dir);
        }
    }
    assert_eq!(names.len(), 3); // README.TXT, BIN.DAT, docs
    assert!(names.iter().any(|(n, _)| n == "README.TXT"));
    assert!(names.iter().any(|(n, _)| n == "BIN.DAT"));
    assert!(
        names
            .iter()
            .any(|(n, k)| n == "docs" && *k == NodeKind::Dir)
    );
}

#[test]
fn fixture_reads_known_file_contents() {
    let worker = mount_fixture();

    let mut readme = worker.open_file("/README.TXT").unwrap();
    let mut buf = [0u8; 256];
    let n = readme.read(&mut buf).unwrap();
    assert_eq!(n, 105);
    assert!(String::from_utf8_lossy(&buf[..n]).starts_with("NaOS exfatd fixture volume"));

    // pread at offset 0 after the sequential read moved the position.
    let mut bin = worker.open_file("BIN.DAT").unwrap();
    let mut b = [0u8; 8];
    assert_eq!(bin.read_at(0, &mut b).unwrap(), 8);
    assert_eq!(&b, &[0x00, 0x01, 0x02, 0x03, 0xde, 0xad, 0xbe, 0xef]);

    // Nested path lookup and read.
    let stat = worker.lookup("/docs/NOTE.TXT").unwrap();
    assert_eq!(stat.size, 25);
    assert_eq!(stat.kind, NodeKind::File);
    let mut note = worker.open_file("/docs/NOTE.TXT").unwrap();
    let mut nb = [0u8; 64];
    assert_eq!(note.read(&mut nb).unwrap(), 25);
    assert_eq!(&nb[..12], b"fixture note");
    assert_eq!(&nb[13..25], b"second line\n");
}

#[test]
fn prepared_image_accepts_mutation_when_requested() {
    let Some(path) = std::env::var_os("NAOS_PREPARED_ROOT_IMAGE") else {
        return;
    };
    let image = std::fs::read(path).expect("read prepared root image");
    let worker = FatWorker::mount(RamBlockDevice::new(image, 512)).expect("mount root image");
    let mut file = worker
        .create_file("/vfs-data-host-smoke.txt")
        .expect("create file in prepared root image");
    let payload = b"host mutation check\n";
    assert_eq!(file.write_at(0, payload).expect("write"), payload.len());
    file.fsync().expect("fsync");
}

#[test]
fn fixture_lookup_stats() {
    let worker = mount_fixture();
    let root = worker.lookup("/").unwrap();
    assert_eq!(root.kind, NodeKind::Dir);

    let docs = worker.lookup("docs/").unwrap();
    assert_eq!(docs.kind, NodeKind::Dir);

    assert_eq!(
        worker.lookup("/missing.file").unwrap_err().errno.to_i32(),
        2 // ENOENT
    );
}
