"""Protect local provisioning data with Windows DPAPI for the current user."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path


class Blob(ctypes.Structure):
    _fields_ = [('size', wintypes.DWORD), ('data', ctypes.POINTER(ctypes.c_ubyte))]


def transform(data: bytes, protect: bool) -> bytes:
    memory = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
    source = Blob(len(data), memory)
    output = Blob()
    crypt = ctypes.WinDLL('crypt32', use_last_error=True)
    function = crypt.CryptProtectData if protect else crypt.CryptUnprotectData
    function.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p,
                         ctypes.POINTER(Blob), ctypes.c_void_p,
                         ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(Blob)]
    function.restype = wintypes.BOOL
    local_free = ctypes.WinDLL('kernel32', use_last_error=True).LocalFree
    local_free.argtypes = [ctypes.c_void_p]
    local_free.restype = ctypes.c_void_p
    # CRYPTPROTECT_UI_FORBIDDEN prevents any unexpected Windows prompt.
    if not function(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(output)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return ctypes.string_at(output.data, output.size)
    finally:
        local_free(output.data)


def save(path: Path, config: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(transform(json.dumps(config, ensure_ascii=False).encode('utf-8'), True))


def load(path: Path) -> dict:
    return json.loads(transform(path.read_bytes(), False))
