#include <linux/module.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/input.h>
#include <linux/moduleparam.h>
#include <linux/hrtimer.h>
#include <linux/fixp-arith.h>

#include "hid-ftec.h"

// parameter to set the initial range
// if unset, the wheels max range is used
int init_range = 0;
module_param(init_range, int, 0);

#define DEFAULT_TIMER_PERIOD 2

#define FF_EFFECT_STARTED 0
#define FF_EFFECT_ALLSET 1
#define FF_EFFECT_PLAYING 2
#define FF_EFFECT_UPDATING 3

#define STOP_EFFECT(state) ((state)->flags = 0)

#undef fixp_sin16
#define fixp_sin16(v) (((v % 360) > 180)? -(fixp_sin32((v % 360) - 180) >> 16) : fixp_sin32(v) >> 16)

#define DEBUG(...) pr_debug("ftecff: " __VA_ARGS__)
#define time_diff(a,b) ({ \
		typecheck(unsigned long, a); \
		typecheck(unsigned long, b); \
		((a) - (long)(b)); })
#define JIFFIES2MS(jiffies) ((jiffies) * 1000 / HZ)

static int timer_msecs = DEFAULT_TIMER_PERIOD;
static int spring_level = 100;
static int damper_level = 100;
static int friction_level = 100;

static int profile = 1;
module_param(profile, int, 0660);
MODULE_PARM_DESC(profile, "Enable profile debug messages.");

#define FTEC_TUNING_REPORT_SIZE 64
#define FTEC_WHEEL_REPORT_SIZE 34

#define ADDR_SLOT 	0x02
#define ADDR_SEN 	0x03
#define ADDR_FF 	0x04
#define ADDR_SHO 	0x05
#define ADDR_BLI 	0x06											
#define ADDR_DRI 	0x09
#define ADDR_FOR 	0x0a
#define ADDR_SPR 	0x0b
#define ADDR_DPR 	0x0c
#define ADDR_FEI 	0x11

static const signed short ftecff_wheel_effects[] = {
	FF_CONSTANT,
	FF_SPRING,
	FF_DAMPER,
	FF_PERIODIC,
	FF_SINE,
	FF_SQUARE,
	FF_TRIANGLE,
	FF_SAW_UP,
	FF_SAW_DOWN,
	-1
};

/* This is realy weird... if i put a value >0x80 into the report,
   the actual value send to the device will be 0x7f. I suspect it has
   s.t. todo with the report fields min/max range, which is -127 to 128
   but I don't know how to handle this properly... So, here a hack around 
   this issue
*/
static void fix_values(s32 *values) {
	int i;
	for(i=0;i<7;i++) {
		if (values[i]>=0x80)
			values[i] = -0x100 + values[i];
	}
}

static u8 num[11][8] = {  { 1,1,1,1,1,1,0,0 },  // 0
						{ 0,1,1,0,0,0,0,0 },  // 1
						{ 1,1,0,1,1,0,1,0 },  // 2
						{ 1,1,1,1,0,0,1,0 },  // 3
						{ 0,1,1,0,0,1,1,0 },  // 4
						{ 1,0,1,1,0,1,1,0 },  // 5
						{ 1,0,1,1,1,1,1,0 },  // 6
						{ 1,1,1,0,0,0,0,0 },  // 7
						{ 1,1,1,1,1,1,1,0 },  // 8
						{ 1,1,1,0,0,1,1,0 },  // 9
						{ 0,0,0,0,0,0,0,1}};  // dot

static u8 seg_bits(u8 value) {
	int i;
	u8 bits = 0;

	for( i=0; i<8; i++) {
		if (num[value][i]) 
			bits |= 1 << i;
	}
	return bits;
}

static void send_report_request_to_device(struct ftec_drv_data *drv_data)
{
	struct hid_device *hdev = drv_data->hid;
	struct hid_report *report = drv_data->report;

	if (hdev->product != CSR_ELITE_WHEELBASE_DEVICE_ID)
	{
		fix_values(report->field[0]->value);
	}

	hid_hw_request(hdev, report, HID_REQ_SET_REPORT);
}

static void ftec_set_range(struct hid_device *hid, u16 range)
{
	struct ftec_drv_data *drv_data;
	unsigned long flags;
	s32 *value;

	drv_data = hid_get_drvdata(hid);
	if (!drv_data) {
		hid_err(hid, "Private driver data not found!\n");
		return;
	}
	value = drv_data->report->field[0]->value;
	dbg_hid("setting range to %u\n", range);

	/* Prepare "coarse" limit command */
	spin_lock_irqsave(&drv_data->report_lock, flags);
	value[0] = 0xf5;
	value[1] = 0x00;
	value[2] = 0x00;
	value[3] = 0x00;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;
	send_report_request_to_device(drv_data);

	value[0] = 0xf8;
	value[1] = 0x09;
	value[2] = 0x01;
	value[3] = 0x06;
	value[4] = 0x01;
	value[5] = 0x00;
	value[6] = 0x00;
	send_report_request_to_device(drv_data);

	value[0] = 0xf8;
	value[1] = 0x81;
	value[2] = range&0xff;
	value[3] = (range>>8)&0xff;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;
	send_report_request_to_device(drv_data);
	spin_unlock_irqrestore(&drv_data->report_lock, flags);
}

/* Export the currently set range of the wheel */
static ssize_t ftec_range_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct hid_device *hid = to_hid_device(dev);
	struct ftec_drv_data *drv_data;
	size_t count;

	drv_data = hid_get_drvdata(hid);
	if (!drv_data) {
		hid_err(hid, "Private driver data not found!\n");
		return 0;
	}

	count = scnprintf(buf, PAGE_SIZE, "%u\n", drv_data->range);
	return count;
}

