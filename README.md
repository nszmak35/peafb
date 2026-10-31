# peafb

**Peaclock-like Femboys**: boykisser ASCII art in the middle of your terminal.

Six pictures, switchable with the arrow keys or number keys. The art is redrawn when the terminal is resized. Based on [peaae](https://github.com/nszmak35/peaae).

## Build & install

```sh
git clone https://github.com/nszmak35/peafb.git
cd peafb
make
sudo make install                   # or: make install PREFIX=$HOME/.local
```

Requires a C99 compiler and `make`. Needs a terminal with 256-color support.

## Usage

```
peafb [-n NUM] [-a ASPECT] [-H]
peafb -o [-n NUM] [-C]
```

| Option      | Description                                      |
|-------------|--------------------------------------------------|
| `-n NUM`    | Start with picture NUM (1–6)                     |
| `-a ASPECT` | Cell height/width ratio, 1.0–4.0 (default: auto) |
| `-H`        | Hide the status line                             |
| `-o`        | Print the picture once and exit                  |
| `-C`        | With `-o`: use colors even when not a TTY        |
| `-h` / `-V` | Show help / version                              |

Keys: `←`/`→` (or `h`/`l`) switch pictures, `1`–`6` pick one, `q`/`Esc` quit.
