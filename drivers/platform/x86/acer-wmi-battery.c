// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * acer-wmi-battery.c: Acer battery health control driver
 *
 * This is a driver for the WMI battery health control interface found
 * on some Acer laptops.  This interface allows to enable/disable a
 * battery charge limit ("health mode") and exposes the battery temperature.
 *
 * Based on acer-wmi-battery https://github.com/frederik-h/acer-wmi-battery/
 *
 * Copyright (C) 2022-2025  Frederik Harwath <frederik@harwath.name>
 */

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/compiler_attributes.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/version.h>
#include <linux/wmi.h>

#include <acpi/battery.h>

#define DRIVER_NAME	"acer-wmi-battery"

#define ACER_BATTERY_GUID "79772EC5-04B1-4BFD-843C-61E7F77B6CC9"

/*
 * The Acer OEM software seems to always use this battery index,
 * so we emulate this behaviour to not confuse the underlying firmware.
 *
 * However this also means that we only fully support devices with a
 * single battery for now.
 */
#define ACER_BATTERY_INDEX	0x1

struct get_battery_health_control_status_input {
	u8 uBatteryNo;
	u8 uFunctionQuery;
	u8 uReserved[2];
} __packed;

struct get_battery_health_control_status_output {
	u8 uFunctionList;
	u8 uReturn[2];
	u8 uFunctionStatus[5];
} __packed;

struct set_battery_health_control_input {
	u8 uBatteryNo;
	u8 uFunctionMask;
	u8 uFunctionStatus;
	u8 uReservedIn[5];
} __packed;

struct set_battery_health_control_output {
	u8 uReturn;
	u8 uReservedOut;
} __packed;

enum battery_mode {
	HEALTH_MODE = 1,
	CALIBRATION_MODE = 2,
};

struct acer_wmi_battery_data {
	struct acpi_battery_hook hook;
	struct wmi_device *wdev;
};

static int acer_wmi_battery_get_information(struct acer_wmi_battery_data *data,
					    u32 index, u32 battery, u32 *result)
{
	u32 args[2] = { index, battery };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer input = { sizeof(args), args };
	int ret;

	ret = wmidev_evaluate_method(data->wdev, 0, 19, &input, &output);
	if (ACPI_FAILURE(ret))
		return -EIO;

	union acpi_object *obj __free(kfree) = output.pointer;
	if (!obj)
		return -EIO;

	if (obj->type != ACPI_TYPE_BUFFER)
		ret = -EIO;

	if (obj->buffer.length != sizeof(u32)) {
		dev_err(&data->wdev->dev, "WMI battery information call returned buffer of unexpected length %u\n",
			obj->buffer.length);
		ret = -EINVAL;
	}

	*result = get_unaligned_le32(obj->buffer.pointer);

	return ret;
}

static int acer_wmi_battery_get_health_control_status(struct acer_wmi_battery_data *data,
						      bool *health_mode)
{
	/*
	 * Acer Care Center seems to always call the WMI method
	 * with fixed parameters. This yields information about
	 * the availability and state of both health and
	 * calibration mode. The modes probably apply to
	 * all batteries of the system.
	 */
	struct get_battery_health_control_status_input params = {
		.uBatteryNo = ACER_BATTERY_INDEX,
		.uFunctionQuery = 0x1,
		.uReserved = { 0x0, 0x0 }
	};
	struct acpi_buffer input = {
		sizeof(struct get_battery_health_control_status_input),
		&params
	};
	struct get_battery_health_control_status_output status_output;
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int ret;

	ret = wmidev_evaluate_method(data->wdev, 0, 20, &input, &output);
	if (ACPI_FAILURE(ret))
		return -EIO;

	union acpi_object *obj __free(kfree) = output.pointer;
	if (!obj)
		return -EIO;

	if (obj->type != ACPI_TYPE_BUFFER)
		ret = -EIO;

	status_output = *((struct get_battery_health_control_status_output *)
			obj->buffer.pointer);
	if (obj->buffer.length != 8) {
		dev_err(&data->wdev->dev, "WMI battery status call returned a buffer of unexpected length %d\n",
			obj->buffer.length);
		ret = -EINVAL;
	}

	if (health_mode) {
		if (status_output.uFunctionList & HEALTH_MODE)
			*health_mode = status_output.uFunctionStatus[0] > 0;
		else
			ret = -EINVAL;
	}

	return ret;
}

