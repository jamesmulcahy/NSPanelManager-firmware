"""Debugging aid, a gdb script (not imported by the tests): lists every FreeRTOS task in a halted
guest, what it is blocked on, and a guess at its call chain. QemuPanel.dump_cpus() (panel.py) runs it when NSPM_QEMU_GDB is set.

    (gdb) source nspm_qemu/freertos_tasks.py
    (gdb) nspm-tasks

QEMU's gdbstub shows CPUs, not tasks, so a guest where every task is blocked shows two idle CPUs and
nothing else. This walks the scheduler's lists instead. For each task: its state, the queue,
semaphore or mutex it waits on (and a mutex's holder), whether it waits for a notification, and
the code addresses found on its stack. The stack scan is a heuristic: it lists every word that
points into code, so read it top-down as "probably called from", and expect some stale entries.
"""

import gdb  # noqa: F401  (provided by gdb)

CODE_LO, CODE_HI = 0x40000000, 0x40400000
SCAN_WORDS = 512
MAX_FRAMES = 16


def _list_items(lst):
    """The owners (TCB pointers) of every item in a FreeRTOS List_t value."""
    n = int(lst["uxNumberOfItems"])
    end = lst["xListEnd"].address
    item = lst["xListEnd"]["pxNext"]
    for _ in range(n):
        if item.cast(end.type) == end:
            break
        yield item["pvOwner"]
        item = item["pxNext"]


def _sym(addr):
    out = gdb.execute("info symbol 0x%x" % addr, to_string=True).strip()
    return None if out.startswith("No symbol") else out


def _scan_stack(tcb):
    top = int(tcb["pxTopOfStack"])
    words = []
    inferior = gdb.selected_inferior()
    try:
        mem = inferior.read_memory(top, SCAN_WORDS * 4).tobytes()
    except gdb.MemoryError:
        return ["(stack unreadable)"]
    for i in range(0, len(mem), 4):
        val = int.from_bytes(mem[i:i + 4], "little")
        # Windowed-ABI return addresses keep the call's window increment in the top two bits.
        addr = (val & 0x3FFFFFFF) | 0x40000000
        if val >> 30 in (1, 2, 3) and CODE_LO <= addr < CODE_HI:
            name = _sym(addr - 3)
            if name and (not words or words[-1] != name):
                words.append(name)
        if len(words) >= MAX_FRAMES:
            break
    return words


def _blocker(tcb, queue_type, offsets):
    item = tcb["xEventListItem"]
    names = [f.name for f in item.type.strip_typedefs().fields()]
    container = item["pxContainer" if "pxContainer" in names else "pvContainer"]
    addr = int(container)
    if addr == 0:
        return None
    for field, off in offsets.items():
        q = gdb.Value(addr - off).cast(queue_type.pointer())
        try:
            n = int(q.dereference()[field]["uxNumberOfItems"])
            length, size = int(q.dereference()["uxLength"]), int(q.dereference()["uxItemSize"])
        except gdb.MemoryError:
            continue
        # The list sits at the same address whichever field it is, so tell the two apart by
        # whether the rest of the queue it implies looks like a queue.
        if n < 1 or n > 64 or not 1 <= length <= 4096 or size > 4096:
            continue
        kind = "semaphore" if size == 0 else "queue"
        what = "waits to %s %s 0x%x" % ("take" if size == 0 and field == "xTasksWaitingToReceive" else
                                        "receive from" if field == "xTasksWaitingToReceive" else "send to", kind, int(q))
        try:
            holder = q.dereference()["u"]["xSemaphore"]["xMutexHolder"]
            if int(q.dereference()["pcHead"]) == 0 and int(holder):
                tcb_t = gdb.lookup_type("TCB_t").pointer()
                what += " (a mutex held by %s)" % holder.cast(tcb_t).dereference()["pcTaskName"].string()
        except (gdb.error, gdb.MemoryError):
            pass
        return what
    return "waits on list 0x%x (event group or other)" % addr


class NspmTasks(gdb.Command):
    """List every FreeRTOS task, what it is blocked on, and code addresses on its stack."""

    def __init__(self):
        super().__init__("nspm-tasks", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        queue_type = gdb.lookup_type("Queue_t")
        offsets = {f.name: f.bitpos // 8 for f in queue_type.fields()
                   if f.name in ("xTasksWaitingToSend", "xTasksWaitingToReceive")}
        lists = []
        ready = gdb.parse_and_eval("pxReadyTasksLists")
        for p in range(ready.type.range()[1] + 1):
            lists.append(("ready", ready[p]))
        for name, state in (("xDelayedTaskList1", "blocked"), ("xDelayedTaskList2", "blocked"),
                            ("xSuspendedTaskList", "suspended/blocked forever"),
                            ("xTasksWaitingTermination", "deleted")):
            lists.append((state, gdb.parse_and_eval(name)))
        pending = gdb.parse_and_eval("xPendingReadyList")
        if pending.type.code == gdb.TYPE_CODE_ARRAY:
            for c in range(pending.type.range()[1] + 1):
                lists.append(("pending ready", pending[c]))
        else:
            lists.append(("pending ready", pending))

        tcb_t = gdb.lookup_type("TCB_t").pointer()
        for state, lst in lists:
            for owner in _list_items(lst):
                tcb = owner.cast(tcb_t).dereference()
                name = tcb["pcTaskName"].string()
                notify = ""
                try:
                    if int(tcb["ucNotifyState"][0]) == 1:
                        notify = ", waits for a notification"
                except gdb.error:
                    pass
                block = _blocker(tcb, queue_type, offsets)
                core = int(tcb["xCoreID"])
                print("%-16s prio %-2d core %-3s %s%s%s" % (
                    name, int(tcb["uxPriority"]), core if core < 2 else "any", state,
                    ", " + block if block else "", notify))
                for frame in _scan_stack(tcb):
                    print("    " + frame)


NspmTasks()
