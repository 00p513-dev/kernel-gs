// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019, Google Inc
 *
 * USB input current management.
 *
 */

#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <misc/gvotable.h>
#include <misc/logbuffer.h>
#include <uapi/linux/sched/types.h>

#include "usb_psy.h"
#include "usb_icl_voter.h"

#define ONLINE_THRESHOLD_UA 125000

#define CDP_DCP_ICL_UA	1500000
/* TODO: needs to be 100mA when SDP_CONFIGURED is voted */
#define SDP_ICL_UA	500000

/* At least get one more try to meet sub state sync requirement */
#define ERR_RETRY_DELAY_MS 20
#define ERR_RETRY_COUNT    3

struct usb_psy_data {
	struct logbuffer *log;

	struct i2c_client *tcpc_client;
	struct power_supply *usb_psy;
	struct power_supply *chg_psy;
	struct power_supply *main_chg_psy;
	enum power_supply_usb_type usb_type;

	/* Casts final vote on usb psy current max */
	struct gvotable_election *usb_icl_el;
	/* Combines the values from thermald and protocol stack */
	struct gvotable_election *usb_icl_combined_el;
	/*
	 * Combines the values from various voters of the protocol stack
	 * such as PD, BC1.2, TYPE, DATA stack.
	 */
	struct gvotable_election *usb_icl_proto_el;

	/* Cached/Request usb ilim to charger psy */
	int current_max_cache;

	struct usb_psy_ops *psy_ops;

	/* For voting current limit */
	char *chg_psy_name;
	/* For reading USB current now */
	char *main_chg_psy_name;

	/*
	 * Setting CURRENT limit on charger side can fail.
	 * Implement retry mechanism.
	 * Needs to be at RT priority to confirm to Type-C timing
	 * constraints.
	 */
	struct kthread_worker *wq;
	struct kthread_delayed_work icl_work;
	atomic_t retry_count;

	/* sink connected state from Type-C */
	bool sink_enabled;
};

void init_vote(struct usb_vote *vote, const char *reason,
	       unsigned int priority, unsigned int val)
{
	strncpy(vote->reason, reason, sizeof(vote->reason));
	vote->priority = priority;
	vote->val = val;
}
EXPORT_SYMBOL_GPL(init_vote);

static int usb_get_current_max_ma(struct usb_psy_data *usb)
{
	union power_supply_propval val = {0};
	int ret;
	struct i2c_client *client = usb->tcpc_client;

	if (!usb->chg_psy_name)
		return usb->current_max_cache;

	if (!usb->chg_psy) {
		usb->chg_psy = power_supply_get_by_name(usb->chg_psy_name);
		if (IS_ERR_OR_NULL(usb->chg_psy)) {
			dev_err(&client->dev, "chg psy not up\n");
			return usb->current_max_cache;
		}
	}

	ret = power_supply_get_property(usb->chg_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX, &val);
	if (ret < 0)
		return ret;

	return val.intval;
}

static int usb_set_current_max_ma(struct usb_psy_data *usb,
				  int current_max)
{
	union power_supply_propval val = {0};
	int ret;
	struct i2c_client *client = usb->tcpc_client;

	if (!usb->chg_psy) {
		usb->chg_psy = power_supply_get_by_name(usb->chg_psy_name);
		if (IS_ERR_OR_NULL(usb->chg_psy)) {
			dev_err(&client->dev, "chg psy not up\n");
			return 0;
		}
	}

	val.intval = current_max;
	ret = power_supply_set_property(usb->chg_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX,
					&val);

	logbuffer_log(usb->log, "set input max current %d to %s, ret=%d",
		      current_max, usb->chg_psy_name, ret);
	return ret;
}

static void set_bc_current_limit(struct gvotable_election *usb_icl_proto_el,
				 enum power_supply_usb_type usb_type,
				 struct logbuffer *log)
{
	int ret;
	struct usb_vote vote;
	bool vote_enable;

	switch (usb_type) {
	case POWER_SUPPLY_USB_TYPE_CDP:
	case POWER_SUPPLY_USB_TYPE_DCP:
		init_vote(&vote, proto_voter_reason[BC12_CDP_DCP],
			  BC12_CDP_DCP, CDP_DCP_ICL_UA);
		vote_enable = true;
		break;
	case POWER_SUPPLY_USB_TYPE_SDP:
		init_vote(&vote, proto_voter_reason[BC12_SDP],
			  BC12_SDP, SDP_ICL_UA);
		vote_enable = true;
		break;
	default:
		vote_enable = false;
		break;
	}

