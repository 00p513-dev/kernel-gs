 * Copyright 2019 Google LLC
 *
 */

#ifndef __TCPCI_MAX77759_H
#define __TCPCI_MAX77759_H

#include <linux/interrupt.h>
#include <linux/usb/tcpm.h>

#include "usb_psy.h"

struct gvotable_election;
struct logbuffer;
struct max77759_contaminant;
struct tcpci_data;

struct max77759_plat {
	struct tcpci_data data;
	struct tcpci *tcpci;
	struct device *dev;
	struct bc12_status *bc12;
	struct i2c_client *client;
	struct power_supply *usb_psy;
	struct max77759_contaminant *contaminant;
	struct gvotable_election *usb_icl_proto_el;
	struct gvotable_election *charger_mode_votable;
	bool vbus_enabled;
	/* Data role notified to the data stack */
	enum typec_data_role active_data_role;
	/* Data role from the TCPM stack */
	enum typec_data_role data_role;
	/* protects tcpc_enable_data_path */
	struct mutex data_path_lock;
	/* Vote for data from BC1.2 */
	bool bc12_data_capable;
	/* Infered from pd caps */
	bool pd_data_capable;
	/* Vote from TCPC for attached */
	bool attached;
	/* Reflects the signal sent out to the data stack */
	bool data_active;
	/* Reflects whether the current partner can do PD */
	bool pd_capable;
	void *usb_psy_data;
	struct mutex icl_proto_el_lock;
	/* Set vbus voltage alarms */
	bool set_voltage_alarm;
	unsigned int vbus_mv;
	/* USB Data notification */
	struct extcon_dev *extcon;
	bool no_bc_12;
	struct tcpm_port *port;
	struct usb_psy_ops psy_ops;
	/* True when TCPC is in SINK DEBUG ACCESSORY CONNECTED state */
	u8 debug_acc_connected:1;

	struct logbuffer *log;
};

/*
 * bc1.2 registration
 */

struct max77759_usb;

void register_tcpc(struct max77759_usb *usb, struct max77759_plat *chip);

#define MAXQ_DETECT_TYPE_CC_AND_SBU	0x10
#define MAXQ_DETECT_TYPE_SBU_ONLY	0x30

#ifdef MAX77759_CONTAMINANT_MAXQ
int maxq_query_contaminant(u8 cc1_raw, u8 cc2_raw, u8 sbu1_raw, u8 sbu2_raw,
			   u8 cc1_rd, u8 cc2_rd, u8 type,
			   u8 cc_adc_skipped);
#else
static inline int maxq_query_contaminant(u8 cc1_raw, u8 cc2_raw, u8 sbu1_raw,
					 u8 sbu2_raw, u8 cc1_rd, u8 cc2_rd,
					 u8 type, u8 cc_adc_skipped)
{
	return -EINVAL;
}
#endif

struct max77759_contaminant *
max77759_contaminant_init(struct max77759_plat *plat, bool enable);

bool process_contaminant_alert(struct max77759_contaminant *contaminant);
int enable_contaminant_detection(struct max77759_plat *chip);

#define VBUS_VOLTAGE_MASK		0x3ff
#define VBUS_VOLTAGE_LSB_MV		25
#define VBUS_HI_HEADROOM_MV		500
#define VBUS_LO_MV			4500

#endif /* __TCPCI_MAX77759_H */
