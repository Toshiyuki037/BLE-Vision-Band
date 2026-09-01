from __future__ import annotations

import re
import sys
import time
from pathlib import Path

import serial


PORT = "COM4"
BAUD = 115200
READ_TIMEOUT_S = 0.20
TRANSFER_STALL_TIMEOUT_S = 15.0
POST_JPEG_CONSOLE_DRAIN_S = 2.0

BEGIN_PREFIX = b"JPEG_BINARY_BEGIN:"
MAX_SYNC_BUFFER = 8192
MAX_JPEG_SIZE = 16 * 1024 * 1024

OUTPUT_PATH = Path(__file__).resolve().parents[2] / "capture.jpg"


def print_clean_console_bytes(chunk: bytes, line_buffer: bytearray) -> None:
    """
    Before the JPEG marker arrives, print only clean ASCII console lines.
    Binary-looking data is discarded instead of dumping gibberish.
    """
    for b in chunk:
        if b in (10, 13):
            if line_buffer:
                try:
                    line = line_buffer.decode("ascii")
                except UnicodeDecodeError:
                    line = ""

                if line and all(ch.isprintable() or ch == "\t" for ch in line):
                    print(line)

                line_buffer.clear()

        elif 32 <= b <= 126 or b == 9:
            if len(line_buffer) < 300:
                line_buffer.append(b)

        else:
            line_buffer.clear()


def extract_begin_marker(buffer: bytearray):
    start = buffer.find(BEGIN_PREFIX)
    if start < 0:
        return None

    newline = buffer.find(b"\n", start)
    if newline < 0:
        return None

    marker = bytes(buffer[start:newline]).rstrip(b"\r")
    match = re.fullmatch(rb"JPEG_BINARY_BEGIN:(\d+)", marker)

    if not match:
        del buffer[: newline + 1]
        return None

    announced_length = int(match.group(1))
    remainder = bytes(buffer[newline + 1 :])

    return announced_length, remainder


def wait_for_begin_marker(ser: serial.Serial):
    sync = bytearray()
    line_buffer = bytearray()

    while True:
        chunk = ser.read(512)

        if not chunk:
            continue

        sync.extend(chunk)

        result = extract_begin_marker(sync)
        if result is not None:
            return result

        print_clean_console_bytes(chunk, line_buffer)

        if len(sync) > MAX_SYNC_BUFFER:
            keep = max(len(BEGIN_PREFIX) + 64, 128)
            del sync[:-keep]


def find_complete_jpeg(data: bytes):
    """
    Return (jpeg_bytes, soi_offset, eoi_offset) once a complete JPEG exists.

    The Arducam FIFO length may include bytes after JPEG EOI (FF D9).
    Those bytes are not part of the image and must not be required before
    declaring the transfer successful.
    """
    soi = data.find(b"\xFF\xD8")
    if soi < 0:
        return None

    eoi = data.find(b"\xFF\xD9", soi + 2)
    if eoi < 0:
        return None

    jpeg = data[soi : eoi + 2]
    return jpeg, soi, eoi


def receive_until_jpeg_complete(
    ser: serial.Serial,
    announced_length: int,
    initial_data: bytes,
):
    data = bytearray(initial_data)

    if len(data) > announced_length:
        data = data[:announced_length]

    last_data_time = time.monotonic()
    next_report = 10

    while True:
        complete = find_complete_jpeg(data)

        if complete is not None:
            jpeg, soi, eoi = complete
            return jpeg, len(data), soi, eoi

        if len(data) >= announced_length:
            raise ValueError(
                "Received the full announced FIFO payload, "
                "but no JPEG EOI marker FF D9 was found"
            )

        chunk = ser.read(min(4096, announced_length - len(data)))

        if chunk:
            data.extend(chunk)
            last_data_time = time.monotonic()

            percent = (len(data) * 100) // announced_length

            if percent >= next_report:
                print(
                    f"Receiving: {len(data)}/{announced_length} bytes "
                    f"({percent}%)"
                )

                while next_report <= percent:
                    next_report += 10

        else:
            if time.monotonic() - last_data_time > TRANSFER_STALL_TIMEOUT_S:
                # One final check before reporting a true failure.
                complete = find_complete_jpeg(data)

                if complete is not None:
                    jpeg, soi, eoi = complete
                    return jpeg, len(data), soi, eoi

                raise TimeoutError(
                    "JPEG transfer stalled before EOI marker: "
                    f"{len(data)}/{announced_length} bytes"
                )



def drain_post_jpeg_console(ser: serial.Serial) -> None:
    """
    Keep COM4 open briefly after FF D9 so firmware can finish its
    remaining FIFO/padding transmission and print its completion log.
    """
    deadline = time.monotonic() + POST_JPEG_CONSOLE_DRAIN_S
    line_buffer = bytearray()

    while time.monotonic() < deadline:
        chunk = ser.read(512)

        if not chunk:
            continue

        print_clean_console_bytes(chunk, line_buffer)


def main() -> int:
    print(f"Opening {PORT} at {BAUD} baud...")
    print("Make sure VS Code Serial Monitor or PuTTY is NOT using COM4.")
    print()

    try:
        with serial.Serial(
            PORT,
            BAUD,
            timeout=READ_TIMEOUT_S,
            write_timeout=2,
        ) as ser:

            ser.reset_input_buffer()

            print("Receiver armed.")
            print("Vision Band can stay running.")
            print("Power it ON with a long BUTTON1 press if needed.")
            print("Then SHORT PRESS BUTTON1 to capture.")
            print("Waiting for JPEG_BINARY_BEGIN marker...")

            announced_length, initial_data = wait_for_begin_marker(ser)

            if announced_length <= 0 or announced_length > MAX_JPEG_SIZE:
                raise ValueError(
                    f"Firmware announced invalid JPEG length: "
                    f"{announced_length}"
                )

            print(f"JPEG announced size: {announced_length} bytes")
            print("Receiving raw JPEG until FF D9...")

            jpeg, bytes_seen, soi, eoi = receive_until_jpeg_complete(
                ser,
                announced_length,
                initial_data,
            )

            OUTPUT_PATH.write_bytes(jpeg)

            print()
            print("JPEG COMPLETE.")
            print(f"Announced FIFO length: {announced_length} bytes")
            print(f"Bytes consumed before completion: {bytes_seen} bytes")
            print(f"FF D8 found at byte: {soi}")
            print(f"FF D9 found at byte: {eoi}")
            print(f"Saved JPEG length: {len(jpeg)} bytes")

            trailing_not_required = announced_length - (eoi + 2)

            if trailing_not_required > 0:
                print(
                    f"Ignored {trailing_not_required} announced bytes "
                    "after JPEG EOI/padding"
                )

            print()
            print("Image written to:")
            print(OUTPUT_PATH)
            print()
            print("Waiting briefly for firmware completion message...")
            drain_post_jpeg_console(ser)
            print()
            print(
                "For another image, run this receiver again and "
                "short press BUTTON1."
            )

            return 0

    except serial.SerialException as exc:
        print(f"Serial error: {exc}", file=sys.stderr)
        return 2

    except KeyboardInterrupt:
        print("\nStopped by user.")
        return 130

    except Exception as exc:
        print(f"Receiver error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
