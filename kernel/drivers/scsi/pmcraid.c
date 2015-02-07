
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/blkdev.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/hdreg.h>
#include <linux/version.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <asm/irq.h>
#include <asm/processor.h>
#include <linux/libata.h>
#include <linux/mutex.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsicam.h>

#include "pmcraid.h"

static unsigned int pmcraid_debug_log;
static unsigned int pmcraid_disable_aen;
static unsigned int pmcraid_log_level = IOASC_LOG_LEVEL_MUST;

static atomic_t pmcraid_adapter_count = ATOMIC_INIT(0);

static unsigned int pmcraid_major;
static struct class *pmcraid_class;
DECLARE_BITMAP(pmcraid_minor, PMCRAID_MAX_ADAPTERS);

MODULE_AUTHOR("Anil Ravindranath<anil_ravindranath@pmc-sierra.com>");
MODULE_DESCRIPTION("PMC Sierra MaxRAID Controller Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(PMCRAID_DRIVER_VERSION);

module_param_named(log_level, pmcraid_log_level, uint, (S_IRUGO | S_IWUSR));
MODULE_PARM_DESC(log_level,
		 "Enables firmware error code logging, default :1 high-severity"
		 " errors, 2: all errors including high-severity errors,"
		 " 0: disables logging");

module_param_named(debug, pmcraid_debug_log, uint, (S_IRUGO | S_IWUSR));
MODULE_PARM_DESC(debug,
		 "Enable driver verbose message logging. Set 1 to enable."
		 "(default: 0)");

module_param_named(disable_aen, pmcraid_disable_aen, uint, (S_IRUGO | S_IWUSR));
MODULE_PARM_DESC(disable_aen,
		 "Disable driver aen notifications to apps. Set 1 to disable."
		 "(default: 0)");

static struct pmcraid_chip_details pmcraid_chip_cfg[] = {
	{
	 .ioastatus = 0x0,
	 .ioarrin = 0x00040,
	 .mailbox = 0x7FC30,
	 .global_intr_mask = 0x00034,
	 .ioa_host_intr = 0x0009C,
	 .ioa_host_intr_clr = 0x000A0,
	 .ioa_host_mask = 0x7FC28,
	 .ioa_host_mask_clr = 0x7FC28,
	 .host_ioa_intr = 0x00020,
	 .host_ioa_intr_clr = 0x00020,
	 .transop_timeout = 300
	 }
};

static struct pci_device_id pmcraid_pci_table[] __devinitdata = {
	{ PCI_DEVICE(PCI_VENDOR_ID_PMC, PCI_DEVICE_ID_PMC_MAXRAID),
	  0, 0, (kernel_ulong_t)&pmcraid_chip_cfg[0]
	},
	{}
};

MODULE_DEVICE_TABLE(pci, pmcraid_pci_table);



static int pmcraid_slave_alloc(struct scsi_device *scsi_dev)
{
	struct pmcraid_resource_entry *temp, *res = NULL;
	struct pmcraid_instance *pinstance;
	u8 target, bus, lun;
	unsigned long lock_flags;
	int rc = -ENXIO;
	pinstance = shost_priv(scsi_dev->host);

	/* Driver exposes VSET and GSCSI resources only; all other device types
	 * are not exposed. Resource list is synchronized using resource lock
	 * so any traversal or modifications to the list should be done inside
	 * this lock
	 */
	spin_lock_irqsave(&pinstance->resource_lock, lock_flags);
	list_for_each_entry(temp, &pinstance->used_res_q, queue) {

		/* do not expose VSETs with order-ids > MAX_VSET_TARGETS */
		if (RES_IS_VSET(temp->cfg_entry)) {
			target = temp->cfg_entry.unique_flags1;
			if (target > PMCRAID_MAX_VSET_TARGETS)
				continue;
			bus = PMCRAID_VSET_BUS_ID;
			lun = 0;
		} else if (RES_IS_GSCSI(temp->cfg_entry)) {
			target = RES_TARGET(temp->cfg_entry.resource_address);
			bus = PMCRAID_PHYS_BUS_ID;
			lun = RES_LUN(temp->cfg_entry.resource_address);
		} else {
			continue;
		}

		if (bus == scsi_dev->channel &&
		    target == scsi_dev->id &&
		    lun == scsi_dev->lun) {
			res = temp;
			break;
		}
	}

	if (res) {
		res->scsi_dev = scsi_dev;
		scsi_dev->hostdata = res;
		res->change_detected = 0;
		atomic_set(&res->read_failures, 0);
		atomic_set(&res->write_failures, 0);
		rc = 0;
	}
	spin_unlock_irqrestore(&pinstance->resource_lock, lock_flags);
	return rc;
}

static int pmcraid_slave_configure(struct scsi_device *scsi_dev)
{
	struct pmcraid_resource_entry *res = scsi_dev->hostdata;

	if (!res)
		return 0;

	/* LLD exposes VSETs and Enclosure devices only */
	if (RES_IS_GSCSI(res->cfg_entry) &&
	    scsi_dev->type != TYPE_ENCLOSURE)
		return -ENXIO;

	pmcraid_info("configuring %x:%x:%x:%x\n",
		     scsi_dev->host->unique_id,
		     scsi_dev->channel,
		     scsi_dev->id,
		     scsi_dev->lun);

	if (RES_IS_GSCSI(res->cfg_entry)) {
		scsi_dev->allow_restart = 1;
	} else if (RES_IS_VSET(res->cfg_entry)) {
		scsi_dev->allow_restart = 1;
		blk_queue_rq_timeout(scsi_dev->request_queue,
				     PMCRAID_VSET_IO_TIMEOUT);
		blk_queue_max_hw_sectors(scsi_dev->request_queue,
				      PMCRAID_VSET_MAX_SECTORS);
	}

	if (scsi_dev->tagged_supported &&
	    (RES_IS_GSCSI(res->cfg_entry) || RES_IS_VSET(res->cfg_entry))) {
		scsi_activate_tcq(scsi_dev, scsi_dev->queue_depth);
		scsi_adjust_queue_depth(scsi_dev, MSG_SIMPLE_TAG,
					scsi_dev->host->cmd_per_lun);
	} else {
		scsi_adjust_queue_depth(scsi_dev, 0,
					scsi_dev->host->cmd_per_lun);
	}

	return 0;
}

static void pmcraid_slave_destroy(struct scsi_device *scsi_dev)
{
	struct pmcraid_resource_entry *res;

	res = (struct pmcraid_resource_entry *)scsi_dev->hostdata;

	if (res)
		res->scsi_dev = NULL;

	scsi_dev->hostdata = NULL;
}

static int pmcraid_change_queue_depth(struct scsi_device *scsi_dev, int depth,
				      int reason)
{
	if (reason != SCSI_QDEPTH_DEFAULT)
		return -EOPNOTSUPP;

	if (depth > PMCRAID_MAX_CMD_PER_LUN)
		depth = PMCRAID_MAX_CMD_PER_LUN;

	scsi_adjust_queue_depth(scsi_dev, scsi_get_tag_type(scsi_dev), depth);

	return scsi_dev->queue_depth;
}

static int pmcraid_change_queue_type(struct scsi_device *scsi_dev, int tag)
{
	struct pmcraid_resource_entry *res;

	res = (struct pmcraid_resource_entry *)scsi_dev->hostdata;

	if ((res) && scsi_dev->tagged_supported &&
	    (RES_IS_GSCSI(res->cfg_entry) || RES_IS_VSET(res->cfg_entry))) {
		scsi_set_tag_type(scsi_dev, tag);

		if (tag)
			scsi_activate_tcq(scsi_dev, scsi_dev->queue_depth);
		else
			scsi_deactivate_tcq(scsi_dev, scsi_dev->queue_depth);
	} else
		tag = 0;

	return tag;
}


void pmcraid_init_cmdblk(struct pmcraid_cmd *cmd, int index)
{
	struct pmcraid_ioarcb *ioarcb = &(cmd->ioa_cb->ioarcb);
	dma_addr_t dma_addr = cmd->ioa_cb_bus_addr;

	if (index >= 0) {
		/* first time initialization (called from  probe) */
		u32 ioasa_offset =
			offsetof(struct pmcraid_control_block, ioasa);

		cmd->index = index;
		ioarcb->response_handle = cpu_to_le32(index << 2);
		ioarcb->ioarcb_bus_addr = cpu_to_le64(dma_addr);
		ioarcb->ioasa_bus_addr = cpu_to_le64(dma_addr + ioasa_offset);
		ioarcb->ioasa_len = cpu_to_le16(sizeof(struct pmcraid_ioasa));
	} else {
		/* re-initialization of various lengths, called once command is
		 * processed by IOA
		 */
		memset(&cmd->ioa_cb->ioarcb.cdb, 0, PMCRAID_MAX_CDB_LEN);
		ioarcb->request_flags0 = 0;
		ioarcb->request_flags1 = 0;
		ioarcb->cmd_timeout = 0;
		ioarcb->ioarcb_bus_addr &= (~0x1FULL);
		ioarcb->ioadl_bus_addr = 0;
		ioarcb->ioadl_length = 0;
		ioarcb->data_transfer_length = 0;
		ioarcb->add_cmd_param_length = 0;
		ioarcb->add_cmd_param_offset = 0;
		cmd->ioa_cb->ioasa.ioasc = 0;
		cmd->ioa_cb->ioasa.residual_data_length = 0;
		cmd->u.time_left = 0;
	}

	cmd->cmd_done = NULL;
	cmd->scsi_cmd = NULL;
	cmd->release = 0;
	cmd->completion_req = 0;
	cmd->dma_handle = 0;
	init_timer(&cmd->timer);
}

static void pmcraid_reinit_cmdblk(struct pmcraid_cmd *cmd)
{
	pmcraid_init_cmdblk(cmd, -1);
}

static struct pmcraid_cmd *pmcraid_get_free_cmd(
	struct pmcraid_instance *pinstance
)
{
	struct pmcraid_cmd *cmd = NULL;
	unsigned long lock_flags;

	/* free cmd block list is protected by free_pool_lock */
	spin_lock_irqsave(&pinstance->free_pool_lock, lock_flags);

	if (!list_empty(&pinstance->free_cmd_pool)) {
		cmd = list_entry(pinstance->free_cmd_pool.next,
				 struct pmcraid_cmd, free_list);
		list_del(&cmd->free_list);
	}
	spin_unlock_irqrestore(&pinstance->free_pool_lock, lock_flags);

	/* Initialize the command block before giving it the caller */
	if (cmd != NULL)
		pmcraid_reinit_cmdblk(cmd);
	return cmd;
}

void pmcraid_return_cmd(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	unsigned long lock_flags;

	spin_lock_irqsave(&pinstance->free_pool_lock, lock_flags);
	list_add_tail(&cmd->free_list, &pinstance->free_cmd_pool);
	spin_unlock_irqrestore(&pinstance->free_pool_lock, lock_flags);
}

static u32 pmcraid_read_interrupts(struct pmcraid_instance *pinstance)
{
	return ioread32(pinstance->int_regs.ioa_host_interrupt_reg);
}

static void pmcraid_disable_interrupts(
	struct pmcraid_instance *pinstance,
	u32 intrs
)
{
	u32 gmask = ioread32(pinstance->int_regs.global_interrupt_mask_reg);
	u32 nmask = gmask | GLOBAL_INTERRUPT_MASK;

	iowrite32(nmask, pinstance->int_regs.global_interrupt_mask_reg);
	iowrite32(intrs, pinstance->int_regs.ioa_host_interrupt_clr_reg);
	iowrite32(intrs, pinstance->int_regs.ioa_host_interrupt_mask_reg);
	ioread32(pinstance->int_regs.ioa_host_interrupt_mask_reg);
}

static void pmcraid_enable_interrupts(
	struct pmcraid_instance *pinstance,
	u32 intrs
)
{
	u32 gmask = ioread32(pinstance->int_regs.global_interrupt_mask_reg);
	u32 nmask = gmask & (~GLOBAL_INTERRUPT_MASK);

	iowrite32(nmask, pinstance->int_regs.global_interrupt_mask_reg);
	iowrite32(~intrs, pinstance->int_regs.ioa_host_interrupt_mask_reg);
	ioread32(pinstance->int_regs.ioa_host_interrupt_mask_reg);

	pmcraid_info("enabled interrupts global mask = %x intr_mask = %x\n",
		ioread32(pinstance->int_regs.global_interrupt_mask_reg),
		ioread32(pinstance->int_regs.ioa_host_interrupt_mask_reg));
}

static void pmcraid_reset_type(struct pmcraid_instance *pinstance)
{
	u32 mask;
	u32 intrs;
	u32 alerts;

	mask = ioread32(pinstance->int_regs.ioa_host_interrupt_mask_reg);
	intrs = ioread32(pinstance->int_regs.ioa_host_interrupt_reg);
	alerts = ioread32(pinstance->int_regs.host_ioa_interrupt_reg);

	if ((mask & INTRS_HRRQ_VALID) == 0 ||
	    (alerts & DOORBELL_IOA_RESET_ALERT) ||
	    (intrs & PMCRAID_ERROR_INTERRUPTS)) {
		pmcraid_info("IOA requires hard reset\n");
		pinstance->ioa_hard_reset = 1;
	}

	/* If unit check is active, trigger the dump */
	if (intrs & INTRS_IOA_UNIT_CHECK)
		pinstance->ioa_unit_check = 1;
}


static void pmcraid_ioa_reset(struct pmcraid_cmd *);

static void pmcraid_bist_done(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	unsigned long lock_flags;
	int rc;
	u16 pci_reg;

	rc = pci_read_config_word(pinstance->pdev, PCI_COMMAND, &pci_reg);

	/* If PCI config space can't be accessed wait for another two secs */
	if ((rc != PCIBIOS_SUCCESSFUL || (!(pci_reg & PCI_COMMAND_MEMORY))) &&
	    cmd->u.time_left > 0) {
		pmcraid_info("BIST not complete, waiting another 2 secs\n");
		cmd->timer.expires = jiffies + cmd->u.time_left;
		cmd->u.time_left = 0;
		cmd->timer.data = (unsigned long)cmd;
		cmd->timer.function =
			(void (*)(unsigned long))pmcraid_bist_done;
		add_timer(&cmd->timer);
	} else {
		cmd->u.time_left = 0;
		pmcraid_info("BIST is complete, proceeding with reset\n");
		spin_lock_irqsave(pinstance->host->host_lock, lock_flags);
		pmcraid_ioa_reset(cmd);
		spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);
	}
}

static void pmcraid_start_bist(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	u32 doorbells, intrs;

	/* proceed with bist and wait for 2 seconds */
	iowrite32(DOORBELL_IOA_START_BIST,
		pinstance->int_regs.host_ioa_interrupt_reg);
	doorbells = ioread32(pinstance->int_regs.host_ioa_interrupt_reg);
	intrs = ioread32(pinstance->int_regs.ioa_host_interrupt_reg);
	pmcraid_info("doorbells after start bist: %x intrs: %x \n",
		      doorbells, intrs);

	cmd->u.time_left = msecs_to_jiffies(PMCRAID_BIST_TIMEOUT);
	cmd->timer.data = (unsigned long)cmd;
	cmd->timer.expires = jiffies + msecs_to_jiffies(PMCRAID_BIST_TIMEOUT);
	cmd->timer.function = (void (*)(unsigned long))pmcraid_bist_done;
	add_timer(&cmd->timer);
}

static void pmcraid_reset_alert_done(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	u32 status = ioread32(pinstance->ioa_status);
	unsigned long lock_flags;

	/* if the critical operation in progress bit is set or the wait times
	 * out, invoke reset engine to proceed with hard reset. If there is
	 * some more time to wait, restart the timer
	 */
	if (((status & INTRS_CRITICAL_OP_IN_PROGRESS) == 0) ||
	    cmd->u.time_left <= 0) {
		pmcraid_info("critical op is reset proceeding with reset\n");
		spin_lock_irqsave(pinstance->host->host_lock, lock_flags);
		pmcraid_ioa_reset(cmd);
		spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);
	} else {
		pmcraid_info("critical op is not yet reset waiting again\n");
		/* restart timer if some more time is available to wait */
		cmd->u.time_left -= PMCRAID_CHECK_FOR_RESET_TIMEOUT;
		cmd->timer.data = (unsigned long)cmd;
		cmd->timer.expires = jiffies + PMCRAID_CHECK_FOR_RESET_TIMEOUT;
		cmd->timer.function =
			(void (*)(unsigned long))pmcraid_reset_alert_done;
		add_timer(&cmd->timer);
	}
}

static void pmcraid_reset_alert(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	u32 doorbells;
	int rc;
	u16 pci_reg;

	/* If we are able to access IOA PCI config space, alert IOA that we are
	 * going to reset it soon. This enables IOA to preserv persistent error
	 * data if any. In case memory space is not accessible, proceed with
	 * BIST or slot_reset
	 */
	rc = pci_read_config_word(pinstance->pdev, PCI_COMMAND, &pci_reg);
	if ((rc == PCIBIOS_SUCCESSFUL) && (pci_reg & PCI_COMMAND_MEMORY)) {

		/* wait for IOA permission i.e until CRITICAL_OPERATION bit is
		 * reset IOA doesn't generate any interrupts when CRITICAL
		 * OPERATION bit is reset. A timer is started to wait for this
		 * bit to be reset.
		 */
		cmd->u.time_left = PMCRAID_RESET_TIMEOUT;
		cmd->timer.data = (unsigned long)cmd;
		cmd->timer.expires = jiffies + PMCRAID_CHECK_FOR_RESET_TIMEOUT;
		cmd->timer.function =
			(void (*)(unsigned long))pmcraid_reset_alert_done;
		add_timer(&cmd->timer);

		iowrite32(DOORBELL_IOA_RESET_ALERT,
			pinstance->int_regs.host_ioa_interrupt_reg);
		doorbells =
			ioread32(pinstance->int_regs.host_ioa_interrupt_reg);
		pmcraid_info("doorbells after reset alert: %x\n", doorbells);
	} else {
		pmcraid_info("PCI config is not accessible starting BIST\n");
		pinstance->ioa_state = IOA_STATE_IN_HARD_RESET;
		pmcraid_start_bist(cmd);
	}
}

static void pmcraid_timeout_handler(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	unsigned long lock_flags;

	dev_info(&pinstance->pdev->dev,
		"Adapter being reset due to command timeout.\n");

	/* Command timeouts result in hard reset sequence. The command that got
	 * timed out may be the one used as part of reset sequence. In this
	 * case restart reset sequence using the same command block even if
	 * reset is in progress. Otherwise fail this command and get a free
	 * command block to restart the reset sequence.
	 */
	spin_lock_irqsave(pinstance->host->host_lock, lock_flags);
	if (!pinstance->ioa_reset_in_progress) {
		pinstance->ioa_reset_attempts = 0;
		cmd = pmcraid_get_free_cmd(pinstance);

		/* If we are out of command blocks, just return here itself.
		 * Some other command's timeout handler can do the reset job
		 */
		if (cmd == NULL) {
			spin_unlock_irqrestore(pinstance->host->host_lock,
					       lock_flags);
			pmcraid_err("no free cmnd block for timeout handler\n");
			return;
		}

		pinstance->reset_cmd = cmd;
		pinstance->ioa_reset_in_progress = 1;
	} else {
		pmcraid_info("reset is already in progress\n");

		if (pinstance->reset_cmd != cmd) {
			/* This command should have been given to IOA, this
			 * command will be completed by fail_outstanding_cmds
			 * anyway
			 */
			pmcraid_err("cmd is pending but reset in progress\n");
		}

		/* If this command was being used as part of the reset
		 * sequence, set cmd_done pointer to pmcraid_ioa_reset. This
		 * causes fail_outstanding_commands not to return the command
		 * block back to free pool
		 */
		if (cmd == pinstance->reset_cmd)
			cmd->cmd_done = pmcraid_ioa_reset;

	}

	pinstance->ioa_state = IOA_STATE_IN_RESET_ALERT;
	scsi_block_requests(pinstance->host);
	pmcraid_reset_alert(cmd);
	spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);
}

static void pmcraid_internal_done(struct pmcraid_cmd *cmd)
{
	pmcraid_info("response internal cmd CDB[0] = %x ioasc = %x\n",
		     cmd->ioa_cb->ioarcb.cdb[0],
		     le32_to_cpu(cmd->ioa_cb->ioasa.ioasc));

	/* Some of the internal commands are sent with callers blocking for the
	 * response. Same will be indicated as part of cmd->completion_req
	 * field. Response path needs to wake up any waiters waiting for cmd
	 * completion if this flag is set.
	 */
	if (cmd->completion_req) {
		cmd->completion_req = 0;
		complete(&cmd->wait_for_completion);
	}

	/* most of the internal commands are completed by caller itself, so
	 * no need to return the command block back to free pool until we are
	 * required to do so (e.g once done with initialization).
	 */
	if (cmd->release) {
		cmd->release = 0;
		pmcraid_return_cmd(cmd);
	}
}

