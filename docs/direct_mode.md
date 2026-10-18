# Direct mode

Direct mode runs an Android VR game in `axrb-direct.exe` on Windows, without an
Android emulator or guest kernel. Dynarmic translates the game's ARM64 code and
the real AOSP ART runtime. Java executes against the matched Android framework,
core libraries and APEX modules, rather than the former custom DEX interpreter.
The Windows runner implements the required Linux syscall and Binder boundary.

Vulkan uses the Windows driver; the existing OpenXR host bridge presents frames
and supplies headset/controller tracking. Android service adapters implement
application/activity lifecycle, package lookup, settings, connectivity,
broadcasts and host surfaces using AOSP classes. Media Foundation decodes video;
AAudio and OpenSL ES PCM output use the host WASAPI backend.

## Launcher installation

The existing **Play in direct mode** action continues to use the OVRPort-patched
APK and the library's content files. No SDK, Android build tools, JDK or system
Python installation is required to import and start a patched game. Python,
AAPT2, the bootstrap DEX and native bridge libraries are bundled. An external
OVRPort patcher may have separate requirements.

On first use, the launcher downloads the pinned official Android 16 ARM64 image
(`arm64-v8a-36_r02.zip`, 810,736,863 bytes, SHA-1
`62ad6714df790f89c8a8ad32552ffe20bb16fe87`) under the Android SDK license accepted
in setup. `make_sysroot.py --art` extracts its matched ART, framework, fonts,
configuration and APEX modules. Existing smaller Direct roots migrate to format
2; a receipt alone cannot make an incomplete root ready. The archive is removed
after installation. A Windows Vulkan driver and working OpenXR runtime/headset
remain required.

`prepare_art_session.py` prepares the same native arguments for the launcher and
developer game probes. It extracts ARM64 APK libraries once per library manifest,
including CRCs so equal-size updates do not keep stale code, and mounts code and
content read-only. Game assets stay at their original host locations. App data
and saves are ordinary files under `<runtime folder>/direct/data` and
`<runtime folder>/direct/sdcard`, separate from emulator saves.

Stop requests pause, stop and destroy activities on the Android main thread.
The launcher waits for shutdown before terminating an unresponsive process.
Engines may deliberately kill their process in `onDestroy`; the session records
whether pause/stop completed before that. Logs are in
`<runtime folder>/output/logs/game` (`direct.out`, `direct.err`, `direct.log`,
`host.err`, `session.json`).

## Compatibility boundaries

This integrates real AOSP userspace and ART, but does not boot a complete Android
`system_server`, SurfaceFlinger or vendor service stack. Unsupported hardware and
platform services are still compatibility work. In particular, Meta/Ubisoft
account services, Play services, hardware DRM, split APK installation and full
Android multi-process execution are not supplied by this backend. Rendering
goes through Vulkan. OpenGL ES games run on the AOSP image's ANGLE, which draws
through the same forwarded Vulkan driver: `prepare_art_session.py --gles auto`
selects it for Unreal games built without Vulkan
(`GameActivity.bSupportsVulkan=false`), `--gles angle|stub` forces a choice
(`run_direct_game.ps1 -Gles`, or `AXRB_DIRECT_GLES`). Other games keep an EGL
stub with no display, so a Vulkan game never starts a second GL device. The
OpenXR runtime exports a GLES game's frames from ANGLE's Vulkan device: each
swapchain image has a 2D texture that ANGLE exports as a VkImage
(`EGL_ANGLE_vulkan_image`); the game's texture (2D, or a multiview array with
its layers stacked) is copied into it at `xrReleaseSwapchainImage` and handed
over with `GL_ANGLE_vulkan_image`, and the export copies run on ANGLE's queue
under its queue lock (`runtime/src/gles_angle.cpp`). Small shims in front of
ANGLE's libEGL and libGLESv2 (`direct/guest/proxy/lib*_angle.c`) add the
multisampled multiview attachment Quest builds of Unreal use, which ANGLE
lacks. Java `MediaExtractor` and `AMediaExtractor` use the host's extractor, as
there is no media.extractor service. Host MediaPlayer currently decodes video, with game audio
handled separately through native audio APIs. Startup verification is not a
claim that every title, online feature or gameplay path works.

## Build and verification

