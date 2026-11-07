#[cfg(target_os = "naos")]
#[tokio::main(flavor = "current_thread")]
async fn main() -> std::process::ExitCode {
    std::process::ExitCode::from(servicekit::run("edud", pcied::run_driver).await as u8)
}

#[cfg(not(target_os = "naos"))]
fn main() {
    println!("edud: QEMU EDU driver requires NaOS");
}
