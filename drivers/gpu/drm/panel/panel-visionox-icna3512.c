// SPDX-License-Identifier: GPL-2.0
/*
 * Visionox ICNA3512 MIPI-DSI panel driver
 *
 * Copyright 2019 NXP
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

/* Panel specific color-format bits */
#define COL_FMT_16BPP 0x55
#define COL_FMT_18BPP 0x66
#define COL_FMT_24BPP 0x77

/* Write Manufacture Command Set Control */
#define WRMAUCCTR 0xFE

static const u32 visionox_bus_formats[] = {
	MEDIA_BUS_FMT_RGB888_1X24,
	MEDIA_BUS_FMT_RGB666_1X18,
	MEDIA_BUS_FMT_RGB565_1X16,
};

static const u32 visionox_bus_flags = DRM_BUS_FLAG_DE_LOW |
				      DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE;

struct visionox_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;

	struct gpio_desc *reset;
	struct backlight_device *backlight;

	struct regulator_bulk_data *supplies;
	unsigned int num_supplies;

	bool prepared;
	bool enabled;

	const struct visionox_platform_data *pdata;
};

struct visionox_platform_data {
	int (*enable)(struct visionox_panel *panel);
};

static const struct drm_display_mode default_mode = {
	.clock = 150000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 92,
	.hsync_end = 1080 + 92 + 8,
	.htotal = 1080 + 92 + 8 + 100,
	.vdisplay = 1920,
	.vsync_start = 1920 + 8,
	.vsync_end = 1920 + 8 + 4,
	.vtotal = 1920 + 8 + 4 + 12,
	.width_mm = 87,
	.height_mm = 155,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static inline struct visionox_panel *to_visionox_panel(struct drm_panel *panel)
{
	return container_of(panel, struct visionox_panel, panel);
}

static int visionox_panel_prepare(struct drm_panel *panel)
{
	struct visionox_panel *visionox = to_visionox_panel(panel);
	struct mipi_dsi_device *dsi = visionox->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dev_info(dev, "visionox_panel_prepare\n");

	if (visionox->prepared)
		return 0;

	ret = regulator_bulk_enable(visionox->num_supplies, visionox->supplies);
	if (ret)
		return ret;

	/* At lest 10ms needed between power-on and reset-out as RM specifies */
	usleep_range(10000, 12000);

	if (visionox->reset) {
		/* Ensure reset is asserted first */
		dev_info(dev, "visionox_panel_prepare: assert reset (set to 1)\n");
		gpiod_set_value_cansleep(visionox->reset, 1);
		msleep(20);  /* Hold reset for 20ms */

		/* Now deassert reset to release the panel */
		dev_info(dev, "visionox_panel_prepare: deassert reset (set to 0)\n");
		gpiod_set_value_cansleep(visionox->reset, 0);
		/*
		 * 120ms delay after reset-out, as per manufacturer initalization
		 * sequence.
		 */
		msleep(120);
	}

	visionox->prepared = true;

	return 0;
}

static int visionox_panel_unprepare(struct drm_panel *panel)
{
	struct visionox_panel *visionox = to_visionox_panel(panel);
	struct mipi_dsi_device *dsi = visionox->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dev_info(dev, "visionox_panel_unprepare\n");

	if (!visionox->prepared)
		return 0;

	/*
	 * Right after asserting the reset, we need to release it, so that the
	 * touch driver can have an active connection with the touch controller
	 * even after the display is turned off.
	 */
	if (visionox->reset) {
		gpiod_set_value_cansleep(visionox->reset, 1);
		usleep_range(15000, 17000);
		gpiod_set_value_cansleep(visionox->reset, 0);
	}

	ret = regulator_bulk_disable(visionox->num_supplies,
				     visionox->supplies);
	if (ret)
		return ret;

	visionox->prepared = false;

	return 0;
}

static int icna3512_enable(struct visionox_panel *panel)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = panel->dsi };
	struct device *dev = &panel->dsi->dev;

	dev_info(dev, "icna3512_enable\n");

	if (panel->enabled) {
		dev_info(dev, "icna3512_enable: already enabled\n");
		return 0;
	}

	panel->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9c, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfd, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x48, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x35);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x11);
	msleep(120);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0x0d, 0xbb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9f, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xce, 0x22);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb3, 0x00, 0xe0, 0xa0, 0x10,
				     0xc8, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9f, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb2, 0x04, 0x18, 0x08, 0x0c,
				     0x02, 0x00, 0xc4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd3, 0x88, 0x4a, 0x4a, 0x88,
				     0x4a, 0x4a, 0x00, 0xeb, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcb, 0x01, 0x01, 0x01, 0x01,
				     0x04, 0x09, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x48, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x48, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x29);

	if (dsi_ctx.accum_err)
		dev_err(dev, "icna3512_enable: DSI commands failed: %d\n", dsi_ctx.accum_err);
	else
		dev_info(dev, "icna3512_enable: DSI commands sent successfully\n");

	panel->enabled = true;

	return dsi_ctx.accum_err;
}