/* Set range to user specified value, call appropriate function
 * according to the type of the wheel */
static ssize_t ftec_range_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct hid_device *hid = to_hid_device(dev);
	struct ftec_drv_data *drv_data;
	u16 range = simple_strtoul(buf, NULL, 10);

	drv_data = hid_get_drvdata(hid);
	if (!drv_data) {
		hid_err(hid, "Private driver data not found!\n");
		return -EINVAL;
	}

	if (range == 0)
		range = drv_data->max_range;

	/* Check if the wheel supports range setting
	 * and that the range is within limits for the wheel */
	if (range >= drv_data->min_range && range <= drv_data->max_range) {
		ftec_set_range(hid, range);
		drv_data->range = range;
	}

	return count;
}
static DEVICE_ATTR(range, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_range_show, ftec_range_store);


/* Export the current wheel id */
static ssize_t ftec_wheel_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct hid_device *hid = to_hid_device(dev);
	struct usb_device *udev = interface_to_usbdev(to_usb_interface(hid->dev.parent));
	u8 *buffer = kcalloc(FTEC_WHEEL_REPORT_SIZE, sizeof(u8), GFP_KERNEL);
	size_t count = 0;
	int ret, actual_len;
	u16 wheel_id;
    	
	// request current values
	buffer[0] = 0x01;
	buffer[1] = 0xf8;
	buffer[2] = 0x09;
	buffer[3] = 0x01;
	buffer[4] = 0x06;

	ret = hid_hw_output_report(hid, buffer, 8);
	if (ret < 0) {
		kfree(buffer);
		return count;
	}

	// FXIME: again ... (values only update 2nd time?)
	ret = hid_hw_output_report(hid, buffer, 8);
	if (ret < 0) {
		kfree(buffer);
		return count;
	}

	// reset memory
	memset((void*)buffer, 0, FTEC_WHEEL_REPORT_SIZE); 

	// read values
	ret = usb_interrupt_msg(udev, usb_rcvintpipe(udev, 81),
				buffer, FTEC_WHEEL_REPORT_SIZE, &actual_len,
				USB_CTRL_SET_TIMEOUT);

	memcpy((void*)&wheel_id, &buffer[0x1e], sizeof(u16));
	kfree(buffer);

	count = scnprintf(buf, PAGE_SIZE, "0x%04x\n", wheel_id);
	return count;
}
static DEVICE_ATTR(wheel_id, S_IRUSR | S_IRGRP | S_IROTH, ftec_wheel_show, NULL);


static ssize_t ftec_set_display(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct hid_device *hid = to_hid_device(dev);
	struct ftec_drv_data *drv_data;
	unsigned long flags;
	s32 *value;
	s16 val = simple_strtol(buf, NULL, 10);

	drv_data = hid_get_drvdata(hid);
	if (!drv_data) {
		hid_err(hid, "Private driver data not found!\n");
		return -EINVAL;
	}

	// dbg_hid(" ... set_display %i\n", val);
	
	value = drv_data->report->field[0]->value;

	spin_lock_irqsave(&drv_data->report_lock, flags);
	value[0] = 0xf8;
	value[1] = 0x09;
	value[2] = 0x01;
	value[3] = 0x02;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;

	if (val>=0) {
		value[4] = seg_bits((val/100)%100);
		value[5] = seg_bits((val/10)%10);
		value[6] = seg_bits(val%10);
	}

	send_report_request_to_device(drv_data);
	spin_unlock_irqrestore(&drv_data->report_lock, flags);

	return count;
}
static DEVICE_ATTR(display, S_IWUSR | S_IWGRP, NULL, ftec_set_display);

static int ftec_tuning_read(struct hid_device *hid, u8 *buf) {
	struct usb_device *dev = interface_to_usbdev(to_usb_interface(hid->dev.parent));
	int ret, actual_len;
    	
	// request current values
	buf[0] = 0xff;
	buf[1] = 0x03;
	buf[2] = 0x02;

	ret = hid_hw_output_report(hid, buf, FTEC_TUNING_REPORT_SIZE);
	if (ret < 0)
		goto out;

	// reset memory
	memset((void*)buf, 0, FTEC_TUNING_REPORT_SIZE); 

	// read values
	ret = usb_interrupt_msg(dev, usb_rcvintpipe(dev, 81),
				buf, FTEC_TUNING_REPORT_SIZE, &actual_len,
				USB_CTRL_SET_TIMEOUT);
out:
	return ret;
}

static int ftec_tuning_write(struct hid_device *hid, int addr, int val) {
	u8 *buf = kcalloc(FTEC_TUNING_REPORT_SIZE+1, sizeof(u8), GFP_KERNEL);
	int ret;
    	
	// shift by 1 so that values are at correct location for write back
	if (ftec_tuning_read(hid, buf+1) < 0)
		goto out;

	dbg_hid(" ... ftec_tuning_write %i; current: %i; new:%i\n", addr, buf[addr+1], val);

	// update requested value and write back
	buf[0] = 0xff;
	buf[1] = 0x03;
	buf[2] = 0x00;
	buf[addr+1] = val;
	ret = hid_hw_output_report(hid, buf, FTEC_TUNING_REPORT_SIZE);

out:
    kfree(buf);
	return 0;
}

