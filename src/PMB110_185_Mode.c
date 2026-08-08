#define OPLUS_FEATURE_DISPLAY 1
#define OPLUS_FEATURE_DISPLAY_ADFR 1
#define OPLUS_FEATURE_DISPLAY_HPWM 1
#define OPLUS_FEATURE_DISPLAY_APOLLO 1
#define OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT 1
#define OPLUS_FEATURE_DISPLAY_TEMP_COMPENSATION 1
#define OPLUS_FEATURE_DISPLAY_MAINLINE 1
#define OPLUS_TRACKPOINT_REPORT 1
#define OPLUS_DISPLAY_FEATURE_DMR 1

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/ptrace.h>
#include <linux/uaccess.h>

#define DRM_CMDQ_DISABLE 1
/* mtk_dsi only needs this opaque CRTC pointer and CMDQ buffer ABI here. */
#define MTK_DRM_CRTC_H
#define DSI_CMD_V2_SCN_NUM 6
struct mtk_drm_crtc;
#include "mtk_dsi.h"

#define PMB110_VERMAGIC \
	"6.12.58-android16-6-g7704a1ae279b-ab15213644-4k " \
	"SMP preempt mod_unload modversions aarch64"

#define PMB110_MODE_COUNT 6
#define PMB110_RES_COUNT 2
#define PMB110_STOCK_MODES (PMB110_MODE_COUNT * PMB110_RES_COUNT)
#define PMB110_CUSTOM_MODES 4
#define PMB110_TOTAL_MODES (PMB110_STOCK_MODES + PMB110_CUSTOM_MODES)
#define PMB110_FULL_165_INDEX 4
#define PMB110_REDUCED_165_INDEX 10
#define PMB110_FULL_170_INDEX 12
#define PMB110_REDUCED_170_INDEX 13
#define PMB110_FULL_185_INDEX 14
#define PMB110_REDUCED_185_INDEX 15
#define PMB110_185_DATA_RATE 1496
#define PMB110_185_PLL_CLK 748
#define PMB110_BASE_DATA_RATE 1374
#define PMB110_FHD_SDC165 6
#define PMB110_PANEL_FUNCS_SCAN_BYTES 2048

extern unsigned long (*_kallsyms_lookup_name)(const char *name);

struct pmb110_panel_info {
	char panel_name[512];
	char manufacture[32];
	char vendor[32];
	u8 panel_flag;
	u8 panel_id1;
	u8 panel_id2;
	u8 panel_id3;
};

struct pmb110_panel_backlight {
	u32 default_percent;
	u32 default_level;
	u32 normal_max;
	u32 hw_max;
	u32 hw_min;
};

struct pmb110_panel_data {
	struct drm_display_mode mode;
	struct mtk_panel_params *ext_params;
	void *dsi_timing_cmds;
};

struct pmb110_lcm_prefix {
	void *dev;
	struct drm_panel panel;
	struct pmb110_panel_info panel_info;
	struct pmb110_panel_backlight backlight_info;
	void *backlight;
	void *reset_gpio;
	void *bias_pos;
	void *bias_neg;
	void *bias_gpio;
	void *vddr_aod_enable_gpio;
	struct pmb110_panel_data *oplus_panel_data;
	struct pmb110_panel_data *cur_data;
	struct pmb110_panel_data *last_data;
	struct drm_display_mode *m;
	void *parser_utils;
	void *te_switch_gpio;
	void *te_out_gpio;
	int mode_num;
	int res_switch;
};

struct pmb110_enum_probe_data {
	bool custom_mode;
};

typedef int (*pmb110_vdo_update_fn_t)(struct drm_connector *connector,
	unsigned int cur_mode, unsigned int dst_mode);

static int pmb110_mode_switch_update_for_vdo(
	struct drm_connector *connector, unsigned int cur_mode,
	unsigned int dst_mode);
static int call_dsi_io(enum mtk_ddp_io_cmd command, void *params);

struct pmb110_state {
	struct pmb110_lcm_prefix *ctx;
	struct pmb110_panel_data *panel_data;
	struct pmb110_panel_data custom[PMB110_CUSTOM_MODES];
	struct mtk_panel_params sticky_ext[PMB110_STOCK_MODES];
	struct mtk_panel_params custom_ext[PMB110_CUSTOM_MODES];
	struct mtk_panel_params *original_ext[PMB110_STOCK_MODES];
	struct mtk_ddp_comp *comp;
	struct mtk_dsi *dsi;
	int (*original_mapping)(int mode_idx);
	pmb110_vdo_update_fn_t original_vdo_update;
	pmb110_vdo_update_fn_t *vdo_update_slot;
	struct kprobe porch_probe;
	struct kretprobe enum_probe;
	bool porch_registered;
	bool enum_registered;
	bool installed;
	bool self_ref;
	bool stock_ext_patched;
	bool link_boosted;
	unsigned long captures;
	unsigned long enum_fixes;
	unsigned long clock_attempts;
	unsigned long clock_switches;
	unsigned int install_stage;
	int last_error;
	char status[512];
};

static struct pmb110_state state;
static DEFINE_MUTEX(control_lock);
static bool boot_mode;

static bool panel_name_matches(const char *name)
{
	static const char expected[] = "panel_aa618_p_3_a0034_dsi_vdo";
	size_t i;

	for (i = 0; i < sizeof(expected); i++) {
		if (name[i] != expected[i])
			return false;
	}

	return true;
}

static unsigned int mode_refresh(const struct drm_display_mode *mode)
{
	u64 pixels;

	if (!mode || mode->clock <= 0 || !mode->htotal || !mode->vtotal)
		return 0;

	pixels = (u64)mode->htotal * mode->vtotal;
	return (unsigned int)(((u64)mode->clock * 1000ULL + pixels / 2) /
		pixels);
}

