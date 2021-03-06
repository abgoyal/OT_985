
#ifndef __ASM_ARCH_MEMORY_H
#define __ASM_ARCH_MEMORY_H

/* x in [0..3] */
#define NS9XXX_CSxSTAT_PHYS(x)	UL(((x) + 4) << 28)

#define NS9XXX_CS0STAT_LENGTH	UL(0x1000)
#define NS9XXX_CS1STAT_LENGTH	UL(0x1000)
#define NS9XXX_CS2STAT_LENGTH	UL(0x1000)
#define NS9XXX_CS3STAT_LENGTH	UL(0x1000)

#define PHYS_OFFSET	UL(0x00000000)

#endif