	/** Disable all votes for unknown type **/
	if (!vote_enable) {
		init_vote(&vote, proto_voter_reason[BC12_CDP_DCP],
			  BC12_CDP_DCP, 0);
		ret = gvotable_cast_vote(usb_icl_proto_el, vote.reason, &vote,
					 false);
		logbuffer_log(log,
			      "%s: %s disabling DCP vote usb proto_el: %d by %s"
			      , __func__, ret < 0 ? "error" : "",  vote.val
			      , vote.reason);

		init_vote(&vote, proto_voter_reason[BC12_SDP],
			  BC12_SDP, 0);
		ret = gvotable_cast_vote(usb_icl_proto_el, vote.reason, &vote,
					 false);
		logbuffer_log(log,
			      "%s: %s disabling SDP vote usb proto_el: %d by %s"
			      , __func__, ret < 0 ? "error" : "",  vote.val
			      , vote.reason);
		return;
	}

	ret = gvotable_cast_vote(usb_icl_proto_el, vote.reason, &vote,
				 true);
	logbuffer_log(log, "%s: %s voting usb proto_el: %d by %s",
		      __func__, ret < 0 ? "error" : "",  vote.val,
		      vote.reason);
}

static int usb_psy_current_now_ma(struct usb_psy_data *usb, int *current_now)
{
	union power_supply_propval val;
	int ret;

	if (IS_ERR_OR_NULL(usb->main_chg_psy)) {
		if (!usb->main_chg_psy_name) {
			logbuffer_log(usb->log, "main-chg-psy-name not set\n");
			return -EINVAL;
		}
		usb->main_chg_psy = power_supply_get_by_name(usb->main_chg_psy_name);
		if (IS_ERR_OR_NULL(usb->main_chg_psy))
			return -EAGAIN;
	}

	ret = power_supply_get_property(usb->main_chg_psy, POWER_SUPPLY_PROP_CURRENT_NOW, &val);
	if (!ret)
		*current_now = val.intval;

	return ret;
}

static int usb_psy_data_get_prop(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct usb_psy_data *usb = power_supply_get_drvdata(psy);
	struct usb_psy_ops *ops = usb->psy_ops;
	struct i2c_client *client = usb->tcpc_client;
	int ret;

	if (!usb)
		return 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = usb->sink_enabled ? usb_get_current_max_ma(usb) > ONLINE_THRESHOLD_UA
			? 1 : 0 : 0;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = usb->sink_enabled;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		/* Report the voted value to reflect TA capability */
		val->intval = usb->current_max_cache;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		/* Report in uv */
		val->intval = ops->tcpc_get_vbus_voltage_max_mv(client) * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = usb_psy_current_now_ma(usb, &val->intval);
		if (ret < 0)
			return ret;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = ops->tcpc_get_vbus_voltage_mv(client);
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = usb->usb_type;
		break;
	default:
		break;
	}

	return 0;
}

static int usb_psy_data_set_prop(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	struct usb_psy_data *usb = power_supply_get_drvdata(psy);
	int ret;
	struct usb_psy_ops *ops = usb->psy_ops;
	struct i2c_client *client = usb->tcpc_client;

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		usb->current_max_cache = val->intval;
		atomic_set(&usb->retry_count, ERR_RETRY_COUNT);
		ret = kthread_mod_delayed_work(usb->wq, &usb->icl_work, 0);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		/* Enough to trigger just the uevent */
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		usb->usb_type = val->intval;
		ops->tcpc_set_port_data_capable(client, usb->usb_type);
		set_bc_current_limit(usb->usb_icl_proto_el, usb->usb_type,
				     usb->log);
		break;
	default:
		break;
	}
	power_supply_changed(usb->usb_psy);

	return 0;
}

void usb_psy_set_sink_state(void *usb_psy, bool enabled)
{
	struct usb_psy_data *usb = usb_psy;

	if (!usb || !usb->usb_psy)
		return;

	usb->sink_enabled = enabled;
	power_supply_changed(usb->usb_psy);
}
EXPORT_SYMBOL_GPL(usb_psy_set_sink_state);

//todo: Expose settled ICL limit
static enum power_supply_property usb_psy_data_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_PRESENT,
};

static enum power_supply_usb_type usb_psy_data_types[] = {
	POWER_SUPPLY_USB_TYPE_UNKNOWN,
	POWER_SUPPLY_USB_TYPE_SDP,
	POWER_SUPPLY_USB_TYPE_CDP,
	POWER_SUPPLY_USB_TYPE_DCP,
};

static const struct power_supply_desc usb_psy_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = usb_psy_data_types,
	.num_usb_types = ARRAY_SIZE(usb_psy_data_types),
	.properties = usb_psy_data_props,
	.num_properties = ARRAY_SIZE(usb_psy_data_props),
	.get_property = usb_psy_data_get_prop,
	.set_property = usb_psy_data_set_prop,
};

static int vote_comp(struct usb_vote *v1, struct usb_vote *v2)
{
	int v1_pri = v1->priority, v2_pri = v2->priority;

	/** Separate WARN_ON's to triage failures **/
	WARN_ON(!v1);
	WARN_ON(!v2);

	if (v1_pri == v2_pri)
		return v2->val - v1->val;

	return v2_pri - v1_pri;
}

