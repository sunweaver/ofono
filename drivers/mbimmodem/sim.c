/*
 * oFono - Open Source Telephony
 * Copyright (C) 2017  Intel Corporation
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include <glib.h>
#include "simutil.h"

#include "drivers/mbimmodem/mbim.h"
#include "drivers/mbimmodem/mbim-message.h"
#include "drivers/mbimmodem/mbimmodem.h"

struct sim_app {
	enum mbim_app_type app_type;
	uint32_t aid_len;
	uint8_t *aid;
	char *label;
	uint32_t pin_key_references;
};

struct sim_data {
	struct mbim_device *device;
	uint32_t app_count;
	uint32_t active_app;
	struct sim_app *apps;
	char *iccid;
	char *imsi;
	uint32_t last_pin_type;
	bool present : 1;
};

static uint32_t mbim_file_structure_to_ofono(uint32_t file_structure)
{
	switch (file_structure) {
		case 1:
			return OFONO_SIM_FILE_STRUCTURE_TRANSPARENT;
		case 2:
			return OFONO_SIM_FILE_STRUCTURE_CYCLIC;
		case 3:
			return OFONO_SIM_FILE_STRUCTURE_FIXED;
		default:
			return OFONO_SIM_FILE_STRUCTURE_TRANSPARENT;
	}
}

static void mbim_sim_state_changed(struct ofono_sim *sim, uint32_t ready_state)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("ready_state: %u", ready_state);

	switch (ready_state) {
	case 0: /* Not Initialized */
		break;
	case 1: /* Initialized */
		if (!sd->present)
			ofono_sim_inserted_notify(sim, true);

		sd->present = true;
		ofono_sim_initialized_notify(sim);
		break;
	case 6: /* Device Locked */
		if (!sd->present)
			ofono_sim_inserted_notify(sim, true);

		sd->present = true;
		break;
	case 2: /* Not inserted */
	case 3: /* Bad SIM */
	case 4: /* Failure */
	case 5: /* Not activated */
		if (sd->present)
			ofono_sim_inserted_notify(sim, false);

		sd->present = false;
		break;
	default:
		break;
	}
}

static void mbim_read_imsi(struct ofono_sim *sim,
				ofono_sim_imsi_cb_t cb, void *user_data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	CALLBACK_WITH_SUCCESS(cb, sd->imsi, user_data);
}

static void read_file_info_cb(struct mbim_message *message, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	unsigned char access[3] = {0x0f, 0xff, 0xff};
	uint32_t version, status_word1, status_word2, file_accessibility;
	uint32_t file_type, file_structure, file_item_count, file_item_size;

	if (!mbim_message_get_error(message)) {
		mbim_message_get_arguments(message, "uuuuuuuu", &version, &status_word1,
					&status_word2, &file_accessibility,
					&file_type, &file_structure,
					&file_item_count, &file_item_size);

		CALLBACK_WITH_SUCCESS(cb, file_item_size,
					mbim_file_structure_to_ofono(file_structure),
					file_item_size * file_item_count,
					access, file_accessibility, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL, -1, cbd->data);
	}
}

static void mbim_read_file_info(struct ofono_sim *sim,
				int fileid, const unsigned char *path,
				unsigned int path_len,
				ofono_sim_file_info_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct mbim_message *message;
	uint8_t *file_id;

	message = mbim_message_new(mbim_ms_uicc_low_level_access,
					MBIM_CID_MS_UICC_LOW_LEVEL_ACCESS_FILE_STATUS,
					MBIM_COMMAND_TYPE_QUERY);

	file_id = mbim_get_fileid(fileid);
	mbim_message_set_arguments(message, "uayay", 1,
					data->apps[data->active_app].aid_len,
					data->apps[data->active_app].aid,
					4, file_id);
	l_free(file_id);

	mbim_device_send(data->device, SIM_GROUP, message, read_file_info_cb, cbd, l_free);
}

