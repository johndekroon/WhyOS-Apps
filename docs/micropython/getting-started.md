# Getting started

## 1. Install MicroPython

MicroPython comes from the app store and needs an SD card in the badge and
a Wi-Fi connection for the download.

1. Apps > App store, category Tools, MicroPython, Install.
2. It installs on the SD card. It also adds the commands `python` and
   `micropython` for the badge's shell.

## 2. Try the prompt

Start Apps > MicroPython. You get the interactive prompt (the REPL) in a
text window:

```
MicroPython v1.29.0 on ...; linux [GCC 13.4.0] version
Type "help()" for more information.
>>> 1 + 1
2
>>> import sys; sys.platform
'linux'
```

| Key | What it does |
|---|---|
| Enter | Run the line, or continue a block (`...`) |
| Tab | Complete a name; when several match, Tab lists them |
| ↑ ↓ | Earlier lines |
| Ctrl-C | Stop a running program |
| Ctrl-E | Paste mode: paste a block with its own indentation, Ctrl-D runs it |
| Ctrl-D | Leave MicroPython (on an empty line) |
| Fn+↑ Fn+↓ | Scroll back through earlier output (in the window from the menu) |
| Logo key | Close the window |

In a block the prompt indents the next line by itself; Backspace removes
that indent, and Enter on an otherwise empty line ends the block.

The same prompt is in the badge's shell: open ● Terminal on the home
screen and type `python`.

## 3. Your first script

Your scripts belong in the folder **`badge/appdata/micropython`** on the SD
card (`/mnt/sd/badge/appdata/micropython` on the badge). The prompt from
the menu starts there, so `import hello` there runs `hello.py`.

### Write it on the badge

Write, the badge's text editor (Apps > Write), can edit and run Python:

1. Ctrl+N for a new document.
2. ■ Options > Language > Python. Write now shows code with colours.
3. Type the program:

   ```python
   name = input("What is your name? ")
   for i in range(3):
       print("Hello", name, "from the badge!")
   ```

4. Ctrl+S, type the name `hello`; Write adds `.py`. Press ↓ to choose the
   folder: SD card > `badge` > `appdata` > `micropython`. Without that
   step the file goes to `/root`, in the badge's small flash memory.
5. ▲ runs it: Write saves it first and runs it in a text window. The
   window stays open when the program ends, so you can read the output;
   Enter or Esc takes you back to Write.

### Run a script from Files

In Files (✕ on the home screen), select a `.py` file and press ■ > Open
with > MicroPython. The program runs; afterwards the prompt stays open with
everything the program defined, handy for trying things out. Ctrl-D closes
it. Open with > Write edits the file.

### Run a script from the shell

```
python /mnt/sd/badge/appdata/micropython/hello.py
python hello.py arg1 arg2        # sys.argv has the arguments
python -i hello.py               # the prompt after the script
python -c 'print(2 ** 100)'
```

## 4. Work from your computer

Typing a long program on the badge's keyboard is slow. Write it on your
computer and copy it over.

### Over Wi-Fi with SSH (recommended)

Once:

1. Connect the badge to Wi-Fi (Settings > Connection > Wi-Fi). The Wi-Fi
   screen shows the badge's address.
2. Give `root` a password: ● Terminal, then `passwd`. Or put your public
   `ssh-ed25519` key in `/root/.ssh/authorized_keys`.

Then from your computer:

```
scp hello.py root@BADGE:/mnt/sd/badge/appdata/micropython/
ssh root@BADGE python /mnt/sd/badge/appdata/micropython/hello.py
ssh -t root@BADGE python          # the prompt, in your own terminal
```

The output appears in your terminal on the computer. A program that draws
on the badge's own window (colours, cursor) also works this way, in your
terminal instead.

### With a USB cable

Unplug the side port and connect the computer to the **bottom** port. The
badge asks what to do: choose "Share the SD card as a disk" (more in the
WhyBadge documentation, chapter 8.6).

- On a card formatted on a computer, the whole card appears: copy your
  scripts to `badge/appdata/micropython/`.
- On a card formatted by the badge (Settings > Storage & devices > Disks >
  Format for the badge), the computer sees only a small FAT part of the
  card, not the folder with your scripts. Copy the script there, then on
  the badge use Files to copy it from that part (Drives) to
  `badge/appdata/micropython`.

Eject the disk on the computer before you unplug the cable.

## 5. Next

- [programming.md](programming.md): input and output, colours, keys,
  saving data, errors, your own menu tile.
- [badge.md](badge.md): sensors, LEDs, the internet, typing on a computer.
