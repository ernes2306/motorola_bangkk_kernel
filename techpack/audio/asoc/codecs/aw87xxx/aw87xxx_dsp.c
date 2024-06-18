/*
 * aw87xxx_dsp.c
 *
 * Copyright (c) 2021 AWINIC Technology CO., LTD
 *
 * Author: Barry <zhaozhongbo@awinic.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/version.h>
#include "aw87xxx_log.h"
#include "aw87xxx_dsp.h"

/***************dsp communicate**************/
#ifdef AW_QCOM_OPEN_DSP_PLATFORM
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 1))
#include <dsp/msm_audio_ion.h>
#include <dsp/q6afe-v2.h>
#include <dsp/q6audio-v2.h>
#include <dsp/q6adm-v2.h>
#else
#include <linux/msm_audio_ion.h>
#include <sound/q6afe-v2.h>
#include <sound/q6audio-v2.h>
#include <sound/q6adm-v2.h>
#include <sound/adsp_err.h>
#endif
#endif

static DEFINE_MUTEX(g_dsp_lock);
static unsigned int g_spin_value;

static int g_rx_topo_id = AW_RX_DEFAULT_TOPO_ID;
static int g_rx_port_id = AW_RX_DEFAULT_PORT_ID;

#define AW_COPP_MODULE_ID		(0X10013D02)	/*SKT module id*/
#define AW_COPP_PARAMS_ID_AWDSP_ENABLE	(0X10013D14)	/*SKT enable param id*/

#ifdef AW_MTK_OPEN_DSP_PLATFORM
extern int mtk_spk_send_ipi_buf_to_dsp(void *data_buffer,
				uint32_t data_size);
extern int mtk_spk_recv_ipi_buf_from_dsp(int8_t *buffer,
				int16_t size, uint32_t *buf_len);
/*
static int mtk_spk_send_ipi_buf_to_dsp(void *data_buffer,
				uint32_t data_size)
{
	AW_LOGI("enter");
	return 0;
}

static int mtk_spk_recv_ipi_buf_from_dsp(int8_t *buffer,
				int16_t size, uint32_t *buf_len)
{
	AW_LOGI("enter");
	return 0;
}
*/
#elif defined AW_QCOM_OPEN_DSP_PLATFORM
extern int afe_get_topology(int port_id);
extern int aw_send_afe_cal_apr(uint32_t param_id,
	void *buf, int cmd_size, bool write);
/*
static int afe_get_topology(int port_id)
{
	return -EPERM;
}

static int aw_send_afe_cal_apr(uint32_t param_id,
	void *buf, int cmd_size, bool write)
{
	AW_LOGI("enter, no define AWINIC_ADSP_ENABLE", __func__);
	return 0;
}
*/
#endif

#ifdef AW_QCOM_OPEN_DSP_PLATFORM
extern void aw_set_port_id(int rx_port_id);
#else
static void aw_set_port_id(int rx_port_id)
{
	return;
}
#endif

static int aw_adm_param_enable(int port_id, int module_id, int param_id, int enable)
{
	/*for v3*/
	int copp_idx = 0;
	uint32_t enable_param;
	struct param_hdr_v3 param_hdr;
	int rc = 0;

	pr_info("%s Awinic port_id 0x%x, module_id 0x%x, enable %d\n",
			__func__, port_id, module_id, enable);

	copp_idx = adm_get_default_copp_idx(port_id);
	if (copp_idx < 0 || copp_idx >= MAX_COPPS_PER_PORT) {
			pr_err("%s: Invalid copp_num: %d\n", __func__, copp_idx);
			return -EINVAL;
	}

	if (enable < 0 || enable > 1) {
			pr_err("%s: Invalid value for enable %d\n", __func__, enable);
			return -EINVAL;
	}

	pr_info("%s Awinic port_id 0x%x, module_id 0x%x, copp_idx 0x%x, enable %d\n",
			__func__, port_id, module_id, copp_idx, enable);

	memset(&param_hdr, 0, sizeof(param_hdr));
	param_hdr.module_id = module_id;
	param_hdr.instance_id = INSTANCE_ID_0;
	param_hdr.param_id = param_id;
	param_hdr.param_size = sizeof(enable_param);
	enable_param = enable;

	rc = adm_pack_and_set_one_pp_param(port_id, copp_idx, param_hdr,
					(uint8_t *) &enable_param);
	if (rc)
		pr_err("%s: Awinic Failed to set enable of module(%d) instance(%d) to %d, err %d\n",
				__func__, module_id, INSTANCE_ID_0, enable, rc);
	return rc;
}

