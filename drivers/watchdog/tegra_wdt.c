/*
 * drivers/watchdog/tegra_wdt.c
 *
 * watchdog driver for NVIDIA tegra internal watchdog
 *
 * Copyright (c) 2010, NVIDIA Corporation.
 *
 * based on drivers/watchdog/softdog.c and drivers/watchdog/omap_wdt.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>
#include <linux/delay.h>
#include <linux/module.h>

static int check_scheduler_thread(void *wdt);

/* minimum and maximum watchdog trigger periods, in seconds */
#define MIN_WDT_PERIOD	0
#define MAX_WDT_PERIOD	1000

#define TIMER_PTV	0x0
#define TIMER_EN	(1 << 31)
#define TIMER_PERIODIC	(1 << 30)

#define TIMER_PCR	0x4
#define TIMER_PCR_INTR	(1 << 30)

#define WDT_EN		(1 << 5)
#define WDT_SEL_TMR1	(0 << 4)
#define WDT_SEL_TMR2	(1 << 4)
#define WDT_SYS_RST	(1 << 2)

#define total_retry 5
static int heartbeat = (130); 

struct tegra_wdt {
	struct miscdevice	miscdev;
	struct notifier_block	notifier;
	struct resource		*res_src;
	struct resource		*res_wdt;
	unsigned long		users;
	void __iomem		*wdt_source;
	void __iomem		*wdt_timer;
	int			irq;
	int			timeout;
	bool			enabled;
};

static struct platform_device *tegra_wdt_dev;

struct mutex my_sync_set_timeout;
struct mutex my_sync_enable;

struct tegra_wdt *wdt_address;
void reboot_device_via_watchdog_now();


static void tegra_wdt_set_timeout(struct tegra_wdt *wdt, int sec)
{
	u32 ptv, src;

    mutex_lock(&my_sync_set_timeout);

	ptv = readl(wdt->wdt_timer + TIMER_PTV);
	src = readl(wdt->wdt_source);

	writel(0, wdt->wdt_source);
	wdt->timeout = clamp(sec, MIN_WDT_PERIOD, MAX_WDT_PERIOD);
    printk("dog, +tegra_wdt_set_timeout, wdt->timeout=%d\n", wdt->timeout);	
	if (ptv & TIMER_EN) {
	    #if 0
		
		ptv = wdt->timeout * 1000000ul / 2;
		#else
		if (wdt->timeout) {
		    ptv = wdt->timeout * 1000000ul ;
		} else {  
		    ptv = 1 * 1000ul ;
		}
		#endif
		ptv |= (TIMER_EN | TIMER_PERIODIC);
		writel(ptv, wdt->wdt_timer + TIMER_PTV);
	}
	else {
	    printk("dog, tegra_wdt_set_timeout, !!! NOT set ptv !!!\n");
	}
	writel(src, wdt->wdt_source);
	mutex_unlock(&my_sync_set_timeout);
}


static void tegra_wdt_enable(struct tegra_wdt *wdt)
{
	u32 val;

	
	printk("dog, +tegra_wdt_enable\n");
	/* since the watchdog reset occurs when a second interrupt
	 * is asserted before the first is processed, program the
	 * timer period to one-half of the watchdog period */
	mutex_lock(&my_sync_enable);
	val = wdt->timeout * 1000000ul / 2;
	val |= (TIMER_EN | TIMER_PERIODIC);
	writel(val, wdt->wdt_timer + TIMER_PTV);

	val = WDT_EN | WDT_SEL_TMR1 | WDT_SYS_RST;
	writel(val, wdt->wdt_source);
	mutex_unlock(&my_sync_enable);	
}

static void tegra_wdt_disable(struct tegra_wdt *wdt)
{
	writel(0, wdt->wdt_source);
	writel(0, wdt->wdt_timer + TIMER_PTV);
}

#if 0
static irqreturn_t tegra_wdt_interrupt(int irq, void *dev_id)
{
	struct tegra_wdt *wdt = dev_id;

	writel(TIMER_PCR_INTR, wdt->wdt_timer + TIMER_PCR);
	return IRQ_HANDLED;
}
#endif

static int tegra_wdt_notify(struct notifier_block *this,
			    unsigned long code, void *dev)
{
	struct tegra_wdt *wdt = container_of(this, struct tegra_wdt, notifier);

	if (code == SYS_DOWN || code == SYS_HALT)
		tegra_wdt_disable(wdt);
	return NOTIFY_DONE;
}

static int tegra_wdt_open(struct inode *inode, struct file *file)
{
	struct tegra_wdt *wdt = platform_get_drvdata(tegra_wdt_dev);


	if (test_and_set_bit(1, &wdt->users))
		return -EBUSY;

	wdt->enabled = true;
	wdt->timeout = heartbeat;
	tegra_wdt_enable(wdt);
	file->private_data = wdt;
	return nonseekable_open(inode, file);
}

