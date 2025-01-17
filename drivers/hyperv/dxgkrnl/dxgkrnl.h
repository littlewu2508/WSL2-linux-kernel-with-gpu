// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2019, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@microsoft.com>
 *
 * Dxgkrnl Graphics Port Driver headers
 *
 */

#ifndef _DXGKRNL_H
#define _DXGKRNL_H

#include <linux/uuid.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <linux/cdev.h>

struct dxgprocess;
struct dxgadapter;
struct dxgdevice;
struct dxgcontext;
struct dxgallocation;
struct dxgresource;
struct dxgsharedresource;
struct dxgsyncobject;
struct dxgsharedsyncobject;
struct dxghwqueue;

#include "misc.h"
#include "hmgr.h"
#include "d3dkmthk.h"

struct dxgk_device_types {
	uint post_device:1;
	uint post_device_certain:1;
	uint software_device:1;
	uint soft_gpu_device:1;
	uint warp_device:1;
	uint bdd_device:1;
	uint support_miracast:1;
	uint mismatched_lda:1;
	uint indirect_display_device:1;
	uint xbox_one_device:1;
	uint child_id_support_dwm_clone:1;
	uint child_id_support_dwm_clone2:1;
	uint has_internal_panel:1;
	uint rfx_vgpu_device:1;
	uint virtual_render_device:1;
	uint support_preserve_boot_display:1;
	uint is_uefi_frame_buffer:1;
	uint removable_device:1;
	uint virtual_monitor_device:1;
};

enum dxgobjectstate {
	DXGOBJECTSTATE_CREATED,
	DXGOBJECTSTATE_ACTIVE,
	DXGOBJECTSTATE_STOPPED,
	DXGOBJECTSTATE_DESTROYED,
};

struct dxgvmbuschannel {
	struct vmbus_channel	*channel;
	struct hv_device	*hdev;
	struct dxgadapter	*adapter;
	spinlock_t		packet_list_mutex;
	struct list_head	packet_list_head;
	struct kmem_cache	*packet_cache;
	atomic64_t		packet_request_id;
};

int dxgvmbuschannel_init(struct dxgvmbuschannel *ch, struct hv_device *hdev);
void dxgvmbuschannel_destroy(struct dxgvmbuschannel *ch);
void dxgvmbuschannel_receive(void *ctx);

struct dxgpagingqueue {
	struct dxgdevice	*device;
	struct dxgprocess	*process;
	struct list_head	pqueue_list_entry;
	d3dkmt_handle		device_handle;
	d3dkmt_handle		handle;
	d3dkmt_handle		syncobj_handle;
	void			*mapped_address;
};

/*
 * The structure describes an event, which will be signaled by
 * a message from host.
 */
struct dxghostevent {
	struct list_head	host_event_list_entry;
	u64			event_id;
	struct dxgprocess	*process;
	struct eventfd_ctx	*cpu_event;
	struct completion	*completion_event;
	bool			destroy_after_signal;
	bool			remove_from_list;
};

struct dxgpagingqueue *dxgpagingqueue_create(struct dxgdevice *device);
void dxgpagingqueue_destroy(struct dxgpagingqueue *pqueue);
void dxgpagingqueue_stop(struct dxgpagingqueue *pqueue);

/*
 * This is GPU synchronization object, which is used to synchronize execution
 * between GPU contextx/hardware queues or for tracking GPU execution progress.
 * A dxgsyncobject is created when somebody creates a syncobject or opens a
 * shared syncobject.
 * A syncobject belongs to an adapter, unless it is a cross-adapter object.
 * Cross adapter syncobjects are currently not implemented.
 *
 * D3DDDI_MONITORED_FENCE and D3DDDI_PERIODIC_MONITORED_FENCE are called
 * "device" syncobject, because the belong to a device (dxgdevice).
 * Device syncobjects are inserted to a list in dxgdevice.
 *
 * A syncobject can be "shared", meaning that it could be opened by many
 * processes.
 *
 * Shared syncobjects are inserted to a list in its owner
 * (dxgsharedsyncobject).
 * A syncobject can be shared by using a global handle or by using
 * "NT security handle".
 * When global handle sharing is used, the handle is created durinig object
 * creation.
 * When "NT security" is used, the handle for sharing is create be calling
 * dxgk_share_objects. On Linux "NT handle" is represented by a file
 * descriptor. FD points to dxgsharedsyncobject.
 */
