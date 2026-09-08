"""
GB Dump Station -- desktop front end for the whole workflow.

One window that covers:

  * picking the board and serial port
  * the reinsertion dance, with live status from the sketch
  * dumping the ROM with a progress bar
  * validating the result (logo, header checksum, global checksum, bank
    uniqueness) and saving it
  * running the controller-as-keyboard bridge

Plain Tkinter so it packages into a single .exe with PyInstaller and has
no web view anywhere in it.

    python gbdump_gui.py
    pyinstaller --onefile --windowed --name GBDumpStation gbdump_gui.py
"""

import hashlib
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import serial
import serial.tools.list_ports

LOGO_HEAD = bytes([0xCE, 0xED, 0x66, 0x66])

# Same mapping the standalone bridge uses; see n64_keyboard/README.md.
D1_BITS = {"A": 0x80, "B": 0x40, "Z": 0x20, "Start": 0x10,
           "Dup": 0x08, "Ddown": 0x04, "Dleft": 0x02, "Dright": 0x01}
D2_BITS = {"L": 0x20, "R": 0x10, "Cup": 0x08, "Cdown": 0x04,
           "Cleft": 0x02, "Cright": 0x01}
BUTTON_TO_KEY = {"A": "x", "B": "z", "Start": "enter", "Z": "rshift",
                 "L": "a", "R": "s", "Cleft": "q", "Cright": "w",
                 "Dup": "up", "Ddown": "down", "Dleft": "left", "Dright": "right"}
KEY_SCANCODES = {"up": (0x48, True), "down": (0x50, True), "left": (0x4B, True),
                 "right": (0x4D, True), "z": (0x2C, False), "x": (0x2D, False),
                 "a": (0x1E, False), "s": (0x1F, False), "q": (0x10, False),
                 "w": (0x11, False), "enter": (0x1C, False), "rshift": (0x36, False)}
STICK_THRESHOLD = 40

# What to build, and where its sources live relative to the repo root.
#
# arduino-cli wants a folder containing a .ino named after the folder, and
# every source beside it. None of the three trees in this repo are laid
# out that way -- the ESP32 one is a PlatformIO project with main.cpp, and
# the keyboard sketch relies on the dumper's shared Joybus sources -- so
# each build is staged into a temporary folder rather than shuffling files
# around inside the repo.
AVR_SHARED = os.path.join("src", "arduinogbdump")

SKETCHES = {
    "avr": {
        "label": "Dumper (Arduino Uno)",
        "fqbn": "arduino:avr:uno",
        "dirs": [AVR_SHARED],
        "entry": "arduinogbdump.ino",
    },
    "esp32": {
        "label": "Dumper (ESP32-C6)",
        "fqbn": "esp32:esp32:esp32c6",
        "dirs": [os.path.join("src", "esp32c6_gbdump", "src")],
        "entry": "main.cpp",  # renamed to <folder>.ino when staged
    },
    "keyboard": {
        "label": "Controller-to-keyboard (Arduino Uno)",
        "fqbn": "arduino:avr:uno",
        "dirs": [os.path.join("src", "n64_keyboard", "n64_keyboard"), AVR_SHARED],
        "entry": "n64_keyboard.ino",
    },
}

SOURCE_SUFFIXES = (".ino", ".cpp", ".h", ".hpp", ".c")


def stage_sketch(root, spec, dest_parent):
    """Assembles an arduino-cli-shaped sketch folder, returns its path.

    Later directories in spec["dirs"] do not overwrite files already
    copied, so a sketch's own version of a file wins over the shared one.
    """
    name = "gbdump_build"
    dest = os.path.join(dest_parent, name)
    os.makedirs(dest, exist_ok=True)

    entry_src = None
    for rel in spec["dirs"]:
        src_dir = os.path.join(root, rel)
        if not os.path.isdir(src_dir):
            raise FileNotFoundError(src_dir)
        for fname in sorted(os.listdir(src_dir)):
            if not fname.endswith(SOURCE_SUFFIXES):
                continue
            src = os.path.join(src_dir, fname)
            if not os.path.isfile(src):
                continue
            if fname == spec["entry"] and entry_src is None:
                entry_src = src
                continue  # copied last, under the folder's name
            if fname.endswith(".ino"):
                # Any other .ino would bring a second setup()/loop() with
                # it -- e.g. the dumper's, pulled in with the shared
                # sources the keyboard sketch needs.
                continue
            target = os.path.join(dest, fname)
            if not os.path.exists(target):
                shutil.copy2(src, target)

    if entry_src is None:
        raise FileNotFoundError(f"entry file {spec['entry']} not found")
    shutil.copy2(entry_src, os.path.join(dest, name + ".ino"))
    return dest


