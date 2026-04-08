from __future__ import annotations

import ast
import struct
import threading
import time
from dataclasses import dataclass
from typing import Optional

from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.reactive import reactive
from textual.widgets import Button, Footer, Header, Input, Label, Log, Select, Static

try:
    import serial.tools.list_ports
except Exception as exc:
    raise RuntimeError("This app requires pyserial. Install with: pip install pyserial") from exc


def load_ssp_module():
    last_exc = None
    for name in ("simple_serial_packet", "simple_serial_packet_fast"):
        try:
            return __import__(name)
        except Exception as exc:
            last_exc = exc
    raise RuntimeError(
        "Could not import the SimpleSerialPacket Python API. Put this file next to "
        "'simple_serial_packet.py' or 'simple_serial_packet_fast.py'."
    ) from last_exc


SSP = load_ssp_module()

IntegrityMode = SSP.IntegrityMode
SerialConfig = SSP.SerialConfig
FixedLengthSerialPacket = SSP.FixedLengthSerialPacket
VariableLengthSerialPacket = SSP.VariableLengthSerialPacket
VariableLengthSerialPacketCOBS = SSP.VariableLengthSerialPacketCOBS

try:
    available_backends = SSP.available_backends
except Exception:
    available_backends = None

MODE_OPTIONS = [
    ("Fixed Length", "fixed"),
    ("Variable Length", "header"),
    ("Variable Length COBS", "cobs"),
]

INTEGRITY_OPTIONS = [
    ("CRC16 Table", "crc16_table"),
    ("CRC16 Bitwise", "crc16_bitwise"),
    ("Fletcher16", "fletcher16"),
]


def list_serial_ports():
    ports = list(serial.tools.list_ports.comports())
    return [(f"{p.device} — {p.description or 'serial'}", p.device) for p in ports]


def parse_hex_bytes(text: str) -> bytes:
    tokens = text.strip().replace(",", " ").split()
    if not tokens:
        return b""
    return bytes(int(tok, 16) for tok in tokens)


def parse_python_values(parts: str):
    s = parts.strip()
    if not s:
        return ()
    value = ast.literal_eval(f"({s},)")
    if not isinstance(value, tuple):
        value = (value,)
    return value


def now_ts() -> str:
    return time.strftime("%H:%M:%S")


@dataclass
class SessionConfig:
    port: str = ""
    baudrate: int = 115200
    timeout: float = 0.2
    write_timeout: float = 1.0
    mode: str = "header"
    integrity: str = "crc16_table"
    fixed_length: int = 8


class PacketClient:
    def __init__(self) -> None:
        self.cfg = SessionConfig()
        self.api = None

    @property
    def connected(self) -> bool:
        return self.api is not None

    def _integrity_mode(self):
        mapping = {
            "crc16_table": IntegrityMode.CRC16_TABLE,
            "crc16_bitwise": IntegrityMode.CRC16_BITWISE,
            "fletcher16": IntegrityMode.FLETCHER16,
        }
        return mapping[self.cfg.integrity]

    def connect(self):
        if self.connected:
            self.disconnect()
        if not self.cfg.port:
            raise ValueError("No serial port selected")

        serial_cfg = SerialConfig(
            port=self.cfg.port,
            baudrate=self.cfg.baudrate,
            timeout=self.cfg.timeout,
            write_timeout=self.cfg.write_timeout,
        )
        integrity_mode = self._integrity_mode()

        if self.cfg.mode == "fixed":
            self.api = FixedLengthSerialPacket(serial_cfg, mode=integrity_mode)
        elif self.cfg.mode == "header":
            self.api = VariableLengthSerialPacket(serial_cfg, mode=integrity_mode)
        elif self.cfg.mode == "cobs":
            self.api = VariableLengthSerialPacketCOBS(serial_cfg, mode=integrity_mode)
        else:
            raise ValueError(f"Unsupported mode: {self.cfg.mode}")

    def disconnect(self):
        if self.api is not None:
            try:
                self.api.close()
            finally:
                self.api = None

    def reset_buffers(self):
        if not self.connected:
            raise RuntimeError("Not connected")
        self.api.reset()

    def send_payload(self, payload: bytes):
        if not self.connected:
            raise RuntimeError("Not connected")
        self.api.send_payload(payload)

    def recv_payload(self) -> bytes:
        if not self.connected:
            raise RuntimeError("Not connected")
        if self.cfg.mode == "fixed":
            return self.api.recv_payload(self.cfg.fixed_length)
        return self.api.recv_payload()

    def send_struct(self, fmt: str, values):
        payload = struct.pack(fmt, *values)
        self.send_payload(payload)

    def recv_struct(self, fmt: str):
        if not self.connected:
            raise RuntimeError("Not connected")
        if self.cfg.mode == "fixed":
            return self.api.recv_struct(fmt)
        payload = self.recv_payload()
        expected = struct.calcsize(fmt)
        if len(payload) != expected:
            raise ValueError(f"Expected {expected} bytes, received {len(payload)}")
        return struct.unpack(fmt, payload)

    def status_text(self) -> str:
        state = "connected" if self.connected else "disconnected"
        return (
            f"port={self.cfg.port or '-'}  baud={self.cfg.baudrate}  mode={self.cfg.mode}  "
            f"integrity={self.cfg.integrity}  fixed_len={self.cfg.fixed_length}  state={state}"
        )


