/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "ble_core.h"
#include "ble_hci_vsc.h"
#include <zephyr.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <sys/byteorder.h>
#include <errno.h>
#include "macros_common.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(ble, CONFIG_LOG_BLE_LEVEL);

#define NET_CORE_RESPONSE_TIMEOUT_MS 500

/* Note that HCI_CMD_TIMEOUT is currently set to 10 seconds in Zephyr */
#define NET_CORE_WATCHDOG_TIME_MS 1000

static ble_core_ready_t m_ready_callback;
static struct bt_le_oob _oob = { .addr = 0 };

static void net_core_timeout_handler(struct k_timer *timer_id);
static void net_core_watchdog_handler(struct k_timer *timer_id);

static struct k_work net_core_ctrl_version_get_work;

K_TIMER_DEFINE(net_core_timeout_alarm_timer, net_core_timeout_handler, NULL);
K_TIMER_DEFINE(net_core_watchdog_timer, net_core_watchdog_handler, NULL);

/* If NET core out of response for a time defined in NET_CORE_RESPONSE_TIMEOUT
 * show error message for indicating user.
 */
static void net_core_timeout_handler(struct k_timer *timer_id)
{
	ERR_CHK_MSG(-EIO, "No response from NET core, check if NET core is programmed");
}

static void mac_print(void)
{
	char dev[BT_ADDR_LE_STR_LEN];
	(void)bt_le_oob_get_local(BT_ID_DEFAULT, &_oob);
	(void)bt_addr_le_to_str(&_oob.addr, dev, BT_ADDR_LE_STR_LEN);
	LOG_INF("MAC: %s", dev);
}

/* Callback called by the Bluetooth stack in Zephyr when Bluetooth is ready */
static void on_bt_ready(int err)
{
	int ret;
	uint16_t ctrl_version = 0;

	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		ERR_CHK(err);
	}

	LOG_DBG("Bluetooth initialized");
	mac_print();

	ret = net_core_ctrl_version_get(&ctrl_version);
	ERR_CHK_MSG(ret, "Failed to get controller version");

	LOG_INF("Controller version: %d", ctrl_version);
	m_ready_callback();
}

static int controller_leds_mapping(void)
{
	int ret;

	ret = ble_hci_vsc_map_led_pin(PAL_LED_ID_CPU_ACTIVE,
				      DT_GPIO_FLAGS_BY_IDX(DT_NODELABEL(rgb2_green), gpios, 0),
				      DT_GPIO_PIN_BY_IDX(DT_NODELABEL(rgb2_green), gpios, 0));
	if (ret) {
		return ret;
	}

	return 0;
}

static void net_core_watchdog_handler(struct k_timer *timer_id)
{
	k_work_submit(&net_core_ctrl_version_get_work);
}

static void work_net_core_ctrl_version_get(struct k_work *work)
{
	int ret;
	uint16_t ctrl_version = 0;

	ret = net_core_ctrl_version_get(&ctrl_version);

	ERR_CHK_MSG(ret, "Failed to get controller version");

	if (!ctrl_version) {
		ERR_CHK_MSG(-EIO, "Failed to contact net core");
	}
}

int net_core_ctrl_version_get(uint16_t *ctrl_version)
{
	int ret;
	struct net_buf *rsp;

	ret = bt_hci_cmd_send_sync(BT_HCI_OP_READ_LOCAL_VERSION_INFO, NULL, &rsp);
	if (ret) {
		return ret;
	}

	struct bt_hci_rp_read_local_version_info *rp = (void *)rsp->data;

	*ctrl_version = sys_le16_to_cpu(rp->hci_revision);

	net_buf_unref(rsp);

	return 0;
}

int ble_core_le_pwr_ctrl_disable(void)
{
	return ble_hci_vsc_set_op_flag(BLE_HCI_VSC_OP_DIS_POWER_MONITOR, 1);
}

int ble_core_init(ble_core_ready_t ready_callback)
{
	int ret;

	if (ready_callback == NULL) {
		return -EINVAL;
	}
	m_ready_callback = ready_callback;

	/* Setup a timer for monitoring if NET core is working or not */
	k_timer_start(&net_core_timeout_alarm_timer, K_MSEC(NET_CORE_RESPONSE_TIMEOUT_MS),
		      K_NO_WAIT);

	/* Enable Bluetooth, with callback function that
	 * will be called when Bluetooth is ready
	 */
	ret = bt_enable(on_bt_ready);
	k_timer_stop(&net_core_timeout_alarm_timer);

	if (ret) {
		LOG_ERR("Bluetooth init failed (ret %d)", ret);
		return ret;
	}

	ret = controller_leds_mapping();
	if (ret) {
		LOG_ERR("Error mapping LED pins to the Bluetooth controller (ret %d)", ret);
		return ret;
	}

	k_work_init(&net_core_ctrl_version_get_work, work_net_core_ctrl_version_get);
	k_timer_start(&net_core_watchdog_timer, K_NO_WAIT, K_MSEC(NET_CORE_WATCHDOG_TIME_MS));

	return 0;
}
