#!/usr/bin/env python3
"""
Car Parts Factory OS Scheduler — Retro CRT Terminal GUI
========================================================
Simulates the same MLFQ logic as factory.c / scheduler.c:
  • 4 priority levels  (Stamping / Painting / Welding / Assembly)
  • Different scheduling algorithm inside each queue
  • Progressive MLFQ quantum per queue level
  • Dynamic base quantum from median READY burst time
  • Context switch guard: q >= 10 × cs
  • Dynamic aging: threshold derived from base quantum + ready load
  • Live terminal output, real-time Gantt chart, stats report

Requirements: Python 3.8+  |  tkinter (stdlib)
Run:  python3 scheduler_gui.py
"""

import tkinter as tk
import threading
import time
import queue
import copy
from dataclasses import dataclass, field
from typing import List, Optional, Tuple
from enum import IntEnum

# ─── Simulation constants (mirrors scheduler.h / factory.h) ──────────────────
PRIORITY_LEVELS = 4
DEFAULT_BASE_QUANTUM = 2
MIN_BASE_QUANTUM = 1
MAX_BASE_QUANTUM = 12

CONTEXT_SWITCH_MS = 2
CONTEXT_SWITCH_FACTOR = 10
SIM_TICK_MS = 80

MIN_AGING_LIMIT = 3
MAX_AGING_LIMIT = 20

STAGE_NAMES     = ["Stamping", "Painting", "Welding", "Assembly"]
QUEUE_ALGO_NAMES = [
    "Longest Job First",
    "FCFS / FIFO",
    "Round Robin / FIFO",
    "SJF / Shortest Remaining",
]

# Special Gantt marker. Normal PIDs are >= 0; this marker displays context switches.
GANTT_CONTEXT_SWITCH = -2


def clamp(value: int, low: int, high: int) -> int:
    return max(low, min(high, value))


