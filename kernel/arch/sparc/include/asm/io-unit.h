
#ifndef _SPARC_IO_UNIT_H
#define _SPARC_IO_UNIT_H

#include <linux/spinlock.h>
#include <asm/page.h>
#include <asm/pgtable.h>

 
 
#define IOUNIT_DMA_BASE	    0xfc000000 /* TOP - 64M */
#define IOUNIT_DMA_SIZE	    0x04000000 /* 64M */
/* We use last 1M for sparc_dvma_malloc */
#define IOUNIT_DVMA_SIZE    0x00100000 /* 1M */

/* The format of an iopte in the external page tables */
#define IOUPTE_PAGE          0xffffff00 /* Physical page number (PA[35:12])	*/
#define IOUPTE_CACHE         0x00000080 /* Cached (in Viking/MXCC)		*/
#define IOUPTE_STREAM        0x00000040 /* Translation can use streaming cache	*/
#define IOUPTE_INTRA	     0x00000008 /* SBUS direct slot->slot transfer	*/
#define IOUPTE_WRITE         0x00000004 /* Writeable				*/
#define IOUPTE_VALID         0x00000002 /* IOPTE is valid			*/
#define IOUPTE_PARITY        0x00000001 /* Parity is checked during DVMA	*/

struct iounit_struct {
	unsigned long		bmap[(IOUNIT_DMA_SIZE >> (PAGE_SHIFT + 3)) / sizeof(unsigned long)];
	spinlock_t		lock;
	iopte_t			*page_table;
	unsigned long		rotor[3];
	unsigned long		limit[4];
};

#define IOUNIT_BMAP1_START	0x00000000
#define IOUNIT_BMAP1_END	(IOUNIT_DMA_SIZE >> (PAGE_SHIFT + 1))
#define IOUNIT_BMAP2_START	IOUNIT_BMAP1_END
#define IOUNIT_BMAP2_END	IOUNIT_BMAP2_START + (IOUNIT_DMA_SIZE >> (PAGE_SHIFT + 2))
#define IOUNIT_BMAPM_START	IOUNIT_BMAP2_END
#define IOUNIT_BMAPM_END	((IOUNIT_DMA_SIZE - IOUNIT_DVMA_SIZE) >> PAGE_SHIFT)

#endif /* !(_SPARC_IO_UNIT_H) */
