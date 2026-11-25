# Ludus

Ludus is a keyboard-driven Wayland library for [oyna]. Each game is an oyna
config in `~/.oyna/`. The filename is the title shown in the cover grid.

## Get started

Build with a C99 compiler, make, pkg-config, wayland-scanner, wayland-protocols,
and the development packages for Wayland client, xkbcommon, Fontconfig, and
FreeType. Put `oyna` on your `PATH` and run these in a Wayland session.

```sh
make
./ludus
```

`make install` copies the binary to `/usr/local/bin`. Use `make uninstall`
to remove it or set `PREFIX` on both commands to use a different prefix.

## Add a game

Press `+`, type a title, and press Enter. Ludus opens a template in `$EDITOR`.
The template uses the title for the prefix and defaults to GE-Proton. Fill in
the executable path and save. For example, `~/.oyna/Black Mesa` could contain

```ini
exe=/games/Black Mesa/bms.exe
art=/pictures/Black Mesa.jpg
WINEPREFIX=~/.oyna/prefix/Black Mesa
PROTONPATH=~/.oyna/proton/GE-Proton
arg=-novid
```

`art=` is optional. Use a JPEG or PNG for the cover. Ludus caches thumbnails
in `~/.cache/ludus/`. Oyna handles launch settings and game arguments.
See the [oyna config documentation][oyna-config].

## Keys

| Key | Action |
| --- | --- |
| `h`, `j`, `k`, `l` | Move left, down, up, right |
| `gg`, `G` | Jump to the first or last game |
| `?` | Show shortcuts. Press `?` or Escape to close |
| Enter | Play the selected game |
| `+` | Add a config |
| `e` | Edit the config |
| `R` | Rename the config and its title |
| `d` | Move the config to trash after confirmation |
| `r` | Rescan `~/.oyna/` |
| `/` | Search titles |
| `n`, `N` | Next or previous match |
| `L` | Open the selected game's last launch log |
| `q` | Quit |

Enter confirms a prompt and Escape cancels it. Removed configs are kept
under `~/.oyna/.trash/` and can be moved back into `~/.oyna/`.

If oyna exits with an error, the bottom bar shows its last diagnostic.
`L` opens the full stderr log for the selected game's most recent launch
in the current Ludus session.

## Editor and appearance

Set `EDITOR` to a command such as `nvim`. Ludus hands it the foreground
terminal, or opens a terminal through your `TERMINAL` command or
`xdg-terminal-exec`. Use a terminal and editor command that wait until
you close the file, such as `EDITOR='code --wait'` for VS Code.
Ludus returns to the grid and reports whether the config changed. If a saved
config is missing a required value, press `e` to reopen it or Escape to dismiss.

Edit `config.h` and rebuild to change the keys, colors, cover size, or
font. The default font is your system's monospace face.

Ludus is licensed under [0BSD](LICENSE). The bundled `stb_image.h`
(revision `5c20573`) includes its license.

[oyna]: https://github.com/gunesemir/oyna
[oyna-config]: https://github.com/gunesemir/oyna#save-per-game-settings
