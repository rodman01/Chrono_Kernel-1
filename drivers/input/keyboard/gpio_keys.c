/*
 * Driver for keys on GPIO lines capable of generating interrupts.
 *
 * Copyright 2005 Phil Blundell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/pm_runtime.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/ab8500-ponkey.h>
#include <linux/earlysuspend.h>
#include <mach/board-sec-u8500.h>

extern struct class *sec_class;

static bool g_bVolUp;
static bool g_bPower;
static bool g_bHome;

static bool emulator_volup = false;
static bool emulator_voldown = false;

struct gpio_button_data {
	struct gpio_keys_button *button;
	struct input_dev *input;
	struct timer_list timer;
	struct work_struct work;
	int timer_debounce;	/* in msecs */
	bool disabled;
	bool key_state;
};

struct gpio_keys_drvdata {
	struct input_dev *input;
	struct device *sec_key;
	struct mutex disable_lock;
	unsigned int n_buttons;
	bool enabled;
	bool enable_after_suspend;
	int (*enable)(struct device *dev);
	void (*disable)(struct device *dev);
	struct gpio_button_data data[0];
};

/*
 * SYSFS interface for enabling/disabling keys and switches:
 *
 * There are 4 attributes under /sys/devices/platform/gpio-keys/
 *	keys [ro]              - bitmap of keys (EV_KEY) which can be
 *	                         disabled
 *	switches [ro]          - bitmap of switches (EV_SW) which can be
 *	                         disabled
 *	disabled_keys [rw]     - bitmap of keys currently disabled
 *	disabled_switches [rw] - bitmap of switches currently disabled
 *
 * Userland can change these values and hence disable event generation
 * for each key (or switch). Disabling a key means its interrupt line
 * is disabled.
 *
 * For example, if we have following switches set up as gpio-keys:
 *	SW_DOCK = 5
 *	SW_CAMERA_LENS_COVER = 9
 *	SW_KEYPAD_SLIDE = 10
 *	SW_FRONT_PROXIMITY = 11
 * This is read from switches:
 *	11-9,5
 * Next we want to disable proximity (11) and dock (5), we write:
 *	11,5
 * to file disabled_switches. Now proximity and dock IRQs are disabled.
 * This can be verified by reading the file disabled_switches:
 *	11,5
 * If we now want to enable proximity (11) switch we write:
 *	5
 * to disabled_switches.
 *
 * We can disable only those keys which don't allow sharing the irq.
 */

/**
 * get_n_events_by_type() - returns maximum number of events per @type
 * @type: type of button (%EV_KEY, %EV_SW)
 *
 * Return value of this function can be used to allocate bitmap
 * large enough to hold all bits for given type.
 */
static inline int get_n_events_by_type(int type)
{
	BUG_ON(type != EV_SW && type != EV_KEY);

	return (type == EV_KEY) ? KEY_CNT : SW_CNT;
}

/**
 * gpio_keys_disable_button() - disables given GPIO button
 * @bdata: button data for button to be disabled
 *
 * Disables button pointed by @bdata. This is done by masking
 * IRQ line. After this function is called, button won't generate
 * input events anymore. Note that one can only disable buttons
 * that don't share IRQs.
 *
 * Make sure that @bdata->disable_lock is locked when entering
 * this function to avoid races when concurrent threads are
 * disabling buttons at the same time.
 */
static void gpio_keys_disable_button(struct gpio_button_data *bdata)
{
	if (!bdata->disabled) {
		/*
		 * Disable IRQ and possible debouncing timer.
		 */
		disable_irq(gpio_to_irq(bdata->button->gpio));
		if (bdata->timer_debounce)
			del_timer_sync(&bdata->timer);

		bdata->disabled = true;
	}
}

/**
 * gpio_keys_enable_button() - enables given GPIO button
 * @bdata: button data for button to be disabled
 *
 * Enables given button pointed by @bdata.
 *
 * Make sure that @bdata->disable_lock is locked when entering
 * this function to avoid races with concurrent threads trying
 * to enable the same button at the same time.
 */
static void gpio_keys_enable_button(struct gpio_button_data *bdata)
{
	if (bdata->disabled) {
		enable_irq(gpio_to_irq(bdata->button->gpio));
		bdata->disabled = false;
	}
}

/**
 * gpio_keys_attr_show_helper() - fill in stringified bitmap of buttons
 * @ddata: pointer to drvdata
 * @buf: buffer where stringified bitmap is written
 * @type: button type (%EV_KEY, %EV_SW)
 * @only_disabled: does caller want only those buttons that are
 *                 currently disabled or all buttons that can be
 *                 disabled
 *
 * This function writes buttons that can be disabled to @buf. If
 * @only_disabled is true, then @buf contains only those buttons
 * that are currently disabled. Returns 0 on success or negative
 * errno on failure.
 */
