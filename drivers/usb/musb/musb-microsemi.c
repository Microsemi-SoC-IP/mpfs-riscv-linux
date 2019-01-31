// SPDX-License-Identifier: GPL-2.0+
/*
 * MUSB OTG controller driver for PolarFire
 *
 * Copyright 2018 Microsemi
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/prefetch.h>
#include <linux/usb/usb_phy_generic.h>

#include <asm/cacheflush.h>

#include "musb_core.h"
#include "musb_dma.h"
#include "musbhsdma.h"


struct pf_glue {
	struct device		*dev;
	struct platform_device	*musb;
	struct platform_device	*phy;
};
#define glue_to_musb(g)		platform_get_drvdata(g->musb)

static void pf_musb_set_vbus(struct musb *musb, int is_on);

static irqreturn_t polarfire_interrupt(int irq, void *__hci)
{
	unsigned long	flags;
	irqreturn_t	retval = IRQ_NONE;
	struct musb	*musb = __hci;

	spin_lock_irqsave(&musb->lock, flags);

	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX);

	if (musb->int_usb || musb->int_tx || musb->int_rx) {
		musb_writeb(musb->mregs, MUSB_INTRUSB, musb->int_usb);
		musb_writew(musb->mregs, MUSB_INTRTX, musb->int_tx);
		musb_writew(musb->mregs, MUSB_INTRRX, musb->int_rx);
		retval = musb_interrupt(musb);
	}

	spin_unlock_irqrestore(&musb->lock, flags);

	return retval;
}

/*
static void musb_conn_timer_handler(struct timer_list *t)
{
}

static void pf_musb_enable(struct musb *musb)
{
}

static void pf_musb_disable(struct musb *musb)
{
}

static int pf_musb_set_power(struct usb_phy *x, unsigned int mA)
{
	return 0;
}

static int pf_musb_vbus_status(struct musb *musb)
{
	return 0;
}

static int pf_musb_set_mode(struct musb *musb, u8 musb_mode)
{
	return -EIO;
}


static void pf_musb_reg_init(struct musb *musb)
{
}
*/

static int pf_musb_init(struct musb *musb)
{
	//int ret = -ENODEV;

pr_info("******* %s() called\n", __func__);

	musb->xceiv = usb_get_phy(USB_PHY_TYPE_USB2);
	if (IS_ERR(musb->xceiv)) {
		pr_err("HS UDC: no transceiver configured\n");
		return PTR_ERR(musb->xceiv);
	}

	musb->dyn_fifo = true;

	/* Assign ISR */
	musb->isr = polarfire_interrupt;

	musb_platform_set_vbus(musb, 1);

	return 0;
}

static int pf_musb_exit(struct musb *musb)
{
	usb_put_phy(musb->xceiv);

	return 0;
}

static void pf_musb_set_vbus(struct musb *musb, int is_on)
{
	u8 devctl;

	/* HDRC controls CPEN, but beware current surges during device
	 * connect.  They can trigger transient overcurrent conditions
	 * that must be ignored.
	 */
	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	if (is_on) {
		musb->is_active = 1;
		musb->xceiv->otg->default_a = 1;
		musb->xceiv->otg->state = OTG_STATE_A_WAIT_VRISE;
		devctl |= MUSB_DEVCTL_SESSION;
		MUSB_HST_MODE(musb);
	} else {
		musb->is_active = 0;

		/* NOTE:  we're skipping A_WAIT_VFALL -> A_IDLE and
		 * jumping right to B_IDLE...
		 */
		musb->xceiv->otg->default_a = 0;
		musb->xceiv->otg->state = OTG_STATE_B_IDLE;
		devctl &= ~MUSB_DEVCTL_SESSION;

		MUSB_DEV_MODE(musb);
	}

	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

	dev_dbg(musb->controller, "VBUS %s, devctl %02x\n",
		usb_otg_state_string(musb->xceiv->otg->state),
		musb_readb(musb->mregs, MUSB_DEVCTL));
}

static const struct musb_platform_ops pf_ops = {
	.quirks		= MUSB_DMA_INVENTRA,
	.init		= pf_musb_init,
	.exit		= pf_musb_exit,
	.fifo_mode	= 2,
#ifdef CONFIG_USB_INVENTRA_DMA
	.dma_init	= musbhs_dma_controller_create,
	.dma_exit	= musbhs_dma_controller_destroy,
#endif
//	.enable		= pf_musb_enable,
//	.disable	= pf_musb_disable,
//	.set_mode	= pf_musb_set_mode,
//	.vbus_status	= pf_musb_vbus_status,
	.set_vbus	= pf_musb_set_vbus
};

static u64 pf_dmamask = DMA_BIT_MASK(32);

