"""The calculator: its sums on their own (unit/test_sums.c), and the app, its badge code on this
computer under sbsim (whyos.testing).

    python3 -m pytest sdapps/calc/tests
"""
import glob
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "sdk", "testing"))
from whyos_testing import App, unit  # noqa: E402

LOGIC = [os.path.join(HERE, "..", "sums.c")]
SRC = [os.path.join(HERE, "..", "calc.c")] + LOGIC


@pytest.mark.parametrize("test_c", sorted(glob.glob(os.path.join(HERE, "unit", "*.c"))))
def test_unit(test_c, tmp_path):
    unit(test_c, LOGIC, tmp_path)


@pytest.fixture
def app(tmp_path):
    return App(SRC, "calc", tmp=tmp_path)


def roll(app):
    return ["%s = %s" % tuple(h) for h in app.screen["history"]]


@pytest.mark.parametrize("typed,want", [
    ("12*7+3\n", ["12*7+3 = 87"]),
    ("3x4\n0x1f\n", ["3*4 = 12", "0x1f = 31"]),
    ("6\n*7\n+1\n", ["6 = 6", "ans*7 = 42", "ans+1 = 43"]),
    ("2=", ["2 = 2"]),
    ("-5\n", ["-5 = -5"]),
])
def test_sums_typed_go_on_the_roll(app, typed, want):
    for part in typed.split("\n"):
        app.type(part)
        if part != typed.split("\n")[-1] or typed.endswith("\n"):
            app.press("Enter")
    assert roll(app)[:len(want)] == want
    app.check()


def test_the_result_shows_while_typing_and_the_bases_with_it(app):
    app.type("12*7+3")
    s = app.screen
    assert s["line"] == "12*7+3" and s["result"] == "= 87" and s["detail"].startswith("HEX  0x57")
    app.press("Enter")
    s = app.screen
    assert s["line"] == "" and s["result"] == "87" and s["log"] == ["12*7+3 = 87"]


def test_an_error_shows_and_esc_clears_it(app):
    app.type("10/0").press("Enter")
    assert app.screen["error"] == "Division by zero" and app.screen["keybar"][-1] == ["Esc", "clear"]
    app.press("Esc")
    assert app.screen["error"] is None and app.screen["line"] == "" and not app.closed
    app.press("Esc")
    assert app.closed


def test_esc_clears_the_line_then_closes(app):
    app.type("abc").press("Esc")
    assert app.screen["line"] == "" and not app.closed
    app.press("Esc")
    assert app.closed and app.report["status"] == 0


def test_up_and_down_bring_back_earlier_sums(app):
    app.type("1+2").press("Enter").type("3").press("Enter", "Up", "Up")
    assert app.screen["line"] == "1+2"
    app.press("Down")
    assert app.screen["line"] == "3"
    app.press("Down")
    assert app.screen["line"] == ""
    app.press("Up", "Up", "Enter")
    assert roll(app)[-1] == "1+2 = 3"


def test_the_cursor_and_erasing(app):
    app.type("12").press("Backspace", "Backspace").type("(1+2)/3").press("Left", "Left", "Backspace")
    assert app.screen["line"] == "(1+2/3" and app.screen["cursor"] == 4
    app.press("Fn+Left")
    assert app.screen["cursor"] == 0
    app.press("Fn+Backspace")
    assert app.screen["line"] == "1+2/3"
    app.press("Fn+Right")
    assert app.screen["cursor"] == 5
    app.check()


def test_tab_and_the_setting_switch_the_angle_unit(app):
    app.type("sin 90").press("Enter")
    assert roll(app)[-1] == "sin 90 = 1" and app.screen["title_right"] == "DEG"
    app.press("Tab")
    assert app.screen["title_right"] == "RAD" and app.settings["angle"] == "rad"
    app.press("♣")
    s = app.screen
    assert s["title"] == "Settings" and s["rows"][0]["value"] == "Radians"
    app.press("Enter")
    p = app.screen["popup"]
    assert p["kind"] == "choice" and p["items"] == ["Degrees", "Radians"] and p["current"] == 1
    app.press("Up", "Enter")
    assert app.screen["rows"][0]["value"] == "Degrees" and app.settings["angle"] == "deg"
    app.press("Esc").type("sin(pi/2)").press("Enter")
    assert roll(app)[-1] == "sin(pi/2) = 0.0274121335920443"
    app.check()


def test_a_choice_cancelled_keeps_the_setting(app):
    app.press("♣", "Enter", "Down", "Esc")
    assert app.screen["popup"] is None and app.settings.get("angle", "deg") == "deg"


def test_the_angle_unit_comes_back_next_time(app, tmp_path):
    app.press("Tab")
    again = App(SRC, "calc", tmp=tmp_path / "again", settings=app.settings)
    assert again.screen["title_right"] == "RAD"


def test_the_log_has_the_sums_and_esc_goes_back(app):
    app.press("●")
    assert app.screen["title"] == "Log" and app.screen["log"] == []
    app.press("Esc").type("1+2").press("Enter").type("0xff").press("Enter", "●")
    assert app.screen["log"] == ["1+2 = 3", "0xff = 255"]
    app.press("Esc")
    assert app.screen["kind"] == "entry" and not app.closed
    app.check()


def test_the_help_lists_the_keys_and_closing_it_keeps_the_line(app):
    app.type("12").press("◆")
    p = app.screen["popup"]
    assert p["title"] == "Calculator" and ["Tab", "Degrees or radians"] in p["keys"]
    assert ["Esc", "Clear the line; on an empty line, close"] in p["keys"]
    app.press("■", "1")             # only an explanation
    assert app.screen["popup"] and app.screen["line"] == "12"
    app.press("Esc")
    assert app.screen["popup"] is None and app.screen["line"] == "12"
    app.check()


def test_symbol_keys_type_nothing(app):
    app.press("■", "▲", "✕")
    assert app.screen["line"] == "" and app.screen["popup"] is None


def test_the_key_bar(app):
    assert app.screen["keybar"] == [["Enter", "work out"], ["Tab", "deg/rad"], ["Esc", "close"]]
    app.type("1")
    assert app.screen["keybar"][-1] == ["Esc", "clear"]


def test_a_long_line_scrolls_and_fits(app):
    app.type("1+" * 40 + "1")
    assert app.screen["result"] == "= 41"
    app.check()


def test_the_logo_key_ends_it(app):
    app.env["SBSIM_TERM"] = "1000"
    app.advance(5)
    assert app.report["status"] == 0 and app.report["end_ms"] < 2000