def repo_root():
    """Repo root, whether running from source or from a PyInstaller exe."""
    if getattr(sys, "frozen", False):
        base = os.path.dirname(sys.executable)
    else:
        base = os.path.dirname(os.path.abspath(__file__))
    # tools/ -> repo root; also allow the exe to sit at the root already.
    for candidate in (os.path.dirname(base), base):
        if os.path.isdir(os.path.join(candidate, "src", "arduinogbdump")):
            return candidate
    return os.path.dirname(base)


def find_arduino_cli():
    found = shutil.which("arduino-cli")
    if found:
        return found
    for guess in (r"C:\Program Files\Arduino CLI\arduino-cli.exe",
                  r"C:\Program Files (x86)\Arduino CLI\arduino-cli.exe"):
        if os.path.isfile(guess):
            return guess
    return None


# --- ROM validation ----------------------------------------------------

def header_checksum(rom):
    total = 0
    for i in range(0x134, 0x14D):
        total = (total - rom[i] - 1) & 0xFF
    return total


def global_checksum(rom):
    total = 0
    for i, b in enumerate(rom):
        if i not in (0x14E, 0x14F):
            total = (total + b) & 0xFFFF
    return total


def rom_title(rom):
    return rom[0x134:0x143].split(b"\x00")[0].decode("ascii", "replace")


def validate_rom(rom):
    """Returns (ok, list of (label, value, ok_or_None)) for display."""
    stored = (rom[0x14E] << 8) | rom[0x14F]
    computed = global_checksum(rom)
    hdr = header_checksum(rom)
    banks = len(rom) // 16384
    distinct = len({hashlib.md5(rom[i * 16384:(i + 1) * 16384]).digest()
                    for i in range(banks)}) if banks else 0

    rows = [
        ("Title", rom_title(rom), None),
        ("Size", f"{len(rom)} bytes ({len(rom) / 1024:.0f} KiB)", None),
        ("Cartridge type", f"0x{rom[0x147]:02X}", None),
        ("Nintendo logo", "OK" if rom[0x104:0x108] == LOGO_HEAD else "MISMATCH",
         rom[0x104:0x108] == LOGO_HEAD),
        ("Header checksum", f"0x{hdr:02X} vs 0x{rom[0x14D]:02X}", hdr == rom[0x14D]),
        ("Global checksum", f"0x{computed:04X} vs 0x{stored:04X}", computed == stored),
        ("Distinct banks", f"{distinct}/{banks}", None),
        ("MD5", hashlib.md5(rom).hexdigest(), None),
    ]
    return computed == stored, rows


# --- Windows key injection --------------------------------------------

def make_key_sender():
    """Returns send(scancode, extended, keyup) or None if unavailable."""
    if os.name != "nt":
        return None
    import ctypes
    import ctypes.wintypes as wintypes

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                    ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]

    class MOUSEINPUT(ctypes.Structure):
        # Present only so the union is the right size: INPUT must come out
        # at 40 bytes on x64, and the union is sized by MOUSEINPUT (32),
        # not KEYBDINPUT (24). Get this wrong and every SendInput call
        # fails with ERROR_INVALID_PARAMETER while pressing nothing.
        _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                    ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                    ("time", wintypes.DWORD),
                    ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]

    class _U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]

    class INPUT(ctypes.Structure):
        _fields_ = [("type", wintypes.DWORD), ("union", _U)]

    expected = 40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28
    if ctypes.sizeof(INPUT) != expected:
        return None

    user32 = ctypes.WinDLL("user32", use_last_error=True)

    def send(scancode, extended, keyup):
        flags = 0x0008  # SCANCODE
        if extended:
            flags |= 0x0001
        if keyup:
            flags |= 0x0002
        inp = INPUT(type=1, union=_U(ki=KEYBDINPUT(0, scancode, flags, 0, None)))
        return user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT)) == 1

    return send