static bool mode_matches(const struct drm_display_mode *mode, u16 hdisplay,
	u16 vdisplay, u16 hsync_start, u16 hsync_end, u16 htotal,
	u16 vsync_start, u16 vsync_end, u16 vtotal, int clock)
{
	return mode->hdisplay == hdisplay && mode->vdisplay == vdisplay &&
		mode->hsync_start == hsync_start &&
		mode->hsync_end == hsync_end && mode->htotal == htotal &&
		mode->vsync_start == vsync_start &&
		mode->vsync_end == vsync_end && mode->vtotal == vtotal &&
		mode->clock == clock && mode->hskew == 0;
}

static bool is_custom_data(const struct pmb110_panel_data *data)
{
	int i;

	for (i = 0; i < PMB110_CUSTOM_MODES; i++) {
		if (data == &state.custom[i])
			return true;
	}

	return false;
}

static bool is_custom_mode(const struct drm_display_mode *mode)
{
	int i;

	for (i = 0; i < PMB110_CUSTOM_MODES; i++) {
		if (mode == &state.custom[i].mode)
			return true;
	}

	return false;
}

static bool dyn_mipi_is_zero(const struct dynamic_mipi_params *dyn)
{
	return dyn->switch_en == 0 && dyn->pll_clk == 0 &&
		dyn->data_rate == 0 && dyn->data_rate_khz == 0 &&
		dyn->vsa == 0 && dyn->vbp == 0 && dyn->vfp == 0 &&
		dyn->vfp_lp_dyn == 0 && dyn->hsa == 0 && dyn->hbp == 0 &&
		dyn->hfp == 0 && dyn->max_vfp_for_msync_dyn == 0;
}

static int stock_data_index(const struct pmb110_panel_data *data)
{
	int i;

	for (i = 0; i < PMB110_STOCK_MODES; i++) {
		if (data == &state.panel_data[i])
			return i;
	}

	return -EINVAL;
}

static bool is_managed_ext(const struct mtk_panel_params *params)
{
	int i;

	for (i = 0; i < PMB110_STOCK_MODES; i++) {
		if (params == &state.sticky_ext[i])
			return true;
	}
	for (i = 0; i < PMB110_CUSTOM_MODES; i++) {
		if (params == &state.custom_ext[i])
			return true;
	}

	return false;
}

static int validate_panel(struct pmb110_lcm_prefix *ctx)
{
	struct pmb110_lcm_prefix prefix;
	struct pmb110_panel_data data;
	int i;

	if (copy_from_kernel_nofault(&prefix, ctx, sizeof(prefix)))
		return -EFAULT;

	prefix.panel_info.panel_name[sizeof(prefix.panel_info.panel_name) - 1] =
		'\0';
	if (!panel_name_matches(prefix.panel_info.panel_name) ||
		prefix.mode_num != PMB110_MODE_COUNT ||
		prefix.res_switch != PMB110_RES_COUNT ||
		!prefix.oplus_panel_data)
		return -ENODEV;

	for (i = 0; i < PMB110_STOCK_MODES; i++) {
		if (copy_from_kernel_nofault(&data,
			prefix.oplus_panel_data + i, sizeof(data)) ||
			!data.ext_params || !data.dsi_timing_cmds)
			return -EFAULT;
	}

	if (copy_from_kernel_nofault(&data,
		prefix.oplus_panel_data + PMB110_FULL_165_INDEX, sizeof(data)))
		return -EFAULT;
	if (!mode_matches(&data.mode, 1272, 2772, 1280, 1296, 1312,
		2928, 2930, 2976, 644245) || mode_refresh(&data.mode) != 165)
		return -ESTALE;

	if (copy_from_kernel_nofault(&data,
		prefix.oplus_panel_data + PMB110_REDUCED_165_INDEX,
		sizeof(data)))
		return -EFAULT;
	if (!mode_matches(&data.mode, 1080, 2354, 1088, 1104, 1120,
		2510, 2512, 2558, 472719) || mode_refresh(&data.mode) != 165)
		return -ESTALE;

	state.panel_data = prefix.oplus_panel_data;
	return 0;
}

static int validate_stock_connector_locked(struct mtk_dsi *dsi)
{
	struct drm_display_mode *mode;
	unsigned int seen = 0;
	int count = 0;
	int i;

	list_for_each_entry(mode, &dsi->conn.modes, head) {
		bool found = false;

		if (++count > PMB110_STOCK_MODES)
			return -E2BIG;

		for (i = 0; i < PMB110_STOCK_MODES; i++) {
			if (mode != &state.panel_data[i].mode)
				continue;
			if (seen & BIT(i))
				return -EEXIST;
			seen |= BIT(i);
			found = true;
			break;
		}
		if (!found)
			return -ESTALE;
	}

	if (count != PMB110_STOCK_MODES || seen != GENMASK(11, 0))
		return -EINVAL;

	return 0;
}

static int validate_installed_connector_locked(struct mtk_dsi *dsi)
{
	struct drm_display_mode *mode;
	int count = 0;

	list_for_each_entry(mode, &dsi->conn.modes, head) {
		if (count < PMB110_STOCK_MODES) {
			int i;
			bool found = false;

			for (i = 0; i < PMB110_STOCK_MODES; i++) {
				if (mode == &state.panel_data[i].mode) {
					found = true;
					break;
				}
			}
			if (!found)
				return -ESTALE;
		} else if (count < PMB110_TOTAL_MODES) {
			if (mode != &state.custom[count - PMB110_STOCK_MODES].mode)
				return -ESTALE;
		} else {
			return -E2BIG;
		}
		count++;
	}

	return count == PMB110_TOTAL_MODES ? 0 : -EINVAL;
}

