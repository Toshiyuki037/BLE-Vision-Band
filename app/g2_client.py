"""
Vision Band -> Even Realities G2 direct BLE client.

PC-only architecture:
    Vision Band -> Windows -> OpenAI -> Windows BLE -> G2

This client deliberately stops using the legacy 06-20 teleprompter transaction
for AI results.  It uses the current EvenHub display wire protocol documented
from official Even App captures:

    e0-20 f1=0   launch/start page
    e0-20 f1=5   in-place text update
    e0-20 f1=12  keepalive

Every display operation is ACK-gated through e0-00 and msgId is kept to a
single byte.  A background supervisor keeps a RIGHT-temple BLE session warm.
If the glasses drop, reconnection happens in the background instead of being
added to the capture's critical path.

Protocol reference:
https://github.com/expectbugs/G2CC/blob/master/docs/G2_BLE_PROTOCOL.md
"""

from __future__ import annotations

import asyncio
import contextlib
import time
from dataclasses import dataclass
from typing import Optional

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice


UUID_BASE = "00002760-08c2-11e1-9073-0e8ac72e{:04x}"
CHAR_WRITE = UUID_BASE.format(0x5401)
CHAR_NOTIFY = UUID_BASE.format(0x5402)

APP_TOKEN = 10000
CONTAINER_ID = 1
CONTAINER_NAME = "main"

ACK_TIMEOUT_SECONDS = 3.0
KEEPALIVE_SECONDS = 5.0
RECONNECT_BACKOFF_SECONDS = 1.0
SCAN_SECONDS = 6.0

# Official captures use ~232 bytes of protobuf payload per AA fragment.
AA_PAYLOAD_CHUNK = 232


def varint(value: int) -> bytes:
    if value < 0:
        value &= (1 << 64) - 1

    out = bytearray()
    while value > 0x7F:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value & 0x7F)
    return bytes(out)


def key(field: int, wire: int) -> bytes:
    return varint((field << 3) | wire)


def p_varint(field: int, value: int) -> bytes:
    return key(field, 0) + varint(value)


def p_bytes(field: int, value: bytes) -> bytes:
    return key(field, 2) + varint(len(value)) + value


def p_string(field: int, value: str) -> bytes:
    return p_bytes(field, value.encode("utf-8"))


def decode_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0

    while offset < len(data):
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift

        if not (byte & 0x80):
            return value, offset

        shift += 7
        if shift > 70:
            raise ValueError("protobuf varint too large")

    raise ValueError("truncated protobuf varint")


def top_varints(payload: bytes) -> dict[int, int]:
    fields: dict[int, int] = {}
    offset = 0

    while offset < len(payload):
        tag, offset = decode_varint(payload, offset)
        field = tag >> 3
        wire = tag & 0x07

        if wire == 0:
            value, offset = decode_varint(payload, offset)
            fields[field] = value
        elif wire == 2:
            length, offset = decode_varint(payload, offset)
            offset += length
        elif wire == 1:
            offset += 8
        elif wire == 5:
            offset += 4
        else:
            break

    return fields


def crc16_ccitt(data: bytes, init: int = 0xFFFF) -> int:
    crc = init

    for byte in data:
        crc ^= byte << 8

        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1

            crc &= 0xFFFF

    return crc


def add_crc(packet_without_crc: bytes) -> bytes:
    crc = crc16_ccitt(packet_without_crc[8:])
    return packet_without_crc + bytes((crc & 0xFF, (crc >> 8) & 0xFF))


