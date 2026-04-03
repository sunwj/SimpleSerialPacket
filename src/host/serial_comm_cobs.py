import sys
import serial, struct
from serial.tools import list_ports
import threading
from PyQt6.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout,
    QTextEdit, QLineEdit, QPushButton, QMessageBox, QLabel, QComboBox
)
from PyQt6.QtCore import pyqtSignal, QObject

import packet

# ========== Signal Bridge ==========
class SerialSignalBridge(QObject):
    data_received = pyqtSignal(str)

# ========== Main GUI Class ==========
class SerialGUI(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Serial Terminal (PyQt6)")
        self.resize(600, 400)

        self.serial = None
        self.read_thread = None
        self.running = False
        self.signals = SerialSignalBridge()
        self.signals.data_received.connect(self.update_display)

        self.init_ui()

    def init_ui(self):
        layout = QVBoxLayout()

        # Serial Port Settings
        self.port_label = QLabel("Serial Port:")
        self.port_box = QComboBox()
        ports = [port.device for port in list_ports.comports()]
        ports.reverse()
        self.port_box.addItems(ports)  # You can scan dynamically if needed

        self.baud_label = QLabel("Baud Rate:")
        self.baud_box = QComboBox()
        self.baud_box.addItems(["115200"])

        settings_layout = QHBoxLayout()
        settings_layout.addWidget(self.port_label)
        settings_layout.addWidget(self.port_box)
        settings_layout.addWidget(self.baud_label)
        settings_layout.addWidget(self.baud_box)

        # Output Display
        self.text_display = QTextEdit()
        self.text_display.setReadOnly(True)
        self.text_display.document().setMaximumBlockCount(1000)

        # Input Field
        self.input_line = QLineEdit()
        self.input_line.setPlaceholderText("Type your message here...")
        self.input_line.returnPressed.connect(self.send_data)

        # Buttons
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self.toggle_connection)

        button_layout = QHBoxLayout()
        button_layout.addWidget(self.connect_button)

        layout.addLayout(settings_layout)
        layout.addWidget(self.text_display)
        layout.addWidget(self.input_line)
        layout.addLayout(button_layout)

        self.setLayout(layout)

    def toggle_connection(self):
        if self.serial and self.serial.is_open:
            self.disconnect_serial()
        else:
            self.connect_serial()

    def connect_serial(self):
        port = self.port_box.currentText()
        baud = int(self.baud_box.currentText())
        try:
            self.serial = serial.Serial(port, baud, timeout=1)
            self.running = True
            self.read_thread = threading.Thread(target=self.read_serial, daemon=True)
            self.read_thread.start()
            self.connect_button.setText("Disconnect")
            self.log(f"[INFO] Connected to {port} at {baud} baud.")
        except serial.SerialException as e:
            QMessageBox.critical(self, "Connection Error", f"Could not open serial port:\n{e}")

    def disconnect_serial(self):
        self.running = False
        if self.serial and self.serial.is_open:
            self.serial.close()
            self.log("[INFO] Disconnected.")
        self.connect_button.setText("Connect") 

    def read_serial(self):
        buffer = bytearray()
        while self.running:
            try:
                while True:
                    byte = self.serial.read(1)
                    if not byte: break
                    if byte == b'\x00':  # End of packet
                        result = packet.decode_packet_cobs(buffer)
                        if result:
                            packet_id, response_payload = result
                            if packet_id == 10:
                                a, b, c = struct.unpack('<fBB', response_payload)
                                self.signals.data_received.emit(f"Received packet ID: {packet_id}, Payload: {a}, {b} {c}")
                        else:
                            self.signals.data_received.emit("Failed to decode response.")
                        buffer.clear()
                    else:
                        buffer.extend(byte)
            except Exception as e:
                self.signals.data_received.emit(f"[ERROR] {e}")
                self.running = False

    def send_data(self):
        if self.serial and self.serial.is_open:
            text = self.input_line.text()
            if text:
                try:
                    text = text.split(' ')
                    text[0] = int(text[0])
                    if text[0] > -1:
                        text[1] = float(text[1])
                        text[2] = int(text[2])
                        text[3] = int(text[3])

                        codes = packet.build_packet_cobs(text[0], struct.pack('<fBB', text[1], text[2], text[3]))
                        self.serial.write(codes)

                    self.log(f"[SENT] {text}")
                    self.input_line.clear()
                except Exception as e:
                    self.log(f"[ERROR] Send failed: {e}")
        else:
            QMessageBox.warning(self, "Not Connected", "Please connect to a serial port first.")

    def update_display(self, message):
        self.text_display.append(f"[RECEIVED] {message}")

    def log(self, message):
        self.text_display.append(message)

    def closeEvent(self, event):
        self.running = False
        if self.serial and self.serial.is_open:
            self.serial.close()
        event.accept()

# ========== Run Application ==========
if __name__ == "__main__":
    app = QApplication(sys.argv)
    gui = SerialGUI()
    gui.show()
    sys.exit(app.exec())