static int prepare_custom_modes(void)
{
	u64 scaled_vtotal;
	u32 vbp;
	u32 vsa;
	int i;

	for (i = 0; i < PMB110_STOCK_MODES; i++) {
		struct drm_display_mode *mode = &state.panel_data[i].mode;

		state.original_ext[i] = state.panel_data[i].ext_params;
		if (!state.original_ext[i] ||
			copy_from_kernel_nofault(&state.sticky_ext[i],
				state.original_ext[i], sizeof(state.sticky_ext[i])))
			return -EFAULT;
		if ((state.sticky_ext[i].data_rate != 0 &&
			state.sticky_ext[i].data_rate != PMB110_BASE_DATA_RATE) ||
			(state.sticky_ext[i].data_rate == 0 &&
			state.sticky_ext[i].pll_clk * 2 !=
				PMB110_BASE_DATA_RATE) ||
			!dyn_mipi_is_zero(&state.sticky_ext[i].dyn))
			return -ESTALE;

		vsa = mode->vsync_end - mode->vsync_start;
		vbp = mode->vtotal - mode->vsync_end;
		scaled_vtotal = DIV_ROUND_CLOSEST_ULL(
			(u64)mode->vtotal * PMB110_185_DATA_RATE,
			PMB110_BASE_DATA_RATE);
		if (scaled_vtotal <= (u64)mode->vdisplay + vsa + vbp ||
			scaled_vtotal > U32_MAX)
			return -ERANGE;

		state.sticky_ext[i].dyn.switch_en = 1;
		state.sticky_ext[i].dyn.pll_clk = PMB110_185_PLL_CLK;
		state.sticky_ext[i].dyn.data_rate = PMB110_185_DATA_RATE;
		state.sticky_ext[i].dyn.vfp = (u32)scaled_vtotal -
			mode->vdisplay - vsa - vbp;
	}

	state.custom[0] = state.panel_data[PMB110_FULL_165_INDEX];
	state.custom[0].mode.clock = 701461;
	state.custom[0].mode.vsync_start = 3098;
	state.custom[0].mode.vsync_end = 3100;
	state.custom[0].mode.vtotal = 3146;
	state.custom[0].mode.type = DRM_MODE_TYPE_DRIVER;
	state.custom[0].mode.expose_to_userspace = false;
	INIT_LIST_HEAD(&state.custom[0].mode.head);
	drm_mode_set_name(&state.custom[0].mode);

	state.custom[1] = state.panel_data[PMB110_REDUCED_165_INDEX];
	state.custom[1].mode.clock = 514271;
	state.custom[1].mode.vsync_start = 2653;
	state.custom[1].mode.vsync_end = 2655;
	state.custom[1].mode.vtotal = 2701;
	state.custom[1].mode.type = DRM_MODE_TYPE_DRIVER;
	state.custom[1].mode.expose_to_userspace = false;
	INIT_LIST_HEAD(&state.custom[1].mode.head);
	drm_mode_set_name(&state.custom[1].mode);

	state.custom_ext[0] = state.sticky_ext[PMB110_FULL_165_INDEX];
	state.custom_ext[0].dyn.vfp = state.custom[0].mode.vsync_start -
		state.custom[0].mode.vdisplay;
	state.custom_ext[0].dyn_fps.vact_timing_fps = 170;
	state.custom_ext[0].dyn_fps.data_rate = 0;
	state.custom_ext[0].dyn_fps.data_rate_khz = 0;
	state.custom[0].ext_params = &state.custom_ext[0];

	state.custom_ext[1] = state.sticky_ext[PMB110_REDUCED_165_INDEX];
	state.custom_ext[1].dyn.vfp = state.custom[1].mode.vsync_start -
		state.custom[1].mode.vdisplay;
	state.custom_ext[1].dyn_fps.vact_timing_fps = 170;
	state.custom_ext[1].dyn_fps.data_rate = 0;
	state.custom_ext[1].dyn_fps.data_rate_khz = 0;
	state.custom[1].ext_params = &state.custom_ext[1];

	state.custom[2] = state.panel_data[PMB110_FULL_165_INDEX];
	state.custom[2].mode.clock = 701461;
	state.custom[2].mode.vsync_start = 2842;
	state.custom[2].mode.vsync_end = 2844;
	state.custom[2].mode.vtotal = 2890;
	state.custom[2].mode.type = DRM_MODE_TYPE_DRIVER;
	state.custom[2].mode.expose_to_userspace = false;
	INIT_LIST_HEAD(&state.custom[2].mode.head);
	drm_mode_set_name(&state.custom[2].mode);
	state.custom_ext[2] = state.sticky_ext[PMB110_FULL_165_INDEX];
	state.custom_ext[2].dyn.vfp = state.custom[2].mode.vsync_start -
		state.custom[2].mode.vdisplay;
	state.custom_ext[2].dyn_fps.vact_timing_fps = 185;
	state.custom_ext[2].dyn_fps.data_rate = 0;
	state.custom_ext[2].dyn_fps.data_rate_khz = 0;
	state.custom[2].ext_params = &state.custom_ext[2];

	state.custom[3] = state.panel_data[PMB110_REDUCED_165_INDEX];
	state.custom[3].mode.clock = 514271;
	state.custom[3].mode.vsync_start = 2434;
	state.custom[3].mode.vsync_end = 2436;
	state.custom[3].mode.vtotal = 2482;
	state.custom[3].mode.type = DRM_MODE_TYPE_DRIVER;
	state.custom[3].mode.expose_to_userspace = false;
	INIT_LIST_HEAD(&state.custom[3].mode.head);
	drm_mode_set_name(&state.custom[3].mode);
	state.custom_ext[3] = state.sticky_ext[PMB110_REDUCED_165_INDEX];
	state.custom_ext[3].dyn.vfp = state.custom[3].mode.vsync_start -
		state.custom[3].mode.vdisplay;
	state.custom_ext[3].dyn_fps.vact_timing_fps = 185;
	state.custom_ext[3].dyn_fps.data_rate = 0;
	state.custom_ext[3].dyn_fps.data_rate_khz = 0;
	state.custom[3].ext_params = &state.custom_ext[3];

	return 0;
}

