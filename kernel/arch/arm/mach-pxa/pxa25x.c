
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>
#include <linux/sysdev.h>

#include <mach/hardware.h>
#include <mach/irqs.h>
#include <mach/gpio.h>
#include <mach/pxa25x.h>
#include <mach/reset.h>
#include <mach/pm.h>
#include <mach/dma.h>

#include "generic.h"
#include "devices.h"
#include "clock.h"


/* Crystal Frequency to Memory Frequency Multiplier (L) */
static unsigned char L_clk_mult[32] = { 0, 27, 32, 36, 40, 45, 0, };

/* Memory Frequency to Run Mode Frequency Multiplier (M) */
static unsigned char M_clk_mult[4] = { 0, 1, 2, 4 };

/* Run Mode Frequency to Turbo Mode Frequency Multiplier (N) */
/* Note: we store the value N * 2 here. */
static unsigned char N2_clk_mult[8] = { 0, 0, 2, 3, 4, 0, 6, 0 };

/* Crystal clock */
#define BASE_CLK	3686400

unsigned int pxa25x_get_clk_frequency_khz(int info)
{
	unsigned long cccr, turbo;
	unsigned int l, L, m, M, n2, N;

	cccr = CCCR;
	asm( "mrc\tp14, 0, %0, c6, c0, 0" : "=r" (turbo) );

	l  =  L_clk_mult[(cccr >> 0) & 0x1f];
	m  =  M_clk_mult[(cccr >> 5) & 0x03];
	n2 = N2_clk_mult[(cccr >> 7) & 0x07];

	L = l * BASE_CLK;
	M = m * L;
	N = n2 * M / 2;

	if(info)
	{
		L += 5000;
		printk( KERN_INFO "Memory clock: %d.%02dMHz (*%d)\n",
			L / 1000000, (L % 1000000) / 10000, l );
		M += 5000;
		printk( KERN_INFO "Run Mode clock: %d.%02dMHz (*%d)\n",
			M / 1000000, (M % 1000000) / 10000, m );
		N += 5000;
		printk( KERN_INFO "Turbo Mode clock: %d.%02dMHz (*%d.%d, %sactive)\n",
			N / 1000000, (N % 1000000) / 10000, n2 / 2, (n2 % 2) * 5,
			(turbo & 1) ? "" : "in" );
	}

	return (turbo & 1) ? (N/1000) : (M/1000);
}

unsigned int pxa25x_get_memclk_frequency_10khz(void)
{
	return L_clk_mult[(CCCR >> 0) & 0x1f] * BASE_CLK / 10000;
}

static unsigned long clk_pxa25x_lcd_getrate(struct clk *clk)
{
	return pxa25x_get_memclk_frequency_10khz() * 10000;
}

static const struct clkops clk_pxa25x_lcd_ops = {
	.enable		= clk_cken_enable,
	.disable	= clk_cken_disable,
	.getrate	= clk_pxa25x_lcd_getrate,
};

static unsigned long gpio12_config_32k[] = {
	GPIO12_32KHz,
};

static unsigned long gpio12_config_gpio[] = {
	GPIO12_GPIO,
};

static void clk_gpio12_enable(struct clk *clk)
{
	pxa2xx_mfp_config(gpio12_config_32k, 1);
}

static void clk_gpio12_disable(struct clk *clk)
{
	pxa2xx_mfp_config(gpio12_config_gpio, 1);
}

static const struct clkops clk_pxa25x_gpio12_ops = {
	.enable         = clk_gpio12_enable,
	.disable        = clk_gpio12_disable,
};

static unsigned long gpio11_config_3m6[] = {
	GPIO11_3_6MHz,
};

static unsigned long gpio11_config_gpio[] = {
	GPIO11_GPIO,
};

static void clk_gpio11_enable(struct clk *clk)
{
	pxa2xx_mfp_config(gpio11_config_3m6, 1);
}

static void clk_gpio11_disable(struct clk *clk)
{
	pxa2xx_mfp_config(gpio11_config_gpio, 1);
}

static const struct clkops clk_pxa25x_gpio11_ops = {
	.enable         = clk_gpio11_enable,
	.disable        = clk_gpio11_disable,
};

static DEFINE_CKEN(pxa25x_hwuart, HWUART, 14745600, 1);

