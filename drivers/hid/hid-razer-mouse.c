// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver for Razer wireless mice — battery reporting via power_supply.
 *
 * The wireless dongle exposes three HID interfaces. Interface 0 carries
 * vendor notification events (Report ID 0x05) that include battery level,
 * connection state, and DPI settings. This driver binds all interfaces
 * but only registers a power_supply on the notification interface.
 *
 * Copyright (c) 2026 Jelle van der Waa <jvanderwaa@redhat.com>
 */

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/usb.h>

#define RAZER_NOTIF_REPORT_ID		0x05
#define RAZER_NOTIF_BATTERY		0x31
#define RAZER_NOTIF_CONN_STATE		0x09

#define RAZER_CONN_SLEEP		0x02
#define RAZER_CONN_WAKE			0x03

/* Notification events arrive on USB interface 0 */
#define RAZER_NOTIF_IFACE		0

struct razer_mouse {
	struct hid_device *hdev;
	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	int capacity;
	bool present;
	bool connected;
};

static enum power_supply_property razer_mouse_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int razer_mouse_battery_get_property(struct power_supply *psy,
					    enum power_supply_property psp,
					    union power_supply_propval *val)
{
	struct razer_mouse *mouse = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (mouse->connected && mouse->present)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = mouse->present;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = mouse->capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (!mouse->present)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
		else if (mouse->capacity <= 5)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		else if (mouse->capacity <= 20)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else if (mouse->capacity <= 50)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = mouse->hdev->name;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Razer";
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void razer_mouse_set_wireless_status(struct razer_mouse *mouse,
					    bool connected)
{
	struct usb_interface *intf;

	if (!hid_is_usb(mouse->hdev))
		return;

	intf = to_usb_interface(mouse->hdev->dev.parent);
	usb_set_wireless_status(intf, connected ?
				USB_WIRELESS_STATUS_CONNECTED :
				USB_WIRELESS_STATUS_DISCONNECTED);
}

static int razer_mouse_raw_event(struct hid_device *hdev,
				 struct hid_report *report,
				 u8 *data, int size)
{
	struct razer_mouse *mouse = hid_get_drvdata(hdev);

	if (!mouse->battery || size < 6 ||
	    data[0] != RAZER_NOTIF_REPORT_ID)
		return 0;

	switch (data[1]) {
	case RAZER_NOTIF_BATTERY:
		mouse->capacity = data[2] * 100 / 255;
		mouse->present = data[5] == 0x01;
		power_supply_changed(mouse->battery);
		break;
	case RAZER_NOTIF_CONN_STATE:
		if (data[2] == RAZER_CONN_WAKE) {
			mouse->connected = true;
		} else if (data[2] == RAZER_CONN_SLEEP) {
			mouse->connected = false;
			power_supply_changed(mouse->battery);
		}
		razer_mouse_set_wireless_status(mouse, mouse->connected);
		break;
	}

	return 0;
}

static int razer_mouse_probe(struct hid_device *hdev,
			     const struct hid_device_id *id)
{
	struct razer_mouse *mouse;
	struct usb_interface *intf;
	struct power_supply_config psy_cfg = {};
	static atomic_t battery_no = ATOMIC_INIT(0);
	unsigned long n;
	int ret;

	if (!hid_is_usb(hdev))
		return -ENODEV;

	mouse = devm_kzalloc(&hdev->dev, sizeof(*mouse), GFP_KERNEL);
	if (!mouse)
		return -ENOMEM;

	mouse->hdev = hdev;
	hid_set_drvdata(hdev, mouse);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	intf = to_usb_interface(hdev->dev.parent);
	if (intf->cur_altsetting->desc.bInterfaceNumber == RAZER_NOTIF_IFACE) {
		n = atomic_inc_return(&battery_no) - 1;
		mouse->battery_desc.name =
			devm_kasprintf(&hdev->dev, GFP_KERNEL,
				       "razer_mouse_battery_%lu", n);
		if (!mouse->battery_desc.name)
			return -ENOMEM;

		mouse->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
		mouse->battery_desc.properties = razer_mouse_battery_props;
		mouse->battery_desc.num_properties =
			ARRAY_SIZE(razer_mouse_battery_props);
		mouse->battery_desc.get_property =
			razer_mouse_battery_get_property;

		razer_mouse_set_wireless_status(mouse, false);

		psy_cfg.drv_data = mouse;
		mouse->battery = devm_power_supply_register(&hdev->dev,
							    &mouse->battery_desc,
							    &psy_cfg);
		if (IS_ERR(mouse->battery))
			return PTR_ERR(mouse->battery);

		ret = power_supply_powers(mouse->battery, &hdev->dev);
		if (ret)
			return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	return 0;
}

static void razer_mouse_remove(struct hid_device *hdev)
{
	hid_hw_stop(hdev);
}

static const struct hid_device_id razer_mouse_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_RAZER,
		USB_DEVICE_ID_RAZER_VIPER_V3_HYPERSPEED) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_RAZER,
		USB_DEVICE_ID_RAZER_BASILISK_X_HYPERSPEED) },
	{ }
};
MODULE_DEVICE_TABLE(hid, razer_mouse_devices);

static struct hid_driver razer_mouse_driver = {
	.name = "razer-mouse",
	.id_table = razer_mouse_devices,
	.probe = razer_mouse_probe,
	.remove = razer_mouse_remove,
	.raw_event = razer_mouse_raw_event,
};
module_hid_driver(razer_mouse_driver);

MODULE_AUTHOR("Jelle van der Waa <jvanderwaa@redhat.com>");
MODULE_DESCRIPTION("HID driver for battery reporting on Razer wireless mice");
MODULE_LICENSE("GPL");