static int ftec_tuning_select(struct hid_device *hid, int slot) {
	u8 *buf = kcalloc(FTEC_TUNING_REPORT_SIZE, sizeof(u8), GFP_KERNEL);
    int ret;
    	
	if (ftec_tuning_read(hid, buf) < 0)
		goto out;
		
	// return if already selected
	if (buf[ADDR_SLOT] == slot || slot<=0 || slot>NUM_TUNING_SLOTS) {
		dbg_hid(" ... ftec_tuning_select slot already selected or invalid value; current: %i; new:%i\n", buf[ADDR_SLOT], slot);
		goto out;
	}

	dbg_hid(" ... ftec_tuning_select current: %i; new:%i\n", buf[ADDR_SLOT], slot);

	// reset memory
	memset((void*)buf, 0, FTEC_TUNING_REPORT_SIZE); 

	buf[0] = 0xff;
	buf[1] = 0x03;
	buf[2] = 0x01;
	buf[3] = slot&0xff;

	ret = hid_hw_output_report(hid, buf, FTEC_TUNING_REPORT_SIZE);
	if (ret < 0)
		goto out;

out:
    kfree(buf);
	return 0;
}


static int ftec_tuning_get_addr(struct device_attribute *attr) {
	int type = 0;
	if(strcmp(attr->attr.name, "SLOT") == 0)
		type = ADDR_SLOT;
	else if(strcmp(attr->attr.name, "SEN") == 0)
		type = ADDR_SEN;
	else if(strcmp(attr->attr.name, "FF") == 0)
		type = ADDR_FF;
	else if(strcmp(attr->attr.name, "DRI") == 0)
		type = ADDR_DRI;
	else if(strcmp(attr->attr.name, "FEI") == 0)
		type = ADDR_FEI;
	else if(strcmp(attr->attr.name, "FOR") == 0)
		type = ADDR_FOR;
	else if(strcmp(attr->attr.name, "SPR") == 0)
		type = ADDR_SPR;
	else if(strcmp(attr->attr.name, "DPR") == 0)
		type = ADDR_DPR;
	else if(strcmp(attr->attr.name, "BLI") == 0)
		type = ADDR_BLI;												
	else if(strcmp(attr->attr.name, "SHO") == 0)
		type = ADDR_SHO;
	else {
		dbg_hid("Unknown attribute %s\n", attr->attr.name);
	}
	return type;
}

static ssize_t ftec_tuning_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hid = to_hid_device(dev);
	u8 *buffer = kcalloc(FTEC_TUNING_REPORT_SIZE, sizeof(u8), GFP_KERNEL);
	int addr = ftec_tuning_get_addr(attr);
	size_t count = 0;
	s8 value = 0;

	dbg_hid(" ... ftec_tuning_show %s, %x\n", attr->attr.name, addr);

	if (addr > 0 && ftec_tuning_read(hid, buffer) >= 0) {
		memcpy((void*)&value, &buffer[addr], sizeof(s8));
		count = scnprintf(buf, PAGE_SIZE, "%i\n", value);
	}
	kfree(buffer);
	return count;
}

static ssize_t ftec_tuning_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct hid_device *hid = to_hid_device(dev);
	int addr;

	s16 val = simple_strtol(buf, NULL, 10);
	dbg_hid(" ... ftec_tuning_store %s %i\n", attr->attr.name, val);

	addr = ftec_tuning_get_addr(attr);
	if (addr == ADDR_SLOT) {
		ftec_tuning_select(hid, val);
	} else if (addr > 0) {
		ftec_tuning_write(hid, addr, val);
	}
	return count;
}

static ssize_t ftec_tuning_reset(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct hid_device *hid = to_hid_device(dev);
	u8 *buffer = kcalloc(FTEC_TUNING_REPORT_SIZE, sizeof(u8), GFP_KERNEL);
	int ret;
    	
	// request current values
	buffer[0] = 0xff;
	buffer[1] = 0x03;
	buffer[2] = 0x04;

	ret = hid_hw_output_report(hid, buffer, FTEC_TUNING_REPORT_SIZE);
	
	return count;
}

static DEVICE_ATTR(RESET, S_IWUSR  | S_IWGRP, NULL, ftec_tuning_reset);
static DEVICE_ATTR(SLOT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);
static DEVICE_ATTR(SEN, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);
static DEVICE_ATTR(FF, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);
static DEVICE_ATTR(DRI, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);
static DEVICE_ATTR(FEI, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);
static DEVICE_ATTR(FOR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);
static DEVICE_ATTR(SPR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);
static DEVICE_ATTR(DPR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);
static DEVICE_ATTR(BLI, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);
static DEVICE_ATTR(SHO, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, ftec_tuning_show, ftec_tuning_store);


#ifdef CONFIG_LEDS_CLASS
static void ftec_set_leds(struct hid_device *hid, u16 leds)
{
	struct ftec_drv_data *drv_data;
	unsigned long flags;
	s32 *value;
	u16 _leds = 0;
	int i;

	// dbg_hid(" ... set_leds base %04X\n", leds);

	drv_data = hid_get_drvdata(hid);
	if (!drv_data) {
		hid_err(hid, "Private driver data not found!\n");
		return;
	}

	value = drv_data->report->field[0]->value;

	spin_lock_irqsave(&drv_data->report_lock, flags);
	value[0] = 0xf8;
	value[1] = 0x13;
	value[2] = leds&0xff;
	value[3] = 0x00;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;
	
	send_report_request_to_device(drv_data);

	// reshuffle, since first led is highest bit
	for( i=0; i<LEDS; i++) {
		if (leds>>i & 1) _leds |= 1 << (LEDS-i-1);
	}

	// dbg_hid(" ... set_leds wheel %04X\n", _leds);

	value = drv_data->report->field[0]->value;

	value[0] = 0xf8;
	value[1] = 0x09;
	value[2] = 0x08;
	value[3] = (_leds>>8)&0xff;
	value[4] = _leds&0xff;
	value[5] = 0x00;
	value[6] = 0x00;
	
	send_report_request_to_device(drv_data);
	spin_unlock_irqrestore(&drv_data->report_lock, flags);
}