static ssize_t gpio_keys_attr_show_helper(struct gpio_keys_drvdata *ddata,
					  char *buf, unsigned int type,
					  bool only_disabled)
{
	int n_events = get_n_events_by_type(type);
	unsigned long *bits;
	ssize_t ret;
	int i;

	bits = kcalloc(BITS_TO_LONGS(n_events), sizeof(*bits), GFP_KERNEL);
	if (!bits)
		return -ENOMEM;

	for (i = 0; i < ddata->n_buttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (only_disabled && !bdata->disabled)
			continue;

		__set_bit(bdata->button->code, bits);
	}

	ret = bitmap_scnlistprintf(buf, PAGE_SIZE - 2, bits, n_events);
	buf[ret++] = '\n';
	buf[ret] = '\0';

	kfree(bits);

	return ret;
}

/**
 * gpio_keys_attr_store_helper() - enable/disable buttons based on given bitmap
 * @ddata: pointer to drvdata
 * @buf: buffer from userspace that contains stringified bitmap
 * @type: button type (%EV_KEY, %EV_SW)
 *
 * This function parses stringified bitmap from @buf and disables/enables
 * GPIO buttons accordinly. Returns 0 on success and negative error
 * on failure.
 */
static ssize_t gpio_keys_attr_store_helper(struct gpio_keys_drvdata *ddata,
					   const char *buf, unsigned int type)
{
	int n_events = get_n_events_by_type(type);
	unsigned long *bits;
	ssize_t error;
	int i;

	bits = kcalloc(BITS_TO_LONGS(n_events), sizeof(*bits), GFP_KERNEL);
	if (!bits)
		return -ENOMEM;

	error = bitmap_parselist(buf, bits, n_events);
	if (error)
		goto out;

	/* First validate */
	for (i = 0; i < ddata->n_buttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (test_bit(bdata->button->code, bits) &&
		    !bdata->button->can_disable) {
			error = -EINVAL;
			goto out;
		}
	}

	mutex_lock(&ddata->disable_lock);

	for (i = 0; i < ddata->n_buttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (test_bit(bdata->button->code, bits))
			gpio_keys_disable_button(bdata);
		else
			gpio_keys_enable_button(bdata);
	}

	mutex_unlock(&ddata->disable_lock);

out:
	kfree(bits);
	return error;
}

/* the volume keys can be the wakeup keys in special case */
static ssize_t wakeup_enable(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);

	int n_events = get_n_events_by_type(EV_KEY);
	unsigned long *bits;
	ssize_t error;
	int i;

	bits = kcalloc(BITS_TO_LONGS(n_events), sizeof(*bits), GFP_KERNEL);
	if (!bits)
		return -ENOMEM;
	error = bitmap_parselist(buf, bits, n_events);
	if (error)
		goto out;

	for (i = 0; i < ddata->n_buttons; i++) {
		struct gpio_button_data *button = &ddata->data[i];
		if (test_bit(button->button->code, bits))
			button->button->wakeup = 1;
		else
			button->button->wakeup = 0;
	}

out:
	kfree(bits);
	return count;
}

#define ATTR_SHOW_FN(name, type, only_disabled)				\
static ssize_t gpio_keys_show_##name(struct device *dev,		\
				     struct device_attribute *attr,	\
				     char *buf)				\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);	\
									\
	return gpio_keys_attr_show_helper(ddata, buf,			\
					  type, only_disabled);		\
}

ATTR_SHOW_FN(keys, EV_KEY, false);
ATTR_SHOW_FN(switches, EV_SW, false);
ATTR_SHOW_FN(disabled_keys, EV_KEY, true);
ATTR_SHOW_FN(disabled_switches, EV_SW, true);

/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/gpio-keys/keys [ro]
 * /sys/devices/platform/gpio-keys/switches [ro]
 */
static DEVICE_ATTR(keys, S_IRUGO, gpio_keys_show_keys, NULL);
static DEVICE_ATTR(switches, S_IRUGO, gpio_keys_show_switches, NULL);

#define ATTR_STORE_FN(name, type)					\
static ssize_t gpio_keys_store_##name(struct device *dev,		\
				      struct device_attribute *attr,	\
				      const char *buf,			\
				      size_t count)			\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);	\
	ssize_t error;							\
									\
	error = gpio_keys_attr_store_helper(ddata, buf, type);		\
	if (error)							\
		return error;						\
									\
	return count;							\
}

ATTR_STORE_FN(disabled_keys, EV_KEY);
ATTR_STORE_FN(disabled_switches, EV_SW);

/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/gpio-keys/disabled_keys [rw]
 * /sys/devices/platform/gpio-keys/disables_switches [rw]
 */
static DEVICE_ATTR(disabled_keys, S_IWUSR | S_IRUGO,
		   gpio_keys_show_disabled_keys,
		   gpio_keys_store_disabled_keys);
static DEVICE_ATTR(disabled_switches, S_IWUSR | S_IRUGO,
		   gpio_keys_show_disabled_switches,
		   gpio_keys_store_disabled_switches);
static DEVICE_ATTR(wakeup_keys, 0664, NULL, wakeup_enable);

static ssize_t keys_pressed_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{

	struct platform_device *pdev = to_platform_device(dev);
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);
	struct gpio_button_data *bdata = NULL;
	int state;
	int i;

	for (i = 0; i < ddata->n_buttons; i++) {
		bdata = &ddata->data[i];
		state = (gpio_get_value_cansleep(bdata->button->gpio) ? 1 : 0) ^ bdata->button->active_low;

		if (state)
			sprintf(buf, "%s%d\n", buf, bdata->button->code);
	}

	return strlen(buf);
}

