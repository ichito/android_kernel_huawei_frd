/*
 * Copyright (C) 2008, 2009 Provigent Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Driver for the ARM PrimeCell(tm) General Purpose Input/Output (PL061)
 *
 * Data sheet: ARM DDI 0190B, September 2000
 */
#include <linux/spinlock.h>
#include <linux/hwspinlock.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/bitops.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/device.h>
#include <linux/amba/bus.h>
#include <linux/amba/pl061.h>
#include <linux/slab.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm.h>
#include <linux/of_address.h>
#include "../hisi/tzdriver/tui.h"

#define GPIODATA 0x3fc
#define GPIODIR 0x400
#define GPIOIS  0x404
#define GPIOIBE 0x408
#define GPIOIEV 0x40C
#define GPIOIE  0x410
/*lint -e750 -esym(750,*)*/
#define GPIORIS 0x414
/*lint -e750 +esym(750,*)*/
#define GPIOMIS 0x418
#define GPIOIC  0x41C

#define PL061_GPIO_NR	8

#define	GPIO_HWLOCK_ID	1
#define	LOCK_TIMEOUT	1000

struct hwspinlock	*gpio_hwlock;

struct pl061_context_save_regs {
	u8 gpio_data;
	u8 gpio_dir;
	u8 gpio_is;
	u8 gpio_ibe;
	u8 gpio_iev;
	u8 gpio_ie;
};

struct pl061_gpio {
	spinlock_t			lock;
	int				sec_status;
	void __iomem		*base;
	struct irq_domain	*domain;
	struct gpio_chip	gc;
	struct pl061_context_save_regs csave_regs;
};

static int pl061_check_security_status(struct pl061_gpio *chip)
{
		WARN(chip->sec_status, "%s controller is busy", dev_name(chip->gc.dev));
		return chip->sec_status;
}

static int pl061_gpio_request(struct gpio_chip *chip, unsigned offset)
{
	/*
	 * Map back to global GPIO space and request muxing, the direction
	 * parameter does not matter for this controller.
	 */
	struct pl061_gpio *pl061_chip = container_of(chip, struct pl061_gpio, gc);
	int gpio = chip->base + offset;

	if (pl061_check_security_status(pl061_chip))
		return -EBUSY;

	return pinctrl_request_gpio(gpio);
}

static void pl061_gpio_free(struct gpio_chip *chip, unsigned offset)
{
	struct pl061_gpio *pl061_chip = container_of(chip, struct pl061_gpio, gc);
	int gpio = chip->base + offset;

	if (pl061_check_security_status(pl061_chip))
		return;

	pinctrl_free_gpio(gpio);
}

static int pl061_direction_input(struct gpio_chip *gc, unsigned offset)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);
	unsigned long flags;
	unsigned char gpiodir;

	if (offset >= gc->ngpio)
		return -EINVAL;

	if (pl061_check_security_status(chip))
		return -EBUSY;

	spin_lock_irqsave(&chip->lock, flags);

	if (hwspin_lock_timeout(gpio_hwlock, LOCK_TIMEOUT)) {
		pr_err("%s: hwspinlock timeout!\n", __func__);
		spin_unlock_irqrestore(&chip->lock, flags);
		return -EBUSY;
	}

	gpiodir = readb(chip->base + GPIODIR);
	gpiodir &= ~(1 << offset);
	writeb(gpiodir, chip->base + GPIODIR);

	hwspin_unlock(gpio_hwlock);
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int pl061_direction_output(struct gpio_chip *gc, unsigned offset,
		int value)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);
	unsigned long flags;
	unsigned char gpiodir;

	if (offset >= gc->ngpio)
		return -EINVAL;

	if (pl061_check_security_status(chip))
		return -EBUSY;

	spin_lock_irqsave(&chip->lock, flags);

	if (hwspin_lock_timeout(gpio_hwlock, LOCK_TIMEOUT)) {
		pr_err("%s: hwspinlock timeout!\n", __func__);
		spin_unlock_irqrestore(&chip->lock, flags);
		return -EBUSY;
	}

	writeb(!!value << offset, chip->base + (1 << (offset + 2)));
	gpiodir = readb(chip->base + GPIODIR);
	gpiodir |= 1 << offset;
	writeb(gpiodir, chip->base + GPIODIR);

	/*
	 * gpio value is set again, because pl061 doesn't allow to set value of
	 * a gpio pin before configuring it in OUT mode.
	 */
	writeb(!!value << offset, chip->base + (1 << (offset + 2)));
	hwspin_unlock(gpio_hwlock);
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int pl061_get_value(struct gpio_chip *gc, unsigned offset)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);

	if (pl061_check_security_status(chip))
		return -EBUSY;

	return !!readb(chip->base + (1 << (offset + 2)));
}