static void ftec_led_set_brightness(struct led_classdev *led_cdev,
			enum led_brightness value)
{
	struct device *dev = led_cdev->dev->parent;
	struct hid_device *hid = to_hid_device(dev);
	struct ftec_drv_data *drv_data = hid_get_drvdata(hid);
	int i, state = 0;

	if (!drv_data) {
		hid_err(hid, "Device data not found.");
		return;
	}

	for (i = 0; i < LEDS; i++) {
		if (led_cdev != drv_data->led[i])
			continue;
		state = (drv_data->led_state >> i) & 1;
		if (value == LED_OFF && state) {
			drv_data->led_state &= ~(1 << i);
			ftec_set_leds(hid, drv_data->led_state);
		} else if (value != LED_OFF && !state) {
			drv_data->led_state |= 1 << i;
			ftec_set_leds(hid, drv_data->led_state);
		}
		break;
	}
}

static enum led_brightness ftec_led_get_brightness(struct led_classdev *led_cdev)
{
	struct device *dev = led_cdev->dev->parent;
	struct hid_device *hid = to_hid_device(dev);
	struct ftec_drv_data *drv_data = hid_get_drvdata(hid);
	int i, value = 0;

	if (!drv_data) {
		hid_err(hid, "Device data not found.");
		return LED_OFF;
	}

	for (i = 0; i < LEDS; i++)
		if (led_cdev == drv_data->led[i]) {
			value = (drv_data->led_state >> i) & 1;
			break;
		}

	return value ? LED_FULL : LED_OFF;
}
#endif


static int ftec_init_led(struct hid_device *hid) {
	struct led_classdev *led;
	size_t name_sz;
	char *name;
	struct ftec_drv_data *drv_data;
	int ret, j;

	drv_data = hid_get_drvdata(hid);
	if (!drv_data) {
		hid_err(hid, "Cannot add device, private driver data not allocated\n");
		return -1;
	}

	{ 
		// wheel LED initialization sequence
		// not sure what's needed 
		s32 *value;
		value = drv_data->report->field[0]->value;

		value[0] = 0xf8;
		value[1] = 0x09;
		value[2] = 0x08;		
		value[3] = 0x01;
		value[4] = 0x00;
		value[5] = 0x00;
		value[6] = 0x00;

		send_report_request_to_device(drv_data);
	}

	drv_data->led_state = 0;
	for (j = 0; j < LEDS; j++)
		drv_data->led[j] = NULL;

	name_sz = strlen(dev_name(&hid->dev)) + 8;

	for (j = 0; j < LEDS; j++) {
		led = kzalloc(sizeof(struct led_classdev)+name_sz, GFP_KERNEL);
		if (!led) {
			hid_err(hid, "can't allocate memory for LED %d\n", j);
			goto err_leds;
		}

		name = (void *)(&led[1]);
		snprintf(name, name_sz, "%s::RPM%d", dev_name(&hid->dev), j+1);
		led->name = name;
		led->brightness = 0;
		led->max_brightness = 1;
		led->brightness_get = ftec_led_get_brightness;
		led->brightness_set = ftec_led_set_brightness;

		drv_data->led[j] = led;
		ret = led_classdev_register(&hid->dev, led);

		if (ret) {
			hid_err(hid, "failed to register LED %d. Aborting.\n", j);
err_leds:
			/* Deregister LEDs (if any) */
			for (j = 0; j < LEDS; j++) {
				led = drv_data->led[j];
				drv_data->led[j] = NULL;
				if (!led)
					continue;
				led_classdev_unregister(led);
				kfree(led);
			}
			return -1;
		}
	}
	return 0;
}

void ftecff_send_cmd(struct ftec_drv_data *drv_data, u8 *cmd)
{
	unsigned short i;
	unsigned long flags;
	s32 *value = drv_data->report->field[0]->value;

	spin_lock_irqsave(&drv_data->report_lock, flags);

	for(i = 0; i < 7; i++)
		value[i] = cmd[i];

	send_report_request_to_device(drv_data);
	spin_unlock_irqrestore(&drv_data->report_lock, flags);

	if (unlikely(profile))
		DEBUG("send_cmd: %02X %02X %02X %02X %02X %02X %02X", cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6]);
}

static __always_inline struct ff_envelope *ftecff_effect_envelope(struct ff_effect *effect)
{
	switch (effect->type) {
		case FF_CONSTANT:
			return &effect->u.constant.envelope;
		case FF_RAMP:
			return &effect->u.ramp.envelope;
		case FF_PERIODIC:
			return &effect->u.periodic.envelope;
	}

	return NULL;
}

