#!/bin/sh
set -eu

# Build the PMB110 full-link-rate source against the exact target kernel ABI.
# All large, device-specific inputs stay outside this public repository.

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
headers_dir=${PMB110_HEADERS_DIR:-}
kernel_source=${PMB110_KERNEL_SOURCE:-}
display_root=${PMB110_DISPLAY_ROOT:-}
clang=${PMB110_CLANG:-}
linker=${PMB110_LD_LLD:-}
serial_fnv64=${PMB110_SERIAL_FNV64:-}
module_name=${PMB110_MODULE_NAME:-pmb110_170_mode}
output_dir=${PMB110_OUTPUT_DIR:-$project_root/out}
source_file=$project_root/src/PMB110_185_Mode.c

fail() {
	echo "error: $*" >&2
	exit 1
}

[ -n "$headers_dir" ] || fail "PMB110_HEADERS_DIR is required"
[ -n "$kernel_source" ] || fail "PMB110_KERNEL_SOURCE is required"
[ -n "$display_root" ] || fail "PMB110_DISPLAY_ROOT is required"
[ -n "$clang" ] || fail "PMB110_CLANG is required"
[ -n "$linker" ] || fail "PMB110_LD_LLD is required"
[ -n "$serial_fnv64" ] || fail "PMB110_SERIAL_FNV64 is required; keep the device value private"
[ -f "$source_file" ] || fail "missing source: $source_file"
[ -f "$headers_dir/include/linux/compiler-version.h" ] || fail "incomplete kernel headers: $headers_dir"
[ -f "$headers_dir/include/linux/kconfig.h" ] || fail "incomplete kernel headers: $headers_dir"
[ -f "$kernel_source/scripts/module.lds.S" ] || fail "missing module linker script in: $kernel_source"
[ -f "$display_root/drivers/gpu/drm/mediatek/mediatek_v2/mtk_dsi.h" ] || \
	fail "missing MTK DSI ABI header below: $display_root"
[ -x "$clang" ] || fail "clang is not executable: $clang"
[ -x "$linker" ] || fail "ld.lld is not executable: $linker"

serial_hex=${serial_fnv64#0x}
serial_hex=${serial_hex%ULL}
[ "${#serial_hex}" -eq 16 ] || fail "PMB110_SERIAL_FNV64 must contain 16 hexadecimal digits"
case "$serial_hex" in
	*[!0-9a-fA-F]*) fail "PMB110_SERIAL_FNV64 is not hexadecimal" ;;
esac
serial_define="-DPMB110_SERIAL_FNV64=0x${serial_hex}ULL"

compiler_version=$("$clang" --version | sed -n '1p')
case "$compiler_version" in
	*"based on r536225"*"clang version 19.0.1"*) ;;
	*) fail "unexpected compiler: $compiler_version" ;;
esac

display_driver=$display_root/drivers/gpu/drm/mediatek/mediatek_v2
display_include=$display_root/include
interconnect_dir=$display_root/drivers/misc/mediatek/mtk-interconnect
cmdq_dir=$display_root/drivers/misc/mediatek/cmdq/mailbox
mkdir -p "$output_dir"

# Generate the exact module linker script from the target kernel tree.
"$clang" \
	--target=aarch64-linux-gnu \
	-E -P -x assembler-with-cpp \
	-D__KERNEL__ -D__ASSEMBLY__ \
	-include "$headers_dir/include/linux/compiler-version.h" \
	-include "$headers_dir/include/linux/kconfig.h" \
	-I"$headers_dir/arch/arm64/include" \
	-I"$headers_dir/arch/arm64/include/generated" \
	-I"$headers_dir/include" \
	-I"$headers_dir/arch/arm64/include/uapi" \
	-I"$headers_dir/arch/arm64/include/generated/uapi" \
	-I"$headers_dir/include/uapi" \
	-I"$headers_dir/include/generated/uapi" \
	"$kernel_source/scripts/module.lds.S" \
	-o "$output_dir/module.lds"

# Keep these flags identical to the validated PMB110 build. In particular,
# KCFI and PAC/BTI-related options must match the target kernel toolchain.
# shellcheck disable=SC2086
"$clang" \
	--target=aarch64-linux-gnu \
	-std=gnu11 -O2 \
	-D__KERNEL__ -DMODULE \
	-DCONFIG_MTK_CMDQ_MBOX_EXT=1 \
	-"DKBUILD_MODNAME=\"$module_name\"" \
	-"DKBUILD_BASENAME=\"$module_name\"" \
	$serial_define \
	-include "$headers_dir/include/linux/compiler-version.h" \
	-include "$headers_dir/include/linux/kconfig.h" \
	-I"$headers_dir/arch/arm64/include" \
	-I"$headers_dir/arch/arm64/include/generated" \
	-I"$headers_dir/include" \
	-I"$headers_dir/arch/arm64/include/uapi" \
	-I"$headers_dir/arch/arm64/include/generated/uapi" \
	-I"$headers_dir/include/uapi" \
	-I"$headers_dir/include/generated/uapi" \
	-I"$display_driver" \
	-I"$interconnect_dir" \
	-I"$display_include" \
	-I"$cmdq_dir" \
	-fno-pic -fno-PIE -fno-common -fno-builtin \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-delete-null-pointer-checks \
	-fno-strict-overflow -fno-optimize-sibling-calls \
	-fno-omit-frame-pointer -ffixed-x18 \
	-fsanitize=kcfi \
	-fsanitize-cfi-icall-experimental-normalize-integers \
	-mbranch-protection=pac-ret -mgeneral-regs-only -mstrict-align \
	-mno-outline-atomics -mcmodel=large \
	-c "$source_file" \
	-o "$output_dir/$module_name.o"

"$linker" \
	-r -m aarch64elf -z noexecstack --build-id=sha1 \
	-T "$output_dir/module.lds" \
	-o "$output_dir/PMB110_185_Mode.ko" \
	"$output_dir/$module_name.o"

echo "compiler: $compiler_version"
echo "source:   $source_file"
echo "output:   $output_dir/PMB110_185_Mode.ko"
