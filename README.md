
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


Contributing
------------

In order for your contribution to Goxel to be accepted, you have to sign the
[Goxel Contributor License Agreement (CLA)](doc/cla/sign-cla.md).  This is
mostly to allow me to distribute the mobile branch goxel under a non GPL
licence.

Also, please read the [contributing document](CONTRIBUTING.md).
