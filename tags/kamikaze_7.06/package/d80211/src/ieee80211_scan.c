/*
 * Copyright 2002-2004, Instant802 Networks, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/skbuff.h>

#include <net/d80211.h>
#include "ieee80211_i.h"
#include "ieee80211_rate.h"


/* Maximum number of seconds to wait for the traffic load to get below
 * threshold before forcing a passive scan. */
#define MAX_SCAN_WAIT 60
/* Threshold (pkts/sec TX or RX) for delaying passive scan */
#define SCAN_TXRX_THRESHOLD 75

static void get_channel_params(struct ieee80211_local *local, int channel,
				struct ieee80211_hw_mode **mode,
				struct ieee80211_channel **chan)
{
	struct ieee80211_hw_mode *m;

	list_for_each_entry(m, &local->modes_list, list) {
		*mode = m;
		if (m->mode == local->hw.conf.phymode)
			break;
	}
	local->scan.mode = m;
	local->scan.chan_idx = 0;
	do {
		*chan = &m->channels[local->scan.chan_idx];
		if ((*chan)->chan == channel)
			return;
		local->scan.chan_idx++;
	} while (local->scan.chan_idx < m->num_channels);
	*chan = NULL;
}


static void next_chan_same_mode(struct ieee80211_local *local,
				struct ieee80211_hw_mode **mode,
				struct ieee80211_channel **chan)
{
	struct ieee80211_hw_mode *m;
	int prev;

	list_for_each_entry(m, &local->modes_list, list) {
		*mode = m;
		if (m->mode == local->hw.conf.phymode)
			break;
	}
	local->scan.mode = m;

	/* Select next channel - scan only channels marked with W_SCAN flag */
	prev = local->scan.chan_idx;
	do {
		local->scan.chan_idx++;
		if (local->scan.chan_idx >= m->num_channels)
			local->scan.chan_idx = 0;
		*chan = &m->channels[local->scan.chan_idx];
		if ((*chan)->flag & IEEE80211_CHAN_W_SCAN)
			break;
	} while (local->scan.chan_idx != prev);
}


static void next_chan_all_modes(struct ieee80211_local *local,
				struct ieee80211_hw_mode **mode,
				struct ieee80211_channel **chan)
{
	struct ieee80211_hw_mode *prev_m;
	int prev;

	/* Select next channel - scan only channels marked with W_SCAN flag */
	prev = local->scan.chan_idx;
	prev_m = local->scan.mode;
	do {
		*mode = local->scan.mode;
		local->scan.chan_idx++;
		if (local->scan.chan_idx >= (*mode)->num_channels) {
			struct list_head *next;

			local->scan.chan_idx = 0;
			next = (*mode)->list.next;
			if (next == &local->modes_list)
				next = next->next;
			*mode = list_entry(next,
					   struct ieee80211_hw_mode,
					   list);
			local->scan.mode = *mode;
		}
		*chan = &(*mode)->channels[local->scan.chan_idx];
		if ((*chan)->flag & IEEE80211_CHAN_W_SCAN)
			break;
	} while (local->scan.chan_idx != prev ||
		 local->scan.mode != prev_m);
}


static void ieee80211_scan_start(struct ieee80211_local *local,
				 struct ieee80211_scan_conf *conf)
{
	struct ieee80211_hw_mode *old_mode = local->scan.mode;
	int old_chan_idx = local->scan.chan_idx;
	struct ieee80211_hw_mode *mode = NULL;
	struct ieee80211_channel *chan = NULL;
	int ret;

	if (!local->ops->passive_scan) {
		printk(KERN_DEBUG "%s: Scan handler called, yet the hardware "
		       "does not support passive scanning. Disabled.\n",
		       local->mdev->name);
		return;
	}

	if ((local->scan.tries < MAX_SCAN_WAIT &&
	     local->scan.txrx_count > SCAN_TXRX_THRESHOLD)) {
		local->scan.tries++;
		/* Count TX/RX packets during one second interval and allow
		 * scan to start only if the number of packets is below the
		 * threshold. */
		local->scan.txrx_count = 0;
		local->scan.timer.expires = jiffies + HZ;
		add_timer(&local->scan.timer);
		return;
	}

