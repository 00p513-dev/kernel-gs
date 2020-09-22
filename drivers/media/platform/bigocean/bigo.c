// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for BigOcean video accelerator
 *
 * Copyright 2020 Google LLC.
 *
 * Author: Vinay Kalia <vinaykalia@google.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#include "bigo_io.h"
#include "bigo_iommu.h"
#include "bigo_of.h"
#include "bigo_pm.h"
#include "bigo_priv.h"
#include "bigo_slc.h"

#define BIGO_DEVCLASS_NAME "video_codec"
#define BIGO_CHRDEV_NAME "bigocean"

#define DEFAULT_WIDTH 3840
#define DEFAULT_HEIGHT 2160
#define DEFAULT_FPS 60

inline void set_curr_inst(struct bigo_core *core, struct bigo_inst *inst)
{
	core->curr_inst = inst;
}

inline struct bigo_inst *get_curr_inst(struct bigo_core *core)
{
	return core->curr_inst;
}

static inline void on_first_instance_open(struct bigo_core *core)
{
	bigo_pt_client_enable(core);
}

static inline void on_last_inst_close(struct bigo_core *core)
{
	bigo_pt_client_disable(core);
	kfree(core->job.regs);
	core->job.regs = NULL;
}

static int bigo_open(struct inode *inode, struct file *file)
{
	int rc = 0;
	struct bigo_core *core = container_of(inode->i_cdev, struct bigo_core, cdev);
	struct bigo_inst *inst;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst) {
		rc = -ENOMEM;
		pr_err("Failed to create instance\n");
		goto err;
	}
	INIT_LIST_HEAD(&inst->list);
	INIT_LIST_HEAD(&inst->buffers);
	mutex_init(&inst->lock);
	file->private_data = inst;
	inst->height = DEFAULT_WIDTH;
	inst->width = DEFAULT_HEIGHT;
	inst->fps = DEFAULT_FPS;
	inst->core = core;
	mutex_lock(&core->lock);
	if (list_empty(&core->instances))
		on_first_instance_open(core);
	list_add_tail(&inst->list, &core->instances);
	mutex_unlock(&core->lock);
	pr_info("opened bigocean instance\n");

err:
	return rc;
}

static int bigo_release(struct inode *inode, struct file *file)
{
	struct bigo_core *core =
		container_of(inode->i_cdev, struct bigo_core, cdev);
	struct bigo_inst *inst = file->private_data;

	if (!inst || !core) {
		pr_err("No instance or core\n");
		return -EINVAL;
	}
	bigo_unmap_all(inst);
	mutex_lock(&core->lock);
	list_del(&inst->list);
	kfree(inst);
	if (list_empty(&core->instances))
		on_last_inst_close(core);
	mutex_unlock(&core->lock);
	pr_info("closed bigocean instance\n");
	return 0;
}

static void bigo_update_stats(struct bigo_core *core, void *regs)
{
	struct bigo_inst *inst = get_curr_inst(core);

	if (!inst) {
		pr_warn("%s called on NULL instance\n", __func__);
		return;
	}
	inst->avg_bw[inst->job_cnt % AVG_CNT].rd_bw =
		*(u32 *)(regs + BIGO_REG_RD_BW);
	inst->avg_bw[inst->job_cnt % AVG_CNT].wr_bw =
		*(u32 *)(regs + BIGO_REG_WR_BW);
	inst->pk_bw[inst->job_cnt % PEAK_CNT].rd_bw =
		*(u32 *)(regs + BIGO_REG_RD_BW);
	inst->pk_bw[inst->job_cnt % PEAK_CNT].wr_bw =
		*(u32 *)(regs + BIGO_REG_WR_BW);
	inst->hw_cycles[inst->job_cnt % AVG_CNT] =
		*(u32 *)(regs + BIGO_REG_HW_CYCLES);
	inst->job_cnt++;
}