def min_quantum_ticks_from_context_switch() -> int:
    required_ms = CONTEXT_SWITCH_FACTOR * CONTEXT_SWITCH_MS
    return max(1, (required_ms + SIM_TICK_MS - 1) // SIM_TICK_MS)


def enforce_context_switch_guard(q: int) -> int:
    return max(q, min_quantum_ticks_from_context_switch())


def quantum_for_priority(priority: int, base_q: int) -> int:
    # Q3 highest priority = base; Q2 = 2×base; Q1 = 4×base; Q0 = 8×base
    multiplier = 1 << (PRIORITY_LEVELS - 1 - priority)
    return enforce_context_switch_guard(base_q * multiplier)


def dynamic_base_quantum_from_ready_bursts(bursts: List[int], fallback: int) -> int:
    if not bursts:
        return enforce_context_switch_guard(clamp(fallback, MIN_BASE_QUANTUM, MAX_BASE_QUANTUM))

    ordered = sorted(bursts)
    n = len(ordered)
    if n % 2 == 1:
        median = ordered[n // 2]
    else:
        median = (ordered[n // 2 - 1] + ordered[n // 2] + 1) // 2

    return enforce_context_switch_guard(clamp(median, MIN_BASE_QUANTUM, MAX_BASE_QUANTUM))


def decide_aging_limit(base_q: int, ready_count: int) -> int:
    # Shorter quantum means aging can happen sooner.
    # Heavier ready-load allows a slightly larger threshold because the belt is busier.
    limit = (2 * base_q) + (ready_count // PRIORITY_LEVELS) + 1
    return clamp(limit, MIN_AGING_LIMIT, MAX_AGING_LIMIT)

# ─── CRT phosphor-green colour palette ───────────────────────────────────────
BG        = "#020802"        # near-black
PANEL_BG  = "#05100A"        # slightly lighter panel
INPUT_BG  = "#030903"        # entry fields
BORDER    = "#1A3C20"        # subtle green border
G_BRIGHT  = "#33FF57"        # bright phosphor green  – headers / values
G_MID     = "#1ECC40"        # body text
G_DIM     = "#0D5C1E"        # dim / inactive
AMBER     = "#FFC107"        # tick counter, quantum expiry
CYAN      = "#00BFA5"        # belt / enqueue messages
RED       = "#FF4444"        # errors / stop
WHITE_G   = "#C8FFC8"        # table headers

# Per-stage block colours for Gantt (foreground, dark background)
GANTT_CLR = [
    ("#FF8C42", "#3D1A00"),   # 0 Stamping  — orange
    ("#FFD166", "#3D2E00"),   # 1 Painting  — yellow
    ("#06D6A0", "#003D2B"),   # 2 Welding   — teal
    ("#38B6FF", "#001E3D"),   # 3 Assembly  — blue
]

# ─── Fonts (all monospace for authentic terminal look) ────────────────────────
FNT_S  = ("Courier", 8)
FNT_M  = ("Courier", 10)
FNT_MB = ("Courier", 10, "bold")
FNT_L  = ("Courier", 13, "bold")
FNT_XL = ("Courier", 16, "bold")
FNT_T  = ("Courier", 18, "bold")


# ─── Process model ────────────────────────────────────────────────────────────
class State(IntEnum):
    NEW = 0
    READY = 1
    RUNNING = 2
    TERMINATED = 3


@dataclass
class Process:
    pid:           int
    init_priority: int
    arrival:       int
    burst:         int
    # Computed fields
    priority:      int   = field(init=False)
    remaining:     int   = field(init=False)
    slice_rem:     int   = field(init=False)
    state:         State = State.NEW
    rdy_wait:      int   = 0
    response:      int   = -1
    completion:    int   = 0
    waiting:       int   = 0
    tat:           int   = 0

    def __post_init__(self):
        self.priority  = self.init_priority
        self.remaining = self.burst
        self.slice_rem = 2   # will be overridden by reset()

    def reset(self, time_slice: int) -> None:
        self.priority   = self.init_priority
        self.remaining  = self.burst
        self.slice_rem  = time_slice
        self.state      = State.NEW
        self.rdy_wait   = 0
        self.response   = -1
        self.completion = 0
        self.waiting    = 0
        self.tat        = 0


# ─── GUI event types (posted from sim thread → main thread) ──────────────────
EVT_LOG   = "log"
EVT_TICK  = "tick"
EVT_GANTT = "gantt"
EVT_STATS = "stats"
EVT_DONE  = "done"


# ─── MLFQ Simulator (runs in a background thread) ────────────────────────────
class MLFQSim:
    """
    Python reimplementation of factory.c + scheduler.c MLFQ logic.
    All results are posted to gui_queue for the main thread to display.
    """

    def __init__(self, procs: List[Process], time_slice: int,
                 gui_queue: "queue.Queue", tick_seconds: float):
        self.procs      = procs          # already reset-and-copied
        self.requested_base_q = time_slice
        self.base_q     = enforce_context_switch_guard(
            clamp(time_slice, MIN_BASE_QUANTUM, MAX_BASE_QUANTUM)
        )
        self.aging_limit = decide_aging_limit(self.base_q, 0)
        self.context_switches = 0
        self.gq         = gui_queue
        self.tick_s     = tick_seconds
        # One queue per priority level (list of pids)
        self.qs: List[List[int]] = [[] for _ in range(PRIORITY_LEVELS)]
        self._stop_evt  = threading.Event()

    def stop(self) -> None:
        self._stop_evt.set()

    # ── Helpers ───────────────────────────────────────────────────────────────

    def _log(self, msg: str, color: str = G_MID) -> None:
        self.gq.put({"type": EVT_LOG, "text": msg, "color": color})

    def _ready_bursts(self) -> List[int]:
        return [p.remaining for p in self.procs if p.state == State.READY]

    def _ready_count(self) -> int:
        return sum(1 for p in self.procs if p.state == State.READY)

    def _recalculate_policy(self, reason: str) -> None:
        old_q = self.base_q
        old_age = self.aging_limit
        bursts = self._ready_bursts()
        ready_count = len(bursts)

        self.base_q = dynamic_base_quantum_from_ready_bursts(bursts, self.base_q)
        self.aging_limit = decide_aging_limit(self.base_q, ready_count)

        if old_q != self.base_q or old_age != self.aging_limit:
            self._log(
                f"[Policy]  {reason}: ready={ready_count}, "
                f"base q={self.base_q}, aging limit={self.aging_limit}, "
                f"min q from cs={min_quantum_ticks_from_context_switch()}",
                CYAN
            )

    def _enqueue(self, pid: int) -> None:
        """Place a part on the belt → priority queue (mirrors beltEnqueue)."""
        pr = self.procs[pid].priority
        self.qs[pr].append(pid)
        self.procs[pid].state    = State.READY
        self.procs[pid].rdy_wait = 0
        self.procs[pid].slice_rem = quantum_for_priority(pr, self.base_q)
        self._log(f"  [Belt]   P{pid} placed on conveyor → {STAGE_NAMES[pr]} queue", CYAN)
        self._recalculate_policy("dynamic quantum/aging recalculated after belt enqueue")
        self.procs[pid].slice_rem = quantum_for_priority(pr, self.base_q)

    def _enqueue_front(self, pid: int) -> None:
        """Re-insert at front of its queue (continuing an unfinished quantum)."""
        pr = self.procs[pid].priority
        self.qs[pr].insert(0, pid)
        self.procs[pid].state = State.READY

    def _select_from_queue(self, pr: int) -> int:
        """
        Select the next process from a specific queue using that queue's
        own internal scheduling algorithm.

        Q3 Assembly : SJF/SRTF-style shortest remaining time first
        Q2 Welding  : Round Robin/FIFO
        Q1 Painting : FCFS/FIFO
        Q0 Stamping : Longest Job First
        """
        if not self.qs[pr]:
            return -1

        if pr == 3:
            # Pick the READY part with the smallest remaining time.
            best_pos = min(
                range(len(self.qs[pr])),
                key=lambda idx: self.procs[self.qs[pr][idx]].remaining
            )
            return self.qs[pr].pop(best_pos)

        if pr == 0:
            # Pick the READY part with the largest remaining time.
            best_pos = max(
                range(len(self.qs[pr])),
                key=lambda idx: self.procs[self.qs[pr][idx]].remaining
            )
            return self.qs[pr].pop(best_pos)

        # Q2 Round Robin and Q1 FCFS both take from the front. Q2 becomes
        # Round Robin because unfinished work is requeued after quantum expiry.
        return self.qs[pr].pop(0)

    # ── Main simulation loop ──────────────────────────────────────────────────

    def run(self) -> None:
        self._log("╔══════════════════════════════════════════════╗", G_BRIGHT)
        self._log("║   FACTORY DISPATCHER  ─  MLFQ CPU SCHEDULER  ║", G_BRIGHT)
        self._log("╚══════════════════════════════════════════════╝", G_BRIGHT)
        self._log("", G_DIM)
        self._log(
            f"[Config] requested base q={self.requested_base_q}, "
            f"context switch={CONTEXT_SWITCH_MS} ms, rule q >= "
            f"{CONTEXT_SWITCH_FACTOR}×cs, min q={min_quantum_ticks_from_context_switch()} tick(s)",
            CYAN)
        self._log(
            f"[Config] starting base q={self.base_q}, aging limit={self.aging_limit}",
            CYAN)
        self._log(
            f"[Config] queue algorithms: Q3={QUEUE_ALGO_NAMES[3]}, "
            f"Q2={QUEUE_ALGO_NAMES[2]}, Q1={QUEUE_ALGO_NAMES[1]}, "
            f"Q0={QUEUE_ALGO_NAMES[0]}",
            CYAN)
        self._log("", G_DIM)

        tick      = 0
        completed = 0
        n         = len(self.procs)

        while completed < n and not self._stop_evt.is_set():
            time.sleep(self.tick_s)

            # Notify GUI of new tick
            self.gq.put({"type": EVT_TICK, "tick": tick})
            self._log(f"╔══ Tick {tick:<3d} ══════════════════════════════════╗", AMBER)

            # ── 1. Admit newly arrived parts ──────────────────────────────
            for p in self.procs:
                if p.arrival == tick and p.state == State.NEW:
                    self._log(
                        f"[Dispatch] P{p.pid} arrived → entering as {STAGE_NAMES[p.priority]}",
                        G_BRIGHT)
                    self._enqueue(p.pid)

            # ── 2. Aging (mirrors dispatcher aging block in factory.c) ────
            aged: List[Tuple[int, int, int]] = []
            for p in self.procs:
                if p.state == State.READY:
                    p.rdy_wait += 1
                    if p.rdy_wait >= self.aging_limit and p.priority < PRIORITY_LEVELS - 1:
                        old_pr = p.priority
                        # Remove from old queue
                        if p.pid in self.qs[old_pr]:
                            self.qs[old_pr].remove(p.pid)
                        p.priority  += 1
                        p.rdy_wait   = 0
                        p.slice_rem   = quantum_for_priority(p.priority, self.base_q)
                        self.qs[p.priority].append(p.pid)
                        aged.append((p.pid, old_pr, p.priority))

            for pid, old_pr, new_pr in aged:
                self._log(
                    f"[Aging]    P{pid}: {STAGE_NAMES[old_pr]} (Q{old_pr})"
                    f" → {STAGE_NAMES[new_pr]} (Q{new_pr})", AMBER)

            if aged:
                self._recalculate_policy("dynamic quantum/aging recalculated after aging")

            # ── 3. Queue snapshot ─────────────────────────────────────────
            for pr in range(PRIORITY_LEVELS - 1, -1, -1):
                items = " ".join(f"P{x}" for x in self.qs[pr]) if self.qs[pr] else "(empty)"
                self._log(f"  {STAGE_NAMES[pr]:<10} [{QUEUE_ALGO_NAMES[pr]:<24}]: {items}", G_DIM)

            # ── 4. Select highest-priority READY queue, then apply
            #       that queue's own internal scheduling algorithm.
            pid_run = -1
            stage   = -1
            for pr in range(PRIORITY_LEVELS - 1, -1, -1):
                if self.qs[pr]:
                    pid_run = self._select_from_queue(pr)
                    stage   = pr
                    break

            if pid_run == -1:
                self._log("  [CPU]    ── IDLE ──", G_DIM)
                self.gq.put({"type": EVT_GANTT, "tick": tick, "pid": -1, "stage": -1})
                tick += 1
                continue

            p = self.procs[pid_run]
            self.context_switches += 1

            # Show the context switch itself on the Gantt chart before the work block.
            self.gq.put({"type": EVT_GANTT, "tick": tick,
                         "pid": GANTT_CONTEXT_SWITCH, "stage": stage})

            # Assign progressive queue quantum if this is the start of a new quantum.
            if p.slice_rem <= 0:
                p.slice_rem = quantum_for_priority(stage, self.base_q)

            # Record first-run response time
            if p.response == -1:
                p.response = tick - p.arrival

            p.state     = State.RUNNING
            p.remaining -= 1
            p.slice_rem -= 1

            self._log(
                f"[{STAGE_NAMES[stage]:<10}] ▶ P{pid_run} using {QUEUE_ALGO_NAMES[stage]}  "
                f"remaining={p.remaining}  slice_left={p.slice_rem}  "
                f"base_q={self.base_q}  station_q={quantum_for_priority(stage, self.base_q)}  "
                f"aging_limit={self.aging_limit}", G_MID)

            # Post Gantt entry
            self.gq.put({"type": EVT_GANTT, "tick": tick,
                         "pid": pid_run, "stage": stage})

            # ── Case A: Part finished ─────────────────────────────────────
            if p.remaining == 0:
                p.state      = State.TERMINATED
                p.completion = tick + 1
                p.tat        = p.completion - p.arrival
                p.waiting    = p.tat - p.burst
                completed   += 1
                self._log(
                    f"[{STAGE_NAMES[stage]:<10}] ✔ P{pid_run} COMPLETED at tick {tick+1}",
                    G_BRIGHT)

            # ── Case B: Quantum expired → MLFQ demotion ──────────────────
            elif p.slice_rem == 0:
                old_pr = p.priority
                if p.priority > 0:
                    p.priority -= 1
                p.slice_rem = quantum_for_priority(p.priority, self.base_q)
                self._log(
                    f"[{STAGE_NAMES[stage]:<10}] ↓ P{pid_run} quantum expired — "
                    f"MLFQ demotion: {STAGE_NAMES[old_pr]} → {STAGE_NAMES[p.priority]}",
                    AMBER)
                self._enqueue(pid_run)

            # ── Case C: Continue in same queue next tick ──────────────────
            else:
                self._enqueue_front(pid_run)

            tick += 1

        # ── Emit final stats ──────────────────────────────────────────────────
        self._emit_stats()
        self.gq.put({"type": EVT_DONE})

    def _emit_stats(self) -> None:
        self._log("", G_DIM)
        self._log("╔══════════════════════════════════════════════════════════╗", G_BRIGHT)
        self._log("║              FACTORY PRODUCTION REPORT                  ║", G_BRIGHT)
        self._log("╚══════════════════════════════════════════════════════════╝", G_BRIGHT)

        hdr = (f"{'PID':<5} {'Stage':<11} {'Arr':>4} {'Burst':>5} "
               f"{'Comp':>5} {'TAT':>5} {'Wait':>5} {'Resp':>5}")
        self._log(hdr, WHITE_G)
        self._log("─" * len(hdr), G_DIM)

        total_w = total_t = total_r = 0
        for p in self.procs:
            total_w += p.waiting
            total_t += p.tat
            total_r += p.response
            self._log(
                f"P{p.pid:<4} {STAGE_NAMES[p.init_priority]:<11} "
                f"{p.arrival:>4} {p.burst:>5} {p.completion:>5} "
                f"{p.tat:>5} {p.waiting:>5} {p.response:>5}", G_MID)

        n = len(self.procs)
        self._log("─" * len(hdr), G_DIM)
        self._log(f"  Avg Waiting Time:    {total_w / n:>6.2f}", G_BRIGHT)
        self._log(f"  Avg Turnaround Time: {total_t / n:>6.2f}", G_BRIGHT)
        self._log(f"  Avg Response Time:   {total_r / n:>6.2f}", G_BRIGHT)
        self._log(f"  Context Switches:    {self.context_switches:>6d}", G_BRIGHT)
        self._log(f"  Final Base Quantum:  {self.base_q:>6d} tick(s)", G_BRIGHT)
        self._log(f"  Final Aging Limit:   {self.aging_limit:>6d} tick(s)", G_BRIGHT)
        self._log(
            f"  CS Guard: q >= {CONTEXT_SWITCH_FACTOR}×{CONTEXT_SWITCH_MS} ms; "
            f"min={min_quantum_ticks_from_context_switch()} tick(s)", G_BRIGHT)

        self.gq.put({"type": EVT_STATS, "data": {
            "procs": self.procs,
            "avg_w": total_w / n,
            "avg_t": total_t / n,
            "avg_r": total_r / n,
            "context_switches": self.context_switches,
            "base_q": self.base_q,
            "aging_limit": self.aging_limit,
        }})


# ─── Main Application ─────────────────────────────────────────────────────────
class FactorySchedulerApp:

    def __init__(self, root: tk.Tk) -> None:
        self.root    = root
        self.procs:  List[Process]         = []
        self.next_pid                       = 0
        self.gq:     queue.Queue           = queue.Queue()
        self.sim:    Optional[MLFQSim]     = None
        self.running                        = False
        self.gantt:  List[Tuple]           = []

        root.title("CAR PARTS FACTORY OS  ·  MLFQ SCHEDULER  v2.0")
        root.configure(bg="#010601")
        root.minsize(1120, 730)

        self._build_ui()
        self._boot_sequence()
        self._poll_queue()

    # ══════════════════════════════════════════════════════════════════════════
    #  UI Construction
    # ══════════════════════════════════════════════════════════════════════════

    def _build_ui(self) -> None:
        # ── Outer monitor bezel ───────────────────────────────────────────
        bezel = tk.Frame(self.root, bg="#0F1F0F", bd=12, relief="ridge")
        bezel.pack(fill="both", expand=True, padx=4, pady=4)

        inner = tk.Frame(bezel, bg="#030A03", bd=4, relief="sunken")
        inner.pack(fill="both", expand=True)

        # ── Title / menu bar ──────────────────────────────────────────────
        self._build_titlebar(inner)
        tk.Frame(inner, bg=BORDER, height=2).pack(fill="x")

        # ── Body ──────────────────────────────────────────────────────────
        body = tk.Frame(inner, bg=BG)
        body.pack(fill="both", expand=True)

        left = tk.Frame(body, bg=PANEL_BG, width=275, bd=0)
        left.pack(side="left", fill="y", padx=(3, 2), pady=3)
        left.pack_propagate(False)
        self._build_left_panel(left)

        right = tk.Frame(body, bg=BG)
        right.pack(side="left", fill="both", expand=True, padx=(2, 3), pady=3)
        self._build_right_panel(right)

        # ── Status bar ────────────────────────────────────────────────────
        sbar = tk.Frame(inner, bg="#030E03", height=22)
        sbar.pack(fill="x")
        sbar.pack_propagate(False)
        tk.Label(sbar, text="  FAST UNIVERSITY  |  Operating Systems Project  |  MLFQ Scheduler",
                 font=FNT_S, bg="#030E03", fg=G_DIM, anchor="w").pack(side="left", fill="y")
        self._sys_label = tk.Label(sbar, text="SYS: READY",
                                   font=FNT_S, bg="#030E03", fg=G_DIM)
        self._sys_label.pack(side="right", padx=8, fill="y")

    def _build_titlebar(self, parent: tk.Frame) -> None:
        tbar = tk.Frame(parent, bg="#081408", height=48)
        tbar.pack(fill="x")
        tbar.pack_propagate(False)

        # LED power indicator
        self._led_cv = tk.Canvas(tbar, width=20, height=20,
                                 bg="#081408", highlightthickness=0)
        self._led_cv.pack(side="left", padx=(14, 6), pady=14)
        self._led = self._led_cv.create_oval(2, 2, 18, 18,
                                             fill=G_DIM, outline="#0A2A0A", width=2)

        tk.Label(tbar,
                 text="▓▓ CAR PARTS FACTORY OS  ─  MLFQ CPU SCHEDULER  v2.0 ▓▓",
                 font=FNT_T, bg="#081408", fg=G_BRIGHT).pack(side="left")

        self._tick_lbl = tk.Label(tbar, text="TICK: 000",
                                  font=FNT_L, bg="#081408", fg=AMBER)
        self._tick_lbl.pack(side="right", padx=20)

    def _section_hdr(self, parent: tk.Frame, text: str) -> None:
        f = tk.Frame(parent, bg=PANEL_BG)
        f.pack(fill="x", pady=(8, 0))
        tk.Label(f, text=f"  {text}  ", font=FNT_MB,
                 bg=PANEL_BG, fg=G_BRIGHT, anchor="w").pack(fill="x", padx=4)
        tk.Frame(f, bg=BORDER, height=1).pack(fill="x", padx=4, pady=(2, 0))

    def _build_left_panel(self, parent: tk.Frame) -> None:
        # ── 1. Top Section: Add Process ───────────────────────────────────
        top_frame = tk.Frame(parent, bg=PANEL_BG)
        top_frame.pack(side="top", fill="x")

        self._section_hdr(top_frame, "▸ ADD PROCESS")

        form = tk.Frame(top_frame, bg=PANEL_BG)
        form.pack(fill="x", padx=8, pady=6)

        # Priority / stage
        tk.Label(form, text="Priority Stage:", font=FNT_S,
                 bg=PANEL_BG, fg=G_MID, anchor="w").pack(fill="x")
        self._prio_var = tk.StringVar(value="Assembly (3)")
        opts = [f"{name} ({i})" for i, name in enumerate(STAGE_NAMES)]
        om = tk.OptionMenu(form, self._prio_var, *opts)
        om.config(bg=INPUT_BG, fg=G_BRIGHT, activebackground=BORDER,
                  activeforeground=G_BRIGHT, font=FNT_S, relief="flat",
                  bd=0, highlightthickness=1, highlightbackground=BORDER,
                  highlightcolor=G_BRIGHT)
        om["menu"].config(bg=INPUT_BG, fg=G_BRIGHT, font=FNT_S,
                          activebackground=BORDER, activeforeground=G_BRIGHT)
        om.pack(fill="x", pady=(1, 6))

        # Arrival time
        tk.Label(form, text="Arrival Time:", font=FNT_S,
                 bg=PANEL_BG, fg=G_MID, anchor="w").pack(fill="x")
        self._arr_var = tk.StringVar(value="0")
        self._entry(form, self._arr_var).pack(fill="x", pady=(1, 6))

        # Burst time
        tk.Label(form, text="Burst Time:", font=FNT_S,
                 bg=PANEL_BG, fg=G_MID, anchor="w").pack(fill="x")
        self._bst_var = tk.StringVar(value="4")
        self._entry(form, self._bst_var).pack(fill="x", pady=(1, 4))

        # Action buttons
        btn_f = tk.Frame(top_frame, bg=PANEL_BG)
        btn_f.pack(fill="x", padx=8, pady=2)
        self._mkbtn(btn_f, "[ + ADD PROCESS ]", self._add_process, G_BRIGHT).pack(fill="x", pady=2)
        self._mkbtn(btn_f, "[ LOAD SAMPLE   ]", self._load_sample, CYAN).pack(fill="x", pady=2)
        self._mkbtn(btn_f, "[ CLEAR ALL     ]", self._clear_all,   RED).pack(fill="x", pady=2)

        # ── 2. Bottom Section: Run Parameters ─────────────────────────────
        # Packing this with side="bottom" BEFORE the expanding middle section
        # guarantees these controls are never pushed off-screen.
        bottom_frame = tk.Frame(parent, bg=PANEL_BG)
        bottom_frame.pack(side="bottom", fill="x")

        self._section_hdr(bottom_frame, "▸ PARAMETERS")
        ctrl = tk.Frame(bottom_frame, bg=PANEL_BG)
        ctrl.pack(fill="x", padx=8, pady=4)

        tk.Label(ctrl, text="Base Time Quantum:", font=FNT_S,
                 bg=PANEL_BG, fg=G_DIM, anchor="w").pack(fill="x")
        self._ts_var = tk.IntVar(value=DEFAULT_BASE_QUANTUM)
        tk.Scale(ctrl, variable=self._ts_var, from_=1, to=6, orient="horizontal",
                 bg=PANEL_BG, fg=G_BRIGHT, troughcolor="#050F05",
                 highlightthickness=0, activebackground=G_MID,
                 font=FNT_S, sliderrelief="flat").pack(fill="x")

        tk.Label(ctrl, text="Tick Speed (sec):", font=FNT_S,
                 bg=PANEL_BG, fg=G_DIM, anchor="w").pack(fill="x", pady=(6, 0))
        self._spd_var = tk.DoubleVar(value=0.30)
        tk.Scale(ctrl, variable=self._spd_var, from_=0.05, to=1.0,
                 resolution=0.05, orient="horizontal",
                 bg=PANEL_BG, fg=AMBER, troughcolor="#050F05",
                 highlightthickness=0, activebackground=AMBER,
                 font=FNT_S, sliderrelief="flat").pack(fill="x")

        self._btn_run  = self._mkbtn(ctrl, "[ ▶  RUN FACTORY  ]", self._run, AMBER)
        self._btn_run.pack(fill="x", pady=(10, 2))
        self._btn_stop = self._mkbtn(ctrl, "[ ■  STOP          ]", self._stop, RED)
        self._btn_stop.pack(fill="x", pady=(0, 8))
        self._btn_stop.config(state="disabled")

        # ── 3. Middle Section: Process Queue List ─────────────────────────
        # Packed with expand=True so it dynamically fills whatever space is
        # left between the top controls and the bottom parameters.
        mid_frame = tk.Frame(parent, bg=PANEL_BG)
        mid_frame.pack(side="top", fill="both", expand=True)

        self._section_hdr(mid_frame, "▸ PROCESS QUEUE")

        plist_frame = tk.Frame(mid_frame, bg=INPUT_BG,
                               highlightthickness=1, highlightbackground=BORDER)
        plist_frame.pack(fill="both", expand=True, padx=8, pady=6)
        self._plist = tk.Text(plist_frame, font=("Courier", 8),
                              bg=INPUT_BG, fg=G_MID, state="disabled",
                              relief="flat", wrap="none", cursor="arrow",
                              highlightthickness=0, width=28)
        psb = tk.Scrollbar(plist_frame, orient="vertical",
                           command=self._plist.yview,
                           bg=PANEL_BG, troughcolor=BG, width=8)
        psb.pack(side="right", fill="y")
        self._plist.config(yscrollcommand=psb.set)
        self._plist.pack(fill="both", expand=True, padx=2, pady=2)
    def _build_right_panel(self, parent: tk.Frame) -> None:
        # ── Terminal output ───────────────────────────────────────────────
        t_outer = tk.Frame(parent, bg=PANEL_BG,
                           highlightthickness=1, highlightbackground=BORDER)
        t_outer.pack(fill="both", expand=True, pady=(0, 3))

        t_hdr = tk.Frame(t_outer, bg="#040D04", height=22)
        t_hdr.pack(fill="x")
        t_hdr.pack_propagate(False)
        tk.Label(t_hdr, text="  ▸ FACTORY TERMINAL OUTPUT",
                 font=FNT_S, bg="#040D04", fg=G_DIM, anchor="w").pack(side="left", fill="y")
        self._status_lbl = tk.Label(t_hdr, text="● IDLE",
                                    font=FNT_S, bg="#040D04", fg=G_DIM)
        self._status_lbl.pack(side="right", padx=10, fill="y")

        # CRT scanline overlay (decorative alternating-row canvas)
        self._term = tk.Text(
            t_outer, font=("Courier", 10), bg=BG, fg=G_MID,
            insertbackground=G_BRIGHT, state="disabled", relief="flat",
            wrap="word", spacing1=2, spacing3=1, highlightthickness=0,
            selectbackground=G_DIM, selectforeground=G_BRIGHT,
        )
        vsb = tk.Scrollbar(t_outer, orient="vertical", command=self._term.yview,
                           bg=PANEL_BG, troughcolor=BG, activebackground=G_DIM,
                           width=10)
        vsb.pack(side="right", fill="y")
        self._term.config(yscrollcommand=vsb.set)
        self._term.pack(fill="both", expand=True, padx=3, pady=3)

        # Colour tags for terminal
        for tag, clr in [
            ("bright", G_BRIGHT), ("mid", G_MID), ("dim",   G_DIM),
            ("amber",  AMBER),    ("cyan", CYAN),  ("red",   RED),
            ("white",  WHITE_G),
        ]:
            self._term.tag_config(tag, foreground=clr)

        # ── Gantt chart ───────────────────────────────────────────────────
        g_outer = tk.Frame(parent, bg=PANEL_BG, height=162,
                           highlightthickness=1, highlightbackground=BORDER)
        g_outer.pack(fill="x", pady=(3, 0))
        g_outer.pack_propagate(False)

        g_hdr = tk.Frame(g_outer, bg="#040D04", height=22)
        g_hdr.pack(fill="x")
        g_hdr.pack_propagate(False)
        tk.Label(g_hdr, text="  ▸ GANTT CHART — WORKSTATION TIMELINE",
                 font=FNT_S, bg="#040D04", fg=G_DIM, anchor="w").pack(side="left", fill="y")

        self._gcv = tk.Canvas(g_outer, bg=BG, highlightthickness=0)
        hsb = tk.Scrollbar(g_outer, orient="horizontal",
                           command=self._gcv.xview,
                           bg=PANEL_BG, troughcolor=BG, width=10)
        hsb.pack(side="bottom", fill="x")
        self._gcv.config(xscrollcommand=hsb.set)
        self._gcv.pack(fill="both", expand=True, padx=3, pady=3)

    # ══════════════════════════════════════════════════════════════════════════
    #  Widget helpers
    # ══════════════════════════════════════════════════════════════════════════

    def _entry(self, parent: tk.Frame, var: tk.Variable) -> tk.Entry:
        return tk.Entry(parent, textvariable=var, font=FNT_M,
                        bg=INPUT_BG, fg=G_BRIGHT, insertbackground=G_BRIGHT,
                        relief="flat", highlightthickness=1,
                        highlightbackground=BORDER, highlightcolor=G_BRIGHT)

    def _mkbtn(self, parent: tk.Frame, text: str,
               cmd, color: str) -> tk.Button:
        btn = tk.Button(parent, text=text, command=cmd, font=FNT_M,
                        bg=PANEL_BG, fg=color, activebackground=color,
                        activeforeground=BG, relief="flat", bd=0,
                        cursor="hand2", highlightthickness=1,
                        highlightbackground=color, highlightcolor=color,
                        pady=3)
        btn.bind("<Enter>", lambda _: btn.config(bg=color, fg=BG))
        btn.bind("<Leave>", lambda _: btn.config(bg=PANEL_BG, fg=color))
        return btn

    # ══════════════════════════════════════════════════════════════════════════
    #  Process management
    # ══════════════════════════════════════════════════════════════════════════

    def _prio_num(self) -> int:
        return int(self._prio_var.get().split("(")[1].rstrip(")"))

    def _add_process(self) -> None:
        try:
            arr = int(self._arr_var.get())
            bst = int(self._bst_var.get())
            if arr < 0 or bst < 1:
                raise ValueError
        except ValueError:
            self._twrite("[ERROR] Arrival ≥ 0 and Burst ≥ 1 required.\n", "red")
            return

        prio = self._prio_num()
        p    = Process(self.next_pid, prio, arr, bst)
        self.procs.append(p)
        self.next_pid += 1
        self._refresh_plist()
        self._twrite(
            f"[Queue]  P{p.pid} added  —  "
            f"{STAGE_NAMES[prio]}, arrival={arr}, burst={bst}\n", "cyan")

    def _load_sample(self) -> None:
        self._clear_all(silent=True)
        for prio, arr, bst in [
            (3, 0, 6), (1, 1, 4), (0, 2, 2),
            (2, 3, 5), (3, 0, 3), (0, 4, 7),
        ]:
            p = Process(self.next_pid, prio, arr, bst)
            self.procs.append(p)
            self.next_pid += 1
        self._refresh_plist()
        self._twrite("[System] Sample workload loaded (6 processes)\n", "amber")

    def _clear_all(self, silent: bool = False) -> None:
        if self.running:
            self._twrite("[ERROR] Cannot clear while factory is running.\n", "red")
            return
        self.procs.clear()
        self.next_pid = 0
        self.gantt.clear()
        self._refresh_plist()
        self._gcv.delete("all")
        if not silent:
            self._twrite("[System] Process queue cleared.\n", "dim")

    def _refresh_plist(self) -> None:
        self._plist.config(state="normal")
        self._plist.delete("1.0", "end")
        self._plist.insert("end", f"{'PID':<5}{'Stage':<11}{'Arr':>3}{'Bst':>4}\n")
        self._plist.insert("end", "─" * 24 + "\n")
        for p in self.procs:
            line = (f"P{p.pid:<4}{STAGE_NAMES[p.init_priority]:<11}"
                    f"{p.arrival:>3}{p.burst:>4}\n")
            self._plist.insert("end", line)
        if not self.procs:
            self._plist.insert("end", "(empty)\n")
        self._plist.config(state="disabled")

    # ══════════════════════════════════════════════════════════════════════════
    #  Terminal
    # ══════════════════════════════════════════════════════════════════════════

    def _twrite(self, text: str, tag: str = "mid") -> None:
        self._term.config(state="normal")
        self._term.insert("end", text, tag)
        self._term.see("end")
        self._term.config(state="disabled")

    def _tclear(self) -> None:
        self._term.config(state="normal")
        self._term.delete("1.0", "end")
        self._term.config(state="disabled")

    # ══════════════════════════════════════════════════════════════════════════
    #  Simulation control
    # ══════════════════════════════════════════════════════════════════════════

    def _run(self) -> None:
        if self.running:
            return
        if not self.procs:
            self._twrite(
                "[ERROR] No processes queued. Add processes or press LOAD SAMPLE.\n",
                "red")
            return

        self._tclear()
        self.gantt.clear()
        self._gcv.delete("all")
        self.running = True
        self._set_running_ui(True)

        ts    = int(self._ts_var.get())
        speed = float(self._spd_var.get())

        # Deep-copy and reset processes for a clean run
        procs_copy = []
        for p in self.procs:
            pc = copy.deepcopy(p)
            pc.reset(ts)
            procs_copy.append(pc)

        self.sim = MLFQSim(procs_copy, ts, self.gq, speed)
        threading.Thread(target=self.sim.run, daemon=True).start()

    def _stop(self) -> None:
        if self.sim:
            self.sim.stop()
        self._set_running_ui(False)
        self._twrite("\n[System] ■ Simulation stopped by user.\n", "red")

    def _set_running_ui(self, on: bool) -> None:
        self.running = on
        state_off = "disabled" if on else "normal"
        state_on  = "normal"   if on else "disabled"
        self._btn_run.config(state=state_off)
        self._btn_stop.config(state=state_on)
        self._status_lbl.config(text="● RUNNING" if on else "● IDLE",
                                fg=G_BRIGHT if on else G_DIM)
        self._sys_label.config(text="SYS: RUNNING" if on else "SYS: READY",
                               fg=G_BRIGHT if on else G_DIM)
        self._led_cv.itemconfig(self._led, fill=G_BRIGHT if on else G_DIM)
        if on:
            self._blink_led()

    # ── LED blink animation ───────────────────────────────────────────────────

    def _blink_led(self) -> None:
        if not self.running:
            return
        cur = self._led_cv.itemcget(self._led, "fill")
        nxt = G_DIM if cur == G_BRIGHT else G_BRIGHT
        self._led_cv.itemconfig(self._led, fill=nxt)
        self.root.after(350, self._blink_led)

    # ══════════════════════════════════════════════════════════════════════════
    #  GUI event queue polling  (called every 40 ms from main thread)
    # ══════════════════════════════════════════════════════════════════════════

    def _poll_queue(self) -> None:
        try:
            while True:
                evt = self.gq.get_nowait()
                self._handle_event(evt)
        except queue.Empty:
            pass
        self.root.after(40, self._poll_queue)

    _COLOR_TAG = {
        G_BRIGHT: "bright", G_MID: "mid", G_DIM: "dim",
        AMBER: "amber", CYAN: "cyan", RED: "red", WHITE_G: "white",
    }

    def _handle_event(self, evt: dict) -> None:
        t = evt["type"]

        if t == EVT_LOG:
            tag = self._COLOR_TAG.get(evt.get("color", G_MID), "mid")
            self._twrite(evt["text"] + "\n", tag)

        elif t == EVT_TICK:
            self._tick_lbl.config(text=f"TICK: {evt['tick']:03d}")

        elif t == EVT_GANTT:
            self.gantt.append((evt["tick"], evt["pid"], evt["stage"]))
            self._draw_gantt()

        elif t == EVT_STATS:
            self._open_stats_window(evt["data"])

        elif t == EVT_DONE:
            self._set_running_ui(False)
            self._twrite("\n[System] ✔ Factory shutdown complete. All parts manufactured.\n",
                         "bright")

    # ══════════════════════════════════════════════════════════════════════════
    #  Gantt chart
    # ══════════════════════════════════════════════════════════════════════════

    def _draw_gantt(self) -> None:
        cv = self._gcv
        cv.delete("all")
        if not self.gantt:
            return

        # Layout constants
        ROW_H  = 28
        CELL_W = 36
        LBL_W  = 92
        PAD_T  = 6
        PAD_B  = 18

        max_tick = max(t for t, _, _ in self.gantt) + 1
        total_w  = LBL_W + max_tick * CELL_W + 20
        total_h  = PAD_T + PRIORITY_LEVELS * ROW_H + PAD_B
        cv.config(scrollregion=(0, 0, total_w, total_h))

        # Row backgrounds + stage labels
        for pr in range(PRIORITY_LEVELS):
            y   = PAD_T + (PRIORITY_LEVELS - 1 - pr) * ROW_H
            row_bg = "#050E05" if pr % 2 == 0 else "#060F06"
            cv.create_rectangle(0, y, total_w, y + ROW_H, fill=row_bg, outline="")
            cv.create_line(0, y, total_w, y, fill=BORDER, width=1)
            cv.create_text(LBL_W - 6, y + ROW_H // 2,
                           text=STAGE_NAMES[pr], anchor="e",
                           font=("Courier", 8), fill=G_DIM)

        # Vertical tick grid lines
        for tick in range(max_tick + 1):
            x = LBL_W + tick * CELL_W
            cv.create_line(x, PAD_T, x, total_h - PAD_B, fill=BORDER, width=1)

        # Tick number labels (every ~1/10 of timeline)
        step = max(1, max_tick // 10)
        for tick in range(0, max_tick + 1, step):
            x = LBL_W + tick * CELL_W
            cv.create_text(x, total_h - PAD_B + 3, text=str(tick),
                           anchor="n", font=("Courier", 7), fill=G_DIM)

        # Process blocks. Draw actual work first.
        for tick, pid, stage in self.gantt:
            if stage == -1 or pid == GANTT_CONTEXT_SWITCH:
                continue
            fg, bg_ = GANTT_CLR[stage]
            y = PAD_T + (PRIORITY_LEVELS - 1 - stage) * ROW_H
            x = LBL_W + tick * CELL_W
            # Shadow
            cv.create_rectangle(x + 4, y + 5, x + CELL_W - 1, y + ROW_H - 1,
                                 fill="#000A00", outline="")
            # Block
            cv.create_rectangle(x + 2, y + 3, x + CELL_W - 2, y + ROW_H - 3,
                                 fill=bg_, outline=fg, width=1)
            # PID label
            cv.create_text(x + CELL_W // 2, y + ROW_H // 2,
                           text=f"P{pid}", font=("Courier", 7, "bold"), fill=fg)

        # Context-switch markers. These are drawn on top so they are visible
        # even when a work block starts in the same tick.
        for tick, pid, stage in self.gantt:
            if stage == -1 or pid != GANTT_CONTEXT_SWITCH:
                continue
            y = PAD_T + (PRIORITY_LEVELS - 1 - stage) * ROW_H
            x = LBL_W + tick * CELL_W
            cv.create_rectangle(x + 2, y + 3, x + 12, y + ROW_H - 3,
                                fill="#2D1600", outline=AMBER, width=1)
            cv.create_text(x + 7, y + ROW_H // 2,
                           text="CS", font=("Courier", 5, "bold"),
                           fill=AMBER, angle=90)

    # ══════════════════════════════════════════════════════════════════════════
    #  Stats popup window
    # ══════════════════════════════════════════════════════════════════════════

    def _open_stats_window(self, data: dict) -> None:
        win = tk.Toplevel(self.root)
        win.title("Factory Production Report")
        win.configure(bg=BG)
        win.geometry("800x610")
        win.resizable(False, False)

        # Header
        for line in [
            "╔══════════════════════════════════════════════════════╗",
            "║           FACTORY PRODUCTION REPORT                 ║",
            "╚══════════════════════════════════════════════════════╝",
        ]:
            tk.Label(win, text=line, font=FNT_MB, bg=BG, fg=G_BRIGHT).pack()

        tk.Frame(win, bg=BORDER, height=1).pack(fill="x", padx=20, pady=4)

        # Table
        tbl = tk.Frame(win, bg=BG)
        tbl.pack(fill="x", padx=20)
        hdrs   = ["PID", "Stage",      "InitPr", "Arr", "Burst", "Comp", "TAT", "Wait", "Resp"]
        widths = [5,     12,           7,        5,     6,       6,      5,     5,       5]

        for c, (h, w) in enumerate(zip(hdrs, widths)):
            tk.Label(tbl, text=h, font=FNT_MB, bg=BG,
                     fg=WHITE_G, width=w, anchor="center").grid(
                row=0, column=c, padx=2, pady=(0, 2))

        tk.Frame(win, bg=BORDER, height=1).pack(fill="x", padx=20, pady=(0, 4))

        tbl2 = tk.Frame(win, bg=BG)
        tbl2.pack(fill="x", padx=20)
        for r, p in enumerate(data["procs"]):
            row_fg = G_BRIGHT if r % 2 == 0 else G_MID
            row_bg = PANEL_BG if r % 2 == 0 else BG
            vals = [
                f"P{p.pid}",
                STAGE_NAMES[p.init_priority],
                str(p.init_priority),
                str(p.arrival),
                str(p.burst),
                str(p.completion),
                str(p.tat),
                str(p.waiting),
                str(p.response),
            ]
            for c, (v, w) in enumerate(zip(vals, widths)):
                tk.Label(tbl2, text=v, font=("Courier", 9),
                         bg=row_bg, fg=row_fg, width=w,
                         anchor="center").grid(row=r, column=c, padx=2, pady=1)

        tk.Frame(win, bg=BORDER, height=1).pack(fill="x", padx=20, pady=6)

        # Averages and scheduler policy summary
        avg_frame = tk.Frame(win, bg=BG)
        avg_frame.pack(pady=4)
        summary_rows = [
            ("Avg Waiting Time:",    f"{data['avg_w']:.2f} ticks"),
            ("Avg Turnaround Time:", f"{data['avg_t']:.2f} ticks"),
            ("Avg Response Time:",   f"{data['avg_r']:.2f} ticks"),
            ("Context Switches:",    f"{data.get('context_switches', 0)}"),
            ("Final Base Quantum:",  f"{data.get('base_q', 0)} tick(s)"),
            ("Final Aging Limit:",   f"{data.get('aging_limit', 0)} tick(s)"),
            ("CS Guard:",            f"q >= {CONTEXT_SWITCH_FACTOR}×{CONTEXT_SWITCH_MS} ms"),
        ]
        for label, val in summary_rows:
            row = tk.Frame(avg_frame, bg=BG)
            row.pack(anchor="w", padx=40, pady=1)
            tk.Label(row, text=label, font=FNT_M, bg=BG,
                     fg=G_MID, width=24, anchor="w").pack(side="left")
            tk.Label(row, text=val, font=FNT_MB,
                     bg=BG, fg=G_BRIGHT).pack(side="left")

        tk.Frame(win, bg=BORDER, height=1).pack(fill="x", padx=20, pady=6)

        # Legend
        legend = tk.Frame(win, bg=BG)
        legend.pack(pady=2)
        for pr, name in enumerate(STAGE_NAMES):
            fg, _ = GANTT_CLR[pr]
            tk.Label(legend, text=f"■ {name}", font=FNT_S,
                     bg=BG, fg=fg).pack(side="left", padx=10)
        tk.Label(legend, text="▌ CS = Context Switch", font=FNT_S,
                 bg=BG, fg=AMBER).pack(side="left", padx=10)

        self._mkbtn(win, "[ CLOSE REPORT ]", win.destroy, G_DIM).pack(pady=10)

    # ══════════════════════════════════════════════════════════════════════════
    #  Boot sequence (runs once at startup)
    # ══════════════════════════════════════════════════════════════════════════

    def _boot_sequence(self) -> None:
        boot_msgs = [
            (1050,"  STAGES:  [0] Stamping  [1] Painting  [2] Welding  [3] Assembly", "cyan"),
            (1150,"  MLFQ:    4 priority levels  |  Aging limit: 5 ticks", "cyan"),
            (1250,"", "dim"),
            (1350,"  Factory ready.  Add processes below and press [ ▶ RUN FACTORY ].", "amber"),
            (1450,"", "dim"),
            (1450,"  Type 'LOAD SAMPLE' to test with a prebuilt workload.", "dim"),
        ]
        for delay, text, tag in boot_msgs:
            self.root.after(delay, lambda t=text, tg=tag: self._twrite(t + "\n", tg))


# ─── Entry point ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    root = tk.Tk()
    app  = FactorySchedulerApp(root)
    root.mainloop()
