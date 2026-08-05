#!/usr/bin/env python3
"""Small, deterministic NaoIDL compiler used by the NaOS tree.

The grammar is parsed by Lark and transformed into a semantic manifest and a
freestanding C++ binding whose value fields are encoded explicitly in little
endian. The generated manifest is the compatibility source of truth.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from dataclasses import dataclass, field
from typing import Any, Iterable

try:
    from lark import Lark, Transformer, UnexpectedInput, v_args
except ModuleNotFoundError as error:
    if error.name != "lark":
        raise
    raise SystemExit("naoidl.py requires lark; install util/requirements.txt") from error

try:
    from jinja2 import Environment, FileSystemLoader, StrictUndefined
except ModuleNotFoundError as error:
    if error.name != "jinja2":
        raise
    raise SystemExit("naoidl.py requires jinja2; install util/requirements.txt") from error


TEMPLATE_DIR = pathlib.Path(__file__).resolve().parent / "templates" / "naoidl"
TEMPLATE_ENVIRONMENT = Environment(
    loader=FileSystemLoader(str(TEMPLATE_DIR)),
    undefined=StrictUndefined,
    autoescape=False,
    keep_trailing_newline=True,
    trim_blocks=False,
    lstrip_blocks=False,
)


def render_template(template_name: str, **context: Any) -> str:
    return TEMPLATE_ENVIRONMENT.get_template(template_name).render(**context)


class IdlError(Exception):
    pass


NAOIDL_MAX_METHOD_ID = 256


def unquote(value: str) -> str:
    try:
        return json.loads(value)
    except json.JSONDecodeError as error:
        raise IdlError(f"invalid string literal {value!r}: {error}") from error


@dataclass
class Type:
    name: str
    bound: int | None = None
    element: "Type | None" = None
    scope: str | None = None
    rights: list[str] = field(default_factory=list)
    ownership: str | None = None

    def to_json(self) -> dict[str, Any]:
        result: dict[str, Any] = {"kind": self.name}
        if self.bound is not None:
            result["bound"] = self.bound
        if self.element is not None:
            result["element"] = self.element.to_json()
        if self.scope is not None:
            result["scope"] = self.scope
        if self.rights:
            result["rights"] = list(self.rights)
        if self.ownership is not None:
            result["ownership"] = self.ownership
        return result


@dataclass
class Field:
    name: str
    ordinal: int
    type: Type
    annotations: dict[str, Any] = field(default_factory=dict)

    def to_json(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "id": self.ordinal,
            "type": self.type.to_json(),
            "annotations": self.annotations,
        }


@dataclass
class Method:
    name: str
    ordinal: int
    request: list[Field]
    response: list[Field]
    annotations: dict[str, Any] = field(default_factory=dict)
    request_reserved: list[int] = field(default_factory=list)
    response_reserved: list[int] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "id": self.ordinal,
            "request": [item.to_json() for item in self.request],
            "response": [item.to_json() for item in self.response],
            "annotations": self.annotations,
            "request_reserved": sorted(self.request_reserved),
            "response_reserved": sorted(self.response_reserved),
        }


@dataclass
class Protocol:
    library: str
    name: str
    uuid: str
    revision: int
    features: int
    annotations: dict[str, Any]
    structs: list[dict[str, Any]]
    enums: list[dict[str, Any]]
    methods: list[Method]
    reserved: list[int] = field(default_factory=list)

    def manifest(self, source_name: str) -> dict[str, Any]:
        methods = sorted((method.to_json() for method in self.methods), key=lambda item: item["id"])
        return {
            "format": 1,
            "source": source_name,
            "library": self.library,
            "protocol": self.name,
            "uuid": self.uuid,
            "revision": self.revision,
            "features": self.features,
            "annotations": self.annotations,
            "structs": sorted(self.structs, key=lambda item: item["name"]),
            "enums": sorted(self.enums, key=lambda item: item["name"]),
            "methods": methods,
            "reserved": sorted(self.reserved),
        }


IDL_GRAMMAR = r"""
start: library_decl declaration* protocol_decl
library_decl: "library" qualified_name ";"
?declaration: struct_decl | enum_decl
struct_decl: "struct" CNAME field_block ";"
enum_decl: _enum_kind CNAME "{" enum_value* "}" ";"
_enum_kind: "enum" | "bits" | "error"
enum_value: CNAME annotations ";"
protocol_decl: "protocol" CNAME annotations "{" protocol_member* "}" ";"
?protocol_member: method_decl | reserved_method
method_decl: "method" CNAME annotations method_request "->" method_response ";"
method_request: field_block -> request_block
              | -> empty_request
method_response: field_block -> response_block
                | -> empty_response
reserved_method: "reserved" annotations ";"
field_block: "{" field_member* "}"
?field_member: field_decl | reserved_field
field_decl: type CNAME annotations ";"
reserved_field: "reserved" annotations ";"
annotations: annotation*
annotation: "@" CNAME -> flag_annotation
          | "@" CNAME "(" [annotation_values] ")" -> value_annotation
annotation_values: annotation_value ("," annotation_value)*
?annotation_value: ESCAPED_STRING -> string_value
                 | INT -> integer_value
                 | CNAME -> identifier_value
type: PRIMITIVE -> primitive_type
    | "bytes" "<" INT ">" -> bytes_type
    | "string" "<" INT ">" -> string_type
    | "vector" "<" type "," INT ">" -> vector_type
    | "array" "<" type "," INT ">" -> array_type
    | handle_type
    | client_end_type
    | server_end_type
    | CNAME -> named_type