static struct clk_lookup pxa25x_hwuart_clkreg =
	INIT_CLKREG(&clk_pxa25x_hwuart, "pxa2xx-uart.3", NULL);

static DEFINE_CK(pxa25x_lcd, LCD, &clk_pxa25x_lcd_ops);
static DEFINE_CKEN(pxa25x_ffuart, FFUART, 14745600, 1);
static DEFINE_CKEN(pxa25x_btuart, BTUART, 14745600, 1);
static DEFINE_CKEN(pxa25x_stuart, STUART, 14745600, 1);
static DEFINE_CKEN(pxa25x_usb, USB, 47923000, 5);
static DEFINE_CLK(pxa25x_gpio11, &clk_pxa25x_gpio11_ops, 3686400, 0);
static DEFINE_CLK(pxa25x_gpio12, &clk_pxa25x_gpio12_ops, 32768, 0);
static DEFINE_CKEN(pxa25x_mmc, MMC, 19169000, 0);
static DEFINE_CKEN(pxa25x_i2c, I2C, 31949000, 0);
static DEFINE_CKEN(pxa25x_ssp, SSP, 3686400, 0);
static DEFINE_CKEN(pxa25x_nssp, NSSP, 3686400, 0);
static DEFINE_CKEN(pxa25x_assp, ASSP, 3686400, 0);
static DEFINE_CKEN(pxa25x_pwm0, PWM0, 3686400, 0);
static DEFINE_CKEN(pxa25x_pwm1, PWM1, 3686400, 0);
static DEFINE_CKEN(pxa25x_ac97, AC97, 24576000, 0);
static DEFINE_CKEN(pxa25x_i2s, I2S, 14745600, 0);
static DEFINE_CKEN(pxa25x_ficp, FICP, 47923000, 0);

static struct clk_lookup pxa25x_clkregs[] = {
	INIT_CLKREG(&clk_pxa25x_lcd, "pxa2xx-fb", NULL),
	INIT_CLKREG(&clk_pxa25x_ffuart, "pxa2xx-uart.0", NULL),
	INIT_CLKREG(&clk_pxa25x_btuart, "pxa2xx-uart.1", NULL),
	INIT_CLKREG(&clk_pxa25x_stuart, "pxa2xx-uart.2", NULL),
	INIT_CLKREG(&clk_pxa25x_usb, "pxa25x-udc", NULL),
	INIT_CLKREG(&clk_pxa25x_mmc, "pxa2xx-mci.0", NULL),
	INIT_CLKREG(&clk_pxa25x_i2c, "pxa2xx-i2c.0", NULL),
	INIT_CLKREG(&clk_pxa25x_ssp, "pxa25x-ssp.0", NULL),
	INIT_CLKREG(&clk_pxa25x_nssp, "pxa25x-nssp.1", NULL),
	INIT_CLKREG(&clk_pxa25x_assp, "pxa25x-nssp.2", NULL),
	INIT_CLKREG(&clk_pxa25x_pwm0, "pxa25x-pwm.0", NULL),
	INIT_CLKREG(&clk_pxa25x_pwm1, "pxa25x-pwm.1", NULL),
	INIT_CLKREG(&clk_pxa25x_i2s, "pxa2xx-i2s", NULL),
	INIT_CLKREG(&clk_pxa25x_stuart, "pxa2xx-ir", "UARTCLK"),
	INIT_CLKREG(&clk_pxa25x_ficp, "pxa2xx-ir", "FICPCLK"),
	INIT_CLKREG(&clk_pxa25x_ac97, NULL, "AC97CLK"),
	INIT_CLKREG(&clk_pxa25x_gpio11, NULL, "GPIO11_CLK"),
	INIT_CLKREG(&clk_pxa25x_gpio12, NULL, "GPIO12_CLK"),
};

#ifdef CONFIG_PM

#define SAVE(x)		sleep_save[SLEEP_SAVE_##x] = x
#define RESTORE(x)	x = sleep_save[SLEEP_SAVE_##x]

enum {
	SLEEP_SAVE_PSTR,
	SLEEP_SAVE_CKEN,
	SLEEP_SAVE_COUNT
};


static void pxa25x_cpu_pm_save(unsigned long *sleep_save)
{
	SAVE(CKEN);
	SAVE(PSTR);
}

