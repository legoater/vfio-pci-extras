// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO PCI driver for Intel 82576 (igb) Virtual Functions
 *
 * Copyright (C) 2025 Red Hat, Inc.
 *
 * Author: Cédric Le Goater <clg@redhat.com>
 */

#include <linux/anon_inodes.h>
#include <linux/compat.h>
#include <linux/interval_tree.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/vfio_pci_core.h>

#ifdef NO_VFIO_CHECK_PRECOPY_IOCTL
static inline int vfio_check_precopy_ioctl(struct vfio_device *device,
					   unsigned int cmd, unsigned long arg,
					   struct vfio_precopy_info *info)
{
	unsigned long minsz;

	if (cmd != VFIO_MIG_GET_PRECOPY_INFO)
		return -ENOTTY;

	minsz = offsetofend(struct vfio_precopy_info, dirty_bytes);
	if (copy_from_user(info, (void __user *)arg, minsz))
		return -EFAULT;
	if (info->argsz < minsz)
		return -EINVAL;

	return 0;
}
#endif

#ifdef NO_KZALLOC_OBJ
#define kzalloc_obj(P, GFP)    kzalloc(sizeof(P), GFP)
#define kzalloc_objs(P, N, GFP)        kcalloc(N, sizeof(P), GFP)
#endif

/*
 * Migration BAR register interface — must match QEMU's igb_common.h
 */

/* Vendor-specific PCI capability for migration BAR discovery */
#define IGB_MIG_CAP_MAGIC		0x4D494742	/* "MIGB" */
#define IGB_MIG_CAP_OFF_MAGIC		4
#define IGB_MIG_CAP_OFF_BARID		8
#define IGB_MIG_CAP_OFF_FLAGS		12

#define IGB_MIG_CAP_F_STATE		BIT(0)
#define IGB_MIG_CAP_F_DIRTY		BIT(1)
#define IGB_MIG_CAP_VERSION		1

/* MIG_CAPS register layout (read-only, offset 0x008) */
#define IGB_MIG_CAPS_MAX_RANGES_SHIFT	8
#define IGB_MIG_CAPS_MAX_RANGES_MASK	(0xfu << 8)	/* bits [11:8] */
#define IGB_MIG_CAPS_PGSIZES_MASK	0xfffff000u	/* bits [31:12] */

/* BAR register offsets */
#define IGB_MIG_DEVICE_STATE		0x000
#define IGB_MIG_STATUS			0x004
#define IGB_MIG_CAPS			0x008
#define IGB_MIG_VERSION			0x00C
#define IGB_MIG_DATA_SIZE		0x010
#define IGB_MIG_DATA_XFER		0x014
#define IGB_MIG_DATA_BUF_ADDR_LO	0x018
#define IGB_MIG_DATA_BUF_ADDR_HI	0x01C
#define IGB_MIG_DIRTY_PGSIZE		0x020
#define IGB_MIG_DIRTY_CTRL		0x024
#define IGB_MIG_DIRTY_RANGE_IOVA_LO	0x028
#define IGB_MIG_DIRTY_RANGE_IOVA_HI	0x02C
#define IGB_MIG_DIRTY_RANGE_SIZE_LO	0x030
#define IGB_MIG_DIRTY_RANGE_SIZE_HI	0x034
#define IGB_MIG_DIRTY_BUF_ADDR_LO	0x038
#define IGB_MIG_DIRTY_BUF_ADDR_HI	0x03C
#define IGB_MIG_DIRTY_STATUS		0x040

/* Migration statistics registers (read-only) */
#define IGB_MIG_STAT_DMA_WRITES		0x100
#define IGB_MIG_STAT_DMA_BYTES_LO	0x104
#define IGB_MIG_STAT_DMA_BYTES_HI	0x108
#define IGB_MIG_STAT_DIRTY_PAGES_SET	0x10C
#define IGB_MIG_STAT_DIRTY_PAGES_CLR	0x110
#define IGB_MIG_STAT_DIRTY_PAGE_COUNT	0x114
#define IGB_MIG_STAT_DIRTY_QUERY_CNT	0x118

/* Device state values */
#define IGB_MIG_STATE_ERROR		0
#define IGB_MIG_STATE_STOP		1
#define IGB_MIG_STATE_RUNNING		2
#define IGB_MIG_STATE_STOP_COPY		3
#define IGB_MIG_STATE_RESUMING		4
#define IGB_MIG_STATE_PRE_COPY		5

/* Status register bits */
#define IGB_MIG_STATUS_DATA_AVAIL	BIT(0)
#define IGB_MIG_STATUS_ERROR		BIT(1)
#define IGB_MIG_STATUS_QUIESCED		BIT(2)
#define IGB_MIG_STATUS_ERR_SHIFT	8
#define IGB_MIG_STATUS_ERR_CODE(s)	(((s) >> IGB_MIG_STATUS_ERR_SHIFT) & 0xff)

#define IGB_MIG_QUIESCE_TIMEOUT_MS	1000

/* Dirty tracking control values */
#define IGB_MIG_DIRTY_CTRL_DISABLE	0
#define IGB_MIG_DIRTY_CTRL_ENABLE	1
#define IGB_MIG_DIRTY_CTRL_QUERY	2

/* Dirty query shared buffer completion status */
#define IGB_MIG_DIRTY_STATUS_COMPLETE	1

/* DIRTY_STATUS register values (read-only, set after each DIRTY_CTRL write) */
#define IGB_MIG_DIRTY_STATUS_OK			0
#define IGB_MIG_DIRTY_STATUS_TOO_MANY_RANGES	1
#define IGB_MIG_DIRTY_STATUS_BAD_RANGE		2
#define IGB_MIG_DIRTY_STATUS_BAD_PGSIZE		3
#define IGB_MIG_DIRTY_STATUS_NOT_ENABLED	4
#define IGB_MIG_DIRTY_STATUS_NO_BUFFER		5
#define IGB_MIG_DIRTY_STATUS_DMA_FAILED		6

struct igb_mig_dirty_query {
	/* Cache line 0: request (written by driver) */
	__le64 iova;
	__le64 size;
	__le32 page_size;
	__le32 flags;
	__le32 reserved0[10];

	/* Cache line 1: completion (written by device) */
	__le32 status;
	__le32 bitmap_size;
	__le32 dirty_page_count;
	__le32 reserved1;
	__le64 dma_writes;
	__le32 reserved2[10];

	/* Cache line 2+: bitmap (written by device) */
	u8 bitmap[];
};

struct igb_pci_dirty_range {
	u64 iova;
	u64 size;
};