uint8_t aw87xxx_dsp_isEnable(void)
{
#if (defined AW_QCOM_OPEN_DSP_PLATFORM) || (defined AW_MTK_OPEN_DSP_PLATFORM)
	return true;
#else
	return false;
#endif
}

/*****************mtk dsp communication function start**********************/
#ifdef AW_MTK_OPEN_DSP_PLATFORM
static int aw_mtk_write_data_to_dsp(int32_t param_id,
			void *data, int size)
{
	int32_t *dsp_data = NULL;
	mtk_dsp_hdr_t *hdr = NULL;
	int ret;

	dsp_data = kzalloc(sizeof(mtk_dsp_hdr_t) + size, GFP_KERNEL);
	if (!dsp_data) {
		AW_LOGE("kzalloc dsp_msg error");
		return -ENOMEM;
	}

	hdr = (mtk_dsp_hdr_t *)dsp_data;
	hdr->type = DSP_MSG_TYPE_DATA;
	hdr->opcode_id = param_id;
	hdr->version = AW_DSP_MSG_HDR_VER;

	memcpy(((char *)dsp_data) + sizeof(mtk_dsp_hdr_t),
		data, size);

	ret = mtk_spk_send_ipi_buf_to_dsp(dsp_data,
				sizeof(mtk_dsp_hdr_t) + size);
	if (ret < 0) {
		AW_LOGE("write data failed");
		kfree(dsp_data);
		dsp_data = NULL;
		return ret;
	}

	kfree(dsp_data);
	dsp_data = NULL;
	return 0;
}

static int aw_mtk_read_data_from_dsp(int32_t param_id, void *data,
					int data_size)
{
	int ret;
	mtk_dsp_hdr_t hdr;

	mutex_lock(&g_dsp_lock);
	hdr.type = DSP_MSG_TYPE_CMD;
	hdr.opcode_id = param_id;
	hdr.version = AW_DSP_MSG_HDR_VER;

	ret = mtk_spk_send_ipi_buf_to_dsp(&hdr, sizeof(mtk_dsp_hdr_t));
	if (ret < 0)
		goto failed;

	ret = mtk_spk_recv_ipi_buf_from_dsp(data, data_size, &data_size);
	if (ret < 0)
		goto failed;

	mutex_unlock(&g_dsp_lock);
	return 0;

failed:
	mutex_unlock(&g_dsp_lock);
	return ret;
}

#endif
/********************mtk dsp communication function end***********************/

/******************qcom dsp communication function start**********************/
#ifdef AW_QCOM_OPEN_DSP_PLATFORM
static int aw_check_dsp_ready(void)
{
	int ret;

	ret = afe_get_topology(g_rx_port_id);
	AW_LOGD("topo_id 0x%x", ret);

	if (ret != g_rx_topo_id)
		AW_LOGE("topo id 0x%x", ret);
	return ret;

}

static int aw_qcom_write_data_to_dsp(int32_t param_id,
				void *data, int data_size)
{
	int ret = -1;
	int topo = -1;

	AW_LOGI("enter");
	mutex_lock(&g_dsp_lock);
	topo = aw_check_dsp_ready();
	if (topo != 0)
		ret = aw_send_afe_cal_apr(param_id, data,
			data_size, true);

	mutex_unlock(&g_dsp_lock);
	return ret;
}

static int aw_qcom_read_data_from_dsp(int32_t param_id,
				void *data, int data_size)
{
	int ret = -1;
	int topo = -1;

	AW_LOGI("enter");
	mutex_lock(&g_dsp_lock);
	topo = aw_check_dsp_ready();
	if (topo != 0)
		ret = aw_send_afe_cal_apr(param_id, data,
			data_size, false);
	mutex_unlock(&g_dsp_lock);
	return ret;
}

#endif
/*****************qcom dsp communication function end*********************/

/*****************read/write msg communication function*********************/
static int aw_write_data_to_dsp(int32_t param_id, void *data, int data_size)
{
#if defined AW_QCOM_OPEN_DSP_PLATFORM
	return aw_qcom_write_data_to_dsp(param_id, data, data_size);
#elif defined AW_MTK_OPEN_DSP_PLATFORM
	return aw_mtk_write_data_to_dsp(param_id, data, data_size);
#else
	return -EINVAL;
#endif
}

static int aw_read_data_from_dsp(int32_t param_id, void *data, int data_size)
{
#if defined AW_QCOM_OPEN_DSP_PLATFORM
	return aw_qcom_read_data_from_dsp(param_id, data, data_size);
#elif defined AW_MTK_OPEN_DSP_PLATFORM
	return aw_mtk_read_data_from_dsp(param_id, data, data_size);
#else
	return -EINVAL;
#endif
}

/***************read/write msg communication function end*******************/