static void pmcraid_reinit_cfgtable_done(struct pmcraid_cmd *cmd)
{
	pmcraid_info("response internal cmd CDB[0] = %x ioasc = %x\n",
		     cmd->ioa_cb->ioarcb.cdb[0],
		     le32_to_cpu(cmd->ioa_cb->ioasa.ioasc));

	if (cmd->release) {
		cmd->release = 0;
		pmcraid_return_cmd(cmd);
	}
	pmcraid_info("scheduling worker for config table reinitialization\n");
	schedule_work(&cmd->drv_inst->worker_q);
}

static void pmcraid_erp_done(struct pmcraid_cmd *cmd)
{
	struct scsi_cmnd *scsi_cmd = cmd->scsi_cmd;
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	u32 ioasc = le32_to_cpu(cmd->ioa_cb->ioasa.ioasc);

	if (PMCRAID_IOASC_SENSE_KEY(ioasc) > 0) {
		scsi_cmd->result |= (DID_ERROR << 16);
		scmd_printk(KERN_INFO, scsi_cmd,
			    "command CDB[0] = %x failed with IOASC: 0x%08X\n",
			    cmd->ioa_cb->ioarcb.cdb[0], ioasc);
	}

	/* if we had allocated sense buffers for request sense, copy the sense
	 * release the buffers
	 */
	if (cmd->sense_buffer != NULL) {
		memcpy(scsi_cmd->sense_buffer,
		       cmd->sense_buffer,
		       SCSI_SENSE_BUFFERSIZE);
		pci_free_consistent(pinstance->pdev,
				    SCSI_SENSE_BUFFERSIZE,
				    cmd->sense_buffer, cmd->sense_buffer_dma);
		cmd->sense_buffer = NULL;
		cmd->sense_buffer_dma = 0;
	}

	scsi_dma_unmap(scsi_cmd);
	pmcraid_return_cmd(cmd);
	scsi_cmd->scsi_done(scsi_cmd);
}

static void _pmcraid_fire_command(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	unsigned long lock_flags;

	/* Add this command block to pending cmd pool. We do this prior to
	 * writting IOARCB to ioarrin because IOA might complete the command
	 * by the time we are about to add it to the list. Response handler
	 * (isr/tasklet) looks for cmb block in the pending pending list.
	 */
	spin_lock_irqsave(&pinstance->pending_pool_lock, lock_flags);
	list_add_tail(&cmd->free_list, &pinstance->pending_cmd_pool);
	spin_unlock_irqrestore(&pinstance->pending_pool_lock, lock_flags);
	atomic_inc(&pinstance->outstanding_cmds);

	/* driver writes lower 32-bit value of IOARCB address only */
	mb();
	iowrite32(le32_to_cpu(cmd->ioa_cb->ioarcb.ioarcb_bus_addr),
		  pinstance->ioarrin);
}

static void pmcraid_send_cmd(
	struct pmcraid_cmd *cmd,
	void (*cmd_done) (struct pmcraid_cmd *),
	unsigned long timeout,
	void (*timeout_func) (struct pmcraid_cmd *)
)
{
	/* initialize done function */
	cmd->cmd_done = cmd_done;

	if (timeout_func) {
		/* setup timeout handler */
		cmd->timer.data = (unsigned long)cmd;
		cmd->timer.expires = jiffies + timeout;
		cmd->timer.function = (void (*)(unsigned long))timeout_func;
		add_timer(&cmd->timer);
	}

	/* fire the command to IOA */
	_pmcraid_fire_command(cmd);
}

static void pmcraid_ioa_shutdown(struct pmcraid_cmd *cmd)
{
	pmcraid_info("response for Cancel CCN CDB[0] = %x ioasc = %x\n",
		     cmd->ioa_cb->ioarcb.cdb[0],
		     le32_to_cpu(cmd->ioa_cb->ioasa.ioasc));

	/* Note that commands sent during reset require next command to be sent
	 * to IOA. Hence reinit the done function as well as timeout function
	 */
	pmcraid_reinit_cmdblk(cmd);
	cmd->ioa_cb->ioarcb.request_type = REQ_TYPE_IOACMD;
	cmd->ioa_cb->ioarcb.resource_handle =
		cpu_to_le32(PMCRAID_IOA_RES_HANDLE);
	cmd->ioa_cb->ioarcb.cdb[0] = PMCRAID_IOA_SHUTDOWN;
	cmd->ioa_cb->ioarcb.cdb[1] = PMCRAID_SHUTDOWN_NORMAL;

	/* fire shutdown command to hardware. */
	pmcraid_info("firing normal shutdown command (%d) to IOA\n",
		     le32_to_cpu(cmd->ioa_cb->ioarcb.response_handle));

	pmcraid_send_cmd(cmd, pmcraid_ioa_reset,
			 PMCRAID_SHUTDOWN_TIMEOUT,
			 pmcraid_timeout_handler);
}


static void pmcraid_querycfg(struct pmcraid_cmd *);

static void pmcraid_identify_hrrq(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	struct pmcraid_ioarcb *ioarcb = &cmd->ioa_cb->ioarcb;
	int index = 0;
	__be64 hrrq_addr = cpu_to_be64(pinstance->hrrq_start_bus_addr[index]);
	u32 hrrq_size = cpu_to_be32(sizeof(u32) * PMCRAID_MAX_CMD);

	pmcraid_reinit_cmdblk(cmd);

	/* Initialize ioarcb */
	ioarcb->request_type = REQ_TYPE_IOACMD;
	ioarcb->resource_handle = cpu_to_le32(PMCRAID_IOA_RES_HANDLE);

	/* initialize the hrrq number where IOA will respond to this command */
	ioarcb->hrrq_id = index;
	ioarcb->cdb[0] = PMCRAID_IDENTIFY_HRRQ;
	ioarcb->cdb[1] = index;

	/* IOA expects 64-bit pci address to be written in B.E format
	 * (i.e cdb[2]=MSByte..cdb[9]=LSB.
	 */
	pmcraid_info("HRRQ_IDENTIFY with hrrq:ioarcb => %llx:%llx\n",
		     hrrq_addr, ioarcb->ioarcb_bus_addr);

	memcpy(&(ioarcb->cdb[2]), &hrrq_addr, sizeof(hrrq_addr));
	memcpy(&(ioarcb->cdb[10]), &hrrq_size, sizeof(hrrq_size));

	/* Subsequent commands require HRRQ identification to be successful.
	 * Note that this gets called even during reset from SCSI mid-layer
	 * or tasklet
	 */
	pmcraid_send_cmd(cmd, pmcraid_querycfg,
			 PMCRAID_INTERNAL_TIMEOUT,
			 pmcraid_timeout_handler);
}

static void pmcraid_process_ccn(struct pmcraid_cmd *cmd);
static void pmcraid_process_ldn(struct pmcraid_cmd *cmd);

static void pmcraid_send_hcam_cmd(struct pmcraid_cmd *cmd)
{
	if (cmd->ioa_cb->ioarcb.cdb[1] == PMCRAID_HCAM_CODE_CONFIG_CHANGE)
		atomic_set(&(cmd->drv_inst->ccn.ignore), 0);
	else
		atomic_set(&(cmd->drv_inst->ldn.ignore), 0);

	pmcraid_send_cmd(cmd, cmd->cmd_done, 0, NULL);
}

static struct pmcraid_cmd *pmcraid_init_hcam
(
	struct pmcraid_instance *pinstance,
	u8 type
)
{
	struct pmcraid_cmd *cmd;
	struct pmcraid_ioarcb *ioarcb;
	struct pmcraid_ioadl_desc *ioadl;
	struct pmcraid_hostrcb *hcam;
	void (*cmd_done) (struct pmcraid_cmd *);
	dma_addr_t dma;
	int rcb_size;

	cmd = pmcraid_get_free_cmd(pinstance);

	if (!cmd) {
		pmcraid_err("no free command blocks for hcam\n");
		return cmd;
	}

	if (type == PMCRAID_HCAM_CODE_CONFIG_CHANGE) {
		rcb_size = sizeof(struct pmcraid_hcam_ccn);
		cmd_done = pmcraid_process_ccn;
		dma = pinstance->ccn.baddr + PMCRAID_AEN_HDR_SIZE;
		hcam = &pinstance->ccn;
	} else {
		rcb_size = sizeof(struct pmcraid_hcam_ldn);
		cmd_done = pmcraid_process_ldn;
		dma = pinstance->ldn.baddr + PMCRAID_AEN_HDR_SIZE;
		hcam = &pinstance->ldn;
	}

	/* initialize command pointer used for HCAM registration */
	hcam->cmd = cmd;

	ioarcb = &cmd->ioa_cb->ioarcb;
	ioarcb->ioadl_bus_addr = cpu_to_le64((cmd->ioa_cb_bus_addr) +
					offsetof(struct pmcraid_ioarcb,
						add_data.u.ioadl[0]));
	ioarcb->ioadl_length = cpu_to_le32(sizeof(struct pmcraid_ioadl_desc));
	ioadl = ioarcb->add_data.u.ioadl;

	/* Initialize ioarcb */
	ioarcb->request_type = REQ_TYPE_HCAM;
	ioarcb->resource_handle = cpu_to_le32(PMCRAID_IOA_RES_HANDLE);
	ioarcb->cdb[0] = PMCRAID_HOST_CONTROLLED_ASYNC;
	ioarcb->cdb[1] = type;
	ioarcb->cdb[7] = (rcb_size >> 8) & 0xFF;
	ioarcb->cdb[8] = (rcb_size) & 0xFF;

	ioarcb->data_transfer_length = cpu_to_le32(rcb_size);

	ioadl[0].flags |= IOADL_FLAGS_READ_LAST;
	ioadl[0].data_len = cpu_to_le32(rcb_size);
	ioadl[0].address = cpu_to_le32(dma);

	cmd->cmd_done = cmd_done;
	return cmd;
}

static void pmcraid_send_hcam(struct pmcraid_instance *pinstance, u8 type)
{
	struct pmcraid_cmd *cmd = pmcraid_init_hcam(pinstance, type);
	pmcraid_send_hcam_cmd(cmd);
}


static void pmcraid_prepare_cancel_cmd(
	struct pmcraid_cmd *cmd,
	struct pmcraid_cmd *cmd_to_cancel
)
{
	struct pmcraid_ioarcb *ioarcb = &cmd->ioa_cb->ioarcb;
	__be64 ioarcb_addr = cmd_to_cancel->ioa_cb->ioarcb.ioarcb_bus_addr;

	/* Get the resource handle to where the command to be aborted has been
	 * sent.
	 */
	ioarcb->resource_handle = cmd_to_cancel->ioa_cb->ioarcb.resource_handle;
	ioarcb->request_type = REQ_TYPE_IOACMD;
	memset(ioarcb->cdb, 0, PMCRAID_MAX_CDB_LEN);
	ioarcb->cdb[0] = PMCRAID_ABORT_CMD;

	/* IOARCB address of the command to be cancelled is given in
	 * cdb[2]..cdb[9] is Big-Endian format. Note that length bits in
	 * IOARCB address are not masked.
	 */
	ioarcb_addr = cpu_to_be64(ioarcb_addr);
	memcpy(&(ioarcb->cdb[2]), &ioarcb_addr, sizeof(ioarcb_addr));
}

static void pmcraid_cancel_hcam(
	struct pmcraid_cmd *cmd,
	u8 type,
	void (*cmd_done) (struct pmcraid_cmd *)
)
{
	struct pmcraid_instance *pinstance;
	struct pmcraid_hostrcb  *hcam;

	pinstance = cmd->drv_inst;
	hcam =  (type == PMCRAID_HCAM_CODE_LOG_DATA) ?
		&pinstance->ldn : &pinstance->ccn;

	/* prepare for cancelling previous hcam command. If the HCAM is
	 * currently not pending with IOA, we would have hcam->cmd as non-null
	 */
	if (hcam->cmd == NULL)
		return;

	pmcraid_prepare_cancel_cmd(cmd, hcam->cmd);

	/* writing to IOARRIN must be protected by host_lock, as mid-layer
	 * schedule queuecommand while we are doing this
	 */
	pmcraid_send_cmd(cmd, cmd_done,
			 PMCRAID_INTERNAL_TIMEOUT,
			 pmcraid_timeout_handler);
}

static void pmcraid_cancel_ccn(struct pmcraid_cmd *cmd)
{
	pmcraid_info("response for Cancel LDN CDB[0] = %x ioasc = %x\n",
		     cmd->ioa_cb->ioarcb.cdb[0],
		     le32_to_cpu(cmd->ioa_cb->ioasa.ioasc));

	pmcraid_reinit_cmdblk(cmd);

	pmcraid_cancel_hcam(cmd,
			    PMCRAID_HCAM_CODE_CONFIG_CHANGE,
			    pmcraid_ioa_shutdown);
}

static void pmcraid_cancel_ldn(struct pmcraid_cmd *cmd)
{
	pmcraid_cancel_hcam(cmd,
			    PMCRAID_HCAM_CODE_LOG_DATA,
			    pmcraid_cancel_ccn);
}

static int pmcraid_expose_resource(struct pmcraid_config_table_entry *cfgte)
{
	int retval = 0;

	if (cfgte->resource_type == RES_TYPE_VSET)
		retval = ((cfgte->unique_flags1 & 0x80) == 0);
	else if (cfgte->resource_type == RES_TYPE_GSCSI)
		retval = (RES_BUS(cfgte->resource_address) !=
				PMCRAID_VIRTUAL_ENCL_BUS_ID);
	return retval;
}

/* attributes supported by pmcraid_event_family */
enum {
	PMCRAID_AEN_ATTR_UNSPEC,
	PMCRAID_AEN_ATTR_EVENT,
	__PMCRAID_AEN_ATTR_MAX,
};
#define PMCRAID_AEN_ATTR_MAX (__PMCRAID_AEN_ATTR_MAX - 1)

/* commands supported by pmcraid_event_family */
enum {
	PMCRAID_AEN_CMD_UNSPEC,
	PMCRAID_AEN_CMD_EVENT,
	__PMCRAID_AEN_CMD_MAX,
};
#define PMCRAID_AEN_CMD_MAX (__PMCRAID_AEN_CMD_MAX - 1)

static struct genl_family pmcraid_event_family = {
	.id = GENL_ID_GENERATE,
	.name = "pmcraid",
	.version = 1,
	.maxattr = PMCRAID_AEN_ATTR_MAX
};

static int pmcraid_netlink_init(void)
{
	int result;

	result = genl_register_family(&pmcraid_event_family);

	if (result)
		return result;

	pmcraid_info("registered NETLINK GENERIC group: %d\n",
		     pmcraid_event_family.id);

	return result;
}

static void pmcraid_netlink_release(void)
{
	genl_unregister_family(&pmcraid_event_family);
}

static int pmcraid_notify_aen(struct pmcraid_instance *pinstance, u8 type)
{
	struct sk_buff *skb;
	struct pmcraid_aen_msg *aen_msg;
	void *msg_header;
	int data_size, total_size;
	int result;


	if (type == PMCRAID_HCAM_CODE_LOG_DATA) {
		aen_msg = pinstance->ldn.msg;
		data_size = pinstance->ldn.hcam->data_len;
	} else {
		aen_msg = pinstance->ccn.msg;
		data_size = pinstance->ccn.hcam->data_len;
	}

	data_size += sizeof(struct pmcraid_hcam_hdr);
	aen_msg->hostno = (pinstance->host->unique_id << 16 |
			   MINOR(pinstance->cdev.dev));
	aen_msg->length = data_size;
	data_size += sizeof(*aen_msg);

	total_size = nla_total_size(data_size);
	skb = genlmsg_new(total_size, GFP_ATOMIC);


	if (!skb) {
		pmcraid_err("Failed to allocate aen data SKB of size: %x\n",
			     total_size);
		return -ENOMEM;
	}

	/* add the genetlink message header */
	msg_header = genlmsg_put(skb, 0, 0,
				 &pmcraid_event_family, 0,
				 PMCRAID_AEN_CMD_EVENT);
	if (!msg_header) {
		pmcraid_err("failed to copy command details\n");
		nlmsg_free(skb);
		return -ENOMEM;
	}

	result = nla_put(skb, PMCRAID_AEN_ATTR_EVENT, data_size, aen_msg);

	if (result) {
		pmcraid_err("failed to copy AEN attribute data \n");
		nlmsg_free(skb);
		return -EINVAL;
	}

	/* send genetlink multicast message to notify appplications */
	result = genlmsg_end(skb, msg_header);

	if (result < 0) {
		pmcraid_err("genlmsg_end failed\n");
		nlmsg_free(skb);
		return result;
	}

	result =
		genlmsg_multicast(skb, 0, pmcraid_event_family.id, GFP_ATOMIC);

	/* If there are no listeners, genlmsg_multicast may return non-zero
	 * value.
	 */
	if (result)
		pmcraid_info("failed to send %s event message %x!\n",
			type == PMCRAID_HCAM_CODE_LOG_DATA ? "LDN" : "CCN",
			result);
	return result;
}


static void pmcraid_handle_config_change(struct pmcraid_instance *pinstance)
{
	struct pmcraid_config_table_entry *cfg_entry;
	struct pmcraid_hcam_ccn *ccn_hcam;
	struct pmcraid_cmd *cmd;
	struct pmcraid_cmd *cfgcmd;
	struct pmcraid_resource_entry *res = NULL;
	unsigned long lock_flags;
	unsigned long host_lock_flags;
	u32 new_entry = 1;
	u32 hidden_entry = 0;
	int rc;

	ccn_hcam = (struct pmcraid_hcam_ccn *)pinstance->ccn.hcam;
	cfg_entry = &ccn_hcam->cfg_entry;

	pmcraid_info
		("CCN(%x): %x type: %x lost: %x flags: %x res: %x:%x:%x:%x\n",
		 pinstance->ccn.hcam->ilid,
		 pinstance->ccn.hcam->op_code,
		 pinstance->ccn.hcam->notification_type,
		 pinstance->ccn.hcam->notification_lost,
		 pinstance->ccn.hcam->flags,
		 pinstance->host->unique_id,
		 RES_IS_VSET(*cfg_entry) ? PMCRAID_VSET_BUS_ID :
		 (RES_IS_GSCSI(*cfg_entry) ? PMCRAID_PHYS_BUS_ID :
			RES_BUS(cfg_entry->resource_address)),
		 RES_IS_VSET(*cfg_entry) ? cfg_entry->unique_flags1 :
			RES_TARGET(cfg_entry->resource_address),
		 RES_LUN(cfg_entry->resource_address));


	/* If this HCAM indicates a lost notification, read the config table */
	if (pinstance->ccn.hcam->notification_lost) {
		cfgcmd = pmcraid_get_free_cmd(pinstance);
		if (cfgcmd) {
			pmcraid_info("lost CCN, reading config table\b");
			pinstance->reinit_cfg_table = 1;
			pmcraid_querycfg(cfgcmd);
		} else {
			pmcraid_err("lost CCN, no free cmd for querycfg\n");
		}
		goto out_notify_apps;
	}

	/* If this resource is not going to be added to mid-layer, just notify
	 * applications and return. If this notification is about hiding a VSET
	 * resource, check if it was exposed already.
	 */
	if (pinstance->ccn.hcam->notification_type ==
	    NOTIFICATION_TYPE_ENTRY_CHANGED &&
	    cfg_entry->resource_type == RES_TYPE_VSET &&
	    cfg_entry->unique_flags1 & 0x80) {
		hidden_entry = 1;
	} else if (!pmcraid_expose_resource(cfg_entry))
		goto out_notify_apps;

	spin_lock_irqsave(&pinstance->resource_lock, lock_flags);
	list_for_each_entry(res, &pinstance->used_res_q, queue) {
		rc = memcmp(&res->cfg_entry.resource_address,
			    &cfg_entry->resource_address,
			    sizeof(cfg_entry->resource_address));
		if (!rc) {
			new_entry = 0;
			break;
		}
	}

	if (new_entry) {

		if (hidden_entry) {
			spin_unlock_irqrestore(&pinstance->resource_lock,
						lock_flags);
			goto out_notify_apps;
		}

		/* If there are more number of resources than what driver can
		 * manage, do not notify the applications about the CCN. Just
		 * ignore this notifications and re-register the same HCAM
		 */
		if (list_empty(&pinstance->free_res_q)) {
			spin_unlock_irqrestore(&pinstance->resource_lock,
						lock_flags);
			pmcraid_err("too many resources attached\n");
			spin_lock_irqsave(pinstance->host->host_lock,
					  host_lock_flags);
			pmcraid_send_hcam(pinstance,
					  PMCRAID_HCAM_CODE_CONFIG_CHANGE);
			spin_unlock_irqrestore(pinstance->host->host_lock,
					       host_lock_flags);
			return;
		}

		res = list_entry(pinstance->free_res_q.next,
				 struct pmcraid_resource_entry, queue);

		list_del(&res->queue);
		res->scsi_dev = NULL;
		res->reset_progress = 0;
		list_add_tail(&res->queue, &pinstance->used_res_q);
	}

	memcpy(&res->cfg_entry, cfg_entry,
		sizeof(struct pmcraid_config_table_entry));

	if (pinstance->ccn.hcam->notification_type ==
	    NOTIFICATION_TYPE_ENTRY_DELETED || hidden_entry) {
		if (res->scsi_dev) {
			res->cfg_entry.unique_flags1 &= 0x7F;
			res->change_detected = RES_CHANGE_DEL;
			res->cfg_entry.resource_handle =
				PMCRAID_INVALID_RES_HANDLE;
			schedule_work(&pinstance->worker_q);
		} else {
			/* This may be one of the non-exposed resources */
			list_move_tail(&res->queue, &pinstance->free_res_q);
		}
	} else if (!res->scsi_dev) {
		res->change_detected = RES_CHANGE_ADD;
		schedule_work(&pinstance->worker_q);
	}
	spin_unlock_irqrestore(&pinstance->resource_lock, lock_flags);

out_notify_apps:

	/* Notify configuration changes to registered applications.*/
	if (!pmcraid_disable_aen)
		pmcraid_notify_aen(pinstance, PMCRAID_HCAM_CODE_CONFIG_CHANGE);

	cmd = pmcraid_init_hcam(pinstance, PMCRAID_HCAM_CODE_CONFIG_CHANGE);
	if (cmd)
		pmcraid_send_hcam_cmd(cmd);
}