static int visionox_panel_enable(struct drm_panel *panel)
{
	struct visionox_panel *visionox = to_visionox_panel(panel);
	struct device *dev = &visionox->dsi->dev;
	int ret;

	dev_info(dev, "visionox_panel_enable\n");

	ret = visionox->pdata->enable(visionox);
	if (ret) {
		dev_err(dev, "panel enable failed: %d\n", ret);
		return ret;
	}

	backlight_enable(visionox->backlight);
	dev_info(dev, "visionox_panel_enable ok\n");

	return 0;
}

static int visionox_panel_disable(struct drm_panel *panel)
{
	struct visionox_panel *visionox = to_visionox_panel(panel);
	struct mipi_dsi_device *dsi = visionox->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	if (!visionox->enabled)
		return 0;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	backlight_disable(visionox->backlight);

	usleep_range(10000, 12000);

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display OFF (%d)\n", ret);
		return ret;
	}

	usleep_range(5000, 10000);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode (%d)\n", ret);
		return ret;
	}

	visionox->enabled = false;

	return 0;
}

static int visionox_panel_get_modes(struct drm_panel *panel,
				    struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	connector->display_info.bus_flags = visionox_bus_flags;

	drm_display_info_set_bus_formats(&connector->display_info,
					 visionox_bus_formats,
					 ARRAY_SIZE(visionox_bus_formats));
	return 1;
}

static int visionox_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct visionox_panel *visionox = mipi_dsi_get_drvdata(dsi);
	struct device *dev = &dsi->dev;

	dev_info(dev, "visionox_bl_update_status: brightness=%d, prepared=%d (skipped)\n",
		 bl->props.brightness, visionox->prepared);

	/*
	 * TODO: Dynamic brightness control is not working correctly on this panel.
	 * The brightness is set during panel enable sequence (0x51, 0x0d, 0xbb).
	 * Attempting to change brightness after init causes display issues.
	 * For now, skip brightness updates to keep the panel stable.
	 */
	return 0;
}

static const struct backlight_ops visionox_bl_ops = {
	.update_status = visionox_bl_update_status,
};

static const struct drm_panel_funcs visionox_panel_funcs = {
	.prepare = visionox_panel_prepare,
	.unprepare = visionox_panel_unprepare,
	.enable = visionox_panel_enable,
	.disable = visionox_panel_disable,
	.get_modes = visionox_panel_get_modes,
};

static const char *const visionox_supply_names[] = {
	"v3p3",
	"v1p8",
};

static int visionox_init_regulators(struct visionox_panel *visionox)
{
	struct device *dev = &visionox->dsi->dev;
	int i;

	visionox->num_supplies = ARRAY_SIZE(visionox_supply_names);
	visionox->supplies = devm_kcalloc(dev, visionox->num_supplies,
					  sizeof(*visionox->supplies),
					  GFP_KERNEL);
	if (!visionox->supplies)
		return -ENOMEM;

	for (i = 0; i < visionox->num_supplies; i++)
		visionox->supplies[i].supply = visionox_supply_names[i];

	return devm_regulator_bulk_get(dev, visionox->num_supplies,
				       visionox->supplies);
};

static const struct visionox_platform_data visionox_icna3512 = {
	.enable = &icna3512_enable,
};

