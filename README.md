# Fluent Experience for Windows 11

A compact, floating taskbar built on top of the native Windows 11 taskbar, as a
[Windhawk](https://windhawk.net) mod. It doesn't replace the taskbar: Start,
search, previews, menus and Windows updates keep working.

![Fluent Experience taskbar](docs/taskbar.svg)

## Philosophy

Fluent Experience is built on what Windows already provides, and adds custom
code only when there's no native way to do something. Materials, rounded
corners, borders and shadows are Windows 11's own, drawn by Windows. When
Microsoft changes how something looks or behaves, the taskbar follows, instead
of being stuck with a copy of an older design.

## Features

The taskbar is actually narrowed and centered, not just clipped, so clicks,
menus and corners follow it. Each taskbar grows and shrinks to fit its icons,
on every monitor, and floats a few pixels above the bottom edge of the screen,
while maximized windows stop above it.

It can use the Windows 11 materials, Acrylic, Mica or Mica Alt, with the
rounded corners and shadow Windows gives windows, following light/dark mode
and Transparency effects. A button on the taskbar, before the clock, opens the
Fluent Experience settings, and Fluent Experience tells you when a new version
is available.

Windows settings still apply: "Show smaller taskbar buttons" and "Animation
effects" work as on the original taskbar. Every feature can be turned off;
turning Fluent Experience off restores the original Windows taskbar.

## Materials

The materials are Windows 11's own, drawn by Windows. In Microsoft's words:

**Acrylic** is "a type of Brush that creates a translucent texture". Background
acrylic "reveals the desktop wallpaper and other windows that are behind the
currently active app".

**Mica** is "an opaque, dynamic material that incorporates theme and desktop
wallpaper to paint the background of long-lived windows". "As the user moves
the window across the screen, the Mica material dynamically adapts to create a
rich visualization using the wallpaper underneath the application."

**Mica Alt** is "a variant of Mica, with stronger tinting of the user's desktop
background color".

All of them "appear as a solid color" when Transparency effects are turned
off, in Battery Saver mode or on low-end hardware. Sources:
[Mica](https://learn.microsoft.com/windows/apps/design/style/mica),
[Acrylic](https://learn.microsoft.com/windows/apps/design/style/acrylic).

## Requirements

Windows 11 and [Windhawk](https://windhawk.net), the customization platform
Fluent Experience runs on.

## Installation

1. Install Windhawk from [windhawk.net](https://windhawk.net) or with
   `winget install RamenSoftware.Windhawk`.
2. Download `fluent-experience.wh.cpp` from the
   [latest release](https://github.com/Will0wn/fluent-experience/releases/latest).
3. In Windhawk, select **Create a New Mod**, replace the code in the editor
   with the downloaded file, then select **Compile Mod** and **Exit Editing
   Mode**.

The taskbar changes right away.

## Settings

Open the settings with the button on the taskbar, before the clock. Changes
apply immediately, no restart needed. The window is available in English and
Italian.

## Updates

Fluent Experience checks GitHub for a new version at startup and once a day
(this can be turned off in the settings). When one is available, the settings
button shows it the way Windows shows an app that needs attention, and the
settings window offers **Update now**. Nothing is installed without you:
Windows asks for administrator permission, then Windhawk reloads the mod.

## Uninstall

In Windhawk, open Fluent Experience and select **Remove**. The original
Windows taskbar comes back right away.

## License

[GPL-3.0](LICENSE) © [Will0wn](https://github.com/Will0wn)
