

#include <linux/pci.h>
#include <linux/of.h>
#include <linux/of_platform.h>

#include <asm/time.h>
#include <asm/ipic.h>
#include <asm/udbg.h>
#include <asm/prom.h>
#include <sysdev/fsl_pci.h>

#include "mpc83xx.h"

#define BCSR12_USB_SER_MASK	0x8a
#define BCSR12_USB_SER_PIN	0x80
#define BCSR12_USB_SER_DEVICE	0x02

static int mpc837xmds_usb_cfg(void)
{
	struct device_node *np;
	const void *phy_type, *mode;
	void __iomem *bcsr_regs = NULL;
	u8 bcsr12;
	int ret;

	ret = mpc837x_usb_cfg();
	if (ret)
		return ret;
	/* Map BCSR area */
	np = of_find_compatible_node(NULL, NULL, "fsl,mpc837xmds-bcsr");
	if (np) {
		bcsr_regs = of_iomap(np, 0);
		of_node_put(np);
	}
	if (!bcsr_regs)
		return -1;

	np = of_find_node_by_name(NULL, "usb");
	if (!np)
		return -ENODEV;
	phy_type = of_get_property(np, "phy_type", NULL);
	if (phy_type && !strcmp(phy_type, "ulpi")) {
		clrbits8(bcsr_regs + 12, BCSR12_USB_SER_PIN);
	} else if (phy_type && !strcmp(phy_type, "serial")) {
		mode = of_get_property(np, "dr_mode", NULL);
		bcsr12 = in_8(bcsr_regs + 12) & ~BCSR12_USB_SER_MASK;
		bcsr12 |= BCSR12_USB_SER_PIN;
		if (mode && !strcmp(mode, "peripheral"))
			bcsr12 |= BCSR12_USB_SER_DEVICE;
		out_8(bcsr_regs + 12, bcsr12);
	} else {
		printk(KERN_ERR "USB DR: unsupported PHY\n");
	}

	of_node_put(np);
	iounmap(bcsr_regs);
	return 0;
}

static void __init mpc837x_mds_setup_arch(void)
{
#ifdef CONFIG_PCI
	struct device_node *np;
#endif

	if (ppc_md.progress)
		ppc_md.progress("mpc837x_mds_setup_arch()", 0);

#ifdef CONFIG_PCI
	for_each_compatible_node(np, "pci", "fsl,mpc8349-pci")
		mpc83xx_add_bridge(np);
	for_each_compatible_node(np, "pci", "fsl,mpc8314-pcie")
		mpc83xx_add_bridge(np);
#endif
	mpc837xmds_usb_cfg();
}

static struct of_device_id mpc837x_ids[] = {
	{ .type = "soc", },
	{ .compatible = "soc", },
	{ .compatible = "simple-bus", },
	{ .compatible = "gianfar", },
	{},
};

static int __init mpc837x_declare_of_platform_devices(void)
{
	/* Publish of_device */
	of_platform_bus_probe(NULL, mpc837x_ids, NULL);

	return 0;
}
machine_device_initcall(mpc837x_mds, mpc837x_declare_of_platform_devices);

static void __init mpc837x_mds_init_IRQ(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "fsl,ipic");
	if (!np)
		return;

	ipic_init(np, 0);

	/* Initialize the default interrupt mapping priorities,
	 * in case the boot rom changed something on us.
	 */
	ipic_set_default_priority();
}

static int __init mpc837x_mds_probe(void)
{
        unsigned long root = of_get_flat_dt_root();

        return of_flat_dt_is_compatible(root, "fsl,mpc837xmds");
}

define_machine(mpc837x_mds) {
	.name			= "MPC837x MDS",
	.probe			= mpc837x_mds_probe,
	.setup_arch		= mpc837x_mds_setup_arch,
	.init_IRQ		= mpc837x_mds_init_IRQ,
	.get_irq		= ipic_get_irq,
	.restart		= mpc83xx_restart,
	.time_init		= mpc83xx_time_init,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};
