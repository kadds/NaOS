#!/usr/bin/env python3
"""End-to-end checks for NaoIDL `include` fragments against the real tree.

Exercises: fragment detection, include merging/provenance, generator skip
behavior, and the hard-error paths (cycles, redefinitions).
"""
import pathlib
import subprocess
import sys
import tempfile

IDL_DIR = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "idl").resolve()
COMPILER = IDL_DIR / "naoidl.py"
sys.path.insert(0, str(IDL_DIR))
import naoidl  # noqa: E402
from naoidl import IdlError  # noqa: E402


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    # 1. Shared fragments are protocol-less documents.
    fragments = [
        IDL_DIR / "system" / "fragments" / "stat.naidl",
        IDL_DIR / "internal" / "fragments" / "node.naidl",
    ]
    for fragment in fragments:
        check(fragment.is_file(), f"missing fragment fixture: {fragment}")
        check(naoidl.is_fragment(fragment), f"{fragment.name} must parse as a pure fragment")
        document = naoidl.load_document(fragment)
        check(document.protocol is None, f"{fragment.name} must not declare a protocol")
        check(len(document.declarations) > 0, f"{fragment.name} must declare structs")

    # 2. Consumers merge included declarations exactly once and keep theirs.
    for consumer, expected_chain in (
        (IDL_DIR / "system" / "directory.naidl", ["fragments/stat.naidl"]),
        (IDL_DIR / "system" / "file.naidl", ["fragments/stat.naidl"]),
        (IDL_DIR / "internal" / "namespace_binding.naidl", ["fragments/node.naidl"]),
        (IDL_DIR / "internal" / "mount_control.naidl", ["fragments/node.naidl"]),
    ):
        document = naoidl.load_document(consumer)
        check(document.protocol is not None, f"{consumer.name} lost its protocol")
        names = [item.name for item in document.declarations]
        check(len(names) == len(set(names)), f"{consumer.name}: duplicate declaration after merge")
        manifest = naoidl.manifest_for(consumer)
        check(manifest["included"] == expected_chain,
              f"{consumer.name}: unexpected include chain {manifest['included']}")
        declared = {item["name"] for item in manifest["structs"]}
        check(declared == set(names), f"{consumer.name}: manifest structs disagree with merged AST")

    # 3. Generators skip fragments without producing artifacts.
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp)
        result = subprocess.run(
            [sys.executable, str(COMPILER), "generate", str(fragments[0]), str(out)],
            capture_output=True, text=True)
        check(result.returncode == 0, f"generate on a fragment failed: {result.stderr}")
        check(list(out.iterdir()) == [], "generate on a fragment produced artifacts")
        index = out / "index.rs"
        result = subprocess.run(
            [sys.executable, str(COMPILER), "generate-rust-index", str(index),
             str(fragments[0]), str(IDL_DIR / "system" / "directory.naidl")],
            capture_output=True, text=True)
        check(result.returncode == 0, f"generate-rust-index failed: {result.stderr}")
        text = index.read_text(encoding="utf-8")
        check("directory" in text and "stat" not in text,
              "rust index must skip fragment modules")

    # 4. Hard errors stay hard.
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        (root / "a.naidl").write_text('library t;\ninclude "b.naidl";\n', encoding="utf-8")
        (root / "b.naidl").write_text('library t;\ninclude "a.naidl";\n', encoding="utf-8")
        try:
            naoidl.load_document(root / "a.naidl")
        except IdlError as error:
            check("include cycle" in str(error), f"wrong cycle error: {error}")
        else:
            raise AssertionError("include cycle did not fail")
        (root / "sub").mkdir()
        (root / "sub" / "dup.naidl").write_text(
            "library t;\nstruct X { u64 a @id(1); };\n", encoding="utf-8")
        (root / "redef.naidl").write_text(
            'library t;\ninclude "sub/dup.naidl";\nstruct X { u64 b @id(1); };\n',
            encoding="utf-8")
        try:
            naoidl.load_document(root / "redef.naidl")
        except IdlError as error:
            check("redefinition" in str(error), f"wrong redefinition error: {error}")
        else:
            raise AssertionError("redefinition across includes did not fail")

    print("idl_fragment_include_test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
