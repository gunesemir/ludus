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
| Edit the title or cover | `e` |
| Search by title | `/` |
| Next or previous match | `n` / `N` |
| Quit | `q` |

## Oyna games

Put `oyna` on your `PATH`, then put your game configs in `~/.ludus/`.
Use the game title as the filename. For example, save this as
`~/.ludus/Black Mesa` with `art=` pointing to an optional JPEG or PNG cover.

```ini
exe=/games/Black Mesa/bms.exe
WINEPREFIX=/games/prefixes/black-mesa
PROTONPATH=/games/proton/GE-Proton
arg=-novid
art=/pictures/Black Mesa.jpg
```

Ludus passes the file to `oyna --config` when you press Enter. Edit launch
settings and arguments in the file itself. Old `.meta` entries are skipped.
See the [oyna config docs](https://github.com/gunesemir/oyna#config-files).

Press `e` to rename the file or change its cover. Use Tab to switch fields,
Ctrl+V or Shift+Insert to paste, Enter to save, or Escape to cancel. A 600×900
cover fits the grid. Thumbnails are cached in `~/.ludus/cache/`.

The bundled `stb_image.h` (revision `5c20573`) includes its license.
