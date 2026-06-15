# velox

velox is a simple media player written in c using gtk4 and gstreamer. it is designed to be fast and lightweight while supporting modern linux desktop features.

## what it does

the application plays most standard audio and video formats. it uses hardware acceleration through gstreamer when available, and renders video frames efficiently to the user interface. it also includes features like picture-in-picture mode, a playlist manager, and basic subtitle support.

## how to build

you will need meson, ninja, and a c compiler. you also need the development packages for gtk4 and gstreamer installed on your system.

to configure the build directory:

    meson setup build

to compile the application:

    ninja -C build

to run it:

    ./build/velox

## license

this project is released under the mit license. see the license file in this repository for more information.