struct dxgsyncobject {
	refcount_t refcount;
	enum d3dddi_synchronizationobject_type	type;
	/*
	 * List entry in dxgdevice for device sync objects.
	 * List entry in dxgadapter for other objects
	 */
	struct list_head		syncobj_list_entry;
	/* List entry in the dxgsharedsyncobject object for shared synobjects */
	struct list_head		shared_syncobj_list_entry;
	/* Adapter, the syncobject belongs to. NULL for stopped sync obejcts. */
	struct dxgadapter		*adapter;
	/*
	 * Pointer to the device, which was used to create the object.
	 * This is NULL for non-device syncbjects
	 */
	struct dxgdevice		*device;
	struct dxgprocess		*process;
	/* Used by D3DDDI_CPU_NOTIFICATION objects */
	struct dxghostevent		*host_event;
	/* Owner object for shared syncobjects */
	struct dxgsharedsyncobject	*shared_owner;
	/* CPU virtual address of the fence value for "device" syncobjects */
	void				*mapped_address;
	/* Handle in the process handle table */
	d3dkmt_handle			handle;
	/* Cached handle of the device. Used to avoid device dereference. */
	d3dkmt_handle			device_handle;
	union {
		struct {
			/* Must be the first bit */
			uint		destroyed:1;
			/* Must be the second bit */
			uint		stopped:1;
			/* device syncobject */
			uint		monitored_fence:1;
			uint		cpu_event:1;
			uint		shared:1;
			/* shared using file descriptor */
			uint		shared_nt:1;
			uint		reserved:26;
		};
		long			flags;
	};
};

/*
 * The object is used as parent of all sync objects, created for a shared
 * syncobject. When a shared syncobject is created without NT security, the
 * handle in the global handle table will point to this object.
 */
struct dxgsharedsyncobject {
	refcount_t refcount;
	/* Referenced by file descriptors */
	int				host_shared_handle_nt_reference;
	/*
	 * Handle in the global handle table. It is zero for NT
	 * security syncobjects
	 */
	d3dkmt_handle			global_shared_handle;
	/* Corresponding handle in the host global handle table */
	d3dkmt_handle			host_shared_handle;
	/*
	 * When the sync object is shared by NT handle, this is the
	 * corresponding handle in the host
	 */
	d3dkmt_handle			host_shared_handle_nt;
	/* Protects access to host_shared_handle_nt */
	struct dxgmutex			fd_mutex;
	struct rw_semaphore		syncobj_list_lock;
	struct list_head		shared_syncobj_list_head;
	struct list_head		adapter_shared_syncobj_list_entry;
	struct dxgadapter		*adapter;
	enum d3dddi_synchronizationobject_type type;
	uint				monitored_fence:1;
};

struct dxgsharedsyncobject *dxgsharedsyncobj_create(struct dxgadapter *adapter,
						    struct dxgsyncobject
						    *syncobj);
bool dxgsharedsyncobj_acquire_reference(struct dxgsharedsyncobject *syncobj);
void dxgsharedsyncobj_release_reference(struct dxgsharedsyncobject *syncobj);
void dxgsharedsyncobj_add_syncobj(struct dxgsharedsyncobject *sharedsyncobj,
				  struct dxgsyncobject *syncobj);
void dxgsharedsyncobj_remove_syncobj(struct dxgsharedsyncobject *sharedsyncobj,
				     struct dxgsyncobject *syncobj);

struct dxgsyncobject *dxgsyncobject_create(struct dxgprocess *process,
					   struct dxgdevice *device,
					   struct dxgadapter *adapter,
					   enum
					   d3dddi_synchronizationobject_type
					   type,
					   struct
					   d3dddi_synchronizationobject_flags
					   flags);
void dxgsyncobject_destroy(struct dxgprocess *process,
			   struct dxgsyncobject *syncobj);
void dxgsyncobject_stop(struct dxgsyncobject *syncobj);
void dxgsyncobject_acquire_reference(struct dxgsyncobject *syncobj);
void dxgsyncobject_release_reference(struct dxgsyncobject *syncobj);

extern struct device *dxgglobaldev;

struct dxgglobal {
	struct dxgvmbuschannel	channel;
	struct delayed_work	dwork;
	struct hv_device	*hdev;
	u32			num_adapters;
	struct resource		*mem;
	u64			mmiospace_base;
	u64			mmiospace_size;
	dev_t			device_devt;
	struct class		*device_class;
	struct device		*device;
	struct cdev		device_cdev;
	struct dxgmutex		device_mutex;

	/*  list of created  processes */
	struct list_head	plisthead;
	struct dxgmutex		plistmutex;

	/* list of created adapters */
	struct list_head	adapter_list_head;
	struct rw_semaphore	adapter_list_lock;

	/* List of all current threads for lock order tracking. */
	struct mutex		thread_info_mutex;
	struct list_head	thread_info_list_head;

	/* protects acces to the global VM bus channel */
	struct rw_semaphore	channel_lock;

	/* protects the dxgprocess_adapter lists */
	struct dxgmutex		process_adapter_mutex;

	/*  list of events, waiting to be signaled by the host */
	struct list_head	host_event_list_head;
	spinlock_t		host_event_list_mutex;
	atomic64_t		host_event_id;

	/* Handle table for shared objects */
	struct hmgrtable	handle_table;

	bool			cdev_initialized;
	bool			devt_initialized;
	bool			vmbus_registered;
};

extern struct dxgglobal		*dxgglobal;

