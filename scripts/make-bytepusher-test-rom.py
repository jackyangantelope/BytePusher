"""Generate a redistributable BytePusher video/VM smoke-test program."""

from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: make-bytepusher-test-rom.py OUTPUT.BytePusher")
        return 2

    output = Path(sys.argv[1])
    image = bytearray(0x30000)
    image[2:5] = bytes((0x01, 0x00, 0x00))  # program counter: 0x010000
    image[5] = 0x02                         # video page: 0x020000
    image[6:8] = bytes((0x03, 0x00))        # silent audio page: 0x030000

    # One byte-copy-jump instruction which loops at 0x010000.
    image[0x10000:0x10009] = bytes(
        (0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00)
    )

    # Web-safe palette indices: gradients plus a high-contrast checker.
    for y in range(256):
        for x in range(256):
            red = (x >> 5) % 6
            green = (y >> 5) % 6
            blue = ((x ^ y) >> 6) % 6
            color = 16 + red * 36 + green * 6 + blue
            if ((x >> 4) ^ (y >> 4)) & 1:
                color = 231 - (color - 16)
            image[0x20000 + y * 256 + x] = color

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    print(f"wrote {len(image)} bytes: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
