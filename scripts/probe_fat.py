"""Read files from a bounded FAT16 test image without mounting a host volume."""

from pathlib import Path
import struct


def read_files(path: Path) -> dict[Path, bytes]:
    with path.open("rb") as image:
        image_bytes = path.stat().st_size

        def read_at(offset: int, size: int) -> bytes:
            if offset < 0 or size < 0 or offset + size > image_bytes:
                raise ValueError("FAT read is outside the test image")
            image.seek(offset)
            data = image.read(size)
            if len(data) != size:
                raise ValueError("short FAT image read")
            return data

        mbr = read_at(0, 512)
        if mbr[510:] != b"\x55\xaa":
            raise ValueError("test image has no MBR signature")
        partitions = [struct.unpack_from("<B3sB3sII", mbr, 446 + index * 16)
                      for index in range(4)]
        candidates = [entry for entry in partitions if entry[2] in (4, 6, 14)]
        if len(candidates) != 1:
            raise ValueError("test image must contain exactly one FAT16 partition")
        partition_offset = candidates[0][4] * 512
        partition_bytes = candidates[0][5] * 512
        if partition_offset < 512 or partition_offset + partition_bytes > image_bytes:
            raise ValueError("FAT16 partition is outside the test image")
        boot = read_at(partition_offset, 512)
        sector_bytes, cluster_sectors, reserved, copies, root_entries, sectors, media, fat_sectors = \
            struct.unpack_from("<HBHBHHBH", boot, 11)
        sectors = sectors or struct.unpack_from("<I", boot, 32)[0]
        if (boot[510:] != b"\x55\xaa" or sector_bytes != 512 or
                cluster_sectors not in (1, 2, 4, 8, 16, 32, 64) or
                not reserved or copies not in (1, 2) or not root_entries or not fat_sectors or
                sectors * sector_bytes > partition_bytes):
            raise ValueError("invalid FAT16 geometry")
        root_sectors = (root_entries * 32 + sector_bytes - 1) // sector_bytes
        data_sector = reserved + copies * fat_sectors + root_sectors
        clusters = (sectors - data_sector) // cluster_sectors
        if not 4085 <= clusters < 65525 or (clusters + 2) * 2 > fat_sectors * sector_bytes:
            raise ValueError("invalid FAT16 cluster count")
        fat_offset = partition_offset + reserved * sector_bytes
        fat = read_at(fat_offset, fat_sectors * sector_bytes)
        if fat[0] != media:
            raise ValueError("FAT media byte mismatch")
        for copy in range(1, copies):
            if read_at(fat_offset + copy * len(fat), len(fat)) != fat:
                raise ValueError("FAT copies disagree")
        cluster_bytes = cluster_sectors * sector_bytes
        data_offset = partition_offset + data_sector * sector_bytes
        owned = set()

        def chain(first: int) -> bytes:
            result = bytearray()
            cluster = first
            while cluster < 0xfff8:
                if cluster < 2 or cluster >= clusters + 2 or cluster in owned:
                    raise ValueError("invalid, cyclic or shared FAT cluster chain")
                owned.add(cluster)
                if len(result) + cluster_bytes > 64 * 1024 * 1024:
                    raise ValueError("FAT test file exceeds 64 MiB")
                result.extend(read_at(data_offset + (cluster - 2) * cluster_bytes, cluster_bytes))
                cluster = struct.unpack_from("<H", fat, cluster * 2)[0]
            return bytes(result)

        files = {}

        def directory(data: bytes, parent: Path, depth: int) -> None:
            if depth > 16:
                raise ValueError("FAT test directory nesting exceeds limit")
            long_entries = []
            for offset in range(0, len(data), 32):
                entry = data[offset:offset + 32]
                if len(entry) != 32 or entry[0] == 0:
                    break
                if entry[0] == 0xe5:
                    long_entries = []
                    continue
                if entry[11] == 15:
                    long_entries.append(entry)
                    continue
                short = entry[:11]
                name = short[:8].decode("ascii").rstrip()
                extension = short[8:].decode("ascii").rstrip()
                if extension:
                    name += "." + extension
                if long_entries:
                    checksum = 0
                    for value in short:
                        checksum = (((checksum & 1) << 7) | (checksum >> 1)) + value
                        checksum &= 255
                    if (len(long_entries) > 20 or long_entries[0][0] != (len(long_entries) | 64) or
                            any((item[0] & 31) != len(long_entries) - index or item[13] != checksum
                                for index, item in enumerate(long_entries))):
                        raise ValueError("invalid FAT long filename")
                    encoded = b"".join(item[1:11] + item[14:26] + item[28:32]
                                       for item in reversed(long_entries))
                    name = encoded.decode("utf-16-le").split("\0", 1)[0].rstrip("\uffff")
                long_entries = []
                if entry[11] & 8 or name in (".", ".."):
                    continue
                if not name or "/" in name or "\\" in name:
                    raise ValueError("invalid FAT pathname")
                relative = parent / name
                first = struct.unpack_from("<H", entry, 26)[0]
                size = struct.unpack_from("<I", entry, 28)[0]
                payload = chain(first) if first else b""
                if entry[11] & 16:
                    if not first:
                        raise ValueError("FAT subdirectory has no cluster")
                    directory(payload, relative, depth + 1)
                else:
                    if size > len(payload) or relative in files:
                        raise ValueError("invalid FAT file size or duplicate path")
                    files[relative] = payload[:size]

        directory(read_at(fat_offset + copies * len(fat), root_entries * 32), Path(), 0)
        return files
