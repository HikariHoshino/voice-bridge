"""Measure the unchanged Tk baseline's requested geometry without starting its service."""
import json
import sys
import tkinter as tk
from pathlib import Path

project = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(project))
import app

app.enable_dpi_awareness()
root = tk.Tk()
root.withdraw()
root.tk.call("tk", "scaling", root.winfo_fpixels("1i") / 72.0)
baseline = object.__new__(app.BridgeApp)
baseline.root = root
baseline.settings = app.DEFAULT_SETTINGS.copy()
baseline.theme_name = "light"
baseline.palette = app.THEMES["light"].copy()
baseline.rounded_cards = []
baseline.animated_buttons = []
baseline.toggle_switches = []
baseline.url = "http://127.0.0.1"
baseline.closing = False
baseline.window_max_size = root.maxsize()
baseline.settings_window = None
baseline.close_dialog = None
baseline._configure_styles()
baseline._build_ui()
root.update_idletasks()
result = {
    "dpi": root.winfo_fpixels("1i"),
    "window_client": [max(580, baseline.main_frame.winfo_reqwidth() + 24), max(360, baseline.main_frame.winfo_reqheight() + 20)],
    "widgets": {},
}
for name in ("main_frame", "transfer_shell", "transfer_content", "desktop_input_shell", "desktop_text", "status_shell", "theme_button", "transfer_toggle_button", "send_phone_button"):
    widget = getattr(baseline, name)
    result["widgets"][name] = {"requested_width": widget.winfo_reqwidth(), "requested_height": widget.winfo_reqheight()}
result["main_children"] = [
    {"class": widget.winfo_class(), "width": widget.winfo_reqwidth(), "height": widget.winfo_reqheight()}
    for widget in baseline.main_frame.winfo_children()
]
result["font_lines"] = {}
import tkinter.font as tkfont
for size in (9, 10, 11, 22):
    font = tkfont.Font(root, family="Microsoft YaHei UI", size=size, weight="bold" if size in (11, 22) else "normal")
    result["font_lines"][str(size)] = font.metrics("linespace")
baseline._show_centered_window = lambda window: window.update_idletasks()
baseline.open_settings()
result["settings_client"] = [baseline.settings_window.winfo_reqwidth(), baseline.settings_window.winfo_reqheight()]
baseline.toggle_qr()
result["pairing_client"] = [baseline.qr_window.winfo_reqwidth(), baseline.qr_window.winfo_reqheight()]
for name, window in (("settings", baseline.settings_window), ("pairing", baseline.qr_window)):
    result[name + "_children"] = [
        {"class": child.winfo_class(), "height": child.winfo_reqheight(), "width": child.winfo_reqwidth()}
        for container in window.winfo_children() for child in container.winfo_children()
    ]
destination = project / "native/test-output/baseline-metrics.json"
destination.parent.mkdir(exist_ok=True)
destination.write_text(json.dumps(result, indent=2), encoding="utf-8")
root.destroy()
print(json.dumps(result, indent=2))