static void pxa25x_cpu_pm_restore(unsigned long *sleep_save)
{
	RESTORE(CKEN);
	RESTORE(PSTR);
}

static void pxa25x_cpu_pm_enter(suspend_state_t state)
{
	/* Clear reset status */
	RCSR = RCSR_HWR | RCSR_WDR | RCSR_SMR | RCSR_GPR;

	switch (state) {
	case PM_SUSPEND_MEM:
		pxa25x_cpu_suspend(PWRMODE_SLEEP);
		break;
	}
}

static int pxa25x_cpu_pm_prepare(void)
{
	/* set resume return address */
	PSPR = virt_to_phys(pxa_cpu_resume);
	return 0;
}

static void pxa25x_cpu_pm_finish(void)
{
	/* ensure not to come back here if it wasn't intended */
	PSPR = 0;
}

static struct pxa_cpu_pm_fns pxa25x_cpu_pm_fns = {
	.save_count	= SLEEP_SAVE_COUNT,
	.valid		= suspend_valid_only_mem,
	.save		= pxa25x_cpu_pm_save,
	.restore	= pxa25x_cpu_pm_restore,
	.enter		= pxa25x_cpu_pm_enter,
	.prepare	= pxa25x_cpu_pm_prepare,
	.finish		= pxa25x_cpu_pm_finish,
};

static void __init pxa25x_init_pm(void)
{
	pxa_cpu_pm_fns = &pxa25x_cpu_pm_fns;
}
#else
static inline void pxa25x_init_pm(void) {}
#endif


static int pxa25x_set_wake(unsigned int irq, unsigned int on)
{
	int gpio = IRQ_TO_GPIO(irq);
	uint32_t mask = 0;

	if (gpio >= 0 && gpio < 85)
		return gpio_set_wake(gpio, on);

	if (irq == IRQ_RTCAlrm) {
		mask = PWER_RTC;
		goto set_pwer;
	}

	return -EINVAL;

set_pwer:
	if (on)
		PWER |= mask;
	else
		PWER &=~mask;

	return 0;
}

void __init pxa25x_init_irq(void)
{
	pxa_init_irq(32, pxa25x_set_wake);
	pxa_init_gpio(IRQ_GPIO_2_x, 2, 84, pxa25x_set_wake);
}

#ifdef CONFIG_CPU_PXA26x
void __init pxa26x_init_irq(void)
{
	pxa_init_irq(32, pxa25x_set_wake);
	pxa_init_gpio(IRQ_GPIO_2_x, 2, 89, pxa25x_set_wake);
}
#endif

static struct platform_device *pxa25x_devices[] __initdata = {
	&pxa25x_device_udc,
	&pxa_device_i2s,
	&sa1100_device_rtc,
	&pxa25x_device_ssp,
	&pxa25x_device_nssp,
	&pxa25x_device_assp,
	&pxa25x_device_pwm0,
	&pxa25x_device_pwm1,
};

static struct sys_device pxa25x_sysdev[] = {
	{
		.cls	= &pxa_irq_sysclass,
	}, {
		.cls	= &pxa2xx_mfp_sysclass,
	}, {
		.cls	= &pxa_gpio_sysclass,
	},
};

static int __init pxa25x_init(void)
{
	int i, ret = 0;

	if (cpu_is_pxa25x()) {

		reset_status = RCSR;

		clkdev_add_table(pxa25x_clkregs, ARRAY_SIZE(pxa25x_clkregs));

		if ((ret = pxa_init_dma(IRQ_DMA, 16)))
			return ret;

		pxa25x_init_pm();

		for (i = 0; i < ARRAY_SIZE(pxa25x_sysdev); i++) {
			ret = sysdev_register(&pxa25x_sysdev[i]);
			if (ret)
				pr_err("failed to register sysdev[%d]\n", i);
		}

		ret = platform_add_devices(pxa25x_devices,
					   ARRAY_SIZE(pxa25x_devices));
		if (ret)
			return ret;
	}

	/* Only add HWUART for PXA255/26x; PXA210/250 do not have it. */
	if (cpu_is_pxa255())
		clkdev_add(&pxa25x_hwuart_clkreg);

	return ret;
}

postcore_initcall(pxa25x_init);
