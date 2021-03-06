/* linux/arch/arm/mach-msm/board-hero.c
 * Copyright (C) 2007-2009 HTC Corporation.
 * Author: Thomas Tsai <thomas_tsai@htc.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/leds.h>
#include <linux/switch.h>
#include <linux/synaptics_i2c_rmi.h>
#include <mach/cy8c_i2c.h>
#include <linux/akm8973.h>
#include <mach/htc_headset.h>
#include <mach/audio_jack.h>
#include <linux/sysdev.h>
#include <linux/android_pmem.h>
#include <linux/bma150.h>

#include <linux/delay.h>

#include <asm/gpio.h>
#include <mach/hardware.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/flash.h>
#include <asm/system.h>
#include <mach/system.h>
#include <mach/vreg.h>

#include <asm/io.h>
#include <asm/delay.h>
#include <asm/setup.h>

#include <linux/gpio_event.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>

#include <asm/mach/mmc.h>
#include <linux/mmc/sdio_ids.h>


#include "gpio_chip.h"
#include "board-hero.h"

#include <mach/board.h>
#include <mach/board_htc.h>
#include <mach/msm_serial_debugger.h>
#include <mach/msm_serial_hs.h>
#include <mach/htc_pwrsink.h>

//#include <mach/h2w_v1.h>
#include <mach/microp_i2c.h>

#ifdef CONFIG_WIFI_CONTROL_FUNC
#ifdef CONFIG_WIFI_MEM_PREALLOC
extern int hero_init_wifi_mem(void);
#endif
extern struct wifi_platform_data hero_wifi_control;
#endif

#include "proc_comm.h"
#include "devices.h"

void msm_init_irq(void);
void msm_init_gpio(void);
void msm_init_pmic_vibrator(void);

extern int hero_init_mmc(unsigned int);

struct hero_axis_info {
	struct gpio_event_axis_info info;
	uint16_t in_state;
	uint16_t out_state;
	uint16_t temp_state;
	uint16_t threshold;
};
static bool nav_just_on;
static int nav_on_jiffies;
#if	defined(CONFIG_MSM_AMSS_SUPPORT_256MB_EBI1)
static int smi_sz = 32;
#else
static int smi_sz = 64;
#endif
static unsigned int hwid = 0;
static unsigned int skuid = 0;
static unsigned engineerid = (0x01 << 1);	/* default is 3M sensor */
static unsigned int die_sz = 1;

uint16_t hero_axis_map(struct gpio_event_axis_info *info, uint16_t in)
{
	struct hero_axis_info *ai = container_of(info, struct hero_axis_info, info);
	uint16_t out = ai->out_state;

	if (nav_just_on) {
		if (jiffies == nav_on_jiffies || jiffies == nav_on_jiffies + 1)
			goto ignore;
		nav_just_on = 0;
	}
	if ((ai->in_state ^ in) & 1)
		out--;
	if ((ai->in_state ^ in) & 2)
		out++;
	ai->out_state = out;
ignore:
	ai->in_state = in;
	if (ai->out_state - ai->temp_state == ai->threshold) {
		ai->temp_state++;
		ai->out_state = ai->temp_state;
	} else if (ai->temp_state - ai->out_state == ai->threshold) {
		ai->temp_state--;
		ai->out_state = ai->temp_state;
	} else if (abs(ai->out_state - ai->temp_state) > ai->threshold)
		ai->temp_state = ai->out_state;

	return ai->temp_state;
}

/* a new search button to be a wake-up source */
static struct gpio_event_direct_entry hero_search_button_v1[] = {
	{ HERO_GPIO_SEARCH_ACT_N, KEY_COMPOSE }, /* CPLD Key Search*/
};

static struct gpio_event_direct_entry hero_search_button_v2[] = {
	{ HERO_GPIO_SEARCH_ACT_N, KEY_HOME }, /* CPLD Key Home */
};

static struct gpio_event_input_info hero_search_button_info = {
	.info.func = gpio_event_input_func,
	/* .flags = GPIOEDF_PRINT_KEYS | GPIOEDF_PRINT_KEY_DEBOUNCE, */
	.flags = 0,
	.poll_time.tv.nsec = 40 * NSEC_PER_MSEC,
	.type = EV_KEY,
	.keymap = hero_search_button_v2,
	.keymap_size = ARRAY_SIZE(hero_search_button_v2)
};

static struct gpio_event_info *hero_search_info[] = {
	&hero_search_button_info.info
};

static struct gpio_event_platform_data hero_search_button_data = {
	.name = "hero-nav-button",
	.info = hero_search_info,
	.info_count = ARRAY_SIZE(hero_search_info),
};

static struct platform_device hero_search_button_device = {
	.name = GPIO_EVENT_DEV_NAME,
	.id = 1,
	.dev = {
		.platform_data = &hero_search_button_data,
	},
};

static int gpio_tp_ls_en = HERO_TP_LS_EN;

static int hero_ts_power(int on)
{
        printk(KERN_INFO "hero_ts_power:%d\n", on);
        if (on) {
                gpio_set_value(HERO_GPIO_TP_EN, 1);
                msleep(250);
                /* enable touch panel level shift */
                gpio_set_value(HERO_TP_LS_EN, 1);
                msleep(2);
        } else {
                gpio_set_value(HERO_TP_LS_EN, 0);
                udelay(50);
                gpio_set_value(HERO_GPIO_TP_EN, 0);
        }
        return 0;
}

//static int hero_ts_power(int on)
//{
//	if (on) {
//		hero_gpio_write(NULL, HERO_GPIO_TP_EN, 1);
//		/* touchscreen must be powered before we enable i2c pullup */
//		msleep(2);
//		/* enable touch panel level shift */
//		gpio_direction_output(gpio_tp_ls_en, 1);
//		msleep(2);
//	} else {
//		gpio_direction_output(gpio_tp_ls_en, 0);
//		udelay(50);
//		hero_gpio_write(NULL, HERO_GPIO_TP_EN, 0);
//	}
//	return 0;
//}

static struct cy8c_i2c_platform_data hero_cypress_ts_data = {
        .version = 0x0001,
        .abs_x_min = 0,
        .abs_x_max = 319,
        .abs_y_min = 0,
        .abs_y_max = 479,
        .abs_pressure_min = 0,
        .abs_pressure_max = 255,
        .abs_width_min = 0,
        .abs_width_max = 15,
        .power = hero_ts_power,
};

static struct synaptics_i2c_rmi_platform_data hero_ts_data[] = {
        {
                .version = 0x0101,
                .power = hero_ts_power,
                .sensitivity_adjust = 7,
                .flags = SYNAPTICS_FLIP_Y | SYNAPTICS_SNAP_TO_INACTIVE_EDGE,
                .inactive_left = -50 * 0x10000 / 4334,
                .inactive_right = -50 * 0x10000 / 4334,
                .inactive_top = -40 * 0x10000 / 6696,
                .inactive_bottom = -40 * 0x10000 / 6696,
                .snap_left_on = 50 * 0x10000 / 4334,
                .snap_left_off = 60 * 0x10000 / 4334,
                .snap_right_on = 50 * 0x10000 / 4334,
                .snap_right_off = 60 * 0x10000 / 4334,
                .snap_top_on = 100 * 0x10000 / 6696,
                .snap_top_off = 110 * 0x10000 / 6696,
                .snap_bottom_on = 100 * 0x10000 / 6696,
                .snap_bottom_off = 110 * 0x10000 / 6696,
                .display_width = 320,
                .display_height = 480,
                .dup_threshold = 10,
        },
        {
                .flags = SYNAPTICS_FLIP_Y | SYNAPTICS_SNAP_TO_INACTIVE_EDGE,
                .inactive_left = ((4674 - 4334) / 2 + 200) * 0x10000 / 4334,
                .inactive_right = ((4674 - 4334) / 2 + 200) * 0x10000 / 4334,
                .inactive_top = ((6946 - 6696) / 2) * 0x10000 / 6696,
                .inactive_bottom = ((6946 - 6696) / 2) * 0x10000 / 6696,
                .display_width = 320,
                .display_height = 480,
        }
};

