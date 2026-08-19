use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn discover_schemas(repository_root: &PathBuf) -> Vec<PathBuf> {
    let mut schemas = Vec::new();
    for directory in [
        repository_root.join("doc/examples"),
        repository_root.join("idl/system"),
    ] {
        let entries = fs::read_dir(&directory)
            .unwrap_or_else(|error| panic!("failed to read {}: {error}", directory.display()));
        for entry in entries {
            let path = entry
                .unwrap_or_else(|error| {
                    panic!("failed to read {} entry: {error}", directory.display())
                })
                .path();
            if path
                .extension()
                .is_some_and(|extension| extension == "naidl")
            {
                schemas.push(path);
            }
        }
    }
    schemas.sort();
    schemas
}

fn main() {
    let manifest_dir =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let repository_root = manifest_dir
        .join("../../../../..")
        .canonicalize()
        .expect("repository root");
    let compiler = env::var_os("NAOS_IDL_COMPILER")
        .map(PathBuf::from)
        .unwrap_or_else(|| repository_root.join("idl/naoidl.py"));
    let python = env::var_os("NAOS_PYTHON").unwrap_or_else(|| "python3".into());
    let schemas: Vec<PathBuf> = match env::var("NAOS_IDL_SCHEMAS") {
        Ok(value) => value
            .split(';')
            .filter(|schema| !schema.is_empty())
            .map(PathBuf::from)
            .collect(),
        Err(_) => discover_schemas(&repository_root),
    };
    if schemas.is_empty() {
        panic!("NAOS_IDL_SCHEMAS did not contain a schema");
    }
    let output = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR"));

    println!("cargo:rerun-if-changed={}", compiler.display());
    println!("cargo:rerun-if-env-changed=NAOS_IDL_COMPILER");
    println!("cargo:rerun-if-env-changed=NAOS_IDL_SCHEMAS");
    println!("cargo:rerun-if-env-changed=NAOS_PYTHON");
    for template in [
        repository_root.join("idl/templates/rust/naoidl/binding.rs.htt"),
        repository_root.join("idl/templates/rust/naoidl/index.rs.htt"),
    ] {
        println!("cargo:rerun-if-changed={}", template.display());
    }

    for schema in &schemas {
        println!("cargo:rerun-if-changed={}", schema.display());
        let status = Command::new(&python)
            .arg(&compiler)
            .arg("generate-rust")
            .arg(schema)
            .arg(&output)
            .status()
            .unwrap_or_else(|error| panic!("failed to execute IDL compiler: {error}"));
        if !status.success() {
            panic!("IDL compiler failed for {} with {status}", schema.display());
        }
    }

    let mut index_command = Command::new(&python);
    index_command
        .arg(&compiler)
        .arg("generate-rust-index")
        .arg(output.join("bindings.rs"));
    for schema in &schemas {
        index_command.arg(schema);
    }
    let status = index_command
        .status()
        .unwrap_or_else(|error| panic!("failed to execute IDL index compiler: {error}"));
    if !status.success() {
        panic!("IDL Rust index compiler failed with {status}");
    }
}
