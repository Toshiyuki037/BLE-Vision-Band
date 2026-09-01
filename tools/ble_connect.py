import asyncio

from bleak import BleakClient
from bleak import BleakScanner


TARGET_NAME = "Vision Band"


async def main():
    print(f'Searching for "{TARGET_NAME}"...')

    device = await BleakScanner.find_device_by_name(
        TARGET_NAME,
        timeout=10.0,
    )

    if device is None:
        print("Vision Band not found.")
        return

    print(f"Found: {device.name}")
    print(f"Address: {device.address}")
    print("Connecting...")

    async with BleakClient(device) as client:

        print()
        print("CONNECTED")
        print(f"Connected: {client.is_connected}")
        print()
        print("Watch the Vision Band status LED.")
        print("It should be BLUE while this program remains connected.")
        print()
        print("Holding connection for 15 seconds...")

        await asyncio.sleep(15)

    print()
    print("DISCONNECTED")
    print("If the Vision Band is logically ON, its status LED should return to WHITE.")


if __name__ == "__main__":
    asyncio.run(main())
