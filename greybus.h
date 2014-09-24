/*
 * Greybus driver and device API
 *
 * Copyright 2014 Google Inc.
 *
 * Released under the GPLv2 only.
 */

#ifndef __LINUX_GREYBUS_H
#define __LINUX_GREYBUS_H

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/list.h>
#include <linux/device.h>
#include <linux/module.h>
#include "greybus_id.h"
#include "greybus_manifest.h"


/* Matches up with the Greybus Protocol specification document */
#define GREYBUS_VERSION_MAJOR	0x00
#define GREYBUS_VERSION_MINOR	0x01

#define GREYBUS_DEVICE_ID_MATCH_DEVICE \
		(GREYBUS_DEVICE_ID_MATCH_VENDOR | GREYBUS_DEVICE_ID_MATCH_PRODUCT)

#define GREYBUS_DEVICE(v, p)					\
	.match_flags	= GREYBUS_DEVICE_ID_MATCH_DEVICE,	\
	.vendor		= (v),					\
	.product	= (p),

#define GREYBUS_DEVICE_SERIAL(s)				\
	.match_flags	= GREYBUS_DEVICE_ID_MATCH_SERIAL,	\
	.serial_number	= (s),


/*
  gbuf

  This is the "main" data structure to send / receive Greybus messages

  There are two different "views" of a gbuf structure:
    - a greybus driver
    - a greybus host controller

  A Greybus driver needs to worry about the following:
    - creating a gbuf
    - putting data into a gbuf
    - sending a gbuf to a device
    - receiving a gbuf from a device

  Creating a gbuf:
    A greybus driver calls greybus_alloc_gbuf()
  Putting data into a gbuf:
    copy data into gbuf->transfer_buffer
  Send a gbuf:
    A greybus driver calls greybus_submit_gbuf()
    The completion function in a gbuf will be called if the gbuf is successful
    or not.  That completion function runs in user context.  After the
    completion function is called, the gbuf must not be touched again as the
    greybus core "owns" it.  But, if a greybus driver wants to "hold on" to a
    gbuf after the completion function has been called, a reference must be
    grabbed on the gbuf with a call to greybus_get_gbuf().  When finished with
    the gbuf, call greybus_free_gbuf() and when the last reference count is
    dropped, it will be removed from the system.
  Receive a gbuf:
    A greybus driver calls gb_register_cport_complete() with a pointer to the
    callback function to be called for when a gbuf is received from a specific
    cport and device.  That callback will be made in user context with a gbuf
    when it is received.  To stop receiving messages, call
    gb_deregister_cport_complete() for a specific cport.


  Greybus Host controller drivers need to provide
    - a way to allocate the transfer buffer for a gbuf
    - a way to free the transfer buffer for a gbuf when it is "finished"
    - a way to submit gbuf for transmissions
    - notify the core the gbuf is complete
    - receive gbuf from the wire and submit them to the core
    - a way to send and receive svc messages
  Allocate a transfer buffer
    the host controller function alloc_gbuf_data is called
  Free a transfer buffer
    the host controller function free_gbuf_data is called
  Submit a gbuf to the hardware
    the host controller function submit_gbuf is called
  Notify the gbuf is complete
    the host controller driver must call greybus_gbuf_finished()
  Submit a SVC message to the hardware
    the host controller function send_svc_msg is called
  Receive gbuf messages
    the host controller driver must call greybus_cport_in() with the data
  Reveive SVC messages from the hardware
    The host controller driver must call greybus_svc_in


*/


struct gbuf;

struct gmod_cport {
	u16	id;
	u16	size;
	u8	speed;	// valid???
	// FIXME, what else?
};

struct gmod_string {
	u16	length;
	u8	id;
	u8	string[0];
};

typedef void (*gbuf_complete_t)(struct gbuf *gbuf);

struct gbuf {
	struct kref kref;
	void *hdpriv;

	struct greybus_module *gmod;
	struct gmod_cport *cport;
	int status;
	void *transfer_buffer;
	u32 transfer_flags;		/* flags for the transfer buffer */
	u32 transfer_buffer_length;
	u32 actual_length;

#define GBUF_DIRECTION_OUT	0
#define GBUF_DIRECTION_IN	1
	unsigned int	direction : 1;	/* 0 is out, 1 is in */

	void *context;
	struct work_struct event;
	gbuf_complete_t complete;
};

/*
 * gbuf->transfer_flags
 */
#define GBUF_FREE_BUFFER	BIT(0)	/* Free the transfer buffer with the gbuf */

/* For SP1 hardware, we are going to "hardcode" each device to have all logical
 * blocks in order to be able to address them as one unified "unit".  Then
 * higher up layers will then be able to talk to them as one logical block and
 * properly know how they are hooked together (i.e. which i2c port is on the
 * same module as the gpio pins, etc.)
 *
 * So, put the "private" data structures here in greybus.h and link to them off
 * of the "main" greybus_module structure.
 */

struct gb_i2c_device;
struct gb_gpio_device;
struct gb_sdio_host;
struct gb_tty;
struct gb_usb_device;
struct gb_battery;
struct greybus_host_device;
struct svc_msg;

/* Greybus "Host driver" structure, needed by a host controller driver to be
 * able to handle both SVC control as well as "real" greybus messages
 */
struct greybus_host_driver {
	size_t	hd_priv_size;

	int (*alloc_gbuf_data)(struct gbuf *gbuf, unsigned int size, gfp_t gfp_mask);
	void (*free_gbuf_data)(struct gbuf *gbuf);
	int (*submit_svc)(struct svc_msg *svc_msg,
			    struct greybus_host_device *hd);
	int (*submit_gbuf)(struct gbuf *gbuf, struct greybus_host_device *hd,
			   gfp_t gfp_mask);
};

