from __future__ import annotations

import os
from pathlib import Path

from dotenv import load_dotenv

APP_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = APP_DIR.parent
load_dotenv(PROJECT_ROOT / ".env")

VISION_BAND_NAME = os.getenv("VISION_BAND_NAME", "Vision Band")

VISION_SERVICE_UUID = "7f510000-1b15-4f0d-8f7b-4c8d4f3a1000"
VISION_CONTROL_TX_UUID = "7f510001-1b15-4f0d-8f7b-4c8d4f3a1000"
VISION_CONTROL_RX_UUID = "7f510002-1b15-4f0d-8f7b-4c8d4f3a1000"
VISION_IMAGE_TX_UUID = "7f510003-1b15-4f0d-8f7b-4c8d4f3a1000"

SCAN_TIMEOUT_SECONDS = float(os.getenv("VISION_SCAN_TIMEOUT", "10"))
IMAGE_TIMEOUT_SECONDS = float(os.getenv("VISION_IMAGE_TIMEOUT", "900"))

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "").strip()
OPENAI_MODEL = os.getenv("OPENAI_MODEL", "gpt-5.6-terra").strip()

VISION_PROMPT = os.getenv(
    "VISION_PROMPT",
    (
        "Analyze this image for a wearable vision assistant. "
        "Return the most useful concise answer for the wearer. "
        "If there is readable text, preserve important text accurately. "
        "If there is a question, equation, circuit, object, chart, document, "
        "or engineering content, explain the useful result directly. "
        "Keep the answer compact enough for smart-glasses display."
    ),
).strip()

CAPTURE_FILE = os.getenv("VISION_CAPTURE_FILE", "capture.jpg").strip()
