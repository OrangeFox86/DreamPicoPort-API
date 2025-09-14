# DreamPicoPort-API
USB-host-side API for host-mode DreamPicoPort

This is currently a work in progress, but it is at least usable.

## Conan Usage

This repo is compatible with conan, simplifying the dependency and prerequisite process. If conan is installed, execute the following.
```bash
conan create . -b missing
```

## Non-Conan Usage

### Dependencies

After checking out this repository, execute the following to pull down the libusb dependency.
```bash
git submodule update --recursive --init
```

### Linux Prerequisites

`libudev-dev` is required to compile libusb.
```bash
sudo apt install libudev-dev
```

`cmake` is required to build the project.
```bash
sudo apt install cmake
```

### Windows Prerequisites

Install MSVC C++ compiler. This can be done by simply installing Visual Studio with C++.

### Compile

Execute the following to compile using cmake.
```bash
cmake --no-warn-unused-cli -S. -B./build
cmake --build ./build --config Release -j 10
```