static int tegra_wdt_release(struct inode *inode, struct file *file)
{
	struct tegra_wdt *wdt = file->private_data;

#ifndef CONFIG_WATCHDOG_NOWAYOUT
	tegra_wdt_disable(wdt);
	wdt->enabled = false;
#endif
	wdt->users = 0;
	return 0;
}

static long tegra_wdt_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct tegra_wdt *wdt = file->private_data;
	static DEFINE_SPINLOCK(lock);
	int new_timeout;
	static const struct watchdog_info ident = {
		.identity = "Tegra Watchdog",
		.options = WDIOF_SETTIMEOUT,
		.firmware_version = 0,
	};

	switch (cmd) {
	case WDIOC_GETSUPPORT:
		return copy_to_user((struct watchdog_info __user *)arg, &ident,
				    sizeof(ident));
	case WDIOC_GETSTATUS:
	case WDIOC_GETBOOTSTATUS:
		return put_user(0, (int __user *)arg);

	case WDIOC_KEEPALIVE:
		return 0;

	case WDIOC_SETTIMEOUT:
		if (get_user(new_timeout, (int __user *)arg))
			return -EFAULT;
        printk("dog, WDIOC_SETTIMEOUT, new_timeout=%d\n", new_timeout);			
		spin_lock(&lock);
		tegra_wdt_disable(wdt);
		wdt->timeout = clamp(new_timeout, MIN_WDT_PERIOD, MAX_WDT_PERIOD);
		wdt->enabled = true;
		tegra_wdt_set_timeout(wdt, wdt->timeout);
		tegra_wdt_enable(wdt);
		printk("dog, -tegra_wdt_enable in WDIOC_SETTIMEOUT\n");
		spin_unlock(&lock);
		printk("dog, -WDIOC_SETTIMEOUT\n");
		break;
	case WDIOC_GETTIMEOUT:
		return put_user(wdt->timeout, (int __user *)arg);
	default:
		return -ENOTTY;
	}
}

static ssize_t tegra_wdt_write(struct file *file, const char __user *data,
			       size_t len, loff_t *ppos)
{
	return len;
}

static const struct file_operations tegra_wdt_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.write		= tegra_wdt_write,
	.unlocked_ioctl	= tegra_wdt_ioctl,
	.open		= tegra_wdt_open,
	.release	= tegra_wdt_release,
};

static int tegra_wdt_probe(struct platform_device *pdev)
{
	struct resource *res_src, *res_wdt, *res_irq;
	struct tegra_wdt *wdt;
	u32 src;
	int ret = 0;

	if (pdev->id != -1) {
		dev_err(&pdev->dev, "only id -1 supported\n");
		return -ENODEV;
	}

	if (tegra_wdt_dev != NULL) {
		dev_err(&pdev->dev, "watchdog already registered\n");
		return -EIO;
	}

	res_src = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	res_wdt = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	res_irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);

	if (!res_src || !res_wdt || !res_irq) {
		dev_err(&pdev->dev, "incorrect resources\n");
		return -ENOENT;
	}

	wdt = kzalloc(sizeof(*wdt), GFP_KERNEL);
	if (!wdt) {
		dev_err(&pdev->dev, "out of memory\n");
		return -ENOMEM;
	}

	wdt->irq = -1;
	wdt->miscdev.parent = &pdev->dev;
	wdt->miscdev.minor = WATCHDOG_MINOR;
	wdt->miscdev.name = "watchdog";
	wdt->miscdev.fops = &tegra_wdt_fops;

	wdt->notifier.notifier_call = tegra_wdt_notify;
	
    wdt_address=wdt;

	res_src = request_mem_region(res_src->start, resource_size(res_src),
				     pdev->name);
	res_wdt = request_mem_region(res_wdt->start, resource_size(res_wdt),
				     pdev->name);

	if (!res_src || !res_wdt) {
		dev_err(&pdev->dev, "unable to request memory resources\n");
		ret = -EBUSY;
		goto fail;
	}

	wdt->wdt_source = ioremap(res_src->start, resource_size(res_src));
	wdt->wdt_timer = ioremap(res_wdt->start, resource_size(res_wdt));
	if (!wdt->wdt_source || !wdt->wdt_timer) {
		dev_err(&pdev->dev, "unable to map registers\n");
		ret = -ENOMEM;
		goto fail;
	}

	src = readl(wdt->wdt_source);
	if (src & BIT(12))
		dev_info(&pdev->dev, "last reset due to watchdog timeout\n");

	tegra_wdt_disable(wdt);
	writel(TIMER_PCR_INTR, wdt->wdt_timer + TIMER_PCR);

#if 0
	ret = request_irq(res_irq->start, tegra_wdt_interrupt, IRQF_DISABLED,
			  dev_name(&pdev->dev), wdt);
	if (ret) {
		dev_err(&pdev->dev, "unable to configure IRQ\n");
		goto fail;
	}