static struct pmcraid_ioasc_error *pmcraid_get_error_info(u32 ioasc)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(pmcraid_ioasc_error_table); i++) {
		if (pmcraid_ioasc_error_table[i].ioasc_code == ioasc)
			return &pmcraid_ioasc_error_table[i];
	}
	return NULL;
}

void pmcraid_ioasc_logger(u32 ioasc, struct pmcraid_cmd *cmd)
{
	struct pmcraid_ioasc_error *error_info = pmcraid_get_error_info(ioasc);

	if (error_info == NULL ||
		cmd->drv_inst->current_log_level < error_info->log_level)
		return;

	/* log the error string */
	pmcraid_err("cmd [%d] for resource %x failed with %x(%s)\n",
		cmd->ioa_cb->ioarcb.cdb[0],
		cmd->ioa_cb->ioarcb.resource_handle,
		le32_to_cpu(ioasc), error_info->error_string);
}

static void pmcraid_handle_error_log(struct pmcraid_instance *pinstance)
{
	struct pmcraid_hcam_ldn *hcam_ldn;
	u32 ioasc;

	hcam_ldn = (struct pmcraid_hcam_ldn *)pinstance->ldn.hcam;

	pmcraid_info
		("LDN(%x): %x type: %x lost: %x flags: %x overlay id: %x\n",
		 pinstance->ldn.hcam->ilid,
		 pinstance->ldn.hcam->op_code,
		 pinstance->ldn.hcam->notification_type,
		 pinstance->ldn.hcam->notification_lost,
		 pinstance->ldn.hcam->flags,
		 pinstance->ldn.hcam->overlay_id);

	/* log only the errors, no need to log informational log entries */
	if (pinstance->ldn.hcam->notification_type !=
	    NOTIFICATION_TYPE_ERROR_LOG)
		return;

	if (pinstance->ldn.hcam->notification_lost ==
	    HOSTRCB_NOTIFICATIONS_LOST)
		dev_info(&pinstance->pdev->dev, "Error notifications lost\n");

	ioasc = le32_to_cpu(hcam_ldn->error_log.fd_ioasc);

	if (ioasc == PMCRAID_IOASC_UA_BUS_WAS_RESET ||
		ioasc == PMCRAID_IOASC_UA_BUS_WAS_RESET_BY_OTHER) {
		dev_info(&pinstance->pdev->dev,
			"UnitAttention due to IOA Bus Reset\n");
		scsi_report_bus_reset(
			pinstance->host,
			RES_BUS(hcam_ldn->error_log.fd_ra));
	}

	return;
}

static void pmcraid_process_ccn(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	u32 ioasc = le32_to_cpu(cmd->ioa_cb->ioasa.ioasc);
	unsigned long lock_flags;

	pinstance->ccn.cmd = NULL;
	pmcraid_return_cmd(cmd);

	/* If driver initiated IOA reset happened while this hcam was pending
	 * with IOA, or IOA bringdown sequence is in progress, no need to
	 * re-register the hcam
	 */
	if (ioasc == PMCRAID_IOASC_IOA_WAS_RESET ||
	    atomic_read(&pinstance->ccn.ignore) == 1) {
		return;
	} else if (ioasc) {
		dev_info(&pinstance->pdev->dev,
			"Host RCB (CCN) failed with IOASC: 0x%08X\n", ioasc);
		spin_lock_irqsave(pinstance->host->host_lock, lock_flags);
		pmcraid_send_hcam(pinstance, PMCRAID_HCAM_CODE_CONFIG_CHANGE);
		spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);
	} else {
		pmcraid_handle_config_change(pinstance);
	}
}

static void pmcraid_initiate_reset(struct pmcraid_instance *);

static void pmcraid_process_ldn(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	struct pmcraid_hcam_ldn *ldn_hcam =
			(struct pmcraid_hcam_ldn *)pinstance->ldn.hcam;
	u32 ioasc = le32_to_cpu(cmd->ioa_cb->ioasa.ioasc);
	u32 fd_ioasc = le32_to_cpu(ldn_hcam->error_log.fd_ioasc);
	unsigned long lock_flags;

	/* return the command block back to freepool */
	pinstance->ldn.cmd = NULL;
	pmcraid_return_cmd(cmd);

	/* If driver initiated IOA reset happened while this hcam was pending
	 * with IOA, no need to re-register the hcam as reset engine will do it
	 * once reset sequence is complete
	 */
	if (ioasc == PMCRAID_IOASC_IOA_WAS_RESET ||
	    atomic_read(&pinstance->ccn.ignore) == 1) {
		return;
	} else if (!ioasc) {
		pmcraid_handle_error_log(pinstance);
		if (fd_ioasc == PMCRAID_IOASC_NR_IOA_RESET_REQUIRED) {
			spin_lock_irqsave(pinstance->host->host_lock,
					  lock_flags);
			pmcraid_initiate_reset(pinstance);
			spin_unlock_irqrestore(pinstance->host->host_lock,
					       lock_flags);
			return;
		}
	} else {
		dev_info(&pinstance->pdev->dev,
			"Host RCB(LDN) failed with IOASC: 0x%08X\n", ioasc);
	}
	/* send netlink message for HCAM notification if enabled */
	if (!pmcraid_disable_aen)
		pmcraid_notify_aen(pinstance, PMCRAID_HCAM_CODE_LOG_DATA);

	cmd = pmcraid_init_hcam(pinstance, PMCRAID_HCAM_CODE_LOG_DATA);
	if (cmd)
		pmcraid_send_hcam_cmd(cmd);
}

static void pmcraid_register_hcams(struct pmcraid_instance *pinstance)
{
	pmcraid_send_hcam(pinstance, PMCRAID_HCAM_CODE_CONFIG_CHANGE);
	pmcraid_send_hcam(pinstance, PMCRAID_HCAM_CODE_LOG_DATA);
}

static void pmcraid_unregister_hcams(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;

	/* During IOA bringdown, HCAM gets fired and tasklet proceeds with
	 * handling hcam response though it is not necessary. In order to
	 * prevent this, set 'ignore', so that bring-down sequence doesn't
	 * re-send any more hcams
	 */
	atomic_set(&pinstance->ccn.ignore, 1);
	atomic_set(&pinstance->ldn.ignore, 1);

	/* If adapter reset was forced as part of runtime reset sequence,
	 * start the reset sequence.
	 */
	if (pinstance->force_ioa_reset && !pinstance->ioa_bringdown) {
		pinstance->force_ioa_reset = 0;
		pinstance->ioa_state = IOA_STATE_IN_RESET_ALERT;
		pmcraid_reset_alert(cmd);
		return;
	}

	/* Driver tries to cancel HCAMs by sending ABORT TASK for each HCAM
	 * one after the other. So CCN cancellation will be triggered by
	 * pmcraid_cancel_ldn itself.
	 */
	pmcraid_cancel_ldn(cmd);
}

static void pmcraid_reinit_buffers(struct pmcraid_instance *);

static int pmcraid_reset_enable_ioa(struct pmcraid_instance *pinstance)
{
	u32 intrs;

	pmcraid_reinit_buffers(pinstance);
	intrs = pmcraid_read_interrupts(pinstance);

	pmcraid_enable_interrupts(pinstance, PMCRAID_PCI_INTERRUPTS);

	if (intrs & INTRS_TRANSITION_TO_OPERATIONAL) {
		iowrite32(INTRS_TRANSITION_TO_OPERATIONAL,
			pinstance->int_regs.ioa_host_interrupt_mask_reg);
		iowrite32(INTRS_TRANSITION_TO_OPERATIONAL,
			pinstance->int_regs.ioa_host_interrupt_clr_reg);
		return 1;
	} else {
		return 0;
	}
}

static void pmcraid_soft_reset(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	u32 int_reg;
	u32 doorbell;

	/* There will be an interrupt when Transition to Operational bit is
	 * set so tasklet would execute next reset task. The timeout handler
	 * would re-initiate a reset
	 */
	cmd->cmd_done = pmcraid_ioa_reset;
	cmd->timer.data = (unsigned long)cmd;
	cmd->timer.expires = jiffies +
			     msecs_to_jiffies(PMCRAID_TRANSOP_TIMEOUT);
	cmd->timer.function = (void (*)(unsigned long))pmcraid_timeout_handler;

	if (!timer_pending(&cmd->timer))
		add_timer(&cmd->timer);

	/* Enable destructive diagnostics on IOA if it is not yet in
	 * operational state
	 */
	doorbell = DOORBELL_RUNTIME_RESET |
		   DOORBELL_ENABLE_DESTRUCTIVE_DIAGS;

	iowrite32(doorbell, pinstance->int_regs.host_ioa_interrupt_reg);
	int_reg = ioread32(pinstance->int_regs.ioa_host_interrupt_reg);
	pmcraid_info("Waiting for IOA to become operational %x:%x\n",
		     ioread32(pinstance->int_regs.host_ioa_interrupt_reg),
		     int_reg);
}

static void pmcraid_get_dump(struct pmcraid_instance *pinstance)
{
	pmcraid_info("%s is not yet implemented\n", __func__);
}

static void pmcraid_fail_outstanding_cmds(struct pmcraid_instance *pinstance)
{
	struct pmcraid_cmd *cmd, *temp;
	unsigned long lock_flags;

	/* pending command list is protected by pending_pool_lock. Its
	 * traversal must be done as within this lock
	 */
	spin_lock_irqsave(&pinstance->pending_pool_lock, lock_flags);
	list_for_each_entry_safe(cmd, temp, &pinstance->pending_cmd_pool,
				 free_list) {
		list_del(&cmd->free_list);
		spin_unlock_irqrestore(&pinstance->pending_pool_lock,
					lock_flags);
		cmd->ioa_cb->ioasa.ioasc =
			cpu_to_le32(PMCRAID_IOASC_IOA_WAS_RESET);
		cmd->ioa_cb->ioasa.ilid =
			cpu_to_be32(PMCRAID_DRIVER_ILID);

		/* In case the command timer is still running */
		del_timer(&cmd->timer);

		/* If this is an IO command, complete it by invoking scsi_done
		 * function. If this is one of the internal commands other
		 * than pmcraid_ioa_reset and HCAM commands invoke cmd_done to
		 * complete it
		 */
		if (cmd->scsi_cmd) {

			struct scsi_cmnd *scsi_cmd = cmd->scsi_cmd;
			__le32 resp = cmd->ioa_cb->ioarcb.response_handle;

			scsi_cmd->result |= DID_ERROR << 16;

			scsi_dma_unmap(scsi_cmd);
			pmcraid_return_cmd(cmd);

			pmcraid_info("failing(%d) CDB[0] = %x result: %x\n",
				     le32_to_cpu(resp) >> 2,
				     cmd->ioa_cb->ioarcb.cdb[0],
				     scsi_cmd->result);
			scsi_cmd->scsi_done(scsi_cmd);
		} else if (cmd->cmd_done == pmcraid_internal_done ||
			   cmd->cmd_done == pmcraid_erp_done) {
			cmd->cmd_done(cmd);
		} else if (cmd->cmd_done != pmcraid_ioa_reset) {
			pmcraid_return_cmd(cmd);
		}

		atomic_dec(&pinstance->outstanding_cmds);
		spin_lock_irqsave(&pinstance->pending_pool_lock, lock_flags);
	}

	spin_unlock_irqrestore(&pinstance->pending_pool_lock, lock_flags);
}

static void pmcraid_ioa_reset(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	u8 reset_complete = 0;

	pinstance->ioa_reset_in_progress = 1;

	if (pinstance->reset_cmd != cmd) {
		pmcraid_err("reset is called with different command block\n");
		pinstance->reset_cmd = cmd;
	}

	pmcraid_info("reset_engine: state = %d, command = %p\n",
		      pinstance->ioa_state, cmd);

	switch (pinstance->ioa_state) {

	case IOA_STATE_DEAD:
		/* If IOA is offline, whatever may be the reset reason, just
		 * return. callers might be waiting on the reset wait_q, wake
		 * up them
		 */
		pmcraid_err("IOA is offline no reset is possible\n");
		reset_complete = 1;
		break;

	case IOA_STATE_IN_BRINGDOWN:
		/* we enter here, once ioa shutdown command is processed by IOA
		 * Alert IOA for a possible reset. If reset alert fails, IOA
		 * goes through hard-reset
		 */
		pmcraid_disable_interrupts(pinstance, ~0);
		pinstance->ioa_state = IOA_STATE_IN_RESET_ALERT;
		pmcraid_reset_alert(cmd);
		break;

	case IOA_STATE_UNKNOWN:
		/* We may be called during probe or resume. Some pre-processing
		 * is required for prior to reset
		 */
		scsi_block_requests(pinstance->host);

		/* If asked to reset while IOA was processing responses or
		 * there are any error responses then IOA may require
		 * hard-reset.
		 */
		if (pinstance->ioa_hard_reset == 0) {
			if (ioread32(pinstance->ioa_status) &
			    INTRS_TRANSITION_TO_OPERATIONAL) {
				pmcraid_info("sticky bit set, bring-up\n");
				pinstance->ioa_state = IOA_STATE_IN_BRINGUP;
				pmcraid_reinit_cmdblk(cmd);
				pmcraid_identify_hrrq(cmd);
			} else {
				pinstance->ioa_state = IOA_STATE_IN_SOFT_RESET;
				pmcraid_soft_reset(cmd);
			}
		} else {
			/* Alert IOA of a possible reset and wait for critical
			 * operation in progress bit to reset
			 */
			pinstance->ioa_state = IOA_STATE_IN_RESET_ALERT;
			pmcraid_reset_alert(cmd);
		}
		break;

	case IOA_STATE_IN_RESET_ALERT:
		/* If critical operation in progress bit is reset or wait gets
		 * timed out, reset proceeds with starting BIST on the IOA.
		 * pmcraid_ioa_hard_reset keeps a count of reset attempts. If
		 * they are 3 or more, reset engine marks IOA dead and returns
		 */
		pinstance->ioa_state = IOA_STATE_IN_HARD_RESET;
		pmcraid_start_bist(cmd);
		break;

	case IOA_STATE_IN_HARD_RESET:
		pinstance->ioa_reset_attempts++;

		/* retry reset if we haven't reached maximum allowed limit */
		if (pinstance->ioa_reset_attempts > PMCRAID_RESET_ATTEMPTS) {
			pinstance->ioa_reset_attempts = 0;
			pmcraid_err("IOA didn't respond marking it as dead\n");
			pinstance->ioa_state = IOA_STATE_DEAD;
			reset_complete = 1;
			break;
		}

		/* Once either bist or pci reset is done, restore PCI config
		 * space. If this fails, proceed with hard reset again
		 */

		if (pci_restore_state(pinstance->pdev)) {
			pmcraid_info("config-space error resetting again\n");
			pinstance->ioa_state = IOA_STATE_IN_RESET_ALERT;
			pmcraid_reset_alert(cmd);
			break;
		}

		/* fail all pending commands */
		pmcraid_fail_outstanding_cmds(pinstance);

		/* check if unit check is active, if so extract dump */
		if (pinstance->ioa_unit_check) {
			pmcraid_info("unit check is active\n");
			pinstance->ioa_unit_check = 0;
			pmcraid_get_dump(pinstance);
			pinstance->ioa_reset_attempts--;
			pinstance->ioa_state = IOA_STATE_IN_RESET_ALERT;
			pmcraid_reset_alert(cmd);
			break;
		}

		/* if the reset reason is to bring-down the ioa, we might be
		 * done with the reset restore pci_config_space and complete
		 * the reset
		 */
		if (pinstance->ioa_bringdown) {
			pmcraid_info("bringing down the adapter\n");
			pinstance->ioa_shutdown_type = SHUTDOWN_NONE;
			pinstance->ioa_bringdown = 0;
			pinstance->ioa_state = IOA_STATE_UNKNOWN;
			reset_complete = 1;
		} else {
			/* bring-up IOA, so proceed with soft reset
			 * Reinitialize hrrq_buffers and their indices also
			 * enable interrupts after a pci_restore_state
			 */
			if (pmcraid_reset_enable_ioa(pinstance)) {
				pinstance->ioa_state = IOA_STATE_IN_BRINGUP;
				pmcraid_info("bringing up the adapter\n");
				pmcraid_reinit_cmdblk(cmd);
				pmcraid_identify_hrrq(cmd);
			} else {
				pinstance->ioa_state = IOA_STATE_IN_SOFT_RESET;
				pmcraid_soft_reset(cmd);
			}
		}
		break;

	case IOA_STATE_IN_SOFT_RESET:
		/* TRANSITION TO OPERATIONAL is on so start initialization
		 * sequence
		 */
		pmcraid_info("In softreset proceeding with bring-up\n");
		pinstance->ioa_state = IOA_STATE_IN_BRINGUP;

		/* Initialization commands start with HRRQ identification. From
		 * now on tasklet completes most of the commands as IOA is up
		 * and intrs are enabled
		 */
		pmcraid_identify_hrrq(cmd);
		break;

	case IOA_STATE_IN_BRINGUP:
		/* we are done with bringing up of IOA, change the ioa_state to
		 * operational and wake up any waiters
		 */
		pinstance->ioa_state = IOA_STATE_OPERATIONAL;
		reset_complete = 1;
		break;

	case IOA_STATE_OPERATIONAL:
	default:
		/* When IOA is operational and a reset is requested, check for
		 * the reset reason. If reset is to bring down IOA, unregister
		 * HCAMs and initiate shutdown; if adapter reset is forced then
		 * restart reset sequence again
		 */
		if (pinstance->ioa_shutdown_type == SHUTDOWN_NONE &&
		    pinstance->force_ioa_reset == 0) {
			reset_complete = 1;
		} else {
			if (pinstance->ioa_shutdown_type != SHUTDOWN_NONE)
				pinstance->ioa_state = IOA_STATE_IN_BRINGDOWN;
			pmcraid_reinit_cmdblk(cmd);
			pmcraid_unregister_hcams(cmd);
		}
		break;
	}

	/* reset will be completed if ioa_state is either DEAD or UNKNOWN or
	 * OPERATIONAL. Reset all control variables used during reset, wake up
	 * any waiting threads and let the SCSI mid-layer send commands. Note
	 * that host_lock must be held before invoking scsi_report_bus_reset.
	 */
	if (reset_complete) {
		pinstance->ioa_reset_in_progress = 0;
		pinstance->ioa_reset_attempts = 0;
		pinstance->reset_cmd = NULL;
		pinstance->ioa_shutdown_type = SHUTDOWN_NONE;
		pinstance->ioa_bringdown = 0;
		pmcraid_return_cmd(cmd);

		/* If target state is to bring up the adapter, proceed with
		 * hcam registration and resource exposure to mid-layer.
		 */
		if (pinstance->ioa_state == IOA_STATE_OPERATIONAL)
			pmcraid_register_hcams(pinstance);

		wake_up_all(&pinstance->reset_wait_q);
	}

	return;
}

