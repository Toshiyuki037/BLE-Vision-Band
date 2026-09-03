from __future__ import annotations

import asyncio
import contextlib
import sys
import time
from pathlib import Path

from config import CAPTURE_FILE, OPENAI_API_KEY, OPENAI_MODEL
from g2_client import G2Client
from openai_vision import OpenAIVisionClient
from result_router import VisionResult
from vision_band_ble import VisionBandClient


VISION_BAND_RETRY_SECONDS = 2.0


async def connect_band_forever(band: VisionBandClient) -> None:
    while True:
        try:
            await band.connect()
            return
        except Exception as exc:
            print(f"Vision Band not ready: {exc}")
            print(
                f"Retrying Vision Band in "
                f"{VISION_BAND_RETRY_SECONDS:.0f} s..."
            )
            await asyncio.sleep(VISION_BAND_RETRY_SECONDS)


async def run() -> None:
    if not OPENAI_API_KEY:
        raise RuntimeError(
            "OPENAI_API_KEY was not found in vision-band\\.env."
        )

    band = VisionBandClient()
    g2 = G2Client()
    ai = OpenAIVisionClient()

    print()
    print("===================================================")
    print("VISION BAND — WARM G2 + ACKED EVENHUB DISPLAY")
    print("===================================================")
    print("PC ONLY: no iPhone, no Even App, no QR.")
    print("G2: RIGHT temple, background reconnect supervisor.")
    print("Display: e0-20 launch/update with matching e0-00 ACK.")
    print(f"OpenAI model: {OPENAI_MODEL}")
    print("===================================================")
    print()

    try:
        # Start both BLE jobs together so one startup scan does not unnecessarily
        # serialize the whole boot.
        g2_start_task = asyncio.create_task(g2.start())
        band_task = asyncio.create_task(connect_band_forever(band))

        await g2_start_task
        await band_task

        print()
        print("Waiting for first warm G2 connection...")
        await g2.wait_until_ready()

        print()
        print("===================================================")
        print("SESSION READY")
        print("===================================================")
        print("Vision Band is connected.")
        print("G2 has a warm authenticated RIGHT-temple BLE session.")
        print("Capture latency now excludes normal G2 connect time.")
        print("===================================================")
        print()

        while True:
            try:
                transfer = await band.wait_for_image()
                image_started_at = band.assembler.started_at

                capture_path = (
                    Path(__file__).resolve().parent / CAPTURE_FILE
                )
                capture_path.write_bytes(transfer.jpeg)

                print()
                print(f"Saved image: {capture_path}")
                print(
                    f"BLE transfer: {transfer.elapsed_seconds:.2f} s | "
                    f"{transfer.throughput_kbps:.1f} kbps"
                )

                text, ai_elapsed = await asyncio.to_thread(
                    ai.analyze_jpeg,
                    transfer.jpeg,
                )

                result = VisionResult(
                    title="VISION RESULT",
                    text=text,
                )

                print()
                print(result.for_console())
                print()
                print(f"AI processing: {ai_elapsed:.2f} s")

                print()
                print(
                    "AI READY -> sending through ACK-gated "
                    "EvenHub display path..."
                )

                display_started = time.perf_counter()
                await g2.display_text(text)
                display_elapsed = time.perf_counter() - display_started
                display_done_at = time.perf_counter()

                print()
                print("Returning RESULT_READY to Vision Band...")
                ready_started = time.perf_counter()
                await band.send_control_command("RESULT_READY")
                ready_elapsed = time.perf_counter() - ready_started
                finished_at = time.perf_counter()

                print()
                print("===================================================")
                print("CAPTURE LATENCY DEBUG")
                print("===================================================")
                print(
                    f"JPEG BLE transfer             "
                    f"{transfer.elapsed_seconds:8.3f} s"
                )
                print(
                    f"OpenAI                        "
                    f"{ai_elapsed:8.3f} s"
                )
                print(
                    f"G2 display ({g2.last_display_mode:<11}) "
                    f"{display_elapsed:8.3f} s"
                )
                print(
                    f"  matching e0 ACK             "
                    f"{g2.last_ack_ms / 1000.0:8.3f} s"
                )
                print(
                    f"RESULT_READY                   "
                    f"{ready_elapsed:8.3f} s"
                )
                print("---------------------------------------------------")
                print(
                    f"IMAGE START -> G2 ACK          "
                    f"{display_done_at - image_started_at:8.3f} s"
                )
                print(
                    f"IMAGE START -> RESULT_READY     "
                    f"{finished_at - image_started_at:8.3f} s"
                )
                print(
                    f"G2 background reconnect count   "
                    f"{g2.reconnect_count}"
                )
                print("===================================================")
                print()

            except ConnectionError as exc:
                print()
                print(f"Vision Band connection error: {exc}")

                with contextlib.suppress(Exception):
                    await band.disconnect()

                await connect_band_forever(band)

            except Exception as exc:
                print()
                print(
                    f"Pipeline error: "
                    f"{type(exc).__name__}: {exc}"
                )
                print("App remains alive for the next capture.")
                await asyncio.sleep(1.0)

    finally:
        await g2.stop()
        await band.disconnect()


def main() -> None:
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        print()
        print("Vision Band app stopped.")
    except Exception as exc:
        print()
        print(f"FATAL: {type(exc).__name__}: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
