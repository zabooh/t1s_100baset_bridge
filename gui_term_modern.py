#!/usr/bin/env python3
"""
gui_term.py - modern theme build

The same three-console terminal tool as gui_term.py, restyled with sv-ttk
(Sun Valley), a ttk theme that mimics Windows 11 Fluent. gui_term.py itself
is untouched - AppModern(App) inherits the connection handling, the Screen
model, the Setup dialog and term_ports.json loading unchanged, and only adds
what the theme gets wrong on its own. See bridge_gui_modern.py's docstring
and FALLSTRICKE.md for the underlying sv-ttk quirks this works around
(discovered there first, same fixes applied here):

    python gui_term_modern.py            dark (default)
    python gui_term_modern.py --light    light variant

Needs the extra "sv-ttk" package - see requirements.txt.

One thing specific to this tool, found while porting the fix over: sv-ttk's
one-time idle-task recolor pass only touches widgets that already existed
before the FIRST root.update() after the theme was loaded - a dialog opened
later (Setup > Configure Ports) is unaffected and needs no restoration,
verified directly rather than assumed. Only the three panes built during
App.__init__ need it: each pane's terminal text color - which is the exact
setting Setup > Configure Ports' "Display" section lets the user pick, so
silently overriding it would have undone that feature - and each pane's
connect-state dot (DOT_ON/DOT_OFF), otherwise stuck showing the theme's
plain text color regardless of whether that board is actually connected.

Two more things sv-ttk cannot reach at all, found from a screenshot of the
running tool rather than guessed at up front:

- Each pane's scrollbar is a plain tk.Scrollbar bundled inside
  scrolledtext.ScrolledText (see cpython's tkinter/scrolledtext.py), which
  renders as a native Windows scrollbar and ignores every Tk color option -
  unlike tk.Canvas/tk.Text, sv-ttk cannot recolor it either. Swapped for a
  ttk.Scrollbar bound to the same Text widget.
- The "Setup" menu bar is native tk.Menu with no ttk hook of its own, and on
  Windows its background/foreground options turned out to be pure decoration
  - setting them had no visible effect at all (confirmed on screen, not just
  suspected). _replace_menu_bar() swaps the always-visible strip for a
  ttk.Menubutton in its own bar instead, which sv-ttk does style; the
  dropdown list that appears on click is still a native tk.Menu popup (Tk has
  no ttk-based dropdown menu), so that part may still look native - much less
  noticeable than the strip, which is what was actually reported.
"""

import argparse
import ctypes
import tkinter as tk
from tkinter import ttk

import sv_ttk

import gui_term