static void pmcraid_initiate_reset(struct pmcraid_instance *pinstance)
{
	struct pmcraid_cmd *cmd;

	/* If the reset is already in progress, just return, otherwise start
	 * reset sequence and return
	 */
	if (!pinstance->ioa_reset_in_progress) {
		scsi_block_requests(pinstance->host);
		cmd = pmcraid_get_free_cmd(pinstance);

		if (cmd == NULL) {
			pmcraid_err("no cmnd blocks for initiate_reset\n");
			return;
		}

		pinstance->ioa_shutdown_type = SHUTDOWN_NONE;
		pinstance->reset_cmd = cmd;
		pinstance->force_ioa_reset = 1;
		pmcraid_ioa_reset(cmd);
	}
}

static int pmcraid_reset_reload(
	struct pmcraid_instance *pinstance,
	u8 shutdown_type,
	u8 target_state
)
{
	struct pmcraid_cmd *reset_cmd = NULL;
	unsigned long lock_flags;
	int reset = 1;

	spin_lock_irqsave(pinstance->host->host_lock, lock_flags);

	if (pinstance->ioa_reset_in_progress) {
		pmcraid_info("reset_reload: reset is already in progress\n");

		spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);

		wait_event(pinstance->reset_wait_q,
			   !pinstance->ioa_reset_in_progress);

		spin_lock_irqsave(pinstance->host->host_lock, lock_flags);

		if (pinstance->ioa_state == IOA_STATE_DEAD) {
			spin_unlock_irqrestore(pinstance->host->host_lock,
					       lock_flags);
			pmcraid_info("reset_reload: IOA is dead\n");
			return reset;
		} else if (pinstance->ioa_state == target_state) {
			reset = 0;
		}
	}

	if (reset) {
		pmcraid_info("reset_reload: proceeding with reset\n");
		scsi_block_requests(pinstance->host);
		reset_cmd = pmcraid_get_free_cmd(pinstance);

		if (reset_cmd == NULL) {
			pmcraid_err("no free cmnd for reset_reload\n");
			spin_unlock_irqrestore(pinstance->host->host_lock,
					       lock_flags);
			return reset;
		}

		if (shutdown_type == SHUTDOWN_NORMAL)
			pinstance->ioa_bringdown = 1;

		pinstance->ioa_shutdown_type = shutdown_type;
		pinstance->reset_cmd = reset_cmd;
		pinstance->force_ioa_reset = reset;
		pmcraid_info("reset_reload: initiating reset\n");
		pmcraid_ioa_reset(reset_cmd);
		spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);
		pmcraid_info("reset_reload: waiting for reset to complete\n");
		wait_event(pinstance->reset_wait_q,
			   !pinstance->ioa_reset_in_progress);

		pmcraid_info("reset_reload: reset is complete !! \n");
		scsi_unblock_requests(pinstance->host);
		if (pinstance->ioa_state == target_state)
			reset = 0;
	}

	return reset;
}

static int pmcraid_reset_bringdown(struct pmcraid_instance *pinstance)
{
	return pmcraid_reset_reload(pinstance,
				    SHUTDOWN_NORMAL,
				    IOA_STATE_UNKNOWN);
}

static int pmcraid_reset_bringup(struct pmcraid_instance *pinstance)
{
	return pmcraid_reset_reload(pinstance,
				    SHUTDOWN_NONE,
				    IOA_STATE_OPERATIONAL);
}

static void pmcraid_request_sense(struct pmcraid_cmd *cmd)
{
	struct pmcraid_ioarcb *ioarcb = &cmd->ioa_cb->ioarcb;
	struct pmcraid_ioadl_desc *ioadl = ioarcb->add_data.u.ioadl;

	/* allocate DMAable memory for sense buffers */
	cmd->sense_buffer = pci_alloc_consistent(cmd->drv_inst->pdev,
						 SCSI_SENSE_BUFFERSIZE,
						 &cmd->sense_buffer_dma);

	if (cmd->sense_buffer == NULL) {
		pmcraid_err
			("couldn't allocate sense buffer for request sense\n");
		pmcraid_erp_done(cmd);
		return;
	}

	/* re-use the command block */
	memset(&cmd->ioa_cb->ioasa, 0, sizeof(struct pmcraid_ioasa));
	memset(ioarcb->cdb, 0, PMCRAID_MAX_CDB_LEN);
	ioarcb->request_flags0 = (SYNC_COMPLETE |
				  NO_LINK_DESCS |
				  INHIBIT_UL_CHECK);
	ioarcb->request_type = REQ_TYPE_SCSI;
	ioarcb->cdb[0] = REQUEST_SENSE;
	ioarcb->cdb[4] = SCSI_SENSE_BUFFERSIZE;

	ioarcb->ioadl_bus_addr = cpu_to_le64((cmd->ioa_cb_bus_addr) +
					offsetof(struct pmcraid_ioarcb,
						add_data.u.ioadl[0]));
	ioarcb->ioadl_length = cpu_to_le32(sizeof(struct pmcraid_ioadl_desc));

	ioarcb->data_transfer_length = cpu_to_le32(SCSI_SENSE_BUFFERSIZE);

	ioadl->address = cpu_to_le64(cmd->sense_buffer_dma);
	ioadl->data_len = cpu_to_le32(SCSI_SENSE_BUFFERSIZE);
	ioadl->flags = IOADL_FLAGS_LAST_DESC;

	/* request sense might be called as part of error response processing
	 * which runs in tasklets context. It is possible that mid-layer might
	 * schedule queuecommand during this time, hence, writting to IOARRIN
	 * must be protect by host_lock
	 */
	pmcraid_send_cmd(cmd, pmcraid_erp_done,
			 PMCRAID_REQUEST_SENSE_TIMEOUT,
			 pmcraid_timeout_handler);
}

static void pmcraid_cancel_all(struct pmcraid_cmd *cmd, u32 sense)
{
	struct scsi_cmnd *scsi_cmd = cmd->scsi_cmd;
	struct pmcraid_ioarcb *ioarcb = &cmd->ioa_cb->ioarcb;
	struct pmcraid_resource_entry *res = scsi_cmd->device->hostdata;
	void (*cmd_done) (struct pmcraid_cmd *) = sense ? pmcraid_erp_done
							: pmcraid_request_sense;

	memset(ioarcb->cdb, 0, PMCRAID_MAX_CDB_LEN);
	ioarcb->request_flags0 = SYNC_OVERRIDE;
	ioarcb->request_type = REQ_TYPE_IOACMD;
	ioarcb->cdb[0] = PMCRAID_CANCEL_ALL_REQUESTS;

	if (RES_IS_GSCSI(res->cfg_entry))
		ioarcb->cdb[1] = PMCRAID_SYNC_COMPLETE_AFTER_CANCEL;

	ioarcb->ioadl_bus_addr = 0;
	ioarcb->ioadl_length = 0;
	ioarcb->data_transfer_length = 0;
	ioarcb->ioarcb_bus_addr &= (~0x1FULL);

	/* writing to IOARRIN must be protected by host_lock, as mid-layer
	 * schedule queuecommand while we are doing this
	 */
	pmcraid_send_cmd(cmd, cmd_done,
			 PMCRAID_REQUEST_SENSE_TIMEOUT,
			 pmcraid_timeout_handler);
}

static void pmcraid_frame_auto_sense(struct pmcraid_cmd *cmd)
{
	u8 *sense_buf = cmd->scsi_cmd->sense_buffer;
	struct pmcraid_resource_entry *res = cmd->scsi_cmd->device->hostdata;
	struct pmcraid_ioasa *ioasa = &cmd->ioa_cb->ioasa;
	u32 ioasc = le32_to_cpu(ioasa->ioasc);
	u32 failing_lba = 0;

	memset(sense_buf, 0, SCSI_SENSE_BUFFERSIZE);
	cmd->scsi_cmd->result = SAM_STAT_CHECK_CONDITION;

	if (RES_IS_VSET(res->cfg_entry) &&
	    ioasc == PMCRAID_IOASC_ME_READ_ERROR_NO_REALLOC &&
	    ioasa->u.vset.failing_lba_hi != 0) {

		sense_buf[0] = 0x72;
		sense_buf[1] = PMCRAID_IOASC_SENSE_KEY(ioasc);
		sense_buf[2] = PMCRAID_IOASC_SENSE_CODE(ioasc);
		sense_buf[3] = PMCRAID_IOASC_SENSE_QUAL(ioasc);

		sense_buf[7] = 12;
		sense_buf[8] = 0;
		sense_buf[9] = 0x0A;
		sense_buf[10] = 0x80;

		failing_lba = le32_to_cpu(ioasa->u.vset.failing_lba_hi);

		sense_buf[12] = (failing_lba & 0xff000000) >> 24;
		sense_buf[13] = (failing_lba & 0x00ff0000) >> 16;
		sense_buf[14] = (failing_lba & 0x0000ff00) >> 8;
		sense_buf[15] = failing_lba & 0x000000ff;

		failing_lba = le32_to_cpu(ioasa->u.vset.failing_lba_lo);

		sense_buf[16] = (failing_lba & 0xff000000) >> 24;
		sense_buf[17] = (failing_lba & 0x00ff0000) >> 16;
		sense_buf[18] = (failing_lba & 0x0000ff00) >> 8;
		sense_buf[19] = failing_lba & 0x000000ff;
	} else {
		sense_buf[0] = 0x70;
		sense_buf[2] = PMCRAID_IOASC_SENSE_KEY(ioasc);
		sense_buf[12] = PMCRAID_IOASC_SENSE_CODE(ioasc);
		sense_buf[13] = PMCRAID_IOASC_SENSE_QUAL(ioasc);

		if (ioasc == PMCRAID_IOASC_ME_READ_ERROR_NO_REALLOC) {
			if (RES_IS_VSET(res->cfg_entry))
				failing_lba =
					le32_to_cpu(ioasa->u.
						 vset.failing_lba_lo);
			sense_buf[0] |= 0x80;
			sense_buf[3] = (failing_lba >> 24) & 0xff;
			sense_buf[4] = (failing_lba >> 16) & 0xff;
			sense_buf[5] = (failing_lba >> 8) & 0xff;
			sense_buf[6] = failing_lba & 0xff;
		}

		sense_buf[7] = 6; /* additional length */
	}
}

static int pmcraid_error_handler(struct pmcraid_cmd *cmd)
{
	struct scsi_cmnd *scsi_cmd = cmd->scsi_cmd;
	struct pmcraid_resource_entry *res = scsi_cmd->device->hostdata;
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	struct pmcraid_ioasa *ioasa = &cmd->ioa_cb->ioasa;
	u32 ioasc = le32_to_cpu(ioasa->ioasc);
	u32 masked_ioasc = ioasc & PMCRAID_IOASC_SENSE_MASK;
	u32 sense_copied = 0;

	if (!res) {
		pmcraid_info("resource pointer is NULL\n");
		return 0;
	}

	/* If this was a SCSI read/write command keep count of errors */
	if (SCSI_CMD_TYPE(scsi_cmd->cmnd[0]) == SCSI_READ_CMD)
		atomic_inc(&res->read_failures);
	else if (SCSI_CMD_TYPE(scsi_cmd->cmnd[0]) == SCSI_WRITE_CMD)
		atomic_inc(&res->write_failures);

	if (!RES_IS_GSCSI(res->cfg_entry) &&
		masked_ioasc != PMCRAID_IOASC_HW_DEVICE_BUS_STATUS_ERROR) {
		pmcraid_frame_auto_sense(cmd);
	}

	/* Log IOASC/IOASA information based on user settings */
	pmcraid_ioasc_logger(ioasc, cmd);

	switch (masked_ioasc) {

	case PMCRAID_IOASC_AC_TERMINATED_BY_HOST:
		scsi_cmd->result |= (DID_ABORT << 16);
		break;

	case PMCRAID_IOASC_IR_INVALID_RESOURCE_HANDLE:
	case PMCRAID_IOASC_HW_CANNOT_COMMUNICATE:
		scsi_cmd->result |= (DID_NO_CONNECT << 16);
		break;

	case PMCRAID_IOASC_NR_SYNC_REQUIRED:
		res->sync_reqd = 1;
		scsi_cmd->result |= (DID_IMM_RETRY << 16);
		break;

	case PMCRAID_IOASC_ME_READ_ERROR_NO_REALLOC:
		scsi_cmd->result |= (DID_PASSTHROUGH << 16);
		break;

	case PMCRAID_IOASC_UA_BUS_WAS_RESET:
	case PMCRAID_IOASC_UA_BUS_WAS_RESET_BY_OTHER:
		if (!res->reset_progress)
			scsi_report_bus_reset(pinstance->host,
					      scsi_cmd->device->channel);
		scsi_cmd->result |= (DID_ERROR << 16);
		break;

	case PMCRAID_IOASC_HW_DEVICE_BUS_STATUS_ERROR:
		scsi_cmd->result |= PMCRAID_IOASC_SENSE_STATUS(ioasc);
		res->sync_reqd = 1;

		/* if check_condition is not active return with error otherwise
		 * get/frame the sense buffer
		 */
		if (PMCRAID_IOASC_SENSE_STATUS(ioasc) !=
		    SAM_STAT_CHECK_CONDITION &&
		    PMCRAID_IOASC_SENSE_STATUS(ioasc) != SAM_STAT_ACA_ACTIVE)
			return 0;

		/* If we have auto sense data as part of IOASA pass it to
		 * mid-layer
		 */
		if (ioasa->auto_sense_length != 0) {
			short sense_len = ioasa->auto_sense_length;
			int data_size = min_t(u16, le16_to_cpu(sense_len),
					      SCSI_SENSE_BUFFERSIZE);

			memcpy(scsi_cmd->sense_buffer,
			       ioasa->sense_data,
			       data_size);
			sense_copied = 1;
		}

		if (RES_IS_GSCSI(res->cfg_entry))
			pmcraid_cancel_all(cmd, sense_copied);
		else if (sense_copied)
			pmcraid_erp_done(cmd);
		else
			pmcraid_request_sense(cmd);

		return 1;

	case PMCRAID_IOASC_NR_INIT_CMD_REQUIRED:
		break;

	default:
		if (PMCRAID_IOASC_SENSE_KEY(ioasc) > RECOVERED_ERROR)
			scsi_cmd->result |= (DID_ERROR << 16);
		break;
	}
	return 0;
}

static int pmcraid_reset_device(
	struct scsi_cmnd *scsi_cmd,
	unsigned long timeout,
	u8 modifier
)
{
	struct pmcraid_cmd *cmd;
	struct pmcraid_instance *pinstance;
	struct pmcraid_resource_entry *res;
	struct pmcraid_ioarcb *ioarcb;
	unsigned long lock_flags;
	u32 ioasc;

	pinstance =
		(struct pmcraid_instance *)scsi_cmd->device->host->hostdata;
	res = scsi_cmd->device->hostdata;

	if (!res) {
		sdev_printk(KERN_ERR, scsi_cmd->device,
			    "reset_device: NULL resource pointer\n");
		return FAILED;
	}

	/* If adapter is currently going through reset/reload, return failed.
	 * This will force the mid-layer to call _eh_bus/host reset, which
	 * will then go to sleep and wait for the reset to complete
	 */
	spin_lock_irqsave(pinstance->host->host_lock, lock_flags);
	if (pinstance->ioa_reset_in_progress ||
	    pinstance->ioa_state == IOA_STATE_DEAD) {
		spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);
		return FAILED;
	}

	res->reset_progress = 1;
	pmcraid_info("Resetting %s resource with addr %x\n",
		     ((modifier & RESET_DEVICE_LUN) ? "LUN" :
		     ((modifier & RESET_DEVICE_TARGET) ? "TARGET" : "BUS")),
		     le32_to_cpu(res->cfg_entry.resource_address));

	/* get a free cmd block */
	cmd = pmcraid_get_free_cmd(pinstance);

	if (cmd == NULL) {
		spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);
		pmcraid_err("%s: no cmd blocks are available\n", __func__);
		return FAILED;
	}

	ioarcb = &cmd->ioa_cb->ioarcb;
	ioarcb->resource_handle = res->cfg_entry.resource_handle;
	ioarcb->request_type = REQ_TYPE_IOACMD;
	ioarcb->cdb[0] = PMCRAID_RESET_DEVICE;

	/* Initialize reset modifier bits */
	if (modifier)
		modifier = ENABLE_RESET_MODIFIER | modifier;

	ioarcb->cdb[1] = modifier;

	init_completion(&cmd->wait_for_completion);
	cmd->completion_req = 1;

	pmcraid_info("cmd(CDB[0] = %x) for %x with index = %d\n",
		     cmd->ioa_cb->ioarcb.cdb[0],
		     le32_to_cpu(cmd->ioa_cb->ioarcb.resource_handle),
		     le32_to_cpu(cmd->ioa_cb->ioarcb.response_handle) >> 2);

	pmcraid_send_cmd(cmd,
			 pmcraid_internal_done,
			 timeout,
			 pmcraid_timeout_handler);

	spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);

	/* RESET_DEVICE command completes after all pending IOARCBs are
	 * completed. Once this command is completed, pmcraind_internal_done
	 * will wake up the 'completion' queue.
	 */
	wait_for_completion(&cmd->wait_for_completion);

	/* complete the command here itself and return the command block
	 * to free list
	 */
	pmcraid_return_cmd(cmd);
	res->reset_progress = 0;
	ioasc = le32_to_cpu(cmd->ioa_cb->ioasa.ioasc);

	/* set the return value based on the returned ioasc */
	return PMCRAID_IOASC_SENSE_KEY(ioasc) ? FAILED : SUCCESS;
}


static int _pmcraid_io_done(struct pmcraid_cmd *cmd, int reslen, int ioasc)
{
	struct scsi_cmnd *scsi_cmd = cmd->scsi_cmd;
	int rc = 0;

	scsi_set_resid(scsi_cmd, reslen);

	pmcraid_info("response(%d) CDB[0] = %x ioasc:result: %x:%x\n",
		le32_to_cpu(cmd->ioa_cb->ioarcb.response_handle) >> 2,
		cmd->ioa_cb->ioarcb.cdb[0],
		ioasc, scsi_cmd->result);

	if (PMCRAID_IOASC_SENSE_KEY(ioasc) != 0)
		rc = pmcraid_error_handler(cmd);

	if (rc == 0) {
		scsi_dma_unmap(scsi_cmd);
		scsi_cmd->scsi_done(scsi_cmd);
	}

	return rc;
}


static void pmcraid_io_done(struct pmcraid_cmd *cmd)
{
	u32 ioasc = le32_to_cpu(cmd->ioa_cb->ioasa.ioasc);
	u32 reslen = le32_to_cpu(cmd->ioa_cb->ioasa.residual_data_length);

	if (_pmcraid_io_done(cmd, reslen, ioasc) == 0)
		pmcraid_return_cmd(cmd);
}

static struct pmcraid_cmd *pmcraid_abort_cmd(struct pmcraid_cmd *cmd)
{
	struct pmcraid_cmd *cancel_cmd;
	struct pmcraid_instance *pinstance;
	struct pmcraid_resource_entry *res;

	pinstance = (struct pmcraid_instance *)cmd->drv_inst;
	res = cmd->scsi_cmd->device->hostdata;

	cancel_cmd = pmcraid_get_free_cmd(pinstance);

	if (cancel_cmd == NULL) {
		pmcraid_err("%s: no cmd blocks are available\n", __func__);
		return NULL;
	}

	pmcraid_prepare_cancel_cmd(cancel_cmd, cmd);

	pmcraid_info("aborting command CDB[0]= %x with index = %d\n",
		cmd->ioa_cb->ioarcb.cdb[0],
		cmd->ioa_cb->ioarcb.response_handle >> 2);

	init_completion(&cancel_cmd->wait_for_completion);
	cancel_cmd->completion_req = 1;

	pmcraid_info("command (%d) CDB[0] = %x for %x\n",
		le32_to_cpu(cancel_cmd->ioa_cb->ioarcb.response_handle) >> 2,
		cmd->ioa_cb->ioarcb.cdb[0],
		le32_to_cpu(cancel_cmd->ioa_cb->ioarcb.resource_handle));