def build_auth_packets() -> list[bytes]:
    """
    The known-good seven-packet prelude that already authenticates the user's
    G2 reliably in the existing PC-only build.
    """
    timestamp = int(time.time())
    timestamp_varint = varint(timestamp)

    txid = bytes((
        0xE8, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0x01,
    ))

    packets: list[bytes] = []

    packets.append(add_crc(bytes((
        0xAA, 0x21, 0x01, 0x0C, 0x01, 0x01, 0x80, 0x00,
        0x08, 0x04, 0x10, 0x0C, 0x1A, 0x04, 0x08, 0x01, 0x10, 0x04,
    ))))

    packets.append(add_crc(bytes((
        0xAA, 0x21, 0x02, 0x0A, 0x01, 0x01, 0x80, 0x20,
        0x08, 0x05, 0x10, 0x0E, 0x22, 0x02, 0x08, 0x02,
    ))))

    payload = (
        bytes((0x08, 0x80, 0x01, 0x10, 0x0F, 0x82, 0x08, 0x11, 0x08))
        + timestamp_varint
        + bytes((0x10,))
        + txid
    )
    packets.append(add_crc(
        bytes((
            0xAA, 0x21, 0x03, len(payload) + 2,
            0x01, 0x01, 0x80, 0x20,
        ))
        + payload
    ))

    packets.append(add_crc(bytes((
        0xAA, 0x21, 0x04, 0x0C, 0x01, 0x01, 0x80, 0x00,
        0x08, 0x04, 0x10, 0x10, 0x1A, 0x04, 0x08, 0x01, 0x10, 0x04,
    ))))

    packets.append(add_crc(bytes((
        0xAA, 0x21, 0x05, 0x0C, 0x01, 0x01, 0x80, 0x00,
        0x08, 0x04, 0x10, 0x11, 0x1A, 0x04, 0x08, 0x01, 0x10, 0x04,
    ))))

    packets.append(add_crc(bytes((
        0xAA, 0x21, 0x06, 0x0A, 0x01, 0x01, 0x80, 0x20,
        0x08, 0x05, 0x10, 0x12, 0x22, 0x02, 0x08, 0x01,
    ))))

    payload = (
        bytes((0x08, 0x80, 0x01, 0x10, 0x13, 0x82, 0x08, 0x11, 0x08))
        + timestamp_varint
        + bytes((0x10,))
        + txid
    )
    packets.append(add_crc(
        bytes((
            0xAA, 0x21, 0x07, len(payload) + 2,
            0x01, 0x01, 0x80, 0x20,
        ))
        + payload
    ))

    return packets


def text_container(content: str) -> bytes:
    """
    One valid full-screen text container.

    Exactly one page container has isEventCapture=1, as required by the
    current G2 renderer.
    """
    message = bytearray()

    message += p_varint(1, 0)       # x
    message += p_varint(2, 0)       # y
    message += p_varint(3, 576)     # width
    message += p_varint(4, 288)     # height
    message += p_varint(5, 0)       # border width
    message += p_varint(6, 15)      # border colour
    message += p_varint(7, 0)       # radius
    message += p_varint(8, 8)       # padding
    message += p_varint(9, CONTAINER_ID)
    message += p_string(10, CONTAINER_NAME)
    message += p_varint(11, 1)      # isEventCapture
    message += p_string(12, content)

    return bytes(message)


def launch_payload(msg_id: int, content: str) -> bytes:
    container = text_container(content)

    wrapper = bytearray()
    wrapper += p_varint(1, 1)             # one total container
    wrapper += p_bytes(3, container)      # textObject[]
    wrapper += p_varint(5, APP_TOKEN)

    payload = bytearray()
    payload += p_varint(1, 0)             # launch
    payload += p_varint(2, msg_id)
    payload += p_bytes(3, bytes(wrapper))

    return bytes(payload)


def text_update_payload(msg_id: int, content: str) -> bytes:
    update = bytearray()
    update += p_varint(1, CONTAINER_ID)
    update += p_string(2, CONTAINER_NAME)
    update += p_varint(3, 0)               # contentOffset
    update += p_varint(4, 0)               # contentLength => full replace
    update += p_string(5, content)

    payload = bytearray()
    payload += p_varint(1, 5)
    payload += p_varint(2, msg_id)
    payload += p_bytes(9, bytes(update))

    return bytes(payload)


def keepalive_payload(msg_id: int) -> bytes:
    # Official capture shape: 08 0c 10 <msgId> 72 00
    return (
        p_varint(1, 12)
        + p_varint(2, msg_id)
        + p_bytes(14, b"")
    )


