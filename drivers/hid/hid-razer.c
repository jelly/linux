// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver to enable macro keys on Razer keyboards
 *
 * Copyright (c) 2019 Jelle van der Waa <jelle@vdwaa.nl>
 */

#include <linux/hid.h>
#include <linux/module.h>
#include "hid-ids.h"

#define RAZER_BLACKWIDOW_FEATURE_REPORT 0x00
#define BUF_SIZE 91

static const u8 data[BUF_SIZE] = {0, 0, 0, 0, 0, 0, 2, 0, 4, 2, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 4, 0};

static const struct hid_device_id razer_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_RAZER, USB_DEVICE_ID_RAZER_BLACKWIDOW) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_RAZER, USB_DEVICE_ID_RAZER_BLACKWIDOW_2013) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_RAZER, USB_DEVICE_ID_RAZER_BLACKWIDOW_ULTIMATE) },
	{}
};

MODULE_DEVICE_TABLE(hid, razer_devices);

static int razer_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct hid_report_enum *rep_enum;
	struct hid_report *rep;
	unsigned char *dmabuf;
	bool has_ff000002 = false;
	int ret = 0;

	dmabuf = kmemdup(data, BUF_SIZE, GFP_KERNEL);
	if (!dmabuf)
		return -ENOMEM;

	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	rep_enum = &hdev->report_enum[HID_FEATURE_REPORT];
	list_for_each_entry(rep, &rep_enum->report_list, list) {
		if (rep->maxfield && rep->field[0]->maxusage)
			if (rep->field[0]->usage[0].hid == 0xff000002)
				has_ff000002 = true;
	}

	if (has_ff000002) {
		ret = hid_hw_raw_request(hdev, RAZER_BLACKWIDOW_FEATURE_REPORT,
				dmabuf, BUF_SIZE, HID_FEATURE_REPORT,
				HID_REQ_SET_REPORT);
		if (ret != BUF_SIZE)
			hid_err(hdev, "Razer failed to enable macro keys\n");
	}

	kfree(dmabuf);

	return hid_hw_start(hdev, HID_CONNECT_DEFAULT);
}

static struct hid_driver razer_driver = {
	.name = "hid-razer",
	.id_table = razer_devices,
	.probe = razer_probe,
};

module_hid_driver(razer_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jelle van der Waa <jelle@vdwaa.nl");
MODULE_DESCRIPTION("Razer blackwidow 2013/2014 HID driver");