	pmcraid_send_cmd(cancel_cmd,
			 pmcraid_internal_done,
			 PMCRAID_INTERNAL_TIMEOUT,
			 pmcraid_timeout_handler);
	return cancel_cmd;
}

static int pmcraid_abort_complete(struct pmcraid_cmd *cancel_cmd)
{
	struct pmcraid_resource_entry *res;
	u32 ioasc;

	wait_for_completion(&cancel_cmd->wait_for_completion);
	res = cancel_cmd->u.res;
	cancel_cmd->u.res = NULL;
	ioasc = le32_to_cpu(cancel_cmd->ioa_cb->ioasa.ioasc);

	/* If the abort task is not timed out we will get a Good completion
	 * as sense_key, otherwise we may get one the following responses
	 * due to subsquent bus reset or device reset. In case IOASC is
	 * NR_SYNC_REQUIRED, set sync_reqd flag for the corresponding resource
	 */
	if (ioasc == PMCRAID_IOASC_UA_BUS_WAS_RESET ||
	    ioasc == PMCRAID_IOASC_NR_SYNC_REQUIRED) {
		if (ioasc == PMCRAID_IOASC_NR_SYNC_REQUIRED)
			res->sync_reqd = 1;
		ioasc = 0;
	}

	/* complete the command here itself */
	pmcraid_return_cmd(cancel_cmd);
	return PMCRAID_IOASC_SENSE_KEY(ioasc) ? FAILED : SUCCESS;
}

static int pmcraid_eh_abort_handler(struct scsi_cmnd *scsi_cmd)
{
	struct pmcraid_instance *pinstance;
	struct pmcraid_cmd *cmd;
	struct pmcraid_resource_entry *res;
	unsigned long host_lock_flags;
	unsigned long pending_lock_flags;
	struct pmcraid_cmd *cancel_cmd = NULL;
	int cmd_found = 0;
	int rc = FAILED;

	pinstance =
		(struct pmcraid_instance *)scsi_cmd->device->host->hostdata;

	scmd_printk(KERN_INFO, scsi_cmd,
		    "I/O command timed out, aborting it.\n");

	res = scsi_cmd->device->hostdata;

	if (res == NULL)
		return rc;

	/* If we are currently going through reset/reload, return failed.
	 * This will force the mid-layer to eventually call
	 * pmcraid_eh_host_reset which will then go to sleep and wait for the
	 * reset to complete
	 */
	spin_lock_irqsave(pinstance->host->host_lock, host_lock_flags);

	if (pinstance->ioa_reset_in_progress ||
	    pinstance->ioa_state == IOA_STATE_DEAD) {
		spin_unlock_irqrestore(pinstance->host->host_lock,
				       host_lock_flags);
		return rc;
	}

	/* loop over pending cmd list to find cmd corresponding to this
	 * scsi_cmd. Note that this command might not have been completed
	 * already. locking: all pending commands are protected with
	 * pending_pool_lock.
	 */
	spin_lock_irqsave(&pinstance->pending_pool_lock, pending_lock_flags);
	list_for_each_entry(cmd, &pinstance->pending_cmd_pool, free_list) {

		if (cmd->scsi_cmd == scsi_cmd) {
			cmd_found = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&pinstance->pending_pool_lock,
				pending_lock_flags);

	/* If the command to be aborted was given to IOA and still pending with
	 * it, send ABORT_TASK to abort this and wait for its completion
	 */
	if (cmd_found)
		cancel_cmd = pmcraid_abort_cmd(cmd);

	spin_unlock_irqrestore(pinstance->host->host_lock,
			       host_lock_flags);

	if (cancel_cmd) {
		cancel_cmd->u.res = cmd->scsi_cmd->device->hostdata;
		rc = pmcraid_abort_complete(cancel_cmd);
	}

	return cmd_found ? rc : SUCCESS;
}

static int pmcraid_eh_device_reset_handler(struct scsi_cmnd *scmd)
{
	scmd_printk(KERN_INFO, scmd,
		    "resetting device due to an I/O command timeout.\n");
	return pmcraid_reset_device(scmd,
				    PMCRAID_INTERNAL_TIMEOUT,
				    RESET_DEVICE_LUN);
}

static int pmcraid_eh_bus_reset_handler(struct scsi_cmnd *scmd)
{
	scmd_printk(KERN_INFO, scmd,
		    "Doing bus reset due to an I/O command timeout.\n");
	return pmcraid_reset_device(scmd,
				    PMCRAID_RESET_BUS_TIMEOUT,
				    RESET_DEVICE_BUS);
}

static int pmcraid_eh_target_reset_handler(struct scsi_cmnd *scmd)
{
	scmd_printk(KERN_INFO, scmd,
		    "Doing target reset due to an I/O command timeout.\n");
	return pmcraid_reset_device(scmd,
				    PMCRAID_INTERNAL_TIMEOUT,
				    RESET_DEVICE_TARGET);
}

static int pmcraid_eh_host_reset_handler(struct scsi_cmnd *scmd)
{
	unsigned long interval = 10000; /* 10 seconds interval */
	int waits = jiffies_to_msecs(PMCRAID_RESET_HOST_TIMEOUT) / interval;
	struct pmcraid_instance *pinstance =
		(struct pmcraid_instance *)(scmd->device->host->hostdata);


	/* wait for an additional 150 seconds just in case firmware could come
	 * up and if it could complete all the pending commands excluding the
	 * two HCAM (CCN and LDN).
	 */
	while (waits--) {
		if (atomic_read(&pinstance->outstanding_cmds) <=
		    PMCRAID_MAX_HCAM_CMD)
			return SUCCESS;
		msleep(interval);
	}

	dev_err(&pinstance->pdev->dev,
		"Adapter being reset due to an I/O command timeout.\n");
	return pmcraid_reset_bringup(pinstance) == 0 ? SUCCESS : FAILED;
}

static u8 pmcraid_task_attributes(struct scsi_cmnd *scsi_cmd)
{
	char tag[2];
	u8 rc = 0;

	if (scsi_populate_tag_msg(scsi_cmd, tag)) {
		switch (tag[0]) {
		case MSG_SIMPLE_TAG:
			rc = TASK_TAG_SIMPLE;
			break;
		case MSG_HEAD_TAG:
			rc = TASK_TAG_QUEUE_HEAD;
			break;
		case MSG_ORDERED_TAG:
			rc = TASK_TAG_ORDERED;
			break;
		};
	}

	return rc;
}


struct pmcraid_ioadl_desc *
pmcraid_init_ioadls(struct pmcraid_cmd *cmd, int sgcount)
{
	struct pmcraid_ioadl_desc *ioadl;
	struct pmcraid_ioarcb *ioarcb = &cmd->ioa_cb->ioarcb;
	int ioadl_count = 0;

	if (ioarcb->add_cmd_param_length)
		ioadl_count = DIV_ROUND_UP(ioarcb->add_cmd_param_length, 16);
	ioarcb->ioadl_length =
		sizeof(struct pmcraid_ioadl_desc) * sgcount;

	if ((sgcount + ioadl_count) > (ARRAY_SIZE(ioarcb->add_data.u.ioadl))) {
		/* external ioadls start at offset 0x80 from control_block
		 * structure, re-using 24 out of 27 ioadls part of IOARCB.
		 * It is necessary to indicate to firmware that driver is
		 * using ioadls to be treated as external to IOARCB.
		 */
		ioarcb->ioarcb_bus_addr &= ~(0x1FULL);
		ioarcb->ioadl_bus_addr =
			cpu_to_le64((cmd->ioa_cb_bus_addr) +
				offsetof(struct pmcraid_ioarcb,
					add_data.u.ioadl[3]));
		ioadl = &ioarcb->add_data.u.ioadl[3];
	} else {
		ioarcb->ioadl_bus_addr =
			cpu_to_le64((cmd->ioa_cb_bus_addr) +
				offsetof(struct pmcraid_ioarcb,
					add_data.u.ioadl[ioadl_count]));

		ioadl = &ioarcb->add_data.u.ioadl[ioadl_count];
		ioarcb->ioarcb_bus_addr |=
				DIV_ROUND_CLOSEST(sgcount + ioadl_count, 8);
	}

	return ioadl;
}

static int pmcraid_build_ioadl(
	struct pmcraid_instance *pinstance,
	struct pmcraid_cmd *cmd
)
{
	int i, nseg;
	struct scatterlist *sglist;

	struct scsi_cmnd *scsi_cmd = cmd->scsi_cmd;
	struct pmcraid_ioarcb *ioarcb = &(cmd->ioa_cb->ioarcb);
	struct pmcraid_ioadl_desc *ioadl = ioarcb->add_data.u.ioadl;

	u32 length = scsi_bufflen(scsi_cmd);

	if (!length)
		return 0;

	nseg = scsi_dma_map(scsi_cmd);

	if (nseg < 0) {
		scmd_printk(KERN_ERR, scsi_cmd, "scsi_map_dma failed!\n");
		return -1;
	} else if (nseg > PMCRAID_MAX_IOADLS) {
		scsi_dma_unmap(scsi_cmd);
		scmd_printk(KERN_ERR, scsi_cmd,
			"sg count is (%d) more than allowed!\n", nseg);
		return -1;
	}

	/* Initialize IOARCB data transfer length fields */
	if (scsi_cmd->sc_data_direction == DMA_TO_DEVICE)
		ioarcb->request_flags0 |= TRANSFER_DIR_WRITE;

	ioarcb->request_flags0 |= NO_LINK_DESCS;
	ioarcb->data_transfer_length = cpu_to_le32(length);
	ioadl = pmcraid_init_ioadls(cmd, nseg);

	/* Initialize IOADL descriptor addresses */
	scsi_for_each_sg(scsi_cmd, sglist, nseg, i) {
		ioadl[i].data_len = cpu_to_le32(sg_dma_len(sglist));
		ioadl[i].address = cpu_to_le64(sg_dma_address(sglist));
		ioadl[i].flags = 0;
	}
	/* setup last descriptor */
	ioadl[i - 1].flags = IOADL_FLAGS_LAST_DESC;

	return 0;
}

static void pmcraid_free_sglist(struct pmcraid_sglist *sglist)
{
	int i;

	for (i = 0; i < sglist->num_sg; i++)
		__free_pages(sg_page(&(sglist->scatterlist[i])),
			     sglist->order);

	kfree(sglist);
}

static struct pmcraid_sglist *pmcraid_alloc_sglist(int buflen)
{
	struct pmcraid_sglist *sglist;
	struct scatterlist *scatterlist;
	struct page *page;
	int num_elem, i, j;
	int sg_size;
	int order;
	int bsize_elem;

	sg_size = buflen / (PMCRAID_MAX_IOADLS - 1);
	order = (sg_size > 0) ? get_order(sg_size) : 0;
	bsize_elem = PAGE_SIZE * (1 << order);

	/* Determine the actual number of sg entries needed */
	if (buflen % bsize_elem)
		num_elem = (buflen / bsize_elem) + 1;
	else
		num_elem = buflen / bsize_elem;

	/* Allocate a scatter/gather list for the DMA */
	sglist = kzalloc(sizeof(struct pmcraid_sglist) +
			 (sizeof(struct scatterlist) * (num_elem - 1)),
			 GFP_KERNEL);

	if (sglist == NULL)
		return NULL;

	scatterlist = sglist->scatterlist;
	sg_init_table(scatterlist, num_elem);
	sglist->order = order;
	sglist->num_sg = num_elem;
	sg_size = buflen;

	for (i = 0; i < num_elem; i++) {
		page = alloc_pages(GFP_KERNEL|GFP_DMA, order);
		if (!page) {
			for (j = i - 1; j >= 0; j--)
				__free_pages(sg_page(&scatterlist[j]), order);
			kfree(sglist);
			return NULL;
		}

		sg_set_page(&scatterlist[i], page,
			sg_size < bsize_elem ? sg_size : bsize_elem, 0);
		sg_size -= bsize_elem;
	}

	return sglist;
}

static int pmcraid_copy_sglist(
	struct pmcraid_sglist *sglist,
	unsigned long buffer,
	u32 len,
	int direction
)
{
	struct scatterlist *scatterlist;
	void *kaddr;
	int bsize_elem;
	int i;
	int rc = 0;

	/* Determine the actual number of bytes per element */
	bsize_elem = PAGE_SIZE * (1 << sglist->order);

	scatterlist = sglist->scatterlist;

	for (i = 0; i < (len / bsize_elem); i++, buffer += bsize_elem) {
		struct page *page = sg_page(&scatterlist[i]);

		kaddr = kmap(page);
		if (direction == DMA_TO_DEVICE)
			rc = __copy_from_user(kaddr,
					      (void *)buffer,
					      bsize_elem);
		else
			rc = __copy_to_user((void *)buffer, kaddr, bsize_elem);

		kunmap(page);

		if (rc) {
			pmcraid_err("failed to copy user data into sg list\n");
			return -EFAULT;
		}

		scatterlist[i].length = bsize_elem;
	}

	if (len % bsize_elem) {
		struct page *page = sg_page(&scatterlist[i]);

		kaddr = kmap(page);

		if (direction == DMA_TO_DEVICE)
			rc = __copy_from_user(kaddr,
					      (void *)buffer,
					      len % bsize_elem);
		else
			rc = __copy_to_user((void *)buffer,
					    kaddr,
					    len % bsize_elem);

		kunmap(page);

		scatterlist[i].length = len % bsize_elem;
	}

	if (rc) {
		pmcraid_err("failed to copy user data into sg list\n");
		rc = -EFAULT;
	}

	return rc;
}

static int pmcraid_queuecommand(
	struct scsi_cmnd *scsi_cmd,
	void (*done) (struct scsi_cmnd *)
)
{
	struct pmcraid_instance *pinstance;
	struct pmcraid_resource_entry *res;
	struct pmcraid_ioarcb *ioarcb;
	struct pmcraid_cmd *cmd;
	int rc = 0;

	pinstance =
		(struct pmcraid_instance *)scsi_cmd->device->host->hostdata;

	scsi_cmd->scsi_done = done;
	res = scsi_cmd->device->hostdata;
	scsi_cmd->result = (DID_OK << 16);

	/* if adapter is marked as dead, set result to DID_NO_CONNECT complete
	 * the command
	 */
	if (pinstance->ioa_state == IOA_STATE_DEAD) {
		pmcraid_info("IOA is dead, but queuecommand is scheduled\n");
		scsi_cmd->result = (DID_NO_CONNECT << 16);
		scsi_cmd->scsi_done(scsi_cmd);
		return 0;
	}

	/* If IOA reset is in progress, can't queue the commands */
	if (pinstance->ioa_reset_in_progress)
		return SCSI_MLQUEUE_HOST_BUSY;

	/* initialize the command and IOARCB to be sent to IOA */
	cmd = pmcraid_get_free_cmd(pinstance);

	if (cmd == NULL) {
		pmcraid_err("free command block is not available\n");
		return SCSI_MLQUEUE_HOST_BUSY;
	}

	cmd->scsi_cmd = scsi_cmd;
	ioarcb = &(cmd->ioa_cb->ioarcb);
	memcpy(ioarcb->cdb, scsi_cmd->cmnd, scsi_cmd->cmd_len);
	ioarcb->resource_handle = res->cfg_entry.resource_handle;
	ioarcb->request_type = REQ_TYPE_SCSI;

	cmd->cmd_done = pmcraid_io_done;

	if (RES_IS_GSCSI(res->cfg_entry) || RES_IS_VSET(res->cfg_entry)) {
		if (scsi_cmd->underflow == 0)
			ioarcb->request_flags0 |= INHIBIT_UL_CHECK;

		if (res->sync_reqd) {
			ioarcb->request_flags0 |= SYNC_COMPLETE;
			res->sync_reqd = 0;
		}

		ioarcb->request_flags0 |= NO_LINK_DESCS;
		ioarcb->request_flags1 |= pmcraid_task_attributes(scsi_cmd);

		if (RES_IS_GSCSI(res->cfg_entry))
			ioarcb->request_flags1 |= DELAY_AFTER_RESET;
	}

	rc = pmcraid_build_ioadl(pinstance, cmd);

	pmcraid_info("command (%d) CDB[0] = %x for %x:%x:%x:%x\n",
		     le32_to_cpu(ioarcb->response_handle) >> 2,
		     scsi_cmd->cmnd[0], pinstance->host->unique_id,
		     RES_IS_VSET(res->cfg_entry) ? PMCRAID_VSET_BUS_ID :
			PMCRAID_PHYS_BUS_ID,
		     RES_IS_VSET(res->cfg_entry) ?
			res->cfg_entry.unique_flags1 :
			RES_TARGET(res->cfg_entry.resource_address),
		     RES_LUN(res->cfg_entry.resource_address));

	if (likely(rc == 0)) {
		_pmcraid_fire_command(cmd);
	} else {
		pmcraid_err("queuecommand could not build ioadl\n");
		pmcraid_return_cmd(cmd);
		rc = SCSI_MLQUEUE_HOST_BUSY;
	}

	return rc;
}

static int pmcraid_chr_open(struct inode *inode, struct file *filep)
{
	struct pmcraid_instance *pinstance;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	/* Populate adapter instance * pointer for use by ioctl */
	pinstance = container_of(inode->i_cdev, struct pmcraid_instance, cdev);
	filep->private_data = pinstance;

	return 0;
}

static int pmcraid_chr_release(struct inode *inode, struct file *filep)
{
	struct pmcraid_instance *pinstance =
		((struct pmcraid_instance *)filep->private_data);

	filep->private_data = NULL;
	fasync_helper(-1, filep, 0, &pinstance->aen_queue);

	return 0;
}

static int pmcraid_chr_fasync(int fd, struct file *filep, int mode)
{
	struct pmcraid_instance *pinstance;
	int rc;

	pinstance = (struct pmcraid_instance *)filep->private_data;
	mutex_lock(&pinstance->aen_queue_lock);
	rc = fasync_helper(fd, filep, mode, &pinstance->aen_queue);
	mutex_unlock(&pinstance->aen_queue_lock);

	return rc;
}


static int pmcraid_build_passthrough_ioadls(
	struct pmcraid_cmd *cmd,
	int buflen,
	int direction
)
{
	struct pmcraid_sglist *sglist = NULL;
	struct scatterlist *sg = NULL;
	struct pmcraid_ioarcb *ioarcb = &cmd->ioa_cb->ioarcb;
	struct pmcraid_ioadl_desc *ioadl;
	int i;

	sglist = pmcraid_alloc_sglist(buflen);

	if (!sglist) {
		pmcraid_err("can't allocate memory for passthrough SGls\n");
		return -ENOMEM;
	}

	sglist->num_dma_sg = pci_map_sg(cmd->drv_inst->pdev,
					sglist->scatterlist,
					sglist->num_sg, direction);

	if (!sglist->num_dma_sg || sglist->num_dma_sg > PMCRAID_MAX_IOADLS) {
		dev_err(&cmd->drv_inst->pdev->dev,
			"Failed to map passthrough buffer!\n");
		pmcraid_free_sglist(sglist);
		return -EIO;
	}

	cmd->sglist = sglist;
	ioarcb->request_flags0 |= NO_LINK_DESCS;

	ioadl = pmcraid_init_ioadls(cmd, sglist->num_dma_sg);

	/* Initialize IOADL descriptor addresses */
	for_each_sg(sglist->scatterlist, sg, sglist->num_dma_sg, i) {
		ioadl[i].data_len = cpu_to_le32(sg_dma_len(sg));
		ioadl[i].address = cpu_to_le64(sg_dma_address(sg));
		ioadl[i].flags = 0;
	}

	/* setup the last descriptor */
	ioadl[i - 1].flags = IOADL_FLAGS_LAST_DESC;

	return 0;
}


static void pmcraid_release_passthrough_ioadls(
	struct pmcraid_cmd *cmd,
	int buflen,
	int direction
)
{
	struct pmcraid_sglist *sglist = cmd->sglist;

	if (buflen > 0) {
		pci_unmap_sg(cmd->drv_inst->pdev,
			     sglist->scatterlist,
			     sglist->num_sg,
			     direction);
		pmcraid_free_sglist(sglist);
		cmd->sglist = NULL;
	}
}

static long pmcraid_ioctl_passthrough(
	struct pmcraid_instance *pinstance,
	unsigned int ioctl_cmd,
	unsigned int buflen,
	unsigned long arg
)
{
	struct pmcraid_passthrough_ioctl_buffer *buffer;
	struct pmcraid_ioarcb *ioarcb;
	struct pmcraid_cmd *cmd;
	struct pmcraid_cmd *cancel_cmd;
	unsigned long request_buffer;
	unsigned long request_offset;
	unsigned long lock_flags;
	int request_size;
	int buffer_size;
	u8 access, direction;
	int rc = 0;

	/* If IOA reset is in progress, wait 10 secs for reset to complete */
	if (pinstance->ioa_reset_in_progress) {
		rc = wait_event_interruptible_timeout(
				pinstance->reset_wait_q,
				!pinstance->ioa_reset_in_progress,
				msecs_to_jiffies(10000));

		if (!rc)
			return -ETIMEDOUT;
		else if (rc < 0)
			return -ERESTARTSYS;
	}

	/* If adapter is not in operational state, return error */
	if (pinstance->ioa_state != IOA_STATE_OPERATIONAL) {
		pmcraid_err("IOA is not operational\n");
		return -ENOTTY;
	}

	buffer_size = sizeof(struct pmcraid_passthrough_ioctl_buffer);
	buffer = kmalloc(buffer_size, GFP_KERNEL);

	if (!buffer) {
		pmcraid_err("no memory for passthrough buffer\n");
		return -ENOMEM;
	}

	request_offset =
	    offsetof(struct pmcraid_passthrough_ioctl_buffer, request_buffer);

	request_buffer = arg + request_offset;

	rc = __copy_from_user(buffer,
			     (struct pmcraid_passthrough_ioctl_buffer *) arg,
			     sizeof(struct pmcraid_passthrough_ioctl_buffer));
	if (rc) {
		pmcraid_err("ioctl: can't copy passthrough buffer\n");
		rc = -EFAULT;
		goto out_free_buffer;
	}

	request_size = buffer->ioarcb.data_transfer_length;

	if (buffer->ioarcb.request_flags0 & TRANSFER_DIR_WRITE) {
		access = VERIFY_READ;
		direction = DMA_TO_DEVICE;
	} else {
		access = VERIFY_WRITE;
		direction = DMA_FROM_DEVICE;
	}

	if (request_size > 0) {
		rc = access_ok(access, arg, request_offset + request_size);

		if (!rc) {
			rc = -EFAULT;
			goto out_free_buffer;
		}
	}

	/* check if we have any additional command parameters */
	if (buffer->ioarcb.add_cmd_param_length > PMCRAID_ADD_CMD_PARAM_LEN) {
		rc = -EINVAL;
		goto out_free_buffer;
	}

	cmd = pmcraid_get_free_cmd(pinstance);

	if (!cmd) {
		pmcraid_err("free command block is not available\n");
		rc = -ENOMEM;
		goto out_free_buffer;
	}

	cmd->scsi_cmd = NULL;
	ioarcb = &(cmd->ioa_cb->ioarcb);

	/* Copy the user-provided IOARCB stuff field by field */
	ioarcb->resource_handle = buffer->ioarcb.resource_handle;
	ioarcb->data_transfer_length = buffer->ioarcb.data_transfer_length;
	ioarcb->cmd_timeout = buffer->ioarcb.cmd_timeout;
	ioarcb->request_type = buffer->ioarcb.request_type;
	ioarcb->request_flags0 = buffer->ioarcb.request_flags0;
	ioarcb->request_flags1 = buffer->ioarcb.request_flags1;
	memcpy(ioarcb->cdb, buffer->ioarcb.cdb, PMCRAID_MAX_CDB_LEN);

	if (buffer->ioarcb.add_cmd_param_length) {
		ioarcb->add_cmd_param_length =
			buffer->ioarcb.add_cmd_param_length;
		ioarcb->add_cmd_param_offset =
			buffer->ioarcb.add_cmd_param_offset;
		memcpy(ioarcb->add_data.u.add_cmd_params,
			buffer->ioarcb.add_data.u.add_cmd_params,
			buffer->ioarcb.add_cmd_param_length);
	}

	if (request_size) {
		rc = pmcraid_build_passthrough_ioadls(cmd,
						      request_size,
						      direction);
		if (rc) {
			pmcraid_err("couldn't build passthrough ioadls\n");
			goto out_free_buffer;
		}
	}

	/* If data is being written into the device, copy the data from user
	 * buffers
	 */
	if (direction == DMA_TO_DEVICE && request_size > 0) {
		rc = pmcraid_copy_sglist(cmd->sglist,
					 request_buffer,
					 request_size,
					 direction);
		if (rc) {
			pmcraid_err("failed to copy user buffer\n");
			goto out_free_sglist;
		}
	}

	/* passthrough ioctl is a blocking command so, put the user to sleep
	 * until timeout. Note that a timeout value of 0 means, do timeout.
	 */
	cmd->cmd_done = pmcraid_internal_done;
	init_completion(&cmd->wait_for_completion);
	cmd->completion_req = 1;

	pmcraid_info("command(%d) (CDB[0] = %x) for %x\n",
		     le32_to_cpu(cmd->ioa_cb->ioarcb.response_handle) >> 2,
		     cmd->ioa_cb->ioarcb.cdb[0],
		     le32_to_cpu(cmd->ioa_cb->ioarcb.resource_handle));

	spin_lock_irqsave(pinstance->host->host_lock, lock_flags);
	_pmcraid_fire_command(cmd);
	spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);

	/* If command timeout is specified put caller to wait till that time,
	 * otherwise it would be blocking wait. If command gets timed out, it
	 * will be aborted.
	 */
	if (buffer->ioarcb.cmd_timeout == 0) {
		wait_for_completion(&cmd->wait_for_completion);
	} else if (!wait_for_completion_timeout(
			&cmd->wait_for_completion,
			msecs_to_jiffies(buffer->ioarcb.cmd_timeout * 1000))) {

		pmcraid_info("aborting cmd %d (CDB[0] = %x) due to timeout\n",
			le32_to_cpu(cmd->ioa_cb->ioarcb.response_handle >> 2),
			cmd->ioa_cb->ioarcb.cdb[0]);

		rc = -ETIMEDOUT;
		spin_lock_irqsave(pinstance->host->host_lock, lock_flags);
		cancel_cmd = pmcraid_abort_cmd(cmd);
		spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);

		if (cancel_cmd) {
			wait_for_completion(&cancel_cmd->wait_for_completion);
			pmcraid_return_cmd(cancel_cmd);
		}

		goto out_free_sglist;
	}

	/* If the command failed for any reason, copy entire IOASA buffer and
	 * return IOCTL success. If copying IOASA to user-buffer fails, return
	 * EFAULT
	 */
	if (le32_to_cpu(cmd->ioa_cb->ioasa.ioasc)) {

		void *ioasa =
		    (void *)(arg +
		    offsetof(struct pmcraid_passthrough_ioctl_buffer, ioasa));

		pmcraid_info("command failed with %x\n",
			     le32_to_cpu(cmd->ioa_cb->ioasa.ioasc));
		if (copy_to_user(ioasa, &cmd->ioa_cb->ioasa,
				 sizeof(struct pmcraid_ioasa))) {
			pmcraid_err("failed to copy ioasa buffer to user\n");
			rc = -EFAULT;
		}
	}
	/* If the data transfer was from device, copy the data onto user
	 * buffers
	 */
	else if (direction == DMA_FROM_DEVICE && request_size > 0) {
		rc = pmcraid_copy_sglist(cmd->sglist,
					 request_buffer,
					 request_size,
					 direction);
		if (rc) {
			pmcraid_err("failed to copy user buffer\n");
			rc = -EFAULT;
		}
	}