class AppModern(gui_term.App):
    """sv-ttk build of App - see the module docstring."""

    def __init__(self, root, config_path, slot_list, font_size=gui_term.DEFAULT_FONT_SIZE,
                 text_color=gui_term.DEFAULT_TEXT_COLOR, columns=False, height=gui_term.HEIGHT):
        # Before super().__init__() so the two buttons in the bar are built with it
        # from the start - see bridge_gui_modern.py's _tighten_button_style for why
        # this is safe to set this early (style-level, not per-widget).
        self._tighten_button_style()
        super().__init__(root, config_path, slot_list, font_size=font_size,
                          text_color=text_color, columns=columns, height=height)
        self.root.title("T1S Bridge Terminals (Modern)")
        # Forces sv-ttk's own idle-task restyle to run NOW, before undoing what it
        # just did to the three panes built above - see the module docstring.
        self.root.update_idletasks()
        self._restore_pane_colors()
        self._replace_pane_scrollbars()
        self._replace_menu_bar()
        self._apply_dark_titlebar(self.root, dark=self._is_dark())

    @staticmethod
    def _is_dark() -> bool:
        return ttk.Style().theme_use() == "sun-valley-dark"

    @staticmethod
    def _tighten_button_style() -> None:
        """Same sv-ttk TButton padding/font adjustment as bridge_gui_modern.py -
        this tool's 2-button bar was never at risk of overflowing on its own, this
        is purely so the two "modern" builds look consistent with each other."""
        style = ttk.Style()
        style.configure("TButton", padding=(4, 2), font=("Segoe UI", 9))

    def _restore_pane_colors(self) -> None:
        """See the module docstring for why only these two, and why only the
        three startup panes need it at all."""
        for key, pane in self.panes.items():
            pane.text.configure(foreground=self.text_color)
            pane.dot.configure(
                foreground=gui_term.DOT_ON if self.connected(key) else gui_term.DOT_OFF)

    def _replace_pane_scrollbars(self) -> None:
        """Each pane's scrolledtext.ScrolledText bundles its own plain
        tk.Scrollbar (see cpython's tkinter/scrolledtext.py - it builds one in
        its own __init__ and keeps it as .vbar). On Windows that renders as a
        native scrollbar control and ignores every Tk color option - unlike
        tk.Canvas/tk.Text, sv-ttk cannot recolor it either, confirmed by it
        staying visibly white after everything else went dark. Swapped for a
        ttk.Scrollbar bound to the same Text widget, which sv-ttk does reach.
        """
        for pane in self.panes.values():
            old = pane.text.vbar
            frame = old.master
            old.destroy()
            pane.text.pack_forget()
            new = ttk.Scrollbar(frame, orient="vertical", command=pane.text.yview)
            new.pack(side="right", fill="y")
            pane.text.configure(yscrollcommand=new.set)
            pane.text.pack(side="left", fill="both", expand=True)
            pane.text.vbar = new

    def _replace_menu_bar(self) -> None:
        """The native Windows menu strip ignores tk.Menu's color options
        entirely - confirmed on screen: background/foreground set on it and on
        the "Setup" submenu had no visible effect at all. Not a color-choice
        bug, a Tk-on-Windows limitation with no config-option workaround; the
        strip is drawn by the OS's own themed menu bar renderer, which Tk's
        Menu widget does not control.

        Replaced the always-visible strip with a ttk.Menubutton in its own
        bar, which sv-ttk does style like any other ttk widget. The dropdown
        list that appears on click is still a native tk.Menu popup (unavoidable
        - Tk has no ttk-based dropdown menu), so it may still show with native
        styling; it is far less prominent than the strip that is visible the
        entire time the window is open, which is what this actually fixes.
        """
        self.root.config(menu="")
        bar = ttk.Frame(self.root)
        bar.pack(side="top", fill="x", before=self.all_button.master)
        button = ttk.Menubutton(bar, text="Setup")
        button.pack(side="left", padx=4, pady=2)
        menu = tk.Menu(button, tearoff=0)
        menu.add_command(label="Configure Ports...", command=self.open_config_dialog)
        button["menu"] = menu

    @staticmethod
    def _apply_dark_titlebar(window, dark: bool) -> None:
        """Color a native Windows title bar to match - identical technique to
        bridge_gui_modern.py's version (ctypes + DWM, forced SWP_FRAMECHANGED
        repaint - setting the attribute alone measurably succeeds but does not
        visibly repaint the title bar on its own). Duplicated rather than shared,
        since both GUI tools in this repo are each meant to stay launchable
        standalone. Takes `window` rather than always using self.root because the
        Setup dialog is its own top-level window with its own title bar.
        """
        try:
            hwnd = ctypes.windll.user32.GetParent(window.winfo_id())
            value = ctypes.c_int(1 if dark else 0)
            for attribute in (20, 19):
                ctypes.windll.dwmapi.DwmSetWindowAttribute(
                    hwnd, attribute, ctypes.byref(value), ctypes.sizeof(value))
            SWP_NOMOVE, SWP_NOSIZE, SWP_NOZORDER, SWP_FRAMECHANGED = 0x2, 0x1, 0x4, 0x20
            ctypes.windll.user32.SetWindowPos(
                hwnd, None, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED)
        except OSError:
            pass  # Windows build without this DWM attribute - title bar stays light

    def open_config_dialog(self):
        """Setup > Configure Ports opens well after startup, past sv-ttk's one-time
        recolor pass (see the module docstring) - its ttk widgets pick up the theme
        normally and its raw tk widgets (the color swatch) keep whatever color they
        were given, no restoration needed. Only the dialog's own native title bar
        needs the same treatment as the main window - it is a separate top-level
        window with a title bar of its own."""
        dlg = gui_term.ConfigDialog(self)
        dlg.update_idletasks()
        self._apply_dark_titlebar(dlg, dark=self._is_dark())
        return dlg


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=gui_term.CONFIG,
                    help="port assignment file (default: term_ports.json in this folder)")
    ap.add_argument("--connect", action="store_true",
                    help="open all three ports right at startup (default: no)")
    ap.add_argument("--columns", action="store_true",
                    help="panes side by side instead of stacked")
    ap.add_argument("--font-size", type=int, default=None,
                    help="override the font size from term_ports.json for this run only")
    ap.add_argument("--height", type=int, default=gui_term.HEIGHT,
                    help="lines per pane at startup")
    ap.add_argument("--light", action="store_true", help="use the light variant instead of dark")
    args = ap.parse_args()

    if gui_term.tk is None:
        print("Tkinter is missing from this Python.")
        return 2

    slot_list, font_size, text_color = gui_term.load_config(args.config)
    if args.font_size is not None:
        font_size = args.font_size

    root = tk.Tk()
    sv_ttk.set_theme("light" if args.light else "dark")
    app = AppModern(root, args.config, slot_list, font_size=font_size, text_color=text_color,
                     columns=args.columns, height=args.height)
    if args.connect:
        app.connect(list(app.panes))
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
