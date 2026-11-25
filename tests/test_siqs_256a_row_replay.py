"""Manually verify a frozen V3 snapshot and the replay consumer's failure paths."""

import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile


MAX_FILE_BYTES = 4 * 1024 * 1024
REPLAY_RECORD = re.compile(
    r"GNFS_SIQS_256A_ROW_REPLAY_V1 backend=(dense_only|sparse_only) "
    r"rows=(\d+) columns=(\d+) verified=(\d+) factorizations=(\d+) "
    r"factor=(\d+) cofactor=(\d+)"
)
HEADER_ERROR = "V3 row snapshot header or factor base mismatch"
IDENTITY_ERROR = "invalid V3 row encoding or arithmetic identity"
CORPUS_ERROR = "selected V3 rows differ from the frozen corpus"


def replay(binary, path):
    return subprocess.run(
        [str(binary), "--replay-rows", str(path)],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )


def check_valid_output(result, modulus, rows, columns):
    records = result.stdout.splitlines()
    if result.returncode or len(records) != 2:
        raise RuntimeError(f"valid replay failed: {result.stdout!r} {result.stderr!r}")
    for expected_backend, record in zip(("dense_only", "sparse_only"), records):
        match = REPLAY_RECORD.fullmatch(record)
        if not match or match[1] != expected_backend:
            raise RuntimeError(f"unexpected replay record: {record!r}")
        actual_rows, actual_cols, verified, found, p, q = map(int, match.groups()[1:])
        if ((actual_rows, actual_cols) != (rows, columns) or verified != 64
                or not 0 < found <= verified or not 1 < p <= q or p * q != modulus):
            raise RuntimeError(f"inconsistent replay factors or dimensions: {record!r}")


def flip(data, offset):
    return data[:offset] + bytes([data[offset] ^ 1]) + data[offset + 1:]


def mutations(data, text_end, rows, columns):
    # Four text lines precede ten 64-bit identity words, then trim/rows/columns.
    table = text_end + 10 * 8 + 3 * 4
    first = table + columns * 4
    frames = []
    cursor = first
    for _ in range(rows):
        payload_size, = struct.unpack_from("<I", data, cursor + 1)
        frames.append(cursor)
        cursor += 5 + payload_size
    if cursor != len(data) or len(frames) < 3:
        raise ValueError("unexpected V3 selected row framing")

    profile = data.index(b"\n") + 1
    yield "wrong fixture", flip(data, profile), HEADER_ERROR
    yield "wrong source fingerprint", flip(data, text_end + 4 * 8), HEADER_ERROR
    swapped_primes = data[table + 8:table + 12] + data[table + 4:table + 8]
    yield "reordered factor base", data[:table + 4] + swapped_primes + data[table + 12:], HEADER_ERROR

    yield "unknown origin", data[:first] + b"\xff" + data[first + 1:], "invalid V3 row origin"
    yield "changed valid origin", flip(data, first), CORPUS_ERROR
    # Canonical row wire stores the strictly increasing source IDs last. Changing
    # the last ID preserves both canonical order and the GMP row identity.
    last_id_offset = frames[1] - 8
    last_id, = struct.unpack_from("<Q", data, last_id_offset)
    new_id = struct.pack("<Q", last_id + 1)
    yield "different valid source ID", data[:last_id_offset] + new_id + data[frames[1]:], CORPUS_ERROR
    first_two_swapped = data[frames[1]:frames[2]] + data[first:frames[1]]
    yield "reordered rows", data[:first] + first_two_swapped + data[frames[2]:], CORPUS_ERROR
    wire = first + 5
    if data[wire:wire + 8] != b"GNFSQROW":
        raise ValueError("V3 row does not start with a canonical row frame")
    yield "wrong row wire magic", flip(data, wire), IDENTITY_ERROR
    # Changing only q_negative preserves the row wire shape and flips the congruence sign.
    yield "wrong row sign", flip(data, wire + 20), IDENTITY_ERROR
    yield "truncated row prefix", data[:frames[-1] + 4], "truncated V3 row frame"
    yield "truncated row payload", data[:-1], "truncated V3 row payload"
    yield "trailing byte", data + b"\x00", "trailing data after V3 selected rows"
    yield "oversized file", data.ljust(MAX_FILE_BYTES + 1, b"\x00"), "V3 row snapshot exceeds byte limit"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="Release test_siqs_256a_scale_profile")
    parser.add_argument("snapshot", type=Path, help="snapshot captured by the 256-A profile")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    with args.snapshot.open("rb") as source:
        data = source.read(MAX_FILE_BYTES + 1)
    if len(data) > MAX_FILE_BYTES:
        raise ValueError("positive control snapshot exceeds the V3 size limit")

    with tempfile.TemporaryDirectory(prefix="siqs-256a-replay-") as directory:
        case = Path(directory) / "rows.bin"
        case.write_bytes(data)
        positive = replay(binary, case)
        if positive.returncode:
            raise RuntimeError(f"positive control rejected: {positive.stderr!r}")
        magic, _, modulus, _, payload = data.split(b"\n", 4)
        if magic != b"GNFS-SIQS-SELECTED-ROWS-V1":
            raise ValueError("unexpected snapshot version")
        text_end = len(data) - len(payload)
        _, rows, columns = struct.unpack_from("<III", payload, 10 * 8)
        check_valid_output(positive, int(modulus), rows, columns)
        print("PASS dense and sparse factor recovery")

        for name, corrupted, reason in mutations(data, text_end, rows, columns):
            case.write_bytes(corrupted)
            result = replay(binary, case)
            expected = f"GNFS_SIQS_256A_ERROR_V3 status=invariant_failure {reason}"
            if result.returncode != 1 or result.stdout or result.stderr.splitlines() != [expected]:
                raise RuntimeError(
                    f"{name}: exit={result.returncode} stdout={result.stdout!r} stderr={result.stderr!r}"
                )
            print(f"PASS {name}")


if __name__ == "__main__":
    main()
