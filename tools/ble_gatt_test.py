import asyncio

from bleak import BleakClient
from bleak import BleakScanner


TARGET_NAME = "Vision Band"

TX_UUID = "7f510001-1b15-4f0d-8f7b-4c8d4f3a1000"
RX_UUID = "7f510002-1b15-4f0d-8f7b-4c8d4f3a1000"


hello_event = asyncio.Event()
pong_event = asyncio.Event()


def notification_handler(sender, data: bytearray):
    message = bytes(data).decode(
        "utf-8",
        errors="replace",
    )

    print(f"Vision Band -> PC: {message}")

    if message == "HELLO_VISION_BAND":
        hello_event.set()

    if message == "PONG":
        pong_event.set()


async def main():
    print(f'Searching for "{TARGET_NAME}"...')

    device = await BleakScanner.find_device_by_name(
        TARGET_NAME,
        timeout=10.0,
    )

    if device is None:
        print()
        print("Vision Band not found.")
        print("Long-press BUTTON1 to power the Vision Band ON.")
        return

    print(f"Found: {device.name}")
    print(f"Address: {device.address}")
    print("Connecting...")

    async with BleakClient(device) as client:
        print()
        print("CONNECTED")
        print(f"Connected: {client.is_connected}")
        print()

        print("Subscribing to Vision Band TX notifications...")

        await client.start_notify(
            TX_UUID,
            notification_handler,
        )

        print("Subscribed.")
        print("Waiting for HELLO_VISION_BAND...")

        try:
            await asyncio.wait_for(
                hello_event.wait(),
                timeout=5.0,
            )
        except asyncio.TimeoutError:
            print()
            print("FAIL: HELLO_VISION_BAND was not received.")
            return

        print()
        print("HELLO PASS")
        print()
        print('PC -> Vision Band: PING')

        await client.write_gatt_char(
            RX_UUID,
            b"PING",
            response=True,
        )

        print("Waiting for PONG...")

        try:
            await asyncio.wait_for(
                pong_event.wait(),
                timeout=5.0,
            )
        except asyncio.TimeoutError:
            print()
            print("FAIL: PONG was not received.")
            return

        print()
        print("PONG PASS")
        print()
        print("==============================")
        print("PHASE 6B PASS")
        print("Bidirectional GATT data works.")
        print("==============================")

        await asyncio.sleep(1)

        await client.stop_notify(
            TX_UUID
        )


if __name__ == "__main__":
    asyncio.run(main())