#endif
	wdt->irq = res_irq->start;
	wdt->res_src = res_src;
	wdt->res_wdt = res_wdt;

#if 1	
    kernel_thread(check_scheduler_thread, (void*)wdt, CLONE_FS | CLONE_FILES);	
#endif    

	ret = register_reboot_notifier(&wdt->notifier);
	if (ret) {
		dev_err(&pdev->dev, "cannot register reboot notifier\n");
		goto fail;
	}

	ret = misc_register(&wdt->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register misc device\n");
		unregister_reboot_notifier(&wdt->notifier);
		goto fail;
	}

	platform_set_drvdata(pdev, wdt);
	tegra_wdt_dev = pdev;
#ifdef CONFIG_TEGRA_WATCHDOG_ENABLE_ON_PROBE
	wdt->enabled = true;
	wdt->timeout = heartbeat;
	tegra_wdt_enable(wdt);
#endif
	return 0;
fail:
	if (wdt->irq != -1)
		free_irq(wdt->irq, wdt);
	if (wdt->wdt_source)
		iounmap(wdt->wdt_source);
	if (wdt->wdt_timer)
		iounmap(wdt->wdt_timer);
	if (res_src)
		release_mem_region(res_src->start, resource_size(res_src));
	if (res_wdt)
		release_mem_region(res_wdt->start, resource_size(res_wdt));
	kfree(wdt);
	return ret;
}

static int tegra_wdt_remove(struct platform_device *pdev)
{
	struct tegra_wdt *wdt = platform_get_drvdata(pdev);

	tegra_wdt_disable(wdt);

	unregister_reboot_notifier(&wdt->notifier);
	misc_deregister(&wdt->miscdev);
	free_irq(wdt->irq, wdt);
	iounmap(wdt->wdt_source);
	iounmap(wdt->wdt_timer);
	release_mem_region(wdt->res_src->start, resource_size(wdt->res_src));
	release_mem_region(wdt->res_wdt->start, resource_size(wdt->res_wdt));
	kfree(wdt);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

#ifdef CONFIG_PM
static int tegra_wdt_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct tegra_wdt *wdt = platform_get_drvdata(pdev);

	tegra_wdt_disable(wdt);
	return 0;
}

static int tegra_wdt_resume(struct platform_device *pdev)
{
	struct tegra_wdt *wdt = platform_get_drvdata(pdev);

	if (wdt->enabled)
		tegra_wdt_enable(wdt);

	return 0;
}
#endif

static struct platform_driver tegra_wdt_driver = {
	.probe		= tegra_wdt_probe,
	.remove		= __devexit_p(tegra_wdt_remove),
#ifdef CONFIG_PM
	.suspend	= tegra_wdt_suspend,
	.resume		= tegra_wdt_resume,
#endif
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "tegra_wdt",
	},
};

static int __init tegra_wdt_init(void)
{
    int ret;
    
    printk("bootlog, +tegra_wdt_init\n");
	mutex_init(&my_sync_set_timeout);
	mutex_init(&my_sync_enable);    
	ret = platform_driver_register(&tegra_wdt_driver);
	printk("bootlog, -tegra_wdt_init, ret=%d\n", ret);
	return ret;
}

static void __exit tegra_wdt_exit(void)
{
	platform_driver_unregister(&tegra_wdt_driver);
}





static int check_scheduler_thread(void *_wdt)
{
    static int kick_times=0;
    int ret=0;
    struct tegra_wdt *wdt=(struct tegra_wdt *)_wdt;
 	struct task_struct *tsk = current;
 	ignore_signals(tsk);
 	set_cpus_allowed_ptr(tsk,  cpu_all_mask);
 	current->flags |= PF_NOFREEZE | PF_FREEZER_NOSIG; 
 	
    
    wdt->enabled = true;
    tegra_wdt_set_timeout(wdt, heartbeat);
    tegra_wdt_enable(wdt); 	

	for(;;)
	{
        for (kick_times=0; kick_times < total_retry; kick_times++)
        {	        
            msleep((heartbeat/total_retry) * 1000);
            tegra_wdt_set_timeout(wdt, heartbeat); 
        }
    }
    return ret;
}


void reboot_device_via_watchdog_now()
{
    printk("dog, +reboot_device_via_watchdog_now\n");
    tegra_wdt_set_timeout(wdt_address, 0);
    while(1);
}

EXPORT_SYMBOL(reboot_device_via_watchdog_now);


module_init(tegra_wdt_init);
module_exit(tegra_wdt_exit);

MODULE_AUTHOR("NVIDIA Corporation");
MODULE_DESCRIPTION("Tegra Watchdog Driver");

module_param(heartbeat, int, 0);
MODULE_PARM_DESC(heartbeat,
		 "Watchdog heartbeat period in seconds");

MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(WATCHDOG_MINOR);
MODULE_ALIAS("platform:tegra_wdt");
