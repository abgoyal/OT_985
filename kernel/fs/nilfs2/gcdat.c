

#include <linux/buffer_head.h>
#include "nilfs.h"
#include "page.h"
#include "mdt.h"

int nilfs_init_gcdat_inode(struct the_nilfs *nilfs)
{
	struct inode *dat = nilfs->ns_dat, *gcdat = nilfs->ns_gc_dat;
	struct nilfs_inode_info *dii = NILFS_I(dat), *gii = NILFS_I(gcdat);
	int err;

	gcdat->i_state = 0;
	gcdat->i_blocks = dat->i_blocks;
	gii->i_flags = dii->i_flags;
	gii->i_state = dii->i_state | (1 << NILFS_I_GCDAT);
	gii->i_cno = 0;
	nilfs_bmap_init_gcdat(gii->i_bmap, dii->i_bmap);
	err = nilfs_copy_dirty_pages(gcdat->i_mapping, dat->i_mapping);
	if (unlikely(err))
		return err;

	return nilfs_copy_dirty_pages(&gii->i_btnode_cache,
				      &dii->i_btnode_cache);
}

void nilfs_commit_gcdat_inode(struct the_nilfs *nilfs)
{
	struct inode *dat = nilfs->ns_dat, *gcdat = nilfs->ns_gc_dat;
	struct nilfs_inode_info *dii = NILFS_I(dat), *gii = NILFS_I(gcdat);
	struct address_space *mapping = dat->i_mapping;
	struct address_space *gmapping = gcdat->i_mapping;

	down_write(&NILFS_MDT(dat)->mi_sem);
	dat->i_blocks = gcdat->i_blocks;
	dii->i_flags = gii->i_flags;
	dii->i_state = gii->i_state & ~(1 << NILFS_I_GCDAT);

	nilfs_bmap_commit_gcdat(gii->i_bmap, dii->i_bmap);

	nilfs_palloc_clear_cache(dat);
	nilfs_palloc_clear_cache(gcdat);
	nilfs_clear_dirty_pages(mapping);
	nilfs_copy_back_pages(mapping, gmapping);
	/* note: mdt dirty flags should be cleared by segctor. */

	nilfs_clear_dirty_pages(&dii->i_btnode_cache);
	nilfs_copy_back_pages(&dii->i_btnode_cache, &gii->i_btnode_cache);

	up_write(&NILFS_MDT(dat)->mi_sem);
}

void nilfs_clear_gcdat_inode(struct the_nilfs *nilfs)
{
	struct inode *gcdat = nilfs->ns_gc_dat;
	struct nilfs_inode_info *gii = NILFS_I(gcdat);

	gcdat->i_state = I_CLEAR;
	gii->i_flags = 0;

	nilfs_palloc_clear_cache(gcdat);
	truncate_inode_pages(gcdat->i_mapping, 0);
	truncate_inode_pages(&gii->i_btnode_cache, 0);
}
