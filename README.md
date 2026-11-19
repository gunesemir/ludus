# ludus

A small Wayland game launcher written in C99. Pick a cover with the vi keys
and press Enter to play.

## Build and run

You'll need a C compiler, make, pkg-config, wayland-scanner, wayland-protocols,
and the development packages for wayland-client, xkbcommon, Fontconfig, and
FreeType. Build and start Ludus from a Wayland session.

```sh
make
./ludus
```

Ludus uses your system's `monospace` font. To change the font, colors, cover
size, or keybindings, edit `config.h` and run `make` again.

## Add a game

Press `+`, give the game a name, and enter the full path to its executable.
For a Windows game, set **Runner** to `wine`, `umu-run`, or the path to your
runner. Leave it blank to launch a native game directly. You can add a JPEG
or PNG cover now or later. A 600×900 image fits the grid.

Use Tab and Shift+Tab to move between fields. Ctrl+V or Shift+Insert pastes
at the cursor. Enter saves your changes and Escape cancels.

| In the library | Key |
| --- | --- |
| Move left, down, up, right | `h`, `j`, `k`, `l` |
| Launch the selected game | Enter |
| Add or edit a game | `+` / `e` |
| Search by title | `/` |
| Next or previous match | `n` / `N` |
| Quit | `q` |

## Game files

You can also edit games by hand. Each one has a file in `~/.ludus/`, with one
`key=value` per line. Only `name` and either `exe` or `cmd` are required.
If the file changes while the game editor is open, Ludus asks you to reopen
the editor before saving. Here’s an example for `~/.ludus/Black Mesa.meta`.

```ini
name=Black Mesa
exe=/games/Black Mesa/bms.exe
workdir=/games/Black Mesa
art=/pictures/Black Mesa.jpg
runner=umu-run
WINEPREFIX=/games/prefixes/black-mesa
PROTONPATH=GE-Proton
```

Paths can contain spaces. Use absolute paths for the executable, artwork,
prefix, and working directory. Leave `workdir` empty to inherit the directory
where you started Ludus. Cover thumbnails are cached in `~/.ludus/cache/`.

Use `cmd` for game arguments, environment variables, or wrappers. It runs in
`/bin/sh`, where `"$@"` expands to the runner and executable you entered.
For example, this starts the game with MangoHud and passes `-novid` to it.

```ini
cmd=exec mangohud "$@" -novid
```

To try a game without saving it, pass its cover and launch command on the
command line. Separate games with `--`.

```sh
./ludus 'covers/Portal 2.jpg' steam -applaunch 620 -- covers/DOOM.png gzdoom
```

Saving one of these games with `e` also saves the directory you started it from.

The bundled `stb_image.h` (revision `5c20573`) includes its license.
