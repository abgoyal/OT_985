

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/time.h>
#include <linux/mm.h>
#include <linux/highuid.h>
#include <linux/smp_lock.h>
#include <linux/net.h>

#include <linux/smb_fs.h>
#include <linux/smb_mount.h>

#include <asm/uaccess.h>

#include "proto.h"

long
smb_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct smb_sb_info *server = server_from_inode(filp->f_path.dentry->d_inode);
	struct smb_conn_opt opt;
	int result = -EINVAL;

	lock_kernel();
	switch (cmd) {
		uid16_t uid16;
		uid_t uid32;
	case SMB_IOC_GETMOUNTUID:
		SET_UID(uid16, server->mnt->mounted_uid);
		result = put_user(uid16, (uid16_t __user *) arg);
		break;
	case SMB_IOC_GETMOUNTUID32:
		SET_UID(uid32, server->mnt->mounted_uid);
		result = put_user(uid32, (uid_t __user *) arg);
		break;

	case SMB_IOC_NEWCONN:
		/* arg is smb_conn_opt, or NULL if no connection was made */
		if (!arg) {
			result = 0;
			smb_lock_server(server);
			server->state = CONN_RETRIED;
			printk(KERN_ERR "Connection attempt failed!  [%d]\n",
			       server->conn_error);
			smbiod_flush(server);
			smb_unlock_server(server);
			break;
		}

		result = -EFAULT;
		if (!copy_from_user(&opt, (void __user *)arg, sizeof(opt)))
			result = smb_newconn(server, &opt);
		break;
	default:
		break;
	}
	unlock_kernel();

	return result;
}