static noinline __used int pmb110_scaling_mode_mapping(int mode_idx)
{
	if (mode_idx == PMB110_FULL_170_INDEX ||
		mode_idx == PMB110_REDUCED_170_INDEX)
		return PMB110_FULL_170_INDEX;
	if (mode_idx == PMB110_FULL_185_INDEX ||
		mode_idx == PMB110_REDUCED_185_INDEX)
		return PMB110_FULL_185_INDEX;

	if (mode_idx >= 0 && mode_idx < PMB110_STOCK_MODES &&
		READ_ONCE(state.original_mapping))
		return state.original_mapping(mode_idx);

	return mode_idx;
}

static int read_kcfi_type(const void *function, u32 *type_id)
{
	if (!function || (unsigned long)function < PAGE_SIZE)
		return -EINVAL;

	return copy_from_kernel_nofault(type_id,
		(void *)((unsigned long)function - sizeof(*type_id)),
		sizeof(*type_id)) ? -EFAULT : 0;
}

static int validate_kcfi(const void *live, const void *local)
{
	u32 live_type_id;
	u32 local_type_id;

	if (read_kcfi_type(live, &live_type_id) ||
		read_kcfi_type(local, &local_type_id))
		return -EFAULT;

	return live_type_id == local_type_id ? 0 : -EINVAL;
}

static pmb110_vdo_update_fn_t *find_vdo_update_slot(
	struct mtk_panel_funcs *funcs)
{
	pmb110_vdo_update_fn_t *match = NULL;
	u32 expected_type;
	size_t offset;

	if (read_kcfi_type(pmb110_mode_switch_update_for_vdo,
		&expected_type))
		return NULL;

	for (offset = 0; offset < PMB110_PANEL_FUNCS_SCAN_BYTES;
		offset += sizeof(unsigned long)) {
		pmb110_vdo_update_fn_t candidate;
		void *slot = (void *)((unsigned long)funcs + offset);
		u32 candidate_type;

		if (copy_from_kernel_nofault(&candidate, slot,
			sizeof(candidate)))
			break;
		if (read_kcfi_type(candidate, &candidate_type) ||
			candidate_type != expected_type)
			continue;
		if (match)
			return NULL;
		match = (pmb110_vdo_update_fn_t *)slot;
	}

	return match;
}

static int patch_stock_ext_params(int current_index)
{
	int i;

	if (!state.dsi || !state.dsi->ext || current_index < 0 ||
		current_index >= PMB110_STOCK_MODES)
		return -EINVAL;
	if (state.stock_ext_patched)
		return -EALREADY;
	if (READ_ONCE(state.dsi->ext->params) !=
		state.original_ext[current_index])
		return -ESTALE;
	for (i = 0; i < PMB110_STOCK_MODES; i++) {
		if (READ_ONCE(state.panel_data[i].ext_params) !=
			state.original_ext[i])
			return -ESTALE;
	}

	for (i = 0; i < PMB110_STOCK_MODES; i++)
		WRITE_ONCE(state.panel_data[i].ext_params,
			&state.sticky_ext[i]);
	WRITE_ONCE(state.dsi->ext->params,
		&state.sticky_ext[current_index]);
	state.stock_ext_patched = true;
	return 0;
}

static int restore_stock_ext_params(int current_index)
{
	int i;

	if (!state.dsi || !state.dsi->ext || current_index < 0 ||
		current_index >= PMB110_STOCK_MODES)
		return -EINVAL;
	if (!state.stock_ext_patched)
		return 0;
	if (READ_ONCE(state.dsi->ext->params) !=
		&state.sticky_ext[current_index])
		return -ESTALE;
	for (i = 0; i < PMB110_STOCK_MODES; i++) {
		if (READ_ONCE(state.panel_data[i].ext_params) !=
			&state.sticky_ext[i])
			return -ESTALE;
	}

	WRITE_ONCE(state.dsi->ext->params,
		state.original_ext[current_index]);
	for (i = 0; i < PMB110_STOCK_MODES; i++)
		WRITE_ONCE(state.panel_data[i].ext_params,
			state.original_ext[i]);
	state.stock_ext_patched = false;
	return 0;
}

static int switch_sticky_link(bool enable)
{
	u32 expected_rate = enable ? PMB110_185_DATA_RATE :
		PMB110_BASE_DATA_RATE;
	int hopping = enable ? 1 : 0;
	int ret;

	if (!state.dsi || !state.comp)
		return -ENODEV;

	if (!state.dsi->ext || !state.dsi->ext->params)
		return -ENODEV;
	if (!is_managed_ext(READ_ONCE(state.dsi->ext->params)))
		return -ESTALE;

	state.clock_attempts++;
	ret = call_dsi_io(MIPI_HOPPING, &hopping);
	if (ret < 0)
		return ret;

	WRITE_ONCE(state.link_boosted,
		READ_ONCE(state.dsi->mipi_hopping_sta));
	if (!!READ_ONCE(state.dsi->mipi_hopping_sta) != enable ||
		READ_ONCE(state.dsi->data_rate) != expected_rate)
		return -EIO;
	if (state.dsi->slave_dsi &&
		READ_ONCE(state.dsi->slave_dsi->data_rate) != expected_rate)
		return -EIO;

	state.clock_switches++;
	return 0;
}

