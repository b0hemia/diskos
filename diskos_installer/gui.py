"""Tkinter/ttk graphical installer.

Architecture:
  * All Tk work on the main thread; a single worker thread runs the engine.
  * The worker drives a QueueReporter; the GUI drains the queue via root.after.
  * A blocking confirm() gate: the worker asks, the main thread shows a modal
    acknowledgement dialog, the worker resumes on the user's decision.
  * Explicit operation states; no Cancel once the destructive flash starts;
    window-close is intercepted while flashing.
"""

import os
import queue
import sys
import threading
import time

from diskos_installer import __version__, bundle, platform_probe, service, state
from diskos_installer.reporter import QueueReporter

ACCENT = "#FF375F"      # diskOS accent
BG = "#1c1c1e"
FG = "#f2f2f7"
SUBFG = "#9a9aa0"
PANEL = "#2c2c2e"
WARN = "#ffcc00"
OKC = "#34c759"
ERRC = "#ff453a"

# operation states
IDLE, RUNNING, CONFIRM, SUCCESS, FAILED = "idle", "running", "confirm", "success", "failed"


class App:
    def __init__(self, root):
        self.root = root
        self.q = queue.Queue()
        self.reporter = QueueReporter(self.q)
        self.worker = None
        self.state = IDLE
        self.flashing = False              # True only during the destructive phase
        self._confirm_event = threading.Event()
        self._confirm_result = False
        self.start_time = None

        root.title("diskOS Installer")
        root.configure(bg=BG)
        root.minsize(560, 640)
        root.protocol("WM_DELETE_WINDOW", self._on_close)

        self._build_style()
        self._build_widgets()
        self._refresh_state_labels()
        self.root.after(80, self._drain)
        self.root.after(1000, self._tick_elapsed)
        self.root.after(4000, self._tick_device)

    # ---- styling -----------------------------------------------------------
    def _build_style(self):
        import tkinter.font as tkfont
        from tkinter import ttk
        self.ttk = ttk
        st = ttk.Style()
        try:
            st.theme_use("clam")           # consistent across Linux/macOS
        except Exception:
            pass
        # Derive from the platform's NAMED fonts (always present) rather than
        # hard-coding families that may be absent (Helvetica/Menlo on Linux).
        def _font(base, size, weight="normal"):
            f = tkfont.nametofont(base).copy()
            f.configure(size=size, weight=weight)
            return f
        self.f_title = _font("TkDefaultFont", 20, "bold")
        self.f_h = _font("TkDefaultFont", 12, "bold")
        self.f_b = _font("TkDefaultFont", 11)
        self.f_mono = _font("TkFixedFont", 9)
        st.configure("TFrame", background=BG)
        st.configure("Panel.TFrame", background=PANEL)
        st.configure("TLabel", background=BG, foreground=FG, font=self.f_b)
        st.configure("Sub.TLabel", background=BG, foreground=SUBFG)
        st.configure("Panel.TLabel", background=PANEL, foreground=FG)
        st.configure("H.TLabel", background=BG, foreground=FG, font=self.f_h)
        st.configure("TRadiobutton", background=BG, foreground=FG, font=self.f_b)
        st.map("TRadiobutton", background=[("active", BG)])
        st.configure("TCheckbutton", background=BG, foreground=FG, font=self.f_b)
        st.map("TCheckbutton", background=[("active", BG)])
        st.configure("Accent.TButton", font=self.f_h, padding=10)
        st.configure("TButton", padding=6)
        st.configure("diskos.Horizontal.TProgressbar", background=ACCENT, troughcolor=PANEL)

    # ---- layout ------------------------------------------------------------
    def _build_widgets(self):
        ttk = self.ttk
        pad = {"padx": 16}
        head = ttk.Frame(self.root)
        head.pack(fill="x", pady=(16, 4), **pad)
        ttk.Label(head, text="diskOS Installer", font=self.f_title,
                  background=BG, foreground=FG).pack(anchor="w")
        self.host_lbl = ttk.Label(head, text="", style="Sub.TLabel")
        self.host_lbl.pack(anchor="w")

        # mode
        self.mode_frame = ttk.Frame(self.root)
        self.mode_frame.pack(fill="x", pady=(10, 4), **pad)
        self.mode = __import__("tkinter").StringVar(value="install")
        for val, txt in (("install", "Install diskOS"),
                         ("restore", "Remove diskOS  (restore stock)"),
                         ("remove", "Uninstall this tool")):
            ttk.Radiobutton(self.mode_frame, text=txt, value=val, variable=self.mode,
                            command=self._on_mode).pack(anchor="w", pady=2)

        # config panel (per-mode)
        self.cfg = ttk.Frame(self.root, style="Panel.TFrame")
        self.cfg.pack(fill="x", pady=10, **pad)
        self._build_config()

        # primary action
        self.action_btn = ttk.Button(self.root, text="Install diskOS...",
                                      style="Accent.TButton", command=self._on_action)
        self.action_btn.pack(fill="x", pady=(4, 8), **pad)

        # progress
        prog = ttk.Frame(self.root)
        prog.pack(fill="both", expand=True, **pad)
        self.phase_lbl = ttk.Label(prog, text="Ready.", style="H.TLabel")
        self.phase_lbl.pack(anchor="w")
        self.status_lbl = ttk.Label(prog, text="", style="Sub.TLabel")
        self.status_lbl.pack(anchor="w", pady=(0, 6))
        self.bar = ttk.Progressbar(prog, style="diskos.Horizontal.TProgressbar",
                                   mode="determinate", maximum=100)
        self.bar.pack(fill="x")
        self.elapsed_lbl = ttk.Label(prog, text="", style="Sub.TLabel")
        self.elapsed_lbl.pack(anchor="w", pady=(2, 6))

        self.warn_banner = __import__("tkinter").Label(
            prog, text="", bg=WARN, fg="#111", font=self.f_h, anchor="center")
        # packed only while flashing

        import tkinter as tk
        logwrap = ttk.Frame(prog)
        logwrap.pack(fill="both", expand=True, pady=(4, 12))
        self.log = tk.Text(logwrap, height=10, bg="#111", fg="#cfcfd4",
                           font=self.f_mono, wrap="word", relief="flat",
                           insertbackground=FG)
        sb = ttk.Scrollbar(logwrap, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set, state="disabled")
        self.log.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

    def _build_config(self):
        for w in self.cfg.winfo_children():
            w.destroy()
        ttk = self.ttk
        import tkinter as tk
        m = self.mode.get()
        inner = ttk.Frame(self.cfg, style="Panel.TFrame")
        inner.pack(fill="x", padx=12, pady=12)
        if m == "install":
            ttk.Label(inner, text="Your FiiO firmware (.zip):", style="Panel.TLabel").grid(
                row=0, column=0, sticky="w")
            self.fw_var = tk.StringVar()
            ttk.Entry(inner, textvariable=self.fw_var, width=44).grid(row=1, column=0, sticky="we", pady=(2, 8))
            ttk.Button(inner, text="Browse...", command=self._pick_firmware).grid(row=1, column=1, padx=(8, 0))
            ttk.Label(inner, text="Variant:", style="Panel.TLabel").grid(row=2, column=0, sticky="w")
            self.variant = tk.StringVar(value="public")
            vr = ttk.Frame(inner, style="Panel.TFrame")
            vr.grid(row=3, column=0, sticky="w")
            ttk.Radiobutton(vr, text="Public (recommended)", value="public",
                            variable=self.variant, command=self._variant_warn).pack(side="left")
            ttk.Radiobutton(vr, text="Dev (root shell)", value="dev",
                            variable=self.variant, command=self._variant_warn).pack(side="left", padx=(12, 0))
            self.variant_warn_lbl = tk.Label(
                inner, text="", bg=PANEL, fg=WARN, font=self.f_b, justify="left", wraplength=460)
            self.variant_warn_lbl.grid(row=4, column=0, sticky="w", pady=(4, 0))
            inner.columnconfigure(0, weight=1)
        elif m == "restore":
            have = state.stock_image_exists()   # fast; full verify happens at restore time
            msg = ("A saved bone-stock image is available - restore is one click."
                   if have else
                   "No saved stock image. Pick your FiiO firmware .zip so I can rebuild it.")
            ttk.Label(inner, text=msg, style="Panel.TLabel", wraplength=460).grid(
                row=0, column=0, columnspan=2, sticky="w")
            if not have:
                self.fw_var = tk.StringVar()
                ttk.Entry(inner, textvariable=self.fw_var, width=44).grid(row=1, column=0, sticky="we", pady=(8, 0))
                ttk.Button(inner, text="Browse...", command=self._pick_firmware).grid(row=1, column=1, padx=(8, 0), pady=(8, 0))
            inner.columnconfigure(0, weight=1)
        else:  # remove
            ttk.Label(inner, wraplength=460, style="Panel.TLabel",
                      text=("Deletes this installer's saved files from THIS computer. "
                            "Nothing was installed system-wide. If diskOS is still on the "
                            "device, use 'Remove diskOS' first.")).pack(anchor="w")

    # ---- helpers -----------------------------------------------------------
    def _pick_firmware(self):
        from tkinter import filedialog
        p = filedialog.askopenfilename(title="Select FiiO firmware .zip",
                                       filetypes=[("Firmware zip", "*.zip"), ("All", "*")])
        if p:
            self.fw_var.set(p)

    def _variant_warn(self):
        lbl = getattr(self, "variant_warn_lbl", None)
        if lbl is None:
            return
        if self.variant.get() == "dev":
            lbl.config(text="⚠ Dev installs an ALWAYS-ON PASSWORDLESS ROOT SHELL over USB - "
                            "anyone with physical access gets root on every boot. It bypasses "
                            "device security. Development devices only, never an everyday one.")
        else:
            lbl.config(text="")

    def _refresh_state_labels(self):
        o, a = platform_probe.host()
        n = platform_probe.maskrom_count()
        dev = {0: "no device in flash mode", -1: "USB not enumerable"}.get(n, f"{n} device(s) in flash mode")
        self.host_lbl.config(text=f"{o}-{a} · v{__version__} · {dev}")

    def _on_mode(self):
        self._build_config()
        self.action_btn.config(text={"install": "Install diskOS...",
                                     "restore": "Remove diskOS (restore stock)...",
                                     "remove": "Uninstall this tool"}[self.mode.get()])

    def _log(self, line, color=None):
        self.log.config(state="normal")
        self.log.insert("end", line + "\n")
        # cap the log so it can't grow unbounded
        if int(self.log.index("end-1c").split(".")[0]) > 500:
            self.log.delete("1.0", "100.0")
        self.log.see("end")
        self.log.config(state="disabled")

    # ---- action / worker ---------------------------------------------------
    def _on_action(self):
        if self.state == RUNNING:
            return
        m = self.mode.get()
        params = {}
        if m == "install":
            fw = getattr(self, "fw_var", None) and self.fw_var.get().strip()
            if not fw:
                self._log("Choose your FiiO firmware .zip first.")
                return
            params = {"firmware": fw, "variant": self.variant.get()}
            fn = service.do_install
        elif m == "restore":
            fw = getattr(self, "fw_var", None)
            params = {"firmware": (fw.get().strip() if fw else None)}
            fn = service.do_restore
        else:
            params = {"force": False}
            fn = service.do_remove

        self.state = RUNNING
        self.start_time = time.monotonic()
        self._set_controls(False)
        self.bar.config(mode="determinate", value=0)
        self.status_lbl.config(text="")
        self.elapsed_lbl.config(text="")
        self.phase_lbl.config(text="Starting...")
        self._clear_finish_extra()

        def run():
            try:
                r = fn(params, self.reporter, self._worker_confirm)
                self.q.put(("done", r))
            except Exception as e:  # BuildError/FlashError/etc.
                self.q.put(("failed", {"error": str(e)}))

        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()

    def _set_controls(self, enabled):
        """Enable/disable all pre-flight controls (mode + config + action) so nothing
        can be changed or re-triggered while a worker is running."""
        st = "normal" if enabled else "disabled"

        def walk(w):
            for c in w.winfo_children():
                cls = c.winfo_class()
                if cls in ("TRadiobutton", "TButton", "TEntry", "TCheckbutton",
                           "Radiobutton", "Button", "Entry", "Checkbutton"):
                    try:
                        c.config(state=st)
                    except Exception:
                        pass
                walk(c)
        walk(self.mode_frame)
        walk(self.cfg)
        self.action_btn.config(state=st)

    def _clear_finish_extra(self):
        w = getattr(self, "_finish_extra", None)
        if w is not None:
            w.destroy()
            self._finish_extra = None

    def _worker_confirm(self, summary):
        """Called ON THE WORKER THREAD. Ask the UI (main thread) and block."""
        self._confirm_result = False
        self._confirm_event.clear()
        self.q.put(("confirm", summary))
        self._confirm_event.wait()
        return self._confirm_result

    def _show_confirm_dialog(self, summary):
        import tkinter as tk
        from tkinter import ttk
        W = 480
        dlg = tk.Toplevel(self.root)
        dlg.title("Confirm")
        dlg.configure(bg=BG)
        dlg.transient(self.root)
        dlg.resizable(False, False)
        destructive = summary.get("action") in ("install", "restore-stock")

        body = ttk.Frame(dlg)
        body.pack(fill="both", expand=True, padx=18, pady=16)
        ttk.Label(body, text=summary.get("action", "confirm").upper(),
                  style="H.TLabel").pack(anchor="w", pady=(0, 6))
        for k in ("variant", "duration", "consequence"):
            if summary.get(k):
                ttk.Label(body, text=f"{k}: {summary[k]}", style="Sub.TLabel",
                          wraplength=W - 36, justify="left").pack(anchor="w", pady=1)

        def _resolve(val):
            # Mark the destructive boundary SYNCHRONOUSLY (main thread) the instant the
            # user accepts - before the worker unblocks and spawns usbboot - so the
            # window can't be closed in the gap before the 'phase' event is drained.
            if val and destructive:
                self.flashing = True
            self._confirm_result = val
            dlg.destroy()
            self._confirm_event.set()

        # button row FIRST (both buttons are direct children of it)
        row = ttk.Frame(body)
        row.pack(side="bottom", fill="x", pady=(14, 0))
        ttk.Button(row, text="Cancel", command=lambda: _resolve(False)).pack(side="right", padx=(8, 0))
        begin = ttk.Button(row, text=("Begin flash (~15 min)" if destructive else "Proceed"),
                           style="Accent.TButton", command=lambda: _resolve(True))
        begin.pack(side="right")

        ack = tk.BooleanVar(value=not destructive)
        if destructive:
            # classic tk.Checkbutton (ttk.Checkbutton has no wraplength)
            tk.Checkbutton(body, variable=ack, wraplength=W - 40, justify="left",
                           text="I understand this rewrites my device and must not be interrupted.",
                           bg=BG, fg=FG, selectcolor=PANEL, activebackground=BG,
                           activeforeground=FG, highlightthickness=0, font=self.f_b,
                           command=lambda: begin.config(state=("normal" if ack.get() else "disabled"))
                           ).pack(anchor="w", pady=(12, 8))
            begin.config(state="disabled")

        # size to content, then centre over the main window
        dlg.update_idletasks()
        h = dlg.winfo_reqheight()
        self.root.update_idletasks()
        px, py = self.root.winfo_rootx(), self.root.winfo_rooty()
        pw = self.root.winfo_width()
        dlg.geometry(f"{W}x{h}+{px + max(0, (pw - W) // 2)}+{py + 110}")
        dlg.grab_set()
        dlg.protocol("WM_DELETE_WINDOW", lambda: _resolve(False))
        dlg.bind("<Escape>", lambda e: _resolve(False))
        dlg.bind("<Return>", lambda e: _resolve(True) if str(begin["state"]) == "normal" else None)
        (begin if not destructive else dlg).focus_set()

    # ---- queue drain (main thread) ----------------------------------------
    def _drain(self):
        budget = 200   # bounded per tick so a flood of events can't starve Tk
        try:
            while budget > 0:
                kind, payload = self.q.get_nowait()
                budget -= 1
                try:
                    self._handle(kind, payload)
                except Exception as e:   # one malformed event must not stop the drain
                    self._log(f"[ui error handling {kind}: {e}]")
        except queue.Empty:
            pass
        self.root.after(80, self._drain)

    def _handle(self, kind, p):
        if kind == "phase":
            self.phase_lbl.config(text=p["name"])
            if p.get("destructive"):
                self.flashing = True
                self.bar.config(mode="indeterminate")
                self.bar.start(12)
                self.warn_banner.config(text="DO NOT DISCONNECT THE DEVICE OR LET THE HOST SLEEP")
                self.warn_banner.pack(fill="x", pady=6, before=self.log.master)
            self._log("▶ " + p["name"])
        elif kind == "status":
            self.status_lbl.config(text=p["message"])
            self._log(p["message"])
        elif kind == "log":
            self._log("   " + p["line"])
        elif kind == "progress":
            if self.bar["mode"] == "indeterminate":
                self.bar.stop(); self.bar.config(mode="determinate")
            total = p["total"] or 1
            self.bar.config(maximum=total, value=p["completed"])
        elif kind == "indeterminate":
            if p["active"]:
                self.bar.config(mode="indeterminate"); self.bar.start(12)
                self.status_lbl.config(text=p.get("note", ""))
            else:
                self.bar.stop(); self.bar.config(mode="determinate")
        elif kind == "warning":
            self._log("! " + p["message"])
        elif kind == "ok":
            self._log("✓ " + p["message"])
        elif kind == "error":
            self._log("✗ " + p["message"])
        elif kind == "confirm":
            self._show_confirm_dialog(p)
        elif kind == "done":
            self._finish(p)
        elif kind == "failed":
            self._fail(p.get("error", "unknown error"))

    def _finish(self, r):
        self.flashing = False
        if self.bar["mode"] == "indeterminate":
            self.bar.stop()
        self.warn_banner.pack_forget()
        self._set_controls(True)
        if r.get("aborted"):
            self.state = IDLE
            self.phase_lbl.config(text="Cancelled - device unchanged.")
        elif r.get("errors"):        # e.g. uninstall couldn't remove everything
            self.state = FAILED
            self.phase_lbl.config(text="Finished with problems - see log.")
        else:
            self.state = SUCCESS
            self.bar.config(mode="determinate", maximum=100, value=100)
            if self.mode.get() == "install":
                self.phase_lbl.config(text="Flashed ✓  One more step ↓")
                self._install_followup()
            elif self.mode.get() == "remove":
                self.phase_lbl.config(text="Removed ✓  Delete the app to finish.")
            else:
                self.phase_lbl.config(text="Done ✓  Power-cycle the device.")
        self._refresh_state_labels()

    def _install_followup(self):
        """Post-flash finish message. The UI is embedded in the flash and installs on first boot."""
        import tkinter as tk
        self._clear_finish_extra()
        box = tk.Frame(self.root, bg=OKC)
        box.pack(fill="x", padx=16, pady=(0, 8))
        self._finish_extra = box
        tk.Label(box, bg=OKC, fg="#111", font=self.f_h, justify="left", wraplength=500,
                 text="Done - power-cycle the device and diskOS installs itself on first "
                      "boot.").pack(anchor="w", padx=10, pady=(8, 8))

    def _fail(self, msg):
        self.flashing = False
        try:
            self.bar.stop()
        except Exception:
            pass
        self.warn_banner.pack_forget()
        self.state = FAILED
        self._set_controls(True)
        self.phase_lbl.config(text="Failed")
        self._log("✗ " + msg)
        from tkinter import messagebox
        messagebox.showerror("diskOS Installer", msg)

    def _tick_elapsed(self):
        if self.state == RUNNING and self.start_time:
            secs = int(time.monotonic() - self.start_time)
            self.elapsed_lbl.config(text=f"elapsed {secs // 60}m{secs % 60:02d}s")
        self.root.after(1000, self._tick_elapsed)

    def _tick_device(self):
        # keep the device-status line fresh, but never scan USB mid-operation
        if self.state != RUNNING:
            self._refresh_state_labels()
        self.root.after(4000, self._tick_device)

    def _on_close(self):
        from tkinter import messagebox
        if self.flashing:
            messagebox.showwarning(
                "Flash in progress",
                "A flash is in progress. Closing now can leave the device needing a "
                "mask-ROM recovery. Please wait until it finishes.")
            return
        if self.state == RUNNING:
            if not messagebox.askokcancel(
                    "Operation in progress",
                    "An operation is still running. Close anyway?"):
                return
        self.root.destroy()


