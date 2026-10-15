"""Condition waits that also give up as soon as the panel dies."""

import threading
import time
from typing import Callable, Optional

# How often a wait re-checks that the panel is alive.
POLL = 0.5


def wait_for(cond: threading.Condition, predicate: Callable[[], bool], timeout: float,
             abort_check: Optional[Callable[[], None]]) -> bool:
    """Like Condition.wait_for (call with `cond` held), but runs abort_check() every POLL seconds.

    abort_check raises to end the wait early, e.g. QemuPanel.check_alive on a panic.
    """
    deadline = time.monotonic() + timeout
    while not predicate():
        if abort_check is not None:
            abort_check()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        cond.wait(min(remaining, POLL))
    return True