static noinline __used int pmb110_mode_switch_update_for_vdo(
	struct drm_connector *connector,
	unsigned int cur_mode, unsigned int dst_mode)
{
	pmb110_vdo_update_fn_t original =
		READ_ONCE(state.original_vdo_update);
	int clock_ret = 0;
	int ret;

	if (!original)
		return -ESTALE;

	ret = original(connector, cur_mode, dst_mode);
	if (!READ_ONCE(state.installed) || !state.dsi ||
		connector != &state.dsi->conn || cur_mode == dst_mode)
		return ret;

	if (!READ_ONCE(state.dsi->mipi_hopping_sta) ||
		(READ_ONCE(state.dsi->data_rate) != 0 &&
		 READ_ONCE(state.dsi->data_rate) != PMB110_185_DATA_RATE))
		clock_ret = switch_sticky_link(true);
	if (clock_ret) {
		WRITE_ONCE(state.last_error, clock_ret);
		pr_err("pmb110_170_mode: sticky link recovery %u->%u failed: %d\n",
			cur_mode, dst_mode, clock_ret);
		return clock_ret;
	}

	return ret;
}

static int call_dsi_io(enum mtk_ddp_io_cmd command, void *params)
{
	if (!state.comp || !state.comp->funcs || !state.comp->funcs->io_cmd)
		return -ENODEV;

	return mtk_ddp_comp_io_cmd(state.comp, NULL, command, params);
}

static void notify_mode_change(void)
{
	if (state.dsi)
		drm_kms_helper_connector_hotplug_event(&state.dsi->conn);
	if (state.dsi && state.dsi->conn.dev)
		drm_kms_helper_hotplug_event(state.dsi->conn.dev);
}

static int rebuild_driver_modes(void)
{
	int ret;

	ret = call_dsi_io(DSI_SET_CRTC_AVAIL_MODES,
		state.comp->mtk_crtc);
	if (ret < 0)
		return ret;

	ret = call_dsi_io(DSI_SET_CRTC_SCALING_MODE_MAPPING,
		state.comp->mtk_crtc);
	if (ret < 0)
		return ret;

	ret = call_dsi_io(DSI_FILL_CONNECTOR_PROP_CAPS,
		state.comp->mtk_crtc);
	return ret < 0 ? ret : 0;
}

static int install_modes(void)
{
	struct pmb110_lcm_prefix prefix;
	struct mtk_panel_funcs *funcs;
	unsigned long io_symbol;
	int current_index;
	int i;
	int ret;
	int cleanup_ret;

	if (READ_ONCE(state.installed))
		return 0;
	if (!READ_ONCE(state.comp))
		return -EAGAIN;

	state.install_stage = 1;
	ret = validate_panel(state.ctx);
	if (ret)
		return ret;
	state.install_stage = 2;
	if (copy_from_kernel_nofault(&prefix, state.ctx, sizeof(prefix)))
		return -EFAULT;
	if (!prefix.cur_data || is_custom_data(prefix.cur_data) ||
		(!boot_mode && mode_refresh(&prefix.cur_data->mode) != 144))
		return -EBUSY;
	current_index = stock_data_index(prefix.cur_data);
	if (current_index < 0)
		return current_index;

	state.dsi = container_of(state.comp, struct mtk_dsi, ddp_comp);
	state.install_stage = 3;
	if (state.dsi->panel != &state.ctx->panel ||
		!state.dsi->conn.dev || !state.dsi->ext ||
		!state.dsi->ext->funcs || !state.comp->mtk_crtc)
		return -ENODEV;

	io_symbol = _kallsyms_lookup_name("mtk_dsi_io_cmd");
	state.install_stage = 4;
	if (!io_symbol || io_symbol !=
		(unsigned long)state.comp->funcs->io_cmd)
		return -ESTALE;

	funcs = state.dsi->ext->funcs;
	state.install_stage = 5;
	if (!funcs->scaling_mode_mapping)
		return -EOPNOTSUPP;
	state.original_mapping = funcs->scaling_mode_mapping;
	state.install_stage = 6;
	state.vdo_update_slot = find_vdo_update_slot(funcs);
	if (!state.vdo_update_slot)
		return -ESTALE;
	state.original_vdo_update = READ_ONCE(*state.vdo_update_slot);
	state.install_stage = 7;
	ret = validate_kcfi(state.original_mapping,
		pmb110_scaling_mode_mapping);
	if (ret)
		return ret;
	state.install_stage = 8;
	ret = validate_kcfi(state.original_vdo_update,
		pmb110_mode_switch_update_for_vdo);
	if (ret)
		return ret;
	state.install_stage = 9;
	if (READ_ONCE(state.dsi->d_rate) != 0 ||
		READ_ONCE(state.dsi->mipi_hopping_sta) ||
		(READ_ONCE(state.dsi->data_rate) != 0 &&
		READ_ONCE(state.dsi->data_rate) != PMB110_BASE_DATA_RATE))
		return -EBUSY;

	state.install_stage = 10;
	ret = prepare_custom_modes();
	if (ret)
		return ret;
	if (mode_refresh(&state.custom[0].mode) != 170 ||
		mode_refresh(&state.custom[1].mode) != 170 ||
		mode_refresh(&state.custom[2].mode) != 185 ||
		mode_refresh(&state.custom[3].mode) != 185)
		return -ERANGE;
	state.install_stage = 11;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	state.self_ref = true;

	mutex_lock(&state.dsi->modes_lock);
	state.install_stage = 12;
	ret = validate_stock_connector_locked(state.dsi);
	if (!ret)
		ret = patch_stock_ext_params(current_index);
	if (!ret) {
		state.install_stage = 13;
		ret = switch_sticky_link(true);
	}
	if (!ret) {
		WRITE_ONCE(state.installed, true);
		WRITE_ONCE(funcs->scaling_mode_mapping,
			pmb110_scaling_mode_mapping);
		WRITE_ONCE(*state.vdo_update_slot,
			pmb110_mode_switch_update_for_vdo);
		for (i = 0; i < PMB110_CUSTOM_MODES; i++)
			list_add_tail(&state.custom[i].mode.head,
				&state.dsi->conn.modes);
		state.dsi->modes_cnt = PMB110_TOTAL_MODES;
		state.dsi->max_vrefresh_mode.htotal = 0;
	}
	mutex_unlock(&state.dsi->modes_lock);
	if (ret)
		goto cleanup_link;

	ret = rebuild_driver_modes();
	state.install_stage = 14;
	if (ret)
		goto rollback;

	notify_mode_change();
	scnprintf(state.status, sizeof(state.status),
		"installed=1 captured=1 modes=16 "
		"hz170=12/13 vfp170=326/299 hz185=14/15 vfp185=70/80 "
		"rate=1496 mipi=sticky-dyn-vfp "
		"enum=FHD_SDC165 dtbo=untouched");
	pr_info("pmb110_170_mode: %s\n", state.status);
	return 0;

rollback:
	mutex_lock(&state.dsi->modes_lock);
	for (i = PMB110_CUSTOM_MODES - 1; i >= 0; i--)
		list_del_init(&state.custom[i].mode.head);
	state.dsi->modes_cnt = PMB110_STOCK_MODES;
	state.dsi->max_vrefresh_mode.htotal = 0;
	WRITE_ONCE(funcs->scaling_mode_mapping, state.original_mapping);
	WRITE_ONCE(*state.vdo_update_slot,
		state.original_vdo_update);
	WRITE_ONCE(state.installed, false);
	mutex_unlock(&state.dsi->modes_lock);
	rebuild_driver_modes();

cleanup_link:
	mutex_lock(&state.dsi->modes_lock);
	if (state.stock_ext_patched) {
		if (READ_ONCE(state.dsi->mipi_hopping_sta) ||
			READ_ONCE(state.dsi->data_rate) == PMB110_185_DATA_RATE) {
			cleanup_ret = switch_sticky_link(false);
			if (!ret && cleanup_ret)
				ret = cleanup_ret;
		}
		if (!READ_ONCE(state.dsi->mipi_hopping_sta) &&
			READ_ONCE(state.dsi->data_rate) == PMB110_BASE_DATA_RATE) {
			cleanup_ret = restore_stock_ext_params(current_index);
			if (!ret && cleanup_ret)
				ret = cleanup_ret;
		}
	}
	mutex_unlock(&state.dsi->modes_lock);
put_ref:
	state.original_mapping = NULL;
	state.original_vdo_update = NULL;
	state.vdo_update_slot = NULL;
	state.dsi = NULL;
	state.self_ref = false;
	module_put(THIS_MODULE);
	return ret;
}