# --- Serial worker -----------------------------------------------------

class Worker(threading.Thread):
    """Runs one job on the serial port, reporting back through a queue."""

    def __init__(self, job, port, baud, board, events, out_path=None,
                 sketch_key=None):
        super().__init__(daemon=True)
        self.job = job          # "dump", "bridge" or "flash"
        self.port = port
        self.baud = baud
        self.board = board
        self.events = events    # queue of (kind, payload)
        self.out_path = out_path
        self.sketch_key = sketch_key
        self.stop_flag = threading.Event()

    def emit(self, kind, payload=None):
        self.events.put((kind, payload))

    def open_port(self, reset=True):
        ser = serial.Serial()
        ser.port = self.port
        ser.baudrate = self.baud
        ser.timeout = 1
        ser.dtr = False
        ser.rts = False
        ser.open()
        time.sleep(0.3)
        if reset:
            if self.board == "esp32":
                # Pulse RTS only. Raising DTR as well would pull IO0 low
                # and drop the chip into its bootloader.
                ser.rts = True
                time.sleep(0.1)
                ser.rts = False
            else:
                ser.dtr = True
                time.sleep(0.1)
                ser.dtr = False
            time.sleep(0.4)
        ser.reset_input_buffer()
        return ser

    def run(self):
        try:
            if self.job == "dump":
                self.run_dump()
            elif self.job == "flash":
                self.run_flash()
            else:
                self.run_bridge()
        except Exception as exc:  # surfaced in the UI rather than a console
            self.emit("error", str(exc))
        finally:
            self.emit("done")

    def run_flash(self):
        """Compiles and uploads one of the sketches with arduino-cli."""
        cli = find_arduino_cli()
        if not cli:
            self.emit("error", "arduino-cli not found. Install it and make sure "
                               "it is on PATH.")
            return

        spec = SKETCHES[self.sketch_key]
        root = repo_root()

        tmp = tempfile.mkdtemp(prefix="gbdump-")
        try:
            sketch_dir = stage_sketch(root, spec, tmp)
            self.emit("log", f"Staged sketch in {sketch_dir}\n")

            for phase, args in (
                ("Compiling", ["compile", "--fqbn", spec["fqbn"], sketch_dir]),
                ("Uploading", ["upload", "-p", self.port, "--fqbn", spec["fqbn"],
                               sketch_dir]),
            ):
                if self.stop_flag.is_set():
                    return
                self.emit("log", f"\n=== {phase} {spec['label']} ===\n")
                proc = subprocess.Popen(
                    [cli] + args, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                    errors="replace",
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                for line in proc.stdout:
                    self.emit("log", line)
                    if self.stop_flag.is_set():
                        proc.terminate()
                        return
                if proc.wait() != 0:
                    self.emit("error", f"{phase} failed.")
                    return

            self.emit("log", "\nFlash complete.\n")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def run_dump(self):
        ser = self.open_port(reset=True)
        self.emit("log", "Waiting for a verified insertion.\n"
                          "Reinsert the Transfer Pak while the LED blinks; "
                          "keep going until it goes solid.\n")

        seen = b""
        deadline = time.time() + 600
        while time.time() < deadline and not self.stop_flag.is_set():
            chunk = ser.read(256)
            if chunk:
                seen += chunk
                self.emit("log", chunk.decode(errors="replace"))
                if b"dump ROM" in seen:
                    break
        else:
            ser.close()
            self.emit("log", "\nGave up waiting for a good insertion.\n")
            return
        if self.stop_flag.is_set():
            ser.close()
            return

        ser.reset_input_buffer()
        ser.write(b"r")
        ser.flush()

        line = ser.readline()
        for _ in range(40):
            if b"GBROM_BEGIN" in line:
                break
            line = ser.readline()
        else:
            ser.close()
            self.emit("error", "Never saw GBROM_BEGIN.")
            return

        expected = int(line.split()[1])
        self.emit("log", f"\nDumping {expected} bytes...\n")
        self.emit("total", expected)

        data = bytearray()
        started = time.time()
        while len(data) < expected and not self.stop_flag.is_set():
            if time.time() - started > 1800:
                break
            chunk = ser.read(min(16384, expected - len(data)))
            if chunk:
                data.extend(chunk)
                self.emit("progress", len(data))
        ser.close()

        if len(data) != expected:
            self.emit("error", f"Short read: {len(data)} of {expected} bytes.")
            return

        self.emit("log", f"Received {len(data)} bytes in "
                          f"{time.time() - started:.1f} s\n")
        self.emit("rom", bytes(data))

    def run_bridge(self):
        send = make_key_sender()
        if send is None:
            self.emit("error", "Key injection is only available on Windows.")
            return

        ser = self.open_port(reset=True)
        self.emit("log", "Controller bridge running. Focus RetroArch and play.\n")

        held = set()

        def set_key(name, down):
            sc, ext = KEY_SCANCODES[name]
            if down and name not in held:
                send(sc, ext, False)
                held.add(name)
            elif not down and name in held:
                send(sc, ext, True)
                held.discard(name)

        buf = bytearray()
        try:
            while not self.stop_flag.is_set():
                chunk = ser.read(64)
                if chunk:
                    buf.extend(chunk)
                while len(buf) >= 5:
                    if buf[0] != 0xA5:
                        del buf[0]
                        continue
                    frame = bytes(buf[:5])
                    del buf[:5]
                    d1, d2 = frame[1], frame[2]
                    sx = frame[3] - 256 if frame[3] > 127 else frame[3]
                    sy = frame[4] - 256 if frame[4] > 127 else frame[4]

                    pressed = {n for n, m in D1_BITS.items() if d1 & m}
                    pressed |= {n for n, m in D2_BITS.items() if d2 & m}
                    wanted = {BUTTON_TO_KEY[b] for b in pressed if b in BUTTON_TO_KEY}
                    if sx < -STICK_THRESHOLD:
                        wanted.add("left")
                    elif sx > STICK_THRESHOLD:
                        wanted.add("right")
                    if sy > STICK_THRESHOLD:
                        wanted.add("up")
                    elif sy < -STICK_THRESHOLD:
                        wanted.add("down")

                    for key in KEY_SCANCODES:
                        set_key(key, key in wanted)
                    self.emit("buttons", (sorted(pressed), sx, sy))
        finally:
            for key in list(held):
                sc, ext = KEY_SCANCODES[key]
                send(sc, ext, True)
            ser.close()
            self.emit("log", "Bridge stopped, keys released.\n")


# --- UI ----------------------------------------------------------------

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("GB Dump Station")
        self.geometry("760x620")
        self.minsize(680, 560)

        self.events = queue.Queue()
        self.worker = None
        self.rom = None

        self._build_ui()
        self.after(50, self._pump_events)

    def _build_ui(self):
        pad = {"padx": 8, "pady": 4}

        top = ttk.LabelFrame(self, text="Connection")
        top.pack(fill="x", **pad)

        ttk.Label(top, text="Port").grid(row=0, column=0, sticky="w", padx=6, pady=6)
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(top, textvariable=self.port_var, width=34,
                                     state="readonly")
        self.port_box.grid(row=0, column=1, sticky="w", pady=6)
        ttk.Button(top, text="Refresh", command=self.refresh_ports).grid(
            row=0, column=2, padx=6)

        ttk.Label(top, text="Board").grid(row=1, column=0, sticky="w", padx=6)
        self.board_var = tk.StringVar(value="esp32")
        board_frame = ttk.Frame(top)
        board_frame.grid(row=1, column=1, sticky="w")
        ttk.Radiobutton(board_frame, text="ESP32-C6 (921600)", value="esp32",
                        variable=self.board_var,
                        command=self._board_changed).pack(side="left")
        ttk.Radiobutton(board_frame, text="Arduino AVR (115200)", value="avr",
                        variable=self.board_var,
                        command=self._board_changed).pack(side="left", padx=12)

        self.baud_var = tk.StringVar(value="921600")
        ttk.Label(top, text="Baud").grid(row=2, column=0, sticky="w", padx=6)
        ttk.Entry(top, textvariable=self.baud_var, width=12).grid(
            row=2, column=1, sticky="w", pady=(0, 6))

        fw = ttk.LabelFrame(self, text="Firmware")
        fw.pack(fill="x", **pad)
        ttk.Label(fw, text="Sketch").grid(row=0, column=0, sticky="w",
                                          padx=6, pady=6)
        self.sketch_var = tk.StringVar(value=SKETCHES["esp32"]["label"])
        self.sketch_box = ttk.Combobox(
            fw, textvariable=self.sketch_var, width=40, state="readonly",
            values=[s["label"] for s in SKETCHES.values()])
        self.sketch_box.grid(row=0, column=1, sticky="w", pady=6)
        self.flash_btn = ttk.Button(fw, text="Compile & upload",
                                    command=self.start_flash)
        self.flash_btn.grid(row=0, column=2, padx=8)

        actions = ttk.Frame(self)
        actions.pack(fill="x", **pad)
        self.dump_btn = ttk.Button(actions, text="Dump ROM", command=self.start_dump)
        self.dump_btn.pack(side="left")
        self.bridge_btn = ttk.Button(actions, text="Start controller bridge",
                                     command=self.start_bridge)
        self.bridge_btn.pack(side="left", padx=8)
        self.stop_btn = ttk.Button(actions, text="Stop", command=self.stop_worker,
                                   state="disabled")
        self.stop_btn.pack(side="left")
        self.save_btn = ttk.Button(actions, text="Save ROM as...",
                                   command=self.save_rom, state="disabled")
        self.save_btn.pack(side="right")

        self.progress = ttk.Progressbar(self, mode="determinate")
        self.progress.pack(fill="x", **pad)

        res = ttk.LabelFrame(self, text="Result")
        res.pack(fill="x", **pad)
        self.tree = ttk.Treeview(res, columns=("value",), show="tree headings",
                                 height=8)
        self.tree.heading("#0", text="Check")
        self.tree.heading("value", text="Value")
        self.tree.column("#0", width=170, stretch=False)
        self.tree.column("value", width=520)
        self.tree.pack(fill="x", padx=6, pady=6)
        self.tree.tag_configure("ok", foreground="#0a7d00")
        self.tree.tag_configure("bad", foreground="#c00000")

        logf = ttk.LabelFrame(self, text="Log")
        logf.pack(fill="both", expand=True, **pad)
        self.log = tk.Text(logf, height=10, wrap="word")
        self.log.pack(side="left", fill="both", expand=True, padx=(6, 0), pady=6)
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        sb.pack(side="right", fill="y", pady=6, padx=(0, 6))
        self.log.configure(yscrollcommand=sb.set)

        self.status = tk.StringVar(value="Ready.")
        ttk.Label(self, textvariable=self.status, anchor="w").pack(
            fill="x", padx=10, pady=(0, 8))

        self.refresh_ports()

    def _board_changed(self):
        self.baud_var.set("921600" if self.board_var.get() == "esp32" else "115200")

    def refresh_ports(self):
        ports = list(serial.tools.list_ports.comports())
        labels = [f"{p.device} - {p.description}" for p in ports]
        self.port_box["values"] = labels
        if labels and not self.port_var.get():
            self.port_var.set(labels[0])

    def selected_port(self):
        raw = self.port_var.get()
        return raw.split(" - ")[0] if raw else ""

    def append_log(self, text):
        self.log.insert("end", text)
        self.log.see("end")

    def _busy(self, busy):
        state = "disabled" if busy else "normal"
        self.dump_btn.configure(state=state)
        self.bridge_btn.configure(state=state)
        self.flash_btn.configure(state=state)
        self.stop_btn.configure(state="normal" if busy else "disabled")

    def start_flash(self):
        port = self.selected_port()
        if not port:
            messagebox.showwarning("No port", "Pick a serial port first.")
            return
        key = next((k for k, s in SKETCHES.items()
                    if s["label"] == self.sketch_var.get()), None)
        if key is None:
            return
        self.log.delete("1.0", "end")
        self.append_log(f"Repo root: {repo_root()}\n")
        self.status.set("Flashing...")
        self._busy(True)
        self.worker = Worker("flash", port, int(self.baud_var.get()),
                             self.board_var.get(), self.events, sketch_key=key)
        self.worker.start()

    def start_dump(self):
        port = self.selected_port()
        if not port:
            messagebox.showwarning("No port", "Pick a serial port first.")
            return
        self.rom = None
        self.save_btn.configure(state="disabled")
        for item in self.tree.get_children():
            self.tree.delete(item)
        self.progress.configure(value=0, maximum=100)
        self.log.delete("1.0", "end")
        self.status.set("Dumping...")
        self._busy(True)
        self.worker = Worker("dump", port, int(self.baud_var.get()),
                             self.board_var.get(), self.events)
        self.worker.start()

    def start_bridge(self):
        port = self.selected_port()
        if not port:
            messagebox.showwarning("No port", "Pick a serial port first.")
            return
        self.log.delete("1.0", "end")
        self.status.set("Controller bridge running.")
        self._busy(True)
        self.worker = Worker("bridge", port, int(self.baud_var.get()),
                             self.board_var.get(), self.events)
        self.worker.start()

    def stop_worker(self):
        if self.worker:
            self.worker.stop_flag.set()
        self.status.set("Stopping...")

    def save_rom(self):
        if not self.rom:
            return
        suggested = rom_title(self.rom).strip() or "dump"
        ext = ".gbc" if self.rom[0x143] in (0x80, 0xC0) else ".gb"
        path = filedialog.asksaveasfilename(
            defaultextension=ext, initialfile=suggested + ext,
            filetypes=[("Game Boy ROM", "*.gb *.gbc"), ("All files", "*.*")])
        if path:
            with open(path, "wb") as f:
                f.write(self.rom)
            self.status.set(f"Saved {path}")

    def _pump_events(self):
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "log":
                    self.append_log(payload)
                elif kind == "total":
                    self.progress.configure(maximum=payload, value=0)
                elif kind == "progress":
                    self.progress.configure(value=payload)
                elif kind == "buttons":
                    pressed, sx, sy = payload
                    self.status.set(
                        f"Buttons: {', '.join(pressed) if pressed else '-'}    "
                        f"Stick: {sx}, {sy}")
                elif kind == "rom":
                    self.rom = payload
                    self._show_result(payload)
                elif kind == "error":
                    self.append_log(f"\nERROR: {payload}\n")
                    self.status.set(f"Error: {payload}")
                elif kind == "done":
                    self._busy(False)
                    self.worker = None
                    if self.status.get().startswith(("Dumping", "Stopping",
                                                     "Controller", "Flashing")):
                        self.status.set("Ready.")
        except queue.Empty:
            pass
        self.after(50, self._pump_events)

    def _show_result(self, rom):
        ok, rows = validate_rom(rom)
        for item in self.tree.get_children():
            self.tree.delete(item)
        for label, value, good in rows:
            tag = "" if good is None else ("ok" if good else "bad")
            self.tree.insert("", "end", text=label, values=(value,),
                             tags=(tag,) if tag else ())
        if ok:
            self.status.set("Good dump -- global checksum matches.")
            self.save_btn.configure(state="normal")
        else:
            banks = len(rom) // 16384
            distinct = len({hashlib.md5(rom[i * 16384:(i + 1) * 16384]).digest()
                            for i in range(banks)}) if banks else 0
            extra = ("\nEvery bank is identical: the cartridge's mapper never "
                     "switched." if distinct == 1 and banks > 1 else "")
            self.append_log(
                "\nBAD DUMP -- the global checksum covers every byte, so this "
                "file is wrong somewhere. Reinsert the pak and dump again." +
                extra + "\n")
            self.status.set("Bad dump -- reinsert and try again.")
            self.save_btn.configure(state="normal")  # still saveable, if wanted


if __name__ == "__main__":
    App().mainloop()
