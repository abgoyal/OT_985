

#ifndef __MACH_MX1_H__
#define __MACH_MX1_H__

#include <mach/vmalloc.h>

#define MX1_IO_BASE_ADDR	0x00200000
#define MX1_IO_SIZE		SZ_1M
#define MX1_IO_BASE_ADDR_VIRT	VMALLOC_END

#define MX1_CS0_PHYS		0x10000000
#define MX1_CS0_SIZE		0x02000000

#define MX1_CS1_PHYS		0x12000000
#define MX1_CS1_SIZE		0x01000000

#define MX1_CS2_PHYS		0x13000000
#define MX1_CS2_SIZE		0x01000000

#define MX1_CS3_PHYS		0x14000000
#define MX1_CS3_SIZE		0x01000000

#define MX1_CS4_PHYS		0x15000000
#define MX1_CS4_SIZE		0x01000000

#define MX1_CS5_PHYS		0x16000000
#define MX1_CS5_SIZE		0x01000000

#define MX1_AIPI1_BASE_ADDR		(0x00000 + MX1_IO_BASE_ADDR)
#define MX1_WDT_BASE_ADDR		(0x01000 + MX1_IO_BASE_ADDR)
#define MX1_TIM1_BASE_ADDR		(0x02000 + MX1_IO_BASE_ADDR)
#define MX1_TIM2_BASE_ADDR		(0x03000 + MX1_IO_BASE_ADDR)
#define MX1_RTC_BASE_ADDR		(0x04000 + MX1_IO_BASE_ADDR)
#define MX1_LCDC_BASE_ADDR		(0x05000 + MX1_IO_BASE_ADDR)
#define MX1_UART1_BASE_ADDR		(0x06000 + MX1_IO_BASE_ADDR)
#define MX1_UART2_BASE_ADDR		(0x07000 + MX1_IO_BASE_ADDR)
#define MX1_PWM_BASE_ADDR		(0x08000 + MX1_IO_BASE_ADDR)
#define MX1_DMA_BASE_ADDR		(0x09000 + MX1_IO_BASE_ADDR)
#define MX1_AIPI2_BASE_ADDR		(0x10000 + MX1_IO_BASE_ADDR)
#define MX1_SIM_BASE_ADDR		(0x11000 + MX1_IO_BASE_ADDR)
#define MX1_USBD_BASE_ADDR		(0x12000 + MX1_IO_BASE_ADDR)
#define MX1_SPI1_BASE_ADDR		(0x13000 + MX1_IO_BASE_ADDR)
#define MX1_MMC_BASE_ADDR		(0x14000 + MX1_IO_BASE_ADDR)
#define MX1_ASP_BASE_ADDR		(0x15000 + MX1_IO_BASE_ADDR)
#define MX1_BTA_BASE_ADDR		(0x16000 + MX1_IO_BASE_ADDR)
#define MX1_I2C_BASE_ADDR		(0x17000 + MX1_IO_BASE_ADDR)
#define MX1_SSI_BASE_ADDR		(0x18000 + MX1_IO_BASE_ADDR)
#define MX1_SPI2_BASE_ADDR		(0x19000 + MX1_IO_BASE_ADDR)
#define MX1_MSHC_BASE_ADDR		(0x1A000 + MX1_IO_BASE_ADDR)
#define MX1_CCM_BASE_ADDR		(0x1B000 + MX1_IO_BASE_ADDR)
#define MX1_SCM_BASE_ADDR		(0x1B804 + MX1_IO_BASE_ADDR)
#define MX1_GPIO_BASE_ADDR		(0x1C000 + MX1_IO_BASE_ADDR)
#define MX1_EIM_BASE_ADDR		(0x20000 + MX1_IO_BASE_ADDR)
#define MX1_SDRAMC_BASE_ADDR		(0x21000 + MX1_IO_BASE_ADDR)
#define MX1_MMA_BASE_ADDR		(0x22000 + MX1_IO_BASE_ADDR)
#define MX1_AVIC_BASE_ADDR		(0x23000 + MX1_IO_BASE_ADDR)
#define MX1_CSI_BASE_ADDR		(0x24000 + MX1_IO_BASE_ADDR)

/* macro to get at IO space when running virtually */
#define MX1_IO_ADDRESS(x) (						\
	IMX_IO_ADDRESS(x, MX1_IO))

