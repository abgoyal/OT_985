

#include "msm_fb.h"

static int __init lcdc_wxga_init(void)
{
	int ret;
	struct msm_panel_info pinfo;

#ifdef CONFIG_FB_MSM_MDDI_AUTO_DETECT
	if (msm_fb_detect_client("lcdc_wxga"))
		return 0;
#endif

	pinfo.xres = 1280;
	pinfo.yres = 720;
	pinfo.type = LCDC_PANEL;
	pinfo.pdest = DISPLAY_1;
	pinfo.wait_cycle = 0;
	pinfo.bpp = 24;
	pinfo.fb_num = 2;
	pinfo.clk_rate = 74250000;

	pinfo.lcdc.h_back_porch = 124;
	pinfo.lcdc.h_front_porch = 110;
	pinfo.lcdc.h_pulse_width = 136;
	pinfo.lcdc.v_back_porch = 19;
	pinfo.lcdc.v_front_porch = 5;
	pinfo.lcdc.v_pulse_width = 6;
	pinfo.lcdc.border_clr = 0;	/* blk */
	pinfo.lcdc.underflow_clr = 0xff;	/* blue */
	pinfo.lcdc.hsync_skew = 0;

	ret = lcdc_device_register(&pinfo);
	if (ret)
		printk(KERN_ERR "%s: failed to register device!\n", __func__);

	return ret;
}

module_init(lcdc_wxga_init);
