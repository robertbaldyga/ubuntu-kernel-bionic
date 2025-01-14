// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Hisilicon Limited, All Rights Reserved.
 * Author: Zhichang Yuan <yuanzhichang@hisilicon.com>
 * Author: Zou Rongrong <zourongrong@huawei.com>
 * Author: John Garry <john.garry@huawei.com>
 */

#include <linux/acpi.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/logic_pio.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/slab.h>

#define DRV_NAME "hisi-lpc"

/*
 * Setting this bit means each IO operation will target a different port
 * address; 0 means repeated IO operations will use the same port,
 * such as BT.
 */
#define FG_INCRADDR_LPC		0x02

struct lpc_cycle_para {
	unsigned int opflags;
	unsigned int csize; /* data length of each operation */
};

struct hisi_lpc_dev {
	spinlock_t cycle_lock;
	void __iomem  *membase;
	struct logic_pio_hwaddr *io_host;
};

/* The max IO cycle counts supported is four per operation at maximum */
#define LPC_MAX_DWIDTH	4

#define LPC_REG_STARTUP_SIGNAL		0x00
#define LPC_REG_STARTUP_SIGNAL_START	BIT(0)
#define LPC_REG_OP_STATUS		0x04
#define LPC_REG_OP_STATUS_IDLE		BIT(0)
#define LPC_REG_OP_STATUS_FINISHED	BIT(1)
#define LPC_REG_OP_LEN			0x10 /* LPC cycles count per start */
#define LPC_REG_CMD			0x14
#define LPC_REG_CMD_OP			BIT(0) /* 0: read, 1: write */
#define LPC_REG_CMD_SAMEADDR		BIT(3)
#define LPC_REG_ADDR			0x20 /* target address */
#define LPC_REG_WDATA			0x24 /* write FIFO */
#define LPC_REG_RDATA			0x28 /* read FIFO */

/* The minimal nanosecond interval for each query on LPC cycle status */
#define LPC_NSEC_PERWAIT	100

/*
 * The maximum waiting time is about 128us.  It is specific for stream I/O,
 * such as ins.
 *
 * The fastest IO cycle time is about 390ns, but the worst case will wait
 * for extra 256 lpc clocks, so (256 + 13) * 30ns = 8 us. The maximum burst
 * cycles is 16. So, the maximum waiting time is about 128us under worst
 * case.
 *
 * Choose 1300 as the maximum.
 */
#define LPC_MAX_WAITCNT		1300

/* About 10us. This is specific for single IO operations, such as inb */
#define LPC_PEROP_WAITCNT	100

static int wait_lpc_idle(unsigned char *mbase, unsigned int waitcnt)
{
	u32 status;

	do {
		status = readl(mbase + LPC_REG_OP_STATUS);
		if (status & LPC_REG_OP_STATUS_IDLE)
			return (status & LPC_REG_OP_STATUS_FINISHED) ? 0 : -EIO;
		ndelay(LPC_NSEC_PERWAIT);
	} while (--waitcnt);

	return -ETIME;
}

/*
 * hisi_lpc_target_in - trigger a series of LPC cycles for read operation
 * @lpcdev: pointer to hisi lpc device
 * @para: some parameters used to control the lpc I/O operations
 * @addr: the lpc I/O target port address
 * @buf: where the read back data is stored
 * @opcnt: how many I/O operations required, i.e. data width
 *
 * Returns 0 on success, non-zero on fail.
 */
static int hisi_lpc_target_in(struct hisi_lpc_dev *lpcdev,
			      struct lpc_cycle_para *para, unsigned long addr,
			      unsigned char *buf, unsigned long opcnt)
{
	unsigned int cmd_word;
	unsigned int waitcnt;
	unsigned long flags;
	int ret;

	if (!buf || !opcnt || !para || !para->csize || !lpcdev)
		return -EINVAL;

	cmd_word = 0; /* IO mode, Read */
	waitcnt = LPC_PEROP_WAITCNT;
	if (!(para->opflags & FG_INCRADDR_LPC)) {
		cmd_word |= LPC_REG_CMD_SAMEADDR;
		waitcnt = LPC_MAX_WAITCNT;
	}

	/* whole operation must be atomic */
	spin_lock_irqsave(&lpcdev->cycle_lock, flags);

	writel_relaxed(opcnt, lpcdev->membase + LPC_REG_OP_LEN);
	writel_relaxed(cmd_word, lpcdev->membase + LPC_REG_CMD);
	writel_relaxed(addr, lpcdev->membase + LPC_REG_ADDR);

	writel(LPC_REG_STARTUP_SIGNAL_START,
	       lpcdev->membase + LPC_REG_STARTUP_SIGNAL);

	/* whether the operation is finished */
	ret = wait_lpc_idle(lpcdev->membase, waitcnt);
	if (ret) {
		spin_unlock_irqrestore(&lpcdev->cycle_lock, flags);
		return ret;
	}

	readsb(lpcdev->membase + LPC_REG_RDATA, buf, opcnt);

	spin_unlock_irqrestore(&lpcdev->cycle_lock, flags);

	return 0;
}