struct igb_migration_file {
	struct file *filp;
	/* Protects migration file data and state */
	struct mutex lock;
	bool disabled;
	u8 *mig_data;
	size_t size;
	struct igb_pci_core_device *igb_dev;
};

struct igb_pci_core_device {
	struct vfio_pci_core_device core_device;
	struct pci_dev *pdev;

	u8 dirty_tracking:1;
	int mig_cap_pos;
	void __iomem *mig_bar;
	/* protect migration state */
	struct mutex state_mutex;
	enum vfio_device_mig_state mig_state;
	struct igb_migration_file *resuming_migf;
	struct igb_migration_file *saving_migf;
	struct igb_mig_dirty_query *dirty_query;
	dma_addr_t dirty_query_dma;
	size_t dirty_query_size;
	struct igb_pci_dirty_range *dirty_ranges;
	u32 dirty_pgsizes;
	u8 max_dirty_ranges;
	u8 num_dirty_ranges;
	u64 dirty_page_size;
#ifdef CONFIG_VFIO_DEBUGFS
	struct dentry *debug_root;
#endif
};

#define MAX_MIGRATION_SIZE (256 * 1024)

static u32 mig_bar_read(struct igb_pci_core_device *dev, u32 offset)
{
	return ioread32(dev->mig_bar + offset);
}

static void mig_bar_write(struct igb_pci_core_device *dev, u32 offset, u32 val)
{
	iowrite32(val, dev->mig_bar + offset);
}

static int igb_mig_bar_wait_quiesced(struct igb_pci_core_device *igb_dev)
{
	struct pci_dev *pdev = igb_dev->core_device.pdev;
	u32 status;
	int ret;

	ret = read_poll_timeout(mig_bar_read, status,
				status & IGB_MIG_STATUS_QUIESCED, 100,
				IGB_MIG_QUIESCE_TIMEOUT_MS * 1000, false,
				igb_dev, IGB_MIG_STATUS);
	if (ret)
		dev_err(&pdev->dev, "device not quiesced (status 0x%x)\n", status);
	return ret;
}

static int igb_mig_bar_set_state(struct igb_pci_core_device *igb_dev,
				 u32 state)
{
	u32 status;

	mig_bar_write(igb_dev, IGB_MIG_DEVICE_STATE, state);
	status = mig_bar_read(igb_dev, IGB_MIG_STATUS);
	if (status & IGB_MIG_STATUS_ERROR) {
		dev_err(&igb_dev->pdev->dev,
			"migration BAR error after state transition to %u (error %u)\n",
			state, IGB_MIG_STATUS_ERR_CODE(status));
		return -EIO;
	}
	return 0;
}

static int igb_discover_mig_bar(struct pci_dev *pdev,
				struct igb_pci_core_device *igb_dev)
{
	int pos = igb_dev->mig_cap_pos;
	u32 bar_idx, flags, caps;
	int ret;

	ret = pci_read_config_dword(pdev, pos + IGB_MIG_CAP_OFF_BARID, &bar_idx);
	if (ret)
		return pcibios_err_to_errno(ret);

	ret = pci_read_config_dword(pdev, pos + IGB_MIG_CAP_OFF_FLAGS, &flags);
	if (ret)
		return pcibios_err_to_errno(ret);

	if (bar_idx >= PCI_STD_NUM_BARS) {
		dev_err(&pdev->dev, "invalid migration BAR index %u\n", bar_idx);
		return -EINVAL;
	}

	if (!(flags & IGB_MIG_CAP_F_STATE)) {
		dev_err(&pdev->dev, "missing migration cap flags 0x%x\n", flags);
		return -EINVAL;
	}

	dev_info(&pdev->dev, "migration cap found at 0x%x: bar=%u flags=0x%x\n",
		 pos, bar_idx, flags);

	igb_dev->mig_bar = pci_iomap(pdev, bar_idx, 0);
	if (!igb_dev->mig_bar) {
		dev_err(&pdev->dev, "failed to map migration BAR %u\n", bar_idx);
		return -ENOMEM;
	}

	if (mig_bar_read(igb_dev, IGB_MIG_VERSION) != IGB_MIG_CAP_VERSION) {
		dev_err(&pdev->dev, "unsupported migration BAR version %u\n",
			mig_bar_read(igb_dev, IGB_MIG_VERSION));
		pci_iounmap(pdev, igb_dev->mig_bar);
		igb_dev->mig_bar = NULL;
		return -EINVAL;
	}

	caps = mig_bar_read(igb_dev, IGB_MIG_CAPS);
	if (!(caps & IGB_MIG_CAP_F_STATE)) {
		dev_err(&pdev->dev, "missing migration BAR caps 0x%x\n", caps);
		pci_iounmap(pdev, igb_dev->mig_bar);
		igb_dev->mig_bar = NULL;
		return -EINVAL;
	}

	igb_dev->max_dirty_ranges = (caps & IGB_MIG_CAPS_MAX_RANGES_MASK)
				     >> IGB_MIG_CAPS_MAX_RANGES_SHIFT;
	if (caps & IGB_MIG_CAP_F_DIRTY) {
		if (!igb_dev->max_dirty_ranges) {
			dev_warn(&pdev->dev,
				 "F_DIRTY set but max_ranges is 0, dirty tracking disabled\n");
		} else {
			igb_dev->dirty_ranges = kzalloc_objs(*igb_dev->dirty_ranges,
							     igb_dev->max_dirty_ranges,
							     GFP_KERNEL);
			if (!igb_dev->dirty_ranges) {
				pci_iounmap(pdev, igb_dev->mig_bar);
				igb_dev->mig_bar = NULL;
				return -ENOMEM;
			}
			igb_dev->dirty_pgsizes = caps & IGB_MIG_CAPS_PGSIZES_MASK;
			dev_dbg(&pdev->dev,
				"dirty caps: max_ranges=%u pgsizes=0x%x\n",
				igb_dev->max_dirty_ranges,
				igb_dev->dirty_pgsizes);
		}
	}

	return 0;
}

static size_t igb_get_data_size(struct igb_pci_core_device *igb_dev)
{
	return mig_bar_read(igb_dev, IGB_MIG_DATA_SIZE);
}