static void usb_icl_callback(struct gvotable_election *el,
			     const char *reason, void *result)
{
	struct usb_psy_data *usb = gvotable_get_data(el);
	union power_supply_propval val = {.intval = (unsigned int)result};
	int ret;

	ret = power_supply_set_property(usb->usb_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX, &val);
	logbuffer_log(usb->log,
		      "%s: %s:%d setting PROP_CURRRENT_MAX: %d by %s",
		      __func__, ret < 0 ? "error" : "success", ret,
		      val.intval, reason);
}

static void usb_icl_combined_callback(struct gvotable_election *el,
				      const char *reason, void *result)
{
	struct usb_psy_data *usb = gvotable_get_data(el);
	struct usb_vote *vote_result = result;
	int ret;

	ret = gvotable_cast_vote(usb->usb_icl_el,
				 icl_voter_reason[USB_ICL_COMB],
				 (void *)(long)vote_result->val, true);
	logbuffer_log(usb->log, "%s: %s:%d voting usb_icl_el: %d by %s",
		      __func__, ret < 0 ? "error" : "success", ret,
		      vote_result->val, icl_voter_reason[USB_ICL_COMB]);
}

/*
 * MIN between USB_ICL_THERMAL_VOTER and USB_ICL_PROTO_VOTER
 */
static int usb_icl_combined_comp(void *vote1, void *vote2)
{
	struct usb_vote *vote1_result = vote1, *vote2_result = vote2;

	if (vote1_result->val < vote2_result->val)
		return 1;
	else if (vote1_result->val > vote2_result->val)
		return -1;
	else
		return 0;
}

static void usb_icl_proto_callback(struct gvotable_election *el,
				   const char *reason, void *result)
{
	struct usb_psy_data *usb = gvotable_get_data(el);
	struct usb_vote *vote_result = result;
	struct usb_vote vote = {
		.reason = USB_ICL_PROTO_VOTER,
		.val = 0,
	};
	int ret;

	/*
	 * All the votes have been disabled when the result is null.
	 * Vote for 0 in that case.
	 * If not vote the value from the result.
	 */
	if (result)
		vote.val = vote_result->val;

	ret = gvotable_cast_vote(usb->usb_icl_combined_el,
				 USB_ICL_PROTO_VOTER, &vote, true);
	logbuffer_log(usb->log,
		      "%s: %s:%d voting usb_icl_combined_el: %d by %s",
		      __func__, ret < 0 ? "error" : "success",
		      ret, vote.val, vote.reason);
}

/*
 * Priority based voting. USB_ICL_DATA_SUSPEND has the highest priority.
 *
 */
static inline int usb_icl_proto_comp(void *vote1, void *vote2)
{
	return vote_comp(vote1, vote2);
}

static int debug_print_vote(char *str,  size_t len, const void *vote)
{
	struct usb_vote *usb_vote = (struct usb_vote *)vote;

	if (!vote)
		return 0;

	return scnprintf(str, len, "val:%u priority:%u", (unsigned int)
			 usb_vote->val, (unsigned int)usb_vote->priority);
}

static void icl_work_item(struct kthread_work *work)
{
	struct usb_psy_data *usb =
		container_of(container_of(work, struct kthread_delayed_work, work),
			     struct usb_psy_data, icl_work);

	if (usb_set_current_max_ma(usb, usb->current_max_cache) < 0 &&
	    !atomic_sub_and_test(1, &usb->retry_count))
		kthread_mod_delayed_work(usb->wq, &usb->icl_work,
					 msecs_to_jiffies(ERR_RETRY_DELAY_MS));
}

void *usb_psy_setup(struct i2c_client *client, struct logbuffer *log,
		    struct usb_psy_ops *ops)
{
	struct usb_psy_data *usb;
	struct power_supply_config usb_cfg = {};
	struct device *dev = &client->dev;
	struct device_node *dn;
	void *ret;

	if (!ops)
		return ERR_PTR(-EINVAL);

	if (!ops->tcpc_get_vbus_voltage_max_mv ||
	    !ops->tcpc_set_vbus_voltage_max_mv ||
	    !ops->tcpc_get_vbus_voltage_mv ||
	    !ops->tcpc_set_port_data_capable)
		return ERR_PTR(-EINVAL);

	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return ERR_PTR(-ENOMEM);

	usb->tcpc_client = client;
	usb->log = log;
	usb->psy_ops = ops;

	dn = dev_of_node(dev);
	if (!dn) {
		dev_err(&client->dev, "of node not found\n");
		return ERR_PTR(-EINVAL);
	}

