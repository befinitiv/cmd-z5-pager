"""ctypes binding of lib/textinput (through tihost.c) for the Python tools."""

import ctypes
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
LIB_DIR = os.path.join(HERE, "..", "..", "lib", "textinput")
SO = os.path.join(HERE, "build", "libtihost.so")


def build_lib():
    srcs = [os.path.join(LIB_DIR, "src", f) for f in ("model.c", "user.c", "predict.c")]
    srcs.append(os.path.join(HERE, "tihost.c"))
    deps = srcs + [os.path.join(LIB_DIR, "include", "textinput", f)
                   for f in ("model.h", "user.h", "predict.h")]
    if os.path.exists(SO) and all(os.path.getmtime(SO) > os.path.getmtime(d) for d in deps):
        return
    os.makedirs(os.path.dirname(SO), exist_ok=True)
    subprocess.check_call(["gcc", "-O2", "-g", "-Wall", "-shared", "-fPIC",
                           "-I", os.path.join(LIB_DIR, "include"), "-o", SO] + srcs + ["-lm"])


class Model:
    def __init__(self, path, with_user=False):
        build_lib()
        self.lib = lib = ctypes.CDLL(SO)
        lib.tih_open.restype = ctypes.c_void_p
        for name in ("tih_vocab", "tih_find", "tih_text", "tih_keys", "tih_predict",
                     "tih_unigram", "tih_learn", "tih_set_penalty", "tih_range"):
            getattr(lib, name).argtypes = None
        self.h = lib.tih_open(path.encode(), int(with_user))
        if not self.h:
            raise RuntimeError(f"cannot open model {path}")
        self.h = ctypes.c_void_p(self.h)
        self.V = lib.tih_vocab(self.h)
        self.BOS = self.V

    def find(self, word):
        return self.lib.tih_find(self.h, word.encode())

    def text(self, wid):
        buf = ctypes.create_string_buffer(64)
        self.lib.tih_text(self.h, wid, buf, 64)
        return buf.value.decode()

    def keys(self, wid):
        buf = ctypes.create_string_buffer(64)
        self.lib.tih_keys(self.h, wid, buf)
        return buf.value.decode()

    def predict(self, p2, p1, keys, n=8):
        ids = (ctypes.c_int * n)()
        exact = (ctypes.c_int * n)()
        costs = (ctypes.c_int * n)()
        k = self.lib.tih_predict(self.h, p2, p1, keys.encode(), ids, exact, costs, n)
        return [(ids[i], bool(exact[i]), costs[i]) for i in range(k)]

    def unigram(self, wid):
        return self.lib.tih_unigram(self.h, wid)

    def range(self, keys):
        lo, ex, hi = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
        self.lib.tih_range(self.h, keys.encode(), ctypes.byref(lo), ctypes.byref(ex),
                           ctypes.byref(hi))
        return lo.value, ex.value, hi.value

    def learn(self, prev, word):
        return self.lib.tih_learn(self.h, prev, word.encode())

    def set_penalty(self, bits):
        self.lib.tih_set_penalty(self.h, int(bits * 64))
