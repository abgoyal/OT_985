
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/physmap.h>

#define ASB2303_PROM_ADDR	0xA0000000	/* Boot PROM */
#define ASB2303_PROM_SIZE	(2 * 1024 * 1024)
#define ASB2303_FLASH_ADDR	0xA4000000	/* System Flash */
#define ASB2303_FLASH_SIZE	(32 * 1024 * 1024)
#define ASB2303_CONFIG_ADDR	0xA6000000	/* System Config EEPROM */
#define ASB2303_CONFIG_SIZE	(8 * 1024)

static struct mtd_partition asb2303_partitions[] = {
	{
		.name		= "Bootloader",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_CAP_ROM /* force read-only */
	}, {
		.name		= "Kernel",
		.size		= 0x00400000,
		.offset		= 0x00040000,
	}, {
		.name		= "Filesystem",
		.size		= MTDPART_SIZ_FULL,
		.offset		= 0x00440000
	}
};

static struct physmap_flash_data asb2303_bootprom_data = {
	.width		= 2,
	.nr_parts	= 1,
	.parts		= asb2303_partitions,
};

static struct resource asb2303_bootprom_resource = {
	.start		= ASB2303_PROM_ADDR,
	.end		= ASB2303_PROM_ADDR + ASB2303_PROM_SIZE,
	.flags		= IORESOURCE_MEM,
};

static struct platform_device asb2303_bootprom = {
	.name		= "physmap-flash",
	.id		= 0,
	.dev.platform_data = &asb2303_bootprom_data,
	.num_resources	= 1,
	.resource	= &asb2303_bootprom_resource,
};

static struct physmap_flash_data asb2303_sysflash_data = {
	.width		= 4,
	.nr_parts	= 1,
	.parts		= asb2303_partitions,
};

static struct resource asb2303_sysflash_resource = {
	.start		= ASB2303_FLASH_ADDR,
	.end		= ASB2303_FLASH_ADDR + ASB2303_FLASH_SIZE,
	.flags		= IORESOURCE_MEM,
};

static struct platform_device asb2303_sysflash = {
	.name		= "physmap-flash",
	.id		= 1,
	.dev.platform_data = &asb2303_sysflash_data,
	.num_resources	= 1,
	.resource	= &asb2303_sysflash_resource,
};

static int __init asb2303_mtd_init(void)
{
	platform_device_register(&asb2303_bootprom);
	platform_device_register(&asb2303_sysflash);
	return 0;
}

module_init(asb2303_mtd_init);
