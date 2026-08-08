# MTK Display Overclock LKM: PMB110 Reference

中文说明：[README_CN.md](README_CN.md)

This project is a source-only reference implementation for MTK display
overclocking through a loadable kernel module. PMB110 is used as the concrete
reference device: it adds 170Hz and 185Hz modes to the stock panel at both
stock resolutions. When the module is enabled, it uses one fixed 1496Mbps MIPI DSI
link for the complete mode table. Official modes are kept at their original
refresh targets by compensating vertical blanking, while the two additional
refresh targets use copied 165Hz panel timing as their DDIC command path.

## PMB110 Example Baseline

The included example is tied to one PMB110 software and kernel ABI. The source
validates the panel name `panel_aa618_p_3_a0034_dsi_vdo`, the stock mode table,
and the 165Hz timing geometry before installing runtime hooks.

The validated baseline is:

- Product: PMB110
- Platform: MediaTek MT6993
- Panel: `panel_aa618_p_3_a0034_dsi_vdo`
- Official software: `PMB110_16.0.9.400(CN01)`
- Kernel release string: `6.12.58-android16-6-g7704a1ae279b-ab15213644-4k`
- Official kernel branch: `oneplus/mt6993_b_16.0_ace_6_ultra`
- Kernel source commit: `2e3b6d890bd2c7bd5779bef6df6e16e2c972b87c`
- Display/module source commit: `2cc7f4606b65a9ede42030ee82614dd845b665a1`
- Kernel release tag family: `android16-6.12-2025-12_r41`

The two source repositories are available from the OnePlusOSS GitHub
organization:

```text
https://github.com/OnePlusOSS/android_kernel_oneplus_mt6993.git
https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_mt6993.git
```

Use the commits above, not whichever branch happens to be current later. A
matching `6.12.58` string alone is insufficient: the ABI, symbol CRCs,
structure layouts, KCFI type hashes, and vermagic must all match the running
kernel.

## Implementation

The module is runtime-only. It does not patch DTBO or replace a panel driver.
Its initialization path is:

1. Resolve the target display symbols through the kernel's kallsyms lookup
   interface.
2. Validate the panel name, resolution count, stock mode count, stock timing,
   and the official 165Hz entries before registering any probe.
3. Obtain the primary MTK DSI component from the panel context. In boot mode
   this is captured before normal mode enumeration; otherwise the porch hook
   captures it when the display pipeline first runs.
4. Hook the MTK mode enumeration path and append four modes after the twelve
   official entries: full/reduced 170Hz followed by full/reduced 185Hz.
   Internally these are mapped to the verified 165Hz DDIC command index, so no
   unverified custom DCS command sequence is sent.
5. Hook the VDO porch/timing path. With `fake_mode=1`, the module applies the
   common 1496Mbps PLL/PHY link and recalculates VFP compensation for stock
   modes. The custom modes use their own host pixel clocks and timing targets.
   With `fake_mode=0`, stock timing and the native 1374Mbps link are restored.

The mode list is only an enumeration layer until the DSI timing callback is
active. A mode showing `170Hz` or `185Hz` in framework UI does not prove that
the panel is physically refreshing at that rate; measure the panel with a
hardware- or driver-side counter.

## Public Build

The only build entry point is `scripts/build_module.sh`. It deliberately uses
explicit paths so that no local workspace layout or device-build directory is
required.

Required inputs:

| Variable | Required value |
| --- | --- |
| `PMB110_HEADERS_DIR` | Complete generated kernel-header tree for the running PMB110 kernel, including `include/linux/compiler-version.h`, `include/linux/kconfig.h`, generated UAPI headers, and `arch/arm64` generated headers |
| `PMB110_KERNEL_SOURCE` | Matching kernel source checkout containing `scripts/module.lds.S` |
| `PMB110_DISPLAY_ROOT` | `kernel/kernel_device_modules-6.12` inside the matching OnePlus modules checkout |
| `PMB110_CLANG` | Android Clang r536225 `clang` executable |
| `PMB110_LD_LLD` | Matching r536225 `ld.lld` executable |

Example (paths are examples only):

```sh
export PMB110_HEADERS_DIR=/path/to/pmb110-headers
export PMB110_KERNEL_SOURCE=/path/to/android_kernel_oneplus_mt6993
export PMB110_DISPLAY_ROOT=/path/to/android_kernel_modules_and_devicetree_oneplus_mt6993/kernel/kernel_device_modules-6.12
export PMB110_CLANG=/path/to/clang-r536225/bin/clang
export PMB110_LD_LLD=/path/to/clang-r536225/bin/ld.lld
sh scripts/build_module.sh
```

