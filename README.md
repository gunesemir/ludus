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

The default grid uses 216×324 covers with up to three lines of title below each
one. Longer titles end with an ellipsis. The selection bar shows the full name
when it fits. The header defaults to 32 px and the status bar to 16 px.

## Add or edit a game

Press `+` to add a game or `e` to edit the selected game. Enter saves the
entry and Escape discards the edits. Tab and Shift+Tab switch fields. Use
Left, Right, Home, and End to move the text cursor, and Backspace or Delete
to remove a character. Ctrl+V or Shift+Insert pastes the first line of the
Wayland clipboard at the cursor. The same bindings work in title search.

Press `/` in the grid and type part of a title. Selection moves to the first
case-insensitive match starting at the current game. Enter keeps the selection;
Escape restores the original one. Use `n` and `N` for the next and previous
matches, wrapping around the library.

Enter a name. Artwork is optional and takes an absolute JPEG or PNG path. For a direct launch,
enter the game's absolute executable path. The runner starts blank. Set it
to a program on PATH or an absolute path to run the executable with that
program, for example `wine`, `umu-run`, or `/bin/sh`.

**Working directory** (`workdir`) takes an absolute directory path. Ludus starts
the runner, wrappers, and launch command in that directory, so relative game
assets and arguments resolve there. Leave it empty to inherit Ludus's directory.

**Prefix** sets `WINEPREFIX`. **Wine/Proton version** sets `PROTONPATH` for
runners such as umu-run, which accepts a Proton path or version name. To
select a particular standalone Wine build, put its Wine binary in **Runner**.

The optional **Launch command** runs through `/bin/sh -c`. Use `"$@"` to
insert the configured runner (if present) and executable as separate arguments.
For example, `exec "$@" -novid` adds a game option and
`exec gamemoderun "$@"` wraps the launch. You can also leave the executable
and runner empty and enter a complete command such as `exec steam -applaunch 620`.

Examples for the `cmd` field (one per game):

```ini
cmd=GAMEID=umu-starcitizen STORE=gog UMU_LOG=debug exec "$@"
cmd=WINEDLLOVERRIDES="dinput8=n,b" WINEDEBUG=+loaddll exec "$@" -novid
cmd=exec mangohud "$@"
cmd=exec gamescope -f -- "$@" -novid
cmd=exec gamescope --mangoapp -f -- "$@"
cmd=exec umu-run --config "./game settings.toml"
```

The last example supplies a complete command and uses `workdir` to resolve
the relative TOML path. Install the tools used by your command. For the
Gamescope overlay example, install mangoapp as well.

Each game has a plain-text file named after its title, such as
`~/.ludus/Black Mesa.meta`. Renaming the game in the editor renames its file.
Titles must have distinct filenames. Spaces and Unicode are preserved. In
filenames, `/`, `%`, and a leading dot are encoded as `%2F`, `%25`, and `%2E`.

Metadata is UTF-8 with one `key=value` per line, in any order. Start a line
with `#` for a comment. Values are literal text, including spaces and any
additional equals signs. Only `cmd` is interpreted by a shell at launch.
Specify `name` and either `exe` or `cmd`. Other keys can be blank or omitted.
Here is an example using all eight keys:

```ini
name=Black Mesa
exe=/games/Black Mesa/bms.exe
workdir=/games/Black Mesa
art=/pictures/Black Mesa.jpg
runner=umu-run
WINEPREFIX=/games/prefixes/black-mesa
PROTONPATH=GE-Proton
cmd=
```

The bottom bar follows the
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
without cropping. Thumbnails are cached in `~/.ludus/cache/` and reused across
launches. Changing the source image or cover size causes regeneration.
Offscreen thumbnails are released from memory. You can delete the cache and
Ludus will rebuild it from your originals. An empty or unreadable artwork
path shows a placeholder.

`vendor/stb_image.h` comes from stb revision `5c20573` and includes its license.
