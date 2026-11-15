// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
 *
 * Runs the suspend and resume callbacks of the drivers of a USB device
 *
 * A system sleep cannot run in the test environment: it freezes the
 * emulator, a process of the same machine that resume has to talk to, and
 * vhci_hcd refuses to sleep with a device attached. The drivers have no
 * runtime PM. The callbacks are called here in the order of a system sleep:
 *
 *   suspend  interfaces last to first: the dev_pm_ops suspend of each
 *            child device, last to first, then the USB driver's suspend
 *   resume   interfaces first to last: the USB driver's resume, then the
 *            dev_pm_ops resume of each child device
 *
 * Not reproduced: the freezing of user space, the suspend of the USB
 * device itself (port suspend, URBs flushed and refused by usbcore), the
 * order the PM core gives to devices outside this USB device.
 *
 *   echo "suspend 1-1" > /sys/module/ftdi_pm_test/parameters/cmd
 *   echo "resume 1-1"  > /sys/module/ftdi_pm_test/parameters/cmd
 *
 * The write fails with EOPNOTSUPP without CONFIG_PM_SLEEP (the child
 * drivers have no callbacks then), or with the error of the first callback
 * that fails.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/usb.h>

struct pm_test_req {
	const char *name;	/* USB device, e.g. "1-1" */
	bool suspend;
	unsigned int intfs;	/* USB driver methods called */
	unsigned int children;	/* child callbacks called */
	int ret;
};

static int pm_test_child(struct device *dev, void *data)
{
	struct pm_test_req *req = data;
	const struct dev_pm_ops *pm;
	int ret = 0;

	device_lock(dev);
	pm = dev->driver ? dev->driver->pm : NULL;
	if (pm && (req->suspend ? pm->suspend : pm->resume)) {
		/* What platform_pm_suspend() / platform_pm_resume() call */
		ret = req->suspend ? pm->suspend(dev) : pm->resume(dev);
		req->children++;
		if (ret)
			dev_err(dev, "pm_test: %s failed: %d\n",
				req->suspend ? "suspend" : "resume", ret);
	}
	device_unlock(dev);
	return ret;
}

static int pm_test_intf(struct usb_interface *intf, struct pm_test_req *req)
{
	struct usb_driver *drv;
	int ret = 0;

	if (req->suspend) {
		ret = device_for_each_child_reverse(&intf->dev, req,
						    pm_test_child);
		if (ret)
			return ret;
	}

	device_lock(&intf->dev);
	if (intf->dev.driver) {
		drv = to_usb_driver(intf->dev.driver);
		if (req->suspend && drv->suspend)
			ret = drv->suspend(intf, PMSG_SUSPEND);
		else if (!req->suspend && drv->resume)
			ret = drv->resume(intf);
		req->intfs++;
		if (ret)
			dev_err(&intf->dev, "pm_test: %s failed: %d\n",
				req->suspend ? "suspend" : "resume", ret);
	}
	device_unlock(&intf->dev);
	if (ret)
		return ret;

	if (!req->suspend)
		ret = device_for_each_child(&intf->dev, req, pm_test_child);
	return ret;
}

static int pm_test_dev(struct usb_device *udev, void *data)
{
	struct pm_test_req *req = data;
	struct usb_host_config *config = udev->actconfig;
	int i, n;

	if (strcmp(dev_name(&udev->dev), req->name))
		return 0;
	if (!config)
		return -ENODEV;

	n = config->desc.bNumInterfaces;
	for (i = 0; i < n; i++) {
		int idx = req->suspend ? n - 1 - i : i;

		req->ret = pm_test_intf(config->interface[idx], req);
		if (req->ret)
			break;
	}
	return 1;	/* found: stop the walk */
}

static int pm_test_cmd(const char *val, const struct kernel_param *kp)
{
	struct pm_test_req req = { 0 };
	char verb[16], name[32];

	if (!IS_ENABLED(CONFIG_PM_SLEEP))
		return -EOPNOTSUPP;
	if (sscanf(val, "%15s %31s", verb, name) != 2)
		return -EINVAL;
	if (!strcmp(verb, "suspend"))
		req.suspend = true;
	else if (strcmp(verb, "resume"))
		return -EINVAL;
	req.name = name;

	if (!usb_for_each_dev(&req, pm_test_dev))
		return -ENODEV;

	pr_info("ftdi_pm_test: %s %s: %u interface(s), %u child callback(s): %d\n",
		name, req.suspend ? "suspended" : "resumed", req.intfs,
		req.children, req.ret);
	return req.ret;
}

static const struct kernel_param_ops pm_test_cmd_ops = {
	.set = pm_test_cmd,
};
module_param_cb(cmd, &pm_test_cmd_ops, NULL, 0200);
MODULE_PARM_DESC(cmd, "\"suspend USBDEV\" or \"resume USBDEV\", e.g. \"suspend 1-1\"");

MODULE_AUTHOR("Vincent Jardin");
MODULE_DESCRIPTION("Runs the suspend/resume callbacks of a USB device's drivers");
MODULE_LICENSE("GPL");