static __always_inline void ftecff_update_state(struct ftecff_effect_state *state, const unsigned long now)
{
	struct ff_effect *effect = &state->effect;
	unsigned long phase_time;

	if (!__test_and_set_bit(FF_EFFECT_ALLSET, &state->flags)) {
		state->play_at = state->start_at + effect->replay.delay;
		if (!test_bit(FF_EFFECT_UPDATING, &state->flags)) {
			state->updated_at = state->play_at;
		}
		state->direction_gain = fixp_sin16(effect->direction * 360 / 0x10000);
		if (effect->type == FF_PERIODIC) {
			state->phase_adj = effect->u.periodic.phase * 360 / effect->u.periodic.period;
		}
		if (effect->replay.length) {
			state->stop_at = state->play_at + effect->replay.length;
		}
	}

	if (__test_and_clear_bit(FF_EFFECT_UPDATING, &state->flags)) {
		__clear_bit(FF_EFFECT_PLAYING, &state->flags);
		state->play_at = state->start_at + effect->replay.delay;
		state->direction_gain = fixp_sin16(effect->direction * 360 / 0x10000);
		if (effect->replay.length) {
			state->stop_at = state->play_at + effect->replay.length;
		}
		if (effect->type == FF_PERIODIC) {
			state->phase_adj = state->phase;
		}
	}

	state->envelope = ftecff_effect_envelope(effect);

	state->slope = 0;
	if (effect->type == FF_RAMP && effect->replay.length) {
		state->slope = ((effect->u.ramp.end_level - effect->u.ramp.start_level) << 16) / (effect->replay.length - state->envelope->attack_length - state->envelope->fade_length);
	}

	if (!test_bit(FF_EFFECT_PLAYING, &state->flags) && time_after_eq(now,
				state->play_at) && (effect->replay.length == 0 ||
					time_before(now, state->stop_at))) {
		__set_bit(FF_EFFECT_PLAYING, &state->flags);
	}

	if (test_bit(FF_EFFECT_PLAYING, &state->flags)) {
		state->time_playing = time_diff(now, state->play_at);
		if (effect->type == FF_PERIODIC) {
			phase_time = time_diff(now, state->updated_at);
			state->phase = (phase_time % effect->u.periodic.period) * 360 / effect->u.periodic.period;
			state->phase += state->phase_adj % 360;
		}
	}
}

void ftecff_update_slot(struct ftecff_slot *slot, struct ftecff_effect_parameters *parameters)
{
	u8 original_cmd[7];
	unsigned short i;
	int d1;
	int d2;
	int s1;
	int s2;

	memcpy(original_cmd, slot->current_cmd, sizeof(original_cmd));

	// select slot
	slot->current_cmd[0] = (slot->id<<4) | 0x1;

	// set params to zero
	for(i = 2; i < 7; i++)
		slot->current_cmd[i] = 0;

	if ((slot->effect_type == FF_CONSTANT && parameters->level == 0) ||
			(slot->effect_type != FF_CONSTANT && parameters->clip == 0)) {
		// disable slot
		slot->current_cmd[0] |= 0x2;
		if (original_cmd[0] != slot->current_cmd[0])
			slot->is_updated = 1;
		return;
	}

#define CLAMP_VALUE_U16(x) ((unsigned short)((x) > 0xffff ? 0xffff : (x)))
#define CLAMP_VALUE_S16(x) ((unsigned short)((x) <= -0x8000 ? -0x8000 : ((x) > 0x7fff ? 0x7fff : (x))))
#define TRANSLATE_FORCE(x) ((CLAMP_VALUE_S16(x) + 0x8000) >> 8)
#define SCALE_COEFF(x, bits) SCALE_VALUE_U16(abs(x) * 2, bits)
#define SCALE_VALUE_U16(x, bits) (CLAMP_VALUE_U16(x) >> (16 - bits))

	switch (slot->effect_type) {
		case FF_CONSTANT:
			slot->current_cmd[2] = TRANSLATE_FORCE(parameters->level);
			break;
		case FF_SPRING:
			d1 = SCALE_VALUE_U16(((parameters->d1) + 0x8000) & 0xffff, 11);
			d2 = SCALE_VALUE_U16(((parameters->d2) + 0x8000) & 0xffff, 11);
			s1 = parameters->k1 < 0;
			s2 = parameters->k2 < 0;
			slot->current_cmd[2] = d1 >> 3;
			slot->current_cmd[3] = d2 >> 3;
			slot->current_cmd[4] = (SCALE_COEFF(parameters->k2, 4) << 4) + SCALE_COEFF(parameters->k1, 4);
			// slot->current_cmd[5] = ((d2 & 7) << 5) + ((d1 & 7) << 1) + (s2 << 4) + s1;
			slot->current_cmd[6] = SCALE_VALUE_U16(parameters->clip, 8);
			// dbg_hid("spring: %i %i %i %i %i %i %i %i %i\n",
			// 	parameters->d1, parameters->d2, parameters->k1, parameters->k2, parameters->clip,
			// 	slot->current_cmd[2], slot->current_cmd[3], slot->current_cmd[4], slot->current_cmd[6]);
			break;
		case FF_DAMPER:
			s1 = parameters->k1 < 0;
			s2 = parameters->k2 < 0;
			slot->current_cmd[2] = SCALE_COEFF(parameters->k1, 4);
			// slot->current_cmd[3] = s1;
			slot->current_cmd[4] = SCALE_COEFF(parameters->k2, 4);
			// slot->current_cmd[5] = s2;
			slot->current_cmd[6] = SCALE_VALUE_U16(parameters->clip, 8);
			// dbg_hid("damper: %i %i %i %i %i %i %i %i\n",
			// 	parameters->d1, parameters->d2, parameters->k1, parameters->k2, parameters->clip,
			// 	slot->current_cmd[2], slot->current_cmd[4], slot->current_cmd[6]);
			break;
		case FF_FRICTION:
			// s1 = parameters->k1 < 0;
			// s2 = parameters->k2 < 0;
			// slot->current_cmd[1] = 0x0e;
			// slot->current_cmd[2] = SCALE_COEFF(parameters->k1, 8);
			// slot->current_cmd[3] = SCALE_COEFF(parameters->k2, 8);
			// slot->current_cmd[4] = SCALE_VALUE_U16(parameters->clip, 8);
			// slot->current_cmd[5] = (s2 << 4) + s1;
			// slot->current_cmd[6] = 0;
			// dbg_hid("friction: %i %i %i %i %i\n",
			// 	parameters->k1, parameters->k2, parameters->clip,
			// 	slot->current_cmd[4], slot->current_cmd[6]);
			break;
	}

	// check if slot needs to be updated
	for(i = 0; i < 7; i++) {
		if (original_cmd[i] != slot->current_cmd[i]) {
			slot->is_updated = 1;
			break;
		}
	}
}