	if (!local->scan.skb) {
		printk(KERN_DEBUG "%s: Scan start called even though scan.skb "
		       "is not set\n", local->mdev->name);
	}

	if (local->scan.our_mode_only) {
		if (local->scan.channel > 0) {
			get_channel_params(local, local->scan.channel, &mode,
					   &chan);
		} else
			next_chan_same_mode(local, &mode, &chan);
	}
	else
		next_chan_all_modes(local, &mode, &chan);

	conf->scan_channel = chan->chan;
	conf->scan_freq = chan->freq;
	conf->scan_channel_val = chan->val;
	conf->scan_phymode = mode->mode;
	conf->scan_power_level = chan->power_level;
	conf->scan_antenna_max = chan->antenna_max;
	conf->scan_time = 2 * local->hw.channel_change_time +
		local->scan.time; /* 10ms scan time+hardware changes */
	conf->skb = local->scan.skb ?
		skb_clone(local->scan.skb, GFP_ATOMIC) : NULL;
	conf->tx_control = &local->scan.tx_control;
#if 0
	printk(KERN_DEBUG "%s: Doing scan on mode: %d freq: %d chan: %d "
	       "for %d ms\n",
	       local->mdev->name, conf->scan_phymode, conf->scan_freq,
	       conf->scan_channel, conf->scan_time);
#endif
	local->scan.rx_packets = 0;
	local->scan.rx_beacon = 0;
	local->scan.freq = chan->freq;
	local->scan.in_scan = 1;

	ieee80211_netif_oper(local_to_hw(local), NETIF_STOP);

	ret = local->ops->passive_scan(local_to_hw(local),
				      IEEE80211_SCAN_START, conf);

	if (ret == 0) {
		long usec = local->hw.channel_change_time +
			local->scan.time;
		usec += 1000000L / HZ - 1;
		usec /= 1000000L / HZ;
		local->scan.timer.expires = jiffies + usec;
	} else {
		local->scan.in_scan = 0;
		if (conf->skb)
			dev_kfree_skb(conf->skb);
		ieee80211_netif_oper(local_to_hw(local), NETIF_WAKE);
		if (ret == -EAGAIN) {
			local->scan.timer.expires = jiffies +
				(local->scan.interval * HZ / 100);
			local->scan.mode = old_mode;
			local->scan.chan_idx = old_chan_idx;
		} else {
			printk(KERN_DEBUG "%s: Got unknown error from "
			       "passive_scan %d\n", local->mdev->name, ret);
			local->scan.timer.expires = jiffies +
				(local->scan.interval * HZ);
		}
		local->scan.in_scan = 0;
	}

	add_timer(&local->scan.timer);
}


static void ieee80211_scan_stop(struct ieee80211_local *local,
				struct ieee80211_scan_conf *conf)
{
	struct ieee80211_hw_mode *mode;
	struct ieee80211_channel *chan;
	int wait;

	if (!local->ops->passive_scan)
		return;

	mode = local->scan.mode;

	if (local->scan.chan_idx >= mode->num_channels)
		local->scan.chan_idx = 0;

	chan = &mode->channels[local->scan.chan_idx];

	local->ops->passive_scan(local_to_hw(local), IEEE80211_SCAN_END,
				conf);

#ifdef CONFIG_D80211_VERBOSE_DEBUG
	printk(KERN_DEBUG "%s: Did scan on mode: %d freq: %d chan: %d "
	       "GOT: %d Beacon: %d (%d)\n",
	       local->mdev->name,
	       mode->mode, chan->freq, chan->chan,
	       local->scan.rx_packets, local->scan.rx_beacon,
	       local->scan.tries);
#endif /* CONFIG_D80211_VERBOSE_DEBUG */
	local->scan.num_scans++;

