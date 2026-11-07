mod catalog;
mod dedicated_worker;
#[cfg(target_os = "naos")]
mod executor_worker;
mod pci_osc_path;
mod platform_topology;
#[cfg(any(target_os = "naos", test))]
mod power_transition;
#[cfg(target_os = "naos")]
mod region;
mod region_wait;
mod sci_wait;
mod topology_health;
mod volatile;

#[cfg(target_os = "naos")]
mod service;

#[cfg(target_os = "naos")]
#[tokio::main(flavor = "current_thread")]
async fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(servicekit::run("platformd", service::run).await as u8)
}

#[cfg(not(target_os = "naos"))]
fn main() {
    // The Linux binary is a diagnostics stub: FirmwareRoot is a NaOS kernel
    // capability and must not be emulated by a host process.
    println!("platformd: FirmwareRoot is unavailable outside NaOS");
}
