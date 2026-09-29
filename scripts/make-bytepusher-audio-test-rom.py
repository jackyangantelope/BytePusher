"""Generate a deterministic BytePusher HDMI-audio acceptance ROM."""

from pathlib import Path
import argparse


TONE_960_HZ = (0, 18, 34, 44, 48, 44, 34, 18,
               0, -18, -34, -44, -48, -44, -34, -18)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--audio-bank", type=lambda s: int(s, 0), default=0x0300,
                        help="256-byte bank, 0x0300..0x03ff; use 0x0301 for low-byte regression")
    args = parser.parse_args()
    if not 0x0300 <= args.audio_bank <= 0x03ff:
        parser.error("audio bank must be 0x0300..0x03ff (outside code/video, inside ROM)")
    output = args.output
    image = bytearray(0x40000)
    image[2:5] = bytes((0x01, 0x00, 0x00))  # program counter: 0x010000
    image[5] = 0x02                         # video page: 0x020000
    image[6:8] = args.audio_bank.to_bytes(2, "big")

    # One byte-copy-jump instruction loops forever without changing VM state.
    image[0x10000:0x10009] = bytes(
        (0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00)
    )

    # A cyan/blue target confirms that the dedicated audio ROM was selected.
    for y in range(256):
        for x in range(256):
            border = x < 8 or x >= 248 or y < 8 or y >= 248
            ring = 52 * 52 <= (x - 128) ** 2 + (y - 128) ** 2 <= 78 * 78
            cross = abs(x - 128) < 5 or abs(y - 128) < 5
            image[0x20000 + y * 256 + x] = 231 if border or ring or cross else 26

    # 16 samples/cycle at BytePusher's 15.36 kHz source rate gives exactly
    # 960 Hz. Sixteen whole cycles fit in the 256-byte page, so the page loops
    # without a discontinuity. Peak output is 48 * 256 = 12,288 (37.5% FS).
    for index in range(256):
        image[(args.audio_bank << 8) + index] = TONE_960_HZ[index % len(TONE_960_HZ)] & 0xFF

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    print(f"wrote {len(image)} bytes: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