/*
 * hisi_lpc_target_out - trigger a series of LPC cycles for write operation
 * @lpcdev: pointer to hisi lpc device
 * @para: some parameters used to control the lpc I/O operations
 * @addr: the lpc I/O target port address
 * @buf: where the data to be written is stored
 * @opcnt: how many I/O operations required, i.e. data width
 *
 * Returns 0 on success, non-zero on fail.
 */
static int hisi_lpc_target_out(struct hisi_lpc_dev *lpcdev,
			       struct lpc_cycle_para *para, unsigned long addr,
			       const unsigned char *buf, unsigned long opcnt)
{
	unsigned int waitcnt;
	unsigned long flags;
	u32 cmd_word;
	int ret;

	if (!buf || !opcnt || !para || !lpcdev)
		return -EINVAL;

	/* default is increasing address */
	cmd_word = LPC_REG_CMD_OP; /* IO mode, write */
	waitcnt = LPC_PEROP_WAITCNT;
	if (!(para->opflags & FG_INCRADDR_LPC)) {
		cmd_word |= LPC_REG_CMD_SAMEADDR;
		waitcnt = LPC_MAX_WAITCNT;
	}

	spin_lock_irqsave(&lpcdev->cycle_lock, flags);

	writel_relaxed(opcnt, lpcdev->membase + LPC_REG_OP_LEN);
	writel_relaxed(cmd_word, lpcdev->membase + LPC_REG_CMD);
	writel_relaxed(addr, lpcdev->membase + LPC_REG_ADDR);

	writesb(lpcdev->membase + LPC_REG_WDATA, buf, opcnt);

	writel(LPC_REG_STARTUP_SIGNAL_START,
	       lpcdev->membase + LPC_REG_STARTUP_SIGNAL);

	/* whether the operation is finished */
	ret = wait_lpc_idle(lpcdev->membase, waitcnt);

	spin_unlock_irqrestore(&lpcdev->cycle_lock, flags);

	return ret;
}

static unsigned long hisi_lpc_pio_to_addr(struct hisi_lpc_dev *lpcdev,
					  unsigned long pio)
{
	return pio - lpcdev->io_host->io_start + lpcdev->io_host->hw_start;
}

/*
 * hisi_lpc_comm_in - input the data in a single operation
 * @hostdata: pointer to the device information relevant to LPC controller
 * @pio: the target I/O port address
 * @dwidth: the data length required to read from the target I/O port
 *
 * When success, data is returned. Otherwise, ~0 is returned.
 */
static u32 hisi_lpc_comm_in(void *hostdata, unsigned long pio, size_t dwidth)
{
	struct hisi_lpc_dev *lpcdev = hostdata;
	struct lpc_cycle_para iopara;
	unsigned long addr;
	u32 rd_data = 0;
	int ret;

	if (!lpcdev || !dwidth || dwidth > LPC_MAX_DWIDTH)
		return ~0;

	addr = hisi_lpc_pio_to_addr(lpcdev, pio);

	iopara.opflags = FG_INCRADDR_LPC;
	iopara.csize = dwidth;

	ret = hisi_lpc_target_in(lpcdev, &iopara, addr,
				 (unsigned char *)&rd_data, dwidth);
	if (ret)
		return ~0;

	return le32_to_cpu(rd_data);
}

/*
 * hisi_lpc_comm_out - output the data in a single operation
 * @hostdata: pointer to the device information relevant to LPC controller
 * @pio: the target I/O port address
 * @val: a value to be output from caller, maximum is four bytes
 * @dwidth: the data width required writing to the target I/O port
 *
 * This function corresponds to out(b,w,l) only.
 */