static void pl061_set_value(struct gpio_chip *gc, unsigned offset, int value)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);

	if (pl061_check_security_status(chip))
		return;

	writeb(!!value << offset, chip->base + (1 << (offset + 2)));
}

static int pl061_to_irq(struct gpio_chip *gc, unsigned offset)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);

	if (pl061_check_security_status(chip))
		return -EBUSY;

	return irq_create_mapping(chip->domain, offset);
}

static int pl061_irq_type(struct irq_data *d, unsigned trigger)
{
	struct pl061_gpio *chip = irq_data_get_irq_chip_data(d);
	int offset = irqd_to_hwirq(d);
	unsigned long flags;
	u8 gpiois, gpioibe, gpioiev;

	if (offset < 0 || offset >= PL061_GPIO_NR)
		return -EINVAL;

	if (pl061_check_security_status(chip))
		return -EBUSY;

	spin_lock_irqsave(&chip->lock, flags);

	if (hwspin_lock_timeout(gpio_hwlock, LOCK_TIMEOUT)) {
		pr_err("%s: hwspinlock timeout!\n", __func__);
		spin_unlock_irqrestore(&chip->lock, flags);
		return -EBUSY;
	}

	gpioiev = readb(chip->base + GPIOIEV);

	gpiois = readb(chip->base + GPIOIS);
	if (trigger & (IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW)) {
		gpiois |= 1 << offset;
		if (trigger & IRQ_TYPE_LEVEL_HIGH)
			gpioiev |= 1 << offset;
		else
			gpioiev &= ~(1 << offset);
	} else
		gpiois &= ~(1 << offset);
	writeb(gpiois, chip->base + GPIOIS);

	gpioibe = readb(chip->base + GPIOIBE);
	if ((trigger & IRQ_TYPE_EDGE_BOTH) == IRQ_TYPE_EDGE_BOTH)
		gpioibe |= 1 << offset;
	else {
		gpioibe &= ~(1 << offset);
		if (trigger & IRQ_TYPE_EDGE_RISING)
			gpioiev |= 1 << offset;
		else if (trigger & IRQ_TYPE_EDGE_FALLING)
			gpioiev &= ~(1 << offset);
	}
	writeb(gpioibe, chip->base + GPIOIBE);

	writeb(gpioiev, chip->base + GPIOIEV);

	hwspin_unlock(gpio_hwlock);
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static void pl061_irq_handler(unsigned irq, struct irq_desc *desc)
{
	unsigned long pending;
	int offset;
	struct pl061_gpio *chip = irq_desc_get_handler_data(desc);
	struct irq_chip *irqchip = irq_desc_get_chip(desc);

	if (pl061_check_security_status(chip))
		return;

	chained_irq_enter(irqchip, desc);

	pending = readb(chip->base + GPIOMIS);
	writeb(pending, chip->base + GPIOIC);
	if (pending) {
		for_each_set_bit(offset, &pending, PL061_GPIO_NR)
			generic_handle_irq(pl061_to_irq(&chip->gc, offset));
	}

	chained_irq_exit(irqchip, desc);
}

static void pl061_irq_mask(struct irq_data *d)
{
	struct pl061_gpio *chip = irq_data_get_irq_chip_data(d);
	u8 mask = 1 << (irqd_to_hwirq(d) % PL061_GPIO_NR);
	u8 gpioie;
	unsigned long flags;

	if (pl061_check_security_status(chip))
		return;

	spin_lock_irqsave(&chip->lock, flags);

	if (hwspin_lock_timeout(gpio_hwlock, LOCK_TIMEOUT)) {
		pr_err("%s: hwspinlock timeout!\n", __func__);
		spin_unlock_irqrestore(&chip->lock, flags);
		return;
	}

	gpioie = readb(chip->base + GPIOIE) & ~mask;
	writeb(gpioie, chip->base + GPIOIE);

	hwspin_unlock(gpio_hwlock);
	spin_unlock_irqrestore(&chip->lock, flags);
}

static void pl061_irq_unmask(struct irq_data *d)
{
	struct pl061_gpio *chip = irq_data_get_irq_chip_data(d);
	u8 mask = 1 << (irqd_to_hwirq(d) % PL061_GPIO_NR);
	u8 gpioie;
	unsigned long flags;

	if (pl061_check_security_status(chip))
		return;

	spin_lock_irqsave(&chip->lock, flags);

	if (hwspin_lock_timeout(gpio_hwlock, LOCK_TIMEOUT)) {
		pr_err("%s: hwspinlock timeout!\n!", __func__);
		spin_unlock_irqrestore(&chip->lock, flags);
		return ;
	}

	gpioie = readb(chip->base + GPIOIE) | mask;
	writeb(gpioie, chip->base + GPIOIE);

	hwspin_unlock(gpio_hwlock);
	spin_unlock_irqrestore(&chip->lock, flags);
}

static int pl061_irq_set_wake(struct irq_data *d, unsigned int on)
{
	return 0;
}

static struct irq_chip pl061_irqchip = {
	.name		= "pl061 gpio",
	.irq_mask	= pl061_irq_mask,
	.irq_unmask	= pl061_irq_unmask,
	.irq_disable	= pl061_irq_mask,
	.irq_enable	= pl061_irq_unmask,
	.irq_set_type	= pl061_irq_type,
	.irq_set_wake	= pl061_irq_set_wake,
};

static int pl061_irq_map(struct irq_domain *d, unsigned int virq,
			 irq_hw_number_t hw)
{
	struct pl061_gpio *chip = d->host_data;

	irq_set_chip_and_handler_name(virq, &pl061_irqchip, handle_simple_irq,
				      "pl061");
	irq_set_chip_data(virq, chip);
	irq_set_irq_type(virq, IRQ_TYPE_NONE);

	return 0;
}

static const struct irq_domain_ops pl061_domain_ops = {
	.map	= pl061_irq_map,
	.xlate	= irq_domain_xlate_twocell,
};

/* Parse gpio base from DT */
static int pl061_parse_gpio_base(struct device *dev)
{
	struct device_node *np = dev->of_node;
	int ret = -EINVAL;

	if (of_property_read_u32(np, "linux,gpio-base", &ret))
		return -ENOENT;
	if (ret >= 0)
		return ret;
	return -EINVAL;
}

static int pl061_tui_request(struct device *dev)
{
	struct pl061_gpio *chip = dev_get_drvdata(dev);
	unsigned long flags;

	pr_debug("%s: is switching sec status\n", dev_name(dev));

	spin_lock_irqsave(&chip->lock, flags);

	chip->sec_status = 1;
	chip->csave_regs.gpio_dir = readb(chip->base + GPIODIR);
	chip->csave_regs.gpio_data = readb(chip->base + GPIODATA);
	chip->csave_regs.gpio_is = readb(chip->base + GPIOIS);
	chip->csave_regs.gpio_ibe = readb(chip->base + GPIOIBE);
	chip->csave_regs.gpio_iev = readb(chip->base + GPIOIEV);
	chip->csave_regs.gpio_ie = readb(chip->base + GPIOIE);
	writeb(0, chip->base + GPIOIE);

	spin_unlock_irqrestore(&chip->lock, flags);

	send_tui_msg_config(TUI_POLL_CFG_OK, 0, (void *)dev_name(dev));
	return 0;
}

static int pl061_tui_release(struct device *dev)
{
	struct pl061_gpio *chip = dev_get_drvdata(dev);
	unsigned long flags;

	pr_debug("%s: is switching non-sec status\n", dev_name(dev));

	spin_lock_irqsave(&chip->lock, flags);

	writeb(chip->csave_regs.gpio_dir, chip->base + GPIODIR);
	writeb(chip->csave_regs.gpio_data, chip->base + GPIODATA);
	writeb(chip->csave_regs.gpio_is, chip->base + GPIOIS);
	writeb(chip->csave_regs.gpio_ibe, chip->base + GPIOIBE);
	writeb(chip->csave_regs.gpio_iev, chip->base + GPIOIEV);
	writeb(chip->csave_regs.gpio_ie, chip->base + GPIOIE);
	chip->sec_status = 0;

	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int pl061_tui_switch_func(void *pdata, int secure)
{
	struct device *dev = pdata;
	int ret;

	if (secure)
		ret = pl061_tui_request(dev);
	else
		ret = pl061_tui_release(dev);

	return ret;
}

static int pl061_probe(struct amba_device *adev, const struct amba_id *id)
{
	struct device *dev = &adev->dev;
	struct pl061_platform_data *pdata = dev->platform_data;
	struct pl061_gpio *chip;
	int ret, irq, i, irq_base;
	struct device_node *np = dev->of_node;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;

	if (pdata) {
		chip->gc.base = pdata->gpio_base;
		irq_base = pdata->irq_base;
		if (irq_base <= 0)
			return -ENODEV;
	} else {
		chip->gc.base = pl061_parse_gpio_base(dev);
		irq_base = 0;
	}

	if (!devm_request_mem_region(dev, adev->res.start,
				     resource_size(&adev->res), "pl061"))
		return -EBUSY;

	chip->base = devm_ioremap(dev, adev->res.start,
				  resource_size(&adev->res));
	if (!chip->base)
		return -ENOMEM;

	spin_lock_init(&chip->lock);

	if (of_get_property(np, "gpio,hwspinlock", NULL)) {
		gpio_hwlock = hwspin_lock_request_specific(GPIO_HWLOCK_ID);
		if (gpio_hwlock == NULL)
			return -EBUSY;
	}

	/* clear sec-flag of the controller */
	chip->sec_status = 0;
	if (of_get_property(np, "sec-controller", NULL)) {
		ret = register_tui_driver(pl061_tui_switch_func, dev_name(dev), dev, 1);
		if (ret)
			pr_err("%s: could not register switch\n", dev_name(dev));
		else
			pr_debug("%s: supports sec property\n", dev_name(dev));
	}

	/* Hook the request()/free() for pinctrl operation */
	if (of_get_property(dev->of_node, "gpio-ranges", NULL)) {
		chip->gc.request = pl061_gpio_request;
		chip->gc.free = pl061_gpio_free;
	}
	chip->gc.direction_input = pl061_direction_input;
	chip->gc.direction_output = pl061_direction_output;
	chip->gc.get = pl061_get_value;
	chip->gc.set = pl061_set_value;
	chip->gc.to_irq = pl061_to_irq;
	chip->gc.ngpio = PL061_GPIO_NR;
	chip->gc.label = dev_name(dev);
	chip->gc.dev = dev;
	chip->gc.owner = THIS_MODULE;

	ret = gpiochip_add(&chip->gc);
	if (ret)
		return ret;

	/*
	 * irq_chip support
	 */
	writeb(0xff, chip->base + GPIOIC);
	writeb(0, chip->base + GPIOIE); /* disable irqs */
	irq = adev->irq[0];
	if (irq < 0)
		return -ENODEV;

	irq_set_chained_handler(irq, pl061_irq_handler);
	irq_set_handler_data(irq, chip);

	chip->domain = irq_domain_add_simple(adev->dev.of_node, PL061_GPIO_NR,
					     irq_base, &pl061_domain_ops, chip);
	if (!chip->domain)
		return -ENODEV;

	for (i = 0; i < PL061_GPIO_NR; i++) {
		if (pdata) {
			if (pdata->directions & (1 << i))
				pl061_direction_output(&chip->gc, i,
						pdata->values & (1 << i));
			else
				pl061_direction_input(&chip->gc, i);
		}
	}

	amba_set_drvdata(adev, chip);

	return 0;
}

#ifdef CONFIG_GPIO_PM_SUPPORT
static int pl061_suspend(struct device *dev)
{
	struct pl061_gpio *chip = dev_get_drvdata(dev);
	int offset;

	chip->csave_regs.gpio_data = 0;
	chip->csave_regs.gpio_dir = readb(chip->base + GPIODIR);
	chip->csave_regs.gpio_is = readb(chip->base + GPIOIS);
	chip->csave_regs.gpio_ibe = readb(chip->base + GPIOIBE);
	chip->csave_regs.gpio_iev = readb(chip->base + GPIOIEV);
	chip->csave_regs.gpio_ie = readb(chip->base + GPIOIE);

	for (offset = 0; offset < PL061_GPIO_NR; offset++) {
		if (chip->csave_regs.gpio_dir & (1 << offset))
			chip->csave_regs.gpio_data |=
				pl061_get_value(&chip->gc, offset) << offset;
	}

	return 0;
}

static int pl061_resume(struct device *dev)
{
	struct pl061_gpio *chip = dev_get_drvdata(dev);
	int offset;

	for (offset = 0; offset < PL061_GPIO_NR; offset++) {
		if (chip->csave_regs.gpio_dir & (1 << offset))
			pl061_direction_output(&chip->gc, offset,
					chip->csave_regs.gpio_data &
					(1 << offset));
		else
			pl061_direction_input(&chip->gc, offset);
	}

	writeb(chip->csave_regs.gpio_is, chip->base + GPIOIS);
	writeb(chip->csave_regs.gpio_ibe, chip->base + GPIOIBE);
	writeb(chip->csave_regs.gpio_iev, chip->base + GPIOIEV);
	writeb(chip->csave_regs.gpio_ie, chip->base + GPIOIE);

	return 0;
}

static const struct dev_pm_ops pl061_dev_pm_ops = {
	.suspend = pl061_suspend,
	.resume = pl061_resume,
	.freeze = pl061_suspend,
	.restore = pl061_resume,
};
#endif

static struct amba_id pl061_ids[] = {
	{
		.id	= 0x00041061,
		.mask	= 0x000fffff,
	},
	{ 0, 0 },
};

MODULE_DEVICE_TABLE(amba, pl061_ids);

static struct amba_driver pl061_gpio_driver = {
	.drv = {
		.name	= "pl061_gpio",
#ifdef CONFIG_GPIO_PM_SUPPORT
		.pm	= &pl061_dev_pm_ops,
#endif
	},
	.id_table	= pl061_ids,
	.probe		= pl061_probe,
};

static int __init pl061_gpio_init(void)
{
	return amba_driver_register(&pl061_gpio_driver);
}
subsys_initcall(pl061_gpio_init);

MODULE_AUTHOR("Baruch Siach <baruch@tkos.co.il>");
MODULE_DESCRIPTION("PL061 GPIO driver");
MODULE_LICENSE("GPL");
