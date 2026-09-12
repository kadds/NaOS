use std::env;
use std::path::PathBuf;
use std::process::Command;

fn run(mut command: Command) {
    let status = command.status().expect("run native IPC build command");
    assert!(
        status.success(),
        "native IPC build command failed: {status}"
    );
}

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let libipc = manifest.join("../../../../libipc");
    let include = libipc.join("include");
    let naos_include = manifest.join("../../../../include");
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR"));
    let compiler = env::var_os("CXX").unwrap_or_else(|| "c++".into());
    let archiver = env::var_os("AR").unwrap_or_else(|| "ar".into());
    let objects = ["ipc_core", "invocation_core", "ipc_core_c"];

    for source in [
        libipc.join("src/ipc_core.cc"),
        libipc.join("src/invocation_core.cc"),
        libipc.join("src/ipc_core_c.cc"),
    ] {
        println!("cargo:rerun-if-changed={}", source.display());
    }
    println!(
        "cargo:rerun-if-changed={}",
        include.join("naos/ipc_core.hpp").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        include.join("naos/ipc_core.h").display()
    );

    for (name, source) in objects.iter().zip([
        libipc.join("src/ipc_core.cc"),
        libipc.join("src/invocation_core.cc"),
        libipc.join("src/ipc_core_c.cc"),
    ]) {
        let object = out_dir.join(format!("{name}.o"));
        let mut command = Command::new(&compiler);
        command
            .arg("-std=c++20")
            .arg("-fno-exceptions")
            .arg("-fno-rtti")
            .arg("-O2")
            .arg("-I")
            .arg(&include)
            .arg("-I")
            .arg(&naos_include)
            .arg("-c")
            .arg(source)
            .arg("-o")
            .arg(object);
        run(command);
    }

    let archive = out_dir.join("libnaos_ipc_core.a");
    let mut archive_command = Command::new(&archiver);
    archive_command.arg("crus").arg(&archive);
    for name in objects {
        archive_command.arg(out_dir.join(format!("{name}.o")));
    }
    run(archive_command);
    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=naos_ipc_core");
}
