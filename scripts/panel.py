"""The Lumia 950's panel, in one place.

Two different things need this number and they must not drift:

  * the menu's layout (scripts/check-payload.py picks portrait for a tall panel), and
  * any screenshot quoted as evidence, which is only this panel's if it is this size.

These constants describe the native panel, not a guarantee of the firmware GOP mode.
The on-device 2026-10-26 probe measured 1080x1920 with a 1080-pixel stride. DarwinBoot
must use the actual GOP mode for boot video. Pass `--panel 1080x1920` to the screenshot
checker when checking captures from that mode; a camera photograph is not a framebuffer
capture. QEMU's display size must also be selected explicitly for the mode being tested.
"""

WIDTH = 1440
HEIGHT = 2560
DEPTH = 32

PANEL = (WIDTH, HEIGHT)


def parse(text: str) -> tuple[int, int]:
    """`1440x2560` -> (1440, 2560), refusing anything that is not two sizes."""
    parts = text.lower().split("x", 1)
    if len(parts) != 2:
        raise ValueError(f"panel takes WIDTHxHEIGHT, not {text!r}")
    try:
        width, height = (int(part) for part in parts)
    except ValueError:
        raise ValueError(f"panel takes WIDTHxHEIGHT, not {text!r}") from None
    if not (0 < width < 1 << 14 and 0 < height < 1 << 14):
        raise ValueError(f"panel sides must be 1..16383, got {text!r}")
    return width, height


def describe(panel: tuple[int, int]) -> str:
    return f"{panel[0]}x{panel[1]}"