/* fixed interrput numbers */
#define MX1_INT_SOFTINT		0
#define MX1_CSI_INT		6
#define MX1_DSPA_MAC_INT	7
#define MX1_DSPA_INT		8
#define MX1_COMP_INT		9
#define MX1_MSHC_XINT		10
#define MX1_GPIO_INT_PORTA	11
#define MX1_GPIO_INT_PORTB	12
#define MX1_GPIO_INT_PORTC	13
#define MX1_LCDC_INT		14
#define MX1_SIM_INT		15
#define MX1_SIM_DATA_INT	16
#define MX1_RTC_INT		17
#define MX1_RTC_SAMINT		18
#define MX1_UART2_MINT_PFERR	19
#define MX1_UART2_MINT_RTS	20
#define MX1_UART2_MINT_DTR	21
#define MX1_UART2_MINT_UARTC	22
#define MX1_UART2_MINT_TX	23
#define MX1_UART2_MINT_RX	24
#define MX1_UART1_MINT_PFERR	25
#define MX1_UART1_MINT_RTS	26
#define MX1_UART1_MINT_DTR	27
#define MX1_UART1_MINT_UARTC	28
#define MX1_UART1_MINT_TX	29
#define MX1_UART1_MINT_RX	30
#define MX1_VOICE_DAC_INT	31
#define MX1_VOICE_ADC_INT	32
#define MX1_PEN_DATA_INT	33
#define MX1_PWM_INT		34
#define MX1_SDHC_INT		35
#define MX1_I2C_INT		39
#define MX1_CSPI_INT		41
#define MX1_SSI_TX_INT		42
#define MX1_SSI_TX_ERR_INT	43
#define MX1_SSI_RX_INT		44
#define MX1_SSI_RX_ERR_INT	45
#define MX1_TOUCH_INT		46
#define MX1_USBD_INT0		47
#define MX1_USBD_INT1		48
#define MX1_USBD_INT2		49
#define MX1_USBD_INT3		50
#define MX1_USBD_INT4		51
#define MX1_USBD_INT5		52
#define MX1_USBD_INT6		53
#define MX1_BTSYS_INT		55
#define MX1_BTTIM_INT		56
#define MX1_BTWUI_INT		57
#define MX1_TIM2_INT		58
#define MX1_TIM1_INT		59
#define MX1_DMA_ERR		60
#define MX1_DMA_INT		61
#define MX1_GPIO_INT_PORTD	62
#define MX1_WDT_INT		63

/* DMA */
#define MX1_DMA_REQ_UART3_T		2
#define MX1_DMA_REQ_UART3_R		3
#define MX1_DMA_REQ_SSI2_T		4
#define MX1_DMA_REQ_SSI2_R		5
#define MX1_DMA_REQ_CSI_STAT		6
#define MX1_DMA_REQ_CSI_R		7
#define MX1_DMA_REQ_MSHC		8
#define MX1_DMA_REQ_DSPA_DCT_DOUT	9
#define MX1_DMA_REQ_DSPA_DCT_DIN	10
#define MX1_DMA_REQ_DSPA_MAC		11
#define MX1_DMA_REQ_EXT			12
#define MX1_DMA_REQ_SDHC		13
#define MX1_DMA_REQ_SPI1_R		14
#define MX1_DMA_REQ_SPI1_T		15
#define MX1_DMA_REQ_SSI_T		16
#define MX1_DMA_REQ_SSI_R		17
#define MX1_DMA_REQ_ASP_DAC		18
#define MX1_DMA_REQ_ASP_ADC		19
#define MX1_DMA_REQ_USP_EP(x)		(20 + (x))
#define MX1_DMA_REQ_SPI2_R		26
#define MX1_DMA_REQ_SPI2_T		27
#define MX1_DMA_REQ_UART2_T		28
#define MX1_DMA_REQ_UART2_R		29
#define MX1_DMA_REQ_UART1_T		30
#define MX1_DMA_REQ_UART1_R		31

#define USBD_INT0		MX1_USBD_INT0