static int hero_microp_intr_debounce(uint8_t *pin_status);
static void hero_microp_intr_function(uint8_t *pin_status);

static struct microp_pin_config microp_pins_skuid_0[] = {
	MICROP_PIN(23, MICROP_PIN_CONFIG_PULL_UP),
	MICROP_PIN(0, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(1, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(2, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(4, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(9, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(10, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(11, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(12, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(13, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(14, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(15, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(16, MICROP_PIN_CONFIG_GPO),
	{	.name = "microp-pullup",
		.pin = 23,
		.config = MICROP_PIN_CONFIG_PULL_UP1,
		.mask = { 0x00, 0x00, 0x01 },
	},
	{
		.name   = "green",
		.pin    = 3,
		.config = MICROP_PIN_CONFIG_GPO_INV,
	},
	{
		.name   = "amber",
		.pin    = 5,
		.config = MICROP_PIN_CONFIG_GPO_INV,
	},
	{
		.name   = "lcd-backlight",
		.pin    = 6,
		.config = MICROP_PIN_CONFIG_PWM,
		.freq   = MICROP_PIN_PWM_FREQ_HZ_15600,
		.levels = { 30, 48, 66, 84, 102, 133, 163, 194, 224, 255 },
		.dutys	= {  8, 16, 34, 61,  96, 138, 167, 195, 227, 255 },
	},
	{
		.name	= "button-backlight",
		.pin	= 7,
		.config = MICROP_PIN_CONFIG_GPO,
	},
	{
		.name   = "adc",
		.pin    = 24,
		.config = MICROP_PIN_CONFIG_ADC,
		.levels = { 0, 0, 0, 6, 24, 60, 425, 497, 569, 638 },
	},
	{
		.pin	 = 17,
		.config  = MICROP_PIN_CONFIG_INTR_ALL,
		.mask 	 = { 0x00, 0x01, 0x00 },
		.intr_debounce = hero_microp_intr_debounce,
		.intr_function = hero_microp_intr_function,
		.init_intr_function = 1,
	}
};

/* XC and enable LABC */
static struct microp_pin_config microp_pins_skuid_1[] = {
	MICROP_PIN(23, MICROP_PIN_CONFIG_PULL_UP),
	MICROP_PIN(0, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(1, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(2, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(4, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(9, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(10, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(11, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(12, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(13, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(14, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(15, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(16, MICROP_PIN_CONFIG_GPO_INV),
	{	.name = "microp-pullup",
		.pin = 23,
		.config = MICROP_PIN_CONFIG_PULL_UP1,
		.mask = { 0x00, 0x00, 0x01 },
	},
	{
		.name   = "green",
		.pin    = 3,
		.config = MICROP_PIN_CONFIG_GPO_INV,
	},
	{
		.name   = "amber",
		.pin    = 5,
		.config = MICROP_PIN_CONFIG_GPO_INV,
	},
	{
		.name	= "button-backlight",
		.pin	= 7,
		.config = MICROP_PIN_CONFIG_GPO,
	},
	{
		.name   = "adc",
		.pin    = 24,
		.config = MICROP_PIN_CONFIG_ADC,
		.levels = { 0, 0, 0, 6, 24, 60, 425, 497, 569, 638 },
	},
	{
		.name	= "35mm_adc",
		.pin	= 25,
		.adc_pin = 7,
		.config = MICROP_PIN_CONFIG_ADC_READ,
	},
	{
		.name   = "microp_intrrupt",
		.pin	 = 17,
		.config  = MICROP_PIN_CONFIG_INTR_ALL,
		.mask 	 = { 0x00, 0x01, 0x00 },
		.intr_debounce = hero_microp_intr_debounce,
		.intr_function = hero_microp_intr_function,
		.init_intr_function = 1,
	}
};

/* XD, add jogball backlight function*/
static struct microp_pin_config microp_pins_skuid_2[] = {
	MICROP_PIN(23, MICROP_PIN_CONFIG_PULL_UP),
	MICROP_PIN(0, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(1, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(2, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(4, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(9, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(11, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(12, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(13, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(14, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(15, MICROP_PIN_CONFIG_GPO),
	{	.name = "microp-pullup",
		.pin = 23,
		.config = MICROP_PIN_CONFIG_PULL_UP1,
		.mask = { 0x00, 0x00, 0x03 },
	},
	{
		.name   = "green",
		.pin    = 3,
		.config = MICROP_PIN_CONFIG_GPO_INV,
	},
	{
		.name   = "amber",
		.pin    = 5,
		.config = MICROP_PIN_CONFIG_GPO_INV,
	},
	{
		.name	= "button-backlight",
		.pin	= 7,
		.config = MICROP_PIN_CONFIG_GPO,
	},
	{
		.name	= "jogball-backlight",
		.pin	= 10,
		.config = MICROP_PIN_CONFIG_PWM,
		.auto_if_on = 1,
		.i_am_jogball_function = 1,
	},
	{
		.name   = "adc",
		.pin    = 24,
		.config = MICROP_PIN_CONFIG_ADC,
		.levels = { 0, 0, 0, 6, 24, 60, 425, 497, 569, 638 },
	},
	{
		.name	= "35mm_adc",
		.pin	= 25,
		.adc_pin = 7,
		.config = MICROP_PIN_CONFIG_ADC_READ,
	},
	{
		.name   = "microp_intrrupt",
		.pin	 = 17,
		.config  = MICROP_PIN_CONFIG_INTR_ALL,
		.mask 	 = { 0x00, 0x01, 0x00 },
		.intr_debounce = hero_microp_intr_debounce,
		.intr_function = hero_microp_intr_function,
		.init_intr_function = 1,
	}
};

/* XE, 11pin mic function*/
static struct microp_pin_config microp_pins_skuid_3[] = {
	MICROP_PIN(23, MICROP_PIN_CONFIG_PULL_UP),
	MICROP_PIN(0, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(1, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(2, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(4, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(9, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(11, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(12, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(13, MICROP_PIN_CONFIG_GPO),
	MICROP_PIN(14, MICROP_PIN_CONFIG_GPO_INV),
	MICROP_PIN(15, MICROP_PIN_CONFIG_GPO),
	{	.name = "microp-pullup",
		.pin = 23,
		.config = MICROP_PIN_CONFIG_PULL_UP1,
		.mask = { 0x00, 0x00, 0x03 },
	},
	{
		.name   = "green",
		.pin    = 3,
		.config = MICROP_PIN_CONFIG_GPO_INV,
	},
	{
		.name   = "amber",
		.pin    = 5,
		.config = MICROP_PIN_CONFIG_GPO_INV,
	},
	{
		.name	= "button-backlight",
		.pin	= 7,
		.config = MICROP_PIN_CONFIG_GPO,
	},
	{
		.name	= "jogball-backlight",
		.pin	= 10,
		.config = MICROP_PIN_CONFIG_PWM,
		.auto_if_on = 1,
		.i_am_jogball_function = 1,
	},
	{
		.name = "microp_11pin_mic",
		.pin = 16,
		.config = MICROP_PIN_CONFIG_MIC,
		.init_value = 1,
	},
	{
		.name   = "adc",
		.pin    = 24,
		.config = MICROP_PIN_CONFIG_ADC,
		.levels = { 0, 0, 0, 6, 24, 60, 425, 497, 569, 638 },
	},
	{
		.name	= "35mm_adc",
		.pin	= 25,
		.adc_pin = 7,
		.config = MICROP_PIN_CONFIG_ADC_READ,
	},
	{
		.name   = "microp_intrrupt",
		.pin	 = 17,
		.config  = MICROP_PIN_CONFIG_INTR_ALL,
		.mask 	 = { 0x00, 0x01, 0x00 },
		.intr_debounce = hero_microp_intr_debounce,
		.intr_function = hero_microp_intr_function,
		.init_intr_function = 1,
	}
};

static struct microp_i2c_platform_data microp_data = {
	.num_pins   = ARRAY_SIZE(microp_pins_skuid_0),
	.pin_config = microp_pins_skuid_0,
	.gpio_reset = HERO_GPIO_UP_RESET_N,
	.cabc_backlight_enable = 0,
	.microp_enable_early_suspend = 1,
	.microp_enable_reset_button = 1,
};

#define DEBOUNCE_LENGTH 4
static int hero_microp_intr_debounce(uint8_t *pin_status)
{
/*Per HW RD's request, wait 300 mill-seconds.*/
#if 1
	mdelay(100);
	return 0;
#else
	static int count;
	static uint8_t data[DEBOUNCE_LENGTH];

	if (pin_status[0] == 0 && pin_status[1] == 0 && pin_status[2] == 0) {
		mdelay(5);
		return 1;
	}
	/*
	printk(KERN_INFO "hero_microp_intr_debounce : %02X %02X %02X\n",
		pin_status[0], pin_status[1], pin_status[2]);
	*/
	if (count < DEBOUNCE_LENGTH - 1) {
		data[count] = pin_status[1] & 0x01;
		count++;
	} else {
		data[DEBOUNCE_LENGTH - 1] = pin_status[1] & 0x01;
		for (count = 0; count < DEBOUNCE_LENGTH - 1; count++)
			if (data[count] != data[count + 1])
				break;
		if (count == DEBOUNCE_LENGTH - 1) {
			count = 0;
			return 0;
		}
		for (count = 0; count < DEBOUNCE_LENGTH - 1; count++)
			data[count] = data[count + 1];
	}

	mdelay(20);

	return 1;
#endif
}

void hero_headset_mic_select(uint8_t select)
{
	microp_i2c_set_pin_mode(4, select, microp_data.dev_id);
}

static void hero_microp_intr_function(uint8_t *pin_status)
{
	static int last_insert = 0;
	int insert;
	/*
	printk(KERN_INFO "hero_microp_intr_function : %02X %02X %02X\n",
		pin_status[0], pin_status[1], pin_status[2]);
	*/
	if (pin_status[1] & 0x01) {
		insert = 0;
	} else {
		insert = 1;
	}

	if (last_insert != insert) {
		printk(KERN_INFO "hero_microp_intr_function : %s\n", insert ? "inserted" : "not inserted");
		microp_i2c_set_pin_mode(4, insert, microp_data.dev_id);
#ifdef CONFIG_HTC_HEADSET_V1
		cnf_driver_event("H2W_extend_headset", &insert);
#endif
		last_insert = insert;
	}
}

static struct akm8973_platform_data compass_platform_data = {
        .layouts = HERO_LAYOUTS,
        .project_name = HERO_PROJECT_NAME,
        .reset = HERO_GPIO_COMPASS_RST_N,
        .intr = HERO_GPIO_COMPASS_INT_N,
};

static struct bma150_platform_data gsensor_platform_data = {
        .intr = HERO_GPIO_GSENSOR_INT_N,
};

static struct i2c_board_info i2c_bma150 = {
        I2C_BOARD_INFO(BMA150_I2C_NAME, 0x38),
        .platform_data = &gsensor_platform_data,
        .irq = HERO_GPIO_TO_INT(HERO_GPIO_GSENSOR_INT_N),
	};

static struct i2c_board_info i2c_devices[] = {
        {
                I2C_BOARD_INFO(SYNAPTICS_I2C_RMI_NAME, 0x20),
                .platform_data = &hero_ts_data,
                .irq = HERO_GPIO_TO_INT(HERO_GPIO_TP_ATT_N)
        },
        {
                I2C_BOARD_INFO(CY8C_I2C_NAME, 0x13),
                .platform_data = &hero_cypress_ts_data,
                .irq = HERO_GPIO_TO_INT(HERO_GPIO_TP_ATT_N)
        },
	{
		I2C_BOARD_INFO(MICROP_I2C_NAME, 0xCC >> 1),
		.platform_data = &microp_data,
		.irq = HERO_GPIO_TO_INT(HERO_GPIO_UP_INT_N)
	},
        {
                I2C_BOARD_INFO(AKM8973_I2C_NAME, 0x1C),
                .platform_data = &compass_platform_data,
                .irq = HERO_GPIO_TO_INT(HERO_GPIO_COMPASS_INT_N),
        },

#ifdef CONFIG_MSM_CAMERA
#ifdef CONFIG_MT9P012
	{
		I2C_BOARD_INFO("mt9p012", 0x6C >> 1),
	},
#endif
#ifdef CONFIG_MT9T013
	{
		I2C_BOARD_INFO("mt9t013", 0x6C),
	},
#endif
#endif/*CONIFIG_MSM_CAMERA*/
#ifdef CONFIG_SENSORS_MT9T013
	{
		I2C_BOARD_INFO("mt9t013", 0x6C >> 1),
	},
#endif
};

#ifdef CONFIG_LEDS_CPLD
static struct resource cpldled_resources[] = {
	{
		.start	= HERO_CPLD_LED_BASE,
		.end	= HERO_CPLD_LED_BASE + HERO_CPLD_LED_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	}
};

static struct platform_device android_CPLD_leds = {
	.name		= "leds-cpld",
	.id			= -1,
	.num_resources	= ARRAY_SIZE(cpldled_resources),
	.resource	= cpldled_resources,
};
#endif

static struct gpio_led android_led_list[] = {
	{
		.name = "button-backlight",
		.gpio = HERO_GPIO_APKEY_LED_EN,
	},
};

static struct gpio_led_platform_data android_leds_data = {
	.num_leds	= ARRAY_SIZE(android_led_list),
	.leds		= android_led_list,
};

static struct platform_device android_leds = {
	.name		= "leds-gpio",
	.id		= -1,
	.dev		= {
		.platform_data = &android_leds_data,
	},
};

#ifdef CONFIG_HTC_HEADSET
/* RTS/CTS to GPO/GPI. */
static uint32_t uart1_on_gpio_table[] = {
	/* allenou, uart hs test, 2008/11/18 */
	#ifdef CONFIG_SERIAL_MSM_HS
	/* RTS */
	PCOM_GPIO_CFG(HERO_GPIO_UART1_RTS, 2,
		      GPIO_OUTPUT, GPIO_PULL_UP, GPIO_8MA),
	/* CTS */
	PCOM_GPIO_CFG(HERO_GPIO_UART1_CTS, 2,
		      GPIO_INPUT, GPIO_PULL_UP, GPIO_8MA),
	#else
	/* RTS */
	PCOM_GPIO_CFG(HERO_GPIO_UART1_RTS, 1,
		      GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA),
	/* CTS */
	PCOM_GPIO_CFG(HERO_GPIO_UART1_CTS, 1,
		      GPIO_INPUT, GPIO_PULL_DOWN, GPIO_4MA),
	#endif
};

/* RTS,CTS to BT. */
static uint32_t uart1_off_gpio_table[] = {
	/* RTS */
	PCOM_GPIO_CFG(HERO_GPIO_UART1_RTS, 0,
		      GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA),
	/* CTS */
	PCOM_GPIO_CFG(HERO_GPIO_UART1_CTS, 0,
		      GPIO_INPUT, GPIO_NO_PULL, GPIO_2MA),
};

/* Hero: Switch between UART3 and GPIO */
static uint32_t uart3_on_gpio_table[] = {
	/* RX */
	PCOM_GPIO_CFG(HERO_GPIO_UART3_RX, 1,
		      GPIO_INPUT, GPIO_NO_PULL, GPIO_2MA),
	/* TX */
	PCOM_GPIO_CFG(HERO_GPIO_UART3_TX, 1,
		      GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA),
};

/* set TX,RX to GPI */
static uint32_t uart3_off_gpi_table[] = {
	/* RX, H2W DATA */
	PCOM_GPIO_CFG(HERO_GPIO_H2W_DATA, 0,
		      GPIO_INPUT, GPIO_NO_PULL, GPIO_2MA),
	/* TX, H2W CLK */
	PCOM_GPIO_CFG(HERO_GPIO_H2W_CLK, 0,
		      GPIO_INPUT, GPIO_KEEPER, GPIO_2MA),
};

static int hero_h2w_path = H2W_GPIO;

static void h2w_config_cpld(int route)
{
	switch (route) {
	case H2W_UART1:
		/* Make sure uart1 funtion pin opened. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+1, 0);
		gpio_set_value(HERO_GPIO_H2W_SEL0, 1);
		gpio_set_value(HERO_GPIO_H2W_SEL1, 0);
		hero_h2w_path = H2W_UART1;
		printk(KERN_INFO "H2W route = H2W-UART1, BT-X, UART3-X \n");
		break;
	case H2W_BT:
		gpio_set_value(HERO_GPIO_H2W_SEL0, 1);
		gpio_set_value(HERO_GPIO_H2W_SEL1, 1);
		/* UART1 RTS/CTS to GPO/GPI. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_off_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_off_gpio_table+1, 0);
		hero_h2w_path = H2W_BT;
		printk(KERN_INFO "H2W route = H2W-BT, UART1-X, UART3-X \n");
		break;
	case H2W_UART3:
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart3_on_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart3_on_gpio_table+1, 0);
		gpio_set_value(HERO_GPIO_H2W_SEL0, 0);
		gpio_set_value(HERO_GPIO_H2W_SEL1, 1);
		/* Make sure uart1 funtion pin opened. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+1, 0);
		hero_h2w_path = H2W_UART3;
		printk(KERN_INFO "H2W route = H2W-UART3, BT-UART1 \n");
		break;
	case H2W_GPIO: /*H2W_UART3 TX,RX are changed to H2W_GPIO */
	default:
		gpio_set_value(HERO_GPIO_H2W_SEL0, 0);
		gpio_set_value(HERO_GPIO_H2W_SEL1, 0);
		/* Set the CPLD connected H2W GPIO's to input */
		gpio_set_value(HERO_GPIO_H2W_CLK_DIR, 0);
		gpio_set_value(HERO_GPIO_H2W_DAT_DIR, 0);
		/* TX,RX GPI first. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart3_off_gpi_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart3_off_gpi_table+1, 0);
		/* Make sure uart1 funtion pin opened. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+1, 0);
		hero_h2w_path = H2W_GPIO;
		printk(KERN_INFO "H2W route = H2W-GPIO, BT-UART1 \n");
		break;
	}
}

static void h2w_init_cpld(void)
{
	h2w_config_cpld(H2W_GPIO);
}

static int h2w_dat_value;
static void set_h2w_dat(int n)
{
	h2w_dat_value = n;
	gpio_set_value(HERO_GPIO_H2W_DATA, n);
}

static int h2w_clk_value;
static void set_h2w_clk(int n)
{
	h2w_clk_value = n;
	gpio_set_value(HERO_GPIO_H2W_CLK, n);
}

static void set_h2w_dat_dir(int n)
{
	if (n == 0) /* input */
		gpio_direction_input(HERO_GPIO_H2W_DATA);
	else
		gpio_direction_output(HERO_GPIO_H2W_DATA, h2w_dat_value);

	gpio_set_value(HERO_GPIO_H2W_DAT_DIR, n);

}

static void set_h2w_clk_dir(int n)
{
	if (n == 0) /* input */
		gpio_direction_input(HERO_GPIO_H2W_CLK);
	else
		gpio_direction_output(HERO_GPIO_H2W_CLK, h2w_clk_value);

	gpio_set_value(HERO_GPIO_H2W_CLK_DIR, n);
}

static int get_h2w_dat(void)
{
	return gpio_get_value(HERO_GPIO_H2W_DATA);
}

static int get_h2w_clk(void)
{
	return gpio_get_value(HERO_GPIO_H2W_CLK);
}

static int set_h2w_path(const char *val, struct kernel_param *kp)
{
	int ret = -EINVAL;

	ret = param_set_int(val, kp);
	if (ret)
		return ret;

	switch (hero_h2w_path) {
	case H2W_GPIO:
	case H2W_UART1:
	case H2W_UART3:
	case H2W_BT:
		break;
	default:
		hero_h2w_path = -1;
		return -EINVAL;
	}

	h2w_config_cpld(hero_h2w_path);
	return ret;
}
module_param_call(h2w_path, set_h2w_path, param_get_int,
		&hero_h2w_path, S_IWUSR | S_IRUGO);


static struct h2w_platform_data hero_h2w_data = {
	.power_name		= "wlan",
	.cable_in1		= HERO_GPIO_CABLE_IN1,
	.cable_in2		= HERO_GPIO_CABLE_IN2,
	.h2w_clk		= HERO_GPIO_H2W_CLK,
	.h2w_data		= HERO_GPIO_H2W_DATA,
	.headset_mic_35mm	= HERO_GPIO_AUD_HSMIC_DET_N,
	.debug_uart 		= H2W_UART3,
	.config_cpld 		= h2w_config_cpld,
	.init_cpld 		= h2w_init_cpld,
	.set_dat		= set_h2w_dat,
	.set_clk		= set_h2w_clk,
	.set_dat_dir		= set_h2w_dat_dir,
	.set_clk_dir		= set_h2w_clk_dir,
	.get_dat		= get_h2w_dat,
	.get_clk		= get_h2w_clk,
};

static struct platform_device hero_h2w = {
	.name		= "h2w",
	.id			= -1,
	.dev		= {
		.platform_data	= &hero_h2w_data,
	},
};
#endif

#ifdef CONFIG_HTC_AUDIO_JACK
static struct audio_jack_platform_data hero_audio_jack_data = {
	.gpio = HERO_GPIO_AUDIO_JACK,
};

static struct platform_device hero_audio_jack = {
	.name		= "audio_jack",
	.id		= -1,
	.dev		= {
		.platform_data = &hero_audio_jack_data,
	},
};
#endif

static void hero_phy_reset(void)
{
	gpio_set_value(HERO_GPIO_USB_PHY_RST_N, 0);
	mdelay(10);
	gpio_set_value(HERO_GPIO_USB_PHY_RST_N, 1);
	mdelay(10);
}

static struct pwr_sink hero_pwrsink_table[] = {
	{
		.id	= PWRSINK_AUDIO,
		.ua_max	= 100000,
	},
	{
		.id	= PWRSINK_BACKLIGHT,
		.ua_max	= 125000,
	},
	{
		.id	= PWRSINK_LED_BUTTON,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_LED_KEYBOARD,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_GP_CLK,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_BLUETOOTH,
		.ua_max	= 15000,
	},
	{
		.id	= PWRSINK_CAMERA,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_SDCARD,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_VIDEO,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_WIFI,
		.ua_max = 200000,
	},
	{
		.id	= PWRSINK_SYSTEM_LOAD,
		.ua_max	= 100000,
		.percent_util = 38,
	},
};

static int hero_pwrsink_resume_early(struct platform_device *pdev)
{
	htc_pwrsink_set(PWRSINK_SYSTEM_LOAD, 7);
	return 0;
}

static void hero_pwrsink_resume_late(struct early_suspend *h)
{
	htc_pwrsink_set(PWRSINK_SYSTEM_LOAD, 38);
}

static void hero_pwrsink_suspend_early(struct early_suspend *h)
{
	htc_pwrsink_set(PWRSINK_SYSTEM_LOAD, 7);
}

static int hero_pwrsink_suspend_late(struct platform_device *pdev, pm_message_t state)
{
	htc_pwrsink_set(PWRSINK_SYSTEM_LOAD, 1);
	return 0;
}

static struct pwr_sink_platform_data hero_pwrsink_data = {
	.num_sinks	= ARRAY_SIZE(hero_pwrsink_table),
	.sinks		= hero_pwrsink_table,
	.suspend_late	= hero_pwrsink_suspend_late,
	.resume_early	= hero_pwrsink_resume_early,
	.suspend_early	= hero_pwrsink_suspend_early,
	.resume_late	= hero_pwrsink_resume_late,
};

static struct platform_device hero_pwr_sink = {
	.name = "htc_pwrsink",
	.id = -1,
	.dev	= {
		.platform_data = &hero_pwrsink_data,
	},
};

static struct platform_device hero_rfkill = {
	.name = "hero_rfkill",
	.id = -1,
};

static struct msm_pmem_setting pmem_setting_32 = {
	.pmem_start = SMI32_MSM_PMEM_MDP_BASE,
	.pmem_size = SMI32_MSM_PMEM_MDP_SIZE,
	.pmem_adsp_start = SMI32_MSM_PMEM_ADSP_BASE,
	.pmem_adsp_size = SMI32_MSM_PMEM_ADSP_SIZE,
	.pmem_gpu0_start = MSM_PMEM_GPU0_BASE,
	.pmem_gpu0_size = MSM_PMEM_GPU0_SIZE,
	.pmem_gpu1_start = SMI32_MSM_PMEM_GPU1_BASE,
	.pmem_gpu1_size = SMI32_MSM_PMEM_GPU1_SIZE,
	.pmem_camera_start = SMI32_MSM_PMEM_CAMERA_BASE,
	.pmem_camera_size = SMI32_MSM_PMEM_CAMERA_SIZE,
	.ram_console_start = SMI32_MSM_RAM_CONSOLE_BASE,
	.ram_console_size = SMI32_MSM_RAM_CONSOLE_SIZE,
};

static struct msm_pmem_setting pmem_setting_32_mono = {
	.pmem_start = 0x1e000000,	// MDP_BASE
	.pmem_size = 0x00800000,	// MDP_SIZE
	.pmem_adsp_start = 0x1e800000,	// ADSP_BASE
	.pmem_adsp_size = 0x00800000,	// ADSP_SIZE
	.pmem_gpu0_start = 0x00000000,	// GPU0_BASE
	.pmem_gpu0_size = 0x00700000,	// GPU0_SIZE
	.pmem_gpu1_start = 0x1d800000,	// GPU1_BASE
	.pmem_gpu1_size = 0x00800000,	// GPU1_SIZE
	.pmem_camera_start = 0x1f000000,	// CAMERA_BASE
	.pmem_camera_size = 0x01000000,	// CAMERA_SIZE
	.ram_console_start = 0x007a0000,	// RAM_CONSOLE_BASE
	.ram_console_size = 0x00020000,	// RAM_CONSOLE_SIZE
};

static struct msm_pmem_setting pmem_setting_32_dual = {
	.pmem_start = 0x26000000,	// MDP_BASE
	.pmem_size = 0x00800000,	// MDP_SIZE
	.pmem_adsp_start = 0x26800000,	// ADSP_BASE
	.pmem_adsp_size = 0x00800000,	// ADSP_SIZE
	.pmem_gpu0_start = 0x00000000,	// GPU0_BASE
	.pmem_gpu0_size = 0x00700000,	// GPU0_SIZE
	.pmem_gpu1_start = 0x25800000,	// GPU1_BASE
	.pmem_gpu1_size = 0x00800000,	// GPU1_SIZE
	.pmem_camera_start = 0x27000000,	// CAMERA_BASE
	.pmem_camera_size = 0x01000000,	// CAMERA_SIZE
	.ram_console_start = 0x007a0000,	// RAM_CONSOLE_BASE
	.ram_console_size = 0x00020000,	// RAM_CONSOLE_SIZE
};

static struct msm_pmem_setting pmem_setting_64 = {
	.pmem_start = SMI64_MSM_PMEM_MDP_BASE,
	.pmem_size = SMI64_MSM_PMEM_MDP_SIZE,
	.pmem_adsp_start = SMI64_MSM_PMEM_ADSP_BASE,
	.pmem_adsp_size = SMI64_MSM_PMEM_ADSP_SIZE,
	.pmem_gpu0_start = MSM_PMEM_GPU0_BASE,
	.pmem_gpu0_size = MSM_PMEM_GPU0_SIZE,
	.pmem_gpu1_start = SMI64_MSM_PMEM_GPU1_BASE,
	.pmem_gpu1_size = SMI64_MSM_PMEM_GPU1_SIZE,
	.pmem_camera_start = SMI64_MSM_PMEM_CAMERA_BASE,
	.pmem_camera_size = SMI64_MSM_PMEM_CAMERA_SIZE,
	.ram_console_start = SMI64_MSM_RAM_CONSOLE_BASE,
	.ram_console_size = SMI64_MSM_RAM_CONSOLE_SIZE,
};

#ifdef CONFIG_WIFI_CONTROL_FUNC
static struct platform_device hero_wifi = {
	.name		= "msm_wifi",
	.id		= 1,
	.num_resources	= 0,
	.resource	= NULL,
	.dev		= {
		.platform_data = &hero_wifi_control,
	},
};
#endif

#define SND(num, desc) { .name = desc, .id = num }
static struct snd_endpoint snd_endpoints_list[] = {
	SND(0, "HANDSET"),
	SND(1, "SPEAKER"),
	SND(2, "HEADSET"),
	SND(3, "BT"),
	SND(44, "BT_EC_OFF"),
	SND(10, "HEADSET_AND_SPEAKER"),
	SND(256, "CURRENT"),

	/* Bluetooth accessories. */

	SND(12, "HTC BH S100"),
	SND(13, "HTC BH M100"),
	SND(14, "Motorola H500"),
	SND(15, "Nokia HS-36W"),
	SND(16, "PLT 510v.D"),
	SND(17, "M2500 by Plantronics"),
	SND(18, "Nokia HDW-3"),
	SND(19, "HBH-608"),
	SND(20, "HBH-DS970"),
	SND(21, "i.Tech BlueBAND"),
	SND(22, "Nokia BH-800"),
	SND(23, "Motorola H700"),
	SND(24, "HTC BH M200"),
	SND(25, "Jabra JX10"),
	SND(26, "320Plantronics"),
	SND(27, "640Plantronics"),
	SND(28, "Jabra BT500"),
	SND(29, "Motorola HT820"),
	SND(30, "HBH-IV840"),
	SND(31, "6XXPlantronics"),
	SND(32, "3XXPlantronics"),
	SND(33, "HBH-PV710"),
	SND(34, "Motorola H670"),
	SND(35, "HBM-300"),
	SND(36, "Nokia BH-208"),
	SND(37, "Samsung WEP410"),
	SND(38, "Jabra BT8010"),
	SND(39, "Motorola S9"),
	SND(40, "Jabra BT620s"),
	SND(41, "Nokia BH-902"),
	SND(42, "HBH-DS220"),
	SND(43, "HBH-DS980"),
};
#undef SND

static struct msm_snd_endpoints hero_snd_endpoints = {
	.endpoints = snd_endpoints_list,
	.num = ARRAY_SIZE(snd_endpoints_list),
};

static struct platform_device hero_snd = {
	.name = "msm_snd",
	.id = -1,
	.dev	= {
		.platform_data = &hero_snd_endpoints,
	},
};

#ifdef CONFIG_MSM_CAMERA
void config_hero_camera_on_gpios(void);
void config_hero_camera_on_gpios(void);
static struct msm_camera_device_platform_data msm_camera_device_data = {
	.camera_gpio_on  = config_hero_camera_on_gpios,
	.camera_gpio_off = config_hero_camera_off_gpios,
	.ioext.mdcphy = MSM_MDC_PHYS,
	.ioext.mdcsz  = MSM_MDC_SIZE,
	.ioext.appphy = MSM_CLK_CTL_PHYS,
	.ioext.appsz  = MSM_CLK_CTL_SIZE,
};

#ifdef CONFIG_MT9T013
static struct msm_camera_sensor_info msm_camera_sensor_mt9t013_data = {
	.sensor_name    = "mt9t013",
	.sensor_reset   = 108,
	.sensor_pwd     = 85,
	.vcm_pwd        = HERO_GPIO_VCM_PWDN,
	.pdata          = &msm_camera_device_data,
};

static struct platform_device msm_camera_sensor_mt9t013 = {
	.name           = "msm_camera_mt9t013",
	.dev            = {
		.platform_data = &msm_camera_sensor_mt9t013_data,
	},
};
#endif

#ifdef CONFIG_MT9P012
static struct msm_camera_sensor_info msm_camera_sensor_mt9p012_data = {
	.sensor_name	= "mt9p012",
	.sensor_reset	= 108,
	.sensor_pwd	= 85,
	.vcm_pwd        = HERO_GPIO_VCM_PWDN,
	.pdata		= &msm_camera_device_data,
};

static struct platform_device msm_camera_sensor_mt9p012 = {
	.name           = "msm_camera_mt9p012",
	.dev            = {
		.platform_data = &msm_camera_sensor_mt9p012_data,
	},
};
#endif
#endif/*CONFIG_MSM_CAMERA*/

#ifdef CONFIG_SENSORS_MT9T013
static struct msm_camera_legacy_device_platform_data msm_camera_device_mt9t013 = {
	.sensor_reset	= 108,
	.sensor_pwd	= 85,
	.vcm_pwd	= HERO_GPIO_VCM_PWDN,
	.config_gpio_on = config_hero_camera_on_gpios,
	.config_gpio_off = config_hero_camera_off_gpios,
};

static struct platform_device hero_camera = {
	.name           = "camera",
	.dev            = {
		.platform_data = &msm_camera_device_mt9t013,
	},
};
#endif

static struct platform_device *devices[] __initdata = {
	&msm_device_smd,
	&msm_device_nand,
	&msm_device_i2c,
	&msm_device_uart1,
#if !defined(CONFIG_MSM_SERIAL_DEBUGGER) && !defined(CONFIG_TROUT_H2W)
	&msm_device_uart3,
#endif
#ifdef CONFIG_SERIAL_MSM_HS
	&msm_device_uart_dm1,
#endif
//	&hero_nav_device,
	&hero_search_button_device,
//	&hero_reset_keys_device,
	&android_leds,
#ifdef CONFIG_LEDS_CPLD
	&android_CPLD_leds,
#endif
#ifdef CONFIG_HTC_HEADSET
	&hero_h2w,
#endif
#ifdef CONFIG_MT9T013
	&msm_camera_sensor_mt9t013,
#endif
#ifdef CONFIG_MT9P012
	&msm_camera_sensor_mt9p012,
#endif
	&hero_rfkill,
#ifdef CONFIG_WIFI_CONTROL_FUNC
	&hero_wifi,
#endif

#ifdef CONFIG_HTC_PWRSINK
	&hero_pwr_sink,
#endif
	&hero_snd,
#ifdef CONFIG_SENSORS_MT9T013
	&hero_camera,
#endif
};

extern struct sys_timer msm_timer;

static void __init hero_init_irq(void)
{
	printk(KERN_DEBUG "hero_init_irq()\n");
	msm_init_irq();
}

static uint cpld_iset;
static uint cpld_charger_en;
static uint cpld_usb_h2w_sw;
static uint opt_disable_uart3;

module_param_named(iset, cpld_iset, uint, 0);
module_param_named(charger_en, cpld_charger_en, uint, 0);
module_param_named(usb_h2w_sw, cpld_usb_h2w_sw, uint, 0);
module_param_named(disable_uart3, opt_disable_uart3, uint, 0);

static void hero_reset(void)
{
	gpio_set_value(HERO_GPIO_PS_HOLD, 0);
}

static uint32_t gpio_table[] = {
	/* BLUETOOTH */
#ifdef CONFIG_SERIAL_MSM_HS
	PCOM_GPIO_CFG(43, 2, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* RTS */
	PCOM_GPIO_CFG(44, 2, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* CTS */
	PCOM_GPIO_CFG(45, 2, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* RX */
	PCOM_GPIO_CFG(46, 3, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* TX */
#else
	PCOM_GPIO_CFG(43, 1, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* RTS */
	PCOM_GPIO_CFG(44, 1, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* CTS */
	PCOM_GPIO_CFG(45, 1, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* RX */
	PCOM_GPIO_CFG(46, 1, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* TX */
#endif
};


static uint32_t camera_off_gpio_table[] = {
	/* CAMERA */
	PCOM_GPIO_CFG(2, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT2 */
	PCOM_GPIO_CFG(3, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT3 */
	PCOM_GPIO_CFG(4, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT4 */
	PCOM_GPIO_CFG(5, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT5 */
	PCOM_GPIO_CFG(6, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT6 */
	PCOM_GPIO_CFG(7, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT7 */
	PCOM_GPIO_CFG(8, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT8 */
	PCOM_GPIO_CFG(9, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT9 */
	PCOM_GPIO_CFG(10, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT10 */
	PCOM_GPIO_CFG(11, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT11 */
	PCOM_GPIO_CFG(12, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* PCLK */
	PCOM_GPIO_CFG(13, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* HSYNC_IN */
	PCOM_GPIO_CFG(14, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* VSYNC_IN */
	PCOM_GPIO_CFG(15, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* MCLK */
};

static uint32_t camera_on_gpio_table[] = {
	/* CAMERA */
	PCOM_GPIO_CFG(2, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT2 */
	PCOM_GPIO_CFG(3, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT3 */
	PCOM_GPIO_CFG(4, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT4 */
	PCOM_GPIO_CFG(5, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT5 */
	PCOM_GPIO_CFG(6, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT6 */
	PCOM_GPIO_CFG(7, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT7 */
	PCOM_GPIO_CFG(8, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT8 */
	PCOM_GPIO_CFG(9, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT9 */
	PCOM_GPIO_CFG(10, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT10 */
	PCOM_GPIO_CFG(11, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT11 */
	PCOM_GPIO_CFG(12, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_16MA), /* PCLK */
	PCOM_GPIO_CFG(13, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* HSYNC_IN */
	PCOM_GPIO_CFG(14, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* VSYNC_IN */
	PCOM_GPIO_CFG(15, 1, GPIO_OUTPUT, GPIO_PULL_DOWN, GPIO_16MA), /* MCLK */
};

static uint32_t camera_off_gpio_12pins_table[] = {
	/* CAMERA */
	PCOM_GPIO_CFG(0, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT0 */
	PCOM_GPIO_CFG(1, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT1 */
	PCOM_GPIO_CFG(2, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT2 */
	PCOM_GPIO_CFG(3, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT3 */
	PCOM_GPIO_CFG(4, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT4 */
	PCOM_GPIO_CFG(5, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT5 */
	PCOM_GPIO_CFG(6, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT6 */
	PCOM_GPIO_CFG(7, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT7 */
	PCOM_GPIO_CFG(8, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT8 */
	PCOM_GPIO_CFG(9, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT9 */
	PCOM_GPIO_CFG(10, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT10 */
	PCOM_GPIO_CFG(11, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT11 */
	PCOM_GPIO_CFG(12, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* PCLK */
	PCOM_GPIO_CFG(13, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* HSYNC_IN */
	PCOM_GPIO_CFG(14, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* VSYNC_IN */
	PCOM_GPIO_CFG(15, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* MCLK */
};

static uint32_t camera_on_gpio_12pins_table[] = {
	/* CAMERA */
	PCOM_GPIO_CFG(0, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT0 */
	PCOM_GPIO_CFG(1, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT1 */
	PCOM_GPIO_CFG(2, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT2 */
	PCOM_GPIO_CFG(3, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT3 */
	PCOM_GPIO_CFG(4, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT4 */
	PCOM_GPIO_CFG(5, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT5 */
	PCOM_GPIO_CFG(6, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT6 */
	PCOM_GPIO_CFG(7, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT7 */
	PCOM_GPIO_CFG(8, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT8 */
	PCOM_GPIO_CFG(9, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT9 */
	PCOM_GPIO_CFG(10, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT10 */
	PCOM_GPIO_CFG(11, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT11 */
	PCOM_GPIO_CFG(12, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_16MA), /* PCLK */
	PCOM_GPIO_CFG(13, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* HSYNC_IN */
	PCOM_GPIO_CFG(14, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* VSYNC_IN */
	PCOM_GPIO_CFG(15, 1, GPIO_OUTPUT, GPIO_PULL_DOWN, GPIO_16MA), /* MCLK */
};

static void config_gpio_table(uint32_t *table, int len)
{
	int n;
	unsigned id;
	for (n = 0; n < len; n++) {
		id = table[n];
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX, &id, 0);
	}
}

void config_hero_camera_on_gpios(void)
{
	/*Add for judage it's 10 pins or 12 pins platform ----->*/
	if  (is_12pin_camera()) {
		config_gpio_table(camera_on_gpio_12pins_table,
				ARRAY_SIZE(camera_on_gpio_12pins_table));
	} else {
		config_gpio_table(camera_on_gpio_table,
				ARRAY_SIZE(camera_on_gpio_table));
	}
	/*End Of Add for judage it's 10 pins or 12 pins platform*/
}

void config_hero_camera_off_gpios(void)
{
	/*Add for judage it's 10 pins or 12 pins platform ----->*/
	if (is_12pin_camera()) {
		config_gpio_table(camera_off_gpio_12pins_table,
		ARRAY_SIZE(camera_off_gpio_12pins_table));
	} else {
		config_gpio_table(camera_off_gpio_table,
		ARRAY_SIZE(camera_off_gpio_table));
	}
	/*End Of Add for judage it's 10 pins or 12 pins platform*/
}

static void __init config_gpios(void)
{
	config_gpio_table(gpio_table, ARRAY_SIZE(gpio_table));
	config_hero_camera_off_gpios();
}

static struct msm_acpu_clock_platform_data hero_clock_data = {
	.acpu_switch_time_us = 20,
	.max_speed_delta_khz = 256000,
	.vdd_switch_time_us = 62,
	.power_collapse_khz = 19200000,
#if defined(CONFIG_TURBO_MODE) 
        .wait_for_irq_khz = 176000000, 
#else 
        .wait_for_irq_khz = 128000000, 
#endif 
};

#ifdef CONFIG_SERIAL_MSM_HS
static struct msm_serial_hs_platform_data msm_uart_dm1_pdata = {
	.rx_wakeup_irq = MSM_GPIO_TO_INT(45),
	.inject_rx_on_wakeup = 1,
	.rx_to_inject = 0x32,
};
#endif

static void __init hero_init(void)
{
	int rc;
	printk("hero_init() revision = 0x%X\n", system_rev);

	/*
	 * Setup common MSM GPIOS
	 */
	config_gpios();

	msm_hw_reset_hook = hero_reset;

	msm_acpu_clock_init(&hero_clock_data);

	/* adjust GPIOs based on bootloader request */
	printk("hero_init: cpld_usb_hw2_sw = %d\n", cpld_usb_h2w_sw);
	gpio_set_value(HERO_GPIO_USB_H2W_SW, cpld_usb_h2w_sw);

#if defined(CONFIG_MSM_SERIAL_DEBUGGER)
	if (!opt_disable_uart3)
		msm_serial_debug_init(MSM_UART3_PHYS, INT_UART3,
				      &msm_device_uart3.dev, 1,
				      MSM_GPIO_TO_INT(86));
#endif

	/* gpio_configure(108, IRQF_TRIGGER_LOW); */

	/* H2W pins <-> UART3, Bluetooth <-> UART1 */
	gpio_set_value(HERO_GPIO_H2W_SEL0, 0);
	gpio_set_value(HERO_GPIO_H2W_SEL1, 1);
	/* put the AF VCM in powerdown mode to avoid noise */
	if (hero_is_5M_camera())
		hero_gpio_write(NULL, HERO_GPIO_VCM_PWDN, 0);
	else
		hero_gpio_write(NULL, HERO_GPIO_VCM_PWDN, 1);
	mdelay(100);

	printk(KERN_DEBUG "hero_is_5M_camera=%d\n",
	       hero_is_5M_camera());
	printk(KERN_DEBUG "is_12pin_camera=%d\n", is_12pin_camera());
#ifdef CONFIG_SERIAL_MSM_HS
	msm_device_uart_dm1.dev.platform_data = &msm_uart_dm1_pdata;
#endif
	msm_add_usb_devices(hero_phy_reset);

	if (32 == smi_sz) {
		switch (hero_get_die_size()) {
		case EBI1_DUAL_128MB_128MB:
			msm_add_mem_devices(&pmem_setting_32_dual);
			break;
		case EBI1_MONO_256MB:
			msm_add_mem_devices(&pmem_setting_32_mono);
			break;
		default:
			msm_add_mem_devices(&pmem_setting_32);
			break;
		}
	} else
		msm_add_mem_devices(&pmem_setting_64);

	rc = hero_init_mmc(system_rev);
	if (rc)
		printk(KERN_CRIT "%s: MMC init failure (%d)\n", __func__, rc);

#ifdef CONFIG_WIFI_MEM_PREALLOC
	rc = hero_init_wifi_mem();
	if (rc) {
		printk(KERN_CRIT "%s: WiFi memory init failure (%d)\n",
		       __func__, rc);
	}
#endif
	msm_init_pmic_vibrator();

	if(system_rev != 0x80)
		hero_search_button_info.keymap = hero_search_button_v1;

	if (is_12pin_camera())
		gpio_tp_ls_en = HERO20_TP_LS_EN;
	gpio_request(gpio_tp_ls_en, "tp_ls_en");

	i2c_register_board_info(0, i2c_devices, ARRAY_SIZE(i2c_devices));
	platform_add_devices(devices, ARRAY_SIZE(devices));
	
	i2c_register_board_info(0, &i2c_bma150, 1);

//	if (hero_engineerid() || system_rev > 2) {
//		if (system_rev >= 4) {
//			microp_data.num_pins = ARRAY_SIZE(microp_pins_skuid_3);
//			microp_data.pin_config = microp_pins_skuid_3;
//		} else if (system_rev >= 3) {
//			microp_data.num_pins = ARRAY_SIZE(microp_pins_skuid_2);
//			microp_data.pin_config = microp_pins_skuid_2;
//		} else {
//			microp_data.num_pins = ARRAY_SIZE(microp_pins_skuid_1);
//			microp_data.pin_config = microp_pins_skuid_1;
//		}
//		microp_data.cabc_backlight_enable = 1;
//	}

#ifdef CONFIG_HTC_AUDIO_JACK
	if (hero_get_skuid() == 0x22800) {
		platform_device_register(&hero_audio_jack);
	}
#endif
}

static struct map_desc hero_io_desc[] __initdata = {
	{
		.virtual = HERO_CPLD_BASE,
		.pfn     = __phys_to_pfn(HERO_CPLD_START),
		.length  = HERO_CPLD_SIZE,
		.type    = MT_DEVICE_NONSHARED
	}
};


unsigned int hero_get_hwid(void)
{
	return hwid;
}

unsigned int hero_get_skuid(void)
{
	return skuid;
}

unsigned hero_engineerid(void)
{
	return engineerid;
}

unsigned int hero_get_die_size(void)
{
	return (smi_sz == 64) ? 1 : die_sz;
}

int hero_is_5M_camera(void)
{
	int ret = 0;
	if (hero_get_skuid() == 0x1FF00 && !(hero_engineerid() & 0x02))
		ret = 1;
	else if (hero_get_skuid() == 0x20100 && !(hero_engineerid() & 0x02))
		ret = 1;
	else if (hero_get_skuid() == 0x22880 && !(hero_engineerid() & 0x02))
		ret = 1;
	return ret;
}

/* it can support 3M and 5M sensor */
unsigned int is_12pin_camera(void)
{
	unsigned int ret = 0;

	if (hero_get_skuid() == 0x1FF00 || hero_get_skuid() == 0x20100 || hero_get_skuid() == 0x22800)
		ret = 1;
	else
		ret = 0;
	return ret;
}

int hero_get_smi_size(void)
{
	printk(KERN_DEBUG "get_smi_size=%d\n", smi_sz);
	return smi_sz;
}

static void __init hero_fixup(struct machine_desc *desc, struct tag *tags,
				  char **cmdline, struct meminfo *mi)
{
	smi_sz = parse_tag_smi((const struct tag *)tags);
	printk("hero_fixup:smisize=%d\n", smi_sz);
	hwid = parse_tag_hwid((const struct tag *)tags);
	printk("hero_fixup:hwid=0x%x\n", hwid);
	skuid = parse_tag_skuid((const struct tag *)tags);
	printk("hero_fixup:skuid=0x%x\n", skuid);
	engineerid = parse_tag_engineerid((const struct tag *)tags);
	printk("hero_fixup:engineerid=0x%x\n", engineerid);
	die_sz = parse_tag_monodie((const struct tag *)tags);
	printk("hero_fixup:diesize=0x%x\n", die_sz);

	mi->nr_banks = 1;
	mi->bank[0].start = PHYS_OFFSET;
	mi->bank[0].node = PHYS_TO_NID(PHYS_OFFSET);
#if	defined(CONFIG_MSM_AMSS_SUPPORT_256MB_EBI1)
	if (smi_sz == 32) {
		switch (hero_get_die_size()) {
		case EBI1_DUAL_128MB_128MB:
			mi->nr_banks = 2;
#ifdef CONFIG_HOLES_IN_ZONE
			mi->bank[0].size = 0x6d00000;
#else
			mi->bank[0].size = 0x6c00000;
#endif
			mi->bank[1].start = 0x20000000;
			mi->bank[1].size = 0x5800000;
			mi->bank[1].node = PHYS_TO_NID(0x20000000);
			break;
		case EBI1_MONO_256MB:
			mi->nr_banks = 2;
#ifdef CONFIG_HOLES_IN_ZONE
			mi->bank[0].size = 0x6d00000;
#else
			mi->bank[0].size = 0x6c00000;
#endif
			mi->bank[1].start = 0x18000000;
			mi->bank[1].size = 0x5800000;
			mi->bank[1].node = PHYS_TO_NID(0x18000000);
			break;
		default:
			mi->bank[0].size = MSM_EBI_SMI32_256MB_SIZE;
			break;
		}
	} else if (smi_sz == 64) {
		mi->bank[0].size = MSM_EBI_SMI64_128MB_SIZE;
	} else {
		printk(KERN_ERR "can not get smi size\n");

		/*Give a default value when not get smi size*/
		smi_sz = 64;
		mi->bank[0].size = MSM_EBI_SMI64_128MB_SIZE;
		printk(KERN_ERR "use default  :  smisize=%d\n", smi_sz);
	}
#else
	if (smi_sz == 32) {
		switch (hero_get_die_size()) {
		case EBI1_DUAL_128MB_128MB:
			mi->nr_banks = 2;
#ifdef CONFIG_HOLES_IN_ZONE
			mi->bank[0].size = 0x6d00000;
#else
			mi->bank[0].size = 0x6c00000;
#endif
			mi->bank[1].start = 0x20000000;
			mi->bank[1].size = 0x5800000;
			mi->bank[1].node = PHYS_TO_NID(0x20000000);
			break;
		case EBI1_MONO_256MB:
			mi->nr_banks = 2;
#ifdef CONFIG_HOLES_IN_ZONE
			mi->bank[0].size = 0x6d00000;
#else
			mi->bank[0].size = 0x6c00000;
#endif
			mi->bank[1].start = 0x18000000;
			mi->bank[1].size = 0x5800000;
			mi->bank[1].node = PHYS_TO_NID(0x18000000);
			break;
		default:
			mi->bank[0].size = (84*1024*1024);
			break;
		}
	} else if (smi_sz == 64) {
		mi->bank[0].size = SMI64_MSM_LINUX_SIZE;	//(101*1024*1024);
	} else {
		printk(KERN_ERR "can not get smi size\n");

		/*Give a default value when not get smi size*/
		smi_sz = 64;
		mi->bank[0].size = SMI64_MSM_LINUX_SIZE;	//(101*1024*1024);
		printk(KERN_ERR "use default  :  smisize=%d\n", smi_sz);
	}
#endif
	printk("hero_fixup:bank size=0x%x\n", mi->bank[0].size);
}

static void __init hero_map_io(void)
{
	msm_map_common_io();
	iotable_init(hero_io_desc, ARRAY_SIZE(hero_io_desc));
	msm_clock_init();
}

MACHINE_START(HERO, "hero")
/* Maintainer: Brian Swetland <swetland@google.com> */
#ifdef CONFIG_MSM_DEBUG_UART
	.phys_io        = MSM_DEBUG_UART_PHYS,
	.io_pg_offst    = ((MSM_DEBUG_UART_BASE) >> 18) & 0xfffc,
#endif
#if defined(CONFIG_MSM_AMSS_SUPPORT_256MB_EBI1)
	.boot_params    = 0x19200100,
#else
	.boot_params    = 0x10000100,
#endif
	.fixup          = hero_fixup,
	.map_io         = hero_map_io,
	.init_irq       = hero_init_irq,
	.init_machine   = hero_init,
	.timer          = &msm_timer,
MACHINE_END
