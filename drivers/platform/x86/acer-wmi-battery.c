// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * acer-wmi-battery.c: Acer battery health control driver
 *
 * This is a driver for the WMI battery health control interface found
 * on some Acer laptops.  This interface allows to enable/disable a
 * battery charge limit ("health mode") and to calibrate the battery.
 *
 * Based on acer-wmi-battery https://github.com/frederik-h/acer-wmi-battery/
 *   Copyright (C) 2022-2025  Frederik Harwath <frederik@harwath.name>
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/power_supply.h>
#include <linux/version.h>
#include <linux/wmi.h>
#include <linux/unaligned.h>

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

enum battery_mode { HEALTH_MODE = 1, CALIBRATION_MODE = 2 };

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
	union acpi_object *obj;
	acpi_status status;

	status = wmi_evaluate_method(ACER_BATTERY_GUID, 0, 19, &input, &output);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = output.pointer;
	if (!obj)
		return -ENODATA;

	if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return -ENOMSG;
	}

	if (obj->buffer.length != sizeof(u32)) {
		dev_err(&data->wdev->dev, "WMI battery information call returned buffer of unexpected length %u\n",
			obj->buffer.length);
		kfree(obj);
		return -EMSGSIZE;
	}

	*result = get_unaligned_le32(obj->buffer.pointer);
	kfree(obj);

	return 0;
}

static int acer_wmi_battery_get_health_control_status(struct acer_wmi_battery_data *data,
							      s8 *health_mode,
							      s8 *calibration_mode)
{
	union acpi_object *obj;
	acpi_status status;

	/*
	 * Acer Care Center seems to always call the WMI method
	 * with fixed parameters. This yields information about
	 * the availability and state of both health and
	 * calibration mode. The modes probably apply to
	 * all batteries of the system - if there are
	 * Acer laptops with multiple batteries?
	 */
	struct get_battery_health_control_status_input params = {
		.uBatteryNo = ACER_BATTERY_INDEX,
		.uFunctionQuery = 0x1,
		.uReserved = { 0x0, 0x0 }
	};
	struct get_battery_health_control_status_output ret;

	struct acpi_buffer input = {
		sizeof(struct get_battery_health_control_status_input), &params
	};

	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(ACER_BATTERY_GUID, 0, 20, &input, &output);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = output.pointer;
	if (!obj)
		return -ENODATA;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return -ENOMSG;
	}

	ret = *((struct get_battery_health_control_status_output *)
			obj->buffer.pointer);
	if (obj->buffer.length != 8) {
		dev_err(&data->wdev->dev, "WMI battery status call returned a buffer of unexpected length %d\n",
			obj->buffer.length);
		kfree(obj);
		return -EMSGSIZE;
	}

	if (health_mode)
		*health_mode = ret.uFunctionList & HEALTH_MODE ?
					  ret.uFunctionStatus[0] > 0 :
					  -1;

	if (calibration_mode)
		*calibration_mode = ret.uFunctionList & CALIBRATION_MODE ?
					       ret.uFunctionStatus[1] > 0 :
					       -1;

	kfree(obj);
	return status;
}

static int set_battery_health_control(struct acer_wmi_battery_data *data,
					      u8 function, bool function_status)
{
	union acpi_object *obj;
	acpi_status status;

	struct set_battery_health_control_input params = {
		.uBatteryNo = ACER_BATTERY_INDEX,
		.uFunctionMask = function,
		.uFunctionStatus = (u8)function_status,
		.uReservedIn = { 0x0, 0x0, 0x0, 0x0, 0x0 }
	};
	struct set_battery_health_control_output ret;

	struct acpi_buffer input = {
		sizeof(struct set_battery_health_control_input), &params
	};

	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(ACER_BATTERY_GUID, 0, 21, &input, &output);

	if (ACPI_FAILURE(status))
		return -EIO;

	obj = output.pointer;

	if (!obj)
		return -ENODATA;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return -ENOMSG;
	}

	ret = *((struct set_battery_health_control_output *)obj->buffer.pointer);

	if (obj->buffer.length != 4) {
		dev_err(&data->wdev->dev, "WMI battery status set operation returned a buffer of unexpected length %d\n",
			obj->buffer.length);
		return -EMSGSIZE;
	}

	kfree(obj);

	return status;
}

static int acer_battery_ext_property_get(struct power_supply *psy,
					 const struct power_supply_ext *ext,
					 void *ext_data,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct acer_wmi_battery_data *data = ext_data;
	s8 calibration_mode;
	s8 health_mode;
	u32 value;
	int err;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_TYPES:
		err = acer_wmi_battery_get_health_control_status(data, &health_mode, NULL);
		if (err)
			return err;

		if (health_mode < 0)
			return -EINVAL;

		val->intval = health_mode ? POWER_SUPPLY_CHARGE_TYPE_LONGLIFE :
				POWER_SUPPLY_CHARGE_TYPE_STANDARD;
		break;
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		err = acer_wmi_battery_get_health_control_status(data, NULL, &calibration_mode);
		if (err)
			return err;

		if (calibration_mode < -1)
			return -EINVAL;

		val->intval = calibration_mode ? POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE :
			POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		err = acer_wmi_battery_get_information(data, 0x8, ACER_BATTERY_INDEX, &value);
		if (err)
			return err;

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
		return set_battery_health_control(data, HEALTH_MODE,
				val->intval == POWER_SUPPLY_CHARGE_TYPE_LONGLIFE);
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		return set_battery_health_control(data, CALIBRATION_MODE,
				val->intval == POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE);
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
	case POWER_SUPPLY_PROP_TEMP:
		return false;
	default:
		return true;
	}
}

static const enum power_supply_property acer_battery_properties[] = {
	POWER_SUPPLY_PROP_CHARGE_TYPES,
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
	POWER_SUPPLY_PROP_TEMP,
};

static const struct power_supply_ext acer_wmi_battery_extension = {
	.name			= "acer_laptop",
	.properties		= acer_battery_properties,
	.num_properties		= ARRAY_SIZE(acer_battery_properties),
	.charge_types           = (BIT(POWER_SUPPLY_CHARGE_TYPE_STANDARD) |
				   BIT(POWER_SUPPLY_CHARGE_TYPE_LONGLIFE)),
	.charge_behaviours      = (BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |
				   BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE)),
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
};
module_wmi_driver(acer_wmi_battery_driver);

MODULE_AUTHOR("Frederik Harwath <frederik@harwath.name>");
MODULE_AUTHOR("Jelle van der Waa <jelle@vdwaa.nl>");
MODULE_DESCRIPTION("Acer battery health control WMI driver");
MODULE_LICENSE("GPL");