static int remove_modes(void)
{
	struct pmb110_lcm_prefix prefix;
	struct mtk_panel_funcs *funcs;
	int current_index;
	int i;
	int ret;
	int recovery_ret;

	if (!READ_ONCE(state.installed))
		return 0;
	if (!state.dsi || !state.original_mapping ||
		!state.original_vdo_update)
		return -ESTALE;
	if (copy_from_kernel_nofault(&prefix, state.ctx, sizeof(prefix)))
		return -EFAULT;
	if (!prefix.cur_data || is_custom_data(prefix.cur_data) ||
		mode_refresh(&prefix.cur_data->mode) != 144)
		return -EBUSY;
	current_index = stock_data_index(prefix.cur_data);
	if (current_index < 0)
		return current_index;
	if (!READ_ONCE(state.link_boosted) ||
		!READ_ONCE(state.dsi->mipi_hopping_sta) ||
		READ_ONCE(state.dsi->d_rate) != 0 ||
		READ_ONCE(state.dsi->data_rate) != PMB110_185_DATA_RATE ||
		!state.stock_ext_patched)
		return -EBUSY;

	funcs = state.dsi->ext->funcs;
	if (READ_ONCE(funcs->scaling_mode_mapping) !=
		pmb110_scaling_mode_mapping ||
		!state.vdo_update_slot ||
		READ_ONCE(*state.vdo_update_slot) !=
		pmb110_mode_switch_update_for_vdo)
		return -ESTALE;
	mutex_lock(&state.dsi->modes_lock);
	ret = validate_installed_connector_locked(state.dsi);
	if (!ret) {
		ret = switch_sticky_link(false);
		if (ret)
			goto unlock;
		ret = restore_stock_ext_params(current_index);
		if (ret) {
			recovery_ret = switch_sticky_link(true);
			if (recovery_ret)
				pr_err("pmb110_170_mode: link recovery after restore failure: %d\n",
					recovery_ret);
			goto unlock;
		}
		for (i = PMB110_CUSTOM_MODES - 1; i >= 0; i--)
			list_del_init(&state.custom[i].mode.head);
		state.dsi->modes_cnt = PMB110_STOCK_MODES;
		state.dsi->max_vrefresh_mode.htotal = 0;
		WRITE_ONCE(funcs->scaling_mode_mapping,
			state.original_mapping);
		WRITE_ONCE(*state.vdo_update_slot,
			state.original_vdo_update);
		WRITE_ONCE(state.installed, false);
		if (state.ctx->last_data == &state.custom[0] ||
			state.ctx->last_data == &state.custom[2])
			state.ctx->last_data =
				&state.panel_data[PMB110_FULL_165_INDEX];
		else if (state.ctx->last_data == &state.custom[1] ||
			state.ctx->last_data == &state.custom[3])
			state.ctx->last_data =
				&state.panel_data[PMB110_REDUCED_165_INDEX];
		if (is_custom_mode(state.ctx->m))
			state.ctx->m = &prefix.cur_data->mode;
	}
unlock:
	mutex_unlock(&state.dsi->modes_lock);
	if (ret)
		return ret;

	ret = rebuild_driver_modes();
	notify_mode_change();
	scnprintf(state.status, sizeof(state.status),
		"installed=0 captured=1 modes=12 last_error=%d "
		"dtbo=untouched",
		ret);
	pr_info("pmb110_170_mode: removed runtime modes ret=%d\n", ret);

	state.original_mapping = NULL;
	state.original_vdo_update = NULL;
	state.vdo_update_slot = NULL;
	state.dsi = NULL;
	state.self_ref = false;
	module_put(THIS_MODULE);
	return ret;
}

