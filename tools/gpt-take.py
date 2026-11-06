#!/usr/bin/env python3
"""Give a whole partition of LUN 0 to iOS: its GPT entry becomes "darwin" (APFS container type), nothing else changes.

usage: gpt-take.py DIR PARTITION

  DIR        gpt-head.bin and gpt-tail.bin: the first and the last 8 blocks of LUN 0 (/dev/block/sda), as
             ../hakios-xnu-kernel/tools/backup-partitions.py saves them (gpt-sda-head.bin, gpt-sda-tail.bin)
  PARTITION  the partition to take (this kit uses it for userdata)

The HakiOS loader and kernel read and write only a partition named "darwin" of the APFS container type, at least
20 GiB, on LUN 0 (xnuloader/loader.c, block_is_darwin). This renames PARTITION to darwin and gives it that type;
where it starts and ends, its unique GUID, its attributes and every other entry stay as they were. Both copies of
the table are made with their CRC-32s and checked again the way a firmware would (the checks of
tools/userdata-carve.py). DIR/gpt-head.new and DIR/gpt-tail.new are written; nothing is written to the phone.
"""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'carve', os.path.join(HERE, '..', 'hakios-xnu-kernel', 'tools', 'userdata-carve.py'))
carve = importlib.util.module_from_spec(spec)
spec.loader.exec_module(carve)
BS = carve.BS
MIN_BLOCKS = 5242875          # what the loader and the kernel accept (BLOCK_DARWIN_MIN, UFS_DARWIN_MIN)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    d, part = sys.argv[1], sys.argv[2]
    head = open(f'{d}/gpt-head.bin', 'rb').read()
    tail = open(f'{d}/gpt-tail.bin', 'rb').read()
    if len(head) != 8 * BS or len(tail) != 8 * BS:
        sys.exit('gpt-take.py: gpt-head.bin and gpt-tail.bin are to be 8 blocks each')
    disk = carve.header_of(tail[7 * BS:8 * BS])['my'] + 1
    ok, ph, bh, entries = carve.check('the table now', head, tail, disk)
    if not ok:
        sys.exit('gpt-take.py: the table does not check out; nothing made')
    used = [i for i, e in enumerate(entries) if e['type'].int != 0]
    if any(entries[i]['name'] == 'darwin' for i in used):
        sys.exit('gpt-take.py: there is a partition named darwin already')
    hit = [i for i in used if entries[i]['name'] == part]
    if len(hit) != 1:
        sys.exit(f'gpt-take.py: not exactly one partition named {part} on LUN 0')
    i = hit[0]
    blocks = entries[i]['last'] - entries[i]['first'] + 1
    if blocks < MIN_BLOCKS:
        sys.exit(f'gpt-take.py: {part} is {blocks * BS / 2**30:.2f} GiB; iOS needs at least {MIN_BLOCKS * BS / 2**30:.0f} GiB')

    praw, _, pat = carve.entries_of(head, ph, 0)
    es = ph['entry_size']
    table = bytearray(praw)
    e = bytearray(table[i * es:(i + 1) * es])
    e[:16] = carve.APFS_CONTAINER.bytes_le
    e[56:128] = bytes(72)
    e[56:56 + 12] = 'darwin'.encode('utf-16le')
    table[i * es:(i + 1) * es] = e
    table = bytes(table)

    new_head = bytearray(head)
    new_head[pat:pat + len(table)] = table
    new_head[BS:2 * BS] = carve.seal(head[BS:2 * BS], table)
    _, _, bat = carve.entries_of(tail, bh, disk - 8)
    new_tail = bytearray(tail)
    new_tail[bat:bat + len(table)] = table
    new_tail[7 * BS:8 * BS] = carve.seal(tail[7 * BS:8 * BS], table)
    new_head, new_tail = bytes(new_head), bytes(new_tail)

    ok, _, _, new_entries = carve.check('the new table', new_head, new_tail, disk)
    changed = [k for k in range(len(entries)) if entries[k] != new_entries[k]]
    n = new_entries[i]
    same = (n['first'], n['last'], n['guid'], n['attrs']) == \
        (entries[i]['first'], entries[i]['last'], entries[i]['guid'], entries[i]['attrs'])
    if not ok or changed != [i] or n['name'] != 'darwin' or n['type'] != carve.APFS_CONTAINER or not same:
        sys.exit('gpt-take.py: the new table is not what it should be; nothing made')
    print(f'{part} (sda{i + 1} under Android, lba {n["first"]}..{n["last"]}, {blocks * BS / 2**30:.2f} GiB) -> darwin, '
          f'type APFS container; nothing else differs')
    open(f'{d}/gpt-head.new', 'wb').write(new_head)
    open(f'{d}/gpt-tail.new', 'wb').write(new_tail)
    print(f'made {d}/gpt-head.new and {d}/gpt-tail.new')


if __name__ == '__main__':
    main()