static DEVICE_ATTR(keys_pressed, 0664, keys_pressed_show, NULL);

static struct attribute *gpio_keys_attrs[] = {
	&dev_attr_keys.attr,
	&dev_attr_keys_pressed.attr,
	&dev_attr_switches.attr,
	&dev_attr_disabled_keys.attr,
	&dev_attr_disabled_switches.attr,
	&dev_attr_wakeup_keys.attr,
	NULL,
};

static struct attribute_group gpio_keys_attr_group = {
	.attrs = gpio_keys_attrs,
};

static ssize_t sec_key_pressed_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
	int i;
	int keystate = 0;

	for (i = 0; i < ddata->n_buttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];
		keystate |= bdata->key_state;
	}

	if (keystate)
		sprintf(buf, "PRESS");
	else
		sprintf(buf, "RELEASE");

	return strlen(buf);
}

static DEVICE_ATTR(sec_key_pressed, 0664, sec_key_pressed_show, NULL);

static struct attribute *sec_key_attrs[] = {
	&dev_attr_sec_key_pressed.attr,
	NULL,
};

static struct attribute_group sec_key_attr_group = {
	.attrs = sec_key_attrs,
};

void gpio_keys_setstate(int keycode, bool bState)
{
	switch (keycode) {
	case KEY_VOLUMEUP:
		g_bVolUp = bState;
		break;
	case KEY_POWER:
		g_bPower = bState;
		break;
	case KEY_HOME:
		g_bHome = bState;
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL(gpio_keys_setstate);

#if defined(CONFIG_MACH_GAVINI)
extern void ProjectorPowerOnSequence();
extern void ProjectorPowerOffSequence();
extern void projector_motor_cw(void);
extern void projector_motor_ccw(void);
#endif

static unsigned int volkey_press_skip_track = false;
static unsigned int homekey_press_play = false;

bool is_homekey_press_play(void){
	return homekey_press_play;
}

bool is_volkey_press_skip_track(void){
	return volkey_press_skip_track;
}

// determines whether skip track thread is already run
static bool volkey_skip_track_is_ongoing = false;

static bool homekey_press_play_is_ongoing = false;

static bool volkey_do_volume_key_press_is_ongoing = false;

static bool homekey_do_press_play_is_ongoing = false;

// determines whether track should be skipped now
static bool volkey_skip_track_now = false;

static bool homekey_press_play_now = false;

// remap vol.up -> KEY_NEXTSONG / vol.down -> KEY_PREVIOUSSONG
static bool volkey_remap_keys = false;

static bool homekey_is_remapped = false;

// if true, KEY_NEXTSONG will be emulated, otherwise - KEY_PREVIOUSSONG
static bool volkey_emulate_key_nextsong = false;

// below this threshold don't emulate KEY_NEXTSONG/KEY_PREVIOUSSONG
static unsigned int volkey_long_press_delay_ms = 300;

static unsigned int homekey_long_press_delay_ms = 300;

// FIXME: key press emulation requires this additional delay 
static unsigned int volkey_do_volume_key_press_delay_ms = 101;

static unsigned int homekey_do_press_play_delay_ms = 101;

static unsigned int volkey_skip_tracks_in_suspend_only = true;

static unsigned int homekey_press_play_in_suspend_only = true;

static unsigned int volkey_debug_level = 1;

module_param_named(volkey_press_skip_track, volkey_press_skip_track, uint, 0644);
module_param_named(volkey_long_press_delay_ms, volkey_long_press_delay_ms, uint, 0644);
module_param_named(volkey_do_volume_key_press_delay_ms, volkey_do_volume_key_press_delay_ms, uint, 0644);
module_param_named(volkey_debug_level, volkey_debug_level, uint, 0644);
module_param_named(volkey_skip_tracks_in_suspend_only, volkey_skip_tracks_in_suspend_only, uint, 0644);
module_param_named(homekey_press_play, homekey_press_play, uint, 0644);
module_param_named(homekey_long_press_delay_ms, homekey_long_press_delay_ms, uint, 0644);
module_param_named(homekey_do_press_play_delay_ms, homekey_do_press_play_delay_ms, uint, 0644);
module_param_named(homekey_press_play_in_suspend_only, homekey_press_play_in_suspend_only, uint, 0644);

void volkey_reset_variables(void) {
        volkey_skip_track_is_ongoing = false;
        volkey_do_volume_key_press_is_ongoing = false;
        volkey_skip_track_now = false;
        volkey_remap_keys = false;
        volkey_emulate_key_nextsong = false;
}

void homekey_reset_variables(void) {
	homekey_press_play_is_ongoing = false;
	homekey_do_press_play_is_ongoing = false;
	homekey_press_play_now = false;
	homekey_is_remapped = false;
}

static void volkey_skip_track_fn(struct work_struct *volkey_skip_track_work)
{
	volkey_skip_track_now = true;
	volkey_skip_track_is_ongoing = false;
}
static DECLARE_DELAYED_WORK(volkey_skip_track_work, volkey_skip_track_fn);

static void homekey_press_play_fn(struct work_struct *homekey_press_play_work)
{
	homekey_press_play_now = true;
        homekey_press_play_is_ongoing = false;
}
static DECLARE_DELAYED_WORK(homekey_press_play_work, homekey_press_play_fn);

static unsigned long emulated_keys[] = {KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_VOLUMEUP, KEY_VOLUMEDOWN, HOME_KEY_JANICE_R0_0, HOME_KEY_CODINA_R0_5, KEY_PLAYPAUSE};

void unmap_keys(void) {
	abb_ponkey_unmap_all_keys(&emulated_keys, ARRAY_SIZE(emulated_keys));
}

static void volkey_do_volume_key_press_fn(struct work_struct *volkey_skip_track_work)
{
	int key, mask;

	if (volkey_remap_keys)
		key = volkey_emulate_key_nextsong ? KEY_NEXTSONG : KEY_PREVIOUSSONG;
	else
		key = volkey_emulate_key_nextsong ? KEY_VOLUMEUP : KEY_VOLUMEDOWN;

	ab8500_ponkey_emulator(key, 1);
	mdelay(volkey_do_volume_key_press_delay_ms);
	ab8500_ponkey_emulator(key, 0);
	unmap_keys();

	volkey_do_volume_key_press_is_ongoing = false;
}
static DECLARE_WORK(volkey_do_volume_key_press_work, volkey_do_volume_key_press_fn);

static void homekey_do_press_play_fn(struct work_struct *homekey_do_press_play_work)
{
        int key = homekey_is_remapped ? KEY_PLAYPAUSE : HOME_KEY_CODINA_R0_5;

        ab8500_ponkey_emulator(key, 1);
        mdelay(homekey_do_press_play_delay_ms);
        ab8500_ponkey_emulator(key, 0);
        unmap_keys();

        homekey_do_press_play_is_ongoing = false;
}
static DECLARE_WORK(homekey_do_press_play_work, homekey_do_press_play_fn);

static bool is_early_suspend = false;
unsigned int is_suspend = 0;
module_param_named(is_suspend, is_suspend, uint, 0444);

static struct early_suspend early_suspend;

static void gpio_keys_early_suspend(struct early_suspend *h)
{
	is_early_suspend = true;
	is_suspend = 0;
}

static void gpio_keys_late_resume(struct early_suspend *h)
{
	is_early_suspend = false;
	is_suspend = 0;
}


static int gpio_keys_report_event(struct gpio_button_data *bdata)
{
	struct gpio_keys_button *button = bdata->button;
	struct input_dev *input = bdata->input;
	unsigned int type = button->type ?: EV_KEY;
	int state = (gpio_get_value_cansleep(button->gpio) ? 1 : 0) ^ button->active_low;

	if (emulator_volup) {
		if (button->gpio == VOL_UP_JANICE_R0_0) {
			ab8500_ponkey_emulator(KEY_POWER, state);

			return 0;
		}
	} else if (emulator_voldown) {
		if (button->gpio == VOL_DOWN_JANICE_R0_0) {
			ab8500_ponkey_emulator(KEY_POWER, state);

			return 0;
		}
	} else if ((volkey_press_skip_track || homekey_press_play) && ! is_suspend && (is_early_suspend || 
			!volkey_skip_tracks_in_suspend_only || ! homekey_press_play_in_suspend_only)) {
		if (homekey_press_play || !homekey_press_play_in_suspend_only) {
			if (button->gpio == HOME_KEY_JANICE_R0_0) {
				if (homekey_press_play_is_ongoing && state == 1) {
                	                if (volkey_debug_level > 0)
                        	                pr_err("[GPIO-KEYS] homekey_press_play_work is already run\n");

            	                  	cancel_delayed_work(&homekey_press_play_work);
                	               	homekey_press_play_is_ongoing = false;
                	                homekey_press_play_now = false;
                	        }

				if (state == 1) {
        	                        if (volkey_debug_level > 0)
                	                        pr_err("[GPIO-KEYS] homekey is pressed\n");

                        	        if (!homekey_press_play_is_ongoing) {
 	                                       schedule_delayed_work(&homekey_press_play_work, homekey_long_press_delay_ms);
 	                                       homekey_press_play_now = false;
 	                                       homekey_press_play_is_ongoing = true;
 	  	                        } else if (volkey_debug_level > 1) {
                                        	pr_err("skipping homekey_press_play_work\n");
                                	}

                                	return 0;
				} else if (state == 0 && homekey_press_play_now) {
                                	// homekey is released and homekey_long_press_delay_ms has spent, press play now
                                	if (volkey_debug_level > 0)
                                        	pr_err("[GPIO-KEYS] homekey is released, skipping track\n");


					if (!homekey_do_press_play_is_ongoing) {

                                        	abb_ponkey_remap_power_key(KEY_POWER, KEY_PLAYPAUSE);
						homekey_is_remapped = true;
                                	        schedule_work(&homekey_do_press_play_work);
                                 	        homekey_do_press_play_is_ongoing = true;
                               	        	homekey_press_play_now = false;
                                	} else if (volkey_debug_level > 1) {
                                        	pr_err("skipping homekey_do_press_play_work\n");
                                	}

                                	return 0;
                        	} else if (state == 0 && !homekey_press_play_now) {
                                	if (volkey_debug_level > 0)
                                	        pr_err("[GPIO-KEYS] homekey is released, not pressing play\n");
                                	if (!homekey_do_press_play_is_ongoing) {
                                        	// home key is released before homekey_long_press_delay_ms
                                        	// has spent, emulate volume key press

                                       		abb_ponkey_remap_power_key(KEY_POWER, HOME_KEY_CODINA_R0_5);
						homekey_is_remapped = false;
						schedule_work(&homekey_do_press_play_work);
						homekey_do_press_play_is_ongoing = true;
                                	} else if (volkey_debug_level > 1) {
                                	        pr_err("skipping homekey_do_press_play_work\n");
                                	}

                                	return 0;
                        	}
			} 
		} if (volkey_press_skip_track || !volkey_skip_tracks_in_suspend_only) {
			if (button->gpio == VOL_UP_JANICE_R0_0 || button->gpio == VOL_DOWN_JANICE_R0_0) {
				// if vol.up/vol.down is pressed when volkey_skip_track_work is running, cancel it first
				if (volkey_skip_track_is_ongoing && state == 1) {
					if (volkey_debug_level > 0) 
						pr_err("[GPIO-KEYS] volkey_skip_track_work is already run\n");

					cancel_delayed_work(&volkey_skip_track_work);
					volkey_skip_track_is_ongoing = false;
					volkey_skip_track_now = false;
				}

				volkey_emulate_key_nextsong = (button->gpio == VOL_UP_JANICE_R0_0);

				// vol.up/vol.down is pressed, start volkey_skip_track_work now
				if (state == 1) {
					if (volkey_debug_level > 0)
						pr_err("[GPIO-KEYS] vol.%s is pressed\n", 
								volkey_emulate_key_nextsong ? "up" : "down");

					if (!volkey_skip_track_is_ongoing) {
						schedule_delayed_work(&volkey_skip_track_work, volkey_long_press_delay_ms);
						volkey_skip_track_now = false;
						volkey_skip_track_is_ongoing = true;
					} else if (volkey_debug_level > 1) {
						pr_err("skipping volkey_skip_track_work\n");
                                	}

					return 0;
				} else if (state == 0 && volkey_skip_track_now) {
					// vol.up/vol.down is released and volkey_long_press_delay_ms has spent, skip track now
					if (volkey_debug_level > 0)
						pr_err("[GPIO-KEYS] vol.%s is released, skipping track\n", 
								volkey_emulate_key_nextsong ? "up" : "down");

					if (!volkey_do_volume_key_press_is_ongoing) {
						// emulate KEY_NEXTSONG / KEY_PREVIOUSSONG
                        	        	volkey_remap_keys = true;

                        	        	abb_ponkey_remap_power_key(KEY_POWER, 
                        	                	volkey_emulate_key_nextsong ? KEY_NEXTSONG : KEY_PREVIOUSSONG);	
						schedule_work(&volkey_do_volume_key_press_work);
						volkey_do_volume_key_press_is_ongoing = true;
						volkey_skip_track_now = false;
					} else if (volkey_debug_level > 1) {
						pr_err("skipping volkey_do_volume_key_press_work\n");
					}

					return 0;
				} else if (state == 0 && !volkey_skip_track_now) {
					if (volkey_debug_level > 0)
						pr_err("[GPIO-KEYS] vol.%s is released, not skipping track\n", 
								volkey_emulate_key_nextsong ? "up" : "down");

					if (!volkey_do_volume_key_press_is_ongoing) {
						// volume key is released before volkey_long_press_delay_ms 
	                                        // has spent, emulate volume key press
	                                	volkey_remap_keys = false;

	 	                               abb_ponkey_remap_power_key(KEY_POWER, 
        		                                volkey_emulate_key_nextsong ? KEY_VOLUMEUP : KEY_VOLUMEDOWN);
        	                                schedule_work(&volkey_do_volume_key_press_work);
                	                        volkey_do_volume_key_press_is_ongoing = true;
					} else if (volkey_debug_level > 1) {
						pr_err("skipping volkey_do_volume_key_press_work\n");
                               		}

					return 0;
				}
			}
		}
	}


	if (type == EV_ABS) {
		if (state)
			input_event(input, type, button->code, button->value);
	} else {
		bdata->key_state = !!state;
		input_event(input, type, button->code, !!state);
	}

	input_sync(input);

	return 0;
}

static void gpio_keys_work_func(struct work_struct *work)
{
	struct gpio_button_data *bdata =
		container_of(work, struct gpio_button_data, work);

	gpio_keys_report_event(bdata);
}

static void gpio_keys_timer(unsigned long _data)
{
	struct gpio_button_data *data = (struct gpio_button_data *)_data;

	schedule_work(&data->work);
}

static irqreturn_t gpio_keys_isr(int irq, void *dev_id)
{
	struct gpio_button_data *bdata = dev_id;
	struct gpio_keys_button *button = bdata->button;

	BUG_ON(irq != gpio_to_irq(button->gpio));

	if (bdata->timer_debounce)
		mod_timer(&bdata->timer,
			jiffies + msecs_to_jiffies(bdata->timer_debounce));
	else
		schedule_work(&bdata->work);

	return IRQ_HANDLED;
}

static int __devinit gpio_keys_setup_key(struct platform_device *pdev,
					 struct gpio_button_data *bdata,
					 struct gpio_keys_button *button)
{
	const char *desc = button->desc ? button->desc : "gpio_keys";
	struct device *dev = &pdev->dev;
	unsigned long irqflags;
	int irq, error;

	setup_timer(&bdata->timer, gpio_keys_timer, (unsigned long)bdata);
	INIT_WORK(&bdata->work, gpio_keys_work_func);

	error = gpio_request(button->gpio, desc);
	if (error < 0) {
		dev_err(dev, "failed to request GPIO %d, error %d\n",
			button->gpio, error);
		goto fail2;
	}

	error = gpio_direction_input(button->gpio);
	if (error < 0) {
		dev_err(dev, "failed to configure"
			" direction for GPIO %d, error %d\n",
			button->gpio, error);
		goto fail3;
	}

	if (button->debounce_interval) {
		error = gpio_set_debounce(button->gpio,
					  button->debounce_interval * 1000);
		/* use timer if gpiolib doesn't provide debounce */
		if (error < 0)
			bdata->timer_debounce = button->debounce_interval;
	}

	irq = gpio_to_irq(button->gpio);
	if (irq < 0) {
		error = irq;
		dev_err(dev, "Unable to get irq number for GPIO %d, error %d\n",
			button->gpio, error);
		goto fail3;
	}

	irqflags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	/*
	 * If platform has specified that the button can be disabled,
	 * we don't want it to share the interrupt line.
	 */
	if (!button->can_disable)
		irqflags |= IRQF_SHARED;

	error = request_any_context_irq(irq, gpio_keys_isr, irqflags, desc, bdata);
	if (error < 0) {
		dev_err(dev, "Unable to claim irq %d; error %d\n",
			irq, error);
		goto fail3;
	}

	return 0;

fail3:
	gpio_free(button->gpio);
fail2:
	return error;
}

static int gpio_keys_open(struct input_dev *input)
{
	struct gpio_keys_drvdata *ddata = input_get_drvdata(input);

	pm_runtime_get_sync(input->dev.parent);
	ddata->enabled = true;
	return ddata->enable ? ddata->enable(input->dev.parent) : 0;
}

static void gpio_keys_close(struct input_dev *input)
{
	struct gpio_keys_drvdata *ddata = input_get_drvdata(input);

	if (ddata->disable)
		ddata->disable(input->dev.parent);
	ddata->enabled = false;
	pm_runtime_put(input->dev.parent);
}

static ssize_t gpio_keys_ponkey_emulator_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	sprintf(buf,   "Vol.UP: %s\n", emulator_volup ? "1" : "0");
	sprintf(buf, "%sVol.DOWN: %s\n", buf, emulator_voldown ? "1" : "0");
	return strlen(buf);
}

static ssize_t gpio_keys_ponkey_emulator_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret, user;

	if (!strncmp(&buf[0], "volup=", 6)) {
		ret = sscanf(&buf[6], "%d", &user);
		if (ret) {
			emulator_volup = user;
			pr_info("gpio_keys: %s Vol.UP POnKey\n", user ? "Enable" : "Disable");
		} else {
			pr_err("gpio_keys: unknown cmds\n");
		}
		
		return count;
	}

	if (!strncmp(&buf[0], "voldown=", 8)) {
		ret = sscanf(&buf[8], "%d", &user);
		if (ret) {
			emulator_voldown = user;
			pr_info("gpio_keys: %s Vol.DOWN POnKey\n", user ? "Enable" : "Disable");
		} else {
			pr_err("gpio_keys: unknown cmds\n");
		}
		
		return count;
	}

	pr_err("gpio_keys: unknown cmds\n");
	
	return count;
}

