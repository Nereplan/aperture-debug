#!/usr/bin/env python3
"""
Pure-Python lossless PNG decoder, encoder, comparator, and metadata utility for
Aperture visual regression verification.

Uses only the standard library (struct, zlib, hashlib, os) without external PIL/cv2 dependencies.
"""

import hashlib
import os
import struct
import zlib


def compute_file_sha256(file_path):
    """Computes SHA-256 hex digest of a file."""
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def read_png_dimensions(data_or_path):
    """Reads width and height from PNG IHDR chunk."""
    if isinstance(data_or_path, (str, os.PathLike)):
        with open(data_or_path, "rb") as f:
            header = f.read(32)
    else:
        header = data_or_path
    if len(header) < 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Invalid PNG signature")
    width, height = struct.unpack(">II", header[16:24])
    return width, height


def write_png(width, height, rgba_data, output_path):
    """Encodes raw RGBA (or BGRA converted to RGBA) bytes to a lossless PNG."""
    raw_scanlines = bytearray()
    row_bytes = width * 4
    for y in range(height):
        raw_scanlines.append(0)  # Filter None
        offset = y * row_bytes
        raw_scanlines.extend(rgba_data[offset:offset + row_bytes])

    compressed = zlib.compress(raw_scanlines, 9)

    def make_chunk(chunk_type, data):
        length = struct.pack(">I", len(data))
        crc = struct.pack(">I", zlib.crc32(chunk_type + data) & 0xFFFFFFFF)
        return length + chunk_type + data + crc

    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png_bytes = (
        b"\x89PNG\r\n\x1a\n"
        + make_chunk(b"IHDR", ihdr_data)
        + make_chunk(b"IDAT", compressed)
        + make_chunk(b"IEND", b"")
    )

    parent_dir = os.path.dirname(os.path.abspath(output_path))
    if parent_dir and not os.path.exists(parent_dir):
        os.makedirs(parent_dir, exist_ok=True)

    with open(output_path, "wb") as f:
        f.write(png_bytes)
    return hashlib.sha256(png_bytes).hexdigest()


