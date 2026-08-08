#!/usr/bin/env python3
"""Check that NaoIDL boilerplate is stored in independently editable templates."""

from pathlib import Path
import subprocess
import sys
import tempfile


REQUIRED_TEMPLATES = {
    "header.hpp.htt",
    "runtime.hpp.htt",
    "client.hpp.htt",
    "server.hpp.htt",
    "uapi.h.htt",
    "index.h.htt",
    "resource_metadata_validator.hpp.htt",
    "struct.hpp.htt",
    "type_codec.htt",
    "message.hpp.htt",
    "resource_validator.hpp.htt",
    "disposition_validator.hpp.htt",
}


def check_generated_whitespace(root: Path) -> None:
    source = root / "naos" / "idl" / "system" / "directory.naidl"
    with tempfile.TemporaryDirectory() as temporary_directory:
        output = Path(temporary_directory)
        subprocess.run(
            [sys.executable, str(root / "util" / "naoidl.py"), "generate", str(source), str(output)],
            check=True,
        )
        for path in output.glob("*.hpp"):
            blank_lines = 0
            for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if line.strip():
                    blank_lines = 0
                    continue
                blank_lines += 1
                if blank_lines > 1:
                    raise AssertionError(f"{path.name}:{line_number}: excessive consecutive blank lines")


def main() -> int:
    root = Path(sys.argv[1])
    template_dir = root / "util" / "templates" / "naoidl"
    missing = sorted(name for name in REQUIRED_TEMPLATES if not (template_dir / name).is_file())
    if missing:
        print(f"missing NaoIDL templates: {', '.join(missing)}", file=sys.stderr)
        return 1

    source = (root / "util" / "naoidl.py").read_text(encoding="utf-8")
    if "from jinja2 import" not in source or "def render_template(" not in source:
        print("naoidl.py does not expose the Jinja2 template renderer", file=sys.stderr)
        return 1
    for forbidden in ("method_lines", "lines.append", "lines +=", "def cpp_type(", "def type_cpp("):
        if forbidden in source:
            print(f"naoidl.py still builds C++ output with {forbidden}", file=sys.stderr)
            return 1

    header_context_start = source.index("def header_context(")
    header_context_end = source.index("def generate_header(", header_context_start)
    header_context = source[header_context_start:header_context_end]
    for forbidden in (
        "generate_struct_code(",
        "generate_message_code(",
        "generate_resource_validator(",
        "generate_resource_metadata_validator(",
        "generate_disposition_validator(",
        "struct_declarations",
        "message_declarations",
    ):
        if forbidden in header_context:
            print(f"header_context still assembles rendered C++ with {forbidden}", file=sys.stderr)
            return 1
    for template_name in REQUIRED_TEMPLATES:
        rendered_directly = f'"{template_name}"' in source
        rendered_as_include = any(
            f'include "{template_name}"' in path.read_text(encoding="utf-8")
            or f'import "{template_name}"' in path.read_text(encoding="utf-8")
            for path in template_dir.glob("*.htt")
        )
        if not rendered_directly and not rendered_as_include:
            print(f"naoidl.py does not render {template_name}", file=sys.stderr)
            return 1
    check_generated_whitespace(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