static int acer_wmi_battery_set_battery_health_control(struct acer_wmi_battery_data *data,
						       u8 function, bool function_status)
{
	struct set_battery_health_control_input params = {
		.uBatteryNo = ACER_BATTERY_INDEX,
		.uFunctionMask = function,
		.uFunctionStatus = function_status ? 1 : 0,
		.uReservedIn = { 0x0, 0x0, 0x0, 0x0, 0x0 }
	};
	struct acpi_buffer input = {
		sizeof(struct set_battery_health_control_input),
		&params,
	};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	int ret;

	ret = wmidev_evaluate_method(data->wdev, 0, 21, &input, &output);
	if (ACPI_FAILURE(ret))
		return -EIO;

	obj = output.pointer;

	if (!obj)
		return -EIO;

	if (obj->type != ACPI_TYPE_BUFFER)
		ret = -EIO;

	if (obj->buffer.length != 4) {
		dev_err(&data->wdev->dev, "WMI battery status set operation returned a buffer of unexpected length %d\n",
			obj->buffer.length);
		ret = -EINVAL;
	}

	return ret;
}

static int acer_battery_ext_property_get(struct power_supply *psy,
					 const struct power_supply_ext *ext,
					 void *ext_data,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct acer_wmi_battery_data *data = ext_data;
	bool health_mode;
	u32 value;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_TYPES:
		ret = acer_wmi_battery_get_health_control_status(data, &health_mode);
		if (ret)
			return ret;

		val->intval = health_mode
			      ? POWER_SUPPLY_CHARGE_TYPE_LONGLIFE
			      : POWER_SUPPLY_CHARGE_TYPE_STANDARD;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = acer_wmi_battery_get_information(data, 0x8, ACER_BATTERY_INDEX, &value);
		if (ret)
			return ret;

		if (value > U16_MAX)
			return -ERANGE;

		val->intval = value - 2731;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int acer_battery_ext_property_set(struct power_supply *psy,
					       const struct power_supply_ext *ext,
					       void *ext_data,
					       enum power_supply_property psp,
					       const union power_supply_propval *val)
{
	struct acer_wmi_battery_data *data = ext_data;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_TYPES:
		return acer_wmi_battery_set_battery_health_control(data, HEALTH_MODE,
				val->intval == POWER_SUPPLY_CHARGE_TYPE_LONGLIFE);
	default:
		return -EINVAL;
	}
}

static int acer_battery_ext_property_is_writeable(struct power_supply *psy,
						  const struct power_supply_ext *ext,
						  void *ext_data,
						  enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_TYPES:
		return true;
	default:
		return false;
	}
}

static const enum power_supply_property acer_battery_properties[] = {
	POWER_SUPPLY_PROP_CHARGE_TYPES,
	POWER_SUPPLY_PROP_TEMP,
};

static const struct power_supply_ext acer_wmi_battery_extension = {
	.name			= "acer_wmi_battery",
	.properties		= acer_battery_properties,
	.num_properties		= ARRAY_SIZE(acer_battery_properties),
	.charge_types           = BIT(POWER_SUPPLY_CHARGE_TYPE_STANDARD) |
				   BIT(POWER_SUPPLY_CHARGE_TYPE_LONGLIFE),
	.get_property		= acer_battery_ext_property_get,
	.set_property		= acer_battery_ext_property_set,
	.property_is_writeable	= acer_battery_ext_property_is_writeable,
};

static int acer_battery_add(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	struct acer_wmi_battery_data *data = container_of(hook, struct acer_wmi_battery_data, hook);

	return power_supply_register_extension(battery, &acer_wmi_battery_extension,
					       &data->wdev->dev, data);
}

static int acer_battery_remove(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	power_supply_unregister_extension(battery, &acer_wmi_battery_extension);

	return 0;
}

static int acer_wmi_battery_battery_add(struct acer_wmi_battery_data *data)
{
	data->hook.name = "Acer Battery Extension";
	data->hook.add_battery = acer_battery_add;
	data->hook.remove_battery = acer_battery_remove;

	return devm_battery_hook_register(&data->wdev->dev, &data->hook);
}

static int acer_wmi_battery_probe(struct wmi_device *wdev, const void *context)
{
	struct acer_wmi_battery_data *data;

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_set_drvdata(&wdev->dev, data);
	data->wdev = wdev;

	return acer_wmi_battery_battery_add(data);
}

static const struct wmi_device_id acer_wmi_battery_id_table[] = {
	{ ACER_BATTERY_GUID, NULL },
	{ }
};
MODULE_DEVICE_TABLE(wmi, acer_wmi_battery_id_table);

static struct wmi_driver acer_wmi_battery_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = acer_wmi_battery_id_table,
	.probe = acer_wmi_battery_probe,
	.no_singleton = true,
};
module_wmi_driver(acer_wmi_battery_driver);

MODULE_AUTHOR("Frederik Harwath <frederik@harwath.name>");
MODULE_AUTHOR("Jelle van der Waa <jelle@vdwaa.nl>");
MODULE_DESCRIPTION("Acer battery health control WMI driver");
MODULE_LICENSE("GPL");