static struct kobj_attribute gpio_keys_ponkey_emulator_interface =
	 __ATTR(ponkey_emu, 0644, gpio_keys_ponkey_emulator_show, gpio_keys_ponkey_emulator_store);

struct input_dev *p_gpio_keys;
struct gpio_keys_platform_data *p_pdata;
struct gpio_keys_drvdata *p_ddata;

static bool emu_working = false;
static unsigned int emu_delay = 100;
static unsigned int emu_keycode;
module_param_named(emu_delay, emu_delay, uint, 0644);

inline int gpio_keys_emulator(unsigned long keycode, bool press)
{
	struct gpio_button_data *bdata;
	struct gpio_keys_button *button;

	int i, idx = -1;

	for (i = 0; i < p_pdata->nbuttons; i++) {
		button = &p_pdata->buttons[i];

		if (button->code == keycode) {
			idx = i;
			break;
		}
	}

	if (idx < 0)
		return -1;


        bdata = &p_ddata->data[idx];

        if (press) {
                gpio_keys_setstate(keycode, true);
                bdata->key_state = true;
                input_report_key(p_gpio_keys, keycode, true);
                pr_err("[gpio-keys] Emulate %d Key PRESS\n", keycode);
                input_sync(p_gpio_keys);
        } else if (!press) {
                gpio_keys_setstate(keycode, false);
                bdata->key_state = false;
                input_report_key(p_gpio_keys, keycode, false);
                pr_err("[gpio-keys] Emulate %d Key RELEASE\n", keycode);
                input_sync(p_gpio_keys);
        }

	return 0;
}
EXPORT_SYMBOL(gpio_keys_emulator);

