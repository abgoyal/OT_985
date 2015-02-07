


#define NR_IRQS          32

#define IRQ_STWDOG        0   /* Watchdog timer */
#define IRQ_PROG          1   /* Programmable interrupt */
#define IRQ_DEBUG_RX      2   /* Comm Rx debug */
#define IRQ_DEBUG_TX      3   /* Comm Tx debug */
#define IRQ_GCTC1         4   /* Timer 1 */
#define IRQ_GCTC2         5   /* Timer 2 / Keyboard */
#define IRQ_DMA           6   /* DMA controller */
#define IRQ_CLCD          7   /* Color LCD controller */
#define IRQ_SM_RX         8   /* Smart card */
#define IRQ_SM_TX         9   /* Smart cart */
#define IRQ_SM_RST       10   /* Smart card */
#define IRQ_SIB          11   /* Serial Interface Bus */
#define IRQ_MMC          12   /* MultiMediaCard */
#define IRQ_SSP1         13   /* Synchronous Serial Port 1 */
#define IRQ_SSP2         14   /* Synchronous Serial Port 1 */
#define IRQ_SPI          15   /* SPI slave */
#define IRQ_UART_1       16   /* UART 1 */
#define IRQ_UART_2       17   /* UART 2 */
#define IRQ_IRDA         18   /* IRDA */
#define IRQ_RTC_TICK     19   /* Real Time Clock tick */
#define IRQ_RTC_ALARM    20   /* Real Time Clock alarm */
#define IRQ_GPIO         21   /* General Purpose IO */
#define IRQ_GPIO_DMA     22   /* General Purpose IO, DMA */
#define IRQ_M2M          23   /* Memory to memory DMA  */
#define IRQ_RESERVED     24   /* RESERVED, don't use */
#define IRQ_INTF         25   /* External active low interrupt */
#define IRQ_INT0         26   /* External active low interrupt */
#define IRQ_INT1         27   /* External active low interrupt */
#define IRQ_INT2         28   /* External active low interrupt */
#define IRQ_UCB1200      29   /* Interrupt generated by UCB1200*/
#define IRQ_BAT_LO       30   /* Low batery or external power */
#define IRQ_MEDIA_CHG    31   /* Media change interrupt */

#define FIQ_START	64