class SSPTextualApp(App):
    CSS = """
    Screen {
        layout: vertical;
    }

    #main {
        layout: horizontal;
        height: 1fr;
    }

    #sidebar {
        width: 36;
        min-width: 30;
        border: round $accent;
        padding: 1;
    }

    #right-pane {
        layout: vertical;
        width: 1fr;
    }

    #log-panel {
        height: 1fr;
        border: round $accent;
    }

    #status {
        height: 2;
        content-align: left middle;
        padding-left: 1;
    }

    #command-bar {
        height: 3;
        border: round $accent;
        margin-top: 1;
    }

    .section-title {
        text-style: bold;
        margin-top: 1;
        margin-bottom: 1;
    }

    Button {
        width: 1fr;
        margin-top: 1;
    }

    Input, Select {
        margin-bottom: 1;
    }
    """

    BINDINGS = [
        Binding("ctrl+c", "quit", "Quit"),
        Binding("ctrl+r", "refresh_ports", "Refresh Ports"),
        Binding("ctrl+l", "clear_log", "Clear Log"),
    ]

    monitor_enabled = reactive(False)

    def __init__(self):
        super().__init__()
        self.client = PacketClient()
        self.monitor_stop = threading.Event()
        self.monitor_thread: Optional[threading.Thread] = None

    def compose(self) -> ComposeResult:
        port_options = list_serial_ports()
        selected_port = port_options[0][1] if port_options else ""
        if selected_port:
            self.client.cfg.port = selected_port

        yield Header(show_clock=True)
        with Horizontal(id="main"):
            with Vertical(id="sidebar"):
                yield Label("Connection", classes="section-title")
                yield Select(
                    options=port_options or [("No ports found", "")],
                    value=selected_port,
                    id="port_select",
                    allow_blank=True,
                )
                yield Input(value=str(self.client.cfg.baudrate), placeholder="Baud rate", id="baud_input")
                yield Input(value=str(self.client.cfg.timeout), placeholder="Read timeout (s)", id="timeout_input")
                yield Input(value=str(self.client.cfg.write_timeout), placeholder="Write timeout (s)", id="write_timeout_input")
                yield Select(options=MODE_OPTIONS, value=self.client.cfg.mode, id="mode_select")
                yield Select(options=INTEGRITY_OPTIONS, value=self.client.cfg.integrity, id="integrity_select")
                yield Input(value=str(self.client.cfg.fixed_length), placeholder="Fixed payload length", id="fixed_len_input")
                yield Button("Open Serial", id="connect_btn", variant="success")
                yield Button("Close Serial", id="disconnect_btn", variant="error")
                yield Button("Reset Buffers", id="reset_btn")
                yield Button("Refresh Ports", id="refresh_btn")
                yield Button("Recv Once", id="recv_btn")
                yield Button("Monitor: Off", id="monitor_btn")
                yield Label("Bottom Commands", classes="section-title")
                yield Static(
                    "/hex 01 02 aa 55\n"
                    "/text hello\n"
                    "/struct <Bf 3 1.25\n"
                    "/recv\n"
                    "/recvstruct <Bf\n"
                    "/clear",
                    id="help_text",
                )
            with Vertical(id="right-pane"):
                yield Log(id="log-panel", highlight=True, auto_scroll=True)
                yield Static("", id="status")
                yield Input(
                    placeholder="Type /hex, /text, /struct, /recv, /recvstruct, /clear ...",
                    id="command-bar",
                )
        yield Footer()

    def on_mount(self):
        self._log("SYS", "Textual SimpleSerialPacket UI ready.")
        self._log("SYS", "Configure the link on the left. Use the bottom command bar to send or receive.")
        self._update_status()

    def _log(self, kind: str, msg: str):
        log = self.query_one("#log-panel", Log)
        for idx, line in enumerate(msg.splitlines() or [""]):
            prefix = kind if idx == 0 else "..."
            log.write_line(f"[{now_ts()}] {prefix:<3} | {line}")

    def _update_status(self):
        backends = ""
        if available_backends is not None:
            try:
                b = available_backends()
                backends = f"   backends: crc={b.crc_backend} cobs={b.cobs_backend}"
            except Exception:
                pass
        self.query_one("#status", Static).update(self.client.status_text() + backends)
        self.query_one("#monitor_btn", Button).label = f"Monitor: {'On' if self.monitor_enabled else 'Off'}"

    def _apply_config_from_widgets(self):
        self.client.cfg.port = self.query_one("#port_select", Select).value or ""
        self.client.cfg.baudrate = int(self.query_one("#baud_input", Input).value.strip())
        self.client.cfg.timeout = float(self.query_one("#timeout_input", Input).value.strip())
        self.client.cfg.write_timeout = float(self.query_one("#write_timeout_input", Input).value.strip())
        self.client.cfg.mode = self.query_one("#mode_select", Select).value
        self.client.cfg.integrity = self.query_one("#integrity_select", Select).value
        self.client.cfg.fixed_length = int(self.query_one("#fixed_len_input", Input).value.strip())

    def action_refresh_ports(self):
        self._refresh_ports()

    def action_clear_log(self):
        self.query_one("#log-panel", Log).clear()
        self._log("SYS", "Log cleared.")

    def _refresh_ports(self):
        port_select = self.query_one("#port_select", Select)
        current = port_select.value
        options = list_serial_ports()
        port_select.set_options(options or [("No ports found", "")])
        if options:
            values = {value for _, value in options}
            port_select.value = current if current in values else options[0][1]
        else:
            port_select.value = ""
        self._log("SYS", "Serial port list refreshed.")
        self._apply_config_from_widgets()
        self._update_status()

    def on_button_pressed(self, event: Button.Pressed):
        try:
            self._apply_config_from_widgets()
            button_id = event.button.id

            if button_id == "connect_btn":
                self.client.connect()
                self._log("SYS", "Serial port opened.")
            elif button_id == "disconnect_btn":
                self._stop_monitor()
                self.client.disconnect()
                self._log("SYS", "Serial port closed.")
            elif button_id == "reset_btn":
                self.client.reset_buffers()
                self._log("SYS", "Serial buffers reset.")
            elif button_id == "refresh_btn":
                self._refresh_ports()
                return
            elif button_id == "recv_btn":
                payload = self.client.recv_payload()
                self._log("RX", payload.hex(" "))
            elif button_id == "monitor_btn":
                if self.monitor_enabled:
                    self._stop_monitor()
                else:
                    self._start_monitor()

            self._update_status()
        except Exception as exc:
            self._log("ERR", str(exc))
            self._update_status()

    def on_select_changed(self, event: Select.Changed):
        try:
            self._apply_config_from_widgets()
            self._update_status()
        except Exception as exc:
            self._log("ERR", str(exc))

    def on_input_submitted(self, event: Input.Submitted):
        if event.input.id == "command-bar":
            cmd = event.value.strip()
            event.input.value = ""
            if cmd:
                self._handle_command(cmd)
        else:
            try:
                self._apply_config_from_widgets()
                self._update_status()
            except Exception as exc:
                self._log("ERR", str(exc))

    def _handle_command(self, text: str):
        try:
            if text == "/clear":
                self.action_clear_log()
                return

            if text == "/recv":
                payload = self.client.recv_payload()
                self._log("RX", payload.hex(" "))
                return

            if text.startswith("/recvstruct "):
                fmt = text.split(maxsplit=1)[1]
                values = self.client.recv_struct(fmt)
                self._log("RX", f"struct fmt={fmt} values={values}")
                return

            if text.startswith("/hex "):
                payload = parse_hex_bytes(text[len("/hex "):])
                self.client.send_payload(payload)
                self._log("TX", payload.hex(" "))
                return

            if text.startswith("/text "):
                arg = text[len("/text "):]
                payload = arg.encode("utf-8")
                self.client.send_payload(payload)
                self._log("TX", f"text {arg!r}")
                return

            if text.startswith("/struct "):
                tail = text[len("/struct "):].strip()
                fmt, rest = tail.split(maxsplit=1)
                values = parse_python_values(rest)
                payload = struct.pack(fmt, *values)
                self.client.send_payload(payload)
                self._log("TX", f"struct fmt={fmt} values={values} bytes={payload.hex(' ')}")
                return

            self._log("SYS", "Unknown command. Use /hex, /text, /struct, /recv, /recvstruct, /clear")
        except Exception as exc:
            self._log("ERR", str(exc))

    def _monitor_loop(self):
        while not self.monitor_stop.is_set():
            try:
                if not self.client.connected:
                    time.sleep(0.1)
                    continue
                payload = self.client.recv_payload()
                self.call_from_thread(self._log, "RX", payload.hex(" "))
            except TimeoutError:
                continue
            except Exception as exc:
                self.call_from_thread(self._log, "ERR", f"monitor stopped: {exc}")
                self.call_from_thread(self._set_monitor_state, False)
                break

    def _set_monitor_state(self, enabled: bool):
        self.monitor_enabled = enabled
        self._update_status()

    def _start_monitor(self):
        if self.monitor_thread and self.monitor_thread.is_alive():
            self._log("SYS", "Monitor already running.")
            return
        self.monitor_stop.clear()
        self.monitor_thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self.monitor_thread.start()
        self._set_monitor_state(True)
        self._log("SYS", "Background receive monitor started.")

    def _stop_monitor(self):
        self.monitor_stop.set()
        if self.monitor_thread and self.monitor_thread.is_alive():
            self.monitor_thread.join(timeout=0.5)
        self.monitor_thread = None
        self._set_monitor_state(False)

    def on_unmount(self):
        self._stop_monitor()
        self.client.disconnect()


if __name__ == "__main__":
    SSPTextualApp().run()