static void gpio_keys_emulator_thread(struct work_struct *abb_ponkey_emulator_work)
{
        pr_err("[gpio-keys] Emulator thread called, timer = %d\n", 100);

        emu_working = true;

        if (gpio_keys_emulator(emu_keycode, 1) < 0) {
		pr_err("[gpio-keys] can't find button with keycode %d", emu_keycode);
	} else {
	        mdelay(100);

	        gpio_keys_emulator(emu_keycode, 0);
	}

        emu_working = false;
}
static DECLARE_WORK(gpio_keys_emulator_work, gpio_keys_emulator_thread);

static ssize_t gpio_keys_emulator_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf,   "emu_keycode=%d\n", emu_keycode);
}

static ssize_t gpio_keys_emulator_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret, len = strlen("emu_keycode=");
	unsigned int keycode;

	if (!strncmp(&buf[0], "emu_keycode=", len)) {
                ret = sscanf(&buf[len], "%d", &keycode);
                if (ret) {
                        emu_keycode = keycode;
                } else {
                        pr_err("%s: unknown cmds\n", __func__);
                }
		
		return count;
        }

	if (strstr(buf, "press") != NULL) {
		if (!emu_working && emu_keycode)
			schedule_work(&gpio_keys_emulator_work);
		else if (emu_working)
			pr_err("%s: gpio_keys_emulator_work already running\n");
		else
			pr_err("%s: emu_keycode is not set\n");

		return count;
	} else {
                pr_err("%s: unknown cmds\n", __func__);
        }


	return count;
}