static void read_file_cb(struct mbim_message *message, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb;
	struct mbim_message_iter iter;
	uint32_t version, status_word1, status_word2, data_size;
	uint8_t *data = NULL;
	int i = 0;

	if (!mbim_message_get_error(message)) {
		mbim_message_get_arguments(message, "uuuay", &version,
						&status_word1, &status_word2,
						&data_size, &iter);

		data = l_malloc(data_size);
		while (mbim_message_iter_next_entry(&iter, data + i)) {
			i++;
		}

		CALLBACK_WITH_SUCCESS(cb, data, data_size, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
	}

	/* The data gets copied by ofono's sim driver, so we can free it */
	if (data)
		l_free(data);
}

static void mbim_read_file_transparent(struct ofono_sim *sim,
					int fileid, int start, int length,
					const unsigned char *path, unsigned int path_len,
					ofono_sim_read_cb_t cb, void *user_data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct mbim_message *message;
	uint8_t *file_id;

	message = mbim_message_new(mbim_ms_uicc_low_level_access,
					MBIM_CID_MS_UICC_LOW_LEVEL_ACCESS_READ_BINARY,
					MBIM_COMMAND_TYPE_QUERY);

	file_id = mbim_get_fileid(fileid);
	mbim_message_set_arguments(message, "uayayuusay", 1,
					sd->apps[sd->active_app].aid_len,
					sd->apps[sd->active_app].aid,
					4, file_id, start,
					length, "", 0, NULL);
	l_free(file_id);

	mbim_device_send(sd->device, SIM_GROUP, message,
				read_file_cb, cbd, l_free);
}

static void mbim_read_file_fixed_cyclic(struct ofono_sim *sim,
					int fileid, int record, int length,
					const unsigned char *path, unsigned int path_len,
					ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct mbim_message *message;
	uint8_t *file_id;

	message = mbim_message_new(mbim_ms_uicc_low_level_access,
					MBIM_CID_MS_UICC_LOW_LEVEL_ACCESS_READ_RECORD,
					MBIM_COMMAND_TYPE_QUERY);

	file_id = mbim_get_fileid(fileid);
	mbim_message_set_arguments(message, "uayayusay", 1,
					sd->apps[sd->active_app].aid_len,
					sd->apps[sd->active_app].aid,
					4, file_id, record,
					"", 0, NULL);
	l_free(file_id);

	mbim_device_send(sd->device, SIM_GROUP, message,
				read_file_cb, cbd, l_free);
}

static enum ofono_sim_password_type mbim_pin_type_to_sim_password(
							uint32_t pin_type)
{
	switch (pin_type) {
	case 0:  /* No Pin */
		return OFONO_SIM_PASSWORD_NONE;
	case 2: /* PIN1 key */
		return OFONO_SIM_PASSWORD_SIM_PIN;
	case 3: /* PIN2 key */
		return OFONO_SIM_PASSWORD_SIM_PIN2;
	case 4: /* device to SIM key */
		return OFONO_SIM_PASSWORD_PHSIM_PIN;
	case 5: /* device to very first SIM key */
		return OFONO_SIM_PASSWORD_PHFSIM_PIN;
	case 6: /* network personalization key */
		return OFONO_SIM_PASSWORD_PHNET_PIN;
	case 7: /* network subset personalization key */
		return OFONO_SIM_PASSWORD_PHNETSUB_PIN;
	case 8: /* service provider (SP) personalization key */
		return OFONO_SIM_PASSWORD_PHSP_PIN;
	case 9: /* corporate personalization key */
		return OFONO_SIM_PASSWORD_PHCORP_PIN;
	case 11: /* PUK1 */
		return OFONO_SIM_PASSWORD_SIM_PUK;
	case 12: /* PUK2 */
		return OFONO_SIM_PASSWORD_SIM_PUK2;
	case 13: /* device to very first SIM PIN unlock key */
		return OFONO_SIM_PASSWORD_PHFSIM_PUK;
	case 14: /* network personalization unlock key */
		return OFONO_SIM_PASSWORD_PHNET_PUK;
	case 15: /* network subset personaliation unlock key */
		return OFONO_SIM_PASSWORD_PHNETSUB_PUK;
	case 16: /* service provider (SP) personalization unlock key */
		return OFONO_SIM_PASSWORD_PHSP_PUK;
	case 17: /* corporate personalization unlock key */
		return OFONO_SIM_PASSWORD_PHCORP_PUK;
	}

	return OFONO_SIM_PASSWORD_INVALID;
}

static uint32_t mbim_pin_type_from_sim_password(
					enum ofono_sim_password_type type)
{
	switch (type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		return 2; /* PIN1 key */
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		return 3; /* PIN2 key */
	case OFONO_SIM_PASSWORD_PHSIM_PIN:
		return 4; /* device to SIM key */
	case OFONO_SIM_PASSWORD_PHFSIM_PIN:
		return 5; /* device to very first SIM key */
	case OFONO_SIM_PASSWORD_PHNET_PIN:
		return 6; /* network personalization key */
	case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
		return 7; /* network subset personalization key */
	case OFONO_SIM_PASSWORD_PHSP_PIN:
		return 8; /* service provider (SP) personalization key */
	case OFONO_SIM_PASSWORD_PHCORP_PIN:
		return 9; /* corporate personalization key */
	case OFONO_SIM_PASSWORD_SIM_PUK:
		return 11; /* PUK1 */
	case OFONO_SIM_PASSWORD_SIM_PUK2:
		return 12; /* PUK2 */
	case OFONO_SIM_PASSWORD_PHFSIM_PUK:
		return 13; /* device to very first SIM PIN unlock key */
	case OFONO_SIM_PASSWORD_PHNET_PUK:
		return 14; /* network personalization unlock key */
	case OFONO_SIM_PASSWORD_PHNETSUB_PUK:
		return 15; /* network subset personaliation unlock key */
	case OFONO_SIM_PASSWORD_PHSP_PUK:
		return 16; /* service provider (SP) personalization unlock key */
	case OFONO_SIM_PASSWORD_PHCORP_PUK:
		return 17; /* corporate personalization unlock key */
	case OFONO_SIM_PASSWORD_NONE:
	case OFONO_SIM_PASSWORD_INVALID:
		break;
	}

	return 0;
}

static void mbim_pin_query_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	struct sim_data *sd = cbd->user;
	ofono_sim_passwd_cb_t cb = cbd->cb;
	uint32_t pin_type;
	uint32_t pin_state;
	enum ofono_sim_password_type sim_password;
	bool r;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		goto error;

	r = mbim_message_get_arguments(message, "uu",
					&pin_type, &pin_state);
	if (!r)
		goto error;

	sim_password = mbim_pin_type_to_sim_password(pin_type);
	if (sim_password == OFONO_SIM_PASSWORD_INVALID)
		goto error;

	if (pin_state == 0)
		sim_password = OFONO_SIM_PASSWORD_NONE;

	sd->last_pin_type = pin_type;

	CALLBACK_WITH_SUCCESS(cb, sim_password, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void mbim_pin_query(struct ofono_sim *sim,
				ofono_sim_passwd_cb_t cb, void *user_data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct mbim_message *message;

	DBG("");

	cbd->user = sd;

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_PIN,
					MBIM_COMMAND_TYPE_QUERY);
	mbim_message_set_arguments(message, "");

	if (mbim_device_send(sd->device, SIM_GROUP, message,
				mbim_pin_query_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, -1, user_data);
}

static void mbim_pin_retries_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	uint32_t pin_type;
	uint32_t pin_state;
	uint32_t remaining;
	enum ofono_sim_password_type sim_password;
	bool r;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		goto error;

	r = mbim_message_get_arguments(message, "uuu",
					&pin_type, &pin_state, &remaining);
	if (!r)
		goto error;

	sim_password = mbim_pin_type_to_sim_password(pin_type);
	if (sim_password == OFONO_SIM_PASSWORD_INVALID)
		goto error;

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		retries[i] = -1;

	if (pin_state == 0 || sim_password == OFONO_SIM_PASSWORD_NONE) {
		CALLBACK_WITH_SUCCESS(cb, retries, cbd->data);
		return;
	}

	if (remaining == 0xffffffff)
		retries[sim_password] = -1;
	else
		retries[sim_password] = remaining;

	CALLBACK_WITH_SUCCESS(cb, retries, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void mbim_pin_retries_query(struct ofono_sim *sim,
				ofono_sim_pin_retries_cb_t cb, void *user_data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct mbim_message *message;

	DBG("");

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_PIN,
					MBIM_COMMAND_TYPE_QUERY);
	mbim_message_set_arguments(message, "");

	if (mbim_device_send(sd->device, SIM_GROUP, message,
				mbim_pin_retries_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, NULL, user_data);
}

static void mbim_pin_set_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	else
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void mbim_pin_set(struct ofono_sim *sim, uint32_t pin_type,
						uint32_t pin_operation,
						const char *old_passwd,
						const char *new_passwd,
						ofono_sim_lock_unlock_cb_t cb,
						void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct mbim_message *message;

	DBG("%u %u %s %s", pin_type, pin_operation, old_passwd, new_passwd);

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_PIN,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(message, "uuss", pin_type, pin_operation,
					old_passwd, new_passwd);

	if (mbim_device_send(sd->device, SIM_GROUP, message,
				mbim_pin_set_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void mbim_pin_enter(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	/* Use MBIMPinOperationEnter (0) and NULL second PIN */
	mbim_pin_set(sim, sd->last_pin_type, 0, passwd, NULL, cb, data);
}

static void mbim_puk_enter(struct ofono_sim *sim, const char *puk,
				const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	/* Use MBIMPinOperationEnter (0) and second PIN */
	mbim_pin_set(sim, sd->last_pin_type, 0, puk, passwd, cb, data);
}

static void mbim_pin_enable(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				int enable, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	uint32_t pin_type = mbim_pin_type_from_sim_password(passwd_type);

	if (pin_type == 0) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	/* Use MBIMPinOperationEnable (1) or MBIMPinOperationDisable (2) */
	mbim_pin_set(sim, pin_type, enable ? 1 : 2, passwd, NULL, cb, data);
}

static void mbim_pin_change(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old_passwd, const char *new_passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	uint32_t pin_type = mbim_pin_type_from_sim_password(passwd_type);

	if (pin_type == 0) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	/* Use MBIMPinOperationChange (3) */
	mbim_pin_set(sim, pin_type, 3, old_passwd, new_passwd, cb, data);
}

static void mbim_subscriber_ready_status_changed(struct mbim_message *message,
								void *user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);
	uint32_t ready_state;
	char *imsi;
	char *iccid;
	uint32_t ready_info;
	uint32_t ready_flags;
	bool r;

	DBG("");

	if (mbim_device_check_mbimex_version(sd->device, 3, 0)) {
		r = mbim_message_get_arguments(message, "uussu",
						&ready_state, &ready_flags, &imsi,
						&iccid, &ready_info);
	} else {
		r = mbim_message_get_arguments(message, "ussu",
						&ready_state, &imsi,
						&iccid, &ready_info);
	}

	if (!r)
		return;

	l_free(sd->iccid);
	sd->iccid = iccid;

	l_free(sd->imsi);
	sd->imsi = imsi;

	DBG("%s %s", iccid, imsi);

	mbim_sim_state_changed(sim, ready_state);
}

static void mbim_subscriber_ready_status_cb(struct mbim_message *message,
								void *user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);
	uint32_t ready_state;
	char *imsi;
	char *iccid;
	uint32_t ready_info;
	uint32_t ready_flags;
	bool r;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		goto error;

	/* We don't bother parsing MSISDN/MDN array */
	if (mbim_device_check_mbimex_version(sd->device, 3, 0)) {
		r = mbim_message_get_arguments(message, "uussu",
						&ready_state, &ready_flags, &imsi,
						&iccid, &ready_info);
	} else {
		r = mbim_message_get_arguments(message, "ussu",
						&ready_state, &imsi,
						&iccid, &ready_info);
	}

	if (!r)
		goto error;

	sd->iccid = iccid;
	sd->imsi = imsi;

	if (!mbim_device_register(sd->device, SIM_GROUP,
					mbim_uuid_basic_connect,
					MBIM_CID_SUBSCRIBER_READY_STATUS,
					mbim_subscriber_ready_status_changed,
					sim, NULL))
		goto error;

	ofono_sim_register(sim);
	DBG("%s %s", iccid, imsi);
	mbim_sim_state_changed(sim, ready_state);
	return;

error:
	ofono_sim_remove(sim);
}

static void mbim_sim_list_apps_cb(struct mbim_message *message,
					void *user)
{
	struct sim_data *data = ofono_sim_get_data(user);
	uint32_t version, count, active_app;
	uint32_t application_id = 0;
	uint32_t aid_size = 0, label_size = 0;
	char* aid_label;
	uint32_t new_count = 0;
	uint32_t size = 0;
	uint32_t offset, length;
	void *array;

	if (mbim_message_get_error(message)) {
		return;
	}

	mbim_message_get_arguments(message, "uuuuuu", &version, &count, &active_app, &size, &offset, &length);

	array = l_malloc(length);
	data->apps = l_new(struct sim_app, count);
	data->active_app = active_app;
	data->app_count = count;

	mbim_message_get_data(message, offset, array, length);

	/* For some reason, using mbim_message_get_arguments does not work,
	 * due to the large number of nested byte arrays in struct. Parse manually.
	 */

	offset = 0;
	for (int i = 0; i < count; i++) {
		int struct_offset = 0;
		int label_offset = 0;

		data->apps[i].app_type = L_LE32_TO_CPU(l_get_le32(array + offset));
		offset += 4;

		struct_offset = L_LE32_TO_CPU(l_get_le32(array + offset));
		offset += 4;

		data->apps[i].aid_len = aid_size = L_LE32_TO_CPU(l_get_le32(array + offset));
		offset += 4;

		data->apps[i].aid = l_malloc(aid_size);
		memcpy(data->apps[i].aid, array + struct_offset, aid_size);

		label_offset = L_LE32_TO_CPU(l_get_le32(array + offset));
		offset += 4;

		label_size = L_LE32_TO_CPU(l_get_le32(array + offset));
		offset += 4;

		data->apps[i].label = l_malloc(label_size + 1);
		memcpy(data->apps[i].label, array + label_offset, label_size);
		data->apps[i].label[label_size] = '\0';

		offset += length - offset;
	}

	l_free(array);
}

static int mbim_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *data)
{
	struct mbim_device *device = data;
	struct mbim_message *message;
	struct sim_data *sd;

	message = mbim_message_new(mbim_ms_uicc_low_level_access,
					MBIM_CID_MS_UICC_LOW_LEVEL_ACCESS_APPLICATION_LIST,
					MBIM_COMMAND_TYPE_QUERY);
	mbim_message_set_arguments(message, "");

	mbim_device_send(device, SIM_GROUP, message,
			mbim_sim_list_apps_cb, sim, NULL);

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_SUBSCRIBER_READY_STATUS,
					MBIM_COMMAND_TYPE_QUERY);
	if (!message)
		return -ENOMEM;

	mbim_message_set_arguments(message, "");

	if (!mbim_device_send(device, SIM_GROUP, message,
				mbim_subscriber_ready_status_cb, sim, NULL)) {
		mbim_message_unref(message);
		return -EIO;
	}

	sd = l_new(struct sim_data, 1);
	sd->device = mbim_device_ref(device);
	ofono_sim_set_data(sim, sd);

	return 0;
}

static void mbim_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	mbim_device_cancel_group(sd->device, SIM_GROUP);
	mbim_device_unregister_group(sd->device, SIM_GROUP);
	mbim_device_unref(sd->device);
	sd->device = NULL;

	l_free(sd->iccid);
	l_free(sd->imsi);

	for (int i = 0; i < sd->app_count; i++) {
		l_free(sd->apps[i].label);
	}
	l_free(sd->apps);

	l_free(sd);
}

static const struct ofono_sim_driver driver = {
	.probe			= mbim_sim_probe,
	.remove			= mbim_sim_remove,
	.read_file_info 	= mbim_read_file_info,
	.read_file_transparent 	= mbim_read_file_transparent,
	.read_file_cyclic	= mbim_read_file_fixed_cyclic,
	.read_file_linear	= mbim_read_file_fixed_cyclic,
	.read_imsi		= mbim_read_imsi,
	.query_passwd_state	= mbim_pin_query,
	.query_pin_retries	= mbim_pin_retries_query,
	.send_passwd		= mbim_pin_enter,
	.reset_passwd		= mbim_puk_enter,
	.change_passwd		= mbim_pin_change,
	.lock			= mbim_pin_enable,
};

OFONO_ATOM_DRIVER_BUILTIN(sim, mbim, &driver)