The output is `out/PMB110_185_Mode.ko`; the internal Linux module name remains
`pmb110_185_mode` for the current runtime parameter paths.
The script requires Android Clang r536225, based on Clang 19.0.1, and fails
closed for another compiler.

## ABI, CRC, And KCFI

These checks are part of the module's compatibility boundary, not optional
warnings:

- **vermagic:** `PMB110_VERMAGIC` in the source must match `uname -r` and the
  target kernel's module configuration. A different build suffix, SMP mode,
  preemption setting, or unload setting can reject the module.
- **symbol CRCs:** the source contains the target `__versions` and
  `__version_ext_*` records. They must be regenerated from the exact target
  `Module.symvers` when the kernel baseline changes. Loading against a nearby
  kernel can fail with “disagrees about version of symbol”.
- **KCFI:** compile with `-fsanitize=kcfi` and the same integer-normalization
  option used by the target. KCFI validates indirect-call type hashes at
  runtime; changing a callback typedef, compiler, optimization mode, or kernel
  headers can produce a CFI failure even when the C code compiles.
- **structure ABI:** `mtk_dsi`, `mtk_panel_params`, the panel context prefix,
  and callback signatures are private vendor ABI. A layout change can turn a
  successful build into a bad memory read or a display crash. Revalidate every
  field offset against the exact display source before testing.
- **architecture flags:** this build is AArch64 and uses the target PAC/BTI
  and no-outlined-atomics flags. Do not substitute a desktop compiler or a
  generic Android NDK compiler.
- **module loading policy:** a production kernel may require a trusted module
  signature. KernelSU/root permission is not a replacement for an enforced
  kernel signing key; an unsigned module can still be rejected before its init
  function runs.

Before a device test, compare `uname -a`, `/proc/version`, `modinfo`, the
kernel configuration, the symbol CRC table, and the panel name. Test first
with `fake_mode=0`, capture logs, and keep a recovery path that can disable or
remove the module without restarting the display stack.

## Adding A New Refresh-Rate Mode

Adding a mode is not just changing the number displayed by Android:

1. Record the panel's native `hdisplay`, `vdisplay`, H/V sync and porch
   values, pixel clock, lane count, bpp/DSC configuration, and the panel's
   actual refresh rate for every stock resolution.
2. Choose the host timing and link rate together. For a fixed horizontal total
   and pixel clock, the approximate refresh is:

   ```text
   refresh_hz = pixel_clock_khz * 1000 / (htotal * vtotal)
   ```

   If HFP is fixed, derive the new total vertical period and set:

   ```text
   vfp = vtotal - vdisplay - vsa - vbp
   ```

   Keep VFP positive and within the panel/driver limits. Rounding must be
   checked against the driver integer units, not only the decimal result.
3. Select a PHY/PLL rate that carries the required pixel payload with margin;
   account for lane count, bits per pixel, DSC slice compression and protocol
   overhead. Do not copy a rate from another DDIC without checking its PHY
   timing table and lock range.
4. Reuse a known-good DDIC command index only when the panel is confirmed to
   accept host-following video timing. Panels that require a frame-rate DCS
   command need a panel-specific command path; host timing alone may only
   change the framework label.
5. Extend the mode mapping, enumeration count, timing copies, and validation
   tables consistently for both resolutions. Verify that official modes still
   restore their native link and porch values.
6. Test one new mode at a time, then test every transition in both directions.
   Check hardware refresh, TE behavior, blanking, flicker/white-screen faults,
   suspend/resume, low-brightness behavior, and recovery after a failed load.

A panel's published maximum is not proof that a higher host rate is safe.
Thermal, PHY, DDIC, touch synchronization and power-budget limits remain the
responsibility of the device integrator.

## MTK Porting Notes

For another MTK device, first identify the exact SoC kernel/module branch and
panel driver. Confirm that it is a VDO panel whose DDIC can follow host video
timing; a command-mode panel or a panel with a hard-coded internal frame-rate
generator requires a different approach. Then derive the private structure
layouts and callback prototypes from that device's display source, regenerate
symbol CRCs from its kernel build, and replace every panel/mode validation
constant. Never bypass validation merely to make `insmod` return success.

The source in this repository is therefore a PMB110 reference baseline, not a
drop-in MTK overclock framework. A successful compile on another device does
not imply ABI or panel compatibility.