void dxgglobal_acquire_adapter_list_lock(enum dxglockstate state);
void dxgglobal_release_adapter_list_lock(enum dxglockstate state);
struct vmbus_channel *dxgglobal_get_vmbus(void);
struct dxgvmbuschannel *dxgglobal_get_dxgvmbuschannel(void);
void dxgglobal_acquire_process_adapter_lock(void);
void dxgglobal_release_process_adapter_lock(void);
void dxgglobal_add_host_event(struct dxghostevent *hostevent);
void dxgglobal_remove_host_event(struct dxghostevent *hostevent);
u64 dxgglobal_new_host_event_id(void);
void dxgglobal_signal_host_event(u64 event_id);
struct dxghostevent *dxgglobal_get_host_event(u64 event_id);
int dxgglobal_acquire_channel_lock(void);
void dxgglobal_release_channel_lock(void);

/*
 * Describes adapter information for each process
 */
struct dxgprocess_adapter {
	/* Entry in dxgadapter::adapter_process_list_head */
	struct list_head	adapter_process_list_entry;
	/* Entry in dxgprocess::process_adapter_list_head */
	struct list_head	process_adapter_list_entry;
	/* List of all dxgdevice objects created for the process on adapter */
	struct list_head	device_list_head;
	struct dxgmutex		device_list_mutex;
	struct dxgadapter	*adapter;
	struct dxgprocess	*process;
	int			refcount;
};

struct dxgprocess_adapter *dxgprocess_adapter_create(struct dxgprocess *process,
						     struct dxgadapter
						     *adapter);
void dxgprocess_adapter_release(struct dxgprocess_adapter *adapter);
int dxgprocess_adapter_add_device(struct dxgprocess *process,
				  struct dxgadapter *adapter,
				  struct dxgdevice *device);
void dxgprocess_adapter_remove_device(struct dxgdevice *device);
void dxgprocess_adapter_stop(struct dxgprocess_adapter *adapter_info);
void dxgprocess_adapter_destroy(struct dxgprocess_adapter *adapter_info);

struct dxgprocess {
	/*
	 * Process list entry in dxgglobal.
	 * Protected by the dxgglobal->plistmutex.
	 */
	struct list_head	plistentry;
	struct task_struct	*process;
	pid_t			pid;
	pid_t			tgid;
	/* how many time the process was opened */
	int			refcount;
	/*
	 * This handle table is used for all objects except dxgadapter
	 * The handle table lock order is higher than the local_handle_table
	 * lock
	 */
	struct hmgrtable	handle_table;
	/*
	 * This handle table is used for dxgadapter objects.
	 * The handle table lock order is lowest.
	 */
	struct hmgrtable	local_handle_table;
	d3dkmt_handle		host_handle;

	/* List of opened adapters (dxgprocess_adapter) */
	struct list_head	process_adapter_list_head;

	struct hmgrtable	*test_handle_table[2];
	atomic_t		dxg_memory[DXGMEM_LAST];
	struct dxgmutex		process_mutex;
};

struct dxgprocess *dxgprocess_create(void);
void dxgprocess_destroy(struct dxgprocess *process);
void dxgprocess_release_reference(struct dxgprocess *process);
int dxgprocess_open_adapter(struct dxgprocess *process,
			    struct dxgadapter *adapter, d3dkmt_handle *handle);
int dxgprocess_close_adapter(struct dxgprocess *process, d3dkmt_handle handle);
struct dxgadapter *dxgprocess_get_adapter(struct dxgprocess *process,
					  d3dkmt_handle handle);
struct dxgadapter *dxgprocess_adapter_by_handle(struct dxgprocess *process,
						d3dkmt_handle handle);
struct dxgdevice *dxgprocess_device_by_handle(struct dxgprocess *process,
					      d3dkmt_handle handle);
struct dxgdevice *dxgprocess_device_by_object_handle(struct dxgprocess *process,
						     enum hmgrentry_type t,
						     d3dkmt_handle handle);
void dxgprocess_ht_lock_shared_down(struct dxgprocess *process);
void dxgprocess_ht_lock_shared_up(struct dxgprocess *process);
void dxgprocess_ht_lock_exclusive_down(struct dxgprocess *process);
void dxgprocess_ht_lock_exclusive_up(struct dxgprocess *process);
struct dxgprocess_adapter *dxgprocess_get_adapter_info(struct dxgprocess
						       *process,
						       struct dxgadapter
						       *adapter);

enum dxgadapter_state {
	DXGADAPTER_STATE_ACTIVE		= 0,
	DXGADAPTER_STATE_STOPPED	= 1,
};

/*
 * This object represents the grapchis adapter.
 * Objects, which take reference on the adapter:
 * - dxgglobal
 * - dxgdevice
 * - adapter handle (d3dkmt_handle)
 */
