#[cfg(target_os = "naos")]
#[tokio::main(flavor = "current_thread")]
async fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(
        servicekit::run("virtioblkd", pcied::run_virtio_blk_driver).await as u8,
    )
}

#[cfg(not(target_os = "naos"))]
fn main() {
    println!("virtioblkd: Virtio PCI block access requires NaOS");
}
