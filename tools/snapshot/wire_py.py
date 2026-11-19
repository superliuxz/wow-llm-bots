"""Python stdlib ctypes wrapper for out/common/libwlb_wire_py.so (BEVE <-> JSON)."""

import ctypes
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SO_PATH = ROOT / "out" / "common" / "libwlb_wire_py.so"

_lib = None


def _load():
    global _lib
    if _lib is not None:
        return _lib
    if not SO_PATH.exists():
        return None
    lib = ctypes.CDLL(str(SO_PATH))
    lib.wlb_module_message_beve_to_json.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.wlb_module_message_beve_to_json.restype = ctypes.c_void_p
    lib.wlb_command_beve_to_json.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.wlb_command_beve_to_json.restype = ctypes.c_void_p
    lib.wlb_command_json_to_beve.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.wlb_command_json_to_beve.restype = ctypes.c_void_p
    lib.wlb_module_message_visibility_errors.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
    ]
    lib.wlb_module_message_visibility_errors.restype = ctypes.c_void_p
    lib.wlb_wire_free.argtypes = [ctypes.c_void_p]
    lib.wlb_wire_free.restype = None
    _lib = lib
    return lib


def module_message_to_json(payload: bytes, pretty: bool = False):
    lib = _load()
    if lib is None:
        return None
    out_len = ctypes.c_size_t(0)
    ptr = lib.wlb_module_message_beve_to_json(payload, len(payload), 1 if pretty else 0,
                                              ctypes.byref(out_len))
    if not ptr:
        return None
    try:
        return ctypes.string_at(ptr, out_len.value).decode("utf-8")
    finally:
        lib.wlb_wire_free(ptr)


def decode_module_message(payload: bytes):
    text = module_message_to_json(payload, pretty=False)
    return json.loads(text) if text is not None else None


def decode_command(payload: bytes):
    lib = _load()
    if lib is None:
        return None
    out_len = ctypes.c_size_t(0)
    ptr = lib.wlb_command_beve_to_json(payload, len(payload), ctypes.byref(out_len))
    if not ptr:
        return None
    try:
        return json.loads(ctypes.string_at(ptr, out_len.value).decode("utf-8"))
    finally:
        lib.wlb_wire_free(ptr)


def encode_command(command_dict: dict) -> bytes:
    lib = _load()
    if lib is None:
        raise RuntimeError(f"missing {SO_PATH}: run `tools/wlb build common --no-tests`")
    raw = json.dumps(command_dict).encode("utf-8")
    out_len = ctypes.c_size_t(0)
    ptr = lib.wlb_command_json_to_beve(raw, len(raw), ctypes.byref(out_len))
    if not ptr:
        raise ValueError(f"invalid Command dict: {command_dict!r}")
    try:
        return ctypes.string_at(ptr, out_len.value)
    finally:
        lib.wlb_wire_free(ptr)


def visibility_errors(payload: bytes):
    """The snapshot's visibility errors (ValidateSnapshotVisibility), or None if the frame
    isn't a ModuleMessage."""
    lib = _load()
    if lib is None:
        return None
    out_len = ctypes.c_size_t(0)
    ptr = lib.wlb_module_message_visibility_errors(payload, len(payload),
                                                   ctypes.byref(out_len))
    if not ptr:
        return None
    try:
        text = ctypes.string_at(ptr, out_len.value).decode("utf-8")
    finally:
        lib.wlb_wire_free(ptr)
    return [line for line in text.split("\n") if line]
