"""Compare copied real thumbnails with WIC output and optional WebP.

Requires Pillow (with WebP) and numpy. Input is read-only; outputs are reports
and a small HTML comparison, never converted files in the input directory.
"""
import argparse
import base64
import csv
import io
import json
import statistics
import struct
import time
from pathlib import Path

import numpy as np
from PIL import Image, features


def decode(data):
    magic, version, w, h, stride, timestamp, size = struct.unpack('<IIIIIdI', data[:32])
    assert magic == 0x5448554D and len(data) == 32 + size
    if version == 1:
        return Image.frombytes('RGBA', (w, h), data[32:], 'raw', 'BGRA').convert('RGB')
    return Image.open(io.BytesIO(data[32:])).convert('RGB')


def box_mean(a, size=11):
    integral = np.pad(a, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    return (integral[size:, size:] - integral[:-size, size:] - integral[size:, :-size] + integral[:-size, :-size]) / (size * size)


def ssim(a, b):
    # Local 11x11 uniform-window luminance SSIM. Kept explicit so the report's
    # metric is reproducible; it is not a claim of perceptual indistinguishability.
    weights = np.array([0.299, 0.587, 0.114])
    x = np.asarray(a, dtype=np.float64) @ weights
    y = np.asarray(b, dtype=np.float64) @ weights
    mx, my = box_mean(x), box_mean(y)
    vx, vy = box_mean(x*x) - mx*mx, box_mean(y*y) - my*my
    cov = box_mean(x*y) - mx*my
    return float(np.mean(((2*mx*my+6.5025)*(2*cov+58.5225)) / ((mx*mx+my*my+6.5025)*(vx+vy+58.5225))))


def psnr(a, b):
    difference = np.asarray(a, dtype=np.float64) - np.asarray(b, dtype=np.float64)
    mse = float(np.mean(difference*difference))
    return 99.0 if mse == 0 else float(10*np.log10(255*255/mse))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('candidate', type=Path)
    args = parser.parse_args()
    assert features.check('webp'), 'Pillow needs WebP support for the comparison'
    assert args.source.resolve() != args.candidate.resolve()
    rows = []
    previews = []
    for path in sorted(args.source.glob('*.bin')):
        original = path.read_bytes()
        start = time.perf_counter()
        image = decode(original)
        png_decode_ms = (time.perf_counter() - start) * 1000
        jpeg = (args.candidate / 'cache' / 'thumbnails' / path.name).read_bytes()
        start = time.perf_counter()
        jpeg_image = decode(jpeg)
        jpeg_decode_ms = (time.perf_counter() - start) * 1000
        row = {'file': path.name, 'png_bytes': len(original), 'wic_jpeg_bytes': len(jpeg),
               'wic_jpeg_ssim': ssim(image, jpeg_image), 'wic_jpeg_psnr_rgb': psnr(image, jpeg_image),
               'pillow_png_decode_ms': png_decode_ms, 'pillow_jpeg_decode_ms': jpeg_decode_ms}
        start = time.perf_counter()
        image.save(io.BytesIO(), format='JPEG', quality=80, subsampling=0)
        row['pillow_jpeg80_444_encode_ms'] = (time.perf_counter() - start) * 1000
        for quality in (75, 80, 85):
            buffer = io.BytesIO()
            start = time.perf_counter()
            image.save(buffer, format='WEBP', quality=quality, method=4)
            row[f'webp{quality}_encode_ms'] = (time.perf_counter() - start) * 1000
            encoded = buffer.getvalue()
            start = time.perf_counter()
            restored = Image.open(io.BytesIO(encoded)).convert('RGB')
            row[f'webp{quality}_decode_ms'] = (time.perf_counter() - start) * 1000
            row[f'webp{quality}_bytes'] = len(encoded) + 32
            row[f'webp{quality}_ssim'] = ssim(image, restored)
            row[f'webp{quality}_psnr_rgb'] = psnr(image, restored)
        rows.append(row)
        preview_original, preview_candidate = io.BytesIO(), io.BytesIO()
        image.save(preview_original, format='PNG')
        jpeg_image.save(preview_candidate, format='PNG')
        previews.append((row['wic_jpeg_ssim'], path.name, preview_original.getvalue(), preview_candidate.getvalue()))
    assert rows, 'No corpus images'
    with (args.candidate / 'formats.csv').open('w', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    summary = {'count': len(rows), 'metric': 'Local 11x11 uniform-window luminance SSIM and RGB PSNR',
               'webp_encoder': features.version('webp'), 'webp_method': 4}
    for field in list(rows[0])[1:]:
        values = sorted(row[field] for row in rows)
        summary[field] = {'total': sum(values), 'median': statistics.median(values),
                          'min': min(values), 'p05': values[int((len(values)-1)*.05)],
                          'p95': values[int((len(values)-1)*.95)]}
    (args.candidate / 'formats-summary.json').write_text(json.dumps(summary, indent=2))
    # Worst measured cases plus evenly distributed samples. Inline only; no
    # upload, CDN, original media path, or extra thumbnail cache is involved.
    previews.sort()
    samples = previews[:6] + [previews[int((len(previews)-1)*p)] for p in (.25,.5,.75,.9,.99)]
    html = ['<!doctype html><meta charset="utf-8"><title>Aperture thumbnail comparison</title>',
            '<style>body{background:#17191c;color:#eee;font:16px system-ui;padding:24px}section{margin:32px 0}img{vertical-align:top;margin-right:16px}p{color:#ccc}</style>',
            '<h1>Original PNG · Production JPEG</h1><p>Same dimensions. First six pairs are the lowest luminance SSIM results, followed by representative samples. Images stay local.</p>']
    for score, name, png, jpeg in samples:
        html += [f'<section><p>{name} · SSIM {score:.4f}</p>',
                 '<img alt="Original PNG" src="data:image/png;base64,' + base64.b64encode(png).decode() + '">',
                 '<img alt="Production JPEG decoded" src="data:image/png;base64,' + base64.b64encode(jpeg).decode() + '"></section>']
    (args.candidate / 'comparison.html').write_text('\n'.join(html), encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