out_free_sglist:
	pmcraid_release_passthrough_ioadls(cmd, request_size, direction);
	pmcraid_return_cmd(cmd);

out_free_buffer:
	kfree(buffer);

	return rc;
}




static long pmcraid_ioctl_driver(
	struct pmcraid_instance *pinstance,
	unsigned int cmd,
	unsigned int buflen,
	void __user *user_buffer
)
{
	int rc = -ENOSYS;

	if (!access_ok(VERIFY_READ, user_buffer, _IOC_SIZE(cmd))) {
		pmcraid_err("ioctl_driver: access fault in request buffer \n");
		return -EFAULT;
	}

	switch (cmd) {
	case PMCRAID_IOCTL_RESET_ADAPTER:
		pmcraid_reset_bringup(pinstance);
		rc = 0;
		break;

	default:
		break;
	}

	return rc;
}


static int pmcraid_check_ioctl_buffer(
	int cmd,
	void __user *arg,
	struct pmcraid_ioctl_header *hdr
)
{
	int rc = 0;
	int access = VERIFY_READ;

	if (copy_from_user(hdr, arg, sizeof(struct pmcraid_ioctl_header))) {
		pmcraid_err("couldn't copy ioctl header from user buffer\n");
		return -EFAULT;
	}

	/* check for valid driver signature */
	rc = memcmp(hdr->signature,
		    PMCRAID_IOCTL_SIGNATURE,
		    sizeof(hdr->signature));
	if (rc) {
		pmcraid_err("signature verification failed\n");
		return -EINVAL;
	}

	/* check for appropriate buffer access */
	if ((_IOC_DIR(cmd) & _IOC_READ) == _IOC_READ)
		access = VERIFY_WRITE;

	rc = access_ok(access,
		       (arg + sizeof(struct pmcraid_ioctl_header)),
		       hdr->buffer_length);
	if (!rc) {
		pmcraid_err("access failed for user buffer of size %d\n",
			     hdr->buffer_length);
		return -EFAULT;
	}

	return 0;
}

static long pmcraid_chr_ioctl(
	struct file *filep,
	unsigned int cmd,
	unsigned long arg
)
{
	struct pmcraid_instance *pinstance = NULL;
	struct pmcraid_ioctl_header *hdr = NULL;
	int retval = -ENOTTY;

	hdr = kmalloc(GFP_KERNEL, sizeof(struct pmcraid_ioctl_header));

	if (!hdr) {
		pmcraid_err("faile to allocate memory for ioctl header\n");
		return -ENOMEM;
	}

	retval = pmcraid_check_ioctl_buffer(cmd, (void *)arg, hdr);

	if (retval) {
		pmcraid_info("chr_ioctl: header check failed\n");
		kfree(hdr);
		return retval;
	}

	pinstance = (struct pmcraid_instance *)filep->private_data;

	if (!pinstance) {
		pmcraid_info("adapter instance is not found\n");
		kfree(hdr);
		return -ENOTTY;
	}

	switch (_IOC_TYPE(cmd)) {

	case PMCRAID_PASSTHROUGH_IOCTL:
		/* If ioctl code is to download microcode, we need to block
		 * mid-layer requests.
		 */
		if (cmd == PMCRAID_IOCTL_DOWNLOAD_MICROCODE)
			scsi_block_requests(pinstance->host);

		retval = pmcraid_ioctl_passthrough(pinstance,
						   cmd,
						   hdr->buffer_length,
						   arg);

		if (cmd == PMCRAID_IOCTL_DOWNLOAD_MICROCODE)
			scsi_unblock_requests(pinstance->host);
		break;

	case PMCRAID_DRIVER_IOCTL:
		arg += sizeof(struct pmcraid_ioctl_header);
		retval = pmcraid_ioctl_driver(pinstance,
					      cmd,
					      hdr->buffer_length,
					      (void __user *)arg);
		break;

	default:
		retval = -ENOTTY;
		break;
	}

	kfree(hdr);

	return retval;
}

static const struct file_operations pmcraid_fops = {
	.owner = THIS_MODULE,
	.open = pmcraid_chr_open,
	.release = pmcraid_chr_release,
	.fasync = pmcraid_chr_fasync,
	.unlocked_ioctl = pmcraid_chr_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = pmcraid_chr_ioctl,
#endif
};




static ssize_t pmcraid_show_log_level(
	struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	struct Scsi_Host *shost = class_to_shost(dev);
	struct pmcraid_instance *pinstance =
		(struct pmcraid_instance *)shost->hostdata;
	return snprintf(buf, PAGE_SIZE, "%d\n", pinstance->current_log_level);
}

static ssize_t pmcraid_store_log_level(
	struct device *dev,
	struct device_attribute *attr,
	const char *buf,
	size_t count
)
{
	struct Scsi_Host *shost;
	struct pmcraid_instance *pinstance;
	unsigned long val;

	if (strict_strtoul(buf, 10, &val))
		return -EINVAL;
	/* log-level should be from 0 to 2 */
	if (val > 2)
		return -EINVAL;

	shost = class_to_shost(dev);
	pinstance = (struct pmcraid_instance *)shost->hostdata;
	pinstance->current_log_level = val;

	return strlen(buf);
}

static struct device_attribute pmcraid_log_level_attr = {
	.attr = {
		 .name = "log_level",
		 .mode = S_IRUGO | S_IWUSR,
		 },
	.show = pmcraid_show_log_level,
	.store = pmcraid_store_log_level,
};

static ssize_t pmcraid_show_drv_version(
	struct device *dev,
	struct device_attribute *attr,
	char *buf
)
{
	return snprintf(buf, PAGE_SIZE, "version: %s, build date: %s\n",
			PMCRAID_DRIVER_VERSION, PMCRAID_DRIVER_DATE);
}

static struct device_attribute pmcraid_driver_version_attr = {
	.attr = {
		 .name = "drv_version",
		 .mode = S_IRUGO,
		 },
	.show = pmcraid_show_drv_version,
};

static ssize_t pmcraid_show_adapter_id(
	struct device *dev,
	struct device_attribute *attr,
	char *buf
)
{
	struct Scsi_Host *shost = class_to_shost(dev);
	struct pmcraid_instance *pinstance =
		(struct pmcraid_instance *)shost->hostdata;
	u32 adapter_id = (pinstance->pdev->bus->number << 8) |
		pinstance->pdev->devfn;
	u32 aen_group = pmcraid_event_family.id;

	return snprintf(buf, PAGE_SIZE,
			"adapter id: %d\nminor: %d\naen group: %d\n",
			adapter_id, MINOR(pinstance->cdev.dev), aen_group);
}

static struct device_attribute pmcraid_adapter_id_attr = {
	.attr = {
		 .name = "adapter_id",
		 .mode = S_IRUGO | S_IWUSR,
		 },
	.show = pmcraid_show_adapter_id,
};

static struct device_attribute *pmcraid_host_attrs[] = {
	&pmcraid_log_level_attr,
	&pmcraid_driver_version_attr,
	&pmcraid_adapter_id_attr,
	NULL,
};


/* host template structure for pmcraid driver */
static struct scsi_host_template pmcraid_host_template = {
	.module = THIS_MODULE,
	.name = PMCRAID_DRIVER_NAME,
	.queuecommand = pmcraid_queuecommand,
	.eh_abort_handler = pmcraid_eh_abort_handler,
	.eh_bus_reset_handler = pmcraid_eh_bus_reset_handler,
	.eh_target_reset_handler = pmcraid_eh_target_reset_handler,
	.eh_device_reset_handler = pmcraid_eh_device_reset_handler,
	.eh_host_reset_handler = pmcraid_eh_host_reset_handler,

	.slave_alloc = pmcraid_slave_alloc,
	.slave_configure = pmcraid_slave_configure,
	.slave_destroy = pmcraid_slave_destroy,
	.change_queue_depth = pmcraid_change_queue_depth,
	.change_queue_type  = pmcraid_change_queue_type,
	.can_queue = PMCRAID_MAX_IO_CMD,
	.this_id = -1,
	.sg_tablesize = PMCRAID_MAX_IOADLS,
	.max_sectors = PMCRAID_IOA_MAX_SECTORS,
	.cmd_per_lun = PMCRAID_MAX_CMD_PER_LUN,
	.use_clustering = ENABLE_CLUSTERING,
	.shost_attrs = pmcraid_host_attrs,
	.proc_name = PMCRAID_DRIVER_NAME
};

static void pmcraid_isr_common(
	struct pmcraid_instance *pinstance,
	u32 intrs,
	int hrrq_id
)
{
	u32 intrs_clear =
		(intrs & INTRS_CRITICAL_OP_IN_PROGRESS) ? intrs
							: INTRS_HRRQ_VALID;
	iowrite32(intrs_clear,
		  pinstance->int_regs.ioa_host_interrupt_clr_reg);
	intrs = ioread32(pinstance->int_regs.ioa_host_interrupt_reg);

	/* hrrq valid bit was set, schedule tasklet to handle the response */
	if (intrs_clear == INTRS_HRRQ_VALID)
		tasklet_schedule(&(pinstance->isr_tasklet[hrrq_id]));
}

static irqreturn_t pmcraid_isr(int irq, void *dev_id)
{
	struct pmcraid_isr_param *hrrq_vector;
	struct pmcraid_instance *pinstance;
	unsigned long lock_flags;
	u32 intrs;

	/* In case of legacy interrupt mode where interrupts are shared across
	 * isrs, it may be possible that the current interrupt is not from IOA
	 */
	if (!dev_id) {
		printk(KERN_INFO "%s(): NULL host pointer\n", __func__);
		return IRQ_NONE;
	}

	hrrq_vector = (struct pmcraid_isr_param *)dev_id;
	pinstance = hrrq_vector->drv_inst;

	/* Acquire the lock (currently host_lock) while processing interrupts.
	 * This interval is small as most of the response processing is done by
	 * tasklet without the lock.
	 */
	spin_lock_irqsave(pinstance->host->host_lock, lock_flags);
	intrs = pmcraid_read_interrupts(pinstance);

	if (unlikely((intrs & PMCRAID_PCI_INTERRUPTS) == 0)) {
		spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);
		return IRQ_NONE;
	}

	/* Any error interrupts including unit_check, initiate IOA reset.
	 * In case of unit check indicate to reset_sequence that IOA unit
	 * checked and prepare for a dump during reset sequence
	 */
	if (intrs & PMCRAID_ERROR_INTERRUPTS) {

		if (intrs & INTRS_IOA_UNIT_CHECK)
			pinstance->ioa_unit_check = 1;

		iowrite32(intrs,
			  pinstance->int_regs.ioa_host_interrupt_clr_reg);
		pmcraid_err("ISR: error interrupts: %x initiating reset\n",
			    intrs);
		intrs = ioread32(pinstance->int_regs.ioa_host_interrupt_reg);
		pmcraid_initiate_reset(pinstance);
	} else {
		pmcraid_isr_common(pinstance, intrs, hrrq_vector->hrrq_id);
	}

	spin_unlock_irqrestore(pinstance->host->host_lock, lock_flags);

	return IRQ_HANDLED;
}



static void pmcraid_worker_function(struct work_struct *workp)
{
	struct pmcraid_instance *pinstance;
	struct pmcraid_resource_entry *res;
	struct pmcraid_resource_entry *temp;
	struct scsi_device *sdev;
	unsigned long lock_flags;
	unsigned long host_lock_flags;
	u8 bus, target, lun;

	pinstance = container_of(workp, struct pmcraid_instance, worker_q);
	/* add resources only after host is added into system */
	if (!atomic_read(&pinstance->expose_resources))
		return;

	spin_lock_irqsave(&pinstance->resource_lock, lock_flags);
	list_for_each_entry_safe(res, temp, &pinstance->used_res_q, queue) {

		if (res->change_detected == RES_CHANGE_DEL && res->scsi_dev) {
			sdev = res->scsi_dev;

			/* host_lock must be held before calling
			 * scsi_device_get
			 */
			spin_lock_irqsave(pinstance->host->host_lock,
					  host_lock_flags);
			if (!scsi_device_get(sdev)) {
				spin_unlock_irqrestore(
						pinstance->host->host_lock,
						host_lock_flags);
				pmcraid_info("deleting %x from midlayer\n",
					     res->cfg_entry.resource_address);
				list_move_tail(&res->queue,
						&pinstance->free_res_q);
				spin_unlock_irqrestore(
					&pinstance->resource_lock,
					lock_flags);
				scsi_remove_device(sdev);
				scsi_device_put(sdev);
				spin_lock_irqsave(&pinstance->resource_lock,
						   lock_flags);
				res->change_detected = 0;
			} else {
				spin_unlock_irqrestore(
						pinstance->host->host_lock,
						host_lock_flags);
			}
		}
	}

	list_for_each_entry(res, &pinstance->used_res_q, queue) {

		if (res->change_detected == RES_CHANGE_ADD) {

			if (!pmcraid_expose_resource(&res->cfg_entry))
				continue;

			if (RES_IS_VSET(res->cfg_entry)) {
				bus = PMCRAID_VSET_BUS_ID;
				target = res->cfg_entry.unique_flags1;
				lun = PMCRAID_VSET_LUN_ID;
			} else {
				bus = PMCRAID_PHYS_BUS_ID;
				target =
				     RES_TARGET(
					res->cfg_entry.resource_address);
				lun = RES_LUN(res->cfg_entry.resource_address);
			}

			res->change_detected = 0;
			spin_unlock_irqrestore(&pinstance->resource_lock,
						lock_flags);
			scsi_add_device(pinstance->host, bus, target, lun);
			spin_lock_irqsave(&pinstance->resource_lock,
					   lock_flags);
		}
	}

	spin_unlock_irqrestore(&pinstance->resource_lock, lock_flags);
}

