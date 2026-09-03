import struct
import time
from dataclasses import dataclass


MSG_START = 0x01
MSG_DATA = 0x02
MSG_END = 0x03


@dataclass
class ImageTransferResult:
    jpeg: bytes
    fifo_upper_bound: int
    packet_count: int
    elapsed_seconds: float

    @property
    def throughput_kbps(self) -> float:
        if self.elapsed_seconds <= 0:
            return 0.0
        return (len(self.jpeg) * 8.0) / self.elapsed_seconds / 1000.0


class ImageAssembler:
    """
    Reconstructs the Vision Band Phase 6C+ IMAGE_TX protocol.

    START:
      0x01 + uint32_le fifo_upper_bound

    DATA:
      0x02 + uint32_le sequence + JPEG bytes

    END:
      0x03 + uint32_le exact_jpeg_length + uint32_le packet_count
    """

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.started = False
        self.complete = False
        self.error: Exception | None = None
        self.fifo_upper_bound = 0
        self.expected_sequence = 0
        self.packet_count = 0
        self.jpeg = bytearray()
        self.started_at = 0.0
        self.finished_at = 0.0
        self.reported_jpeg_length = 0
        self.reported_packet_count = 0

    def feed(self, packet: bytes) -> None:
        if not packet:
            return

        msg_type = packet[0]

        if msg_type == MSG_START:
            if len(packet) < 5:
                raise ValueError("Malformed IMAGE START packet")

            self.reset()
            self.fifo_upper_bound = struct.unpack_from("<I", packet, 1)[0]
            self.started = True
            self.started_at = time.perf_counter()

            print()
            print("IMAGE START")
            print(f"Arducam FIFO upper-bound: {self.fifo_upper_bound:,} bytes")
            print("Receiving JPEG over BLE...")
            return

        if msg_type == MSG_DATA:
            if not self.started:
                raise RuntimeError("Received IMAGE DATA before IMAGE START")
            if len(packet) < 6:
                raise ValueError("Malformed IMAGE DATA packet")

            sequence = struct.unpack_from("<I", packet, 1)[0]
            payload = packet[5:]

            if sequence != self.expected_sequence:
                raise RuntimeError(
                    f"Packet sequence mismatch: expected "
                    f"{self.expected_sequence}, received {sequence}"
                )

            self.jpeg.extend(payload)
            self.packet_count += 1
            self.expected_sequence += 1

            if self.packet_count % 1000 == 0:
                print(
                    f"Packets: {self.packet_count:,} | "
                    f"JPEG bytes: {len(self.jpeg):,}"
                )
            return

        if msg_type == MSG_END:
            if not self.started:
                raise RuntimeError("Received IMAGE END before IMAGE START")
            if len(packet) < 9:
                raise ValueError("Malformed IMAGE END packet")

            self.reported_jpeg_length = struct.unpack_from("<I", packet, 1)[0]
            self.reported_packet_count = struct.unpack_from("<I", packet, 5)[0]
            self.finished_at = time.perf_counter()

            if len(self.jpeg) != self.reported_jpeg_length:
                raise RuntimeError(
                    f"JPEG byte-count mismatch: received {len(self.jpeg)}, "
                    f"reported {self.reported_jpeg_length}"
                )

            if self.packet_count != self.reported_packet_count:
                raise RuntimeError(
                    f"Packet-count mismatch: received {self.packet_count}, "
                    f"reported {self.reported_packet_count}"
                )

            if len(self.jpeg) < 4:
                raise RuntimeError("JPEG is too short")

            if self.jpeg[0:2] != b"\xFF\xD8":
                raise RuntimeError("JPEG SOI marker FF D8 missing")

            if self.jpeg[-2:] != b"\xFF\xD9":
                raise RuntimeError("JPEG EOI marker FF D9 missing")

            self.complete = True

            print()
            print("IMAGE END")
            print(f"Reported JPEG length: {self.reported_jpeg_length:,} bytes")
            print(f"Received JPEG length: {len(self.jpeg):,} bytes")
            print(f"Reported data packets: {self.reported_packet_count:,}")
            print()
            print("JPEG MARKERS PASS")
            print("PACKET SEQUENCE PASS")
            print("BYTE COUNT PASS")
            return

        # Ignore unknown message types rather than crashing the BLE callback.

    def result(self) -> ImageTransferResult:
        if not self.complete:
            raise RuntimeError("Image transfer is not complete")

        elapsed = self.finished_at - self.started_at

        return ImageTransferResult(
            jpeg=bytes(self.jpeg),
            fifo_upper_bound=self.fifo_upper_bound,
            packet_count=self.packet_count,
            elapsed_seconds=elapsed,
        )
