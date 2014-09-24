/*
 * Greybus gbuf handling
 *
 * Copyright 2014 Google Inc.
 *
 * Released under the GPLv2 only.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "greybus.h"

static void cport_process_event(struct work_struct *work);

static struct kmem_cache *gbuf_head_cache;

/* Workqueue to handle Greybus buffer completions. */
static struct workqueue_struct *gbuf_workqueue;

static struct gbuf *__alloc_gbuf(struct greybus_module *gmod,
				cport_id_t cport_id,
				gbuf_complete_t complete,
				gfp_t gfp_mask,
				void *context)
{
	struct gbuf *gbuf;

	gbuf = kmem_cache_zalloc(gbuf_head_cache, gfp_mask);
	if (!gbuf)
		return NULL;

	kref_init(&gbuf->kref);
	gbuf->gmod = gmod;
	gbuf->cport_id = cport_id;
	INIT_WORK(&gbuf->event, cport_process_event);
	gbuf->complete = complete;
	gbuf->context = context;

	return gbuf;
}

/**
 * greybus_alloc_gbuf - allocate a greybus buffer
 *
 * @gmod: greybus device that wants to allocate this
 * @cport: cport to send the data to
 * @complete: callback when the gbuf is finished with
 * @size: size of the buffer
 * @gfp_mask: allocation mask
 * @context: context added to the gbuf by the driver
 *
 * TODO: someday it will be nice to handle DMA, but for now, due to the
 * architecture we are stuck with, the greybus core has to allocate the buffer
 * that the driver can then fill up with the data to be sent out.  Curse
 * hardware designers for this issue...
 */
struct gbuf *greybus_alloc_gbuf(struct greybus_module *gmod,
				cport_id_t cport_id,
				gbuf_complete_t complete,
				unsigned int size,
				gfp_t gfp_mask,
				void *context)
{
	struct gbuf *gbuf;
	int retval;

	gbuf = __alloc_gbuf(gmod, cport_id, complete, gfp_mask, context);
	if (!gbuf)
		return NULL;

	gbuf->direction = GBUF_DIRECTION_OUT;

	/* Host controller specific allocation for the actual buffer */
	retval = gbuf->gmod->hd->driver->alloc_gbuf_data(gbuf, size, gfp_mask);
	if (retval) {
		greybus_free_gbuf(gbuf);
		return NULL;
	}

	return gbuf;
}
EXPORT_SYMBOL_GPL(greybus_alloc_gbuf);

static DEFINE_MUTEX(gbuf_mutex);

static void free_gbuf(struct kref *kref)
{
	struct gbuf *gbuf = container_of(kref, struct gbuf, kref);

	/* If the direction is "out" then the host controller frees the data */
	if (gbuf->direction == GBUF_DIRECTION_OUT) {
		gbuf->gmod->hd->driver->free_gbuf_data(gbuf);
	} else {
		/* we "own" this in data, so free it ourselves */
		kfree(gbuf->transfer_buffer);
	}

	kmem_cache_free(gbuf_head_cache, gbuf);
}

void greybus_free_gbuf(struct gbuf *gbuf)
{
	/* drop the reference count and get out of here */
	kref_put_mutex(&gbuf->kref, free_gbuf, &gbuf_mutex);
}
EXPORT_SYMBOL_GPL(greybus_free_gbuf);

struct gbuf *greybus_get_gbuf(struct gbuf *gbuf)
{
	mutex_lock(&gbuf_mutex);
	kref_get(&gbuf->kref);
	mutex_unlock(&gbuf_mutex);
	return gbuf;
}
EXPORT_SYMBOL_GPL(greybus_get_gbuf);

int greybus_submit_gbuf(struct gbuf *gbuf, gfp_t gfp_mask)
{
	return gbuf->gmod->hd->driver->submit_gbuf(gbuf, gbuf->gmod->hd, gfp_mask);
}

int greybus_kill_gbuf(struct gbuf *gbuf)
{
	// FIXME - implement
	return -ENOMEM;
}

static void cport_process_event(struct work_struct *work)
{
	struct gbuf *gbuf = container_of(work, struct gbuf, event);

	/* Call the completion handler, then drop our reference */
	gbuf->complete(gbuf);
	greybus_put_gbuf(gbuf);
}

#define MAX_CPORTS	1024
struct gb_cport_handler {
	gbuf_complete_t handler;
	cport_id_t cport_id;
	struct greybus_module *gmod;
	void *context;
};

static struct gb_cport_handler cport_handler[MAX_CPORTS];
// FIXME - use a lock for this list of handlers, but really, for now we don't
// need it, we don't have a dynamic system...

int gb_register_cport_complete(struct greybus_module *gmod,
			       gbuf_complete_t handler, cport_id_t cport_id,
			       void *context)
{
	if (cport_handler[cport_id].handler)
		return -EINVAL;
	cport_handler[cport_id].context = context;
	cport_handler[cport_id].gmod = gmod;
	cport_handler[cport_id].cport_id = cport_id;
	cport_handler[cport_id].handler = handler;
	return 0;
}

void gb_deregister_cport_complete(cport_id_t cport_id)
{
	cport_handler[cport_id].handler = NULL;
}

void greybus_cport_in(struct greybus_host_device *hd, cport_id_t cport_id,
			u8 *data, size_t length)
{
	struct gb_cport_handler *ch;
	struct gbuf *gbuf;

	/* first check to see if we have a cport handler for this cport */
	ch = &cport_handler[cport_id];
	if (!ch->handler) {
		/* Ugh, drop the data on the floor, after logging it... */
		dev_err(hd->parent,
			"Received data for cport %d, but no handler!\n",
			cport_id);
		return;
	}

	gbuf = __alloc_gbuf(ch->gmod, ch->cport_id, ch->handler, GFP_ATOMIC,
			ch->context);
	if (!gbuf) {
		/* Again, something bad went wrong, log it... */
		pr_err("can't allocate gbuf???\n");
		return;
	}
	gbuf->hdpriv = hd;
	gbuf->direction = GBUF_DIRECTION_IN;

	/*
	 * FIXME:
	 * Very dumb copy data method for now, if this is slow (odds are it will
	 * be, we should move to a model where the hd "owns" all buffers, but we
	 * want something up and working first for now.
	 */
	gbuf->transfer_buffer = kmalloc(length, GFP_ATOMIC);
	if (!gbuf->transfer_buffer) {
		kfree(gbuf);
		return;
	}
	memcpy(gbuf->transfer_buffer, data, length);
	gbuf->transfer_buffer_length = length;
	gbuf->actual_length = length;

	queue_work(gbuf_workqueue, &gbuf->event);
}
EXPORT_SYMBOL_GPL(greybus_cport_in);

/* Can be called in interrupt context, do the work and get out of here */
void greybus_gbuf_finished(struct gbuf *gbuf)
{
	queue_work(gbuf_workqueue, &gbuf->event);
}
EXPORT_SYMBOL_GPL(greybus_gbuf_finished);

int gb_gbuf_init(void)
{
	gbuf_workqueue = alloc_workqueue("greybus_gbuf", 0, 1);
	if (!gbuf_workqueue)
		return -ENOMEM;

	gbuf_head_cache = kmem_cache_create("gbuf_head_cache",
					    sizeof(struct gbuf), 0, 0, NULL);
	return 0;
}

void gb_gbuf_exit(void)
{
	destroy_workqueue(gbuf_workqueue);
	kmem_cache_destroy(gbuf_head_cache);
}
