"""The flashlight, its badge code on this computer under sbsim (whyos.testing).

    python3 -m pytest sdapps/flashlight/tests
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "sdk", "testing"))
from whyos_testing import App, WHITE, RED  # noqa: E402

SRC = [os.path.join(HERE, "..", "flashlight.c")]
WHOLE_WHITE = bytes(WHITE) * (720 * 720)


@pytest.fixture
def app(tmp_path):
    return App(SRC, "flashlight", tmp=tmp_path)


def test_the_keys_show_for_two_seconds_at_the_systems_brightness(app):
    s = app.screen
    assert s["kind"] == "colour" and s["colour"] == "#ffffff"
    assert s["keybar"] == [["#1", "White 100%"], ["#2", "White 50%"], ["#3", "Red 100%"], ["Esc", "close"]]
    assert app.backlight is None
    assert app.pixel(10, 10) == WHITE and app.pixel(360, 690) != WHITE
    app.advance(2)
    assert app.screen["keybar"] is None and app.pixels == WHOLE_WHITE
    assert app.backlight == 80
    app.check()


@pytest.mark.parametrize("keys,colour,duty", [
    (["▲"], "#ffffff", 30),
    (["✕"], "#ff0000", 80),
    (["✕", "■"], "#ffffff", 80),
    (["Right"], "#ffffff", 30),
    (["Down", "Down"], "#ff0000", 80),
    (["Right", "Right", "Right"], "#ffffff", 80),
    (["Left"], "#ff0000", 80),
    (["Up", "Up"], "#ffffff", 30),
    (["●", "Esc", "♣", "Esc"], "#ffffff", 80),
])
def test_the_modes(app, keys, colour, duty):
    app.advance(2).press(*keys)
    assert app.screen["colour"] == colour and app.backlight == duty
    app.check()


def test_a_mode_chosen_in_the_first_two_seconds_shows_at_once_and_lights_after(app):
    app.press("✕")
    assert app.screen["colour"] == "#ff0000" and app.backlight is None
    assert app.screen["keybar"] is not None
    app.advance(2)
    assert app.backlight == 80 and app.pixels == bytes(RED) * (720 * 720)


def test_esc_closes_and_fn_esc_is_standbys(app):
    app.advance(2).press("Fn+Esc")
    assert not app.closed
    app.press("Esc")
    assert app.closed and app.report["status"] == 0


def test_the_help_lists_the_modes_and_closes_on_esc_or_the_diamond(app):
    app.advance(2).press("◆")
    p = app.screen["popup"]
    assert p["kind"] == "help" and p["title"] == "Flashlight"
    assert ["#3", "Red at full brightness"] in p["keys"]
    assert ["↑↓←→", "Step through the modes"] in p["keys"]
    assert ["Esc", "Close Flashlight"] in p["keys"]
    assert all(app.pixel(20, y) != WHITE for y in range(4, 720, 40))
    app.press("✕", "Right")         # only an explanation: other keys do nothing
    assert app.screen["popup"] and app.screen["colour"] == "#ffffff"
    app.press("Esc")
    assert app.screen["popup"] is None and not app.closed and app.pixels == WHOLE_WHITE
    app.press("◆", "◆")
    assert app.screen["popup"] is None and app.pixels == WHOLE_WHITE
    app.check()


def test_log_and_settings_open_their_screens(app):
    app.advance(2).press("●")
    assert app.screen["title"] == "Log" and app.screen["depth"] == 2
    app.press("♣")
    assert app.screen["title"] == "Settings" and app.screen["depth"] == 2
    app.press("Esc")
    assert app.screen["kind"] == "colour" and not app.closed
    app.check()


def test_the_mode_comes_back_next_time(app, tmp_path):
    app.advance(2).press("✕")
    assert app.settings == {"mode": "2"}
    again = App(SRC, "flashlight", tmp=tmp_path / "again", settings=app.settings)
    assert again.screen["colour"] == "#ff0000" and again.backlight is None
    assert again.advance(2).backlight == 80


@pytest.mark.parametrize("kept", ["x", "", "7"])
def test_a_kept_mode_that_makes_no_sense_is_a_mode(tmp_path, kept):
    app = App(SRC, "flashlight", tmp=tmp_path, settings={"mode": kept})
    assert app.screen["colour"] in ("#ffffff", "#ff0000")


def test_without_a_place_to_keep_it_it_still_works(app):
    app.env["APP_CONFIG"] = ""
    app.advance(2).press("▲")
    assert app.screen["colour"] == "#ffffff" and app.backlight == 30


def test_ten_minutes_without_a_key_the_screen_stays_on(app):
    app.advance(600)
    r = app.report
    assert r["slept"] == 0 and r["touches"] > 100 and r["refused"] == []


def test_in_standby_no_touch_and_no_light(app):
    app.env["SBSIM_STANDBY"] = "10000-20000"
    app.advance(30)
    r = app.report
    assert r["woken"] == 0 and r["touches_in_standby"] == 0 and r["lights_in_standby"] == 0


def test_standby_in_the_first_two_seconds_puts_the_light_after_it(app):
    app.env["SBSIM_STANDBY"] = "500-5000"
    app.advance(8)
    r = app.report
    assert r["lights_in_standby"] == 0 and r["light"] and r["light"][0][0] >= 5000


def test_the_logo_key_ends_it(app):
    app.env["SBSIM_TERM"] = "3000"
    app.advance(20)
    assert app.report["status"] == 0 and app.report["end_ms"] < 4000


def test_the_simulation_sees_a_refused_touch(tmp_path):
    """The fault of flashlight 1.0.0 still shows here, or the tests above prove nothing: with a
    sandbox policy that does not let apps write /run/active, the screen goes off."""
    sandbox = os.path.join(HERE, "..", "..", "..", "sandbox")
    with open(os.path.join(sandbox, "policy.h")) as f:
        policy = f.read()
    assert '"/run/active", ' in policy and '"/run/standby", ' in policy
    old = tmp_path / "old-policy"
    old.mkdir()
    (old / "policy.h").write_text(policy.replace('"/run/active", ', "").replace('"/run/standby", ', ""))
    app = App(SRC, "flashlight", tmp=tmp_path, flags=["-I" + str(old)])
    app.advance(600)
    assert app.report["slept"] > 0 and ("write", "/run/active") in app.report["refused"]
