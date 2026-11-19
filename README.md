# ludus

A C99 Wayland game launcher. Browse a grid of portrait covers on a black background. The bottom
bar shows the selection and the latest launch or exit. Use `h` and `l` within a row, `j` and `k` between rows,
`Return` to run the selected entry, and `q` to quit.

## Build

Install a C compiler, make, pkg-config, wayland-scanner, wayland-protocols, and development
packages for wayland-client, xkbcommon, Fontconfig, and FreeType. Run `make`,
then start `./ludus` inside a Wayland session. Edit `config.h` and rerun
`make` to change cover width, spacing, colors, keys, or the font. FreeType draws the text and
Fontconfig resolves `FONT_PATTERN` to an installed font. The default is
`"Hack,monospace"`. To use another family, set it to a pattern such as
`"DejaVu Sans:style=Book"`. Use `fc-match 'Hack,monospace'` to see which
font your system selects.

## Add or edit a game

Press `+` to add a game or `e` to edit the selected game. Enter saves the
entry and Escape discards the edits. Tab and Shift+Tab switch fields. Use
Left, Right, Home, and End to move the text cursor, and Backspace or Delete
to remove a character.

Enter a name and an absolute JPEG or PNG artwork path. For a direct launch,
enter the game's absolute executable path. The runner starts blank. Set it
to a program on PATH or an absolute path to run the executable with that
program, for example `wine`, `umu-run`, or `/bin/sh`. Set WINEPREFIX or
PROTONPATH to pass those environment variables to the game.

The optional **Launch command** runs through `/bin/sh -c`. Use `"$@"` to
insert the configured runner (if present) and executable as separate arguments.
For example, `exec "$@" -novid` adds a game option and
`exec gamemoderun "$@"` wraps the launch. You can also leave the executable
and runner empty and enter a complete command such as `exec steam -applaunch 620`.

Games are saved individually under `$XDG_DATA_HOME/ludus/games/`, which
defaults to `~/.local/share/ludus/games/`. The bottom bar follows the
launched process: it shows “Playing” at launch and “Stopped” when that
process exits. A runner that detaches from its game can exit earlier.

## Command-line entries

Pass a cover followed by an optional executable and its arguments. Separate
entries with a standalone `--`. For example, using your own cover paths:

```sh
./ludus 'covers/Portal 2.jpg' steam -applaunch 620 -- 'covers/DOOM.png' gzdoom
```

For a cover-only grid, run `./ludus first.jpg -- second.png`. Command-line entries appear
after your saved library, keep their argument order, and last for this session unless you save them with `e`. Commands launch directly with their argument
boundaries intact and inherit the launcher's environment and working directory.

The bundled stb_image decoder reads JPEG and PNG files as they come into view.
The grid center-crops each image to a 2:3 thumbnail. A 600×900 cover fits
without cropping. An unreadable cover produces a placeholder and a diagnostic
on standard error.

`vendor/stb_image.h` comes from stb revision `5c20573` and includes its license.
