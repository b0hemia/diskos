"""Terminal progress + messaging. TTY-aware: renders live bars/spinners on a
terminal, and degrades to plain timestamped log lines when piped to a file so a
saved log stays readable."""

import sys
import time

# --- colour (only on a TTY that isn't dumb) ---------------------------------
_TTY = sys.stdout.isatty()


def _c(code, s):
    return f"\033[{code}m{s}\033[0m" if _TTY else s


def bold(s):   return _c("1", s)
def dim(s):    return _c("2", s)
def red(s):    return _c("31", s)
def green(s):  return _c("32", s)
def yellow(s): return _c("33", s)
def cyan(s):   return _c("36", s)


def _t():
    return time.strftime("%H:%M:%S")


def info(msg):
    print(f"{dim(_t())} {msg}", flush=True)


def step(msg):
    print(f"\n{cyan('▶')} {bold(msg)}", flush=True)


def ok(msg):
    print(f"{green('✓')} {msg}", flush=True)


def warn(msg):
    print(f"{yellow('!')} {msg}", flush=True)


def err(msg):
    print(f"{red('✗')} {msg}", file=sys.stderr, flush=True)


def fmt_dur(secs):
    secs = int(secs)
    h, rem = divmod(secs, 3600)
    m, s = divmod(rem, 60)
    if h:
        return f"{h}h{m:02d}m{s:02d}s"
    if m:
        return f"{m}m{s:02d}s"
    return f"{s}s"


class Bar:
    """A determinate progress bar for steps with a known total (0..total)."""

    def __init__(self, label, total, width=28):
        self.label = label
        self.total = max(1, total)
        self.width = width
        self.start = time.monotonic()
        self.last_len = 0
        self.update(0)

    def update(self, done, note=""):
        frac = min(1.0, done / self.total)
        elapsed = time.monotonic() - self.start
        eta = (elapsed / frac - elapsed) if frac > 0.02 else 0
        if _TTY:
            fill = int(self.width * frac)
            bar = "█" * fill + "·" * (self.width - fill)
            line = (f"\r  {self.label} {cyan(bar)} {int(frac*100):3d}%"
                    f"  {dim(fmt_dur(elapsed))}"
                    + (f" · ETA {fmt_dur(eta)}" if eta > 0 else "")
                    + (f"  {note}" if note else ""))
            pad = max(0, self.last_len - len(line))
            sys.stdout.write(line + " " * pad)
            sys.stdout.flush()
            self.last_len = len(line)
        else:
            # non-TTY: emit occasional milestone lines, not a live bar
            pct = int(frac * 100)
            if pct in (0, 25, 50, 75, 100) and pct != getattr(self, "_last_pct", -1):
                self._last_pct = pct
                print(f"  {self.label}: {pct}% ({fmt_dur(elapsed)}){(' ' + note) if note else ''}",
                      flush=True)

    def done(self, note=""):
        self.update(self.total, note)
        if _TTY:
            sys.stdout.write("\n")
            sys.stdout.flush()


class Heartbeat:
    """Indeterminate progress for a long step with no reliable progress channel
    (the ~15 min mask-ROM flash). Shows elapsed time + a spinner and a fixed
    'do not disconnect' reminder. Call tick() periodically; stop() to finish."""

    FRAMES = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"

    def __init__(self, label, expect_secs=None, reminder="do NOT disconnect the device"):
        self.label = label
        self.expect = expect_secs
        self.reminder = reminder
        self.start = time.monotonic()
        self.i = 0
        self.last_len = 0

    def tick(self, note=""):
        elapsed = time.monotonic() - self.start
        self.i = (self.i + 1) % len(self.FRAMES)
        if _TTY:
            spin = self.FRAMES[self.i]
            approx = ""
            if self.expect:
                approx = f" / ~{fmt_dur(self.expect)}"
            line = (f"\r  {cyan(spin)} {self.label}  {dim(fmt_dur(elapsed) + approx)}"
                    f"  {yellow('· ' + self.reminder)}"
                    + (f"  {note}" if note else ""))
            pad = max(0, self.last_len - len(line))
            sys.stdout.write(line + " " * pad)
            sys.stdout.flush()
            self.last_len = len(line)
        else:
            # non-TTY: a line every ~30s so a log shows liveness without spamming
            if int(elapsed) % 30 == 0 and int(elapsed) != getattr(self, "_last_log", -1):
                self._last_log = int(elapsed)
                print(f"  {self.label}: {fmt_dur(elapsed)} elapsed ({self.reminder})", flush=True)

    def stop(self):
        if _TTY:
            sys.stdout.write("\n")
            sys.stdout.flush()


def confirm(prompt, default=False):
    """Yes/no prompt. Non-interactive stdin -> returns default (never blocks a pipe)."""
    if not sys.stdin.isatty():
        return default
    d = "Y/n" if default else "y/N"
    try:
        ans = input(f"{yellow('?')} {prompt} [{d}] ").strip().lower()
    except EOFError:
        return default
    if not ans:
        return default
    return ans in ("y", "yes")