static struct kobj_attribute gpio_keys_emulator_interface =
         __ATTR(emu, 0644, gpio_keys_emulator_show, gpio_keys_emulator_store);

static struct attribute *gpio_keys_attrs_kobjects[] = {
	&gpio_keys_ponkey_emulator_interface.attr, 
	&gpio_keys_emulator_interface.attr,
	NULL,
};

static struct attribute_group gpio_keys_interface_group = {
	.attrs = gpio_keys_attrs_kobjects,
};

static struct kobject *gpio_keys_kobject;

static int __devinit gpio_keys_probe(struct platform_device *pdev)
{
	struct gpio_keys_platform_data *pdata = pdev->dev.platform_data;
	struct gpio_keys_drvdata *ddata;
	struct device *dev = &pdev->dev;
	struct input_dev *input;
	int i, error;
	int wakeup = 0;
	int ret;	// To hide warnings

	ddata = kzalloc(sizeof(struct gpio_keys_drvdata) +
			pdata->nbuttons * sizeof(struct gpio_button_data),
			GFP_KERNEL);
	input = input_allocate_device();
	if (!ddata || !input) {
		dev_err(dev, "failed to allocate state\n");
		error = -ENOMEM;
		goto fail1;
	}

	ddata->input = input;
	ddata->n_buttons = pdata->nbuttons;
	ddata->enable = pdata->enable;
	ddata->disable = pdata->disable;
	ddata->enabled = false;
	mutex_init(&ddata->disable_lock);

	platform_set_drvdata(pdev, ddata);
	input_set_drvdata(input, ddata);

	input->name = pdata->name ? : pdev->name;
	input->phys = "gpio-keys/input0";
	input->dev.parent = &pdev->dev;
	input->open = gpio_keys_open;
	input->close = gpio_keys_close;

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	pm_runtime_enable(&pdev->dev);

	/* Enable auto repeat feature of Linux input subsystem */
	if (pdata->rep)
		__set_bit(EV_REP, input->evbit);

	for (i = 0; i < pdata->nbuttons; i++) {
		struct gpio_keys_button *button = &pdata->buttons[i];
		struct gpio_button_data *bdata = &ddata->data[i];
		unsigned int type = button->type ?: EV_KEY;

		bdata->input = input;
		bdata->button = button;

		error = gpio_keys_setup_key(pdev, bdata, button);
		if (error)
			goto fail2;

		if (button->wakeup)
			wakeup = 1;

		input_set_capability(input, type, button->code);
	}

	error = sysfs_create_group(&pdev->dev.kobj, &gpio_keys_attr_group);
	if (error) {
		dev_err(dev, "Unable to export keys/switches, error: %d\n",
			error);
		goto fail2;
	}

	ddata->sec_key = device_create(sec_class, NULL, 0, ddata, "sec_key");
	if (IS_ERR(ddata->sec_key))
		dev_err(dev, "Failed to create sec_key device\n");

	error = sysfs_create_group(&ddata->sec_key->kobj, &sec_key_attr_group);
	if (error) {
		dev_err(dev, "Unable to export sec_key device, error: %d\n",
				error);
		goto fail2;
	}

	error = input_register_device(input);
	if (error) {
		dev_err(dev, "Unable to register input device, error: %d\n",
			error);
		goto fail3;
	}

	/* get current state of buttons */
	for (i = 0; i < pdata->nbuttons; i++)
		gpio_keys_report_event(&ddata->data[i]);
	input_sync(input);

	device_init_wakeup(&pdev->dev, wakeup);

	gpio_keys_kobject = kobject_create_and_add("gpio-keys", kernel_kobj);

	if (!gpio_keys_kobject) {
		return -ENOMEM;
	}

	ret = sysfs_create_group(gpio_keys_kobject, &gpio_keys_interface_group);

	if (ret) {
		kobject_put(gpio_keys_kobject);
	}

	p_gpio_keys = input;
	p_pdata = pdata;
        p_ddata = ddata;

	early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	early_suspend.suspend = gpio_keys_early_suspend;
	early_suspend.resume = gpio_keys_late_resume;

	register_early_suspend(&early_suspend);

	return 0;

 fail3:
	sysfs_remove_group(&pdev->dev.kobj, &gpio_keys_attr_group);
	sysfs_remove_group(&ddata->sec_key->kobj, &sec_key_attr_group);
 fail2:
	while (--i >= 0) {
		free_irq(gpio_to_irq(pdata->buttons[i].gpio), &ddata->data[i]);
		if (ddata->data[i].timer_debounce)
			del_timer_sync(&ddata->data[i].timer);
		cancel_work_sync(&ddata->data[i].work);
		gpio_free(pdata->buttons[i].gpio);
	}

	platform_set_drvdata(pdev, NULL);
 fail1:
	input_free_device(input);
	kfree(ddata);

	return error;
}