struct dxgadapter {
	struct rw_semaphore	core_lock;
	struct rw_semaphore	adapter_process_list_lock;
	refcount_t		refcount;
	/* Entry in the list of adapters in dxgglobal */
	struct list_head	adapter_list_entry;
	/* The list of dxgprocess_adapter entries */
	struct list_head	adapter_process_list_head;
	/* List of all dxgsharedresource objects */
	struct list_head	shared_resource_list_head;
	/* List of all dxgsharedsyncobject objects */
	struct list_head	adapter_shared_syncobj_list_head;
	/* List of all non-device dxgsyncobject objects */
	struct list_head	syncobj_list_head;
	/* This lock protects shared resource and syncobject lists */
	struct rw_semaphore	shared_resource_list_lock;
	struct winluid		luid;
	struct dxgvmbuschannel	channel;
	d3dkmt_handle		host_handle;
	enum dxgadapter_state	adapter_state;
	struct winluid		host_adapter_luid;
	winwchar		device_description[80];
	winwchar		device_instance_id[W_MAX_PATH];
};

int dxgadapter_init(struct dxgadapter *adapter, struct hv_device *hdev);
bool dxgadapter_is_active(struct dxgadapter *adapter);
void dxgadapter_stop(struct dxgadapter *adapter);
void dxgadapter_destroy(struct dxgadapter *adapter);
bool dxgadapter_acquire_reference(struct dxgadapter *adapter);
void dxgadapter_release_reference(struct dxgadapter *adapter);
int dxgadapter_acquire_lock_shared(struct dxgadapter *adapter);
void dxgadapter_release_lock_shared(struct dxgadapter *adapter);
int dxgadapter_acquire_lock_exclusive(struct dxgadapter *adapter);
void dxgadapter_acquire_lock_forced(struct dxgadapter *adapter);
void dxgadapter_release_lock_exclusive(struct dxgadapter *adapter);
void dxgadapter_add_shared_resource(struct dxgadapter *adapter,
				    struct dxgsharedresource *res);
void dxgadapter_remove_shared_resource(struct dxgadapter *adapter,
				       struct dxgsharedresource *res);
void dxgadapter_add_shared_syncobj(struct dxgadapter *adapter,
				   struct dxgsharedsyncobject *so);
void dxgadapter_remove_shared_syncobj(struct dxgadapter *adapter,
				      struct dxgsharedsyncobject *so);
void dxgadapter_add_syncobj(struct dxgadapter *adapter,
			    struct dxgsyncobject *so);
void dxgadapter_remove_syncobj(struct dxgsyncobject *so);
void dxgadapter_add_process(struct dxgadapter *adapter,
			    struct dxgprocess_adapter *process_info);
void dxgadapter_remove_process(struct dxgprocess_adapter *process_info);

/*
 * The object represent the device object.
 * The following objects take reference on the device
 * - dxgcontext
 * - device handle (d3dkmt_handle)
 */
struct dxgdevice {
	enum dxgobjectstate	object_state;
	/* Device takes reference on the adapter */
	struct dxgadapter	*adapter;
	struct dxgprocess_adapter *adapter_info;
	struct dxgprocess	*process;
	/* Entry in the DGXPROCESS_ADAPTER device list */
	struct list_head	device_list_entry;
	refcount_t		refcount;
	/* Protects destcruction of the device object */
	struct rw_semaphore	device_lock;
	struct rw_semaphore	context_list_lock;
	struct list_head	context_list_head;
	/* List of device allocations */
	struct rw_semaphore	alloc_list_lock;
	struct list_head	alloc_list_head;
	struct list_head	resource_list_head;
	/* List of paging queues. Protected by process handle table lock. */
	struct list_head	pqueue_list_head;
	struct list_head	syncobj_list_head;
	d3dkmt_handle		handle;
	uint			handle_valid;
};

struct dxgdevice *dxgdevice_create(struct dxgadapter *a, struct dxgprocess *p);
void dxgdevice_destroy(struct dxgdevice *device);
void dxgdevice_stop(struct dxgdevice *device);
int dxgdevice_acquire_lock_shared(struct dxgdevice *dev);
void dxgdevice_release_lock_shared(struct dxgdevice *dev);
bool dxgdevice_acquire_reference(struct dxgdevice *dev);
void dxgdevice_release_reference(struct dxgdevice *dev);
void dxgdevice_add_context(struct dxgdevice *dev, struct dxgcontext *ctx);
void dxgdevice_remove_context(struct dxgdevice *dev, struct dxgcontext *ctx);
void dxgdevice_add_alloc(struct dxgdevice *dev, struct dxgallocation *a);
void dxgdevice_remove_alloc(struct dxgdevice *dev, struct dxgallocation *a);
void dxgdevice_remove_alloc_safe(struct dxgdevice *dev,
				 struct dxgallocation *a);
void dxgdevice_add_resource(struct dxgdevice *dev, struct dxgresource *res);
void dxgdevice_remove_resource(struct dxgdevice *dev, struct dxgresource *res);
void dxgdevice_add_paging_queue(struct dxgdevice *dev,
				struct dxgpagingqueue *pqueue);
