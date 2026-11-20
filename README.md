# ludus

A small Wayland game library for oyna, written in C99. Pick a cover with the
vi keys and press Enter to play.

## Build and run

You'll need a C compiler, make, pkg-config, wayland-scanner, wayland-protocols,
and the development packages for wayland-client, xkbcommon, Fontconfig, and
FreeType. Build and start Ludus from a Wayland session.

```sh
make
./ludus
```

Use `make install` to install Ludus in `/usr/local/bin` and `make uninstall`
to remove it. Set `PREFIX` on both commands to use another installation prefix.

Ludus uses your system's `monospace` font. To change the font, colors, cover
size, or keybindings, edit `config.h` and run `make` again.

## Controls

| In the library | Key |
| --- | --- |
| Move left, down, up, right | `h`, `j`, `k`, `l` |
| Launch the selected game | Enter |
| Edit the selected config in `$EDITOR` | `e` |
| Add a game config in `$EDITOR` | `+` |
| Search by title | `/` |
| Next or previous match | `n` / `N` |
| Quit | `q` |

## Oyna games

Put `oyna` on your `PATH`, then put your game configs in `~/.oyna/`.
Use the game title as the filename. For example, save this as
`~/.oyna/Black Mesa` with `art=` pointing to an optional JPEG or PNG cover.

```ini
exe=/games/Black Mesa/bms.exe
WINEPREFIX=/games/prefixes/black-mesa
PROTONPATH=/games/proton/GE-Proton
arg=-novid
art=/pictures/Black Mesa.jpg
```

Ludus passes the file to `oyna --config` when you press Enter.
See the [oyna config docs](https://github.com/gunesemir/oyna#config-files).

Press `+` to open a new config template. Fill it in and save it under your
game's title in `~/.oyna/`. Press `e` to edit an existing config. The grid
refreshes when the editor exits.

Set `EDITOR` to a command that waits until you're done editing, such as
`code --wait`. For a terminal editor, start Ludus from a terminal or use
an editor command that opens one, such as `EDITOR='foot nvim'`.

A 600×900 cover fits the grid. Thumbnails are cached in `~/.cache/ludus/`.

The bundled `stb_image.h` (revision `5c20573`) includes its license.
