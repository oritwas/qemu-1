#ifndef QDEV_H
#define QDEV_H

#include "hw.h"
#include "qemu-queue.h"
#include "qemu-char.h"
#include "qemu-option.h"
#include "qapi/qapi-visit-core.h"
#include "qemu/object.h"
#include "error.h"
#include "qdev-properties.h"

typedef struct CompatProperty CompatProperty;

typedef struct BusState BusState;

typedef struct BusClass BusClass;

enum DevState {
    DEV_STATE_CREATED = 1,
    DEV_STATE_INITIALIZED,
};

enum {
    DEV_NVECTORS_UNSPECIFIED = -1,
};

#define TYPE_DEVICE "device"
#define DEVICE(obj) OBJECT_CHECK(DeviceState, (obj), TYPE_DEVICE)
#define DEVICE_CLASS(klass) OBJECT_CLASS_CHECK(DeviceClass, (klass), TYPE_DEVICE)
#define DEVICE_GET_CLASS(obj) OBJECT_GET_CLASS(DeviceClass, (obj), TYPE_DEVICE)

typedef int (*qdev_initfn)(DeviceState *dev);
typedef int (*qdev_event)(DeviceState *dev);
typedef void (*qdev_resetfn)(DeviceState *dev);

typedef struct DeviceClass {
    ObjectClass parent_class;

    const char *fw_name;
    const char *desc;
    Property *props;
    int no_user;

    /* callbacks */
    void (*reset)(DeviceState *dev);

    /* device state */
    const VMStateDescription *vmsd;

    /* Private to qdev / bus.  */
    qdev_initfn init;
    qdev_event unplug;
    qdev_event exit;
    const char *bus_type;
} DeviceClass;

/* This structure should not be accessed directly.  We declare it here
   so that it can be embedded in individual device state structures.  */
QIDL_DECLARE(DeviceState) {
    Object parent_obj;

    const char q_immutable *id;
    enum DevState q_immutable state;
    QemuOpts q_immutable *opts;
    int q_immutable hotplugged;
    BusState q_immutable *parent_bus;
    int q_immutable num_gpio_out;
    qemu_irq q_immutable *gpio_out;
    int q_immutable num_gpio_in;
    qemu_irq q_immutable *gpio_in;
    QLIST_HEAD(, BusState) q_immutable child_bus;
    int q_immutable num_child_bus;
    int q_immutable instance_id_alias;
    int q_immutable alias_required_for_version;
};

#define TYPE_BUS "bus"
#define BUS(obj) OBJECT_CHECK(BusState, (obj), TYPE_BUS)
#define BUS_CLASS(klass) OBJECT_CLASS_CHECK(BusClass, (klass), TYPE_BUS)
#define BUS_GET_CLASS(obj) OBJECT_GET_CLASS(BusClass, (obj), TYPE_BUS)

struct BusClass {
    ObjectClass parent_class;

    /* FIXME first arg should be BusState */
    void (*print_dev)(Monitor *mon, DeviceState *dev, int indent);
    char *(*get_dev_path)(DeviceState *dev);
    /*
     * This callback is used to create Open Firmware device path in accordance
     * with OF spec http://forthworks.com/standards/of1275.pdf. Individual bus
     * bindings can be found at http://playground.sun.com/1275/bindings/.
     */
    char *(*get_fw_dev_path)(DeviceState *dev);
    int (*reset)(BusState *bus);
};

typedef struct BusChild {
    DeviceState *child;
    int index;
    QTAILQ_ENTRY(BusChild) sibling;
} BusChild;

/**
 * BusState:
 * @qom_allocated: Indicates whether the object was allocated by QOM.
 * @glib_allocated: Indicates whether the object was initialized in-place
 * yet is expected to be freed with g_free().
 */
struct BusState {
    Object obj;
    DeviceState *parent;
    const char *name;
    int allow_hotplug;
    bool qom_allocated;
    bool glib_allocated;
    int max_index;
    QTAILQ_HEAD(ChildrenHead, BusChild) children;
    QLIST_ENTRY(BusState) sibling;
};

/*** Board API.  This should go away once we have a machine config file.  ***/

DeviceState *qdev_create(BusState *bus, const char *name);
DeviceState *qdev_try_create(BusState *bus, const char *name);
int qdev_device_help(QemuOpts *opts);
DeviceState *qdev_device_add(QemuOpts *opts);
int qdev_init(DeviceState *dev) QEMU_WARN_UNUSED_RESULT;
void qdev_init_nofail(DeviceState *dev);
void qdev_set_legacy_instance_id(DeviceState *dev, int alias_id,
                                 int required_for_version);
void qdev_unplug(DeviceState *dev, Error **errp);
void qdev_free(DeviceState *dev);
int qdev_simple_unplug_cb(DeviceState *dev);
void qdev_machine_creation_done(void);
bool qdev_machine_modified(void);