void dxgdevice_remove_paging_queue(struct dxgpagingqueue *pqueue);
void dxgdevice_add_syncobj(struct dxgdevice *dev, struct dxgsyncobject *so);
void dxgdevice_remove_syncobj(struct dxgsyncobject *so);
bool dxgdevice_is_active(struct dxgdevice *dev);
void dxgdevice_acquire_context_list_lock(struct dxgdevice *dev);
void dxgdevice_release_context_list_lock(struct dxgdevice *dev);
void dxgdevice_acquire_alloc_list_lock(struct dxgdevice *dev);
void dxgdevice_release_alloc_list_lock(struct dxgdevice *dev);
void dxgdevice_acquire_alloc_list_lock_shared(struct dxgdevice *dev);
void dxgdevice_release_alloc_list_lock_shared(struct dxgdevice *dev);

/*
 * The object represent the execution context of a device.
 */
struct dxgcontext {
	enum dxgobjectstate	object_state;
	struct dxgdevice	*device;
	struct dxgprocess	*process;
	/* entry in the device context list */
	struct list_head	context_list_entry;
	struct list_head	hwqueue_list_head;
	struct rw_semaphore	hwqueue_list_lock;
	refcount_t		refcount;
	d3dkmt_handle		handle;
	d3dkmt_handle		device_handle;
};

struct dxgcontext *dxgcontext_create(struct dxgdevice *dev);
void dxgcontext_destroy(struct dxgprocess *pr, struct dxgcontext *ctx);
void dxgcontext_destroy_safe(struct dxgprocess *pr, struct dxgcontext *ctx);
bool dxgcontext_acquire_reference(struct dxgcontext *ctx);
void dxgcontext_release_reference(struct dxgcontext *ctx);
int dxgcontext_add_hwqueue(struct dxgcontext *ctx, struct dxghwqueue *hq);
void dxgcontext_remove_hwqueue(struct dxgcontext *ctx, struct dxghwqueue *hq);
void dxgcontext_remove_hwqueue_safe(struct dxgcontext *ctx,
				    struct dxghwqueue *hq);
bool dxgcontext_is_active(struct dxgcontext *ctx);

/*
 * The object represent the execution hardware queue of a device.
 */
struct dxghwqueue {
	/* entry in the context hw queue list */
	struct list_head	hwqueue_list_entry;
	refcount_t		refcount;
	struct dxgcontext	*context;
	struct dxgprocess	*process;
	d3dkmt_handle		progress_fence_sync_object;
	d3dkmt_handle		handle;
	d3dkmt_handle		device_handle;
	void			*progress_fence_mapped_address;
};

struct dxghwqueue *dxghwqueue_create(struct dxgcontext *ctx);
void dxghwqueue_destroy(struct dxgprocess *pr, struct dxghwqueue *hq);
bool dxghwqueue_acquire_reference(struct dxghwqueue *hq);
void dxghwqueue_release_reference(struct dxghwqueue *hq);

/*
 * A shared resource object is created to track the list of dxgresource objects,
 * which are opened for the same underlying shared resource.
 * There are two types of sharing resource objects:
 * - sharing by using a global handle (nt_security is false).
 *   The global handle is a handle in the handle table of dxgglobal. It points
 *   to a dxgsharedresource object. dxgk_open_resource() creates a dxgresource
 *   object using dxgsharedresource.
 * - shariing by using a file descriptor handle (nt_security is true).
 *   FD is created by calling dxgk_share_objects and providing shandle to
 *   dxgsharedresource. The FD points to a dxgresource object, which is created
 *   by calling dxgk_open_resource_nt.  dxgresource object is referenced by the
 *   FD.
 *
 * The object is referenced by every dxgresource in its list.
 *
 */
struct dxgsharedresource {
	/* Every dxgresource object in the resource list takes a reference */
	refcount_t		refcount;
	struct dxgadapter	*adapter;
	/* List of dxgresource objects, opened for the shared resource. */
	/* Protected by dxgadapter::shared_resource_list_lock */
	struct list_head	resource_list_head;
	/* Entry in the list of dxgsharedresource in dxgadapter */
	/* Protected by dxgadapter::shared_resource_list_lock */
	struct list_head	shared_resource_list_entry;
	struct dxgmutex		fd_mutex;
	/* Referenced by file descriptors */
	int			host_shared_handle_nt_reference;
	/* Handle in the dxgglobal handle table, when nt_security is not used */
	d3dkmt_handle		global_handle;
	/* Corresponding global handle in the host */
	d3dkmt_handle		host_shared_handle;
	/*
	 * When the sync object is shared by NT handle, this is the
	 * corresponding handle in the host
	 */
	d3dkmt_handle		host_shared_handle_nt;
	/* Values below are computed when the resource is sealed */
	uint			runtime_private_data_size;
	uint			alloc_private_data_size;
	uint			resource_private_data_size;
	uint			allocation_count;
	union {
		struct {
			/* Referenced by file descriptor */
			uint	nt_security:1;
			/* Cannot add new allocations */
			uint	sealed:1;
			uint	reserved:30;
		};
		long		flags;
	};
	uint			*alloc_private_data_sizes;
	uint8_t			*alloc_private_data;
	uint8_t			*runtime_private_data;
	uint8_t			*resource_private_data;
};