static int __devexit gpio_keys_remove(struct platform_device *pdev)
{
	struct gpio_keys_platform_data *pdata = pdev->dev.platform_data;
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);
	struct input_dev *input = ddata->input;
	int i;

	pm_runtime_disable(&pdev->dev);

	sysfs_remove_group(&pdev->dev.kobj, &gpio_keys_attr_group);

	device_init_wakeup(&pdev->dev, 0);

	for (i = 0; i < pdata->nbuttons; i++) {
		int irq = gpio_to_irq(pdata->buttons[i].gpio);
		free_irq(irq, &ddata->data[i]);
		if (ddata->data[i].timer_debounce)
			del_timer_sync(&ddata->data[i].timer);
		cancel_work_sync(&ddata->data[i].work);
		gpio_free(pdata->buttons[i].gpio);
	}

	input_unregister_device(input);

	return 0;
}


#ifdef CONFIG_PM
static int gpio_keys_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);
	struct gpio_keys_platform_data *pdata = pdev->dev.platform_data;
	int i;

	if (device_may_wakeup(&pdev->dev)) {
		for (i = 0; i < pdata->nbuttons; i++) {
			struct gpio_keys_button *button = &pdata->buttons[i];
			if (button->wakeup) {
				int irq = gpio_to_irq(button->gpio);
				enable_irq_wake(irq);
			}
		}
	} else {
		ddata->enable_after_suspend = ddata->enabled;
		if (ddata->enabled && ddata->disable)
			ddata->disable(&pdev->dev);
	}

	return 0;
}