static int bigo_run_job(struct bigo_core *core, struct bigo_job *job)
{
	long ret = 0;
	int rc = 0;

#if IS_ENABLED(CONFIG_PM)
	rc = pm_runtime_get_sync(core->dev);
	if (rc) {
		pr_err("failed to resume: %d\n", rc);
		return rc;
	}
#endif
	bigo_bypass_ssmt_pid(core);
	bigo_push_regs(core, job->regs);
	bigo_core_enable(core);
	ret = wait_for_completion_interruptible_timeout(&core->frame_done,
							msecs_to_jiffies(JOB_COMPLETE_TIMEOUT_MS));
	if (!ret) {
		pr_err("timed out waiting for HW: %d\n", rc);
		bigo_core_disable(core);
		rc = -ETIMEDOUT;
	} else {
		rc = (ret > 0) ? 0 : ret;
	}
	bigo_check_status(core);
	bigo_wait_disabled(core, BIGO_DISABLE_TIMEOUT_MS);
	bigo_pull_regs(core, job->regs);
	*(u32 *)(job->regs + BIGO_REG_STAT) = core->stat_with_irq;
	bigo_update_stats(core, job->regs);
#if IS_ENABLED(CONFIG_PM)
	if (pm_runtime_put_sync_suspend(core->dev))
		pr_warn("failed to suspend\n");
#endif
	return rc;
}

static int bigo_process(struct bigo_inst *inst, struct bigo_core *core,
			struct bigo_ioc_regs *desc)
{
	int rc = 0;
	struct bigo_job *job = &core->job;

	if (!desc) {
		pr_err("Invalid input\n");
		return -EINVAL;
	}
	if (desc->regs_size != core->regs_size) {
		pr_err("Register size passed from userspace(%u) is different(%u)\n",
		       (unsigned int)desc->regs_size, core->regs_size);
		return -EINVAL;
	}

	mutex_lock(&core->lock);

	if (!job->regs) {
		job->regs = kzalloc(core->regs_size, GFP_KERNEL);
		if (!job->regs) {
			rc = -ENOMEM;
			goto unlock;
		}
	}

	if (copy_from_user(job->regs, (void *)desc->regs, core->regs_size)) {
		pr_err("Failed to copy from user\n");
		rc = -EFAULT;
		goto unlock;
	}

	/*TODO(vinaykalia@): Replace this with EDF scheduler.*/
	set_curr_inst(core, inst);
	rc = bigo_run_job(core, job);
	set_curr_inst(core, NULL);
	if (rc) {
		pr_err("Error running job\n");
		goto unlock;
	}

	if (copy_to_user((void *)desc->regs, job->regs, core->regs_size)) {
		pr_err("Failed to copy to user\n");
		rc = -EFAULT;
		goto unlock;
	}

unlock:
	mutex_unlock(&core->lock);
	return rc;
}

inline void bigo_config_frmrate(struct bigo_inst *inst, __u32 frmrate)
{
	inst->fps = frmrate;
}

inline void bigo_config_frmsize(struct bigo_inst *inst,
				struct bigo_ioc_frmsize *frmsize)
{
	inst->height = frmsize->height;
	inst->width = frmsize->width;
}

