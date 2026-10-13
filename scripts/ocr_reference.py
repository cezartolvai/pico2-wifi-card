#!/usr/bin/env python3
"""OCR the requirements reference image and print what it actually says.

The requirements reference for this product is openrouter-banana2-rpi-pico.png.
The rules say that image defines what the adapter must do, so the transcription
in .clinerules/00-requirements-baseline.md must be checkable against the image
itself instead of trusted. This script is how that check is made: it upscales
the image, runs tesseract over it in two page-segmentation modes, and prints
both readings so a human can tell an OCR artefact from a real label.

Usage:  python3 scripts/ocr_reference.py [image.png]
Needs:  tesseract on PATH, Pillow.
"""

import subprocess
import sys

from PIL import Image, ImageFilter, ImageOps


def prepare(gray, factor):
    """Upscale and contrast-stretch the grayscale image; return the PNG path."""
    up = gray.resize((gray.width * factor, gray.height * factor), Image.LANCZOS)
    up = ImageOps.autocontrast(up)
    up = up.filter(ImageFilter.UnsharpMask(radius=2, percent=120))
    path = "/tmp/ocr_ref_x%d.png" % factor
    up.save(path)
    return path


def ocr(png_path, out_base, psm):
    """Run tesseract and return the recognised text."""
    subprocess.run(
        ["tesseract", png_path, out_base, "--psm", str(psm)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    with open(out_base + ".txt", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "openrouter-banana2-rpi-pico.png"
    img = Image.open(src)
    print("source %s %dx%d" % (src, img.width, img.height))

    for factor in (2, 3):
        png = prepare(img.convert("L"), factor)
        for psm in (11, 6):
            base = "/tmp/ocr_ref_x%d_psm%d" % (factor, psm)
            text = ocr(png, base, psm)
            print("\n===== upscale x%d psm %d =====" % (factor, psm))
            print(text.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