static int fake_mode_set(const char *value, const struct kernel_param *kp)
{
	bool requested;
	int ret;

	(void)kp;
	if ((value[0] == '1' || value[0] == 'Y' || value[0] == 'y') &&
		(value[1] == '\0' || value[1] == '\n'))
		requested = true;
	else if ((value[0] == '0' || value[0] == 'N' || value[0] == 'n') &&
		(value[1] == '\0' || value[1] == '\n'))
		requested = false;
	else
		return -EINVAL;

	mutex_lock(&control_lock);
	ret = requested ? install_modes() : remove_modes();
	state.last_error = ret;
	if (ret)
		scnprintf(state.status, sizeof(state.status),
			"installed=%u captured=%u last_error=%d "
				"dtbo=untouched",
			state.installed, state.comp != NULL, ret);
	mutex_unlock(&control_lock);
	return ret;
}

static int fake_mode_get(char *buffer, const struct kernel_param *kp)
{
	(void)kp;
	buffer[0] = READ_ONCE(state.installed) ? '1' : '0';
	buffer[1] = '\n';
	buffer[2] = '\0';
	return 2;
}

static int status_get(char *buffer, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buffer, 4096, "%s captures=%lu enum_fixes=%lu "
		"enum_missed=%d install_stage=%u clock_attempts=%lu "
		"clock_switches=%lu sticky_link=%u ext_patched=%u "
		"data_rate=%u d_rate=%u boot_mode=%u\n",
		state.status, state.captures, state.enum_fixes,
		state.enum_probe.nmissed, state.install_stage,
		state.clock_attempts, state.clock_switches,
		state.link_boosted, state.stock_ext_patched,
		state.dsi ? READ_ONCE(state.dsi->data_rate) : 0,
		state.dsi ? READ_ONCE(state.dsi->d_rate) : 0,
		boot_mode);
}

static const struct kernel_param_ops fake_mode_ops = {
	.set = fake_mode_set,
	.get = fake_mode_get,
};

static const struct kernel_param_ops status_ops = {
	.get = status_get,
};

module_param(boot_mode, bool, 0400);
module_param_cb(fake_mode, &fake_mode_ops, NULL, 0644);
module_param_cb(status, &status_ops, NULL, 0444);

static int capture_primary_dsi(void)
{
	struct pmb110_lcm_prefix prefix;
	struct mipi_dsi_device *dsi_dev;
	struct mipi_dsi_host *host;
	struct drm_panel *panel;
	struct mtk_dsi *dsi;

	if (copy_from_kernel_nofault(&prefix, state.ctx, sizeof(prefix)) ||
		!prefix.dev)
		return -EFAULT;

	dsi_dev = to_mipi_dsi_device((struct device *)prefix.dev);
	if (copy_from_kernel_nofault(&host, &dsi_dev->host, sizeof(host)) ||
		!host)
		return -ENODEV;

	dsi = container_of(host, struct mtk_dsi, host);
	if (copy_from_kernel_nofault(&panel, &dsi->panel, sizeof(panel)) ||
		panel != &state.ctx->panel)
		return -ENODEV;

	if (cmpxchg(&state.comp, NULL, &dsi->ddp_comp) == NULL)
		state.captures++;

	return READ_ONCE(state.comp) == &dsi->ddp_comp ? 0 : -EBUSY;
}

static int porch_pre_handler(struct kprobe *probe, struct pt_regs *regs)
{
	struct mtk_ddp_comp *comp = (struct mtk_ddp_comp *)regs->regs[0];
	struct mtk_dsi *dsi;

	(void)probe;
	if (!comp || READ_ONCE(state.comp))
		return 0;

	dsi = container_of(comp, struct mtk_dsi, ddp_comp);
	if (READ_ONCE(dsi->panel) != &state.ctx->panel)
		return 0;

	if (cmpxchg(&state.comp, NULL, comp) == NULL) {
		state.captures++;
		scnprintf(state.status, sizeof(state.status),
			"installed=0 captured=1 modes=12 ready=1 "
			"dtbo=untouched");
		pr_info("pmb110_170_mode: captured primary DSI component\n");
	}

	return 0;
}

static int enum_entry_handler(struct kretprobe_instance *instance,
	struct pt_regs *regs)
{
	struct pmb110_enum_probe_data *data =
		(struct pmb110_enum_probe_data *)instance->data;
	struct drm_display_mode *mode =
		(struct drm_display_mode *)regs->regs[0];

	data->custom_mode = READ_ONCE(state.installed) &&
		is_custom_mode(mode);
	return 0;
}

static int enum_ret_handler(struct kretprobe_instance *instance,
	struct pt_regs *regs)
{
	struct pmb110_enum_probe_data *data =
		(struct pmb110_enum_probe_data *)instance->data;

	if (data->custom_mode) {
		regs->regs[0] = PMB110_FHD_SDC165;
		state.enum_fixes++;
	}
	return 0;
}