static int igb_save_data(struct igb_pci_core_device *igb_dev,
			 void *buffer, size_t buffer_len)
{
	struct pci_dev *pdev = igb_dev->pdev;
	dma_addr_t dma_addr;
	u32 status;
	void *dma_buf;

	if (!(mig_bar_read(igb_dev, IGB_MIG_STATUS) &
	      IGB_MIG_STATUS_DATA_AVAIL)) {
		dev_err(&pdev->dev, "BAR state data not available\n");
		return -EIO;
	}

	dma_buf = kzalloc(buffer_len, GFP_KERNEL);
	if (!dma_buf)
		return -ENOMEM;

	dma_addr = dma_map_single(&pci_physfn(pdev)->dev, dma_buf, buffer_len,
				  DMA_FROM_DEVICE);
	if (dma_mapping_error(&pci_physfn(pdev)->dev, dma_addr)) {
		kfree(dma_buf);
		return -ENOMEM;
	}

	mig_bar_write(igb_dev, IGB_MIG_DATA_BUF_ADDR_LO,
		      lower_32_bits(dma_addr));
	mig_bar_write(igb_dev, IGB_MIG_DATA_BUF_ADDR_HI,
		      upper_32_bits(dma_addr));
	mig_bar_write(igb_dev, IGB_MIG_DATA_XFER, 1);

	dma_unmap_single(&pci_physfn(pdev)->dev, dma_addr, buffer_len, DMA_FROM_DEVICE);

	status = mig_bar_read(igb_dev, IGB_MIG_STATUS);
	if (status & IGB_MIG_STATUS_ERROR) {
		dev_err(&pdev->dev, "BAR state save DMA failed (error %u)\n",
			IGB_MIG_STATUS_ERR_CODE(status));
		kfree(dma_buf);
		return -EIO;
	}

	memcpy(buffer, dma_buf, buffer_len);
	kfree(dma_buf);
	dev_dbg(&pdev->dev, "saved BAR state: %zu bytes\n", buffer_len);
	return 0;
}

static int igb_load_data(struct igb_pci_core_device *igb_dev,
			 struct igb_migration_file *migf)
{
	struct pci_dev *pdev = igb_dev->pdev;
	dma_addr_t dma_addr;
	u32 status;
	void *dma_buf;

	if (!migf->size)
		return 0;

	if (migf->size > MAX_MIGRATION_SIZE) {
		dev_err(&pdev->dev, "BAR state too large (%zu)\n", migf->size);
		return -ENOSPC;
	}

	dma_buf = kzalloc(migf->size, GFP_KERNEL);
	if (!dma_buf)
		return -ENOMEM;

	memcpy(dma_buf, migf->mig_data, migf->size);

	dma_addr = dma_map_single(&pci_physfn(pdev)->dev, dma_buf, migf->size,
				  DMA_TO_DEVICE);
	if (dma_mapping_error(&pci_physfn(pdev)->dev, dma_addr)) {
		kfree(dma_buf);
		return -ENOMEM;
	}

	mig_bar_write(igb_dev, IGB_MIG_DATA_BUF_ADDR_LO,
		      lower_32_bits(dma_addr));
	mig_bar_write(igb_dev, IGB_MIG_DATA_BUF_ADDR_HI,
		      upper_32_bits(dma_addr));
	mig_bar_write(igb_dev, IGB_MIG_DATA_SIZE, migf->size);
	mig_bar_write(igb_dev, IGB_MIG_DATA_XFER, 1);

	dma_unmap_single(&pci_physfn(pdev)->dev, dma_addr, migf->size, DMA_TO_DEVICE);

	status = mig_bar_read(igb_dev, IGB_MIG_STATUS);
	if (status & IGB_MIG_STATUS_ERROR) {
		dev_err(&pdev->dev, "BAR state restore failed (error %u)\n",
			IGB_MIG_STATUS_ERR_CODE(status));
		kfree(dma_buf);
		return -EIO;
	}

	kfree(dma_buf);
	dev_dbg(&pdev->dev, "loaded BAR state: %zu bytes\n", migf->size);
	return 0;
}

static struct igb_pci_core_device *igb_drvdata(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *core_device = dev_get_drvdata(&pdev->dev);

	return container_of(core_device, struct igb_pci_core_device, core_device);
}

static void igb_disable_fd(struct igb_migration_file *migf)
{
	mutex_lock(&migf->lock);

	/* release the device states buffer */
	kvfree(migf->mig_data);
	migf->mig_data = NULL;
	migf->disabled = true;
	migf->size = 0;
	migf->filp->f_pos = 0;
	mutex_unlock(&migf->lock);
}

static int igb_release_file(struct inode *inode, struct file *filp)
{
	struct igb_migration_file *migf = filp->private_data;

	igb_disable_fd(migf);
	mutex_destroy(&migf->lock);
	kfree(migf);
	return 0;
}

static ssize_t igb_save_read(struct file *filp, char __user *buf, size_t len, loff_t *pos)
{
	struct igb_migration_file *migf = filp->private_data;
	ssize_t done = 0;
	int ret;

	if (pos)
		return -ESPIPE;
	pos = &filp->f_pos;

	mutex_lock(&migf->lock);
	if (*pos > migf->size) {
		done = -EINVAL;
		goto out_unlock;
	}

	if (migf->disabled) {
		done = -ENODEV;
		goto out_unlock;
	}

	len = min_t(size_t, migf->size - *pos, len);
	if (len) {
		ret = copy_to_user(buf, migf->mig_data + *pos, len);
		if (ret) {
			done = -EFAULT;
			goto out_unlock;
		}
		*pos += len;
		done = len;
	}

out_unlock:
	mutex_unlock(&migf->lock);
	return done;
}

static long igb_precopy_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	struct igb_migration_file *migf = filp->private_data;
	struct igb_pci_core_device *igb_dev = migf->igb_dev;
	struct vfio_precopy_info info = {};
	loff_t *pos = &filp->f_pos;
	int ret = 0;

	ret = vfio_check_precopy_ioctl(&igb_dev->core_device.vdev, cmd, arg, &info);
	if (ret)
		return ret;

	mutex_lock(&igb_dev->state_mutex);
	if (igb_dev->mig_state != VFIO_DEVICE_STATE_PRE_COPY) {
		mutex_unlock(&igb_dev->state_mutex);
		return -EINVAL;
	}

	mutex_lock(&migf->lock);
	if (migf->disabled) {
		ret = -ENODEV;
		goto out;
	}
	if (*pos > migf->size) {
		ret = -EINVAL;
		goto out;
	}
	info.initial_bytes = migf->size - *pos;
out:
	mutex_unlock(&migf->lock);
	mutex_unlock(&igb_dev->state_mutex);
	if (ret)
		return ret;
	return copy_to_user((void __user *)arg, &info,
			    offsetofend(struct vfio_precopy_info, dirty_bytes)) ? -EFAULT : 0;
}

static const struct file_operations igb_save_fops = {
	.owner = THIS_MODULE,
	.read = igb_save_read,
	.unlocked_ioctl = igb_precopy_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.release = igb_release_file,
};

