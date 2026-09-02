import asyncio
import struct
import time
from pathlib import Path

from bleak import BleakClient
from bleak import BleakScanner
from bleak.exc import BleakCharacteristicNotFoundError


TARGET_NAME = "Vision Band"

IMAGE_TX_UUID = "7f510003-1b15-4f0d-8f7b-4c8d4f3a1000"

IMAGE_START = 0x01
IMAGE_DATA = 0x02
IMAGE_END = 0x03

OUTPUT_PATH = Path("capture.jpg")

image_done = asyncio.Event()

image_buffer = bytearray()

expected_sequence = 0
announced_fifo_length = None
reported_jpeg_length = None
reported_packet_count = None

transfer_started_at = None
transfer_finished_at = None

protocol_error = None


def image_notification_handler(sender, data: bytearray):
    global expected_sequence
    global announced_fifo_length
    global reported_jpeg_length
    global reported_packet_count
    global transfer_started_at
    global transfer_finished_at
    global protocol_error

    packet = bytes(data)

    if not packet:
        protocol_error = "Received an empty image packet."
        image_done.set()
        return

    packet_type = packet[0]

    if packet_type == IMAGE_START:
        if len(packet) != 5:
            protocol_error = (
                f"Invalid IMAGE_START length: {len(packet)}"
            )
            image_done.set()
            return

        announced_fifo_length = struct.unpack_from(
            "<I",
            packet,
            1,
        )[0]

        image_buffer.clear()
        expected_sequence = 0

        transfer_started_at = time.perf_counter()

        print()
        print("IMAGE START")
        print(
            f"Arducam FIFO upper-bound: "
            f"{announced_fifo_length:,} bytes"
        )
        print("Receiving JPEG over BLE...")
        print("Receiver timeout guard: 15 minutes")
        print()

        return

    if packet_type == IMAGE_DATA:
        if len(packet) < 6:
            protocol_error = (
                f"Invalid IMAGE_DATA length: {len(packet)}"
            )
            image_done.set()
            return

        sequence = struct.unpack_from(
            "<I",
            packet,
            1,
        )[0]

        if sequence != expected_sequence:
            protocol_error = (
                "BLE image packet sequence mismatch: "
                f"expected {expected_sequence}, got {sequence}"
            )
            image_done.set()
            return

        image_buffer.extend(
            packet[5:]
        )

        expected_sequence += 1

        if expected_sequence % 1000 == 0:
            print(
                f"Packets: {expected_sequence:,} | "
                f"JPEG bytes: {len(image_buffer):,}"
            )

        return

    if packet_type == IMAGE_END:
        if len(packet) != 9:
            protocol_error = (
                f"Invalid IMAGE_END length: {len(packet)}"
            )
            image_done.set()
            return

        reported_jpeg_length = struct.unpack_from(
            "<I",
            packet,
            1,
        )[0]

        reported_packet_count = struct.unpack_from(
            "<I",
            packet,
            5,
        )[0]

        transfer_finished_at = time.perf_counter()

        image_done.set()
        return

    protocol_error = (
        f"Unknown image packet type: 0x{packet_type:02X}"
    )

    image_done.set()


async def main():
    global protocol_error

    print(
        f'Searching for "{TARGET_NAME}"...'
    )

    device = await BleakScanner.find_device_by_name(
        TARGET_NAME,
        timeout=10.0,
    )

    if device is None:
        print()
        print("Vision Band not found.")
        print(
            "Long-press BUTTON1 to power the Vision Band ON."
        )
        return

    print(
        f"Found: {device.name}"
    )

    print(
        f"Address: {device.address}"
    )

    print(
        "Connecting..."
    )

    try:
        async with BleakClient(device) as client:
            print()
            print("CONNECTED")
            print(
                f"Connected: {client.is_connected}"
            )
            print()

            print(
                "Subscribing to image notifications..."
            )

            await client.start_notify(
                IMAGE_TX_UUID,
                image_notification_handler,
            )

            print()
            print("==============================")
            print("BLE IMAGE RECEIVER READY - 6C.2 DIAGNOSTIC")
            print("==============================")
            print()
            print(
                "Short-press BUTTON1 on the Vision Band now."
            )
            print(
                "Keep this program running until the JPEG completes."
            )
            print()

            try:
                await asyncio.wait_for(
                    image_done.wait(),
                    timeout=900.0,
                )
            except asyncio.TimeoutError:
                print(
                    "FAIL: Timed out waiting for image transfer."
                )
                return

            if protocol_error is not None:
                print()
                print(
                    f"FAIL: {protocol_error}"
                )
                return

            if reported_jpeg_length is None:
                print(
                    "FAIL: IMAGE_END was not received."
                )
                return

            print()
            print("IMAGE END")
            print(
                f"Reported JPEG length: "
                f"{reported_jpeg_length:,} bytes"
            )
            print(
                f"Received JPEG length: "
                f"{len(image_buffer):,} bytes"
            )
            print(
                f"Reported data packets: "
                f"{reported_packet_count:,}"
            )

            if len(image_buffer) != reported_jpeg_length:
                print()
                print(
                    "FAIL: Received byte count does not match "
                    "firmware byte count."
                )
                return

            if reported_packet_count != expected_sequence:
                print()
                print(
                    "FAIL: Packet count does not match "
                    "sequence count."
                )
                return

            if len(image_buffer) < 4:
                print()
                print("FAIL: JPEG is too small.")
                return

            if image_buffer[0:2] != b"\xFF\xD8":
                print()
                print("FAIL: JPEG SOI FF D8 is missing.")
                return

            if image_buffer[-2:] != b"\xFF\xD9":
                print()
                print("FAIL: JPEG EOI FF D9 is missing.")
                return

            OUTPUT_PATH.write_bytes(
                image_buffer
            )

            duration = (
                transfer_finished_at -
                transfer_started_at
            )

            bits_per_second = (
                len(image_buffer) * 8 / duration
                if duration > 0
                else 0.0
            )

            print()
            print("JPEG MARKERS PASS")
            print("PACKET SEQUENCE PASS")
            print("BYTE COUNT PASS")
            print()
            print(
                f"Transfer time: {duration:.2f} s"
            )
            print(
                f"Effective JPEG throughput: "
                f"{bits_per_second / 1000:.1f} kbps"
            )
            print()
            print(
                f"Saved image: {OUTPUT_PATH.resolve()}"
            )
            print()
            print("==============================")
            print("PHASE 6C BLE IMAGE PASS")
            print("==============================")
            print()

            await asyncio.sleep(1)

            await client.stop_notify(
                IMAGE_TX_UUID
            )

    except BleakCharacteristicNotFoundError as exc:
        print()
        print(
            f"GATT characteristic not found: {exc}"
        )
        print()
        print(
            "Windows may still have the previous GATT database "
            "cached."
        )
        print(
            "Power-cycle the Vision Band, run a BLE discovery once, "
            "then retry this receiver."
        )

    except OSError as exc:
        print()
        print(
            f"Windows BLE operation failed: {exc}"
        )
        print()
        print(
            "The connection was established, but Windows canceled "
            "a GATT operation. Power-cycle the Vision Band and retry."
        )


if __name__ == "__main__":
    asyncio.run(main())