static void hisi_lpc_comm_out(void *hostdata, unsigned long pio,
			      u32 val, size_t dwidth)
{
	struct hisi_lpc_dev *lpcdev = hostdata;
	struct lpc_cycle_para iopara;
	const unsigned char *buf;
	unsigned long addr;

	if (!lpcdev || !dwidth || dwidth > LPC_MAX_DWIDTH)
		return;

	val = cpu_to_le32(val);

	buf = (const unsigned char *)&val;
	addr = hisi_lpc_pio_to_addr(lpcdev, pio);

	iopara.opflags = FG_INCRADDR_LPC;
	iopara.csize = dwidth;

	hisi_lpc_target_out(lpcdev, &iopara, addr, buf, dwidth);
}

/*
 * hisi_lpc_comm_ins - input the data in the buffer in multiple operations
 * @hostdata: pointer to the device information relevant to LPC controller
 * @pio: the target I/O port address
 * @buffer: a buffer where read/input data bytes are stored
 * @dwidth: the data width required writing to the target I/O port
 * @count: how many data units whose length is dwidth will be read
 *
 * When success, the data read back is stored in buffer pointed by buffer.
 * Returns 0 on success, -errno otherwise.
 */
static u32 hisi_lpc_comm_ins(void *hostdata, unsigned long pio, void *buffer,
			     size_t dwidth, unsigned int count)
{
	struct hisi_lpc_dev *lpcdev = hostdata;
	unsigned char *buf = buffer;
	struct lpc_cycle_para iopara;
	unsigned long addr;

	if (!lpcdev || !buf || !count || !dwidth || dwidth > LPC_MAX_DWIDTH)
		return -EINVAL;

	iopara.opflags = 0;
	if (dwidth > 1)
		iopara.opflags |= FG_INCRADDR_LPC;
	iopara.csize = dwidth;

	addr = hisi_lpc_pio_to_addr(lpcdev, pio);

	do {
		int ret;

		ret = hisi_lpc_target_in(lpcdev, &iopara, addr, buf, dwidth);
		if (ret)
			return ret;
		buf += dwidth;
	} while (--count);

	return 0;
}

/*
 * hisi_lpc_comm_outs - output the data in the buffer in multiple operations
 * @hostdata: pointer to the device information relevant to LPC controller
 * @pio: the target I/O port address
 * @buffer: a buffer where write/output data bytes are stored
 * @dwidth: the data width required writing to the target I/O port
 * @count: how many data units whose length is dwidth will be written
 */
static void hisi_lpc_comm_outs(void *hostdata, unsigned long pio,
			       const void *buffer, size_t dwidth,
			       unsigned int count)
{
	struct hisi_lpc_dev *lpcdev = hostdata;
	struct lpc_cycle_para iopara;
	const unsigned char *buf = buffer;
	unsigned long addr;

	if (!lpcdev || !buf || !count || !dwidth || dwidth > LPC_MAX_DWIDTH)
		return;

	iopara.opflags = 0;
	if (dwidth > 1)
		iopara.opflags |= FG_INCRADDR_LPC;
	iopara.csize = dwidth;

	addr = hisi_lpc_pio_to_addr(lpcdev, pio);
	do {
		if (hisi_lpc_target_out(lpcdev, &iopara, addr, buf, dwidth))
			break;
		buf += dwidth;
	} while (--count);
}

static const struct logic_pio_host_ops hisi_lpc_ops = {
	.in = hisi_lpc_comm_in,
	.out = hisi_lpc_comm_out,
	.ins = hisi_lpc_comm_ins,
	.outs = hisi_lpc_comm_outs,
};

#ifdef CONFIG_ACPI
#define MFD_CHILD_NAME_PREFIX DRV_NAME"-"
#define MFD_CHILD_NAME_LEN (ACPI_ID_LEN + sizeof(MFD_CHILD_NAME_PREFIX) - 1)

struct hisi_lpc_mfd_cell {
	struct mfd_cell_acpi_match acpi_match;
	char name[MFD_CHILD_NAME_LEN];
	char pnpid[ACPI_ID_LEN];
};

static int hisi_lpc_acpi_xlat_io_res(struct acpi_device *adev,
				     struct acpi_device *host,
				     struct resource *res)
{
	unsigned long sys_port;
	resource_size_t len = resource_size(res);