void pmcraid_tasklet_function(unsigned long instance)
{
	struct pmcraid_isr_param *hrrq_vector;
	struct pmcraid_instance *pinstance;
	unsigned long hrrq_lock_flags;
	unsigned long pending_lock_flags;
	unsigned long host_lock_flags;
	spinlock_t *lockp; /* hrrq buffer lock */
	int id;
	u32 intrs;
	__le32 resp;

	hrrq_vector = (struct pmcraid_isr_param *)instance;
	pinstance = hrrq_vector->drv_inst;
	id = hrrq_vector->hrrq_id;
	lockp = &(pinstance->hrrq_lock[id]);
	intrs = pmcraid_read_interrupts(pinstance);

	/* If interrupts was as part of the ioa initialization, clear and mask
	 * it. Delete the timer and wakeup the reset engine to proceed with
	 * reset sequence
	 */
	if (intrs & INTRS_TRANSITION_TO_OPERATIONAL) {
		iowrite32(INTRS_TRANSITION_TO_OPERATIONAL,
			pinstance->int_regs.ioa_host_interrupt_mask_reg);
		iowrite32(INTRS_TRANSITION_TO_OPERATIONAL,
			pinstance->int_regs.ioa_host_interrupt_clr_reg);

		if (pinstance->reset_cmd != NULL) {
			del_timer(&pinstance->reset_cmd->timer);
			spin_lock_irqsave(pinstance->host->host_lock,
					  host_lock_flags);
			pinstance->reset_cmd->cmd_done(pinstance->reset_cmd);
			spin_unlock_irqrestore(pinstance->host->host_lock,
					       host_lock_flags);
		}
		return;
	}

	/* loop through each of the commands responded by IOA. Each HRRQ buf is
	 * protected by its own lock. Traversals must be done within this lock
	 * as there may be multiple tasklets running on multiple CPUs. Note
	 * that the lock is held just for picking up the response handle and
	 * manipulating hrrq_curr/toggle_bit values.
	 */
	spin_lock_irqsave(lockp, hrrq_lock_flags);

	resp = le32_to_cpu(*(pinstance->hrrq_curr[id]));

	while ((resp & HRRQ_TOGGLE_BIT) ==
		pinstance->host_toggle_bit[id]) {

		int cmd_index = resp >> 2;
		struct pmcraid_cmd *cmd = NULL;

		if (cmd_index < PMCRAID_MAX_CMD) {
			cmd = pinstance->cmd_list[cmd_index];
		} else {
			/* In case of invalid response handle, initiate IOA
			 * reset sequence.
			 */
			spin_unlock_irqrestore(lockp, hrrq_lock_flags);

			pmcraid_err("Invalid response %d initiating reset\n",
				    cmd_index);

			spin_lock_irqsave(pinstance->host->host_lock,
					  host_lock_flags);
			pmcraid_initiate_reset(pinstance);
			spin_unlock_irqrestore(pinstance->host->host_lock,
					       host_lock_flags);

			spin_lock_irqsave(lockp, hrrq_lock_flags);
			break;
		}

		if (pinstance->hrrq_curr[id] < pinstance->hrrq_end[id]) {
			pinstance->hrrq_curr[id]++;
		} else {
			pinstance->hrrq_curr[id] = pinstance->hrrq_start[id];
			pinstance->host_toggle_bit[id] ^= 1u;
		}

		spin_unlock_irqrestore(lockp, hrrq_lock_flags);

		spin_lock_irqsave(&pinstance->pending_pool_lock,
				   pending_lock_flags);
		list_del(&cmd->free_list);
		spin_unlock_irqrestore(&pinstance->pending_pool_lock,
					pending_lock_flags);
		del_timer(&cmd->timer);
		atomic_dec(&pinstance->outstanding_cmds);

		if (cmd->cmd_done == pmcraid_ioa_reset) {
			spin_lock_irqsave(pinstance->host->host_lock,
					  host_lock_flags);
			cmd->cmd_done(cmd);
			spin_unlock_irqrestore(pinstance->host->host_lock,
					       host_lock_flags);
		} else if (cmd->cmd_done != NULL) {
			cmd->cmd_done(cmd);
		}
		/* loop over until we are done with all responses */
		spin_lock_irqsave(lockp, hrrq_lock_flags);
		resp = le32_to_cpu(*(pinstance->hrrq_curr[id]));
	}

	spin_unlock_irqrestore(lockp, hrrq_lock_flags);
}

static
void pmcraid_unregister_interrupt_handler(struct pmcraid_instance *pinstance)
{
	free_irq(pinstance->pdev->irq, &(pinstance->hrrq_vector[0]));
}

static int
pmcraid_register_interrupt_handler(struct pmcraid_instance *pinstance)
{
	struct pci_dev *pdev = pinstance->pdev;

	pinstance->hrrq_vector[0].hrrq_id = 0;
	pinstance->hrrq_vector[0].drv_inst = pinstance;
	pinstance->hrrq_vector[0].vector = 0;
	pinstance->num_hrrq = 1;
	return request_irq(pdev->irq, pmcraid_isr, IRQF_SHARED,
			   PMCRAID_DRIVER_NAME, &pinstance->hrrq_vector[0]);
}

static void
pmcraid_release_cmd_blocks(struct pmcraid_instance *pinstance, int max_index)
{
	int i;
	for (i = 0; i < max_index; i++) {
		kmem_cache_free(pinstance->cmd_cachep, pinstance->cmd_list[i]);
		pinstance->cmd_list[i] = NULL;
	}
	kmem_cache_destroy(pinstance->cmd_cachep);
	pinstance->cmd_cachep = NULL;
}

static void
pmcraid_release_control_blocks(
	struct pmcraid_instance *pinstance,
	int max_index
)
{
	int i;

	if (pinstance->control_pool == NULL)
		return;

	for (i = 0; i < max_index; i++) {
		pci_pool_free(pinstance->control_pool,
			      pinstance->cmd_list[i]->ioa_cb,
			      pinstance->cmd_list[i]->ioa_cb_bus_addr);
		pinstance->cmd_list[i]->ioa_cb = NULL;
		pinstance->cmd_list[i]->ioa_cb_bus_addr = 0;
	}
	pci_pool_destroy(pinstance->control_pool);
	pinstance->control_pool = NULL;
}

static int __devinit
pmcraid_allocate_cmd_blocks(struct pmcraid_instance *pinstance)
{
	int i;

	sprintf(pinstance->cmd_pool_name, "pmcraid_cmd_pool_%d",
		pinstance->host->unique_id);


	pinstance->cmd_cachep = kmem_cache_create(
					pinstance->cmd_pool_name,
					sizeof(struct pmcraid_cmd), 0,
					SLAB_HWCACHE_ALIGN, NULL);
	if (!pinstance->cmd_cachep)
		return -ENOMEM;

	for (i = 0; i < PMCRAID_MAX_CMD; i++) {
		pinstance->cmd_list[i] =
			kmem_cache_alloc(pinstance->cmd_cachep, GFP_KERNEL);
		if (!pinstance->cmd_list[i]) {
			pmcraid_release_cmd_blocks(pinstance, i);
			return -ENOMEM;
		}
	}
	return 0;
}

static int __devinit
pmcraid_allocate_control_blocks(struct pmcraid_instance *pinstance)
{
	int i;

	sprintf(pinstance->ctl_pool_name, "pmcraid_control_pool_%d",
		pinstance->host->unique_id);

	pinstance->control_pool =
		pci_pool_create(pinstance->ctl_pool_name,
				pinstance->pdev,
				sizeof(struct pmcraid_control_block),
				PMCRAID_IOARCB_ALIGNMENT, 0);

	if (!pinstance->control_pool)
		return -ENOMEM;

	for (i = 0; i < PMCRAID_MAX_CMD; i++) {
		pinstance->cmd_list[i]->ioa_cb =
			pci_pool_alloc(
				pinstance->control_pool,
				GFP_KERNEL,
				&(pinstance->cmd_list[i]->ioa_cb_bus_addr));

		if (!pinstance->cmd_list[i]->ioa_cb) {
			pmcraid_release_control_blocks(pinstance, i);
			return -ENOMEM;
		}
		memset(pinstance->cmd_list[i]->ioa_cb, 0,
			sizeof(struct pmcraid_control_block));
	}
	return 0;
}

static void
pmcraid_release_host_rrqs(struct pmcraid_instance *pinstance, int maxindex)
{
	int i;
	for (i = 0; i < maxindex; i++) {

		pci_free_consistent(pinstance->pdev,
				    HRRQ_ENTRY_SIZE * PMCRAID_MAX_CMD,
				    pinstance->hrrq_start[i],
				    pinstance->hrrq_start_bus_addr[i]);

		/* reset pointers and toggle bit to zeros */
		pinstance->hrrq_start[i] = NULL;
		pinstance->hrrq_start_bus_addr[i] = 0;
		pinstance->host_toggle_bit[i] = 0;
	}
}

static int __devinit
pmcraid_allocate_host_rrqs(struct pmcraid_instance *pinstance)
{
	int i;
	int buf_count = PMCRAID_MAX_CMD / pinstance->num_hrrq;

	for (i = 0; i < pinstance->num_hrrq; i++) {
		int buffer_size = HRRQ_ENTRY_SIZE * buf_count;

		pinstance->hrrq_start[i] =
			pci_alloc_consistent(
					pinstance->pdev,
					buffer_size,
					&(pinstance->hrrq_start_bus_addr[i]));

		if (pinstance->hrrq_start[i] == 0) {
			pmcraid_err("could not allocate host rrq: %d\n", i);
			pmcraid_release_host_rrqs(pinstance, i);
			return -ENOMEM;
		}

		memset(pinstance->hrrq_start[i], 0, buffer_size);
		pinstance->hrrq_curr[i] = pinstance->hrrq_start[i];
		pinstance->hrrq_end[i] =
			pinstance->hrrq_start[i] + buf_count - 1;
		pinstance->host_toggle_bit[i] = 1;
		spin_lock_init(&pinstance->hrrq_lock[i]);
	}
	return 0;
}

static void pmcraid_release_hcams(struct pmcraid_instance *pinstance)
{
	if (pinstance->ccn.msg != NULL) {
		pci_free_consistent(pinstance->pdev,
				    PMCRAID_AEN_HDR_SIZE +
				    sizeof(struct pmcraid_hcam_ccn),
				    pinstance->ccn.msg,
				    pinstance->ccn.baddr);

		pinstance->ccn.msg = NULL;
		pinstance->ccn.hcam = NULL;
		pinstance->ccn.baddr = 0;
	}

	if (pinstance->ldn.msg != NULL) {
		pci_free_consistent(pinstance->pdev,
				    PMCRAID_AEN_HDR_SIZE +
				    sizeof(struct pmcraid_hcam_ldn),
				    pinstance->ldn.msg,
				    pinstance->ldn.baddr);

		pinstance->ldn.msg = NULL;
		pinstance->ldn.hcam = NULL;
		pinstance->ldn.baddr = 0;
	}
}

static int pmcraid_allocate_hcams(struct pmcraid_instance *pinstance)
{
	pinstance->ccn.msg = pci_alloc_consistent(
					pinstance->pdev,
					PMCRAID_AEN_HDR_SIZE +
					sizeof(struct pmcraid_hcam_ccn),
					&(pinstance->ccn.baddr));

	pinstance->ldn.msg = pci_alloc_consistent(
					pinstance->pdev,
					PMCRAID_AEN_HDR_SIZE +
					sizeof(struct pmcraid_hcam_ldn),
					&(pinstance->ldn.baddr));

	if (pinstance->ldn.msg == NULL || pinstance->ccn.msg == NULL) {
		pmcraid_release_hcams(pinstance);
	} else {
		pinstance->ccn.hcam =
			(void *)pinstance->ccn.msg + PMCRAID_AEN_HDR_SIZE;
		pinstance->ldn.hcam =
			(void *)pinstance->ldn.msg + PMCRAID_AEN_HDR_SIZE;

		atomic_set(&pinstance->ccn.ignore, 0);
		atomic_set(&pinstance->ldn.ignore, 0);
	}

	return (pinstance->ldn.msg == NULL) ? -ENOMEM : 0;
}

static void pmcraid_release_config_buffers(struct pmcraid_instance *pinstance)
{
	if (pinstance->cfg_table != NULL &&
	    pinstance->cfg_table_bus_addr != 0) {
		pci_free_consistent(pinstance->pdev,
				    sizeof(struct pmcraid_config_table),
				    pinstance->cfg_table,
				    pinstance->cfg_table_bus_addr);
		pinstance->cfg_table = NULL;
		pinstance->cfg_table_bus_addr = 0;
	}

	if (pinstance->res_entries != NULL) {
		int i;

		for (i = 0; i < PMCRAID_MAX_RESOURCES; i++)
			list_del(&pinstance->res_entries[i].queue);
		kfree(pinstance->res_entries);
		pinstance->res_entries = NULL;
	}

	pmcraid_release_hcams(pinstance);
}

static int __devinit
pmcraid_allocate_config_buffers(struct pmcraid_instance *pinstance)
{
	int i;

	pinstance->res_entries =
			kzalloc(sizeof(struct pmcraid_resource_entry) *
				PMCRAID_MAX_RESOURCES, GFP_KERNEL);

	if (NULL == pinstance->res_entries) {
		pmcraid_err("failed to allocate memory for resource table\n");
		return -ENOMEM;
	}

	for (i = 0; i < PMCRAID_MAX_RESOURCES; i++)
		list_add_tail(&pinstance->res_entries[i].queue,
			      &pinstance->free_res_q);

	pinstance->cfg_table =
		pci_alloc_consistent(pinstance->pdev,
				     sizeof(struct pmcraid_config_table),
				     &pinstance->cfg_table_bus_addr);

	if (NULL == pinstance->cfg_table) {
		pmcraid_err("couldn't alloc DMA memory for config table\n");
		pmcraid_release_config_buffers(pinstance);
		return -ENOMEM;
	}

	if (pmcraid_allocate_hcams(pinstance)) {
		pmcraid_err("could not alloc DMA memory for HCAMS\n");
		pmcraid_release_config_buffers(pinstance);
		return -ENOMEM;
	}

	return 0;
}

static void pmcraid_init_tasklets(struct pmcraid_instance *pinstance)
{
	int i;
	for (i = 0; i < pinstance->num_hrrq; i++)
		tasklet_init(&pinstance->isr_tasklet[i],
			     pmcraid_tasklet_function,
			     (unsigned long)&pinstance->hrrq_vector[i]);
}

static void pmcraid_kill_tasklets(struct pmcraid_instance *pinstance)
{
	int i;
	for (i = 0; i < pinstance->num_hrrq; i++)
		tasklet_kill(&pinstance->isr_tasklet[i]);
}

static int __devinit pmcraid_init_buffers(struct pmcraid_instance *pinstance)
{
	int i;

	if (pmcraid_allocate_host_rrqs(pinstance)) {
		pmcraid_err("couldn't allocate memory for %d host rrqs\n",
			     pinstance->num_hrrq);
		return -ENOMEM;
	}

	if (pmcraid_allocate_config_buffers(pinstance)) {
		pmcraid_err("couldn't allocate memory for config buffers\n");
		pmcraid_release_host_rrqs(pinstance, pinstance->num_hrrq);
		return -ENOMEM;
	}

	if (pmcraid_allocate_cmd_blocks(pinstance)) {
		pmcraid_err("couldn't allocate memory for cmd blocks \n");
		pmcraid_release_config_buffers(pinstance);
		pmcraid_release_host_rrqs(pinstance, pinstance->num_hrrq);
		return -ENOMEM;
	}

	if (pmcraid_allocate_control_blocks(pinstance)) {
		pmcraid_err("couldn't allocate memory control blocks \n");
		pmcraid_release_config_buffers(pinstance);
		pmcraid_release_cmd_blocks(pinstance, PMCRAID_MAX_CMD);
		pmcraid_release_host_rrqs(pinstance, pinstance->num_hrrq);
		return -ENOMEM;
	}

	/* Initialize all the command blocks and add them to free pool. No
	 * need to lock (free_pool_lock) as this is done in initialization
	 * itself
	 */
	for (i = 0; i < PMCRAID_MAX_CMD; i++) {
		struct pmcraid_cmd *cmdp = pinstance->cmd_list[i];
		pmcraid_init_cmdblk(cmdp, i);
		cmdp->drv_inst = pinstance;
		list_add_tail(&cmdp->free_list, &pinstance->free_cmd_pool);
	}

	return 0;
}

static void pmcraid_reinit_buffers(struct pmcraid_instance *pinstance)
{
	int i;
	int buffer_size = HRRQ_ENTRY_SIZE * PMCRAID_MAX_CMD;

	for (i = 0; i < pinstance->num_hrrq; i++) {
		memset(pinstance->hrrq_start[i], 0, buffer_size);
		pinstance->hrrq_curr[i] = pinstance->hrrq_start[i];
		pinstance->hrrq_end[i] =
			pinstance->hrrq_start[i] + PMCRAID_MAX_CMD - 1;
		pinstance->host_toggle_bit[i] = 1;
	}
}

static int __devinit pmcraid_init_instance(
	struct pci_dev *pdev,
	struct Scsi_Host *host,
	void __iomem *mapped_pci_addr
)
{
	struct pmcraid_instance *pinstance =
		(struct pmcraid_instance *)host->hostdata;

	pinstance->host = host;
	pinstance->pdev = pdev;

	/* Initialize register addresses */
	pinstance->mapped_dma_addr = mapped_pci_addr;

	/* Initialize chip-specific details */
	{
		struct pmcraid_chip_details *chip_cfg = pinstance->chip_cfg;
		struct pmcraid_interrupts *pint_regs = &pinstance->int_regs;

		pinstance->ioarrin = mapped_pci_addr + chip_cfg->ioarrin;

		pint_regs->ioa_host_interrupt_reg =
			mapped_pci_addr + chip_cfg->ioa_host_intr;
		pint_regs->ioa_host_interrupt_clr_reg =
			mapped_pci_addr + chip_cfg->ioa_host_intr_clr;
		pint_regs->host_ioa_interrupt_reg =
			mapped_pci_addr + chip_cfg->host_ioa_intr;
		pint_regs->host_ioa_interrupt_clr_reg =
			mapped_pci_addr + chip_cfg->host_ioa_intr_clr;

		/* Current version of firmware exposes interrupt mask set
		 * and mask clr registers through memory mapped bar0.
		 */
		pinstance->mailbox = mapped_pci_addr + chip_cfg->mailbox;
		pinstance->ioa_status = mapped_pci_addr + chip_cfg->ioastatus;
		pint_regs->ioa_host_interrupt_mask_reg =
			mapped_pci_addr + chip_cfg->ioa_host_mask;
		pint_regs->ioa_host_interrupt_mask_clr_reg =
			mapped_pci_addr + chip_cfg->ioa_host_mask_clr;
		pint_regs->global_interrupt_mask_reg =
			mapped_pci_addr + chip_cfg->global_intr_mask;
	};

	pinstance->ioa_reset_attempts = 0;
	init_waitqueue_head(&pinstance->reset_wait_q);

	atomic_set(&pinstance->outstanding_cmds, 0);
	atomic_set(&pinstance->expose_resources, 0);

	INIT_LIST_HEAD(&pinstance->free_res_q);
	INIT_LIST_HEAD(&pinstance->used_res_q);
	INIT_LIST_HEAD(&pinstance->free_cmd_pool);
	INIT_LIST_HEAD(&pinstance->pending_cmd_pool);

	spin_lock_init(&pinstance->free_pool_lock);
	spin_lock_init(&pinstance->pending_pool_lock);
	spin_lock_init(&pinstance->resource_lock);
	mutex_init(&pinstance->aen_queue_lock);

	/* Work-queue (Shared) for deferred processing error handling */
	INIT_WORK(&pinstance->worker_q, pmcraid_worker_function);

	/* Initialize the default log_level */
	pinstance->current_log_level = pmcraid_log_level;

	/* Setup variables required for reset engine */
	pinstance->ioa_state = IOA_STATE_UNKNOWN;
	pinstance->reset_cmd = NULL;
	return 0;
}