`scripts/build/build_direct.ps1` builds and stages `out/direct/axrb-direct.exe`,
`art/axrb-bootstrap.dex`, `art/lib64` and `guest/lib64`. Build machines need an
NDK, `build-tools;35.0.0`, `platforms;android-36` and a JDK. The script downloads
and verifies the matched AOSP image for its NDK forwarding adapter, or accepts
`-ArtSysroot` pointing to a developer extraction of that image. Build the ARM64
OpenXR runtime first with `runtime/apk/build_apk.ps1 -Abi arm64-v8a`.
`scripts/build/installer.ps1` includes these files in launcher builds.

The older managed interpreter remains in the source tree; its boot DEX is an
optional developer target (`AXRB_BUILD_LEGACY_BOOT=ON`) and is not the launcher
execution path. `direct_deps.ps1` supplies the pinned Dynarmic sources and Boost
headers. bc7e uses the prebuilt Windows objects in
`runtime/vulkan/texture/bc7e/win64` (`scripts/build/build_bc7e.py` regenerates them).

Run `direct/tools/test_art.py --help` for the native Linux/Binder, real ART/JNI
and framework regression probes. `--host-bridges out/direct` adds real Vulkan
image/memory tests and OpenSL ES buffer-queue/audio tests.
`python -m unittest discover -s direct/tests -p test_art_session.py` verifies
safe APK extraction and library-cache invalidation without an SDK.

Packaging validates payload hashes and round-trips the entire Direct payload
through the installer's actual NSIS extractor. The pinned 7-Zip BCJ filter avoids
unsupported ARM64 compression that previously omitted guest libraries silently.
The packaged reader and preparation script are also exercised with SDK, Java
and system Python removed from PATH. Launcher version is **2.0.0**.


## AXRB-Emulatorless build

The launcher is version 2.0.0. It installs as **AXRB-Emulatorless** with
application ID `org.axrb.emulatorless`, independently of AXRB:

- Program: `%LOCALAPPDATA%\Programs\axrb-emulatorless-launcher` (per-user installer).
- Launcher profile/library: `%APPDATA%\AXRB-Emulatorless`.
- Runtime and Android app data: `%LOCALAPPDATA%\AXRB-Emulatorless Runtime` by default.
- Downloads: `Downloads\AXRB-Emulatorless`.

The installer allows a custom program folder. The first-run screen accepts the
system-library license and lets users browse for exact runtime-data and
game-download folders, prefilled with recommended defaults and reset controls.
It performs no emulator, SDK, AVD or hypervisor setup.
The Direct setup requires at least 6 GB of system RAM and states the GPU
requirement as Vulkan support, with no AMD/NVIDIA vendor gate. RAM is allocated
as needed; the 6 GB minimum does not reserve memory. The build profile selects
the Direct screen even if stale setup metadata contains an emulator phase.
The existing AXRB profile and saves are not migrated automatically. Direct APK
launches retain the existing patch/import and Windows graphics integration.
The Android system libraries still download on first launch; no developer SDK,
JDK or system Python installation is needed to run the packaged launcher.

`launcher/core/build-profile.mjs` contains the build-time `emulatorEnabled`
flag. It is false for this build. To restore the old Android Emulator backend
in a future build, set it to true and rebuild with
`scripts/build/installer.ps1 -Setup`. This restores legacy setup/UI and packages
the emulator launch scripts and clock helpers. Installed user settings and
IPC requests cannot enable it in an Emulatorless build. Both configurations
keep the independent AXRB-Emulatorless identity. Automatic updates from the
original AXRB repository are disabled; configure a separate release channel
before enabling updates.

The developer storage regression is `direct/tools/test_storage.py`. It builds
a small APK, launches it through the production AOSP bootstrap, then launches
a second process to verify private files, UTF-8 bytes, atomic rename, fsync,
SharedPreferences, a SQLite WAL transaction, and external app files. It also
checks cache writes/deletion and verifies the saved bytes from Windows. Use a
fresh `--work` directory, `--direct out/direct`, an extracted AOSP `--sysroot`,
and developer `--android-jar` / `--build-tools` paths. This verifies storage
APIs and restart persistence, not every game's save logic or power-loss recovery.

The regression also signals the launcher's real Direct stop event, writes
preferences with asynchronous `apply()` from `Activity.onStop`, and checks them
after restart. Shutdown drains AOSP `QueuedWork` after stop and destroy, matching
Android's handling of pending preference writes before the process exits.
