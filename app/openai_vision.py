import base64
import time

from openai import OpenAI

from config import OPENAI_MODEL, VISION_PROMPT


class OpenAIVisionClient:
    def __init__(self) -> None:
        # The official SDK reads OPENAI_API_KEY from the environment.
        self.client = OpenAI()

    def analyze_jpeg(self, jpeg: bytes) -> tuple[str, float]:
        encoded = base64.b64encode(jpeg).decode("ascii")
        data_url = f"data:image/jpeg;base64,{encoded}"

        print()
        print("Uploading image to OpenAI...")
        started = time.perf_counter()

        response = self.client.responses.create(
            model=OPENAI_MODEL,
            input=[
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "input_text",
                            "text": VISION_PROMPT,
                        },
                        {
                            "type": "input_image",
                            "image_url": data_url,
                        },
                    ],
                }
            ],
            text={
                "verbosity": "low",
            },
        )

        elapsed = time.perf_counter() - started
        text = (response.output_text or "").strip()

        if not text:
            raise RuntimeError("OpenAI returned no text result")

        return text, elapsed