static void pmcraid_release_buffers(struct pmcraid_instance *pinstance)
{
	pmcraid_release_config_buffers(pinstance);
	pmcraid_release_control_blocks(pinstance, PMCRAID_MAX_CMD);
	pmcraid_release_cmd_blocks(pinstance, PMCRAID_MAX_CMD);
	pmcraid_release_host_rrqs(pinstance, pinstance->num_hrrq);

}

static void pmcraid_shutdown(struct pci_dev *pdev)
{
	struct pmcraid_instance *pinstance = pci_get_drvdata(pdev);
	pmcraid_reset_bringdown(pinstance);
}


static unsigned short pmcraid_get_minor(void)
{
	int minor;

	minor = find_first_zero_bit(pmcraid_minor, sizeof(pmcraid_minor));
	__set_bit(minor, pmcraid_minor);
	return minor;
}

static void pmcraid_release_minor(unsigned short minor)
{
	__clear_bit(minor, pmcraid_minor);
}

static int pmcraid_setup_chrdev(struct pmcraid_instance *pinstance)
{
	int minor;
	int error;

	minor = pmcraid_get_minor();
	cdev_init(&pinstance->cdev, &pmcraid_fops);
	pinstance->cdev.owner = THIS_MODULE;

	error = cdev_add(&pinstance->cdev, MKDEV(pmcraid_major, minor), 1);

	if (error)
		pmcraid_release_minor(minor);
	else
		device_create(pmcraid_class, NULL, MKDEV(pmcraid_major, minor),
			      NULL, "pmcsas%u", minor);
	return error;
}

static void pmcraid_release_chrdev(struct pmcraid_instance *pinstance)
{
	pmcraid_release_minor(MINOR(pinstance->cdev.dev));
	device_destroy(pmcraid_class,
		       MKDEV(pmcraid_major, MINOR(pinstance->cdev.dev)));
	cdev_del(&pinstance->cdev);
}

static void __devexit pmcraid_remove(struct pci_dev *pdev)
{
	struct pmcraid_instance *pinstance = pci_get_drvdata(pdev);

	/* remove the management interface (/dev file) for this device */
	pmcraid_release_chrdev(pinstance);

	/* remove host template from scsi midlayer */
	scsi_remove_host(pinstance->host);

	/* block requests from mid-layer */
	scsi_block_requests(pinstance->host);

	/* initiate shutdown adapter */
	pmcraid_shutdown(pdev);

	pmcraid_disable_interrupts(pinstance, ~0);
	flush_scheduled_work();

	pmcraid_kill_tasklets(pinstance);
	pmcraid_unregister_interrupt_handler(pinstance);
	pmcraid_release_buffers(pinstance);
	iounmap(pinstance->mapped_dma_addr);
	pci_release_regions(pdev);
	scsi_host_put(pinstance->host);
	pci_disable_device(pdev);

	return;
}

#ifdef CONFIG_PM
static int pmcraid_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct pmcraid_instance *pinstance = pci_get_drvdata(pdev);

	pmcraid_shutdown(pdev);
	pmcraid_disable_interrupts(pinstance, ~0);
	pmcraid_kill_tasklets(pinstance);
	pci_set_drvdata(pinstance->pdev, pinstance);
	pmcraid_unregister_interrupt_handler(pinstance);
	pci_save_state(pdev);
	pci_disable_device(pdev);
	pci_set_power_state(pdev, pci_choose_state(pdev, state));

	return 0;
}

static int pmcraid_resume(struct pci_dev *pdev)
{
	struct pmcraid_instance *pinstance = pci_get_drvdata(pdev);
	struct Scsi_Host *host = pinstance->host;
	int rc;
	int hrrqs;

	pci_set_power_state(pdev, PCI_D0);
	pci_enable_wake(pdev, PCI_D0, 0);
	pci_restore_state(pdev);

	rc = pci_enable_device(pdev);

	if (rc) {
		dev_err(&pdev->dev, "resume: Enable device failed\n");
		return rc;
	}

	pci_set_master(pdev);

	if ((sizeof(dma_addr_t) == 4) ||
	     pci_set_dma_mask(pdev, DMA_BIT_MASK(64)))
		rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));

	if (rc == 0)
		rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));

	if (rc != 0) {
		dev_err(&pdev->dev, "resume: Failed to set PCI DMA mask\n");
		goto disable_device;
	}

	atomic_set(&pinstance->outstanding_cmds, 0);
	hrrqs = pinstance->num_hrrq;
	rc = pmcraid_register_interrupt_handler(pinstance);

	if (rc) {
		dev_err(&pdev->dev,
			"resume: couldn't register interrupt handlers\n");
		rc = -ENODEV;
		goto release_host;
	}

	pmcraid_init_tasklets(pinstance);
	pmcraid_enable_interrupts(pinstance, PMCRAID_PCI_INTERRUPTS);

	/* Start with hard reset sequence which brings up IOA to operational
	 * state as well as completes the reset sequence.
	 */
	pinstance->ioa_hard_reset = 1;

	/* Start IOA firmware initialization and bring card to Operational
	 * state.
	 */
	if (pmcraid_reset_bringup(pinstance)) {
		dev_err(&pdev->dev, "couldn't initialize IOA \n");
		rc = -ENODEV;
		goto release_tasklets;
	}

	return 0;

release_tasklets:
	pmcraid_kill_tasklets(pinstance);
	pmcraid_unregister_interrupt_handler(pinstance);

release_host:
	scsi_host_put(host);

disable_device:
	pci_disable_device(pdev);

	return rc;
}

#else

#define pmcraid_suspend NULL
#define pmcraid_resume  NULL

#endif /* CONFIG_PM */

static void pmcraid_complete_ioa_reset(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	unsigned long flags;

	spin_lock_irqsave(pinstance->host->host_lock, flags);
	pmcraid_ioa_reset(cmd);
	spin_unlock_irqrestore(pinstance->host->host_lock, flags);
	scsi_unblock_requests(pinstance->host);
	schedule_work(&pinstance->worker_q);
}

static void pmcraid_set_supported_devs(struct pmcraid_cmd *cmd)
{
	struct pmcraid_ioarcb *ioarcb = &cmd->ioa_cb->ioarcb;
	void (*cmd_done) (struct pmcraid_cmd *) = pmcraid_complete_ioa_reset;

	pmcraid_reinit_cmdblk(cmd);

	ioarcb->resource_handle = cpu_to_le32(PMCRAID_IOA_RES_HANDLE);
	ioarcb->request_type = REQ_TYPE_IOACMD;
	ioarcb->cdb[0] = PMCRAID_SET_SUPPORTED_DEVICES;
	ioarcb->cdb[1] = ALL_DEVICES_SUPPORTED;

	/* If this was called as part of resource table reinitialization due to
	 * lost CCN, it is enough to return the command block back to free pool
	 * as part of set_supported_devs completion function.
	 */
	if (cmd->drv_inst->reinit_cfg_table) {
		cmd->drv_inst->reinit_cfg_table = 0;
		cmd->release = 1;
		cmd_done = pmcraid_reinit_cfgtable_done;
	}

	/* we will be done with the reset sequence after set supported devices,
	 * setup the done function to return the command block back to free
	 * pool
	 */
	pmcraid_send_cmd(cmd,
			 cmd_done,
			 PMCRAID_SET_SUP_DEV_TIMEOUT,
			 pmcraid_timeout_handler);
	return;
}

static void pmcraid_init_res_table(struct pmcraid_cmd *cmd)
{
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	struct pmcraid_resource_entry *res, *temp;
	struct pmcraid_config_table_entry *cfgte;
	unsigned long lock_flags;
	int found, rc, i;
	LIST_HEAD(old_res);

	if (pinstance->cfg_table->flags & MICROCODE_UPDATE_REQUIRED)
		pmcraid_err("IOA requires microcode download\n");

	/* resource list is protected by pinstance->resource_lock.
	 * init_res_table can be called from probe (user-thread) or runtime
	 * reset (timer/tasklet)
	 */
	spin_lock_irqsave(&pinstance->resource_lock, lock_flags);

	list_for_each_entry_safe(res, temp, &pinstance->used_res_q, queue)
		list_move_tail(&res->queue, &old_res);

	for (i = 0; i < pinstance->cfg_table->num_entries; i++) {
		cfgte = &pinstance->cfg_table->entries[i];

		if (!pmcraid_expose_resource(cfgte))
			continue;

		found = 0;

		/* If this entry was already detected and initialized */
		list_for_each_entry_safe(res, temp, &old_res, queue) {

			rc = memcmp(&res->cfg_entry.resource_address,
				    &cfgte->resource_address,
				    sizeof(cfgte->resource_address));
			if (!rc) {
				list_move_tail(&res->queue,
						&pinstance->used_res_q);
				found = 1;
				break;
			}
		}

		/* If this is new entry, initialize it and add it the queue */
		if (!found) {

			if (list_empty(&pinstance->free_res_q)) {
				pmcraid_err("Too many devices attached\n");
				break;
			}

			found = 1;
			res = list_entry(pinstance->free_res_q.next,
					 struct pmcraid_resource_entry, queue);

			res->scsi_dev = NULL;
			res->change_detected = RES_CHANGE_ADD;
			res->reset_progress = 0;
			list_move_tail(&res->queue, &pinstance->used_res_q);
		}

		/* copy new configuration table entry details into driver
		 * maintained resource entry
		 */
		if (found) {
			memcpy(&res->cfg_entry, cfgte,
				sizeof(struct pmcraid_config_table_entry));
			pmcraid_info("New res type:%x, vset:%x, addr:%x:\n",
				 res->cfg_entry.resource_type,
				 res->cfg_entry.unique_flags1,
				 le32_to_cpu(res->cfg_entry.resource_address));
		}
	}

	/* Detect any deleted entries, mark them for deletion from mid-layer */
	list_for_each_entry_safe(res, temp, &old_res, queue) {

		if (res->scsi_dev) {
			res->change_detected = RES_CHANGE_DEL;
			res->cfg_entry.resource_handle =
				PMCRAID_INVALID_RES_HANDLE;
			list_move_tail(&res->queue, &pinstance->used_res_q);
		} else {
			list_move_tail(&res->queue, &pinstance->free_res_q);
		}
	}

	/* release the resource list lock */
	spin_unlock_irqrestore(&pinstance->resource_lock, lock_flags);
	pmcraid_set_supported_devs(cmd);
}

static void pmcraid_querycfg(struct pmcraid_cmd *cmd)
{
	struct pmcraid_ioarcb *ioarcb = &cmd->ioa_cb->ioarcb;
	struct pmcraid_ioadl_desc *ioadl = ioarcb->add_data.u.ioadl;
	struct pmcraid_instance *pinstance = cmd->drv_inst;
	int cfg_table_size = cpu_to_be32(sizeof(struct pmcraid_config_table));

	ioarcb->request_type = REQ_TYPE_IOACMD;
	ioarcb->resource_handle = cpu_to_le32(PMCRAID_IOA_RES_HANDLE);

	ioarcb->cdb[0] = PMCRAID_QUERY_IOA_CONFIG;

	/* firmware requires 4-byte length field, specified in B.E format */
	memcpy(&(ioarcb->cdb[10]), &cfg_table_size, sizeof(cfg_table_size));

	/* Since entire config table can be described by single IOADL, it can
	 * be part of IOARCB itself
	 */
	ioarcb->ioadl_bus_addr = cpu_to_le64((cmd->ioa_cb_bus_addr) +
					offsetof(struct pmcraid_ioarcb,
						add_data.u.ioadl[0]));
	ioarcb->ioadl_length = cpu_to_le32(sizeof(struct pmcraid_ioadl_desc));
	ioarcb->ioarcb_bus_addr &= ~(0x1FULL);

	ioarcb->request_flags0 |= NO_LINK_DESCS;
	ioarcb->data_transfer_length =
		cpu_to_le32(sizeof(struct pmcraid_config_table));

	ioadl = &(ioarcb->add_data.u.ioadl[0]);
	ioadl->flags = IOADL_FLAGS_LAST_DESC;
	ioadl->address = cpu_to_le64(pinstance->cfg_table_bus_addr);
	ioadl->data_len = cpu_to_le32(sizeof(struct pmcraid_config_table));

	pmcraid_send_cmd(cmd, pmcraid_init_res_table,
			 PMCRAID_INTERNAL_TIMEOUT, pmcraid_timeout_handler);
}


static int __devinit pmcraid_probe(
	struct pci_dev *pdev,
	const struct pci_device_id *dev_id
)
{
	struct pmcraid_instance *pinstance;
	struct Scsi_Host *host;
	void __iomem *mapped_pci_addr;
	int rc = PCIBIOS_SUCCESSFUL;

	if (atomic_read(&pmcraid_adapter_count) >= PMCRAID_MAX_ADAPTERS) {
		pmcraid_err
			("maximum number(%d) of supported adapters reached\n",
			 atomic_read(&pmcraid_adapter_count));
		return -ENOMEM;
	}

	atomic_inc(&pmcraid_adapter_count);
	rc = pci_enable_device(pdev);

	if (rc) {
		dev_err(&pdev->dev, "Cannot enable adapter\n");
		atomic_dec(&pmcraid_adapter_count);
		return rc;
	}

	dev_info(&pdev->dev,
		"Found new IOA(%x:%x), Total IOA count: %d\n",
		 pdev->vendor, pdev->device,
		 atomic_read(&pmcraid_adapter_count));

	rc = pci_request_regions(pdev, PMCRAID_DRIVER_NAME);

	if (rc < 0) {
		dev_err(&pdev->dev,
			"Couldn't register memory range of registers\n");
		goto out_disable_device;
	}

	mapped_pci_addr = pci_iomap(pdev, 0, 0);

	if (!mapped_pci_addr) {
		dev_err(&pdev->dev, "Couldn't map PCI registers memory\n");
		rc = -ENOMEM;
		goto out_release_regions;
	}

	pci_set_master(pdev);

	/* Firmware requires the system bus address of IOARCB to be within
	 * 32-bit addressable range though it has 64-bit IOARRIN register.
	 * However, firmware supports 64-bit streaming DMA buffers, whereas
	 * coherent buffers are to be 32-bit. Since pci_alloc_consistent always
	 * returns memory within 4GB (if not, change this logic), coherent
	 * buffers are within firmware acceptible address ranges.
	 */
	if ((sizeof(dma_addr_t) == 4) ||
	    pci_set_dma_mask(pdev, DMA_BIT_MASK(64)))
		rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));

	/* firmware expects 32-bit DMA addresses for IOARRIN register; set 32
	 * bit mask for pci_alloc_consistent to return addresses within 4GB
	 */
	if (rc == 0)
		rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));

	if (rc != 0) {
		dev_err(&pdev->dev, "Failed to set PCI DMA mask\n");
		goto cleanup_nomem;
	}

	host = scsi_host_alloc(&pmcraid_host_template,
				sizeof(struct pmcraid_instance));

	if (!host) {
		dev_err(&pdev->dev, "scsi_host_alloc failed!\n");
		rc = -ENOMEM;
		goto cleanup_nomem;
	}

	host->max_id = PMCRAID_MAX_NUM_TARGETS_PER_BUS;
	host->max_lun = PMCRAID_MAX_NUM_LUNS_PER_TARGET;
	host->unique_id = host->host_no;
	host->max_channel = PMCRAID_MAX_BUS_TO_SCAN;
	host->max_cmd_len = PMCRAID_MAX_CDB_LEN;

	/* zero out entire instance structure */
	pinstance = (struct pmcraid_instance *)host->hostdata;
	memset(pinstance, 0, sizeof(*pinstance));

	pinstance->chip_cfg =
		(struct pmcraid_chip_details *)(dev_id->driver_data);

	rc = pmcraid_init_instance(pdev, host, mapped_pci_addr);

	if (rc < 0) {
		dev_err(&pdev->dev, "failed to initialize adapter instance\n");
		goto out_scsi_host_put;
	}

	pci_set_drvdata(pdev, pinstance);

	/* Save PCI config-space for use following the reset */
	rc = pci_save_state(pinstance->pdev);

	if (rc != 0) {
		dev_err(&pdev->dev, "Failed to save PCI config space\n");
		goto out_scsi_host_put;
	}

	pmcraid_disable_interrupts(pinstance, ~0);

	rc = pmcraid_register_interrupt_handler(pinstance);

	if (rc) {
		dev_err(&pdev->dev, "couldn't register interrupt handler\n");
		goto out_scsi_host_put;
	}

	pmcraid_init_tasklets(pinstance);

	/* allocate verious buffers used by LLD.*/
	rc = pmcraid_init_buffers(pinstance);

	if (rc) {
		pmcraid_err("couldn't allocate memory blocks\n");
		goto out_unregister_isr;
	}

	/* check the reset type required */
	pmcraid_reset_type(pinstance);

	pmcraid_enable_interrupts(pinstance, PMCRAID_PCI_INTERRUPTS);

	/* Start IOA firmware initialization and bring card to Operational
	 * state.
	 */
	pmcraid_info("starting IOA initialization sequence\n");
	if (pmcraid_reset_bringup(pinstance)) {
		dev_err(&pdev->dev, "couldn't initialize IOA \n");
		rc = 1;
		goto out_release_bufs;
	}

	/* Add adapter instance into mid-layer list */
	rc = scsi_add_host(pinstance->host, &pdev->dev);
	if (rc != 0) {
		pmcraid_err("couldn't add host into mid-layer: %d\n", rc);
		goto out_release_bufs;
	}

	scsi_scan_host(pinstance->host);

	rc = pmcraid_setup_chrdev(pinstance);

	if (rc != 0) {
		pmcraid_err("couldn't create mgmt interface, error: %x\n",
			     rc);
		goto out_remove_host;
	}

	/* Schedule worker thread to handle CCN and take care of adding and
	 * removing devices to OS
	 */
	atomic_set(&pinstance->expose_resources, 1);
	schedule_work(&pinstance->worker_q);
	return rc;

out_remove_host:
	scsi_remove_host(host);

out_release_bufs:
	pmcraid_release_buffers(pinstance);

out_unregister_isr:
	pmcraid_kill_tasklets(pinstance);
	pmcraid_unregister_interrupt_handler(pinstance);

out_scsi_host_put:
	scsi_host_put(host);

cleanup_nomem:
	iounmap(mapped_pci_addr);

out_release_regions:
	pci_release_regions(pdev);

out_disable_device:
	atomic_dec(&pmcraid_adapter_count);
	pci_set_drvdata(pdev, NULL);
	pci_disable_device(pdev);
	return -ENODEV;
}

static struct pci_driver pmcraid_driver = {
	.name = PMCRAID_DRIVER_NAME,
	.id_table = pmcraid_pci_table,
	.probe = pmcraid_probe,
	.remove = pmcraid_remove,
	.suspend = pmcraid_suspend,
	.resume = pmcraid_resume,
	.shutdown = pmcraid_shutdown
};

static int __init pmcraid_init(void)
{
	dev_t dev;
	int error;

	pmcraid_info("%s Device Driver version: %s %s\n",
			 PMCRAID_DRIVER_NAME,
			 PMCRAID_DRIVER_VERSION, PMCRAID_DRIVER_DATE);

	error = alloc_chrdev_region(&dev, 0,
				    PMCRAID_MAX_ADAPTERS,
				    PMCRAID_DEVFILE);

	if (error) {
		pmcraid_err("failed to get a major number for adapters\n");
		goto out_init;
	}

	pmcraid_major = MAJOR(dev);
	pmcraid_class = class_create(THIS_MODULE, PMCRAID_DEVFILE);

	if (IS_ERR(pmcraid_class)) {
		error = PTR_ERR(pmcraid_class);
		pmcraid_err("failed to register with with sysfs, error = %x\n",
			    error);
		goto out_unreg_chrdev;
	}

	error = pmcraid_netlink_init();

	if (error)
		goto out_unreg_chrdev;

	error = pci_register_driver(&pmcraid_driver);

	if (error == 0)
		goto out_init;

	pmcraid_err("failed to register pmcraid driver, error = %x\n",
		     error);
	class_destroy(pmcraid_class);
	pmcraid_netlink_release();

out_unreg_chrdev:
	unregister_chrdev_region(MKDEV(pmcraid_major, 0), PMCRAID_MAX_ADAPTERS);

out_init:
	return error;
}

static void __exit pmcraid_exit(void)
{
	pmcraid_netlink_release();
	class_destroy(pmcraid_class);
	unregister_chrdev_region(MKDEV(pmcraid_major, 0),
				 PMCRAID_MAX_ADAPTERS);
	pci_unregister_driver(&pmcraid_driver);
}

module_init(pmcraid_init);
module_exit(pmcraid_exit);