#ifdef IMX_NEEDS_DEPRECATED_SYMBOLS
/* these should go away */
#define IMX_IO_PHYS MX1_IO_BASE_ADDR
#define IMX_IO_SIZE MX1_IO_SIZE
#define IMX_IO_BASE MX1_IO_BASE_ADDR_VIRT
#define IMX_CS0_PHYS MX1_CS0_PHYS
#define IMX_CS0_SIZE MX1_CS0_SIZE
#define IMX_CS1_PHYS MX1_CS1_PHYS
#define IMX_CS1_SIZE MX1_CS1_SIZE
#define IMX_CS2_PHYS MX1_CS2_PHYS
#define IMX_CS2_SIZE MX1_CS2_SIZE
#define IMX_CS3_PHYS MX1_CS3_PHYS
#define IMX_CS3_SIZE MX1_CS3_SIZE
#define IMX_CS4_PHYS MX1_CS4_PHYS
#define IMX_CS4_SIZE MX1_CS4_SIZE
#define IMX_CS5_PHYS MX1_CS5_PHYS
#define IMX_CS5_SIZE MX1_CS5_SIZE
#define AIPI1_BASE_ADDR MX1_AIPI1_BASE_ADDR
#define WDT_BASE_ADDR MX1_WDT_BASE_ADDR
#define TIM1_BASE_ADDR MX1_TIM1_BASE_ADDR
#define TIM2_BASE_ADDR MX1_TIM2_BASE_ADDR
#define RTC_BASE_ADDR MX1_RTC_BASE_ADDR
#define LCDC_BASE_ADDR MX1_LCDC_BASE_ADDR
#define UART1_BASE_ADDR MX1_UART1_BASE_ADDR
#define UART2_BASE_ADDR MX1_UART2_BASE_ADDR
#define PWM_BASE_ADDR MX1_PWM_BASE_ADDR
#define DMA_BASE_ADDR MX1_DMA_BASE_ADDR
#define AIPI2_BASE_ADDR MX1_AIPI2_BASE_ADDR
#define SIM_BASE_ADDR MX1_SIM_BASE_ADDR
#define USBD_BASE_ADDR MX1_USBD_BASE_ADDR
#define SPI1_BASE_ADDR MX1_SPI1_BASE_ADDR
#define MMC_BASE_ADDR MX1_MMC_BASE_ADDR
#define ASP_BASE_ADDR MX1_ASP_BASE_ADDR
#define BTA_BASE_ADDR MX1_BTA_BASE_ADDR
#define I2C_BASE_ADDR MX1_I2C_BASE_ADDR
#define SSI_BASE_ADDR MX1_SSI_BASE_ADDR
#define SPI2_BASE_ADDR MX1_SPI2_BASE_ADDR
#define MSHC_BASE_ADDR MX1_MSHC_BASE_ADDR
#define CCM_BASE_ADDR MX1_CCM_BASE_ADDR
#define SCM_BASE_ADDR MX1_SCM_BASE_ADDR
#define GPIO_BASE_ADDR MX1_GPIO_BASE_ADDR
#define EIM_BASE_ADDR MX1_EIM_BASE_ADDR
#define SDRAMC_BASE_ADDR MX1_SDRAMC_BASE_ADDR
#define MMA_BASE_ADDR MX1_MMA_BASE_ADDR
#define AVIC_BASE_ADDR MX1_AVIC_BASE_ADDR
#define CSI_BASE_ADDR MX1_CSI_BASE_ADDR
#define IO_ADDRESS(x) MX1_IO_ADDRESS(x)
#define AVIC_IO_ADDRESS(x) IO_ADDRESS(x)
#define INT_SOFTINT MX1_INT_SOFTINT
#define CSI_INT MX1_CSI_INT
#define DSPA_MAC_INT MX1_DSPA_MAC_INT
#define DSPA_INT MX1_DSPA_INT
#define COMP_INT MX1_COMP_INT
#define MSHC_XINT MX1_MSHC_XINT
#define GPIO_INT_PORTA MX1_GPIO_INT_PORTA
#define GPIO_INT_PORTB MX1_GPIO_INT_PORTB
#define GPIO_INT_PORTC MX1_GPIO_INT_PORTC
#define LCDC_INT MX1_LCDC_INT
#define SIM_INT MX1_SIM_INT
#define SIM_DATA_INT MX1_SIM_DATA_INT
#define RTC_INT MX1_RTC_INT
#define RTC_SAMINT MX1_RTC_SAMINT
#define UART2_MINT_PFERR MX1_UART2_MINT_PFERR
#define UART2_MINT_RTS MX1_UART2_MINT_RTS
#define UART2_MINT_DTR MX1_UART2_MINT_DTR
#define UART2_MINT_UARTC MX1_UART2_MINT_UARTC
#define UART2_MINT_TX MX1_UART2_MINT_TX
#define UART2_MINT_RX MX1_UART2_MINT_RX
#define UART1_MINT_PFERR MX1_UART1_MINT_PFERR
#define UART1_MINT_RTS MX1_UART1_MINT_RTS
#define UART1_MINT_DTR MX1_UART1_MINT_DTR
#define UART1_MINT_UARTC MX1_UART1_MINT_UARTC
#define UART1_MINT_TX MX1_UART1_MINT_TX
#define UART1_MINT_RX MX1_UART1_MINT_RX
#define VOICE_DAC_INT MX1_VOICE_DAC_INT
#define VOICE_ADC_INT MX1_VOICE_ADC_INT
#define PEN_DATA_INT MX1_PEN_DATA_INT
#define PWM_INT MX1_PWM_INT
#define SDHC_INT MX1_SDHC_INT
#define I2C_INT MX1_I2C_INT
#define CSPI_INT MX1_CSPI_INT
#define SSI_TX_INT MX1_SSI_TX_INT
#define SSI_TX_ERR_INT MX1_SSI_TX_ERR_INT
#define SSI_RX_INT MX1_SSI_RX_INT
#define SSI_RX_ERR_INT MX1_SSI_RX_ERR_INT
#define TOUCH_INT MX1_TOUCH_INT
#define USBD_INT1 MX1_USBD_INT1
#define USBD_INT2 MX1_USBD_INT2
#define USBD_INT3 MX1_USBD_INT3
#define USBD_INT4 MX1_USBD_INT4
#define USBD_INT5 MX1_USBD_INT5
#define USBD_INT6 MX1_USBD_INT6
#define BTSYS_INT MX1_BTSYS_INT
#define BTTIM_INT MX1_BTTIM_INT
#define BTWUI_INT MX1_BTWUI_INT
#define TIM2_INT MX1_TIM2_INT
#define TIM1_INT MX1_TIM1_INT
#define DMA_ERR MX1_DMA_ERR
#define DMA_INT MX1_DMA_INT
#define GPIO_INT_PORTD MX1_GPIO_INT_PORTD
#define WDT_INT MX1_WDT_INT
#define DMA_REQ_UART3_T MX1_DMA_REQ_UART3_T
#define DMA_REQ_UART3_R MX1_DMA_REQ_UART3_R
#define DMA_REQ_SSI2_T MX1_DMA_REQ_SSI2_T
#define DMA_REQ_SSI2_R MX1_DMA_REQ_SSI2_R
#define DMA_REQ_CSI_STAT MX1_DMA_REQ_CSI_STAT
#define DMA_REQ_CSI_R MX1_DMA_REQ_CSI_R
#define DMA_REQ_MSHC MX1_DMA_REQ_MSHC
#define DMA_REQ_DSPA_DCT_DOUT MX1_DMA_REQ_DSPA_DCT_DOUT
#define DMA_REQ_DSPA_DCT_DIN MX1_DMA_REQ_DSPA_DCT_DIN
#define DMA_REQ_DSPA_MAC MX1_DMA_REQ_DSPA_MAC
#define DMA_REQ_EXT MX1_DMA_REQ_EXT
#define DMA_REQ_SDHC MX1_DMA_REQ_SDHC
#define DMA_REQ_SPI1_R MX1_DMA_REQ_SPI1_R
#define DMA_REQ_SPI1_T MX1_DMA_REQ_SPI1_T
#define DMA_REQ_SSI_T MX1_DMA_REQ_SSI_T
#define DMA_REQ_SSI_R MX1_DMA_REQ_SSI_R
#define DMA_REQ_ASP_DAC MX1_DMA_REQ_ASP_DAC
#define DMA_REQ_ASP_ADC MX1_DMA_REQ_ASP_ADC
#define DMA_REQ_USP_EP(x) MX1_DMA_REQ_USP_EP(x)
#define DMA_REQ_SPI2_R MX1_DMA_REQ_SPI2_R
#define DMA_REQ_SPI2_T MX1_DMA_REQ_SPI2_T
#define DMA_REQ_UART2_T MX1_DMA_REQ_UART2_T
#define DMA_REQ_UART2_R MX1_DMA_REQ_UART2_R
#define DMA_REQ_UART1_T MX1_DMA_REQ_UART1_T
#define DMA_REQ_UART1_R MX1_DMA_REQ_UART1_R
#endif /* ifdef IMX_NEEDS_DEPRECATED_SYMBOLS */

#endif /* ifndef __MACH_MX1_H__ */