static __always_inline int ftecff_calculate_constant(struct ftecff_effect_state *state)
{
	int level = state->effect.u.constant.level;
	int level_sign;
	long d, t;

	if (state->time_playing < state->envelope->attack_length) {
		level_sign = level < 0 ? -1 : 1;
		d = level - level_sign * state->envelope->attack_level;
		level = level_sign * state->envelope->attack_level + d * state->time_playing / state->envelope->attack_length;
	} else if (state->effect.replay.length) {
		t = state->time_playing - state->effect.replay.length + state->envelope->fade_length;
		if (t > 0) {
			level_sign = level < 0 ? -1 : 1;
			d = level - level_sign * state->envelope->fade_level;
			level = level - d * t / state->envelope->fade_length;
		}
	}

	return state->direction_gain * level / 0x7fff;
}

static __always_inline int ftecff_calculate_periodic(struct ftecff_effect_state *state)
{
	struct ff_periodic_effect *periodic = &state->effect.u.periodic;
	int level = periodic->offset;
	int magnitude = periodic->magnitude;
	int magnitude_sign = magnitude < 0 ? -1 : 1;
	long d, t;

	if (state->time_playing < state->envelope->attack_length) {
		d = magnitude - magnitude_sign * state->envelope->attack_level;
		magnitude = magnitude_sign * state->envelope->attack_level + d * state->time_playing / state->envelope->attack_length;
	} else if (state->effect.replay.length) {
		t = state->time_playing - state->effect.replay.length + state->envelope->fade_length;
		if (t > 0) {
			d = magnitude - magnitude_sign * state->envelope->fade_level;
			magnitude = magnitude - d * t / state->envelope->fade_length;
		}
	}

	switch (periodic->waveform) {
		case FF_SINE:
			level += fixp_sin16(state->phase) * magnitude / 0x7fff;
			break;
		case FF_SQUARE:
			level += (state->phase < 180 ? 1 : -1) * magnitude;
			break;
		case FF_TRIANGLE:
			level += abs(state->phase * magnitude * 2 / 360 - magnitude) * 2 - magnitude;
			break;
		case FF_SAW_UP:
			level += state->phase * magnitude * 2 / 360 - magnitude;
			break;
		case FF_SAW_DOWN:
			level += magnitude - state->phase * magnitude * 2 / 360;
			break;
	}

	return state->direction_gain * level / 0x7fff;
}

static __always_inline void ftecff_calculate_spring(struct ftecff_effect_state *state, struct ftecff_effect_parameters *parameters)
{
	struct ff_condition_effect *condition = &state->effect.u.condition[0];
	int d1;
	int d2;

	d1 = condition->center - condition->deadband / 2;
	d2 = condition->center + condition->deadband / 2;
	if (d1 < parameters->d1) {
		parameters->d1 = d1;
	}
	if (d2 > parameters->d2) {
		parameters->d2 = d2;
	}
	parameters->k1 += condition->left_coeff;
	parameters->k2 += condition->right_coeff;
	parameters->clip = max(parameters->clip, (unsigned)max(condition->left_saturation, condition->right_saturation));
}

static __always_inline void ftecff_calculate_resistance(struct ftecff_effect_state *state, struct ftecff_effect_parameters *parameters)
{
	struct ff_condition_effect *condition = &state->effect.u.condition[0];

	parameters->k1 += condition->left_coeff;
	parameters->k2 += condition->right_coeff;
	parameters->clip = max(parameters->clip, (unsigned)max(condition->left_saturation, condition->right_saturation));
}

static __always_inline int ftecff_timer(struct ftec_drv_data *drv_data)
{
	struct usbhid_device *usbhid = drv_data->hid->driver_data;
	struct ftecff_slot *slot;
	struct ftecff_effect_state *state;
	struct ftecff_effect_parameters parameters[4];
	unsigned long jiffies_now = jiffies;
	unsigned long now = JIFFIES2MS(jiffies_now);
	unsigned long flags;
	unsigned int gain;
	int current_period;
	int count;
	int effect_id;
	int i;


	// if (usbhid->outhead != usbhid->outtail) {
	// 	current_period = timer_msecs;
	// 	timer_msecs *= 2;
	// 	hid_info(drv_data->hid, "Commands stacking up, increasing timer period to %d ms.", timer_msecs);
	// 	return current_period;
	// }

	memset(parameters, 0, sizeof(parameters));

	gain = 0xffff; //(unsigned long)entry->wdata.master_gain * entry->wdata.gain / 0xffff;

	spin_lock_irqsave(&drv_data->timer_lock, flags);

	count = drv_data->effects_used;

	for (effect_id = 0; effect_id < FTECFF_MAX_EFFECTS; effect_id++) {

		if (!count) {
			break;
		}

		state = &drv_data->states[effect_id];

		if (!test_bit(FF_EFFECT_STARTED, &state->flags)) {
			continue;
		}

		count--;

		if (test_bit(FF_EFFECT_ALLSET, &state->flags)) {
			if (state->effect.replay.length && time_after_eq(now, state->stop_at)) {
				STOP_EFFECT(state);
				if (!--state->count) {
					drv_data->effects_used--;
					continue;
				}
				__set_bit(FF_EFFECT_STARTED, &state->flags);
				state->start_at = state->stop_at;
			}
		}

		ftecff_update_state(state, now);

		if (!test_bit(FF_EFFECT_PLAYING, &state->flags)) {
			continue;
		}

		switch (state->effect.type) {
			case FF_CONSTANT:
				parameters[0].level += ftecff_calculate_constant(state);
				break;
			case FF_SPRING:
				ftecff_calculate_spring(state, &parameters[1]);
				break;
			case FF_DAMPER:
				ftecff_calculate_resistance(state, &parameters[2]);
				break;
			case FF_PERIODIC:
				parameters[0].level += ftecff_calculate_periodic(state);
				break;				
		}
	}

	spin_unlock_irqrestore(&drv_data->timer_lock, flags);

	parameters[0].level = (long)parameters[0].level * gain / 0xffff;
	parameters[1].clip = (long)parameters[1].clip * spring_level / 100;
	parameters[2].clip = (long)parameters[2].clip * damper_level / 100;
	parameters[3].clip = (long)parameters[3].clip * friction_level / 100;

	for (i = 1; i < 4; i++) {
		parameters[i].k1 = (long)parameters[i].k1 * gain / 0xffff;
		parameters[i].k2 = (long)parameters[i].k2 * gain / 0xffff;
		parameters[i].clip = (long)parameters[i].clip * gain / 0xffff;
	}

	for (i = 0; i < 4; i++) {
		slot = &drv_data->slots[i];
		ftecff_update_slot(slot, &parameters[i]);
		if (slot->is_updated) {
			ftecff_send_cmd(drv_data, slot->current_cmd);
			slot->is_updated = 0;
		}
	}

	return 0;
}

