# colorello

A tiny terminal program that shows the color palette of your terminal in the
middle of the screen, as a row of seven slanted pixel blocks. Written in C, no
dependencies besides libc.

- The blocks are the ANSI colors 1-7 of your terminal, so the picture always
  follows your color scheme (no fixed RGB values are used).
- The picture is centered and scaled to the terminal size. It is redrawn when
  the window is resized.
- Uses the alternate screen and restores the terminal on exit (also on
  `SIGINT`, `SIGTERM` and `SIGHUP`).

## Build

    make

Requires a C99 compiler and `make`.

## Install

    make install

Installs `colorello` to `/usr/local/bin`. Use `PREFIX` and `DESTDIR` to change
the location:

    make install PREFIX=$HOME/.local
    doas make install PREFIX=/usr

Other targets:

    make uninstall    remove the installed binary
    make clean        remove build output

## Usage

    colorello [-p] [-a ASPECT]
    colorello -h | -V

| Option      | Description                                                        |
|-------------|--------------------------------------------------------------------|
| `-p`        | Plain mode: whole cells only, no half-block characters.            |
| `-a ASPECT` | Cell height/width ratio, 1.0-4.0. Default: auto-detected, else 2.0.|
| `-h`        | Show help.                                                         |
| `-V`        | Show version.                                                      |

Press `q`, `Esc`, `Ctrl-C` or `Ctrl-D` to quit.
