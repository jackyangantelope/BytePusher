from __future__ import annotations

import os
import random
import tempfile

PAGE = 4096
VIRTUAL = 16 * 1024 * 1024
PAGES = VIRTUAL // PAGE
CACHE_PAGES = 64


class Pager:
    def __init__(self, backing):
        self.backing = backing
        self.cache: dict[int, bytearray] = {}
        self.dirty: set[int] = set()
        self.age: dict[int, int] = {}
        self.tick = 0

    def page(self, number: int) -> bytearray:
        self.tick += 1
        if number not in self.cache:
            if len(self.cache) == CACHE_PAGES:
                victim = min(self.cache, key=self.age.__getitem__)
                if victim in self.dirty:
                    self.backing.seek(victim * PAGE)
                    self.backing.write(self.cache[victim])
                    self.dirty.remove(victim)
                del self.cache[victim]
                del self.age[victim]
            self.backing.seek(number * PAGE)
            self.cache[number] = bytearray(self.backing.read(PAGE))
        self.age[number] = self.tick
        return self.cache[number]

    def read8(self, address: int) -> int:
        address &= VIRTUAL - 1
        return self.page(address // PAGE)[address % PAGE]

    def write8(self, address: int, value: int) -> None:
        address &= VIRTUAL - 1
        page = address // PAGE
        self.page(page)[address % PAGE] = value
        self.dirty.add(page)

    def flush(self) -> None:
        for page in list(self.dirty):
            self.backing.seek(page * PAGE)
            self.backing.write(self.cache[page])
        self.dirty.clear()
        self.backing.flush()


def main() -> None:
    random.seed(0x4250)
    with tempfile.TemporaryFile() as backing:
        backing.truncate(VIRTUAL)
        pager = Pager(backing)
        expected: dict[int, int] = {}
        for _ in range(20000):
            address = random.randrange(VIRTUAL)
            value = random.randrange(256)
            pager.write8(address, value)
            expected[address] = value
            if random.randrange(4) == 0:
                probe = random.choice(tuple(expected))
                assert pager.read8(probe) == expected[probe]
        pager.flush()
        for address, value in expected.items():
            backing.seek(address)
            assert backing.read(1) == bytes((value,))
    print(f"PASS virtual={VIRTUAL} page={PAGE} cache_pages={CACHE_PAGES} writes={len(expected)}")


if __name__ == "__main__":
    main()