static int pf_probe(struct platform_device *pdev)
{
	#define GPIOBASEADDR 0x2000103000
	#define IRQ_REG_OFFSET1 0x1C
	#define IRQ_REG_OFFSET2 0x88
	#define CLEAR_ALL_IRQ32 0xFFFFFFFF
	#define GPIO_OUTPUT_MODE 0x0000000005UL

	struct resource musb_resources[3];
	struct musb_hdrc_platform_data	*pdata = dev_get_platdata(&pdev->dev);
	struct platform_device		*musb;
	struct pf_glue		        *glue;

	struct musb_hdrc_config		*config;
	struct device_node		*np = pdev->dev.of_node;
	const char			*mode;
	int				strlen;
	int				ret = -ENOMEM, val;

	void __iomem *reg = ioremap(GPIOBASEADDR, 4095);
	writew(0x00000005, (reg+IRQ_REG_OFFSET1));
	writeb(0x000000FF, (reg+IRQ_REG_OFFSET2));

	glue = devm_kzalloc(&pdev->dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		goto err0;

	writeb(0x00000000, (reg+IRQ_REG_OFFSET2));

	musb = platform_device_alloc("musb-hdrc", PLATFORM_DEVID_AUTO);
	if (!musb)
		goto err0;

	musb->dev.parent		= &pdev->dev;
	musb->dev.dma_mask		= &pf_dmamask;
	musb->dev.coherent_dma_mask	= pf_dmamask;

	glue->dev			= &pdev->dev;
	glue->musb			= musb;

	if (np) {
		pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
		if (!pdata)
			goto err1;

		config = devm_kzalloc(&pdev->dev, sizeof(*config), GFP_KERNEL);
		if (!config)
			goto err1;

		of_property_read_u32(np, "mode", (u32 *)&pdata->mode);
		of_property_read_u32(np, "num-eps", (u32 *)&config->num_eps);
		of_property_read_u32(np, "ram-bits", (u32 *)&config->ram_bits);
		of_property_read_u32(np, "power", (u32 *)&pdata->power);

		ret = of_property_read_u32(np, "multipoint", &val);
		if (!ret && val)
			config->multipoint = true;

		/* TODO: Temporary arrangement need to read from device tree*/
		config->dyn_fifo	= true;
		pdata->config		= config;

	}

	pdata->platform_ops		= &pf_ops;

	mode = of_get_property(np, "dr_mode", &strlen);
	if (!mode) {
		dev_err(&pdev->dev, "No 'dr_mode' property found\n");
		return 0;
	}

	if (strlen > 0) {
		if (!strcmp(mode, "host"))
			pdata->mode = MUSB_HOST;

		if (!strcmp(mode, "otg"))
			pdata->mode = MUSB_OTG;

		if (!strcmp(mode, "peripheral"))
			pdata->mode = MUSB_PERIPHERAL;
	}

	/* register a nop PHY */
	usb_phy_generic_register();

	platform_set_drvdata(pdev, glue);

	memset(musb_resources, 0x00,
		sizeof(*musb_resources) * ARRAY_SIZE(musb_resources));

	musb_resources[0].name  = pdev->resource[0].name;
	musb_resources[0].start = pdev->resource[0].start;
	musb_resources[0].end   = pdev->resource[0].end;
	musb_resources[0].flags = pdev->resource[0].flags;

	musb_resources[1].name  = pdev->resource[1].name;
	musb_resources[1].start = pdev->resource[1].start;
	musb_resources[1].end   = pdev->resource[1].end;
	musb_resources[1].flags = pdev->resource[1].flags;

	musb_resources[2].name  = pdev->resource[2].name;
	musb_resources[2].start = pdev->resource[2].start;
	musb_resources[2].end   = pdev->resource[2].end;
	musb_resources[2].flags = pdev->resource[2].flags;

	ret = platform_device_add_resources(musb, musb_resources,
		ARRAY_SIZE(musb_resources));
	if (ret) {
		dev_err(&pdev->dev, "failed to add resources\n");
		goto err1;
	}

	ret = platform_device_add_data(musb, pdata, sizeof(*pdata));
	if (ret) {
		dev_err(&pdev->dev, "failed to add platform_data\n");
		goto err1;
	}


	ret = platform_device_add(musb);
	if (ret) {
		dev_err(&pdev->dev, "failed to register musb device\n");
		goto err1;
	}

	return 0;

err1:
	usb_phy_generic_unregister(glue->phy);

	platform_device_put(musb);

err0:
	return ret;
}

static int pf_remove(struct platform_device *pdev)
{
	struct pf_glue	*glue = platform_get_drvdata(pdev);

	platform_device_unregister(glue->musb);
	usb_phy_generic_unregister(pdev);

	return 0;
}


static int pf_suspend(struct device *dev)
{
	return 0; }

static int pf_resume(struct device *dev)
{
	return 0;
}


static SIMPLE_DEV_PM_OPS(pf_pm_ops, pf_suspend, pf_resume);

static const struct of_device_id pf_id_table[] = {
	{
		.compatible = "microsemi,ms-pf-usb-host",
	},
	{ }
};

/*
MODULE_DEVICE_TABLE(of, pf_id_table);
*/

static struct platform_driver microsemi_musb_driver = {
	.probe		= pf_probe,
	.remove		= pf_remove,
	.driver		= {
		.name	= "microsemi-musb",
		.pm = &pf_pm_ops,
		.of_match_table = of_match_ptr(pf_id_table)
	},
};

/*
MODULE_DESCRIPTION("PolarFire MUSB Glue Layer");
MODULE_AUTHOR("Sagar Khadgi <sagar.khadgi@microsemi.com>");
MODULE_LICENSE("GPL v2");
module_platform_driver(microsemi_musb_driver);
*/
builtin_platform_driver(microsemi_musb_driver);
