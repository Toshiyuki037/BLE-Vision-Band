import asyncio

from bleak import BleakScanner


TARGET_NAME = "Vision Band"


async def main():
    print("Scanning for BLE devices...")
    print(f'Looking for "{TARGET_NAME}"')
    print()

    devices = await BleakScanner.discover(
        timeout=8.0,
        return_adv=True,
    )

    found = False

    for address, (device, adv) in devices.items():
        name = (
            adv.local_name
            or
            device.name
            or
            ""
        )

        if name == TARGET_NAME:

            found = True

            print("VISION BAND FOUND")
            print(f"Name:    {name}")
            print(f"Address: {address}")
            print(f"RSSI:    {adv.rssi} dBm")
            print()

    if not found:
        print("Vision Band was not found.")
        print("Confirm the DK is powered, flashed, and advertising.")


if __name__ == "__main__":
    asyncio.run(main())
