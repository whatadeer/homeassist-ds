"""
Generates placeholder icon.png, banner.png, and audio.wav so the project
builds out of the box. Replace these with real artwork whenever you like -
bannertool only cares that icon.png is 48x48 and banner.png is 256x128.

Re-run with: python gen_placeholder_assets.py
"""
import struct
import wave
import zlib


def write_png(path, width, height, rgb):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    row = b"\x00" + bytes(rgb) * width
    raw = row * height
    idat = zlib.compress(raw, 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def write_silent_wav(path, seconds=1, sample_rate=32728):
    nframes = seconds * sample_rate
    silence = struct.pack("<h", 0) * 2 * nframes
    with wave.open(path, "w") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(silence)


if __name__ == "__main__":
    write_png("icon.png", 48, 48, (0x03, 0xA9, 0xF4))
    write_png("banner.png", 256, 128, (0x0B, 0x1F, 0x33))
    write_silent_wav("audio.wav")
    print("Wrote icon.png, banner.png, audio.wav")
