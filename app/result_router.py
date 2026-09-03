from dataclasses import dataclass


@dataclass
class VisionResult:
    title: str
    text: str
    source: str = "openai"

    def for_console(self) -> str:
        return (
            f"{self.title}\n"
            f"{'-' * len(self.title)}\n"
            f"{self.text}"
        )

    def for_g2(self, max_chars: int = 900) -> str:
        """
        G2-friendly compact output.

        Phase 7C will send this string over the direct G2 BLE transport.
        Keep the result textual so the AI transport and display transport
        remain independent.
        """
        text = " ".join(self.text.split())

        if len(text) <= max_chars:
            return text

        return text[: max_chars - 1].rstrip() + "…"