struct dxgsharedresource *dxgsharedresource_create(struct dxgadapter *adapter);
bool dxgsharedresource_acquire_reference(struct dxgsharedresource *res);
void dxgsharedresource_release_reference(struct dxgsharedresource *res);
void dxgsharedresource_add_resource(struct dxgsharedresource *sres,
				    struct dxgresource *res);
void dxgsharedresource_remove_resource(struct dxgsharedresource *sres,
				       struct dxgresource *res);

struct dxgresource {
	refcount_t		refcount;
	enum dxgobjectstate	object_state;
	d3dkmt_handle		handle;
	struct list_head	alloc_list_head;
	struct list_head	resource_list_entry;
	struct list_head	shared_resource_list_entry;
	struct dxgdevice	*device;
	struct dxgprocess	*process;
	/* Protects adding allocations to resource and resource destruction */
	struct dxgmutex		resource_mutex;
	winhandle		private_runtime_handle;
	union {
		struct {
			uint	destroyed:1;	/* Must be the first */
			uint	handle_valid:1;
			uint	reserved:30;
		};
		long		flags;
	};
	/* Owner of the shared resource */
	struct dxgsharedresource *shared_owner;
};

struct dxgresource *dxgresource_create(struct dxgdevice *dev);
void dxgresource_destroy(struct dxgresource *res);
void dxgresource_free_handle(struct dxgresource *res);
void dxgresource_acquire_reference(struct dxgresource *res);
void dxgresource_release_reference(struct dxgresource *res);
int dxgresource_add_alloc(struct dxgresource *res, struct dxgallocation *a);
void dxgresource_remove_alloc(struct dxgresource *res, struct dxgallocation *a);
void dxgresource_remove_alloc_safe(struct dxgresource *res,
				   struct dxgallocation *a);
bool dxgresource_is_active(struct dxgresource *res);

struct privdata {
	uint data_size;
	uint8_t data[1];
};

struct dxgallocation {
	/* Entry in the device list or resource list (when resource exists) */
	struct list_head		alloc_list_entry;
	/* Allocation owner */
	union {
		struct dxgdevice	*device;
		struct dxgresource	*resource;
	} owner;
	struct dxgprocess		*process;
	/* Pointer to private driver data desc. Used for shared resources */
	struct privdata			*priv_drv_data;
	d3dkmt_handle			alloc_handle;
	/* Set to 1 when allocation belongs to resource. */
	uint				resource_owner:1;
	/* Set to 1 when 'cpu_address' is mapped to the IO space. */
	uint				cpu_address_mapped:1;
	/* Set to 1 when the allocatio is mapped as cached */
	uint				cached:1;
	uint				handle_valid:1;
	/* GPADL address list for existing sysmem allocations */
	uint				gpadl;
	/* Number of pages in the 'pages' array */
	uint				num_pages;
	/*
	 * How many times dxgk_lock2 is called to allocation, which is mapped
	 * to IO space.
	 */
	uint				cpu_address_refcount;
	/*
	 * CPU address from the existing sysmem allocation, or
	 * mapped to the CPU visible backing store in the IO space
	 */
	void				*cpu_address;
	/* Describes pages for the existing sysmem allocation */
	struct page			**pages;
};

struct dxgallocation *dxgallocation_create(struct dxgprocess *process);
void dxgallocation_stop(struct dxgallocation *a);
void dxgallocation_destroy(struct dxgallocation *a);
void dxgallocation_free_handle(struct dxgallocation *a);

void ioctl_desc_init(void);
long dxgk_compat_ioctl(struct file *f, unsigned int p1, unsigned long p2);
long dxgk_unlocked_ioctl(struct file *f, unsigned int p1, unsigned long p2);

int dxg_unmap_iospace(void *va, uint size);
int dxg_copy_from_user(void *to, const void __user *from, unsigned long len);
int dxg_copy_to_user(void *to, const void __user *from, unsigned long len);
static inline void guid_to_luid(guid_t *guid, struct winluid *luid)
{
	*luid = *(struct winluid *)&guid->b[0];
}

/*
 * VM bus interface
 *
 */
int dxgvmb_send_set_iospace_region(u64 start, u64 len, u32 shared_mem_gpadl);
int dxgvmb_send_create_process(struct dxgprocess *process);
int dxgvmb_send_destroy_process(d3dkmt_handle process);
int dxgvmb_send_open_adapter(struct dxgadapter *adapter);
int dxgvmb_send_close_adapter(struct dxgadapter *adapter);
int dxgvmb_send_get_internal_adapter_info(struct dxgadapter *adapter);
d3dkmt_handle dxgvmb_send_create_device(struct dxgadapter *adapter,
					struct dxgprocess *process,
					struct d3dkmt_createdevice *args);
int dxgvmb_send_destroy_device(struct dxgadapter *adapter,
			       struct dxgprocess *process, d3dkmt_handle h);
d3dkmt_handle dxgvmb_send_create_context(struct dxgadapter *adapter,
					 struct dxgprocess *process,
					 struct d3dkmt_createcontextvirtual
					 *args);
