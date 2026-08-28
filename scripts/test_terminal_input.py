#!/usr/bin/env python3
"""Debug test: Kann ein disabled ScrolledText Key-Events empfangen?"""

import tkinter as tk
from tkinter import scrolledtext

root = tk.Tk()
root.title("Terminal Input Debug Test")
root.geometry("400x300")

# Test 1: disabled ScrolledText mit Bindings
text1 = scrolledtext.ScrolledText(root, height=5, state="disabled")
text1.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

def on_key_disabled(event):
    print(f"[DISABLED] Key: {event.keysym} / char: {repr(event.char)}")
    return "break"

text1.bind("<Key>", on_key_disabled)

# Test 2: Focus Button
def set_focus():
    text1.focus_set()
    print("Focus set to text1")

btn = tk.Button(root, text="Set Focus", command=set_focus)
btn.pack(pady=5)

# Test 3: Status
status = tk.Label(root, text="Click 'Set Focus', then type in the black box above")
status.pack(pady=5)

# Auto-focus on start
root.after(100, set_focus)

root.mainloop()
