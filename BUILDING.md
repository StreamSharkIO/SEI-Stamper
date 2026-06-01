# Building SEI Stamper

## Prerequisites

### macOS

- Xcode Command Line Tools (`xcode-select --install`)
- [Homebrew](https://brew.sh)
- CMake 3.20+, pkg-config, simde:
  ```bash
  brew install cmake pkg-config simde
  ```

### Windows

- Visual Studio 2022 with C++ desktop workload (64-bit toolset)
- CMake 3.20+

## OBS Studio Source Tree

Both platforms require the OBS Studio source tree with pre-built dependencies. Clone it into the project root as `obs-studio-master/`:

```bash
git clone --recursive https://github.com/obsproject/obs-studio.git obs-studio-master
```

Then follow the OBS build instructions for your platform to populate the `.deps/` directory:

- macOS: https://github.com/obsproject/obs-studio/wiki/build-instructions-for-mac
- Windows: https://github.com/obsproject/obs-studio/wiki/build-instructions-for-windows

The expected layout after setup:

```
sei-stamper/
  obs-studio-master/
    libobs/
    build/
    .deps/
      obs-deps-YYYY-MM-DD-universal/   (macOS)
      obs-deps-YYYY-MM-DD-x64/         (Windows)
```

## Building

### macOS

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

The output is a `.plugin` bundle at `build/plugin/sei-stamper.plugin`.

### Windows

The simplest path is the included batch script:

```cmd
build_and_install.bat
```

Or manually with CMake:

```cmd
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

The output is `build/plugin/Release/sei-stamper.dll`.

## Installing

### macOS

Copy the plugin bundle to OBS's plugin directory:

```bash
cp -R build/plugin/sei-stamper.plugin ~/Library/Application\ Support/obs-studio/plugins/
```

Restart OBS after copying.

### Windows

Copy the DLL to OBS's plugin directory:

```cmd
copy build\plugin\Release\sei-stamper.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
```

Restart OBS after copying.

## Verification

1. Launch OBS and open **Help > Log Files > View Current Log**
2. Search for `sei-stamper` or `SEI Stamper` -- you should see the plugin loading
3. In **Settings > Output > Streaming**, the **Video Encoder** dropdown should include "SEI STAMPER (H.264)"
4. On macOS, the **Hardware Encoder** dropdown in the encoder settings should show "Apple VideoToolbox" and "x264 (Software)"
