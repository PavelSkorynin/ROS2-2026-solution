import argparse
import asyncio
import struct
import threading
import time
import sys
import tkinter as tk

from bleak import BleakClient, BleakScanner, BleakError

SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # Write


async def find_device(address: str | None, name: str | None):
    if address:
        return address

    if not name:
        raise ValueError("Either --address or --name must be provided")

    devices = await BleakScanner.discover(timeout=5.0)
    for device in devices:
        if device.name == name:
            return device.address
    return None


def _compute_command(pressed: set[str]) -> tuple[int, int, int]:
    forward = "w" in pressed
    backward = "s" in pressed
    left = "a" in pressed
    right = "d" in pressed

    if forward and backward:
        forward = backward = False
    if left and right:
        left = right = False

    if forward:
        if left:
            l, r = 32, 64
        elif right:
            l, r = 64, 32
        else:
            l, r = 64, 64
    elif backward:
        if left:
            l, r = -32, -64
        elif right:
            l, r = -64, -32
        else:
            l, r = -64, -64
    elif left:
        l, r = -64, 64
    elif right:
        l, r = 64, -64
    else:
        l, r = 0, 0

    t = 500 if (l != 0 or r != 0) else 0
    return l, r, t


class KeyState:
    def __init__(self) -> None:
        self._pressed: set[str] = set()
        self._last_press_ts: dict[str, float] = {}
        self._lock = threading.Lock()

    def on_press(self, key: str) -> None:
        if key not in {"w", "a", "s", "d"}:
            return
        with self._lock:
            self._pressed.add(key)
            self._last_press_ts[key] = time.monotonic()

    def on_release(self, key: str) -> None:
        if key not in {"w", "a", "s", "d"}:
            return
        with self._lock:
            self._pressed.discard(key)

    def pressed(self) -> set[str]:
        with self._lock:
            return set(self._pressed)

    def last_press_ts(self, key: str) -> float:
        with self._lock:
            return self._last_press_ts.get(key, 0.0)

    def clear(self) -> None:
        with self._lock:
            self._pressed.clear()
            self._last_press_ts.clear()


async def run_ble(
    address: str | None,
    name: str | None,
    interval_s: float,
    debug: bool,
    key_state: KeyState,
    stop_event: threading.Event,
    connection_status: dict[str, str],
):
    try:
        while not stop_event.is_set():
            connection_status["value"] = "Scanning"
            target_address = address or await find_device(address, name)
            if not target_address:
                connection_status["value"] = "Not found"
                print("Device not found. Retrying...")
                await asyncio.sleep(2.0)
                continue

            print(f"Connecting to {target_address}...")
            try:
                client = BleakClient(target_address)
                try:
                    connection_status["value"] = "Connecting"
                    await client.connect()
                    if not client.is_connected:
                        connection_status["value"] = "Connect failed"
                        print("Failed to connect. Retrying...")
                        await asyncio.sleep(2.0)
                        continue

                    connection_status["value"] = "Connected"
                    print("Connected.")
                    last_cmd = None
                    while not stop_event.is_set():
                        pressed = key_state.pressed()
                        l, r, t = _compute_command(pressed)

                        payload = struct.pack("<bbH", l, r, t)
                        try:
                            await client.write_gatt_char(RX_UUID, payload, response=False)
                        except (BleakError, OSError) as exc:
                            print(f"BLE write error: {exc}. Reconnecting...")
                            break
                        if debug and (last_cmd != (l, r, t)):
                            print(f"cmd L={l} R={r} T={t}")
                            last_cmd = (l, r, t)
                        await asyncio.sleep(interval_s)
                finally:
                    if client.is_connected:
                        connection_status["value"] = "Disconnecting"
                        await client.disconnect()
            except (BleakError, OSError) as exc:
                connection_status["value"] = "Error"
                print(f"BLE error: {exc}. Reconnecting...")
                await asyncio.sleep(1.0)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                connection_status["value"] = "Error"
                print(f"Unexpected error: {exc}. Reconnecting...")
                await asyncio.sleep(1.0)
    finally:
        stop_event.set()


