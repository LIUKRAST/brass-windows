# brass-windows

Pixel-art windows for Hyprland (0.57, Lua config).

- **Cut corners**: no border thickness is added. The window corners are
  cut in a staircase of "art pixels", so whatever is behind shows through.
- **Focus outline**: the focused window gets an outline drawn *on top of*
  its contents, following the staircase.
- **Cutout**: a window erases everything below it in its own shape grown by
  `cutout_gap` art pixels, so the wallpaper shows in a gap around it. The
  windows below draw their outline along the edge of the hole too.
- **Floating title bar**: floating windows get a bar with pixel-art
  buttons (your own PNG sprites) and the title. The corners of the bar + window
  are cut as a single shape.

One *art pixel* is `pixel_size` logical pixels, so corners, outline, bar and
sprites all sit on the same grid.

## Install

With [hyprpm](https://wiki.hypr.land/Plugins/Using-Plugins/) (builds it for
your Hyprland version):

```sh
hyprpm update
hyprpm add https://github.com/LiukRast/brass-windows
hyprpm enable brasswindows
```

and run `hyprpm reload -n` at startup. Or build it yourself:

```sh
make            # -> brasswindows.so
```

and load it with `hl.plugin.load("/path/to/brasswindows.so")`.

> Hyprland reads the config *before* loading plugins, then reloads it.
> Wrap the plugin setup in `if hl.plugin.brasswindows then ... end` so the
> first pass doesn't report `unknown config key 'plugin.brasswindows.*'`.

## Hyprland setup

The plugin replaces the Hyprland border and rounding, so turn those off:

```lua
hl.config({
    general    = { border_size = 0 },
    decoration = { rounding = 0 },
})

hl.plugin.load(plugin .. "brasswindows.so")

hl.config({
    plugin = {
        brasswindows = {
            pixel_size     = 4,         -- logical px per art pixel
            corner         = "2,1",     -- art pixels cut per row, from the edge
            border_size    = 1,         -- outline, in art pixels
            active_color   = "rgba(ffffffff)",
            inactive_color = "rgba(00000000)", -- no outline when unfocused

            bar_height     = 9,         -- art pixels
            bar_color      = "rgb(000000)",
            title_font     = "MonocraftFive",
            title_size     = 16,
            on_double_click = "maximize",
        },
    },
})

local sprites = config .. "/hypr/brass/"
hl.plugin.brasswindows.add_button({
    sprite   = sprites .. "close.png",
    hover    = sprites .. "close_hover.png",    -- optional
    pressed  = sprites .. "close_pressed.png",  -- optional
    inactive = sprites .. "close_inactive.png", -- optional, window not focused
    action   = "close",
})
hl.plugin.brasswindows.add_button({ sprite = sprites .. "minimize.png", action = "minimize" })
hl.plugin.brasswindows.add_button({ sprite = sprites .. "maximize.png", action = "maximize" })
```

### Options (`plugin.brasswindows.*`)

| option | default | |
|---|---|---|
| `enabled` | `true` | cut corners + outline |
| `pixel_size` | `4` | logical px per art pixel |
| `corner` | `"1"` | staircase: `"1"` one missing pixel, `"2,1"`, `"3,1,1"`, `"0"` square |
| `border_size` | `1` | outline thickness (art px), drawn over the window |
| `cutout` | `true` | windows erase what is below them, plus a gap |
| `cutout_gap` | `1` | art px of gap around a window, cut into the windows below |
| `active_color` / `inactive_color` | white / transparent | outline colors (animated with the `border` animation) |
| `bar_enabled` | `true` | title bar on floating windows |
| `bar_on_tiled` | `false` | title bar on tiled windows too |
| `bar_height` | `9` | art px |
| `bar_color` | `rgb(000000)` | |
| `buttons_align` | `"left"` | `"left"` (macOS) or `"right"` |
| `buttons_padding` | `2` | art px between the bar edge and the buttons |
| `buttons_spacing` | `1` | art px between buttons |
| `title_enabled` | `true` | |
| `title_font` / `title_size` | `Sans` / `16` | size in logical px |
| `title_align` | `"center"` | or `"left"` |
| `title_color` / `title_inactive_color` | white / grey | |
| `on_double_click` | `"maximize"` | any button action, `""` to disable |

### Sprites

PNG, drawn 1 sprite pixel = 1 art pixel, with nearest-neighbor filtering, and
centered vertically in the bar. All states of one button should have the same
size as `sprite`. With `bar_height = 9` a 5x5 or 7x7 sprite centers exactly.

### Button actions

`close`, `maximize` (toggle), `fullscreen` (toggle), `minimize` (moves the window
to `special:minimized`), `float` (toggle), `pin` (toggle). Anything else is run as
a shell command, after the clicked window has been focused.

### Window rules

- `brasswindows:no_bar`: no title bar on matching windows
- `brasswindows:no_frame`: no corner cut / outline on matching windows

## How it works

- Corners: hooks `IElementRenderer::preDrawSurface` and removes the corner
  staircase from the damage while the window's surfaces are drawn, so those
  pixels are never written. `CSurfacePassElement::opaqueRegion` is hooked too so
  Hyprland still draws what's behind the corners.
- Cutout: the same damage trick, with the shapes (+ gap) of the windows painted
  after this one (Hyprland's paint order: tiled, focused tiled, floating, then
  special workspace, then pinned). The outline around the hole is drawn by the
  window below, inside its own shape. A second, invisible decoration grows each
  window's damage box by the gap + outline, so moving or closing a window
  repaints the gap it cut.
- Bar and outline: one window decoration (`DECORATION_LAYER_OVER`), drawn above
  the surface and below popups. The bar reserves space on top of floating windows.

The old shader-based border (`brassborders.so`) is in `legacy/`.
