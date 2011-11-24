/*
 * Virtio 9p
 *
 * Copyright IBM, Corp. 2010
 *
 * Authors:
 *  Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_VIRTIO_9P_DEVICE_H
#define QEMU_VIRTIO_9P_DEVICE_H

typedef struct V9fsConf
{
    /* tag name for the device */
    char *tag;
    char *fsdev_id;
} V9fsConf;

VirtIODevice *virtio_9p_init(DeviceState *dev, V9fsConf *conf);

#define DEFINE_VIRTIO_9P_PROPERTIES(_state, _features_field, _conf_field) \
    DEFINE_VIRTIO_COMMON_FEATURES(VirtIOPCIProxy, _features_field), \
    DEFINE_PROP_STRING("mount_tag", VirtIOPCIProxy, _conf_field.tag), \
    DEFINE_PROP_STRING("fsdev", VirtIOPCIProxy, _conf_field.fsdev_id)

#endif