static enum hrtimer_restart ftecff_timer_hires(struct hrtimer *t)
{
	struct ftec_drv_data *drv_data = container_of(t, struct ftec_drv_data, hrtimer);
	int delay_timer;
	int overruns;

	delay_timer = ftecff_timer(drv_data);

	if (delay_timer) {
		hrtimer_forward_now(&drv_data->hrtimer, ms_to_ktime(delay_timer));
		return HRTIMER_RESTART;
	}

	if (drv_data->effects_used) {
		overruns = hrtimer_forward_now(&drv_data->hrtimer, ms_to_ktime(timer_msecs));
		overruns--;
		if (unlikely(profile && overruns > 0))
			DEBUG("Overruns: %d", overruns);
		return HRTIMER_RESTART;
	} else {
		if (unlikely(profile))
			DEBUG("Stop timer.");
		return HRTIMER_NORESTART;
	}
}

static void ftecff_init_slots(struct ftec_drv_data *drv_data)
{
	struct ftecff_effect_parameters parameters;
	int i;

	memset(&drv_data->states, 0, sizeof(drv_data->states));
	memset(&drv_data->slots, 0, sizeof(drv_data->slots));
	memset(&parameters, 0, sizeof(parameters));

	drv_data->slots[0].effect_type = FF_CONSTANT;
	drv_data->slots[1].effect_type = FF_SPRING;
	drv_data->slots[2].effect_type = FF_DAMPER;
	drv_data->slots[3].effect_type = FF_FRICTION;

	drv_data->slots[0].current_cmd[1] = 0x08;
	drv_data->slots[1].current_cmd[1] = 0x0b;
	drv_data->slots[2].current_cmd[1] = 0x0c;
	drv_data->slots[3].current_cmd[1] = 0x0; // FIXME: don't know this yet

	for (i = 0; i < 4; i++) {
		drv_data->slots[i].id = i;
		ftecff_update_slot(&drv_data->slots[i], &parameters);
		ftecff_send_cmd(drv_data, drv_data->slots[i].current_cmd);
		drv_data->slots[i].is_updated = 0;
	}
}

static void ftecff_stop_effects(struct ftec_drv_data *drv_data)
{
	u8 cmd[7] = {0};

	cmd[0] = 0xf3;
	ftecff_send_cmd(drv_data, cmd);
}

static int ftecff_upload_effect(struct input_dev *dev, struct ff_effect *effect, struct ff_effect *old)
{
	struct hid_device *hdev = input_get_drvdata(dev);
	struct ftec_drv_data *drv_data = hid_get_drvdata(hdev);
	struct ftecff_effect_state *state;
	unsigned long now = JIFFIES2MS(jiffies);
	unsigned long flags;

	if (effect->type == FF_PERIODIC && effect->u.periodic.period == 0) {
		return -EINVAL;
	}

	state = &drv_data->states[effect->id];

	if (test_bit(FF_EFFECT_STARTED, &state->flags) && effect->type != state->effect.type) {
		return -EINVAL;
	}

	spin_lock_irqsave(&drv_data->timer_lock, flags);

	state->effect = *effect;

	if (test_bit(FF_EFFECT_STARTED, &state->flags)) {
		__set_bit(FF_EFFECT_UPDATING, &state->flags);
		state->updated_at = now;
	}

	spin_unlock_irqrestore(&drv_data->timer_lock, flags);

	return 0;
}