def aa_fragments(
    seq: int,
    service_hi: int,
    service_lo: int,
    payload: bytes,
) -> list[bytes]:
    """
    Build the official AA envelope.

    For multi-packet messages all fragments share the same Seq.  Only the
    final fragment carries the CRC, and that CRC is over the entire protobuf
    payload.
    """
    chunks = [
        payload[index:index + AA_PAYLOAD_CHUNK]
        for index in range(0, len(payload), AA_PAYLOAD_CHUNK)
    ]

    if not chunks:
        chunks = [b""]

    total = len(chunks)
    crc = crc16_ccitt(payload)
    packets: list[bytes] = []

    for index, chunk in enumerate(chunks, start=1):
        final = index == total
        length = len(chunk) + (2 if final else 0)

        header = bytes((
            0xAA,
            0x21,
            seq & 0xFF,
            length,
            total & 0xFF,
            index & 0xFF,
            service_hi,
            service_lo,
        ))

        packet = header + chunk

        if final:
            packet += bytes((crc & 0xFF, (crc >> 8) & 0xFF))

        packets.append(packet)

    return packets


@dataclass
class E0Ack:
    ack_type: int
    msg_id: int
    latency_ms: float


class G2Client:
    """
    Warm, supervised RIGHT-temple G2 connection.

    Important distinction:
    - BLE link health is supervised in the background.
    - A page is considered alive ONLY after an e0 launch ACK.
    - A text result is considered accepted ONLY after its matching e0 ACK.
    """

    def __init__(self) -> None:
        self._device: Optional[BLEDevice] = None
        self._client: Optional[BleakClient] = None

        self._seq = 0x08
        self._msg_id = 0x14

        self._ready = asyncio.Event()
        self._stopping = False
        self._supervisor_task: Optional[asyncio.Task] = None
        self._keepalive_task: Optional[asyncio.Task] = None

        self._tx_lock = asyncio.Lock()
        self._pending: dict[
            int,
            tuple[int, float, asyncio.Future[E0Ack]],
        ] = {}

        self._page_active = False

        self.last_connect_seconds = 0.0
        self.last_auth_seconds = 0.0
        self.last_display_seconds = 0.0
        self.last_display_mode = ""
        self.last_ack_ms = 0.0
        self.reconnect_count = 0

    @property
    def is_connected(self) -> bool:
        return bool(self._client and self._client.is_connected)

    @property
    def page_active(self) -> bool:
        return self._page_active

    def _next_seq(self) -> int:
        value = self._seq & 0xFF
        self._seq = (value + 1) & 0xFF
        return value

    def _next_msg_id(self) -> int:
        # HARD RULE from official captures: msgId must remain one byte.
        value = self._msg_id & 0xFF
        self._msg_id = (value + 1) & 0xFF
        return value

    async def start(self) -> None:
        if self._supervisor_task is not None:
            return

        self._stopping = False
        self._supervisor_task = asyncio.create_task(
            self._supervisor(),
            name="g2-supervisor",
        )

    async def wait_until_ready(self) -> None:
        await self._ready.wait()

    def _on_disconnected(self, _client: BleakClient) -> None:
        if self._stopping:
            return

        print()
        print("G2 BLE DROPPED -> background reconnect armed.")

        self._ready.clear()
        self._page_active = False
        self._client = None

        for _, _, future in self._pending.values():
            if not future.done():
                future.set_exception(ConnectionError("G2 disconnected"))
        self._pending.clear()

    async def _scan_right(self) -> BLEDevice:
        print("G2 supervisor: scanning for RIGHT temple...")

        devices = await BleakScanner.discover(timeout=SCAN_SECONDS)

        match = next(
            (
                device
                for device in devices
                if device.name
                and "G2" in device.name
                and "_R_" in device.name
            ),
            None,
        )

        if match is None:
            visible = [
                device.name
                for device in devices
                if device.name and "G2" in device.name
            ]
            raise RuntimeError(
                "RIGHT G2 not visible. "
                f"Visible G2 devices: {visible or 'none'}"
            )

        self._device = match
        return match

    async def _connect_once(self) -> None:
        device = self._device

        if device is None:
            device = await self._scan_right()

        started = time.perf_counter()
        print(f"G2 supervisor: connecting to {device.name}...")

        client = BleakClient(
            device,
            disconnected_callback=self._on_disconnected,
        )

        try:
            await client.connect()

            if not client.is_connected:
                raise RuntimeError("G2 BLE connection failed")

            self._client = client
            self.last_connect_seconds = time.perf_counter() - started

            print(
                f"G2 supervisor: connected "
                f"[{self.last_connect_seconds:.3f} s]"
            )

            await client.start_notify(
                CHAR_NOTIFY,
                self._notification_handler,
            )

            auth_started = time.perf_counter()

            for packet in build_auth_packets():
                await client.write_gatt_char(
                    CHAR_WRITE,
                    packet,
                    response=False,
                )
                await asyncio.sleep(0.1)

            await asyncio.sleep(0.5)

            self.last_auth_seconds = time.perf_counter() - auth_started

            # New BLE session: new app session.  Reset counters and page state.
            self._seq = 0x08
            self._msg_id = 0x14
            self._page_active = False

            print(
                f"G2 supervisor: authenticated "
                f"[{self.last_auth_seconds:.3f} s]"
            )

            self._ready.set()

        except Exception:
            with contextlib.suppress(Exception):
                if client.is_connected:
                    await client.disconnect()

            if self._client is client:
                self._client = None

            self._ready.clear()
            self._page_active = False
            raise

    async def _supervisor(self) -> None:
        first_connection = True

        while not self._stopping:
            if self.is_connected:
                await asyncio.sleep(0.25)
                continue

            self._ready.clear()
            self._page_active = False

            try:
                await self._connect_once()

                if first_connection:
                    print("G2 supervisor: WARM SESSION READY.")
                    first_connection = False
                else:
                    self.reconnect_count += 1
                    print(
                        "G2 supervisor: RECONNECTED in background "
                        f"(count={self.reconnect_count})."
                    )

            except asyncio.CancelledError:
                raise

            except Exception as exc:
                print(
                    "G2 supervisor: connect attempt failed: "
                    f"{type(exc).__name__}: {exc}"
                )

                # Force a fresh scan after a failed cached-device connect.
                self._device = None
                await asyncio.sleep(RECONNECT_BACKOFF_SECONDS)

    def _notification_handler(self, _sender, data: bytearray) -> None:
        raw = bytes(data)

        if len(raw) < 10 or raw[0] != 0xAA:
            return

        service_hi = raw[6]
        service_lo = raw[7]

        if (service_hi, service_lo) != (0xE0, 0x00):
            return

        payload = raw[8:-2]

        try:
            fields = top_varints(payload)
        except Exception:
            return

        ack_type = fields.get(1)
        msg_id = fields.get(2)

        if ack_type is None or msg_id is None:
            return

        pending = self._pending.get(msg_id)

        if pending is None:
            return

        expected_ack, sent_at, future = pending

        if ack_type != expected_ack:
            return

        if not future.done():
            future.set_result(
                E0Ack(
                    ack_type=ack_type,
                    msg_id=msg_id,
                    latency_ms=(time.monotonic() - sent_at) * 1000.0,
                )
            )

    async def _send_e0(
        self,
        *,
        label: str,
        payload: bytes,
        msg_id: int,
        expected_ack: int,
    ) -> E0Ack:
        await self.wait_until_ready()

        async with self._tx_lock:
            client = self._client

            if client is None or not client.is_connected:
                raise ConnectionError("G2 disconnected before display write")

            seq = self._next_seq()
            packets = aa_fragments(
                seq,
                0xE0,
                0x20,
                payload,
            )

            loop = asyncio.get_running_loop()
            future: asyncio.Future[E0Ack] = loop.create_future()
            sent_at = time.monotonic()

            self._pending[msg_id] = (
                expected_ack,
                sent_at,
                future,
            )

            print(
                f"G2 {label}: msgId={msg_id} "
                f"fragments={len(packets)} "
                f"expectACK={expected_ack}"
            )

            try:
                for packet in packets:
                    await client.write_gatt_char(
                        CHAR_WRITE,
                        packet,
                        response=False,
                    )

                    # Keep multi-fragment writes gentle.  The official capture
                    # is burst/gap paced; text messages are normally tiny.
                    if len(packets) > 1:
                        await asyncio.sleep(0.012)

                ack = await asyncio.wait_for(
                    future,
                    timeout=ACK_TIMEOUT_SECONDS,
                )

                self.last_ack_ms = ack.latency_ms

                print(
                    f"G2 {label}: ACK PASS "
                    f"msgId={ack.msg_id} "
                    f"{ack.latency_ms:.1f} ms"
                )

                return ack

            finally:
                self._pending.pop(msg_id, None)

    async def _launch(self, text: str) -> None:
        msg_id = self._next_msg_id()

        await self._send_e0(
            label="HUB LAUNCH",
            payload=launch_payload(msg_id, text),
            msg_id=msg_id,
            expected_ack=1,
        )

        self._page_active = True
        self._start_keepalive()

    async def _update(self, text: str) -> None:
        msg_id = self._next_msg_id()

        await self._send_e0(
            label="TEXT UPDATE",
            payload=text_update_payload(msg_id, text),
            msg_id=msg_id,
            expected_ack=6,
        )

    def _start_keepalive(self) -> None:
        if (
            self._keepalive_task is not None
            and not self._keepalive_task.done()
        ):
            return

        self._keepalive_task = asyncio.create_task(
            self._keepalive_loop(),
            name="g2-evenhub-keepalive",
        )

    async def _keepalive_loop(self) -> None:
        try:
            while (
                not self._stopping
                and self.is_connected
                and self._page_active
            ):
                await asyncio.sleep(KEEPALIVE_SECONDS)

                if (
                    self._stopping
                    or not self.is_connected
                    or not self._page_active
                ):
                    return

                msg_id = self._next_msg_id()

                try:
                    await self._send_e0(
                        label="KEEPALIVE",
                        payload=keepalive_payload(msg_id),
                        msg_id=msg_id,
                        expected_ack=12,
                    )
                except Exception as exc:
                    print(
                        "G2 keepalive failed: "
                        f"{type(exc).__name__}: {exc}"
                    )
                    return

        except asyncio.CancelledError:
            raise

    async def display_text(self, text: str) -> None:
        """
        Display one AI result.

        If the current EvenHub page is still alive, use the ~62 ms in-place
        update path.  If the BLE link dropped and the supervisor rebuilt the
        session, launch a fresh one-container page.

        A successful return means a matching e0-00 ACK was received.
        """
        text = text.strip() or " "

        # SDK launch cap is 1000 chars.  Vision Band responses are intentionally
        # concise, but fail loudly rather than silently truncating.
        if len(text) > 1000 and not self._page_active:
            raise ValueError(
                "G2 launch text exceeds the 1000-character startup limit"
            )

        if len(text) > 2000:
            raise ValueError(
                "G2 text update exceeds the 2000-character update limit"
            )

        await self.wait_until_ready()

        started = time.perf_counter()

        # If the link changed between wait_until_ready() and the operation,
        # retry once after the background supervisor has repaired it.
        for attempt in (1, 2):
            try:
                if self._page_active:
                    self.last_display_mode = "TEXT UPDATE"
                    await self._update(text)
                else:
                    self.last_display_mode = "HUB LAUNCH"
                    await self._launch(text)

                self.last_display_seconds = (
                    time.perf_counter() - started
                )

                print(
                    f"G2 DISPLAY ACCEPTED via {self.last_display_mode} "
                    f"[{self.last_display_seconds:.3f} s]"
                )
                return

            except (ConnectionError, asyncio.TimeoutError, TimeoutError) as exc:
                self._ready.clear()
                self._page_active = False

                if attempt == 2:
                    raise

                print(
                    "G2 display interrupted; waiting for background "
                    f"reconnect: {type(exc).__name__}: {exc}"
                )

                await self.wait_until_ready()

    async def stop(self) -> None:
        self._stopping = True
        self._ready.clear()
        self._page_active = False

        tasks = [
            self._keepalive_task,
            self._supervisor_task,
        ]

        for task in tasks:
            if task is not None:
                task.cancel()

        for task in tasks:
            if task is not None:
                with contextlib.suppress(asyncio.CancelledError):
                    await task

        client = self._client
        self._client = None

        if client is not None and client.is_connected:
            with contextlib.suppress(Exception):
                await client.stop_notify(CHAR_NOTIFY)

            with contextlib.suppress(Exception):
                await client.disconnect()