def decode_png(file_path):
    """Decodes a PNG file into (width, height, rgba_bytearray). Supports all PNG filter types."""
    with open(file_path, "rb") as f:
        content = f.read()
    if len(content) < 8 or content[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"Invalid PNG header in {file_path}")

    offset = 8
    width, height, bit_depth, color_type = 0, 0, 0, 0
    idat_chunks = []
    while offset < len(content):
        length = struct.unpack(">I", content[offset:offset + 4])[0]
        chunk_type = content[offset + 4:offset + 8]
        data = content[offset + 8:offset + 8 + length]
        offset += 8 + length + 4  # skip chunk + CRC
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", data)
        elif chunk_type == b"IDAT":
            idat_chunks.append(data)
        elif chunk_type == b"IEND":
            break

    if width <= 0 or height <= 0:
        raise ValueError(f"Invalid dimensions {width}x{height} in {file_path}")

    decompressed = zlib.decompress(b"".join(idat_chunks))

    bpp = 4 if color_type == 6 else (3 if color_type == 2 else 1)
    stride = width * bpp
    raw_pixels = bytearray(width * height * 4)

    raw_offset = 0
    prev_row = bytearray(stride)
    curr_row = bytearray(stride)

    for y in range(height):
        filter_type = decompressed[raw_offset]
        raw_offset += 1
        scanline = decompressed[raw_offset:raw_offset + stride]
        raw_offset += stride

        for i in range(stride):
            val = scanline[i]
            left = curr_row[i - bpp] if i >= bpp else 0
            up = prev_row[i]
            upper_left = prev_row[i - bpp] if i >= bpp else 0

            if filter_type == 0:  # None
                recon = val
            elif filter_type == 1:  # Sub
                recon = (val + left) & 0xFF
            elif filter_type == 2:  # Up
                recon = (val + up) & 0xFF
            elif filter_type == 3:  # Average
                recon = (val + ((left + up) // 2)) & 0xFF
            elif filter_type == 4:  # Paeth
                p = left + up - upper_left
                pa = abs(p - left)
                pb = abs(p - up)
                pc = abs(p - upper_left)
                if pa <= pb and pa <= pc:
                    pr = left
                elif pb <= pc:
                    pr = up
                else:
                    pr = upper_left
                recon = (val + pr) & 0xFF
            else:
                recon = val
            curr_row[i] = recon

        dst_row_offset = y * width * 4
        if color_type == 6:  # RGBA
            raw_pixels[dst_row_offset:dst_row_offset + width * 4] = curr_row
        elif color_type == 2:  # RGB
            for x in range(width):
                raw_pixels[dst_row_offset + x * 4] = curr_row[x * 3]
                raw_pixels[dst_row_offset + x * 4 + 1] = curr_row[x * 3 + 1]
                raw_pixels[dst_row_offset + x * 4 + 2] = curr_row[x * 3 + 2]
                raw_pixels[dst_row_offset + x * 4 + 3] = 255
        prev_row[:] = curr_row

    return width, height, raw_pixels


def compare_images(
    baseline_path,
    actual_path,
    diff_output_path=None,
    tolerance_per_pixel=2,
    max_mismatch_pct=0.05,
    max_peak_delta=16,
):
    """
    Robust pixel-level image comparator.
    
    Computes per-pixel channel deltas, maximum color difference across R,G,B,A,
    total mismatched pixels above tolerance, percentage of mismatched pixels,
    and writes a diagnostic visual diff highlight image upon mismatch.
    """
    w1, h1, px1 = decode_png(baseline_path)
    w2, h2, px2 = decode_png(actual_path)
    if w1 != w2 or h1 != h2:
        return {
            "passed": False,
            "error": f"Dimension mismatch: baseline {w1}x{h1} vs actual {w2}x{h2}",
            "width": w2,
            "height": h2,
            "expected_width": w1,
            "expected_height": h1,
            "total_pixels": w1 * h1,
            "mismatched_pixels": w1 * h1,
            "mismatch_percent": 100.0,
            "peak_delta": 255,
        }

    total_pixels = w1 * h1
    mismatched_count = 0
    peak_delta = 0
    diff_pixels = bytearray(total_pixels * 4)

    for i in range(total_pixels):
        idx = i * 4
        r1, g1, b1, a1 = px1[idx], px1[idx + 1], px1[idx + 2], px1[idx + 3]
        r2, g2, b2, a2 = px2[idx], px2[idx + 1], px2[idx + 2], px2[idx + 3]

        dr = abs(r1 - r2)
        dg = abs(g1 - g2)
        db = abs(b1 - b2)
        da = abs(a1 - a2)
        delta = max(dr, dg, db, da)
        if delta > peak_delta:
            peak_delta = delta

        if delta > tolerance_per_pixel:
            mismatched_count += 1
            # Diff highlight in bright magenta/red (#FF0055)
            diff_pixels[idx] = 255
            diff_pixels[idx + 1] = 0
            diff_pixels[idx + 2] = 85
            diff_pixels[idx + 3] = 255
        else:
            # Grayscale dimmed baseline context
            gray = (r1 * 299 + g1 * 587 + b1 * 114) // 1000
            dim = gray // 3
            diff_pixels[idx] = dim
            diff_pixels[idx + 1] = dim
            diff_pixels[idx + 2] = dim
            diff_pixels[idx + 3] = 255

    mismatch_pct = (mismatched_count / total_pixels) * 100.0
    passed = (mismatch_pct <= max_mismatch_pct) and (peak_delta <= max_peak_delta)

    if diff_output_path and not passed:
        write_png(w1, h1, diff_pixels, diff_output_path)

    return {
        "passed": passed,
        "width": w1,
        "height": h1,
        "total_pixels": total_pixels,
        "mismatched_pixels": mismatched_count,
        "mismatch_percent": mismatch_pct,
        "peak_delta": peak_delta,
        "max_mismatch_pct_allowed": max_mismatch_pct,
        "max_peak_delta_allowed": max_peak_delta,
        "per_pixel_threshold": tolerance_per_pixel,
    }
