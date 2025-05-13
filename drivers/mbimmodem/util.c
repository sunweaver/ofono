/*
 * oFono - Open Source Telephony
 * Copyright (C) 2017  Intel Corporation
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdint.h>
#include <stdbool.h>

#include "src/common.h"
#include "simutil.h"
#include "mbim.h"
#include "util.h"

int mbim_data_class_to_tech(uint32_t n)
{
	if (n & MBIM_DATA_CLASS_LTE)
		return ACCESS_TECHNOLOGY_EUTRAN;

	if (n & (MBIM_DATA_CLASS_HSUPA | MBIM_DATA_CLASS_HSDPA))
		return ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;

	if (n & MBIM_DATA_CLASS_HSUPA)
		return ACCESS_TECHNOLOGY_UTRAN_HSUPA;

	if (n & MBIM_DATA_CLASS_HSDPA)
		return ACCESS_TECHNOLOGY_UTRAN_HSDPA;

	if (n & MBIM_DATA_CLASS_UMTS)
		return ACCESS_TECHNOLOGY_UTRAN;

	if (n & MBIM_DATA_CLASS_EDGE)
		return ACCESS_TECHNOLOGY_GSM_EGPRS;

	if (n & MBIM_DATA_CLASS_GPRS)
		return ACCESS_TECHNOLOGY_GSM;

	return -1;
}

uint8_t *mbim_get_fileid(uint32_t fileid)
{
	/* TODO: Handle MF paths for different app types */

	/* File IDs are always 4 bytes */
	uint8_t *out = l_malloc(4);
	uint32_t mf_path;

	switch (fileid) {
		case SIM_EF_ICCID_FILEID:
		case SIM_EFPL_FILEID:
			mf_path = SIM_MF_FILEID;
			break;
		case SIM_EFIMG_FILEID:
			mf_path = 0x5F50;
			break;
		case SIM_EFADN_FILEID:
			mf_path = 0x7F10;
			break;
		case SIM_EFPHASE_FILEID:
			mf_path = 0x0000;
			break;
		default:
			mf_path = 0x7FFF;
			break;
	}

	/* Serialize the IDs into one byte array */
	out[0] = (mf_path >> 8) & 0xFF;
	out[1] = mf_path & 0xFF;
	out[2] = (fileid >> 8) & 0xFF;
	out[3] = fileid & 0xFF;

	return out;
}