static int gpio_keys_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);
	struct gpio_keys_platform_data *pdata = pdev->dev.platform_data;
	int i;

	for (i = 0; i < pdata->nbuttons; i++) {

		struct gpio_keys_button *button = &pdata->buttons[i];
		if (button->wakeup && device_may_wakeup(&pdev->dev)) {
			int irq = gpio_to_irq(button->gpio);
			disable_irq_wake(irq);
		}

		gpio_keys_report_event(&ddata->data[i]);
	}

	if (!device_may_wakeup(&pdev->dev) && ddata->enable_after_suspend
	    && ddata->enable)
		ddata->enable(&pdev->dev);

	input_sync(ddata->input);

	return 0;
}

static const struct dev_pm_ops gpio_keys_pm_ops = {
	.suspend	= gpio_keys_suspend,
	.resume		= gpio_keys_resume,
};
#endif

static struct platform_driver gpio_keys_device_driver = {
	.probe		= gpio_keys_probe,
	.remove		= __devexit_p(gpio_keys_remove),
	.driver		= {
		.name	= "gpio-keys",
		.owner	= THIS_MODULE,
#ifdef CONFIG_PM
		.pm	= &gpio_keys_pm_ops,
#endif
	}
};

static int __init gpio_keys_init(void)
{
	return platform_driver_register(&gpio_keys_device_driver);
}

static void __exit gpio_keys_exit(void)
{
	platform_driver_unregister(&gpio_keys_device_driver);
}

module_init(gpio_keys_init);
module_exit(gpio_keys_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Phil Blundell <pb@handhelds.org>");
MODULE_DESCRIPTION("Keyboard driver for CPU GPIOs");
MODULE_ALIAS("platform:gpio-keys");