static ssize_t igb_resume_write(struct file *filp, const char __user *buf,
				size_t len, loff_t *pos)
{
	struct igb_migration_file *migf = filp->private_data;
	loff_t requested_length;
	ssize_t done = 0;
	int ret;

	if (pos)
		return -ESPIPE;
	pos = &filp->f_pos;

	if (*pos < 0 || check_add_overflow((loff_t)len, *pos, &requested_length))
		return -EINVAL;

	if (requested_length > MAX_MIGRATION_SIZE)
		return -EFBIG;
	mutex_lock(&migf->lock);
	if (migf->disabled) {
		done = -ENODEV;
		goto out_unlock;
	}

	ret = copy_from_user(migf->mig_data + *pos, buf, len);
	if (ret) {
		done = -EFAULT;
		goto out_unlock;
	}
	*pos += len;
	done = len;
	migf->size += len;

out_unlock:
	mutex_unlock(&migf->lock);
	return done;
}

static const struct file_operations igb_resume_fops = {
	.owner = THIS_MODULE,
	.write = igb_resume_write,
	.release = igb_release_file,
};

static void igb_disable_fds(struct igb_pci_core_device *igb_dev)
{
	if (igb_dev->resuming_migf) {
		igb_disable_fd(igb_dev->resuming_migf);
		fput(igb_dev->resuming_migf->filp);
		igb_dev->resuming_migf = NULL;
	}

	if (igb_dev->saving_migf) {
		igb_disable_fd(igb_dev->saving_migf);
		fput(igb_dev->saving_migf->filp);
		igb_dev->saving_migf = NULL;
	}
}

static struct igb_migration_file *
igb_pci_resume_device_data(struct igb_pci_core_device *igb_dev)
{
	struct igb_migration_file *migf;
	int ret;

	migf = kzalloc_obj(*migf, GFP_KERNEL);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	migf->filp = anon_inode_getfile("igb_mig", &igb_resume_fops, migf, O_WRONLY);
	if (IS_ERR(migf->filp)) {
		int err = PTR_ERR(migf->filp);

		kfree(migf);
		return ERR_PTR(err);
	}
	stream_open(migf->filp->f_inode, migf->filp);
	mutex_init(&migf->lock);
	migf->igb_dev = igb_dev;

	/* Allocate buffer to load the device state */
	migf->mig_data = kvzalloc(MAX_MIGRATION_SIZE, GFP_KERNEL);
	if (!migf->mig_data) {
		ret = -ENOMEM;
		goto out_free;
	}

	return migf;

out_free:
	fput(migf->filp);
	return ERR_PTR(ret);
}

static int igb_populate_save_file(struct igb_pci_core_device *igb_dev,
				  struct igb_migration_file *migf)
{
	struct pci_dev *pdev = igb_dev->pdev;
	size_t size;
	int ret;

	size = igb_get_data_size(igb_dev);
	if (!size || size > MAX_MIGRATION_SIZE) {
		dev_err(&pdev->dev, "invalid or excessive state size (%zu)\n",
			size);
		return -EINVAL;
	}

	mutex_lock(&migf->lock);

	migf->mig_data = kvzalloc(size, GFP_KERNEL);
	if (!migf->mig_data) {
		mutex_unlock(&migf->lock);
		return -ENOMEM;
	}

	ret = igb_save_data(igb_dev, migf->mig_data, size);
	if (ret) {
		kvfree(migf->mig_data);
		migf->mig_data = NULL;
		mutex_unlock(&migf->lock);
		return ret;
	}

	migf->size = size;
	migf->filp->f_pos = 0;
	mutex_unlock(&migf->lock);
	return 0;
}

static struct igb_migration_file *
igb_pci_save_device_data(struct igb_pci_core_device *igb_dev)
{
	struct igb_migration_file *migf;
	int ret;

	migf = kzalloc_obj(*migf, GFP_KERNEL);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	migf->filp = anon_inode_getfile("igb_mig", &igb_save_fops, migf, O_RDONLY);
	if (IS_ERR(migf->filp)) {
		int err = PTR_ERR(migf->filp);

		kfree(migf);
		return ERR_PTR(err);
	}

	stream_open(migf->filp->f_inode, migf->filp);
	mutex_init(&migf->lock);
	migf->igb_dev = igb_dev;

	ret = igb_populate_save_file(igb_dev, migf);
	if (ret) {
		fput(migf->filp);
		return ERR_PTR(ret);
	}

	return migf;
}

static const char *vfio_device_mig_state_str(enum vfio_device_mig_state state)
{
	switch (state) {
	case VFIO_DEVICE_STATE_ERROR:
		return "VFIO_DEVICE_STATE_ERROR";
	case VFIO_DEVICE_STATE_STOP:
		return "VFIO_DEVICE_STATE_STOP";
	case VFIO_DEVICE_STATE_RUNNING:
		return "VFIO_DEVICE_STATE_RUNNING";
	case VFIO_DEVICE_STATE_STOP_COPY:
		return "VFIO_DEVICE_STATE_STOP_COPY";
	case VFIO_DEVICE_STATE_RESUMING:
		return "VFIO_DEVICE_STATE_RESUMING";
	case VFIO_DEVICE_STATE_RUNNING_P2P:
		return "VFIO_DEVICE_STATE_RUNNING_P2P";
	case VFIO_DEVICE_STATE_PRE_COPY:
		return "VFIO_DEVICE_STATE_PRE_COPY";
	case VFIO_DEVICE_STATE_PRE_COPY_P2P:
		return "VFIO_DEVICE_STATE_PRE_COPY_P2P";
	default:
		return "VFIO_DEVICE_STATE_INVALID";
	}
}