	sys_port = logic_pio_trans_hwaddr(&host->fwnode, res->start, len);
	if (sys_port == ~0UL)
		return -EFAULT;

	res->start = sys_port;
	res->end = sys_port + len;

	return 0;
}

/*
 * hisi_lpc_acpi_set_io_res - set the resources for a child's MFD
 * @child: the device node to be updated the I/O resource
 * @hostdev: the device node associated with host controller
 * @res: double pointer to be set to the address of translated resources
 * @num_res: pointer to variable to hold the number of translated resources
 *
 * Returns 0 when successful, and a negative value for failure.
 *
 * For a given host controller, each child device will have an associated
 * host-relative address resource.  This function will return the translated
 * logical PIO addresses for each child devices resources.
 */
static int hisi_lpc_acpi_set_io_res(struct device *child,
				    struct device *hostdev,
				    const struct resource **res, int *num_res)
{
	struct acpi_device *adev;
	struct acpi_device *host;
	struct resource_entry *rentry;
	LIST_HEAD(resource_list);
	struct resource *resources;
	int count;
	int i;

	if (!child || !hostdev)
		return -EINVAL;

	host = to_acpi_device(hostdev);
	adev = to_acpi_device(child);

	if (!adev->status.present) {
		dev_dbg(child, "device is not present\n");
		return -EIO;
	}

	if (acpi_device_enumerated(adev)) {
		dev_dbg(child, "has been enumerated\n");
		return -EIO;
	}

	/*
	 * The following code segment to retrieve the resources is common to
	 * acpi_create_platform_device(), so consider a common helper function
	 * in future.
	 */
	count = acpi_dev_get_resources(adev, &resource_list, NULL, NULL);
	if (count <= 0) {
		dev_dbg(child, "failed to get resources\n");
		return count ? count : -EIO;
	}

	resources = devm_kcalloc(hostdev, count, sizeof(*resources),
				 GFP_KERNEL);
	if (!resources) {
		dev_warn(hostdev, "could not allocate memory for %d resources\n",
			 count);
		acpi_dev_free_resource_list(&resource_list);
		return -ENOMEM;
	}
	count = 0;
	list_for_each_entry(rentry, &resource_list, node)
		resources[count++] = *rentry->res;

	acpi_dev_free_resource_list(&resource_list);

	/* translate the I/O resources */
	for (i = 0; i < count; i++) {
		int ret;

		if (!(resources[i].flags & IORESOURCE_IO))
			continue;
		ret = hisi_lpc_acpi_xlat_io_res(adev, host, &resources[i]);
		if (ret) {
			dev_err(child, "translate IO range %pR failed (%d)\n",
				&resources[i], ret);
			return ret;
		}
	}
	*res = resources;
	*num_res = count;

	return 0;
}

/*
 * hisi_lpc_acpi_probe - probe children for ACPI FW
 * @hostdev: LPC host device pointer
 *
 * Returns 0 when successful, and a negative value for failure.
 *
 * Scan all child devices and create a per-device MFD with
 * logical PIO translated IO resources.
 */
