"""Thin ctypes wrapper over libopenmpt's public C API (test-only, #14).

Not a converter dependency — `xm2t00t.py` never imports this. It exists
solely so `test_xm2t00t.py` can cross-check xm_parser.py's structure
reading against an independent, battle-tested implementation, using
whatever `libopenmpt.so` is installed rather than vendoring a Python
binding. The public C API only exposes counts/names/order->pattern
mapping (not raw sample data or envelopes), which is exactly the subset
the "matches openmpt123 --info / libopenmpt" acceptance check needs.
"""

from __future__ import annotations

import ctypes
import ctypes.util
from typing import List, Optional


def _find_library() -> str:
    name = ctypes.util.find_library("openmpt")
    if name:
        return name
    # find_library can fail inside minimal/containerized environments even
    # when the .so is present; fall back to the versioned SONAME directly.
    return "libopenmpt.so.0"


_lib: Optional[ctypes.CDLL] = None


def _get_lib() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        lib = ctypes.CDLL(_find_library())
        lib.openmpt_module_create_from_memory.restype = ctypes.c_void_p
        lib.openmpt_module_create_from_memory.argtypes = [
            ctypes.c_char_p, ctypes.c_size_t,
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ]
        lib.openmpt_module_destroy.argtypes = [ctypes.c_void_p]
        for fn in ("openmpt_module_get_num_channels", "openmpt_module_get_num_orders",
                   "openmpt_module_get_num_patterns", "openmpt_module_get_num_instruments",
                   "openmpt_module_get_num_samples"):
            getattr(lib, fn).argtypes = [ctypes.c_void_p]
            getattr(lib, fn).restype = ctypes.c_int32
        lib.openmpt_module_get_order_pattern.argtypes = [ctypes.c_void_p, ctypes.c_int32]
        lib.openmpt_module_get_order_pattern.restype = ctypes.c_int32
        _lib = lib
    return _lib


def available() -> bool:
    try:
        _get_lib()
        return True
    except OSError:
        return False


class OpenmptModule:
    def __init__(self, data: bytes):
        self._lib = _get_lib()
        self._handle = self._lib.openmpt_module_create_from_memory(
            data, len(data), None, None, None)
        if not self._handle:
            raise RuntimeError("libopenmpt rejected this file")

    def close(self) -> None:
        if self._handle:
            self._lib.openmpt_module_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "OpenmptModule":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    @property
    def num_channels(self) -> int:
        return self._lib.openmpt_module_get_num_channels(self._handle)

    @property
    def num_orders(self) -> int:
        return self._lib.openmpt_module_get_num_orders(self._handle)

    @property
    def num_patterns(self) -> int:
        return self._lib.openmpt_module_get_num_patterns(self._handle)

    @property
    def num_instruments(self) -> int:
        return self._lib.openmpt_module_get_num_instruments(self._handle)

    @property
    def num_samples(self) -> int:
        return self._lib.openmpt_module_get_num_samples(self._handle)

    @property
    def order_list(self) -> List[int]:
        return [self._lib.openmpt_module_get_order_pattern(self._handle, i)
                for i in range(self.num_orders)]


def load_file(path: str) -> OpenmptModule:
    with open(path, "rb") as f:
        return OpenmptModule(f.read())