int dxgvmb_send_destroy_context(struct dxgadapter *adapter,
				struct dxgprocess *process, d3dkmt_handle h);
int dxgvmb_send_create_paging_queue(struct dxgprocess *pr,
				    struct dxgvmbuschannel *ch,
				    struct dxgdevice *dev,
				    struct d3dkmt_createpagingqueue *args,
				    struct dxgpagingqueue *pq);
int dxgvmb_send_destroy_paging_queue(struct dxgprocess *process,
				     struct dxgvmbuschannel *ch,
				     d3dkmt_handle h);
int dxgvmb_send_create_allocation(struct dxgprocess *pr, struct dxgdevice *dev,
				  struct d3dkmt_createallocation *args,
				  struct d3dkmt_createallocation *__user
				  input_args, struct dxgresource *res,
				  struct dxgallocation **allocs,
				  struct d3dddi_allocationinfo2 *alloc_info,
				  struct d3dkmt_createstandardallocation
				  *standard_alloc);
int dxgvmb_send_destroy_allocation(struct dxgprocess *pr, struct dxgdevice *dev,
				   struct dxgvmbuschannel *ch,
				   struct d3dkmt_destroyallocation2 *args,
				   d3dkmt_handle *alloc_handles);
int dxgvmb_send_make_resident(struct dxgprocess *pr, struct dxgdevice *dev,
			      struct dxgvmbuschannel *ch,
			      struct d3dddi_makeresident *args);
int dxgvmb_send_evict(struct dxgprocess *pr, struct dxgvmbuschannel *ch,
		      struct d3dkmt_evict *args);
int dxgvmb_send_submit_command(struct dxgprocess *pr,
			       struct dxgvmbuschannel *ch,
			       struct d3dkmt_submitcommand *args);
int dxgvmb_send_map_gpu_va(struct dxgprocess *pr, d3dkmt_handle h,
			   struct dxgvmbuschannel *ch,
			   struct d3dddi_mapgpuvirtualaddress *args);
int dxgvmb_send_reserve_gpu_va(struct dxgprocess *pr,
			       struct dxgvmbuschannel *ch,
			       struct d3dddi_reservegpuvirtualaddress *args);
int dxgvmb_send_free_gpu_va(struct dxgprocess *pr, struct dxgvmbuschannel *ch,
			    struct d3dkmt_freegpuvirtualaddress *args);
int dxgvmb_send_update_gpu_va(struct dxgprocess *pr, struct dxgvmbuschannel *ch,
			      struct d3dkmt_updategpuvirtualaddress *args);
int dxgvmb_send_create_sync_object(struct dxgprocess *pr,
				   struct dxgvmbuschannel *ch,
				   struct d3dkmt_createsynchronizationobject2
				   *args, struct dxgsyncobject *so);
int dxgvmb_send_destroy_sync_object(struct dxgprocess *pr, d3dkmt_handle h);
int dxgvmb_send_signal_sync_object(struct dxgprocess *process,
				   struct dxgvmbuschannel *channel,
				   struct d3dddicb_signalflags flags,
				   uint64_t legacy_fence_value,
				   d3dkmt_handle context,
				   uint object_count, d3dkmt_handle *object,
				   uint context_count, d3dkmt_handle *contexts,
				   uint fence_count, uint64_t *fences,
				   struct eventfd_ctx *cpu_event,
				   d3dkmt_handle device);
int dxgvmb_send_wait_sync_object_gpu(struct dxgprocess *process,
				     struct dxgvmbuschannel *channel,
				     d3dkmt_handle context, uint object_count,
				     d3dkmt_handle *objects, uint64_t *fences,
				     bool legacy_fence);
int dxgvmb_send_wait_sync_object_cpu(struct dxgprocess *process,
				     struct dxgvmbuschannel *channel,
				     struct
				     d3dkmt_waitforsynchronizationobjectfromcpu
				     *args, u64 cpu_event);
int dxgvmb_send_lock2(struct dxgprocess *process,
		      struct dxgvmbuschannel *channel,
		      struct d3dkmt_lock2 *args,
		      struct d3dkmt_lock2 *__user outargs);
int dxgvmb_send_unlock2(struct dxgprocess *process,
			struct dxgvmbuschannel *channel,
			struct d3dkmt_unlock2 *args);
int dxgvmb_send_update_alloc_property(struct dxgprocess *process,
				      struct dxgvmbuschannel *channel,
				      struct d3dddi_updateallocproperty *args,
				      struct d3dddi_updateallocproperty *__user
				      inargs);
int dxgvmb_send_mark_device_as_error(struct dxgprocess *process,
				     struct dxgvmbuschannel *channel,
				     struct d3dkmt_markdeviceaserror *args);
int dxgvmb_send_set_allocation_priority(struct dxgprocess *process,
					struct dxgvmbuschannel *channel,
					struct d3dkmt_setallocationpriority
					*args);