static struct file *
igb_pci_step_device_state_locked(struct igb_pci_core_device *igb_dev, u32 new)
{
	struct pci_dev *pdev = igb_dev->pdev;
	u32 cur = igb_dev->mig_state;
	int ret;

	dev_dbg(&pdev->dev, "%s => %s\n", vfio_device_mig_state_str(cur),
		vfio_device_mig_state_str(new));

	if (cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_STOP) {
		ret = igb_mig_bar_set_state(igb_dev, IGB_MIG_STATE_STOP);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_STOP_COPY) {
		struct igb_migration_file *migf;

		ret = igb_mig_bar_wait_quiesced(igb_dev);
		if (ret)
			return ERR_PTR(ret);

		ret = igb_mig_bar_set_state(igb_dev, IGB_MIG_STATE_STOP_COPY);
		if (ret)
			return ERR_PTR(ret);
		migf = igb_pci_save_device_data(igb_dev);
		if (IS_ERR(migf))
			return ERR_CAST(migf);
		get_file(migf->filp);
		igb_dev->saving_migf = migf;
		return migf->filp;
	}

	if (cur == VFIO_DEVICE_STATE_STOP_COPY && new == VFIO_DEVICE_STATE_STOP) {
		igb_disable_fds(igb_dev);
		ret = igb_mig_bar_set_state(igb_dev, IGB_MIG_STATE_STOP);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RESUMING) {
		struct igb_migration_file *migf;

		ret = igb_mig_bar_set_state(igb_dev, IGB_MIG_STATE_RESUMING);
		if (ret)
			return ERR_PTR(ret);
		migf = igb_pci_resume_device_data(igb_dev);
		if (IS_ERR(migf))
			return ERR_CAST(migf);
		get_file(migf->filp);
		igb_dev->resuming_migf = migf;
		return migf->filp;
	}

	if (cur == VFIO_DEVICE_STATE_RESUMING && new == VFIO_DEVICE_STATE_STOP) {
		ret = igb_load_data(igb_dev, igb_dev->resuming_migf);
		if (ret)
			return ERR_PTR(ret);
		igb_disable_fds(igb_dev);
		ret = igb_mig_bar_set_state(igb_dev, IGB_MIG_STATE_STOP);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RUNNING) {
		ret = igb_mig_bar_set_state(igb_dev, IGB_MIG_STATE_RUNNING);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_PRE_COPY) {
		struct igb_migration_file *migf;

		ret = igb_mig_bar_set_state(igb_dev, IGB_MIG_STATE_PRE_COPY);
		if (ret)
			return ERR_PTR(ret);
		/*
		 * Create an empty save file.  Full device state is deferred
		 * to the PRE_COPY -> STOP_COPY transition so that ring
		 * pointers are captured after the VM is paused.
		 */
		migf = kzalloc_obj(*migf, GFP_KERNEL);
		if (!migf)
			return ERR_PTR(-ENOMEM);
		migf->filp = anon_inode_getfile("igb_mig", &igb_save_fops, migf, O_RDONLY);
		if (IS_ERR(migf->filp)) {
			int err = PTR_ERR(migf->filp);

			kfree(migf);
			return ERR_PTR(err);
		}
		stream_open(migf->filp->f_inode, migf->filp);
		mutex_init(&migf->lock);
		migf->igb_dev = igb_dev;

		get_file(migf->filp);
		igb_dev->saving_migf = migf;
		return migf->filp;
	}

	if (cur == VFIO_DEVICE_STATE_PRE_COPY && new == VFIO_DEVICE_STATE_STOP_COPY) {
		struct igb_migration_file *migf = igb_dev->saving_migf;

		ret = igb_mig_bar_set_state(igb_dev, IGB_MIG_STATE_STOP_COPY);
		if (ret)
			return ERR_PTR(ret);

		ret = igb_mig_bar_wait_quiesced(igb_dev);
		if (ret)
			return ERR_PTR(ret);

		/*
		 * Now that the VM is paused, snapshot the full device state
		 * into the existing save file.  QEMU will read from the
		 * same data_fd it got during PRE_COPY.
		 */
		if (migf) {
			ret = igb_populate_save_file(igb_dev, migf);
			if (ret)
				return ERR_PTR(ret);
		}
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_PRE_COPY && new == VFIO_DEVICE_STATE_RUNNING) {
		igb_disable_fds(igb_dev);
		ret = igb_mig_bar_set_state(igb_dev, IGB_MIG_STATE_RUNNING);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	WARN_ON(true);
	return ERR_PTR(-EINVAL);
}

static struct file *
igb_pci_set_device_state(struct vfio_device *vdev,
			 enum vfio_device_mig_state new_state)
{
	struct igb_pci_core_device *igb_dev =
		container_of(vdev, struct igb_pci_core_device, core_device.vdev);
	enum vfio_device_mig_state next_state;
	struct file *res = NULL;
	int ret;

	mutex_lock(&igb_dev->state_mutex);
	while (new_state != igb_dev->mig_state) {
		ret = vfio_mig_get_next_state(vdev, igb_dev->mig_state, new_state, &next_state);
		if (ret) {
			res = ERR_PTR(-EINVAL);
			break;
		}

		res = igb_pci_step_device_state_locked(igb_dev, next_state);
		if (IS_ERR(res))
			break;
		igb_dev->mig_state = next_state;
		if (WARN_ON(res && new_state != igb_dev->mig_state)) {
			fput(res);
			res = ERR_PTR(-EINVAL);
			break;
		}
	}
	mutex_unlock(&igb_dev->state_mutex);
	return res;
}

static int igb_pci_get_device_state(struct vfio_device *vdev,
				    enum vfio_device_mig_state *curr_state)
{
	struct igb_pci_core_device *igb_dev =
		container_of(vdev, struct igb_pci_core_device, core_device.vdev);

	mutex_lock(&igb_dev->state_mutex);
	*curr_state = igb_dev->mig_state;
	mutex_unlock(&igb_dev->state_mutex);
	return 0;
}

static int igb_pci_get_data_size(struct vfio_device *vdev,
				 unsigned long *stop_copy_length)
{
	*stop_copy_length = MAX_MIGRATION_SIZE;
	return 0;
}

#ifdef CONFIG_VFIO_DEBUGFS
static void igb_debugfs_init(struct igb_pci_core_device *igb_dev);
static void igb_debugfs_exit(struct igb_pci_core_device *igb_dev);
#else
static inline void igb_debugfs_init(struct igb_pci_core_device *igb_dev) {}
static inline void igb_debugfs_exit(struct igb_pci_core_device *igb_dev) {}
#endif

static int igb_pci_open_device(struct vfio_device *core_vdev)
{
	struct igb_pci_core_device *igb_dev =
		container_of(core_vdev, struct igb_pci_core_device, core_device.vdev);
	struct vfio_pci_core_device *core_device = &igb_dev->core_device;
	int ret;

	ret = vfio_pci_core_enable(core_device);
	if (ret)
		return ret;

	igb_dev->mig_state = VFIO_DEVICE_STATE_RUNNING;
	ret = igb_discover_mig_bar(to_pci_dev(core_vdev->dev), igb_dev);
	if (ret) {
		vfio_pci_core_disable(core_device);
		return ret;
	}

	vfio_pci_core_finish_enable(core_device);
	return 0;
}

static int igb_pci_dirty_disable(struct igb_pci_core_device *igb_dev);

static void igb_close_migratable(struct igb_pci_core_device *igb_dev)
{
	mutex_lock(&igb_dev->state_mutex);
	igb_disable_fds(igb_dev);
	igb_pci_dirty_disable(igb_dev);
	mutex_unlock(&igb_dev->state_mutex);
}

static void igb_pci_close_device(struct vfio_device *core_vdev)
{
	struct igb_pci_core_device *igb_dev =
		container_of(core_vdev, struct igb_pci_core_device, core_device.vdev);

	igb_close_migratable(igb_dev);
	vfio_pci_core_close_device(core_vdev);

	kfree(igb_dev->dirty_ranges);
	igb_dev->dirty_ranges = NULL;

	if (igb_dev->mig_bar) {
		pci_iounmap(igb_dev->pdev, igb_dev->mig_bar);
		igb_dev->mig_bar = NULL;
	}
}

static int igb_find_mig_cap(struct pci_dev *pdev)
{
	int pos;
	u32 magic;

	pos = pci_find_capability(pdev, PCI_CAP_ID_VNDR);
	while (pos) {
		if (pci_read_config_dword(pdev, pos + IGB_MIG_CAP_OFF_MAGIC, &magic))
			break;
		if (magic == IGB_MIG_CAP_MAGIC)
			return pos;
		pos = pci_find_next_capability(pdev, pos, PCI_CAP_ID_VNDR);
	}
	return 0;
}

static const struct vfio_migration_ops igb_pci_mig_ops = {
	.migration_set_state = igb_pci_set_device_state,
	.migration_get_state = igb_pci_get_device_state,
	.migration_get_data_size = igb_pci_get_data_size,
};

static int igb_pci_dirty_enable(struct igb_pci_core_device *igb_dev,
				struct rb_root_cached *ranges, u32 nnodes,
				u64 *page_size)
{
	struct pci_dev *pdev = igb_dev->pdev;
	struct interval_tree_node *node;
	u32 num_ranges = nnodes;
	u64 max_range_size = 0;
	size_t alloc_size;
	u32 status;
	int i;

	if (igb_dev->dirty_tracking)
		return -EBUSY;

	if (!igb_dev->max_dirty_ranges)
		return -EOPNOTSUPP;

	/* Merge IOVA ranges if the device supports fewer than requested */
	if (num_ranges > igb_dev->max_dirty_ranges) {
		vfio_combine_iova_ranges(ranges, nnodes,
					 igb_dev->max_dirty_ranges);
		num_ranges = igb_dev->max_dirty_ranges;
	}

	/* Validate requested page size against device capabilities */
	*page_size = max_t(u64, *page_size, PAGE_SIZE);
	if (!(igb_dev->dirty_pgsizes & (1u << ilog2(*page_size)))) {
		dev_err(&igb_dev->pdev->dev,
			"unsupported dirty page size %llu (supported 0x%x)\n",
			(unsigned long long)*page_size, igb_dev->dirty_pgsizes);
		return -EINVAL;
	}
	igb_dev->dirty_page_size = *page_size;
	mig_bar_write(igb_dev, IGB_MIG_DIRTY_PGSIZE, *page_size);

	/* Shadow ranges and program them into the device */
	node = interval_tree_iter_first(ranges, 0, ULONG_MAX);
	for (i = 0; i < num_ranges && node; i++) {
		u64 start = node->start;
		u64 size = node->last - node->start + 1;

		igb_dev->dirty_ranges[i].iova = start;
		igb_dev->dirty_ranges[i].size = size;

		if (size > max_range_size)
			max_range_size = size;

		mig_bar_write(igb_dev, IGB_MIG_DIRTY_RANGE_IOVA_LO,
			      lower_32_bits(start));
		mig_bar_write(igb_dev, IGB_MIG_DIRTY_RANGE_IOVA_HI,
			      upper_32_bits(start));
		mig_bar_write(igb_dev, IGB_MIG_DIRTY_RANGE_SIZE_LO,
			      lower_32_bits(size));
		mig_bar_write(igb_dev, IGB_MIG_DIRTY_RANGE_SIZE_HI,
			      upper_32_bits(size));
		mig_bar_write(igb_dev, IGB_MIG_DIRTY_CTRL,
			      IGB_MIG_DIRTY_CTRL_ENABLE);

		status = mig_bar_read(igb_dev, IGB_MIG_DIRTY_STATUS);
		if (status != IGB_MIG_DIRTY_STATUS_OK) {
			dev_err(&pdev->dev,
				"dirty range[%d] enable failed: status=%u\n",
				i, status);
			mig_bar_write(igb_dev, IGB_MIG_DIRTY_CTRL,
				      IGB_MIG_DIRTY_CTRL_DISABLE);
			return -EINVAL;
		}

		dev_dbg(&pdev->dev,
			"dirty range[%d]: iova=0x%llx size=0x%llx pgsize=%llu\n",
			i, start, size, (unsigned long long)*page_size);

		node = interval_tree_iter_next(node, 0, ULONG_MAX);
	}
	igb_dev->num_dirty_ranges = num_ranges;

	/*
	 * Allocate the DMA shared buffer for dirty queries. Sized to
	 * hold the header plus a bitmap covering the largest range.
	 * The device reads query parameters and writes bitmap + completion
	 * status through this buffer on each DIRTY_CTRL=QUERY doorbell.
	 */
	alloc_size = sizeof(*igb_dev->dirty_query) +
		     DIV_ROUND_UP(max_range_size / *page_size, BITS_PER_BYTE);
	igb_dev->dirty_query = dma_alloc_coherent(&pci_physfn(pdev)->dev, alloc_size,
					       &igb_dev->dirty_query_dma,
					       GFP_KERNEL);
	if (!igb_dev->dirty_query)
		return -ENOMEM;
	igb_dev->dirty_query_size = alloc_size;

	mig_bar_write(igb_dev, IGB_MIG_DIRTY_BUF_ADDR_LO,
		      lower_32_bits(igb_dev->dirty_query_dma));
	mig_bar_write(igb_dev, IGB_MIG_DIRTY_BUF_ADDR_HI,
		      upper_32_bits(igb_dev->dirty_query_dma));

	igb_dev->dirty_tracking = true;
	return 0;
}

static int igb_pci_dirty_disable(struct igb_pci_core_device *igb_dev)
{
	struct pci_dev *pdev = igb_dev->pdev;

	if (!igb_dev->dirty_tracking)
		return 0;

	mig_bar_write(igb_dev, IGB_MIG_DIRTY_CTRL, IGB_MIG_DIRTY_CTRL_DISABLE);

	if (igb_dev->dirty_query) {
		mig_bar_write(igb_dev, IGB_MIG_DIRTY_BUF_ADDR_LO, 0);
		mig_bar_write(igb_dev, IGB_MIG_DIRTY_BUF_ADDR_HI, 0);
		dma_free_coherent(&pci_physfn(pdev)->dev, igb_dev->dirty_query_size,
				  igb_dev->dirty_query,
				  igb_dev->dirty_query_dma);
		igb_dev->dirty_query = NULL;
		igb_dev->dirty_query_size = 0;
	}

	igb_dev->num_dirty_ranges = 0;
	igb_dev->dirty_tracking = false;
	dev_dbg(&pdev->dev, "stop dirty page tracking\n");
	return 0;
}

static bool igb_pci_dirty_range_valid(struct igb_pci_core_device *igb_dev,
				      unsigned long iova, unsigned long length)
{
	int i;

	for (i = 0; i < igb_dev->num_dirty_ranges; i++) {
		struct igb_pci_dirty_range *r = &igb_dev->dirty_ranges[i];

		if (iova >= r->iova && iova + length <= r->iova + r->size)
			return true;
	}
	return false;
}

static int igb_pci_dirty_sync(struct igb_pci_core_device *igb_dev,
			      struct iova_bitmap *dirty_bitmap,
			      unsigned long iova, unsigned long length)
{
	struct pci_dev *pdev = igb_dev->pdev;
	u64 pgsize = igb_dev->dirty_page_size;
	struct igb_mig_dirty_query *query = igb_dev->dirty_query;
	unsigned long nbits;
	u32 dirty_pages = 0, i;
	__le32 status_val;

	if (!igb_dev->dirty_tracking)
		return -EINVAL;

	if (!igb_pci_dirty_range_valid(igb_dev, iova, length)) {
		dev_err(&pdev->dev,
			"dirty sync outside tracked range: iova=0x%lx length=0x%lx\n",
			iova, length);
		return -EINVAL;
	}

	dev_dbg(&pdev->dev, "dirty_sync iova=0x%lx length=0x%lx\n", iova, length);

	nbits = length / pgsize;

	/* Fill request fields in the DMA shared buffer */
	query->iova = cpu_to_le64(iova);
	query->size = cpu_to_le64(length);
	query->page_size = cpu_to_le32(pgsize);
	query->status = 0;
	memset(query->bitmap, 0, DIV_ROUND_UP(nbits, BITS_PER_BYTE));

	/* Ensure request is visible before the doorbell */
	wmb();
	mig_bar_write(igb_dev, IGB_MIG_DIRTY_CTRL, IGB_MIG_DIRTY_CTRL_QUERY);
	/* Ensure we read the device's response after the doorbell returns */
	rmb();

	if (read_poll_timeout(READ_ONCE, status_val,
			      le32_to_cpu(status_val) == IGB_MIG_DIRTY_STATUS_COMPLETE,
			      100, 1000000, false, query->status)) {
		dev_err(&pdev->dev, "dirty query timeout\n");
		return -ETIMEDOUT;
	}

	/* Check DIRTY_STATUS for device-side errors */
	status_val = mig_bar_read(igb_dev, IGB_MIG_DIRTY_STATUS);
	if (status_val != IGB_MIG_DIRTY_STATUS_OK) {
		dev_err(&pdev->dev, "dirty query failed: status=%u\n", status_val);
		return -EIO;
	}

	/* Ensure bitmap DMA writes are visible before reading */
	rmb();

	/* Report dirty pages from the device bitmap to VFIO core */
	for_each_set_bit(i, (unsigned long *)query->bitmap, nbits) {
		unsigned long dirty_iova = iova + (unsigned long)i * pgsize;

		dirty_pages++;
		iova_bitmap_set(dirty_bitmap, dirty_iova, pgsize);
	}

	dev_dbg(&pdev->dev,
		"dirty sync iova=%lx size=%lu dirty_pages=%u/%u dma_writes=%llu\n",
		iova, length, dirty_pages,
		le32_to_cpu(query->dirty_page_count),
		le64_to_cpu(query->dma_writes));

	return 0;
}

static int igb_pci_dma_log_read_and_clear(struct vfio_device *core_vdev,
					  unsigned long iova,
					  unsigned long length,
					  struct iova_bitmap *dirty)
{
	struct igb_pci_core_device *igb_dev =
		container_of(core_vdev, struct igb_pci_core_device, core_device.vdev);
	int ret;

	mutex_lock(&igb_dev->state_mutex);
	ret = igb_pci_dirty_sync(igb_dev, dirty, iova, length);
	mutex_unlock(&igb_dev->state_mutex);
	return ret;
}

static int igb_pci_dma_log_start(struct vfio_device *core_vdev,
				 struct rb_root_cached *ranges, u32 nnodes,
				 u64 *page_size)
{
	struct igb_pci_core_device *igb_dev =
		container_of(core_vdev, struct igb_pci_core_device, core_device.vdev);
	int ret;

	mutex_lock(&igb_dev->state_mutex);
	ret = igb_pci_dirty_enable(igb_dev, ranges, nnodes, page_size);
	mutex_unlock(&igb_dev->state_mutex);
	return ret;
}

static int igb_pci_dma_log_stop(struct vfio_device *core_vdev)
{
	struct igb_pci_core_device *igb_dev =
		container_of(core_vdev, struct igb_pci_core_device, core_device.vdev);
	int ret;

	mutex_lock(&igb_dev->state_mutex);
	ret = igb_pci_dirty_disable(igb_dev);
	mutex_unlock(&igb_dev->state_mutex);
	return ret;
}

/*
 * Hide migration BAR (BAR2) from userspace — it is a host-only
 * interface between this driver and the emulated device.
 */
static bool igb_is_mig_bar_region(unsigned int index)
{
	return index == VFIO_PCI_BAR2_REGION_INDEX;
}

static int igb_pci_get_region_info(struct vfio_device *core_vdev,
				   struct vfio_region_info *info,
				   struct vfio_info_cap *caps)
{
	if (igb_is_mig_bar_region(info->index)) {
		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		info->size = 0;
		info->flags = 0;
		return 0;
	}
	return vfio_pci_ioctl_get_region_info(core_vdev, info, caps);
}

static ssize_t igb_pci_read(struct vfio_device *core_vdev, char __user *buf,
			    size_t count, loff_t *ppos)
{
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);

	if (igb_is_mig_bar_region(index))
		return -EINVAL;

	return vfio_pci_core_read(core_vdev, buf, count, ppos);
}

