# ludus

A C99 Wayland game launcher. The window shows a fixed bitmap header and a grid
of portrait covers. Use `h` and `l` within a row, `j` and `k` between rows,
`Return` to run the selected entry, and `q` to quit.

## Build

Install a C compiler, make, pkg-config, the Wayland client and xkbcommon
development packages, wayland-scanner, and wayland-protocols. Run `make`,
then start `./ludus` inside a Wayland session. Edit `config.h` and rerun
`make` to change cover width, spacing, selection color, or keys.

## Try a library

Pass a cover followed by an optional executable and its arguments. Separate
entries with a standalone `--`. For example, using your own cover paths:

```sh
./ludus 'covers/Portal 2.jpg' steam -applaunch 620 -- 'covers/DOOM.png' gzdoom
```

For a cover-only grid, run `./ludus first.jpg -- second.png`. With no
arguments the window opens with an empty grid. Entries keep their command-line
order and last for this session. Commands launch directly with their argument
boundaries intact and inherit the launcher's environment and working directory.

The bundled stb_image decoder reads JPEG and PNG files as they come into view.
The grid center-crops each image to a 2:3 thumbnail. A 600×900 cover fits
without cropping. An unreadable cover produces a placeholder and a diagnostic
on standard error.

`vendor/stb_image.h` comes from stb revision `5c20573` and includes its license.
