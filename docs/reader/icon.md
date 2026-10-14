# The app's icon

PPSSPP Reader has an icon of its own, so that it is not taken for the emulator it is made from:
the word 文字 ("written characters") on a plain ground. It is what the Android launcher,
the Windows program and the desktop window show.

## Where it is

| Path | |
| --- | --- |
| `reader/icon/master.png` | The picture itself, 1024 pixels square, its ground one colour to the edges. |
| `reader/icon/app.ico`, `app.png` | The Windows program's icon and the desktop window's. |
| `reader/icon/android/res/` | The launcher icon in every density, with the shape Android cuts it to allowed for. |
| `tools/reader/icon.py` | Makes everything else from `master.png`: `tools/reader/icon.py reader/icon/master.png reader/icon` (needs Pillow). |

The builds take it from there: `android/build.gradle.kts` (flavour `reader`) for Android, `CMakeLists.txt` and `reader/CMakeLists.txt` for the
window, `reader/qt/app.rc` for the Windows program.

## An icon of your own

A folder `private/icon` (which Git ignores) with the same files comes before the committed
one in every build:

```sh
tools/reader/icon.py my-picture.png private/icon
```

The desktop builds look for it when they are configured, so run the build script once more
after making or removing it (`touch` the `CMakeLists.txt` named above if nothing else
changed). Whatever is in `private/icon` stays on your machine and in your own builds. **The
committed icon carries no console maker's logo or wordmark, and none is to be committed**:
those are trademarks, and this app is not theirs.

## What was checked

On the machine it was made on, with a set in `private/icon`: the Linux and the Windows
program of the desktop app were built and hold the picture (looked for in the files, all
seven sizes in the Windows program); the Android app was built and its package holds the
launcher pictures. **Not seen on a screen**: no launcher, no Windows taskbar and no window
title was looked at.
