from __future__ import annotations

import random

VIRTUAL = 16 * 1024 * 1024
ENTRIES = 4096
MASK = ENTRIES - 1
VALID = 0x8000


class ValueCache:
    def __init__(self, memory: bytearray):
        self.memory = memory
        self.tags = [0] * ENTRIES
        self.values = bytearray(ENTRIES)
        self.hits = 0
        self.misses = 0
        self.skips = 0
        self.commits = 0

    @staticmethod
    def normalize(address: int) -> int:
        return address & (VIRTUAL - 1)

    def read8(self, address: int) -> int:
        address = self.normalize(address)
        index = address & MASK
        tag = VALID | (address >> 12)
        if self.tags[index] == tag:
            self.hits += 1
            return self.values[index]
        value = self.memory[address]
        self.tags[index] = tag
        self.values[index] = value
        self.misses += 1
        return value

    def write8(self, address: int, value: int) -> bool:
        address = self.normalize(address)
        index = address & MASK
        tag = VALID | (address >> 12)
        value &= 0xFF
        if self.tags[index] == tag and self.values[index] == value:
            self.skips += 1
            return False
        self.memory[address] = value
        self.tags[index] = tag
        self.values[index] = value
        self.commits += 1
        return True


def main() -> None:
    random.seed(0x42505235)
    actual = bytearray(VIRTUAL)
    expected = bytearray(VIRTUAL)
    cache = ValueCache(actual)
    touched: set[int] = set()

    conflict_offsets = (0, 1, 2, 17, 255, 1023, 2048, 4095)
    for operation in range(100_000):
        if operation % 3 == 0:
            page = random.randrange(4096)
            address = (page << 12) | random.choice(conflict_offsets)
        else:
            address = random.randrange(VIRTUAL)
        address &= VIRTUAL - 1
        touched.add(address)

        if random.randrange(5) < 3:
            assert cache.read8(address) == expected[address]
        else:
            value = expected[address] if random.randrange(4) == 0 else random.randrange(256)
            committed = cache.write8(address, value)
            if committed:
                expected[address] = value
            assert actual[address] == expected[address]
            assert cache.read8(address) == expected[address]

    for address in touched:
        assert cache.read8(address) == expected[address]
        assert actual[address] == expected[address]

    probe = VIRTUAL - 1
    unchanged = cache.read8(probe)
    assert not cache.write8(probe, unchanged)
    assert cache.skips > 0
    print(
        f"PASS entries={ENTRIES} operations=100000 touched={len(touched)} "
        f"hits={cache.hits} misses={cache.misses} skips={cache.skips} commits={cache.commits}"
    )


if __name__ == "__main__":
    main()