static int hisi_lpc_acpi_probe(struct device *hostdev)
{
	struct acpi_device *adev = ACPI_COMPANION(hostdev);
	struct hisi_lpc_mfd_cell *hisi_lpc_mfd_cells;
	struct mfd_cell *mfd_cells;
	struct acpi_device *child;
	int size, ret, count = 0, cell_num = 0;

	list_for_each_entry(child, &adev->children, node)
		cell_num++;

	/* allocate the mfd cell and companion ACPI info, one per child */
	size = sizeof(*mfd_cells) + sizeof(*hisi_lpc_mfd_cells);
	mfd_cells = devm_kcalloc(hostdev, cell_num, size, GFP_KERNEL);
	if (!mfd_cells)
		return -ENOMEM;

	hisi_lpc_mfd_cells = (struct hisi_lpc_mfd_cell *)&mfd_cells[cell_num];
	/* Only consider the children of the host */
	list_for_each_entry(child, &adev->children, node) {
		struct mfd_cell *mfd_cell = &mfd_cells[count];
		struct hisi_lpc_mfd_cell *hisi_lpc_mfd_cell =
					&hisi_lpc_mfd_cells[count];
		struct mfd_cell_acpi_match *acpi_match =
					&hisi_lpc_mfd_cell->acpi_match;
		char *name = hisi_lpc_mfd_cell[count].name;
		char *pnpid = hisi_lpc_mfd_cell[count].pnpid;
		struct mfd_cell_acpi_match match = {
			.pnpid = pnpid,
		};

		/*
		 * For any instances of this host controller (Hip06 and Hip07
		 * are the only chipsets), we would not have multiple slaves
		 * with the same HID. And in any system we would have just one
		 * controller active. So don't worrry about MFD name clashes.
		 */
		snprintf(name, MFD_CHILD_NAME_LEN, MFD_CHILD_NAME_PREFIX"%s",
			 acpi_device_hid(child));
		snprintf(pnpid, ACPI_ID_LEN, "%s", acpi_device_hid(child));

		memcpy(acpi_match, &match, sizeof(*acpi_match));
		mfd_cell->name = name;
		mfd_cell->acpi_match = acpi_match;

		ret = hisi_lpc_acpi_set_io_res(&child->dev, &adev->dev,
					       &mfd_cell->resources,
					       &mfd_cell->num_resources);
		if (ret) {
			dev_warn(&child->dev, "set resource fail (%d)\n", ret);
			return ret;
		}
		count++;
	}

	ret = mfd_add_devices(hostdev, PLATFORM_DEVID_NONE,
			      mfd_cells, cell_num, NULL, 0, NULL);
	if (ret) {
		dev_err(hostdev, "failed to add mfd cells (%d)\n", ret);
		return ret;
	}

	return 0;
}

static const struct acpi_device_id hisi_lpc_acpi_match[] = {
	{"HISI0191"},
	{}
};
#else
static int hisi_lpc_acpi_probe(struct device *dev)
{
	return -ENODEV;
}
#endif // CONFIG_ACPI

/*
 * hisi_lpc_probe - the probe callback function for hisi lpc host,
 *		   will finish all the initialization.
 * @pdev: the platform device corresponding to hisi lpc host
 *
 * Returns 0 on success, non-zero on fail.
 */
static int hisi_lpc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct acpi_device *acpi_device = ACPI_COMPANION(dev);
	struct logic_pio_hwaddr *range;
	struct hisi_lpc_dev *lpcdev;
	resource_size_t io_end;
	struct resource *res;
	int ret;

	lpcdev = devm_kzalloc(dev, sizeof(*lpcdev), GFP_KERNEL);
	if (!lpcdev)
		return -ENOMEM;

	spin_lock_init(&lpcdev->cycle_lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	lpcdev->membase = devm_ioremap_resource(dev, res);
	if (IS_ERR(lpcdev->membase))
		return PTR_ERR(lpcdev->membase);

	range = devm_kzalloc(dev, sizeof(*range), GFP_KERNEL);
	if (!range)
		return -ENOMEM;

	range->fwnode = dev->fwnode;
	range->flags = LOGIC_PIO_INDIRECT;
	range->size = PIO_INDIRECT_SIZE;
	range->hostdata = lpcdev;
	range->ops = &hisi_lpc_ops;
	lpcdev->io_host = range;

	ret = logic_pio_register_range(range);
	if (ret) {
		dev_err(dev, "register IO range failed (%d)!\n", ret);
		return ret;
	}

	/* register the LPC host PIO resources */
	if (acpi_device)
		ret = hisi_lpc_acpi_probe(dev);
	else
		ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret) {
		logic_pio_unregister_range(range);
		return ret;
	}

	io_end = lpcdev->io_host->io_start + lpcdev->io_host->size;
	dev_info(dev, "registered range [%pa - %pa]\n",
		 &lpcdev->io_host->io_start, &io_end);

	return ret;
}

static const struct of_device_id hisi_lpc_of_match[] = {
	{ .compatible = "hisilicon,hip06-lpc", },
	{ .compatible = "hisilicon,hip07-lpc", },
	{}
};

static struct platform_driver hisi_lpc_driver = {
	.driver = {
		.name           = DRV_NAME,
		.of_match_table = hisi_lpc_of_match,
		.acpi_match_table = ACPI_PTR(hisi_lpc_acpi_match),
	},
	.probe = hisi_lpc_probe,
};
builtin_platform_driver(hisi_lpc_driver);