int dxgvmb_send_get_allocation_priority(struct dxgprocess *process,
					struct dxgvmbuschannel *channel,
					struct d3dkmt_getallocationpriority
					*args);
int dxgvmb_send_set_context_scheduling_priority(struct dxgprocess *process,
						struct dxgvmbuschannel *channel,
						d3dkmt_handle context,
						int priority, bool in_process);
int dxgvmb_send_get_context_scheduling_priority(struct dxgprocess *process,
						struct dxgvmbuschannel *channel,
						d3dkmt_handle context,
						int *priority, bool in_process);
int dxgvmb_send_offer_allocations(struct dxgprocess *process,
				  struct dxgvmbuschannel *channel,
				  struct d3dkmt_offerallocations *args);
int dxgvmb_send_reclaim_allocations(struct dxgprocess *process,
				    struct dxgvmbuschannel *channel,
				    d3dkmt_handle device,
				    struct d3dkmt_reclaimallocations2 *args,
				    uint64_t * __user paging_fence_value);
int dxgvmb_send_change_vidmem_reservation(struct dxgprocess *process,
					  struct dxgvmbuschannel *channel,
					  d3dkmt_handle other_process,
					  struct
					  d3dkmt_changevideomemoryreservation
					  *args);
int dxgvmb_send_create_hwqueue(struct dxgprocess *process,
			       struct dxgvmbuschannel *channel,
			       struct d3dkmt_createhwqueue *args,
			       struct d3dkmt_createhwqueue *__user inargs,
			       struct dxghwqueue *hq);
int dxgvmb_send_destroy_hwqueue(struct dxgprocess *process,
				struct dxgvmbuschannel *channel,
				d3dkmt_handle handle);
int dxgvmb_send_query_adapter_info(struct dxgprocess *process,
				   struct dxgvmbuschannel *channel,
				   struct d3dkmt_queryadapterinfo *args);
int dxgvmb_send_submit_command_to_hwqueue(struct dxgprocess *process,
					  struct dxgvmbuschannel *channel,
					  struct d3dkmt_submitcommandtohwqueue
					  *args);
int dxgvmb_send_query_clock_calibration(struct dxgprocess *process,
					struct dxgvmbuschannel *channel,
					struct d3dkmt_queryclockcalibration
					*args,
					struct d3dkmt_queryclockcalibration
					*__user inargs);
int dxgvmb_send_flush_heap_transitions(struct dxgprocess *process,
				       struct dxgvmbuschannel *channel,
				       struct d3dkmt_flushheaptransitions
				       *args);
int dxgvmb_send_open_sync_object(struct dxgprocess *process,
				 struct dxgvmbuschannel *channel,
				 d3dkmt_handle h, d3dkmt_handle *ph);
int dxgvmb_send_open_sync_object_nt(struct dxgprocess *process,
				    struct dxgvmbuschannel *channel,
				    struct d3dkmt_opensyncobjectfromnthandle2
				    *args, struct dxgsyncobject *syncobj);
int dxgvmb_send_query_alloc_residency(struct dxgprocess *process,
				      struct dxgvmbuschannel *channel,
				      struct d3dkmt_queryallocationresidency
				      *args);
int dxgvmb_send_escape(struct dxgprocess *process,
		       struct dxgvmbuschannel *channel,
		       struct d3dkmt_escape *args);
int dxgvmb_send_query_vidmem_info(struct dxgprocess *process,
				  struct dxgvmbuschannel *channel,
				  struct d3dkmt_queryvideomemoryinfo *args,
				  struct d3dkmt_queryvideomemoryinfo *__user
				  inargs);
int dxgvmb_send_get_device_state(struct dxgprocess *process,
				 struct dxgvmbuschannel *channel,
				 struct d3dkmt_getdevicestate *args,
				 struct d3dkmt_getdevicestate *__user inargs);
int dxgvmb_send_create_nt_shared_object(struct dxgprocess *process,
					d3dkmt_handle object,
					d3dkmt_handle *shared_handle);
int dxgvmb_send_destroy_nt_shared_object(d3dkmt_handle shared_handle);
int dxgvmb_send_open_resource(struct dxgprocess *process,
			      struct dxgvmbuschannel *channel,
			      d3dkmt_handle device, bool nt_security_sharing,
			      d3dkmt_handle global_share, uint allocation_count,
			      uint total_priv_drv_data_size,
			      d3dkmt_handle *resource_handle,
			      d3dkmt_handle *alloc_handles);
int dxgvmb_send_get_standard_alloc_priv_data(struct dxgdevice *device,
					     enum d3dkmdt_standardallocationtype
					     alloc_type,
					     struct d3dkmdt_gdisurfacedata
					     *alloc_data,
					     uint physical_adapter_index,
					     uint *alloc_priv_driver_size,
					     void *prive_alloc_data,
					     uint *res_priv_data_size,
					     void *priv_res_data);
int dxgvmb_send_query_statistics(struct dxgprocess* process,
				 struct dxgvmbuschannel *channel,
				 struct d3dkmt_querystatistics *args);

#endif