static int ftecff_play_effect(struct input_dev *dev, int effect_id, int value)
{
	struct hid_device *hdev = input_get_drvdata(dev);
	struct ftec_drv_data *drv_data = hid_get_drvdata(hdev);
	struct ftecff_effect_state *state;
	unsigned long now = JIFFIES2MS(jiffies);
	unsigned long flags;

	state = &drv_data->states[effect_id];

	spin_lock_irqsave(&drv_data->timer_lock, flags);

	if (value > 0) {
		if (test_bit(FF_EFFECT_STARTED, &state->flags)) {
			STOP_EFFECT(state);
		} else {
			drv_data->effects_used++;
			if (!hrtimer_active(&drv_data->hrtimer)) {
				hrtimer_start(&drv_data->hrtimer, ms_to_ktime(timer_msecs), HRTIMER_MODE_REL);
				if (unlikely(profile))
					DEBUG("Start timer.");
			}
		}
		__set_bit(FF_EFFECT_STARTED, &state->flags);
		state->start_at = now;
		state->count = value;
	} else {
		if (test_bit(FF_EFFECT_STARTED, &state->flags)) {
			STOP_EFFECT(state);
			drv_data->effects_used--;
		}
	}

	spin_unlock_irqrestore(&drv_data->timer_lock, flags);

	return 0;
}

static void ftecff_destroy(struct ff_device *ff)
{
}

int ftecff_init(struct hid_device *hdev) {
	struct ftec_drv_data *drv_data = hid_get_drvdata(hdev);
    struct hid_input *hidinput = list_entry(hdev->inputs.next, struct hid_input, list);
    struct input_dev *inputdev = hidinput->input;
	struct ff_device *ff;
	int ret,j;

    dbg_hid(" ... setting FF bits");
	for (j = 0; ftecff_wheel_effects[j] >= 0; j++)
		set_bit(ftecff_wheel_effects[j], inputdev->ffbit);

	ret = input_ff_create(hidinput->input, FTECFF_MAX_EFFECTS);
	if (ret) {
		hid_err(hdev, "Unable to create ff: %i\n", ret);
		return ret;
	}

	ff = hidinput->input->ff;
	ff->upload = ftecff_upload_effect;
	ff->playback = ftecff_play_effect;
	ff->destroy = ftecff_destroy;	

	/* Set range so that centering spring gets disabled */
	if (init_range > 0 && (init_range > drv_data->max_range || init_range < drv_data->min_range)) {
		hid_warn(hdev, "Invalid init_range %i; using max range of %i instead\n", init_range, drv_data->max_range);
		init_range = -1;
	}
	drv_data->range = init_range > 0 ? init_range : drv_data->max_range;
	ftec_set_range(hdev, drv_data->range);

	/* Create sysfs interface */
#define CREATE_SYSFS_FILE(name) \
	ret = device_create_file(&hdev->dev, &dev_attr_##name); \
	if (ret) \
		hid_warn(hdev, "Unable to create sysfs interface for '%s', errno %d\n", #name, ret); \
	
	CREATE_SYSFS_FILE(display)
	CREATE_SYSFS_FILE(range)
	CREATE_SYSFS_FILE(wheel_id)
	

	if (hdev->product == CSL_ELITE_WHEELBASE_DEVICE_ID || hdev->product == CSL_ELITE_PS4_WHEELBASE_DEVICE_ID) {
		CREATE_SYSFS_FILE(RESET)
		CREATE_SYSFS_FILE(SLOT)
		CREATE_SYSFS_FILE(SEN)
		CREATE_SYSFS_FILE(FF)
		CREATE_SYSFS_FILE(DRI)
		CREATE_SYSFS_FILE(FEI)
		CREATE_SYSFS_FILE(FOR)
		CREATE_SYSFS_FILE(SPR)
		CREATE_SYSFS_FILE(DPR)
		CREATE_SYSFS_FILE(BLI)
		CREATE_SYSFS_FILE(SHO)
	}

#ifdef CONFIG_LEDS_CLASS
	if (ftec_init_led(hdev))
		hid_err(hdev, "LED init failed\n"); /* Let the driver continue without LEDs */
#endif

	drv_data->effects_used = 0;

	ftecff_init_slots(drv_data);
	spin_lock_init(&drv_data->timer_lock);

	hrtimer_init(&drv_data->hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	drv_data->hrtimer.function = ftecff_timer_hires;
	hid_info(hdev, "Hires timer: period = %d ms", timer_msecs);

	return 0;
}


void ftecff_remove(struct hid_device *hdev)
{
	struct ftec_drv_data *drv_data = hid_get_drvdata(hdev);

	hrtimer_cancel(&drv_data->hrtimer);
	ftecff_stop_effects(drv_data);

	device_remove_file(&hdev->dev, &dev_attr_display);
	device_remove_file(&hdev->dev, &dev_attr_range);
	device_remove_file(&hdev->dev, &dev_attr_wheel_id);

	if (hdev->product == CSL_ELITE_WHEELBASE_DEVICE_ID || hdev->product == CSL_ELITE_PS4_WHEELBASE_DEVICE_ID) {
		device_remove_file(&hdev->dev, &dev_attr_RESET);
		device_remove_file(&hdev->dev, &dev_attr_SLOT);
		device_remove_file(&hdev->dev, &dev_attr_SEN);
		device_remove_file(&hdev->dev, &dev_attr_FF);
		device_remove_file(&hdev->dev, &dev_attr_DRI);
		device_remove_file(&hdev->dev, &dev_attr_FEI);
		device_remove_file(&hdev->dev, &dev_attr_FOR);
		device_remove_file(&hdev->dev, &dev_attr_SPR);
		device_remove_file(&hdev->dev, &dev_attr_DPR);
		device_remove_file(&hdev->dev, &dev_attr_BLI);
		device_remove_file(&hdev->dev, &dev_attr_SHO);
	}

#ifdef CONFIG_LEDS_CLASS
	{
		int j;
		struct led_classdev *led;

		/* Deregister LEDs (if any) */
		for (j = 0; j < LEDS; j++) {

			led = drv_data->led[j];
			drv_data->led[j] = NULL;
			if (!led)
				continue;
			led_classdev_unregister(led);
			kfree(led);
		}
	}
#endif
}
