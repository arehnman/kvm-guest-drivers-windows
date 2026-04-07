# Building viogpu3d driver

This repository contains kernel-mode part of viogpu3d driver, and full-driver will be build only if there is user-mode driver dll's available in directory pointed by environment variable `MESA_PREFIX` and `MESA_PREFIX_WOW64`.

## Build mesa
Now inside virtual machine with build tools installed create working directory, then inside it (this assumes use of Powershell):
1. Create mesa prefix dir `mkdir mesa_prefix`  and set env `MESA_PREFIX` to its path: `$env:MESA_PREFIX="$PWD\mesa_prefix"`
2. Acquire [modified mesa source code](https://github.com/arehnman/virtio-win-mesa) and then cd into it `cd virtio-win-mesa`
3. Create build directory `mkdir build && cd build`
4. Configure build `meson setup .. --prefix=$env:MESA_PREFIX  -Dgallium-drivers=virgl -Dgallium-d3d10umd=true -Dgallium-wgl-dll-name=viogpu_wgl -Dgallium-d3d10-dll-name=viogpu_d3d10 -Dvulkan-drivers=virtio -Dzlib=disabled -Db_vscrt=mt -Dc_std=c11`, build options explained:
  * `--prefix=$env:MESA_PREFIX` set installation path to dir created in step 1
  * `-Dgallium-drivers=virgl` build only virgl driver
  * `-Dgallium-d3d10umd=true` build DirectX 10 user-mode driver (opengl one is build by default)
  * `-Dgallium-d3d10-dll-name=viogpu_d3d10` name of generated d3d10 dll to `viogpu_d3d10.dll`
  * `-Dgallium-wgl-dll-name=viogpu_wgl` name of generated wgl dll to `viogpu_wgl.dll`
  * `-Dvulkan-drivers=virtio` build Vulkan user-mode driver
  * `-Dzlib=disabled` disable ZLIB library
  * `-Db_vscrt=mt` use static c runtime
  * `-Dc_std=c11` use c11 standard
5. Build and install (to mesa prefix): `ninja install`
6. (optional) Repeat step 1-5 build wow64 mesa `meson setup .. --prefix=$env:MESA_PREFIX_WOW64  -Dgallium-drivers=virgl -Dgallium-d3d10umd=true -Dgallium-wgl-dll-name=viogpu_wgl_wow64 -Dgallium-d3d10-dll-name=viogpu_d3d10_wow64 -Dvulkan-drivers=virtio -Dzlib=disabled -Db_vscrt=mt -Dc_std=c11`

NOTE: Only mingw w64 clang is supported when building Vulkan driver

## Build driver
Now that mesa is build and installed into `%MESA_PREFIX%` and `%MESA_PREFIX_WOW64%` viogpu3d will be built (in case `%MESA_PREFIX` is not set viogpu3d inf generation is skipped)
1. Acquire [drivers source code](https://github.com/arehnman/kvm-guest-drivers-windows) and cd into it `cd kvm-guest-drivers-windows`
2. Go to viogpu `cd viogpu`
3. (optional, but very useful) setup test code signning from visual studio
4. Call build `.\build_AllNoSdv.bat`

Built driver will be available at `kvm-guest-drivers-windows\viogpu\Install\Win10`.
