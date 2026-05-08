# Building viogpu3d driver

This repository contains the kernel-mode part of the viogpu3d driver. The full driver package is built only when the user-mode driver DLLs are available in the directories pointed to by `MESA_PREFIX_x64` and, for 32-bit compatibility on 64-bit Windows, `MESA_PREFIX_x86`.

## Build mesa
These are general guidelines and may vary depending on the build environment and Mesa options used.

Now inside virtual machine with build tools installed create working directory, then inside it (this assumes use of Powershell):
1. Create mesa prefix dir `mkdir mesa_prefix` and set env `MESA_PREFIX_x64` to its path: `$env:MESA_PREFIX_x64="$PWD\mesa_prefix"`
2. Acquire [modified mesa source code](https://github.com/arehnman/virtio-win-mesa) and then cd into it `cd virtio-win-mesa`
3. Create build directory `mkdir build && cd build`
4. Configure build `meson setup .. --prefix=$env:MESA_PREFIX_x64  -Dgallium-drivers=virgl -Dgallium-d3d10umd=true -Dgallium-wgl-dll-name=viogpu_wgl -Dgallium-d3d10-dll-name=viogpu_d3d10 -Dvulkan-drivers=virtio -Dzlib=disabled -Dc_std=c11`, build options explained:
  * `--prefix=$env:MESA_PREFIX_x64` set installation path to dir created in step 1
  * `-Dgallium-drivers=virgl` build only virgl driver
  * `-Dgallium-d3d10umd=true` build DirectX 10 user-mode driver (opengl one is build by default)
  * `-Dgallium-d3d10-dll-name=viogpu_d3d10` name of generated d3d10 dll to `viogpu_d3d10.dll`
  * `-Dgallium-wgl-dll-name=viogpu_wgl` name of generated wgl dll to `viogpu_wgl.dll`
  * `-Dvulkan-drivers=virtio` build Vulkan user-mode driver
  * `-Dzlib=disabled` disable ZLIB library
  * `-Dc_std=c11` use c11 standard
  * `-Db_vscrt=mt` may be used with MSVC/clang-cl to select the static Visual Studio CRT; it does not make MinGW runtime libraries static
5. Build and install (to mesa prefix): `ninja install`
6. (optional) Repeat step 1-5 to build x86 Mesa and set `MESA_PREFIX_x86` to that prefix. Keep the installed DLL names as `viogpu_wgl.dll` and `viogpu_d3d10.dll`; the driver package renames them while staging. For MinGW builds that should not depend on MinGW runtime DLLs, add `-Dc_link_args="-static -static-libgcc"` and `-Dcpp_link_args="-static -static-libgcc -static-libstdc++"`.

NOTE: Only mingw w64 clang is supported when building Vulkan driver

## Build driver
Now that Mesa is built and installed into `%MESA_PREFIX_x64%` and optionally `%MESA_PREFIX_x86%`, viogpu3d can be built. If `%MESA_PREFIX_x64%` is not set, the x64 viogpu3d build fails before packaging.
1. Acquire [drivers source code](https://github.com/arehnman/kvm-guest-drivers-windows) and cd into it `cd kvm-guest-drivers-windows`
2. Go to viogpu `cd viogpu`
3. (optional, but very useful) setup test code signning from visual studio
4. Call build `.\build_AllNoSdv.bat`

Built driver will be available at `kvm-guest-drivers-windows\viogpu\Install\Win10`.