handle_type: "handle" "<" CNAME "," rights ["," OWNERSHIP] ">"
client_end_type: "client_end" "<" CNAME "," rights ["," OWNERSHIP] ">"
server_end_type: "server_end" "<" CNAME "," rights ["," OWNERSHIP] ">"
rights: CNAME ("+" CNAME)*
qualified_name: CNAME ("." CNAME)*
PRIMITIVE: "bool" | "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64" | "f32" | "f64"
OWNERSHIP: "move" | "duplicate"
%import common.CNAME
%import common.ESCAPED_STRING
%import common.INT
%import common.WS
%ignore WS
%ignore /#[^\n]*/
%ignore /\/\/[^\n]*/
"""


@dataclass
class FieldNode:
    name: str
    type_: Type
    annotations: dict[str, Any]
    line: int
    column: int


@dataclass
class ReservedNode:
    annotations: dict[str, Any]
    line: int
    column: int


@dataclass
class FieldBlock:
    fields: list[FieldNode]
    reserved: list[ReservedNode]


@dataclass
class EnumValueNode:
    name: str
    annotations: dict[str, Any]
    line: int
    column: int


@dataclass
class StructNode:
    name: str
    fields: FieldBlock


@dataclass
class EnumNode:
    name: str
    values: list[EnumValueNode]


@dataclass
class MethodNode:
    name: str
    annotations: dict[str, Any]
    request: FieldBlock
    response: FieldBlock
    line: int
    column: int


@dataclass
class ProtocolNode:
    name: str
    annotations: dict[str, Any]
    members: list[MethodNode | ReservedNode]
    line: int
    column: int


@dataclass
class DocumentNode:
    library: str
    declarations: list[StructNode | EnumNode]
    protocol: ProtocolNode


class AstBuilder(Transformer):
    @staticmethod
    def _position(meta: Any) -> tuple[int, int]:
        return meta.line, meta.column

    def start(self, items: list[Any]) -> DocumentNode:
        library = items[0]
        protocol = items[-1]
        return DocumentNode(library, items[1:-1], protocol)

    def library_decl(self, items: list[Any]) -> str:
        return items[0]

    def qualified_name(self, items: list[Any]) -> str:
        return ".".join(str(item) for item in items)

    @v_args(meta=True, inline=True)
    def struct_decl(self, meta: Any, name: Any, fields: FieldBlock) -> StructNode:
        return StructNode(str(name), fields)

    @v_args(inline=True)
    def enum_decl(self, name: Any, *values: EnumValueNode) -> EnumNode:
        return EnumNode(str(name), list(values))

    @v_args(meta=True, inline=True)
    def protocol_decl(
        self, meta: Any, name: Any, annotations: dict[str, Any], *members: MethodNode | ReservedNode
    ) -> ProtocolNode:
        line, column = self._position(meta)
        return ProtocolNode(str(name), annotations, list(members), line, column)

    @v_args(meta=True, inline=True)
    def method_decl(
        self,
        meta: Any,
        name: Any,
        annotations: dict[str, Any],
        request: FieldBlock,
        response: FieldBlock,
    ) -> MethodNode:
        line, column = self._position(meta)
        return MethodNode(str(name), annotations, request, response, line, column)

    @v_args(meta=True, inline=True)
    def reserved_method(self, meta: Any, annotations: dict[str, Any]) -> ReservedNode:
        line, column = self._position(meta)
        return ReservedNode(annotations, line, column)

    def request_block(self, items: list[Any]) -> FieldBlock:
        return items[0]

    def empty_request(self, _items: list[Any]) -> FieldBlock:
        return FieldBlock([], [])

    def response_block(self, items: list[Any]) -> FieldBlock:
        return items[0]

    def empty_response(self, _items: list[Any]) -> FieldBlock:
        return FieldBlock([], [])

    @v_args(inline=True)
    def field_block(self, *members: FieldNode | ReservedNode) -> FieldBlock:
        fields = [member for member in members if isinstance(member, FieldNode)]
        reserved = [member for member in members if isinstance(member, ReservedNode)]
        return FieldBlock(fields, reserved)

    @v_args(meta=True, inline=True)
    def field_decl(self, meta: Any, type_: Type, name: Any, annotations: dict[str, Any]) -> FieldNode:
        line, column = self._position(meta)
        return FieldNode(str(name), type_, annotations, line, column)

    @v_args(meta=True, inline=True)
    def reserved_field(self, meta: Any, annotations: dict[str, Any]) -> ReservedNode:
        line, column = self._position(meta)
        return ReservedNode(annotations, line, column)

    @v_args(meta=True, inline=True)
    def enum_value(self, meta: Any, name: Any, annotations: dict[str, Any]) -> EnumValueNode:
        line, column = self._position(meta)
        return EnumValueNode(str(name), annotations, line, column)

    @v_args(inline=True)
    def annotations(self, *items: tuple[str, Any]) -> dict[str, Any]:
        return dict(items)

    @v_args(inline=True)
    def flag_annotation(self, name: Any) -> tuple[str, Any]:
        return str(name), True

    def value_annotation(self, items: list[Any]) -> tuple[str, Any]:
        values = items[1] if len(items) == 2 else []
        return str(items[0]), values[0] if len(values) == 1 else values

    def annotation_values(self, items: list[Any]) -> list[Any]:
        return items

    @v_args(inline=True)
    def string_value(self, value: Any) -> str:
        return unquote(str(value))

    @v_args(inline=True)
    def integer_value(self, value: Any) -> int:
        return int(str(value), 0)

    @v_args(inline=True)
    def identifier_value(self, value: Any) -> str:
        return str(value)

    @v_args(inline=True)
    def primitive_type(self, name: Any) -> Type:
        return Type("u8" if str(name) == "bool" else str(name))

    @v_args(inline=True)
    def bytes_type(self, bound: Any) -> Type:
        return Type("bytes", bound=int(str(bound), 0))

    @v_args(inline=True)
    def string_type(self, bound: Any) -> Type:
        return Type("string", bound=int(str(bound), 0))

    @v_args(inline=True)
    def vector_type(self, element: Type, bound: Any) -> Type:
        return Type("vector", bound=int(str(bound), 0), element=element)

    @v_args(inline=True)
    def array_type(self, element: Type, bound: Any) -> Type:
        return Type("array", bound=int(str(bound), 0), element=element)

    @staticmethod
    def _capability_type(kind: str, items: list[Any]) -> Type:
        scope = str(items[0])
        rights = sorted(str(right) for right in items[1])
        ownership = str(items[2]) if len(items) == 3 else "move"
        return Type(kind, scope=scope, rights=rights, ownership=ownership)

    def handle_type(self, items: list[Any]) -> Type:
        return self._capability_type("handle", items)

    def client_end_type(self, items: list[Any]) -> Type:
        return self._capability_type("client_end", items)

    def server_end_type(self, items: list[Any]) -> Type:
        return self._capability_type("server_end", items)

    @v_args(inline=True)
    def rights(self, first: Any, *rest: Any) -> list[str]:
        return [str(first), *(str(item) for item in rest)]

    @v_args(inline=True)
    def named_type(self, name: Any) -> Type:
        return Type(str(name))

    def type(self, items: list[Type]) -> Type:
        return items[0]


IDL_PARSER = Lark(
    IDL_GRAMMAR,
    parser="lalr",
    lexer="contextual",
    maybe_placeholders=False,
    propagate_positions=True,
)


def _semantic_error(message: str, node: Any) -> IdlError:
    line = getattr(node, "line", 0)
    column = getattr(node, "column", 0)
    return IdlError(f"{message} at {line}:{column}") if line else IdlError(message)


def _required_id(annotations: dict[str, Any], node: Any, description: str) -> int:
    value = annotations.pop("id", None)
    if not isinstance(value, int):
        raise _semantic_error(f"{description} requires @id(N)", node)
    return value


def _assert_unique_ids(items: Iterable[Field | Method], kind: str) -> None:
    ids = [item.ordinal for item in items]
    if len(ids) != len(set(ids)) or any(item_id <= 0 for item_id in ids):
        raise IdlError(f"{kind} IDs must be unique positive integers")


def _build_fields(block: FieldBlock) -> tuple[list[Field], list[int]]:
    fields: list[Field] = []
    for node in block.fields:
        _validate_type(node.type_, node)
        annotations = dict(node.annotations)
        fields.append(Field(node.name, _required_id(annotations, node, f"field {node.name}"), node.type_, annotations))
    reserved = [_required_id(dict(node.annotations), node, "reserved field") for node in block.reserved]
    _assert_unique_ids(fields, "field")
    if len(reserved) != len(set(reserved)) or any(item <= 0 for item in reserved):
        raise IdlError("reserved field IDs must be unique positive integers")
    if set(reserved) & {item.ordinal for item in fields}:
        raise IdlError("reserved field ID overlaps a field")
    return fields, sorted(reserved)


def _validate_type(type_: Type, node: Any) -> None:
    bounds = {
        "bytes": (1, 65536),
        "string": (1, 65536),
        "vector": (1, 65536),
        "array": (1, 256),
    }
    if type_.name in {"bytes", "string", "vector", "array", "handle", "client_end", "server_end"}:
        if type_.name in bounds:
            if type_.bound is None:
                raise _semantic_error(f"{type_.name} requires an explicit bound", node)
            lower, upper = bounds[type_.name]
            if not lower <= type_.bound <= upper:
                raise _semantic_error(f"{type_.name} bound must be in {lower}..{upper}", node)
        elif type_.scope is None:
            raise _semantic_error(f"{type_.name} requires a scope and rights", node)
    if type_.element is not None:
        _validate_type(type_.element, node)


def _build_struct(node: StructNode) -> dict[str, Any]:
    fields, reserved = _build_fields(node.fields)
    return {"name": node.name, "fields": [field.to_json() for field in fields], "reserved": reserved}


def _build_enum(node: EnumNode) -> dict[str, Any]:
    values: list[dict[str, Any]] = []
    for value_node in node.values:
        annotations = dict(value_node.annotations)
        value = _required_id(annotations, value_node, f"enum value {value_node.name}")
        values.append({"name": value_node.name, "id": value, "annotations": annotations})
    ids = [value["id"] for value in values]
    if len(ids) != len(set(ids)):
        raise IdlError(f"enum {node.name} has duplicate IDs")
    return {"name": node.name, "values": sorted(values, key=lambda item: item["id"])}


def _build_protocol(document: DocumentNode) -> Protocol:
    annotations = dict(document.protocol.annotations)
    uuid = annotations.pop("uuid", None)
    revision = annotations.pop("revision", 1)
    features = annotations.pop("features", 0)
    if not isinstance(uuid, str) or not re.fullmatch(
        r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}", uuid
    ):
        raise _semantic_error(
            'protocol requires explicit @uuid("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")', document.protocol
        )
    if not isinstance(revision, int) or revision <= 0 or not isinstance(features, int):
        raise _semantic_error("revision must be positive and features must be an integer", document.protocol)

    methods: list[Method] = []
    reserved: list[int] = []
    for member in document.protocol.members:
        if isinstance(member, ReservedNode):
            reserved.append(_required_id(dict(member.annotations), member, "reserved method"))
            if reserved[-1] > NAOIDL_MAX_METHOD_ID:
                raise _semantic_error(f"method IDs must be in 1..{NAOIDL_MAX_METHOD_ID}", member)
            continue
        request, request_reserved = _build_fields(member.request)
        response, response_reserved = _build_fields(member.response)
        method_annotations = dict(member.annotations)
        ordinal = _required_id(method_annotations, member, f"method {member.name}")
        if ordinal > NAOIDL_MAX_METHOD_ID or ordinal <= 0:
            raise _semantic_error(f"method IDs must be in 1..{NAOIDL_MAX_METHOD_ID}", member)
        methods.append(
            Method(member.name, ordinal, request, response, method_annotations, request_reserved, response_reserved)
        )
    _assert_unique_ids(methods, "method")
    if len(reserved) != len(set(reserved)) or any(item <= 0 for item in reserved):
        raise IdlError("reserved IDs must be unique positive integers")
    if set(reserved) & {item.ordinal for item in methods}:
        raise IdlError("reserved method ID overlaps a method")

    structs = [_build_struct(node) for node in document.declarations if isinstance(node, StructNode)]
    enums = [_build_enum(node) for node in document.declarations if isinstance(node, EnumNode)]
    return Protocol(document.library, document.protocol.name, uuid.lower(), revision, features, annotations,
                    structs, enums, methods, reserved)


def parse_file(path: pathlib.Path) -> Protocol:
    source = path.read_text(encoding="utf-8")
    try:
        document = AstBuilder().transform(IDL_PARSER.parse(source))
    except UnexpectedInput as error:
        raise IdlError(f"syntax error at {error.line}:{error.column}: {error}") from error
    return _build_protocol(document)


PRIMITIVE_SIZES = {"u8": 1, "u16": 2, "u32": 4, "u64": 8, "i8": 1, "i16": 2, "i32": 4, "i64": 8,
                   "f32": 4, "f64": 8}
RESOURCE_KINDS = {"handle", "client_end", "server_end"}
NAOIDL_MAX_RESOURCES = 64


def resource_bound(type_: dict[str, Any], named_structs: dict[str, dict[str, Any]] | None = None,
                   stack: tuple[str, ...] = ()) -> int:
    """Return the maximum number of resource slots represented by a value."""
    named_structs = named_structs or {}
    kind = type_["kind"]
    if kind in RESOURCE_KINDS:
        return 1
    if kind in ("array", "vector"):
        return int(type_["bound"]) * resource_bound(type_["element"], named_structs, stack)
    struct_ = named_structs.get(kind)
    if struct_ is None:
        return 0
    if kind in stack:
        raise IdlError(f"recursive resource-bearing struct {kind}")
    return sum(resource_bound(field_["type"], named_structs, (*stack, kind))
               for field_ in struct_["fields"])


def type_has_resources(type_: dict[str, Any], named_structs: dict[str, dict[str, Any]] | None = None) -> bool:
    return resource_bound(type_, named_structs) != 0


def type_size(type_: dict[str, Any], named_sizes: dict[str, int | None] | None = None) -> int | None:
    named_sizes = named_sizes or {}
    kind = type_["kind"]
    if kind in PRIMITIVE_SIZES:
        return PRIMITIVE_SIZES[kind]
    if kind in ("handle", "client_end", "server_end"):
        return 4  # canonical resource slot index, never a process-local handle
    if kind == "array":
        element = type_size(type_["element"], named_sizes)
        return None if element is None else element * type_["bound"]
    return named_sizes.get(kind)


def is_dynamic(type_: dict[str, Any], named_sizes: dict[str, int | None] | None = None) -> bool:
    return type_size(type_, named_sizes) is None


def layout_fields(fields: list[dict[str, Any]], named_sizes: dict[str, int | None] | None = None) -> list[dict[str, Any]]:
    offset = 0
    result = []
    for field_ in sorted(fields, key=lambda item: item["id"]):
        size = type_size(field_["type"], named_sizes)
        result.append({"id": field_["id"], "name": field_["name"], "offset": offset, "size": size})
        offset += size if size is not None else 16  # canonical offset/count descriptor
    return result


def cpp_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", value)


def macro_name(value: str) -> str:
    value = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", value)
    return re.sub(r"[^A-Za-z0-9]", "_", value).upper()


def type_is_dynamic(type_: dict[str, Any], named_sizes: dict[str, int | None]) -> bool:
    return is_dynamic(type_, named_sizes)


def struct_context(struct_: dict[str, Any], named_sizes: dict[str, int | None]) -> dict[str, Any]:
    name = cpp_name(struct_["name"])
    fields = sorted(struct_["fields"], key=lambda item: item["id"])
    if any(type_is_dynamic(field_["type"], named_sizes) for field_ in fields):
        raise IdlError(f"struct {struct_['name']} contains dynamic data; use a method payload in v1")
    return {
        "name": name,
        "fields": fields,
        "cpp_name": cpp_name,
        "resource_kinds": RESOURCE_KINDS,
    }


def generate_struct_code(struct_: dict[str, Any], named_sizes: dict[str, int | None]) -> str:
    return render_template(
        "struct.hpp.htt",
        **struct_context(struct_, named_sizes),
    ).rstrip("\n")


def resource_validator_context(name: str, fields: list[dict[str, Any]],
                               named_structs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    return {
        "name": name,
        "fields": fields,
        "named_structs": named_structs,
        "resource_kinds": RESOURCE_KINDS,
        "type_has_resources": lambda type_: type_has_resources(type_, named_structs),
        "cpp_name": cpp_name,
        "has_resources": any(type_has_resources(field_["type"], named_structs) for field_ in fields),
    }


def resource_metadata_validator_context(name: str, fields: list[dict[str, Any]],
                                        named_structs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    return {
        "name": name,
        "fields": fields,
        "named_structs": named_structs,
        "resource_kinds": RESOURCE_KINDS,
        "bindings": {
            "handle": "NA_BINDING_NONE",
            "client_end": "NA_BINDING_CLIENT_END",
            "server_end": "NA_BINDING_SERVER_END",
        },
        "scope": disposition_scope,
        "rights": disposition_rights,
        "cpp_name": cpp_name,
        "has_resources": any(type_has_resources(field_["type"], named_structs) for field_ in fields),
    }


def disposition_validator_context(name: str, fields: list[dict[str, Any]],
                                  named_structs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    return {
        "name": name,
        "fields": fields,
        "named_structs": named_structs,
        "resource_kinds": RESOURCE_KINDS,
        "type_has_resources": lambda type_: type_has_resources(type_, named_structs),
        "scope": disposition_scope,
        "rights": disposition_rights,
        "cpp_name": cpp_name,
        "has_resources": any(type_has_resources(field_["type"], named_structs) for field_ in fields),
    }


def message_context(protocol: str, method: dict[str, Any], direction: str,
                    named_sizes: dict[str, int | None]) -> dict[str, Any]:
    suffix = "request" if direction == "request" else "response"
    message_name = f"{cpp_name(method['name'])}_{suffix}"
    fields = [dict(field_) for field_ in sorted(method[direction], key=lambda item: item["id"])]
    dynamic = [field_ for field_ in fields if type_is_dynamic(field_["type"], named_sizes)]
    if any(field_["type"]["kind"] not in ("bytes", "string", "vector") for field_ in dynamic):
        raise IdlError(f"{message_name}: dynamic named fields are not supported in v1")
    inline_dynamic = [field_ for field_ in dynamic if field_.get("annotations", {}).get("inline", False)]
    descriptor_dynamic = [field_ for field_ in dynamic if field_ not in inline_dynamic]
    if inline_dynamic and descriptor_dynamic:
        raise IdlError(f"{message_name}: inline and descriptor dynamic fields cannot be mixed")
    first_inline = next((index for index, field_ in enumerate(fields) if field_ in inline_dynamic), len(fields))
    if any(field_ not in inline_dynamic for field_ in fields[first_inline:]):
        raise IdlError(f"{message_name}: inline dynamic fields must be trailing")

    header_size = sum(type_size(field_["type"], named_sizes) or 16 for field_ in fields if field_ not in inline_dynamic)
    descriptor_index = {id(field_): index for index, field_ in enumerate(descriptor_dynamic)}
    dynamic_index = {id(field_): index for index, field_ in enumerate(dynamic)}
    for field_ in fields:
        field_["cpp_name"] = cpp_name(field_["name"])
        field_["is_dynamic"] = field_ in dynamic
        field_["is_inline"] = field_ in inline_dynamic
        field_["is_descriptor"] = field_ in descriptor_dynamic
        field_["descriptor_index"] = descriptor_index.get(id(field_))
        field_["payload_index"] = dynamic_index.get(id(field_), 0)
        field_["count_member"] = "count" if field_["type"]["kind"] == "vector" else "size"
        length_field = field_.get("annotations", {}).get("length_field")
        field_["length_field"] = cpp_name(length_field) if length_field is not None else None
        field_["count_is_remainder"] = field_["is_inline"] and field_["length_field"] is None
        field_["element_size"] = type_size(field_["type"].get("element"), named_sizes) if field_["type"]["kind"] == "vector" else None
        if field_["element_size"] is None and field_["type"]["kind"] == "vector":
            field_["element_size"] = 0
    return {
        "name": message_name,
        "protocol": protocol,
        "fields": fields,
        "dynamic_fields": dynamic,
        "descriptor_fields": descriptor_dynamic,
        "header_size": header_size,
    }


def generate_message_code(protocol: str, method: dict[str, Any], direction: str,
                          named_sizes: dict[str, int | None]) -> str:
    return render_template(
        "message.hpp.htt",
        message=message_context(protocol, method, direction, named_sizes),
        cpp_name=cpp_name,
        resource_kinds=RESOURCE_KINDS,
        named_sizes=named_sizes,
        type_size=type_size,
    ).rstrip("\n")

def header_context(manifest: dict[str, Any]) -> dict[str, Any]:
    guard = re.sub(r"[^A-Za-z0-9]", "_", f"NAOIDL_{manifest['library']}_{manifest['protocol']}_H").upper()
    namespace = f"{manifest['library'].replace('.', '::')}::{manifest['protocol']}"
    named_structs = {item["name"]: item for item in manifest["structs"]}
    scope = int(manifest["annotations"].get("scope", 0))
    if scope == 0:
        scope = int(hashlib.sha256(manifest["uuid"].encode()).hexdigest()[:16], 16) or 1

    named_sizes: dict[str, int | None] = {item["name"]: 4 for item in manifest["enums"]}
    for struct_ in manifest["structs"]:
        named_sizes[struct_["name"]] = None if any(is_dynamic(field_["type"], named_sizes) for field_ in struct_["fields"]) else sum(
            type_size(field_["type"], named_sizes) or 0 for field_ in struct_["fields"])
    structs = [struct_context(struct_, named_sizes) for struct_ in manifest["structs"]]
    declarations = [
        {
            "message": message_context(manifest["protocol"], method, direction, named_sizes),
            "resource": resource_validator_context(f"{cpp_name(method['name'])}_{direction}", method[direction], named_structs),
            "metadata": resource_metadata_validator_context(f"{cpp_name(method['name'])}_{direction}", method[direction], named_structs),
            "disposition": disposition_validator_context(f"{cpp_name(method['name'])}_{direction}", method[direction], named_structs),
        }
        for method in manifest["methods"]
        for direction in ("request", "response")
    ]

    max_request = max((int(item.get("annotations", {}).get("max_bytes", 65536)) for item in manifest["methods"]), default=65536)
    max_response = max_request
    if max_request == 0:
        max_request = max_response = 65536
    max_method = max((item["id"] for item in manifest["methods"]), default=0)
    max_resources = max((max(
        sum(resource_bound(field_["type"], named_structs) for field_ in item["request"]),
        sum(resource_bound(field_["type"], named_structs) for field_ in item["response"]),
    ) for item in manifest["methods"]), default=0)
    if max_resources > NAOIDL_MAX_RESOURCES:
        raise IdlError(f"protocol requires {max_resources} resources, limit is {NAOIDL_MAX_RESOURCES}")

    method_bitmap = [0] * 4
    oneway_bitmap = [0] * 4
    for method in manifest["methods"]:
        method_id = int(method["id"])
        if method_id <= 0 or method_id > 256:
            raise IdlError("method IDs must be in 1..256")
        method_bitmap[(method_id - 1) // 64] |= 1 << ((method_id - 1) % 64)
        if method.get("annotations", {}).get("oneway_best_effort", False) or method.get("annotations", {}).get("oneway", False):
            oneway_bitmap[(method_id - 1) // 64] |= 1 << ((method_id - 1) % 64)
    return {
        "guard": guard,
        "namespace": namespace,
        "uuid_values": list(bytes.fromhex(manifest["uuid"].replace("-", ""))),
        "scope": scope,
        "revision": manifest["revision"],
        "features": manifest["features"],
        "protocol_flags": int(bool(manifest["annotations"].get("allow_oneway", False))) * 1,
        "enums": manifest["enums"],
        "methods": manifest["methods"],
        "cpp_name": cpp_name,
        "type_size": type_size,
        "named_sizes": named_sizes,
        "resource_kinds": RESOURCE_KINDS,
        "structs": structs,
        "declarations": declarations,
        "max_method": max_method,
        "max_request": max_request,
        "max_response": max_response,
        "max_resources": max_resources,
        "method_bitmap": method_bitmap,
        "oneway_bitmap": oneway_bitmap,
    }


def generate_header(manifest: dict[str, Any]) -> str:
    return render_template("header.hpp.htt", **header_context(manifest))

def generate_resource_validator(name: str, fields: list[dict[str, Any]],
                                named_structs: dict[str, dict[str, Any]] | None = None) -> str:
    named_structs = named_structs or {}
    return render_template(
        "resource_validator.hpp.htt",
        **resource_validator_context(name, fields, named_structs),
    ).rstrip("\n")


def generate_resource_metadata_validator(name: str, fields: list[dict[str, Any]],
                                         named_structs: dict[str, dict[str, Any]] | None = None) -> str:
    named_structs = named_structs or {}
    return render_template(
        "resource_metadata_validator.hpp.htt",
        **resource_metadata_validator_context(name, fields, named_structs),
    ).rstrip("\n")


def disposition_scope(scope: str | None) -> str | None:
    return {
        "stream": "NA_SCOPE_STREAM", "file": "NA_SCOPE_FILE", "directory": "NA_SCOPE_DIRECTORY",
        "tty_control": "NA_SCOPE_TTY_CONTROL", "pty_admin": "NA_SCOPE_PTY_ADMIN", "test_echo": "NA_SCOPE_TEST_ECHO",
        "memory_object": "NA_SCOPE_MEMORY_OBJECT", "shared_ring": "NA_SCOPE_SHARED_RING",
    }.get(scope)


def disposition_rights(rights: list[str]) -> str | None:
    values = {
        "duplicate": "NA_RIGHT_DUPLICATE", "transfer": "NA_RIGHT_TRANSFER", "wait": "NA_RIGHT_WAIT",
        "inspect": "NA_RIGHT_INSPECT",
    }
    known = [values[item] for item in rights if item in values]
    return " | ".join(known) if known else None


def generate_disposition_validator(name: str, fields: list[dict[str, Any]],
                                   named_structs: dict[str, dict[str, Any]] | None = None) -> str:
    named_structs = named_structs or {}
    return render_template(
        "disposition_validator.hpp.htt",
        **disposition_validator_context(name, fields, named_structs),
    ).rstrip("\n")


def binding_methods(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        {
            "name": cpp_name(method["name"]),
            "request_name": f"{cpp_name(method['name'])}_request",
            "response_name": f"{cpp_name(method['name'])}_response",
            "oneway": bool(method.get("annotations", {}).get("oneway_best_effort", False) or method.get("annotations", {}).get("oneway", False)),
        }
        for method in manifest["methods"]
    ]


def generate_client_binding(manifest: dict[str, Any]) -> str:
    return render_template(
        "client.hpp.htt",
        namespace=f"{manifest['library'].replace('.', '::')}::{manifest['protocol']}",
        protocol=manifest["protocol"],
        class_name=f"{cpp_name(manifest['protocol'])}Client",
        methods=binding_methods(manifest),
    )


def generate_server_binding(manifest: dict[str, Any]) -> str:
    return render_template(
        "server.hpp.htt",
        namespace=f"{manifest['library'].replace('.', '::')}::{manifest['protocol']}",
        protocol=manifest["protocol"],
        class_name=f"{cpp_name(manifest['protocol'])}Server",
        methods=binding_methods(manifest),
    )

def manifest_for(path: pathlib.Path) -> dict[str, Any]:
    protocol = parse_file(path)
    manifest = protocol.manifest(path.name)
    named_sizes: dict[str, int | None] = {}
    for enum_ in manifest["enums"]:
        named_sizes[enum_["name"]] = 4
    for struct_ in manifest["structs"]:
        named_sizes[struct_["name"]] = None
    for struct_ in manifest["structs"]:
        named_sizes[struct_["name"]] = None if any(is_dynamic(field_["type"], named_sizes) for field_ in struct_["fields"]) else sum(
            type_size(field_["type"], named_sizes) or 0 for field_ in struct_["fields"])
        struct_["layout"] = layout_fields(struct_["fields"], named_sizes)
    for method in manifest["methods"]:
        method["request_layout"] = layout_fields(method["request"], named_sizes)
        method["response_layout"] = layout_fields(method["response"], named_sizes)
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    manifest["schema_hash"] = hashlib.sha256(encoded).hexdigest()
    return manifest


def write_json(path: pathlib.Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"{json.dumps(value, indent=2, sort_keys=True)}\n", encoding="utf-8")


def compare_type(old: dict[str, Any], new: dict[str, Any], where: str, errors: list[str]) -> None:
    if old.get("kind") != new.get("kind"):
        errors.append(f"{where}: type changed")
        return
    kind = old.get("kind")
    if kind in ("bytes", "string", "vector", "array"):
        old_bound = old.get("bound")
        new_bound = new.get("bound")
        if old_bound != new_bound and (new_bound is None or old_bound is None or new_bound < old_bound):
            errors.append(f"{where}: bound shrank")
        if kind in ("vector", "array"):
            compare_type(old["element"], new["element"], f"{where} element", errors)
    for key in ("scope", "rights", "ownership"):
        if old.get(key) != new.get(key):
            errors.append(f"{where}: {key} changed")


def check_compatibility(old: dict[str, Any], new: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    for key in ("library", "protocol", "uuid"):
        if old.get(key) != new.get(key):
            errors.append(f"{key}: identity changed")
    if new.get("revision", 0) < old.get("revision", 0):
        errors.append("revision: decreased")
    if (int(new.get("features", 0)) & int(old.get("features", 0))) != int(old.get("features", 0)):
        errors.append("features: previously advertised feature removed")
    old_reserved = set(old.get("reserved", []))
    new_reserved = set(new.get("reserved", []))
    if not old_reserved.issubset(new_reserved):
        errors.append("reserved IDs: an old reserved ID was released")
    old_methods = {item["id"]: item for item in old.get("methods", [])}
    new_methods = {item["id"]: item for item in new.get("methods", [])}
    for ordinal, previous in old_methods.items():
        current = new_methods.get(ordinal)
        if current is None:
            errors.append(f"method @{ordinal}: removed without reserve")
            continue
        if previous["name"] != current["name"]:
            errors.append(f"method @{ordinal}: name changed")
        if previous.get("annotations", {}) != current.get("annotations", {}):
            errors.append(f"method @{ordinal}: annotations changed")
        for direction in ("request", "response"):
            old_fields = {item["id"]: item for item in previous.get(direction, [])}
            new_fields = {item["id"]: item for item in current.get(direction, [])}
            old_direction_reserved = set(previous.get(f"{direction}_reserved", []))
            new_direction_reserved = set(current.get(f"{direction}_reserved", []))
            if not old_direction_reserved.issubset(new_direction_reserved):
                errors.append(f"method @{ordinal} {direction}: an old reserved ID was released")
            for field_id, old_field in old_fields.items():
                new_field = new_fields.get(field_id)
                if new_field is None:
                    if field_id not in new_direction_reserved:
                        errors.append(f"method @{ordinal} {direction} field @{field_id}: removed without reserve")
                    continue
                if old_field["name"] != new_field["name"]:
                    errors.append(f"method @{ordinal} {direction} field @{field_id}: name changed")
                compare_type(old_field["type"], new_field["type"], f"method @{ordinal} {direction} field @{field_id}", errors)
            for field_id, new_field in new_fields.items():
                if field_id not in old_fields and not (
                    direction == "response" or current.get("annotations", {}).get("extensible") or
                    new_field.get("annotations", {}).get("optional")
                ):
                    errors.append(f"method @{ordinal} {direction} field @{field_id}: required field added")
    old_structs = {item["name"]: item for item in old.get("structs", [])}
    new_structs = {item["name"]: item for item in new.get("structs", [])}
    for name, previous in old_structs.items():
        current = new_structs.get(name)
        if current is None:
            errors.append(f"struct {name}: removed")
            continue
        old_fields = {item["id"]: item for item in previous.get("fields", [])}
        new_fields = {item["id"]: item for item in current.get("fields", [])}
        old_struct_reserved = set(previous.get("reserved", []))
        new_struct_reserved = set(current.get("reserved", []))
        if not old_struct_reserved.issubset(new_struct_reserved):
            errors.append(f"struct {name}: an old reserved ID was released")
        for field_id, old_field in old_fields.items():
            new_field = new_fields.get(field_id)
            if new_field is None:
                if field_id not in new_struct_reserved:
                    errors.append(f"struct {name} field @{field_id}: removed without reserve")
                continue
            if old_field["name"] != new_field["name"]:
                errors.append(f"struct {name} field @{field_id}: name changed")
            compare_type(old_field["type"], new_field["type"], f"struct {name} field @{field_id}", errors)
    old_enums = {item["name"]: item for item in old.get("enums", [])}
    new_enums = {item["name"]: item for item in new.get("enums", [])}
    for name, previous in old_enums.items():
        current = new_enums.get(name)
        if current is None:
            errors.append(f"enum {name}: removed")
            continue
        old_values = {item["id"]: item for item in previous.get("values", [])}
        new_values = {item["id"]: item for item in current.get("values", [])}
        for value_id, old_value in old_values.items():
            new_value = new_values.get(value_id)
            if new_value is None or new_value["name"] != old_value["name"]:
                errors.append(f"enum {name} value @{value_id}: removed or renamed")
    return errors


def command_generate(args: argparse.Namespace) -> int:
    manifest = manifest_for(pathlib.Path(args.input))
    output = pathlib.Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / f"{manifest['protocol']}.abi.json", manifest)
    header = generate_header(manifest)
    (output / f"{manifest['protocol']}.hpp").write_text(header, encoding="utf-8")
    (output / f"{manifest['protocol']}_types.hpp").write_text(header, encoding="utf-8")
    (output / f"{manifest['protocol']}_client.hpp").write_text(generate_client_binding(manifest), encoding="utf-8")
    (output / f"{manifest['protocol']}_server.hpp").write_text(generate_server_binding(manifest), encoding="utf-8")
    uapi_name = macro_name(manifest["annotations"].get("uapi_name", manifest["protocol"]))
    scope_name = macro_name(manifest["annotations"].get("scope_name", uapi_name))
    scope = int(manifest["annotations"].get("scope", 0))
    if scope == 0:
        scope = int(hashlib.sha256(manifest["uuid"].encode()).hexdigest()[:16], 16) or 1
    uapi = render_template(
        "uapi.h.htt",
        protocol_macro=cpp_name(manifest["protocol"]).upper(),
        scope_macro=scope_name,
        scope=scope,
        revision=manifest["revision"],
        method_count=max((item["id"] for item in manifest["methods"]), default=0),
        methods=[
            {
                "uapi_macro": uapi_name,
                "name": macro_name(method.get("annotations", {}).get("uapi_name", method["name"])),
                "id": method["id"],
            }
            for method in manifest["methods"]
        ],
    )
    (output / f"{manifest['protocol']}_uapi.h").write_text(uapi, encoding="utf-8")
    write_json(output / f"{manifest['protocol']}_metadata.json", {
        "uuid": manifest["uuid"], "revision": manifest["revision"], "features": manifest["features"],
        "scope": scope,
        "max_resources": max((max(
            sum(resource_bound(field_["type"], {item["name"]: item for item in manifest["structs"]})
                for field_ in item["request"]),
            sum(resource_bound(field_["type"], {item["name"]: item for item in manifest["structs"]})
                for field_ in item["response"]),
        ) for item in manifest["methods"]), default=0),
        "methods": [{"id": item["id"], "name": item["name"], "annotations": item["annotations"]} for item in manifest["methods"]],
    })
    return 0


def command_generate_index(args: argparse.Namespace) -> int:
    source_dir = pathlib.Path(args.input)
    manifests = [manifest_for(path) for path in sorted(source_dir.glob("*.naidl"))]
    protocols: list[dict[str, Any]] = []
    for manifest in manifests:
        protocol = manifest["protocol"]
        uapi_name = macro_name(manifest["annotations"].get("uapi_name", protocol))
        scope_name = macro_name(manifest["annotations"].get("scope_name", uapi_name))
        protocols.append({
            "name": protocol,
            "scope_macro": scope_name,
            "methods": [
                {
                    "name": (
                        macro_name(method.get("annotations", {}).get("uapi_name", method["name"]))
                        if "uapi_name" in method.get("annotations", {})
                        else f"{uapi_name}_{macro_name(method.get('annotations', {}).get('uapi_name', method['name']))}"
                    ),
                    "uapi_macro": uapi_name,
                    "method_macro": macro_name(method.get("annotations", {}).get("uapi_name", method["name"])),
                }
                for method in manifest["methods"]
            ],
        })
    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(render_template("index.h.htt", protocols=protocols), encoding="utf-8")
    return 0


def command_generate_system(args: argparse.Namespace) -> int:
    source_dir = pathlib.Path(args.input)
    output_dir = pathlib.Path(args.output)
    index = pathlib.Path(args.index)
    output_dir.mkdir(parents=True, exist_ok=True)
    for source in sorted(source_dir.glob("*.naidl")):
        status = command_test(argparse.Namespace(input=str(source)))
        if status != 0:
            return status
        command_generate(argparse.Namespace(input=str(source), output=str(output_dir)))
    command_generate_index(argparse.Namespace(input=str(source_dir), output=str(index)))
    if args.stamp is not None:
        pathlib.Path(args.stamp).parent.mkdir(parents=True, exist_ok=True)
        pathlib.Path(args.stamp).write_text("generated\n", encoding="utf-8")
    return 0


def command_check_system(args: argparse.Namespace) -> int:
    source_dir = pathlib.Path(args.input)
    baseline_dir = pathlib.Path(args.baseline)
    status = 0
    for source in sorted(source_dir.glob("*.naidl")):
        new = manifest_for(source)
        baseline = baseline_dir / f"{new['protocol']}.abi.json"
        if not baseline.exists():
            if not args.allow_missing:
                print(f"missing ABI baseline: {baseline}", file=sys.stderr)
                status = 1
            continue
        old = json.loads(baseline.read_text(encoding="utf-8"))
        errors = check_compatibility(old, new)
        for error in errors:
            print(f"{source.name}: incompatible: {error}", file=sys.stderr)
        if errors:
            status = 1
    return status


def command_check(args: argparse.Namespace) -> int:
    old = json.loads(pathlib.Path(args.old).read_text(encoding="utf-8"))
    new = manifest_for(pathlib.Path(args.new)) if args.new.endswith(".naidl") else json.loads(pathlib.Path(args.new).read_text(encoding="utf-8"))
    errors = check_compatibility(old, new)
    if errors:
        for error in errors:
            print(f"incompatible: {error}", file=sys.stderr)
        return 1
    return 0


def command_test(args: argparse.Namespace) -> int:
    manifest = manifest_for(pathlib.Path(args.input))
    first = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    second = json.dumps(manifest_for(pathlib.Path(args.input)), sort_keys=True, separators=(",", ":"))
    if first != second:
        print("non-deterministic manifest", file=sys.stderr)
        return 1
    if any(item["id"] <= 0 for item in manifest["methods"]):
        print("invalid method ID", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    generate = subparsers.add_parser("generate")
    generate.add_argument("input")
    generate.add_argument("output")
    generate.set_defaults(function=command_generate)
    generate_index = subparsers.add_parser("generate-index")
    generate_index.add_argument("input")
    generate_index.add_argument("output")
    generate_index.set_defaults(function=command_generate_index)
    generate_system = subparsers.add_parser("generate-system")
    generate_system.add_argument("input")
    generate_system.add_argument("output")
    generate_system.add_argument("index")
    generate_system.add_argument("stamp", nargs="?")
    generate_system.set_defaults(function=command_generate_system)
    check_system = subparsers.add_parser("check-system")
    check_system.add_argument("input")
    check_system.add_argument("baseline")
    check_system.add_argument("--allow-missing", action="store_true")
    check_system.set_defaults(function=command_check_system)
    check = subparsers.add_parser("check")
    check.add_argument("old")
    check.add_argument("new")
    check.set_defaults(function=command_check)
    test = subparsers.add_parser("test")
    test.add_argument("input")
    test.set_defaults(function=command_test)
    args = parser.parse_args()
    try:
        return args.function(args)
    except IdlError as error:
        print(f"naoidl: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
