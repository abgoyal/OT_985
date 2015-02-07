
#ifndef __ASM_ARCH_MXC_UNCOMPRESS_H__
#define __ASM_ARCH_MXC_UNCOMPRESS_H__

#define __MXC_BOOT_UNCOMPRESS

#include <asm/mach-types.h>

static unsigned long uart_base;

#define UART(x) (*(volatile unsigned long *)(uart_base + (x)))

#define USR2 0x98
#define USR2_TXFE (1<<14)
#define TXR  0x40
#define UCR1 0x80
#define UCR1_UARTEN 1


static void putc(int ch)
{
	if (!uart_base)
		return;
	if (!(UART(UCR1) & UCR1_UARTEN))
		return;

	while (!(UART(USR2) & USR2_TXFE))
		barrier();

	UART(TXR) = ch;
}

static inline void flush(void)
{
}

#define MX1_UART1_BASE_ADDR	0x00206000
#define MX25_UART1_BASE_ADDR	0x43f90000
#define MX2X_UART1_BASE_ADDR	0x1000a000
#define MX3X_UART1_BASE_ADDR	0x43F90000
#define MX3X_UART2_BASE_ADDR	0x43F94000
#define MX51_UART1_BASE_ADDR	0x73fbc000

static __inline__ void __arch_decomp_setup(unsigned long arch_id)
{
	switch (arch_id) {
	case MACH_TYPE_MX1ADS:
	case MACH_TYPE_SCB9328:
		uart_base = MX1_UART1_BASE_ADDR;
		break;
	case MACH_TYPE_MX25_3DS:
		uart_base = MX25_UART1_BASE_ADDR;
		break;
	case MACH_TYPE_IMX27LITE:
	case MACH_TYPE_MX27_3DS:
	case MACH_TYPE_MX27ADS:
	case MACH_TYPE_PCM038:
	case MACH_TYPE_MX21ADS:
	case MACH_TYPE_PCA100:
	case MACH_TYPE_MXT_TD60:
		uart_base = MX2X_UART1_BASE_ADDR;
		break;
	case MACH_TYPE_MX31LITE:
	case MACH_TYPE_ARMADILLO5X0:
	case MACH_TYPE_MX31MOBOARD:
	case MACH_TYPE_QONG:
	case MACH_TYPE_MX31_3DS:
	case MACH_TYPE_PCM037:
	case MACH_TYPE_MX31ADS:
	case MACH_TYPE_MX35_3DS:
	case MACH_TYPE_PCM043:
	case MACH_TYPE_LILLY1131:
		uart_base = MX3X_UART1_BASE_ADDR;
		break;
	case MACH_TYPE_MAGX_ZN5:
		uart_base = MX3X_UART2_BASE_ADDR;
		break;
	case MACH_TYPE_MX51_BABBAGE:
		uart_base = MX51_UART1_BASE_ADDR;
		break;
	default:
		break;
	}
}

#define arch_decomp_setup()	__arch_decomp_setup(arch_id)
#define arch_decomp_wdog()

#endif				/* __ASM_ARCH_MXC_UNCOMPRESS_H__ */