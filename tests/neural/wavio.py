#!/usr/bin/env python3
"""Minimal WAV reader (PCM 16/24/32, float32, any channel count → mono float64) for the neural cross-check."""
import struct
import numpy as np

def load_wav(path):
    with open(path, 'rb') as f:
        h = f.read(12)
        if h[:4] != b'RIFF' or h[8:12] != b'WAVE': return None, 0
        fmt = ch = bits = 0; sr = 0
        while True:
            c = f.read(8)
            if len(c) < 8: return None, 0
            cid, sz = c[:4], struct.unpack('<I', c[4:])[0]
            if cid == b'fmt ':
                b = f.read(sz)
                fmt, ch, sr = struct.unpack('<HHI', b[:8]); bits = struct.unpack('<H', b[14:16])[0]
                if fmt == 0xFFFE and sz >= 26: fmt = struct.unpack('<H', b[24:26])[0]
            elif cid == b'data':
                b = f.read(sz)
                bps = bits // 8
                n = len(b) // (bps * ch)
                b = b[:n * bps * ch]
                if fmt == 3 and bits == 32: x = np.frombuffer(b, dtype='<f4').astype(np.float64)
                elif bits == 16: x = np.frombuffer(b, dtype='<i2').astype(np.float64) / 32768.0
                elif bits == 24:
                    a = np.frombuffer(b, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
                    v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
                    v = np.where(v >= 1 << 23, v - (1 << 24), v)
                    x = v.astype(np.float64) / 8388608.0
                elif bits == 32: x = np.frombuffer(b, dtype='<i4').astype(np.float64) / 2147483648.0
                else: return None, 0
                if ch > 1: x = x.reshape(-1, ch).mean(axis=1)
                return x, sr
            else:
                f.seek((sz + 1) & ~1, 1)