int aw87xxx_dsp_get_rx_module_enable(int *enable)
{
	if (!enable) {
		AW_LOGE("enable is NULL");
		return -EINVAL;
	}

	return aw_read_data_from_dsp(AWDSP_RX_SET_ENABLE,
			(void *)enable, sizeof(uint32_t));
}

int aw87xxx_dsp_set_rx_module_enable(int enable)
{
	switch (enable) {
	case AW_RX_MODULE_DISENABLE:
		AW_LOGD("set enable=%d", enable);
		break;
	case AW_RX_MODULE_ENABLE:
		AW_LOGD("set enable=%d", enable);
		break;
	default:
		AW_LOGE("unsupport enable=%d", enable);
		return -EINVAL;
	}

	return aw_write_data_to_dsp(AWDSP_RX_SET_ENABLE,
			&enable, sizeof(uint32_t));
}

int aw87xxx_dsp_set_copp_module_enable(bool enable)
{
	int ret;

	ret = aw_adm_param_enable(g_rx_port_id, AW_COPP_MODULE_ID,
			AW_COPP_PARAMS_ID_AWDSP_ENABLE, enable);
	if (ret)
		return -EINVAL;

	AW_LOGI("set skt %s", enable == 1 ? "enable" : "disable");
	return 0;
}

int aw87xxx_dsp_get_vmax(uint32_t *vmax, int dev_index)
{
	int32_t param_id = 0;

	switch (dev_index % AW_DSP_CHANNEL_MAX) {
	case AW_DSP_CHANNEL_0:
		param_id = AWDSP_RX_VMAX_0;
		break;
	case AW_DSP_CHANNEL_1:
		param_id = AWDSP_RX_VMAX_1;
		break;
	default:
		AW_LOGE("algo only support double PA channel:%d unsupport",
			dev_index);
		return -EINVAL;
	}

	return aw_read_data_from_dsp(param_id,
			(void *)vmax, sizeof(uint32_t));
}

int aw87xxx_dsp_set_vmax(uint32_t vmax, int dev_index)
{
	int32_t param_id = 0;

	switch (dev_index % AW_DSP_CHANNEL_MAX) {
	case AW_DSP_CHANNEL_0:
		param_id = AWDSP_RX_VMAX_0;
		break;
	case AW_DSP_CHANNEL_1:
		param_id = AWDSP_RX_VMAX_1;
		break;
	default:
		AW_LOGE("algo only support double PA channel:%d unsupport",
			dev_index);
		return -EINVAL;
	}

	return aw_write_data_to_dsp(param_id, &vmax, sizeof(uint32_t));
}

int aw87xxx_dsp_set_spin(uint32_t ctrl_value)
{
	int ret = 0;

	if (ctrl_value >= AW_SPIN_MAX) {
		AW_LOGE("spin [%d] unsupported ", ctrl_value);
		return -EINVAL;
	}
	ret = aw_write_data_to_dsp(AW_MSG_ID_SPIN, &ctrl_value,
		sizeof(uint32_t));
	if (ret) {
		AW_LOGE("spin [%d] set failed ", ctrl_value);
		return ret;
	}

	g_spin_value = ctrl_value;
	return 0;
}

int aw87xxx_dsp_get_spin(void)
{
	return g_spin_value;
}

int aw87xxx_spin_set_record_val(void)
{
	AW_LOGD("record write spin enter");

	return aw87xxx_dsp_set_spin(g_spin_value);
}
EXPORT_SYMBOL(aw87xxx_spin_set_record_val);

void aw87xxx_device_parse_topo_id_dt(struct aw_device *aw_dev)
{
	int ret;

	ret = of_property_read_u32(aw_dev->dev->of_node, "aw-rx-topo-id", &g_rx_topo_id);
	if (ret < 0) {
		g_rx_topo_id = AW_RX_DEFAULT_TOPO_ID;
		AW_DEV_LOGI(aw_dev->dev, "read aw-rx-topo-id failed,use default");
	}

	AW_DEV_LOGI(aw_dev->dev, "rx-topo-id: 0x%x",  g_rx_topo_id);
}

void aw87xxx_device_parse_port_id_dt(struct aw_device *aw_dev)
{
	int ret;

	ret = of_property_read_u32(aw_dev->dev->of_node, "aw-rx-port-id", &g_rx_port_id);
	if (ret < 0) {
		g_rx_port_id = AW_RX_DEFAULT_PORT_ID;
		AW_DEV_LOGI(aw_dev->dev, "read aw-rx-port-id failed,use default");
	}

	aw_set_port_id(g_rx_port_id);
	AW_DEV_LOGI(aw_dev->dev, "rx-port-id: 0x%x", g_rx_port_id);
}
