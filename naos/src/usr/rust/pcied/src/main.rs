#[cfg(target_os = "naos")]
#[tokio::main(flavor = "current_thread")]
async fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(servicekit::run("pcied", pcied::run_manager).await as u8)
}

#[cfg(not(target_os = "naos"))]
fn main() {
    println!("pcied: PciDiscoveryFabric is unavailable outside NaOS");
}