def main():
    try:
        import tkinter as tk
    except Exception as e:
        sys.stderr.write(f"diskOS Installer: Tkinter unavailable ({e}). Use the CLI: "
                         "diskos-installer --help\n")
        return 2
    try:
        root = tk.Tk()
        App(root)
        root.mainloop()
        return 0
    except Exception as e:
        # A packaged GUI hides early tracebacks - persist one and show a dialog.
        import tempfile
        import traceback
        # Unique, 0600 crash log (a fixed name in a world-writable temp dir could be pre-placed as a
        # symlink to truncate an arbitrary target). The exact path is shown in the message below.
        try:
            fd, logp = tempfile.mkstemp(prefix="diskos-installer-startup-", suffix=".log")
            with os.fdopen(fd, "w") as f:
                f.write(traceback.format_exc())
        except OSError:
            logp = "(could not write a log file)"
        sys.stderr.write(f"diskOS Installer failed to start: {e}\n(log: {logp})\n"
                         "Try the CLI: diskos-installer doctor\n")
        try:
            import tkinter.messagebox as mb
            mb.showerror("diskOS Installer",
                         f"Failed to start:\n{e}\n\nDetails: {logp}\n"
                         "You can still use the command line: diskos-installer doctor")
        except Exception:
            pass
        return 1


if __name__ == "__main__":
    sys.exit(main())
