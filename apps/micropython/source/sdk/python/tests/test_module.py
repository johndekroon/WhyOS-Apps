"""The whyos module in MicroPython: Python apps on this computer under sbsim (whyos_testing.PyApp),
the same checks as the C apps get.

    python3 -m pytest sdk/python/tests       (builds MicroPython once: sdk/tools/micropython-sbsim.sh)
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "testing"))
from whyos_testing import PyApp, RED, WHITE  # noqa: E402

EXAMPLES = os.path.join(HERE, "..", "examples")


def example(name, tmp, **kw):
    return PyApp(os.path.join(EXAMPLES, name + ".py"), name, tmp=tmp, **kw)


def test_the_flashlight_in_python(tmp_path):
    app = example("flashlight", tmp_path)
    s = app.screen
    assert s["kind"] == "colour" and s["keybar"][0] == ["#1", "White 100%"] and app.backlight is None
    app.advance(2).press("✕")
    assert app.screen["colour"] == "#ff0000" and app.backlight == 80 and app.settings == {"mode": "2"}
    assert app.pixels == bytes(RED) * (720 * 720)
    app.press("Right", "Right")
    assert app.screen["colour"] == "#ffffff" and app.backlight == 30
    app.press("◆")
    keys = app.screen["popup"]["keys"]
    assert ["#3", "Red at full brightness"] in keys and ["↑↓←→", "Step through the modes"] in keys
    app.press("Esc", "Esc")
    assert app.closed and app.report["status"] == 0
    app.check()


def test_the_python_flashlight_keeps_its_mode(tmp_path):
    again = example("flashlight", tmp_path, settings={"mode": "2"})
    assert again.screen["colour"] == "#ff0000"
    assert again.advance(600).report["touches"] > 100


def test_a_list_an_entry_and_settings(tmp_path):
    app = example("notes", tmp_path)
    s = app.screen
    assert s["kind"] == "list" and [r["label"] for r in s["rows"]] == ["Shopping", "Ideas"]
    app.press("Enter")
    assert app.screen["kind"] == "entry" and app.screen["title"] == "Shopping"
    app.type("milk").press("Enter")
    assert app.screen["history"] == [["milk", "4 letters"]] and app.screen["log"] == ["Shopping: milk"]
    app.press("Esc")
    s = app.screen
    assert s["kind"] == "list" and s["rows"][0]["value"] == "1"
    app.press("Down", "■", "Down", "Enter")
    p = app.screen["popup"]
    assert p["kind"] == "ask" and p["items"] == ["Keep", "Clear"] and p["focus"] == 0
    app.press("Down", "Enter")
    assert app.screen["rows"][1]["value"] == "0" and app.screen["log"][-1] == "Cleared Ideas"
    app.press("■", "Enter", "Backspace", "Backspace", "Backspace", "Backspace", "Backspace", "Enter")
    assert app.screen["popup"]["error"] == "Another name, please"
    app.type("Plans").press("Enter")
    assert app.screen["rows"][1]["label"] == "Plans" and app.screen["message"] == "Renamed"
    app.press("♣", "Enter")
    assert app.settings["upper"] == "1"
    app.press("Esc", "Up", "Enter").type("eggs").press("Enter")
    assert app.screen["log"][-1] == "Shopping: EGGS"
    app.check()


def test_an_error_in_python_is_shown_or_logged_and_the_app_goes_on(tmp_path):
    app = example("notes", tmp_path)
    app.press("▲")                          # an action that raises a TypeError: in the log, the app goes on
    assert not app.closed and "TypeError" in app.report["stderr"]
    app.press("Enter").type(" ").press("Enter")    # evaluate raises ValueError: its message is the error
    assert app.screen["error"] == "Type something first" and app.screen["history"] == []
    app.press("Esc", "Esc", "Esc")
    assert app.closed and app.report["status"] == 0


def test_a_form_in_python(tmp_path):
    script = tmp_path / "form.py"
    script.write_text('''import whyos
form = whyos.List(title="New", fields=[("n.name", "Name", whyos.TEXT, "x"), ("n.big", "Big", whyos.SWITCH, "0")])
home = whyos.List(rows=["Make one"], choose=lambda i: whyos.push(form))
whyos.run("Forms", home)
''')
    app = PyApp(str(script), "forms", tmp=tmp_path)
    app.press("Enter", "Down", "Enter", "Up", "Enter").type("y").press("Enter")
    assert app.screen["rows"] == [{"label": "Name", "value": "xy"}, {"label": "Big", "on": 1}]
    assert app.settings == {"n.big": "1", "n.name": "xy"}
    app.check()


def test_an_editor_in_python(tmp_path):
    script = tmp_path / "ed.py"
    script.write_text('''import whyos
kept = ["one"]
def save():
    kept[0] = ed.text
    ed.dirty = False
    whyos.log("saved " + repr(ed.text))
    whyos.pop()
def open_it(i):
    ed.text = kept[0]
    whyos.push(ed)
ed = whyos.Editor(title="Note", max=200, text="")
ed.action(whyos.MAIN, "save", "Save it", save)
home = whyos.List(rows=["Note"], choose=open_it)
whyos.run("Notes", home)
''')
    app = PyApp(str(script), "ednotes", tmp=tmp_path)
    app.press("Enter")
    assert app.screen["kind"] == "editor" and app.screen["text"] == "one"
    app.press("Enter").type("two").press("▲")
    assert app.screen["log"] == ["saved 'one\\ntwo'"] and app.screen["kind"] == "list"
    app.press("Enter")
    assert app.screen["text"] == "one\ntwo"
    app.check()


def test_rows_that_are_not_text_and_many_screens_with_actions(tmp_path):
    script = tmp_path / "rows.py"
    script.write_text('''import whyos
def detail(i):
    d = whyos.List(title="Detail", rows=["one"])
    d.action(whyos.MAIN, "go", "Go", lambda: whyos.log("go"))
    d.action(whyos.SECOND, "back", "Back", whyos.pop)
    whyos.push(d)
home = whyos.List(rows=[("Temp", 21), ("Hum", 41.5, None), 7], choose=detail)
whyos.run("Rows", home)
''')
    app = PyApp(str(script), "rows", tmp=tmp_path)
    s = app.screen
    assert [(r["label"], r.get("value")) for r in s["rows"]] == [("Temp", "21"), ("Hum", "41.5"), ("7", None)]
    for i in range(70):                     # 140 actions made in all: no table runs out
        app.press("Enter", "✕")
    app.press("Enter", "▲")
    assert app.screen["title"] == "Detail" and app.screen["log"] == ["go"]
    assert "Error" not in app.report["stderr"], app.report["stderr"]
