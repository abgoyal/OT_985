

#include <mach/orion5x.h>

	.macro  addruart, rx, tmp
	mrc	p15, 0, \rx, c1, c0
	tst	\rx, #1					@ MMU enabled?
	ldreq	\rx, =ORION5X_REGS_PHYS_BASE
	ldrne	\rx, =ORION5X_REGS_VIRT_BASE
	orr	\rx, \rx, #0x00012000
	.endm

#define UART_SHIFT	2
#include <asm/hardware/debug-8250.S>