static const struct of_device_id visionox_of_match[] = {
	{ .compatible = "visionox,icna3512", .data = &visionox_icna3512 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, visionox_of_match);

static int visionox_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct of_device_id *of_id =
		of_match_device(visionox_of_match, dev);
	struct device_node *np = dev->of_node;
	struct visionox_panel *panel;
	struct backlight_properties bl_props;
	int ret;
	u32 video_mode;

	if (!of_id || !of_id->data)
		return -ENODEV;

	dev_info(dev, "visionox_panel_probe\n");

	panel = devm_kzalloc(&dsi->dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, panel);

	panel->dsi = dsi;
	panel->pdata = of_id->data;

	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_NO_EOT_PACKET;

	ret = of_property_read_u32(np, "video-mode", &video_mode);
	if (!ret) {
		switch (video_mode) {
		case 0:
			/* burst mode */
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_BURST |
					   MIPI_DSI_MODE_VIDEO;
			break;
		case 1:
			/* non-burst mode with sync event */
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO;
			break;
		case 2:
			/* non-burst mode with sync pulse */
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
					   MIPI_DSI_MODE_VIDEO;
			break;
		case 3:
			/* command mode */
			dsi->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS |
					   MIPI_DSI_MODE_VSYNC_FLUSH;
			break;
		default:
			dev_warn(dev, "invalid video mode %d\n", video_mode);
			break;
		}
	}

	ret = of_property_read_u32(np, "dsi-lanes", &dsi->lanes);
	if (ret) {
		dev_err(dev, "Failed to get dsi-lanes property (%d)\n", ret);
		return ret;
	}

	panel->reset = devm_gpiod_get_optional(
		dev, "reset", GPIOD_OUT_LOW | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(panel->reset)) {
		ret = PTR_ERR(panel->reset);
		dev_err(dev, "Failed to get reset gpio (%d)\n", ret);
		return ret;
	}
	dev_info(dev, "visionox_panel_probe: assert reset (set to 1)\n");
	gpiod_set_value_cansleep(panel->reset, 1);

	memset(&bl_props, 0, sizeof(bl_props));
	bl_props.type = BACKLIGHT_RAW;
	bl_props.brightness = 3515;	/* 0x0DBB - match init sequence */
	bl_props.max_brightness = 4095;	/* 12-bit: 0x0FFF */

	panel->backlight = devm_backlight_device_register(
		dev, dev_name(dev), dev, dsi, &visionox_bl_ops, &bl_props);
	if (IS_ERR(panel->backlight)) {
		ret = PTR_ERR(panel->backlight);
		dev_err(dev, "Failed to register backlight (%d)\n", ret);
		return ret;
	}

	ret = visionox_init_regulators(panel);
	if (ret)
		return ret;

	drm_panel_init(&panel->panel, dev, &visionox_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	dev_set_drvdata(dev, panel);

	drm_panel_add(&panel->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret)
		drm_panel_remove(&panel->panel);

	dev_info(dev, "visionox_panel_probe ok\n");

	return ret;
}

static void visionox_panel_remove(struct mipi_dsi_device *dsi)
{
	struct visionox_panel *visionox = mipi_dsi_get_drvdata(dsi);
	struct device *dev = &dsi->dev;
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret)
		dev_err(dev, "Failed to detach from host (%d)\n", ret);

	drm_panel_remove(&visionox->panel);
}

static void visionox_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct visionox_panel *visionox = mipi_dsi_get_drvdata(dsi);

	visionox_panel_disable(&visionox->panel);
	visionox_panel_unprepare(&visionox->panel);
}

static struct mipi_dsi_driver visionox_panel_driver = {
	.driver = {
		.name = "panel-visionox-icna3512",
		.of_match_table = visionox_of_match,
	},
	.probe = visionox_panel_probe,
	.remove = visionox_panel_remove,
	.shutdown = visionox_panel_shutdown,
};
module_mipi_dsi_driver(visionox_panel_driver);

MODULE_AUTHOR("Robert Chiras <robert.chiras@nxp.com>");
MODULE_DESCRIPTION("DRM Driver for Visionox ICNA3512 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