qemu_irq qdev_get_gpio_in(DeviceState *dev, int n);
void qdev_connect_gpio_out(DeviceState *dev, int n, qemu_irq pin);

BusState *qdev_get_child_bus(DeviceState *dev, const char *name);

/*** Device API.  ***/

/* Register device properties.  */
/* GPIO inputs also double as IRQ sinks.  */
void qdev_init_gpio_in(DeviceState *dev, qemu_irq_handler handler, int n);
void qdev_init_gpio_out(DeviceState *dev, qemu_irq *pins, int n);

BusState *qdev_get_parent_bus(DeviceState *dev);

/*** BUS API. ***/

DeviceState *qdev_find_recursive(BusState *bus, const char *id);

/* Returns 0 to walk children, > 0 to skip walk, < 0 to terminate walk. */
typedef int (qbus_walkerfn)(BusState *bus, void *opaque);
typedef int (qdev_walkerfn)(DeviceState *dev, void *opaque);

void qbus_create_inplace(BusState *bus, const char *typename,
                         DeviceState *parent, const char *name);
BusState *qbus_create(const char *typename, DeviceState *parent, const char *name);
/* Returns > 0 if either devfn or busfn skip walk somewhere in cursion,
 *         < 0 if either devfn or busfn terminate walk somewhere in cursion,
 *           0 otherwise. */
int qbus_walk_children(BusState *bus, qdev_walkerfn *devfn,
                       qbus_walkerfn *busfn, void *opaque);
int qdev_walk_children(DeviceState *dev, qdev_walkerfn *devfn,
                       qbus_walkerfn *busfn, void *opaque);
void qdev_reset_all(DeviceState *dev);
void qbus_reset_all_fn(void *opaque);

void qbus_free(BusState *bus);

#define FROM_QBUS(type, dev) DO_UPCAST(type, qbus, dev)

/* This should go away once we get rid of the NULL bus hack */
BusState *sysbus_get_default(void);

/*** monitor commands ***/

void do_info_qtree(Monitor *mon);
void do_info_qdm(Monitor *mon);
int do_device_add(Monitor *mon, const QDict *qdict, QObject **ret_data);
int do_device_del(Monitor *mon, const QDict *qdict, QObject **ret_data);

/* Set properties between creation and init.  */
void *qdev_get_prop_ptr(DeviceState *dev, Property *prop);
int qdev_prop_parse(DeviceState *dev, const char *name, const char *value);
void qdev_prop_set_bit(DeviceState *dev, const char *name, bool value);
void qdev_prop_set_uint8(DeviceState *dev, const char *name, uint8_t value);
void qdev_prop_set_uint16(DeviceState *dev, const char *name, uint16_t value);
void qdev_prop_set_uint32(DeviceState *dev, const char *name, uint32_t value);
void qdev_prop_set_int32(DeviceState *dev, const char *name, int32_t value);
void qdev_prop_set_uint64(DeviceState *dev, const char *name, uint64_t value);
void qdev_prop_set_string(DeviceState *dev, const char *name, const char *value);
void qdev_prop_set_chr(DeviceState *dev, const char *name, CharDriverState *value);
void qdev_prop_set_netdev(DeviceState *dev, const char *name, NetClientState *value);
int qdev_prop_set_drive(DeviceState *dev, const char *name, BlockDriverState *value) QEMU_WARN_UNUSED_RESULT;
void qdev_prop_set_drive_nofail(DeviceState *dev, const char *name, BlockDriverState *value);
void qdev_prop_set_macaddr(DeviceState *dev, const char *name, uint8_t *value);
void qdev_prop_set_enum(DeviceState *dev, const char *name, int value);
/* FIXME: Remove opaque pointer properties.  */
void qdev_prop_set_ptr(DeviceState *dev, const char *name, void *value);

void qdev_prop_register_global_list(GlobalProperty *props);
void qdev_prop_set_globals(DeviceState *dev);
void error_set_from_qdev_prop_error(Error **errp, int ret, DeviceState *dev,
                                    Property *prop, const char *value);

char *qdev_get_fw_dev_path(DeviceState *dev);

/**
 * @qdev_property_add_static - add a @Property to a device referencing a
 * field in a struct.
 */
void qdev_property_add_static(DeviceState *dev, Property *prop, Error **errp);

/**
 * @qdev_machine_init
 *
 * Initialize platform devices before machine init.  This is a hack until full
 * support for composition is added.
 */
void qdev_machine_init(void);

/**
 * @device_reset
 *
 * Reset a single device (by calling the reset method).
 */
void device_reset(DeviceState *dev);

const VMStateDescription *qdev_get_vmsd(DeviceState *dev);

const char *qdev_fw_name(DeviceState *dev);

Object *qdev_get_machine(void);

/* FIXME: make this a link<> */
void qdev_set_parent_bus(DeviceState *dev, BusState *bus);

extern int qdev_hotplug;

char *qdev_get_dev_path(DeviceState *dev);

#endif