struct greybus_host_device {
	struct kref kref;
	struct device *parent;
	const struct greybus_host_driver *driver;

	/* Private data for the host driver */
	unsigned long hd_priv[0] __attribute__ ((aligned(sizeof(s64))));
};

struct greybus_host_device *greybus_create_hd(struct greybus_host_driver *host_driver,
					      struct device *parent);
void greybus_remove_hd(struct greybus_host_device *hd);
void greybus_cport_in(struct greybus_host_device *hd, int cport_id, u8 *data,
			   size_t length);
void greybus_gbuf_finished(struct gbuf *gbuf);


/* Increase these values if needed */
#define MAX_CPORTS_PER_MODULE	10
#define MAX_STRINGS_PER_MODULE	10

struct greybus_module {
	struct device dev;
	u16 module_number;
	struct greybus_descriptor_function function;
	struct greybus_descriptor_module_id module_id;
	struct greybus_descriptor_serial_number serial_number;
	int num_cports;
	int num_strings;
	struct gmod_cport *cport[MAX_CPORTS_PER_MODULE];
	struct gmod_string *string[MAX_STRINGS_PER_MODULE];

	struct greybus_host_device *hd;

	struct gb_i2c_device *gb_i2c_dev;
	struct gb_gpio_device *gb_gpio_dev;
	struct gb_sdio_host *gb_sdio_host;
	struct gb_tty *gb_tty;
	struct gb_usb_device *gb_usb_dev;
	struct gb_battery *gb_battery;
};
#define to_greybus_module(d) container_of(d, struct greybus_module, dev)

struct gbuf *greybus_alloc_gbuf(struct greybus_module *gmod,
				struct gmod_cport *cport,
				gbuf_complete_t complete,
				unsigned int size,
				gfp_t gfp_mask,
				void *context);
void greybus_free_gbuf(struct gbuf *gbuf);
struct gbuf *greybus_get_gbuf(struct gbuf *gbuf);
#define greybus_put_gbuf	greybus_free_gbuf

int greybus_submit_gbuf(struct gbuf *gbuf, gfp_t mem_flags);
int greybus_kill_gbuf(struct gbuf *gbuf);


struct greybus_driver {
	const char *name;

	int (*probe)(struct greybus_module *gmod,
		     const struct greybus_module_id *id);
	void (*disconnect)(struct greybus_module *gmod);

	int (*suspend)(struct greybus_module *gmod, pm_message_t message);
	int (*resume)(struct greybus_module *gmod);

	const struct greybus_module_id *id_table;

	struct device_driver driver;
};
#define to_greybus_driver(d) container_of(d, struct greybus_driver, driver)

static inline void greybus_set_drvdata(struct greybus_module *gmod, void *data)
{
	dev_set_drvdata(&gmod->dev, data);
}

static inline void *greybus_get_drvdata(struct greybus_module *gmod)
{
	return dev_get_drvdata(&gmod->dev);
}

/* Don't call these directly, use the module_greybus_driver() macro instead */
int greybus_register_driver(struct greybus_driver *driver,
			    struct module *module, const char *mod_name);
void greybus_deregister(struct greybus_driver *driver);

/* define to get proper THIS_MODULE and KBUILD_MODNAME values */
#define greybus_register(driver) \
	greybus_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)

/**
 * module_greybus_driver() - Helper macro for registering a Greybus driver
 * @__greybus_driver: greybus_driver structure
 *
 * Helper macro for Greybus drivers to set up proper module init / exit
 * functions.  Replaces module_init() and module_exit() and keeps people from
 * printing pointless things to the kernel log when their driver is loaded.
 */
#define module_greybus_driver(__greybus_driver)	\
	module_driver(__greybus_driver, greybus_register, greybus_deregister)

int greybus_disabled(void);

void greybus_remove_device(struct greybus_module *gmod);

const u8 *greybus_string(struct greybus_module *gmod, int id);

/* Internal functions to gb module, move to internal .h file eventually. */

void gb_add_module(struct greybus_host_device *hd, u8 module_id,
		   u8 *data, int size);
void gb_remove_module(struct greybus_host_device *hd, u8 module_id);

int greybus_svc_in(u8 *data, int length, struct greybus_host_device *hd);
int gb_ap_init(void);
void gb_ap_exit(void);
int gb_debugfs_init(void);
void gb_debugfs_cleanup(void);
int gb_gbuf_init(void);
void gb_gbuf_exit(void);

int gb_register_cport_complete(struct greybus_module *gmod,
			       gbuf_complete_t handler, int cport_id,
			       void *context);
void gb_deregister_cport_complete(int cport_id);

extern const struct attribute_group *greybus_module_groups[];

/*
 * Because we are allocating a data structure per "type" in the greybus device,
 * we have static functions for this, not "dynamic" drivers like we really
 * should in the end.
 */
int gb_i2c_probe(struct greybus_module *gmod, const struct greybus_module_id *id);
void gb_i2c_disconnect(struct greybus_module *gmod);
int gb_gpio_probe(struct greybus_module *gmod, const struct greybus_module_id *id);
void gb_gpio_disconnect(struct greybus_module *gmod);
int gb_sdio_probe(struct greybus_module *gmod, const struct greybus_module_id *id);
void gb_sdio_disconnect(struct greybus_module *gmod);
int gb_tty_probe(struct greybus_module *gmod, const struct greybus_module_id *id);
void gb_tty_disconnect(struct greybus_module *gmod);
int gb_battery_probe(struct greybus_module *gmod, const struct greybus_module_id *id);
void gb_battery_disconnect(struct greybus_module *gmod);

int gb_tty_init(void);
void gb_tty_exit(void);



#endif /* __KERNEL__ */
#endif /* __LINUX_GREYBUS_H */
