"""The sensor panel, its badge code on this computer under sbsim (whyos.testing), with the BME680
and BMI270 as files in a made-up sysfs.

    python3 -m pytest sdapps/sensorpanel/tests [--shots DIR: see whyos-sdk shots]
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "sdk", "testing"))
from whyos_testing import App  # noqa: E402

SRC = [os.path.join(HERE, "..", "sensorpanel.c")]
IIO = "/sys/bus/iio/devices/iio:device%d/"

SENSORS = {
    # the BMI270 first: the panel finds them by name, not by number
    IIO % 0 + "name": "bmi270\n",
    IIO % 0 + "in_accel_scale": "0.000598624\n",
    IIO % 0 + "in_accel_x_raw": "120\n",
    IIO % 0 + "in_accel_y_raw": "-16400\n",
    IIO % 0 + "in_accel_z_raw": "300\n",
    IIO % 1 + "name": "bme680\n",
    IIO % 1 + "in_temp_input": "22030\n",
    IIO % 1 + "in_humidityrelative_input": "41.870000000\n",
    IIO % 1 + "in_pressure_input": "235.12000000\n",
}


def card(app, x, y):
    """Is there more than the card's flat colour around (x, y): text drawn there?"""
    px = {app.pixel(x + dx, y + dy) for dx in range(0, 120, 3) for dy in range(0, 30, 3)}
    return len(px) > 3


@pytest.fixture
def app(tmp_path):
    return App(SRC, "sensorpanel", tmp=tmp_path, files=SENSORS)


def test_the_readings_and_the_graph(app):
    app.advance(8)
    s = app.screen
    assert s["kind"] == "canvas" and s["title"] == "Sensor panel" and s["keybar"] == [["Esc", "close"]]
    r = app.report
    assert r["refused"] == [] and r["fb_writes"] > 20     # drawn three times a second
    app.check()


def test_without_sensors_it_says_so(tmp_path):
    with_ = App(SRC, "sensorpanel", tmp=tmp_path / "with", files=SENSORS).advance(2)
    without = App(SRC, "sensorpanel", tmp=tmp_path / "without").advance(2)
    # the values' row of the first card differs: a number there, a dash and "no BME680" here
    assert with_.pixels != without.pixels
    assert without.report["refused"] == []
    without.check()


def test_the_help_and_esc(app):
    app.press("◆")
    p = app.screen["popup"]
    assert p["title"] == "Sensor panel" and "BME680" in p["about"]
    app.press("Esc")
    assert app.screen["popup"] is None and not app.closed
    app.press("Esc")
    assert app.closed


def test_ten_minutes(app):
    """Three samples a second for ten minutes: no access outside its places, memory steady."""
    app.advance(600)
    r = app.report
    assert r["refused"] == [] and r["status"] == 0 and r["heap_peak"] < 512 * 1024