static long bigo_unlocked_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct bigo_inst *inst = file->private_data;
	struct bigo_core *core =
		container_of(file->f_inode->i_cdev, struct bigo_core, cdev);
	void __user *user_desc = (void __user *)arg;
	struct bigo_ioc_regs desc;
	struct bigo_ioc_mapping mapping;
	struct bigo_ioc_frmsize frmsize;
	struct bigo_cache_info cinfo;
	int rc = 0;

	if (_IOC_TYPE(cmd) != BIGO_IOC_MAGIC) {
		pr_err("Bad IOCTL\n");
		return -EINVAL;
	}
	if (_IOC_NR(cmd) > BIGO_CMD_MAXNR) {
		pr_err("Bad IOCTL\n");
		return -EINVAL;
	}
	if (!inst || !core) {
		pr_err("No instance or core\n");
		return -EINVAL;
	}
	switch (cmd) {
	case BIGO_IOCX_PROCESS:
		if (copy_from_user(&desc, user_desc, sizeof(desc))) {
			pr_err("Failed to copy from user\n");
			return -EFAULT;
		}

		rc = bigo_process(inst, core, &desc);
		if (rc)
			pr_err("Error processing data: %d\n", rc);
		break;
	case BIGO_IOCX_MAP:
		if (copy_from_user(&mapping, user_desc, sizeof(mapping))) {
			pr_err("Failed to copy from user\n");
			return -EFAULT;
		}
		rc = bigo_map(core, inst, &mapping);
		if (rc)
			pr_err("Error mapping: %d\n", mapping.fd);
		if (copy_to_user(user_desc, &mapping, sizeof(mapping))) {
			pr_err("Failed to copy to user\n");
			rc = -EFAULT;
		}
		break;
	case BIGO_IOCX_UNMAP:
		if (copy_from_user(&mapping, user_desc, sizeof(mapping))) {
			pr_err("Failed to copy from user\n");
			return -EFAULT;
		}
		rc = bigo_unmap(inst, &mapping);
		if (rc)
			pr_err("Error un-mapping: %d\n", mapping.fd);
		break;
	case BIGO_IOCX_CONFIG_FRMRATE: {
		u32 frmrate = (u32)arg;

		bigo_config_frmrate(inst, frmrate);
		break;
	}
	case BIGO_IOCX_CONFIG_FRMSIZE:
		if (copy_from_user(&frmsize, user_desc, sizeof(frmsize))) {
			pr_err("Failed to copy from user\n");
			return -EFAULT;
		}
		bigo_config_frmsize(inst, &frmsize);
		break;
	case BIGO_IOCX_GET_CACHE_INFO:
		bigo_get_cache_info(inst->core, &cinfo);
		if (copy_to_user(user_desc, &cinfo, sizeof(cinfo))) {
			pr_err("Failed to copy to user\n");
			rc = -EFAULT;
		}
		break;
	case BIGO_IOCX_ABORT:
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static irqreturn_t bigo_isr(int irq, void *arg)
{
	struct bigo_core *core = (struct bigo_core *)arg;
	u32 bigo_stat;

	bigo_stat = bigo_core_readl(core, BIGO_REG_STAT);

	if (!(bigo_stat & BIGO_STAT_IRQ))
		return IRQ_NONE;

	core->stat_with_irq = bigo_stat;
	bigo_stat &= ~BIGO_STAT_IRQMASK;
	bigo_core_writel(core, BIGO_REG_STAT, bigo_stat);
	complete(&core->frame_done);
	return IRQ_HANDLED;
}

#if IS_ENABLED(CONFIG_PM)
static const struct dev_pm_ops bigo_pm_ops = {
	SET_RUNTIME_PM_OPS(bigo_runtime_suspend, bigo_runtime_resume, NULL)
};
#endif

static const struct file_operations bigo_fops = {
	.owner = THIS_MODULE,
	.open = bigo_open,
	.release = bigo_release,
	.unlocked_ioctl = bigo_unlocked_ioctl,
	.compat_ioctl = bigo_unlocked_ioctl,
};

static int init_chardev(struct bigo_core *core)
{
	int rc;

	cdev_init(&core->cdev, &bigo_fops);
	core->cdev.owner = THIS_MODULE;
	rc = alloc_chrdev_region(&core->devno, 0, 1, BIGO_CHRDEV_NAME);
	if (rc < 0) {
		pr_err("Failed to alloc chrdev region\n");
		goto err;
	}
	rc = cdev_add(&core->cdev, core->devno, 1);
	if (rc) {
		pr_err("Failed to register chrdev\n");
		goto err_cdev_add;
	}

	core->_class = class_create(THIS_MODULE, BIGO_DEVCLASS_NAME);
	if (IS_ERR(core->_class)) {
		rc = PTR_ERR(core->_class);
		goto err_class_create;
	}

	core->svc_dev = device_create(core->_class, NULL, core->cdev.dev, core,
				      BIGO_CHRDEV_NAME);
	if (IS_ERR(core->svc_dev)) {
		pr_err("device_create err\n");
		rc = PTR_ERR(core->svc_dev);
		goto err_device_create;
	}
	return rc;

err_device_create:
	class_destroy(core->_class);
err_class_create:
	cdev_del(&core->cdev);
err_cdev_add:
	unregister_chrdev_region(core->devno, 1);
err:
	return rc;
}

static void deinit_chardev(struct bigo_core *core)
{
	if (!core)
		return;

	device_destroy(core->_class, core->devno);
	class_destroy(core->_class);
	cdev_del(&core->cdev);
	unregister_chrdev_region(core->devno, 1);
}

static int bigo_probe(struct platform_device *pdev)
{
	int rc = 0;
	struct bigo_core *core;

	core = devm_kzalloc(&pdev->dev, sizeof(struct bigo_core), GFP_KERNEL);
	if (!core) {
		rc = -ENOMEM;
		goto err;
	}

	mutex_init(&core->lock);
	INIT_LIST_HEAD(&core->instances);
	INIT_LIST_HEAD(&core->pm.opps);
	init_completion(&core->frame_done);
	core->dev = &pdev->dev;
	platform_set_drvdata(pdev, core);

	rc = init_chardev(core);
	if (rc) {
		pr_err("Failed to initialize chardev for bigocean: %d\n", rc);
		goto err_init_chardev;
	}

	rc = bigo_of_dt_parse(core);
	if (rc) {
		pr_err("Failed to parse DT node\n");
		goto err_dt_parse;
	}

	rc = bigo_init_io(core, bigo_isr);
	if (rc < 0) {
		pr_err("failed to request irq\n");
		goto err_io;
	}

	/* TODO(vinaykalia@): Move pm_runtime_enable call somewhere else? */
	pm_runtime_enable(&pdev->dev);
	rc = bigo_pm_init(core);
	if (rc) {
		pr_err("Failed to initialize power management\n");
		goto err_io;
	}

	iovmm_set_fault_handler(&pdev->dev, bigo_iommu_fault_handler, core);
	rc = iovmm_activate(&pdev->dev);
	if (rc < 0) {
		pr_err("failed to activate iommu\n");
		goto err_iovmm;
	}

	core->slc.pt_hnd = pt_client_register(pdev->dev.of_node, (void *)core,
					      bigo_pt_resize_cb);
	if (IS_ERR(core->slc.pt_hnd)) {
		core->slc.pt_hnd = NULL;
		pr_warn("Failed to register pt_client.\n");
	}
	return rc;

err_iovmm:
	pm_runtime_disable(&pdev->dev);
err_io:
	bigo_of_dt_release(core);
err_dt_parse:
	deinit_chardev(core);
err_init_chardev:
	platform_set_drvdata(pdev, NULL);
err:
	return rc;
}

static int bigo_remove(struct platform_device *pdev)
{
	struct bigo_core *core = (struct bigo_core *)platform_get_drvdata(pdev);

	pt_client_unregister(core->slc.pt_hnd);
	pm_runtime_disable(&pdev->dev);
	bigo_of_dt_release(core);
	deinit_chardev(core);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct of_device_id bigo_dt_match[] = {
	{ .compatible = "google,bigocean" },
	{}
};

static struct platform_driver bigo_driver = {
	.probe = bigo_probe,
	.remove = bigo_remove,
	.driver = {
		.name = "bigocean",
		.owner = THIS_MODULE,
		.of_match_table = bigo_dt_match,
#ifdef CONFIG_PM
		.pm = &bigo_pm_ops,
#endif
	},
};

module_platform_driver(bigo_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vinay Kalia <vinaykalia@google.com>");
MODULE_DESCRIPTION("BigOcean driver");
