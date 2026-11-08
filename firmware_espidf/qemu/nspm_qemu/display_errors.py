"""Data, no code: the instructions the firmware is known to send that the display rejects (0x1A).

Each entry is a pattern on the instruction's text and why it is there. A test fails on any
rejected instruction not listed here (see tests/conftest.py). When a firmware fix lands, remove
its entry.
"""

import re
from typing import List, Tuple

KNOWN: List[Tuple[str, str]] = [
    (r"^DRAKJHSUYDGBNCJHGJKSHBDN$",
     "Nextion::init() sends this on purpose, to get the display out of whatever it was doing"),
]

_COMPILED = [(re.compile(pattern), why) for pattern, why in KNOWN]


def unexpected(errors: List[Tuple[str, int]]) -> List[Tuple[str, int]]:
    """The (instruction, code) pairs that no KNOWN entry explains."""
    return [(text, code) for text, code in errors if not any(p.search(text) for p, _ in _COMPILED)]