	usb->chg_psy_name = (char *)of_get_property(dn, "chg-psy-name", NULL);
	if (!usb->chg_psy_name) {
		dev_err(&client->dev, "chg-psy-name not set\n");
	} else {
		usb->chg_psy = power_supply_get_by_name(usb->chg_psy_name);
		if (IS_ERR_OR_NULL(usb->chg_psy))
			dev_err(&client->dev, "chg psy not up\n");
	}

	usb->main_chg_psy_name = (char *)of_get_property(dn, "main-chg-psy-name", NULL);

	usb_cfg.drv_data = usb;
	usb_cfg.of_node =  dev->of_node;
	usb->usb_psy = power_supply_register(dev, &usb_psy_desc, &usb_cfg);
	if (IS_ERR(usb->usb_psy)) {
		dev_err(dev, "usb: Power supply register failed");
		ret = usb->usb_psy;
		goto chg_psy_put;
	}
	usb->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;

	/*
	 * PRIORITY VOTE : will have two voters.
	 * 1. USER
	 * 2. COMBINED
	 */
	usb->usb_icl_el =
		gvotable_create_int_election(USB_ICL_EL,
					     gvotable_comparator_int_min,
					     usb_icl_callback, usb);
	if (IS_ERR_OR_NULL(usb->usb_icl_el)) {
		ret = usb->usb_icl_el;
		goto usb_psy_unreg;
	}
	gvotable_set_vote2str(usb->usb_icl_el, gvotable_v2s_uint);

	/*
	 * MIN VOTE: will have two voters
	 * 1. Thermal
	 * 2. PROTOCOL vote
	 */
	usb->usb_icl_combined_el =
		gvotable_create_election(USB_ICL_COMBINED_EL,
					 sizeof(struct usb_vote),
					 usb_icl_combined_comp,
					 usb_icl_combined_callback, usb);
	if (IS_ERR_OR_NULL(usb->usb_icl_combined_el)) {
		ret = usb->usb_icl_combined_el;
		goto unreg_icl_el;
	}
	gvotable_set_vote2str(usb->usb_icl_combined_el, debug_print_vote);

	/*
	 * PRIORITY VOTE: Various players of the protocol stack such as
	 * PD, BC12, TYPEC, DEAD_BATTERY etc.
	 */
	usb->usb_icl_proto_el =
		gvotable_create_election(USB_ICL_PROTO_EL,
					 sizeof(struct usb_vote),
					 usb_icl_proto_comp,
					 usb_icl_proto_callback, usb);
	if (IS_ERR_OR_NULL(usb->usb_icl_proto_el)) {
		ret = usb->usb_icl_proto_el;
		goto unreg_icl_combined_el;
	}
	gvotable_set_vote2str(usb->usb_icl_proto_el, debug_print_vote);

	usb->wq = kthread_create_worker(0, "wq-tcpm-usb-psy");
	if (IS_ERR_OR_NULL(usb->wq)) {
		ret = usb->wq;
		goto unreg_icl_proto_el;
	}

	kthread_init_delayed_work(&usb->icl_work, icl_work_item);

	return usb;

unreg_icl_proto_el:
	gvotable_destroy_election(usb->usb_icl_proto_el);
unreg_icl_combined_el:
	gvotable_destroy_election(usb->usb_icl_combined_el);
unreg_icl_el:
	gvotable_destroy_election(usb->usb_icl_el);
usb_psy_unreg:
	if (!IS_ERR_OR_NULL(usb->main_chg_psy))
		power_supply_put(usb->main_chg_psy);
	if (!IS_ERR_OR_NULL(usb->usb_psy))
		power_supply_unregister(usb->usb_psy);
chg_psy_put:
	if (!IS_ERR_OR_NULL(usb->chg_psy))
		power_supply_put(usb->chg_psy);

	return ret;
}
EXPORT_SYMBOL_GPL(usb_psy_setup);

void usb_psy_teardown(void *usb_data)
{
	struct usb_psy_data *usb = (struct usb_psy_data *)usb_data;

	kthread_destroy_worker(usb->wq);
	gvotable_destroy_election(usb->usb_icl_proto_el);
	gvotable_destroy_election(usb->usb_icl_combined_el);
	gvotable_destroy_election(usb->usb_icl_el);
	if (!IS_ERR_OR_NULL(usb->chg_psy))
		power_supply_put(usb->chg_psy);
	if (!IS_ERR_OR_NULL(usb->main_chg_psy))
		power_supply_put(usb->main_chg_psy);
	if (!IS_ERR_OR_NULL(usb->usb_psy))
		power_supply_unregister(usb->usb_psy);
}
EXPORT_SYMBOL_GPL(usb_psy_teardown);

MODULE_DESCRIPTION("USB_PSY Module");
MODULE_AUTHOR("Badhri Jagan Sridharan <badhri@google.com>");
MODULE_LICENSE("GPL");
