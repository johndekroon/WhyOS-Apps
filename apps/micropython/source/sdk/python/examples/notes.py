# Notes in Python on the WhyOS SDK: a list of lists, an entry screen per
# list, a menu on the square key and a setting. sdk/python/README.md.
import whyos

names = ["Shopping", "Ideas"]
notes = {n: [] for n in names}


def rows():
    return [(n, str(len(notes[n]))) for n in names]


def add(line):
    if not line.strip():
        raise ValueError("Type something first")
    if whyos.get("upper") == "1":
        line = line.upper()
    notes[page.title].append(line)
    whyos.log("%s: %s" % (page.title, line))
    return "%d letters" % len(line)


def open_list(i):
    page.title = names[i]
    whyos.push(page)


def clear(i):
    def done(choice):
        if choice == 1:
            notes[names[i]].clear()
            whyos.log("Cleared " + names[i])
    whyos.ask("Clear %s?" % names[i], "Its notes go.", ["Keep", "Clear"], done, danger=1)


def rename(i):
    def done(text):
        if not text or text in names:
            return "Another name, please"
        notes[text] = notes.pop(names[i])
        names[i] = text
        whyos.message("Renamed")
    whyos.ask_text("Rename", "A new name for the list.", names[i], done)


def broken():
    return 1 + "1"


page = whyos.Entry(evaluate=add, placeholder="Type a note")
home = whyos.List(rows=rows(), choose=open_list, menu=[("Rename", rename), ("Clear", clear)])
home.update = lambda: setattr(home, "rows", rows())
home.action(whyos.ENTER, "open", "Open the list")
home.action(whyos.MAIN, "break", "An action that fails", broken)
whyos.run("Notes", home, about="Notes in lists.",
          settings=[("upper", "Capitals", whyos.SWITCH, "0", None, "Notes in capital letters")])