static ssize_t igb_pci_write(struct vfio_device *core_vdev,
			     const char __user *buf,
			     size_t count, loff_t *ppos)
{
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);

	if (igb_is_mig_bar_region(index))
		return -EINVAL;

	return vfio_pci_core_write(core_vdev, buf, count, ppos);
}

static int igb_pci_mmap(struct vfio_device *core_vdev,
			struct vm_area_struct *vma)
{
	unsigned int index;

	index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);
	if (igb_is_mig_bar_region(index))
		return -EINVAL;

	return vfio_pci_core_mmap(core_vdev, vma);
}

static const struct vfio_log_ops igb_pci_log_ops = {
	.log_start = igb_pci_dma_log_start,
	.log_stop = igb_pci_dma_log_stop,
	.log_read_and_clear = igb_pci_dma_log_read_and_clear,
};

#ifdef CONFIG_VFIO_DEBUGFS

static int igb_stats_show(struct seq_file *s, void *unused)
{
	struct igb_pci_core_device *igb_dev = s->private;
	u32 lo, hi;

	mutex_lock(&igb_dev->state_mutex);
	if (!igb_dev->mig_bar) {
		seq_puts(s, "migration BAR not mapped\n");
		goto out;
	}

	lo = mig_bar_read(igb_dev, IGB_MIG_STAT_DMA_BYTES_LO);
	hi = mig_bar_read(igb_dev, IGB_MIG_STAT_DMA_BYTES_HI);

	seq_printf(s, "dma_writes:       %u\n",
		   mig_bar_read(igb_dev, IGB_MIG_STAT_DMA_WRITES));
	seq_printf(s, "dma_bytes:        %llu\n",
		   ((u64)hi << 32) | lo);
	seq_printf(s, "dirty_pages_set:  %u\n",
		   mig_bar_read(igb_dev, IGB_MIG_STAT_DIRTY_PAGES_SET));
	seq_printf(s, "dirty_pages_clr:  %u\n",
		   mig_bar_read(igb_dev, IGB_MIG_STAT_DIRTY_PAGES_CLR));
	seq_printf(s, "dirty_page_count: %u\n",
		   mig_bar_read(igb_dev, IGB_MIG_STAT_DIRTY_PAGE_COUNT));
	seq_printf(s, "dirty_query_cnt:  %u\n",
		   mig_bar_read(igb_dev, IGB_MIG_STAT_DIRTY_QUERY_CNT));
out:
	mutex_unlock(&igb_dev->state_mutex);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(igb_stats);

static void igb_debugfs_init(struct igb_pci_core_device *igb_dev)
{
	struct dentry *migration_dir;

	migration_dir = debugfs_lookup("migration",
				       igb_dev->core_device.vdev.debug_root);
	if (!migration_dir)
		return;

	igb_dev->debug_root = debugfs_create_dir("dirty", migration_dir);
	dput(migration_dir);

	debugfs_create_file("stats", 0444, igb_dev->debug_root,
			    igb_dev, &igb_stats_fops);
}

static void igb_debugfs_exit(struct igb_pci_core_device *igb_dev)
{
	debugfs_remove_recursive(igb_dev->debug_root);
	igb_dev->debug_root = NULL;
}
#endif

static int igb_vfio_pci_init_dev(struct vfio_device *core_vdev)
{
	struct igb_pci_core_device *igb_dev =
		container_of(core_vdev, struct igb_pci_core_device, core_device.vdev);
	struct pci_dev *pdev = to_pci_dev(core_vdev->dev);
	int ret = -EINVAL;

	if (!pdev->is_virtfn) {
		dev_err(&pdev->dev, "not a VF\n");
		return ret;
	}

	igb_dev->mig_cap_pos = igb_find_mig_cap(pdev);
	if (!igb_dev->mig_cap_pos) {
		dev_err(&pdev->dev, "no migration cap found\n");
		return ret;
	}

	dev_dbg(&pdev->dev, "init: vf_id=%d mig_cap_pos=0x%x\n",
		pci_iov_vf_id(pdev), igb_dev->mig_cap_pos);

	mutex_init(&igb_dev->state_mutex);

	core_vdev->migration_flags = VFIO_MIGRATION_STOP_COPY | VFIO_MIGRATION_PRE_COPY;
	core_vdev->mig_ops = &igb_pci_mig_ops;
	core_vdev->log_ops = &igb_pci_log_ops;
	return vfio_pci_core_init_dev(core_vdev);
}

static const struct vfio_device_ops igb_vfio_pci_ops = {
	.name = "igb-vfio-pci",
	.init = igb_vfio_pci_init_dev,
	.release = vfio_pci_core_release_dev,
	.open_device = igb_pci_open_device,
	.close_device = igb_pci_close_device,
	.ioctl = vfio_pci_core_ioctl,
	.get_region_info_caps = igb_pci_get_region_info,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read = igb_pci_read,
	.write = igb_pci_write,
	.mmap = igb_pci_mmap,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
	.match_token_uuid = vfio_pci_core_match_token_uuid,
	.bind_iommufd = vfio_iommufd_physical_bind,
	.unbind_iommufd	= vfio_iommufd_physical_unbind,
	.attach_ioas = vfio_iommufd_physical_attach_ioas,
	.detach_ioas = vfio_iommufd_physical_detach_ioas,
};

static int igb_vfio_pci_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	struct igb_pci_core_device *igb_dev;
	int ret;

	igb_dev = vfio_alloc_device(igb_pci_core_device, core_device.vdev,
				    &pdev->dev, &igb_vfio_pci_ops);
	if (IS_ERR(igb_dev))
		return PTR_ERR(igb_dev);

	dev_set_drvdata(&pdev->dev, &igb_dev->core_device);
	igb_dev->pdev = pdev;

	ret = vfio_pci_core_register_device(&igb_dev->core_device);
	if (ret)
		goto out_put_vdev;

	igb_debugfs_init(igb_dev);
	return 0;

out_put_vdev:
	vfio_put_device(&igb_dev->core_device.vdev);
	return ret;
}

static void igb_vfio_pci_remove(struct pci_dev *pdev)
{
	struct igb_pci_core_device *igb_dev = igb_drvdata(pdev);

	igb_debugfs_exit(igb_dev);
	vfio_pci_core_unregister_device(&igb_dev->core_device);
	vfio_put_device(&igb_dev->core_device.vdev);
}

static const struct pci_device_id igb_vfio_pci_table[] = {
	/* Intel Corporation 82576 Gigabit Network Connection */
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_INTEL, 0x10ca) },
	{}
};

MODULE_DEVICE_TABLE(pci, igb_vfio_pci_table);

static struct pci_driver igb_vfio_pci_driver = {
	.name = "igb-vfio-pci",
	.id_table = igb_vfio_pci_table,
	.probe = igb_vfio_pci_probe,
	.remove = igb_vfio_pci_remove,
	.driver_managed_dma = true,
};

module_pci_driver(igb_vfio_pci_driver);

MODULE_IMPORT_NS("IOMMUFD");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cédric Le Goater <clg@redhat.com>");
MODULE_DESCRIPTION("VFIO PCI - Intel 82576 Virtual Function");