def _run_ble_thread(
    address: str | None,
    name: str | None,
    interval_s: float,
    debug: bool,
    key_state: KeyState,
    stop_event: threading.Event,
    connection_status: dict[str, str],
) -> threading.Thread:
    def runner():
        asyncio.run(run_ble(address, name, interval_s, debug, key_state, stop_event, connection_status))

    thread = threading.Thread(target=runner, daemon=True)
    thread.start()
    return thread


def _run_gui(address: str | None, name: str | None, interval_s: float, debug: bool) -> None:
    key_state = KeyState()
    stop_event = threading.Event()
    connection_status = {"value": "Disconnected"}
    ble_thread = _run_ble_thread(address, name, interval_s, debug, key_state, stop_event, connection_status)

    root = tk.Tk()
    root.title("BLE Robot Control (WASD)")
    root.geometry("640x280")
    root.resizable(False, False)

    label = tk.Label(
        root,
        text="Focus this window and use WASD.\n"
             "Close the window to stop.",
        justify="center",
    )
    label.pack(expand=True)

    conn_var = tk.StringVar(value="Status: Disconnected")
    conn_label = tk.Label(root, textvariable=conn_var, font=("TkDefaultFont", 12))
    conn_label.pack()

    status_var = tk.StringVar(value="L=0 R=0 T=0")
    status_label = tk.Label(root, textvariable=status_var, font=("TkDefaultFont", 14, "bold"))
    status_label.pack()

    release_debounce_ms = 80

    if sys.platform.startswith("win"):
        keycode_map = {87: "w", 65: "a", 83: "s", 68: "d"}  # Virtual-Key codes
    elif sys.platform == "darwin":
        keycode_map = {13: "w", 0: "a", 1: "s", 2: "d"}  # macOS keycodes
    else:
        keycode_map = {25: "w", 38: "a", 39: "s", 40: "d"}  # X11 keycodes

    def _key_from_event(event) -> str | None:
        key = keycode_map.get(event.keycode)
        if key is not None:
            return key
        # Fallback if keycode map doesn't match (e.g., Wayland variations)
        keysym = event.keysym.lower()
        if keysym in {"w", "a", "s", "d"}:
            return keysym
        return None

    def on_press(event):
        key = _key_from_event(event)
        if key is not None:
            key_state.on_press(key)

    def on_release(event):
        key = _key_from_event(event)
        if key is None:
            return
        release_ts = time.monotonic()

        def apply_release():
            if key_state.last_press_ts(key) <= release_ts:
                key_state.on_release(key)

        root.after(release_debounce_ms, apply_release)

    root.bind("<KeyPress>", on_press)
    root.bind("<KeyRelease>", on_release)
    root.bind("<FocusOut>", lambda _event: key_state.clear())

    def on_close():
        stop_event.set()
        ble_thread.join(timeout=2.0)
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)

    def update_status():
        pressed = key_state.pressed()
        l, r, t = _compute_command(pressed)
        status_var.set(f"L={l} R={r} T={t}")
        conn_var.set(f"Status: {connection_status['value']}")
        if not stop_event.is_set():
            root.after(50, update_status)

    update_status()
    root.mainloop()


def main():
    parser = argparse.ArgumentParser(description="BLE robot control client")
    parser.add_argument("--address", help="BLE address of esp32dev")
    parser.add_argument("--name", default="esp32dev", help="BLE device name")
    parser.add_argument("--interval", type=float, default=0.4, help="Send interval seconds")
    parser.add_argument("--debug", action="store_true", help="Print key and command events")
    args = parser.parse_args()

    _run_gui(args.address, args.name, args.interval, args.debug)


if __name__ == "__main__":
    main()
