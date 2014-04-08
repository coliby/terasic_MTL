/*
 * Terasic MTL touch screen input driver
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of.h>

#define MTC_REG_CLEAR_FIFO    0     // write only (write any value to clear fifo)
#define MTC_REG_INT_ACK        1        // write only (write any value to ack)
#define MTC_REG_DATA_NUM        2       // read only
#define MTC_REG_TOUCH_NUM       3       // read only
#define MTC_REG_X1              4       // read only
#define MTC_REG_Y1              5       // read only
#define MTC_REG_X2              6       // read only
#define MTC_REG_Y2              7       // read only
#define MTC_REG_GESTURE         8       // read only
#define MTC_REG_TEST         10       

#define MTL_TSC_MIN_X_VAL			0x0
#define MTL_TSC_MAX_X_VAL			0x320
#define MTL_TSC_MIN_Y_VAL			0x0
#define MTL_TSC_MAX_Y_VAL			0x1E0

#define tsc_readl(dev, reg) \
	__raw_readl((dev)->tsc_base + (reg))
#define tsc_writel(dev, reg, val) \
	__raw_writel((val), (dev)->tsc_base + (reg))

#define MOD_NAME "ts-mtl"

static u32 xs, ys;
	
struct mtl_tsc {
	struct input_dev *dev;
	void __iomem *tsc_base;
	int irq;
};
static void touch_timer_fire(unsigned long data);
static struct mtl_tsc *tsc;

static struct timer_list touch_timer =  
        TIMER_INITIALIZER(touch_timer_fire, 0, 0); 

static void touch_timer_fire(unsigned long data)
{
	struct input_dev *input = tsc->dev;
	input_report_abs(input, ABS_X, xs);
	input_report_abs(input, ABS_Y, ys);
        input_report_key(input, BTN_TOUCH, 0);  
        input_report_abs(input, ABS_PRESSURE, 0);  
        input_sync(input);
	writel(0,tsc->tsc_base);
	return;
 
} 
  
static void mtl_fifo_clear(struct mtl_tsc *tsc)
{
	tsc_writel(tsc, MTC_REG_CLEAR_FIFO, 0);
}

static irqreturn_t mtl_ts_interrupt(int irq, void *dev_id)
{
	
	struct input_dev *input = tsc->dev;	
	xs=readl(tsc->tsc_base+MTC_REG_X1*4);
	ys=readl(tsc->tsc_base+MTC_REG_Y1*4);
	tsc_writel(tsc, MTC_REG_INT_ACK*4, 0x00);
	input_report_abs(input, ABS_X, xs);
	input_report_abs(input, ABS_Y, ys);
	input_report_abs(input, ABS_PRESSURE, 1);
	input_report_key(input, BTN_TOUCH, 1);
	input_sync(input);
	mod_timer(&touch_timer, jiffies+8);  
	mtl_fifo_clear(tsc);
	return IRQ_HANDLED;
}


static int mtl_ts_probe(struct platform_device *pdev)
{
	
	struct input_dev *input;
	struct resource *res;
	resource_size_t size;
	int irq;
	int error;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Can't get memory resource\n");
		return -ENOENT;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "Can't get interrupt resource\n");
		return irq;
	}

	tsc = kzalloc(sizeof(*tsc), GFP_KERNEL);
	input = input_allocate_device();
	if (!tsc || !input) {
		dev_err(&pdev->dev, "failed allocating memory\n");
		error = -ENOMEM;
		goto err_free_mem;
	}

	tsc->dev = input;
	tsc->irq = irq;

	size = resource_size(res);

	if (!request_mem_region(res->start, size, pdev->name)) {
		dev_err(&pdev->dev, "TSC registers are not free\n");
		error = -EBUSY;
		goto err_free_mem;
	}
	tsc->tsc_base = ioremap(res->start, size);
	if (!tsc->tsc_base) {
		dev_err(&pdev->dev, "Can't map memory\n");
		error = -ENOMEM;
		goto err_release_mem;
	}
	input->name = MOD_NAME;
	input->phys = "mtl/input0";
	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0002;
	input->id.version = 0x0100;
	input->dev.parent = &pdev->dev;

	input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	input_set_abs_params(input, ABS_X, MTL_TSC_MIN_X_VAL,
			     MTL_TSC_MAX_X_VAL, 0, 0);
	input_set_abs_params(input, ABS_Y, MTL_TSC_MIN_Y_VAL,
			     MTL_TSC_MAX_Y_VAL, 0, 0);

	input_set_drvdata(input, tsc);

	error = request_irq(tsc->irq, mtl_ts_interrupt,
			    0, pdev->name, NULL);
	if (error) {
		dev_err(&pdev->dev, "failed requesting interrupt\n");
		goto err_unmap;
	}

	error = input_register_device(input);
	if (error) {
		dev_err(&pdev->dev, "failed registering input device\n");
		goto err_free_irq;
	}

	platform_set_drvdata(pdev, tsc);
	device_init_wakeup(&pdev->dev, 1);

	return 0;

err_free_irq:
	free_irq(tsc->irq, tsc);
err_unmap:
	iounmap(tsc->tsc_base);
err_release_mem:
	release_mem_region(res->start, size);
err_free_mem:
	input_free_device(input);
	kfree(tsc);

	return error;
}

static int mtl_ts_remove(struct platform_device *pdev)
{
	struct mtl_tsc *tsc = platform_get_drvdata(pdev);
	struct resource *res;

	device_init_wakeup(&pdev->dev, 0);
	free_irq(tsc->irq, tsc);

	input_unregister_device(tsc->dev);

	iounmap(tsc->tsc_base);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(res->start, resource_size(res));

	kfree(tsc);

	return 0;
}
static struct of_device_id mtl_touch_match[] = {
	{ .compatible = "terasic,mlt_touch_screen",  .data = NULL},
	{},
};
MODULE_DEVICE_TABLE(of, mtl_touch_match);

static struct platform_driver mtl_touch_driver = {
	.probe = mtl_ts_probe,
	.remove = mtl_ts_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "mtl_touch",
		.of_match_table = mtl_touch_match,
        },
};

module_platform_driver(mtl_touch_driver);

MODULE_DESCRIPTION("mtl touch screen test driver");
MODULE_AUTHOR("matthew wang--terasic");
MODULE_LICENSE("GPL");
