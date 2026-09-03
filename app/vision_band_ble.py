import asyncio

from bleak import BleakClient, BleakScanner

from config import (
    IMAGE_TIMEOUT_SECONDS,
    SCAN_TIMEOUT_SECONDS,
    VISION_BAND_NAME,
    VISION_IMAGE_TX_UUID,
    VISION_CONTROL_RX_UUID,
)
from image_protocol import ImageAssembler, ImageTransferResult


class VisionBandClient:
    def __init__(self) -> None:
        self.device = None
        self.client: BleakClient | None = None

        self.assembler = ImageAssembler()

        self._image_complete = asyncio.Event()
        self._loop: asyncio.AbstractEventLoop | None = None

    async def find(self):
        print(f'Searching for "{VISION_BAND_NAME}"...')

        devices = await BleakScanner.discover(
            timeout=SCAN_TIMEOUT_SECONDS
        )

        for device in devices:
            if device.name == VISION_BAND_NAME:
                self.device = device

                print(f"Found: {device.name}")
                print(f"Address: {device.address}")

                return device

        raise RuntimeError(
            f'Could not find BLE device "{VISION_BAND_NAME}"'
        )

    async def connect(self) -> None:
        if self.device is None:
            await self.find()

        print("Connecting...")

        self.client = BleakClient(
            self.device
        )

        await self.client.connect()

        if not self.client.is_connected:
            raise RuntimeError(
                "Vision Band BLE connection failed"
            )

        print()
        print("CONNECTED")
        print(f"Connected: {self.client.is_connected}")

        self._loop = asyncio.get_running_loop()

        await self.client.start_notify(
            VISION_IMAGE_TX_UUID,
            self._on_image_notification,
        )

        print(
            "Subscribed to Vision Band image notifications."
        )

    def _on_image_notification(
        self,
        sender,
        data: bytearray,
    ) -> None:
        try:
            self.assembler.feed(
                bytes(data)
            )

            if (
                self.assembler.complete
                and self._loop is not None
            ):
                self._loop.call_soon_threadsafe(
                    self._image_complete.set
                )

        except Exception as exc:
            self.assembler.error = exc

            if self._loop is not None:
                self._loop.call_soon_threadsafe(
                    self._image_complete.set
                )

    async def wait_for_image(
        self,
    ) -> ImageTransferResult:
        self.assembler.reset()
        self._image_complete.clear()

        print()
        print("==============================")
        print("VISION BAND + OPENAI READY")
        print("==============================")
        print()
        print(
            "Short-press BUTTON1 on the Vision Band."
        )

        try:
            await asyncio.wait_for(
                self._image_complete.wait(),
                timeout=IMAGE_TIMEOUT_SECONDS,
            )

        except asyncio.TimeoutError as exc:
            raise TimeoutError(
                "No complete image received within "
                f"{IMAGE_TIMEOUT_SECONDS:.0f} seconds"
            ) from exc

        if self.assembler.error is not None:
            raise self.assembler.error

        return self.assembler.result()

    async def send_control_command(
        self,
        command: str,
    ) -> None:
        if (
            self.client is None
            or not self.client.is_connected
        ):
            raise RuntimeError(
                "Cannot send control command: Vision Band is not connected"
            )

        payload = command.encode("utf-8")

        print(
            f'Windows -> Vision Band: "{command}"'
        )

        # Use a write-with-response for Phase 7B so returning from this
        # await means the nRF GATT write handler accepted the command.
        await self.client.write_gatt_char(
            VISION_CONTROL_RX_UUID,
            payload,
            response=True,
        )

        print(
            "RESULT_READY command accepted by BLE peripheral."
        )


    async def disconnect(self) -> None:
        if self.client is None:
            return

        try:
            if self.client.is_connected:
                try:
                    await self.client.stop_notify(
                        VISION_IMAGE_TX_UUID
                    )
                except Exception:
                    pass

                await self.client.disconnect()

        finally:
            self.client = None
