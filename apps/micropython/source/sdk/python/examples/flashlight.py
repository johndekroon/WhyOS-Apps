# The flashlight in Python on the WhyOS SDK: the whole screen as a light.
import whyos

MODES = [(0xFFFFFF, 80), (0xFFFFFF, 30), (0xFF0000, 80)]
light = whyos.Colour(keys_ms=2000)


def show(mode):
    mode %= 3
    whyos.set("mode", mode)
    light.colour, light.brightness = MODES[mode]


light.action(whyos.OPTIONS, "White 100%", "White at full brightness", lambda: show(0))
light.action(whyos.MAIN, "White 50%", "White at half brightness", lambda: show(1))
light.action(whyos.SECOND, "Red 100%", "Red at full brightness", lambda: show(2))
light.action(whyos.ARROWS, None, "Step through the modes", lambda step: show(int(whyos.get("mode", 0)) + step))
show(int(whyos.get("mode", 0)))
whyos.run("Flashlight", light, keep_on=True,
          about="The whole screen as a light. It stays on until you close it, and the mode you chose comes back next time.")
