
Goxel
=====

This is a fork of Goxel, a voxel editor made by Guillaume Chereau.
For original features and base code, visit [the official repository](
https://github.com/guillaumechereau/goxel).

Contributions
------
"Goxel", the original program, by Guillaume Chereau <guillaume@noctua-software.com> / https://goxel.xyz

"libvxl", the updated means by which Ace of Spades maps can be easily loaded/written to, by ByteBit/xtreme8000 / [repository](https://github.com/xtreme8000/libvxl).

Licence
-------

[goxel](https://github.com/guillaumechereau/goxel) is released under the GNU GPL3 licence.

[libvxl](https://github.com/xtreme8000/libvxl) is released under the MIT license.

Changes in this fork
-------

    - ByteBit's .vxl fix from his fork (xtreme8000)
    - Tools
        - Brush can set width/height/depth separately
    - Colours
        - Colour picker popup no longer bleeds its clicks into the scene behind
        - Colour picker also integrated directly into the tools panel for tools like the brush
    - Camera
        - First person camera (#)
            - Arrow keys for forward/back/left/right, Page Up/Down for up/down, RMB or MMB for look
            - Speed
            - FOV
            - Manual X/Y/Z
        - Rotational camera (default) FOV setting (and its base value is higher than it was before)
    - Import/Export
        - Heightmap export .bmp file in greyscale, z0 = black, (zN = 8 + ((zN-1) * 4) = max 64 height
        - Colourmap export .bmp file
        - voxlap import (e.g. kvx) no longer retains bounding box bg on import
        - Imports now automatically add to a new layer, which is named after the file
    - Layers
        - Layers pane is now its own dedicated area on the right
    - Hotkeys
        - Add/Sub/Paint = U/I/O
        - Move plane up/down = < >
        - Plane visibility = /
        - FPV Camera = #

Building
--------

The building system uses scons.  You can compile in debug with 'scons', and in
release with 'scons mode=release'.  On Windows, currently possible to build
with [msys2](https://www.msys2.org/) or try prebuilt
[goxel](https://packages.msys2.org/base/mingw-w64-goxel) package directly.
The code is in C99, using some gnu extensions, so it does not compile
with msvc.

In order to compile, after cloning the repo you will need to manually place the contents of the [libvxl repository](https://github.com/xtreme8000/libvxl) into the following directory, otherwise it will not build.

    ./ext_src/libvxl


# Linux/BSD

Install dependencies using your package manager.  On Debian/Ubuntu:

    - scons
    - pkg-config
    - libglfw3-dev
    - libgtk-3-dev

Then to build, run the command:

    make release

# Windows

You need to install msys2 mingw, and the following packages:

    pacman -S mingw-w64-x86_64-gcc
    pacman -S mingw-w64-x86_64-glfw
    pacman -S mingw-w64-x86_64-libtre-git
    pacman -S scons
    pacman -S make

Then to build:

    make release

Assets
--------

Asset building is using python 2, and require extra modules to be installed, namely `fontforge` ([download](https://fontforge.org/)) and `PIL.image` ([download](https://github.com/python-pillow/Pillow/releases/tag/8.4.0)).

I have not got this working.