	local->scan.in_scan = 0;
	ieee80211_netif_oper(local_to_hw(local), NETIF_WAKE);

	local->scan.tries = 0;
	/* Use random interval of scan.interval .. 2 * scan.interval */
	wait = (local->scan.interval * HZ * ((net_random() & 127) + 128)) /
		128;
	local->scan.timer.expires = jiffies + wait;

	add_timer(&local->scan.timer);
}


static void ieee80211_scan_handler(unsigned long ullocal)
{
	struct ieee80211_local *local = (struct ieee80211_local *) ullocal;
	struct ieee80211_scan_conf conf;

	if (local->scan.interval == 0 && !local->scan.in_scan) {
		/* Passive scanning is disabled - keep the timer always
		 * running to make code cleaner. */
		local->scan.timer.expires = jiffies + 10 * HZ;
		add_timer(&local->scan.timer);
		return;
	}

	memset(&conf, 0, sizeof(struct ieee80211_scan_conf));
	conf.running_freq = local->hw.conf.freq;
	conf.running_channel = local->hw.conf.channel;
        conf.running_phymode = local->hw.conf.phymode;
	conf.running_channel_val = local->hw.conf.channel_val;
        conf.running_power_level = local->hw.conf.power_level;
        conf.running_antenna_max = local->hw.conf.antenna_max;

	if (local->scan.in_scan == 0)
		ieee80211_scan_start(local, &conf);
	else
		ieee80211_scan_stop(local, &conf);
}


void ieee80211_init_scan(struct ieee80211_local *local)
{
	struct ieee80211_hdr hdr;
	u16 fc;
	int len = 10;
	struct rate_control_extra extra;

	/* Only initialize passive scanning if the hardware supports it */
	if (!local->ops->passive_scan) {
		local->scan.skb = NULL;
		memset(&local->scan.tx_control, 0,
		       sizeof(local->scan.tx_control));
		printk(KERN_DEBUG "%s: Does not support passive scan, "
		       "disabled\n", local->mdev->name);
		return;
	}

	local->scan.interval = 0;
	local->scan.our_mode_only = 1;
	local->scan.time = 10000;
	local->scan.timer.function = ieee80211_scan_handler;
	local->scan.timer.data = (unsigned long) local;
	local->scan.timer.expires = jiffies + local->scan.interval * HZ;
	add_timer(&local->scan.timer);

	/* Create a CTS from for broadcasting before
	 * the low level changes channels */
	local->scan.skb = alloc_skb(len, GFP_KERNEL);
	if (!local->scan.skb) {
		printk(KERN_WARNING "%s: Failed to allocate CTS packet for "
		       "passive scan\n", local->mdev->name);
		return;
	}

	fc = IEEE80211_FTYPE_CTL | IEEE80211_STYPE_CTS;
	hdr.frame_control = cpu_to_le16(fc);
	hdr.duration_id =
		cpu_to_le16(2 * local->hw.channel_change_time +
			    local->scan.time);
	memcpy(hdr.addr1, local->mdev->dev_addr, ETH_ALEN); /* DA */
	hdr.seq_ctrl = 0;

	memcpy(skb_put(local->scan.skb, len), &hdr, len);

	memset(&local->scan.tx_control, 0, sizeof(local->scan.tx_control));
	local->scan.tx_control.key_idx = HW_KEY_IDX_INVALID;
	local->scan.tx_control.flags |= IEEE80211_TXCTL_DO_NOT_ENCRYPT;
	memset(&extra, 0, sizeof(extra));
	extra.endidx = local->num_curr_rates;
	local->scan.tx_control.tx_rate =
		rate_control_get_rate(local, local->mdev,
				      local->scan.skb, &extra)->val;
	local->scan.tx_control.flags |= IEEE80211_TXCTL_NO_ACK;
}


void ieee80211_stop_scan(struct ieee80211_local *local)
{
	if (local->ops->passive_scan) {
		del_timer_sync(&local->scan.timer);
		dev_kfree_skb(local->scan.skb);
		local->scan.skb = NULL;
	}
}