static int __init pmb110_170_mode_init(void)
{
	unsigned long panel_symbol;
	unsigned long porch_symbol;
	unsigned long enum_symbol;
	struct pmb110_lcm_prefix *ctx;
	int ret;

	if (!_kallsyms_lookup_name)
		return -ENOENT;

	panel_symbol = _kallsyms_lookup_name("oplus_display0_params");
	porch_symbol = _kallsyms_lookup_name("mtk_dsi_porch_setting");
	enum_symbol = _kallsyms_lookup_name("get_mode_enum");
	if (!panel_symbol || !porch_symbol || !enum_symbol ||
		copy_from_kernel_nofault(&ctx, (void *)panel_symbol,
			sizeof(ctx)) || !ctx)
		return -ENOENT;

	state.ctx = ctx;
	ret = validate_panel(ctx);
	if (ret)
		return ret;
	if (boot_mode) {
		ret = capture_primary_dsi();
		if (ret)
			return ret;
	}

	state.enum_probe.kp.addr = (kprobe_opcode_t *)enum_symbol;
	state.enum_probe.entry_handler = enum_entry_handler;
	state.enum_probe.handler = enum_ret_handler;
	state.enum_probe.maxactive = 32;
	state.enum_probe.data_size = sizeof(struct pmb110_enum_probe_data);
	ret = register_kretprobe(&state.enum_probe);
	if (ret)
		return ret;
	state.enum_registered = true;

	state.porch_probe.addr = (kprobe_opcode_t *)porch_symbol;
	state.porch_probe.pre_handler = porch_pre_handler;
	ret = register_kprobe(&state.porch_probe);
	if (ret) {
		unregister_kretprobe(&state.enum_probe);
		state.enum_registered = false;
		return ret;
	}
	state.porch_registered = true;

	if (READ_ONCE(state.comp)) {
		scnprintf(state.status, sizeof(state.status),
			"installed=0 captured=1 modes=12 ready=1 "
				"fake_mode_default=0 boot_mode=1 "
			"dtbo=untouched");
		pr_info("pmb110_170_mode: loaded boot-ready disabled\n");
	} else {
		scnprintf(state.status, sizeof(state.status),
			"installed=0 captured=0 modes=12 ready=0 "
				"fake_mode_default=0 boot_mode=0 "
			"dtbo=untouched");
		pr_info("pmb110_170_mode: loaded disabled; switch mode via "
			"SurfaceFlinger to capture DSI\n");
	}
	return 0;
}

static void __exit pmb110_170_mode_exit(void)
{
	if (state.porch_registered) {
		unregister_kprobe(&state.porch_probe);
		state.porch_registered = false;
	}
	if (state.enum_registered) {
		unregister_kretprobe(&state.enum_probe);
		state.enum_registered = false;
	}

	pr_info("pmb110_170_mode: unloaded captures=%lu enum_fixes=%lu "
		"clock_switches=%lu\n", state.captures, state.enum_fixes,
		state.clock_switches);
}

module_init(pmb110_170_mode_init);
module_exit(pmb110_170_mode_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smartisan_Apple_Kt");
MODULE_DESCRIPTION("Runtime-only PMB110 SurfaceFlinger-visible 170/185Hz modes");
MODULE_INFO(name, KBUILD_MODNAME);
MODULE_INFO(depends, "");
MODULE_INFO(vermagic, PMB110_VERMAGIC);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x797f2b3e, "module_layout" },
	{ 0x16b5b21d, "_printk" },
	{ 0x2aacc14e, "copy_from_kernel_nofault" },
	{ 0x6fabd45b, "__list_add_valid_or_report" },
	{ 0x802f8919, "__list_del_entry_valid_or_report" },
	{ 0xb8c8345a, "alt_cb_patch_nops" },
	{ 0x8a7493b2, "memcpy" },
	{ 0xf4386284, "scnprintf" },
	{ 0x995658e3, "mutex_lock" },
	{ 0x995658e3, "mutex_unlock" },
	{ 0x54518962, "try_module_get" },
	{ 0x9ccb4024, "module_put" },
	{ 0xe061c9d9, "drm_mode_set_name" },
	{ 0xf60a5b17, "drm_kms_helper_connector_hotplug_event" },
	{ 0x792a27c3, "drm_kms_helper_hotplug_event" },
	{ 0xcca4d86f, "register_kprobe" },
	{ 0x1d045bd3, "unregister_kprobe" },
	{ 0x2a38fe47, "register_kretprobe" },
	{ 0x3d5e2716, "unregister_kretprobe" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x797f2b3e,
	0x16b5b21d,
	0x2aacc14e,
	0x6fabd45b,
	0x802f8919,
	0xb8c8345a,
	0x8a7493b2,
	0xf4386284,
	0x995658e3,
	0x995658e3,
	0x54518962,
	0x9ccb4024,
	0xe061c9d9,
	0xf60a5b17,
	0x792a27c3,
	0xcca4d86f,
	0x1d045bd3,
	0x2a38fe47,
	0x3d5e2716,
};

static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"module_layout\0"
	"_printk\0"
	"copy_from_kernel_nofault\0"
	"__list_add_valid_or_report\0"
	"__list_del_entry_valid_or_report\0"
	"alt_cb_patch_nops\0"
	"memcpy\0"
	"scnprintf\0"
	"mutex_lock\0"
	"mutex_unlock\0"
	"try_module_get\0"
	"module_put\0"
	"drm_mode_set_name\0"
	"drm_kms_helper_connector_hotplug_event\0"
	"drm_kms_helper_hotplug_event\0"
	"register_kprobe\0"
	"unregister_kprobe\0"
	"register_kretprobe\0"
	"unregister_kretprobe\0";
