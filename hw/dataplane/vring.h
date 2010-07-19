#ifndef VRING_H
#define VRING_H

#include <linux/virtio_ring.h>
#include "qemu-common.h"

typedef struct {
    void *phys_mem_zero_host_ptr;   /* host pointer to guest RAM */
    struct vring vr;                /* virtqueue vring mapped to host memory */
    __u16 last_avail_idx;           /* last processed avail ring index */
    __u16 last_used_idx;            /* last processed used ring index */
} Vring;

static inline unsigned int vring_get_num(Vring *vring)
{
    return vring->vr.num;
}

/* Map target physical address to host address
 */
static inline void *phys_to_host(Vring *vring, target_phys_addr_t phys)
{
    /* Adjust for 3.6-4 GB PCI memory range */
    if (phys >= 0x100000000) {
        phys -= 0x100000000 - 0xe0000000;
    } else if (phys >= 0xe0000000) {
        fprintf(stderr, "phys_to_host bad physical address in PCI range %#lx\n", phys);
        exit(1);
    }
    return vring->phys_mem_zero_host_ptr + phys;
}

/* Setup for cheap target physical to host address conversion
 *
 * This is a hack for direct access to guest memory, we're not really allowed
 * to do this.
 */
static void setup_phys_to_host(Vring *vring)
{
    target_phys_addr_t len = 4096; /* RAM is really much larger but we cheat */
    vring->phys_mem_zero_host_ptr = cpu_physical_memory_map(0, &len, 0);
    if (!vring->phys_mem_zero_host_ptr) {
        fprintf(stderr, "setup_phys_to_host failed\n");
        exit(1);
    }
}

/* Map the guest's vring to host memory
 *
 * This is not allowed but we know the ring won't move.
 */
static void vring_setup(Vring *vring, VirtIODevice *vdev, int n)
{
    setup_phys_to_host(vring);

    vring_init(&vring->vr, virtio_queue_get_num(vdev, n),
               phys_to_host(vring, virtio_queue_get_ring_addr(vdev, n)), 4096);

    vring->last_avail_idx = 0;
    vring->last_used_idx = 0;

    fprintf(stderr, "vring physical=%#lx desc=%p avail=%p used=%p\n",
            virtio_queue_get_ring_addr(vdev, n),
            vring->vr.desc, vring->vr.avail, vring->vr.used);
}

static bool vring_more_avail(Vring *vring)
{
	return vring->vr.avail->idx != vring->last_avail_idx;
}

/* This is stolen from linux-2.6/drivers/vhost/vhost.c. */
static bool get_indirect(Vring *vring,
			struct iovec iov[], struct iovec *iov_end,
			unsigned int *out_num, unsigned int *in_num,
			struct vring_desc *indirect)
{
	struct vring_desc desc;
	unsigned int i = 0, count, found = 0;

	/* Sanity check */
	if (unlikely(indirect->len % sizeof desc)) {
		fprintf(stderr, "Invalid length in indirect descriptor: "
		       "len 0x%llx not multiple of 0x%zx\n",
		       (unsigned long long)indirect->len,
		       sizeof desc);
		exit(1);
	}

	count = indirect->len / sizeof desc;
	/* Buffers are chained via a 16 bit next field, so
	 * we can have at most 2^16 of these. */
	if (unlikely(count > USHRT_MAX + 1)) {
		fprintf(stderr, "Indirect buffer length too big: %d\n",
		       indirect->len);
        exit(1);
	}

    /* Point to translate indirect desc chain */
    indirect = phys_to_host(vring, indirect->addr);

	/* We will use the result as an address to read from, so most
	 * architectures only need a compiler barrier here. */
	__sync_synchronize(); /* read_barrier_depends(); */

	do {
		if (unlikely(++found > count)) {
			fprintf(stderr, "Loop detected: last one at %u "
			       "indirect size %u\n",
			       i, count);
			exit(1);
		}

        desc = *indirect++;
		if (unlikely(desc.flags & VRING_DESC_F_INDIRECT)) {
			fprintf(stderr, "Nested indirect descriptor\n");
            exit(1);
		}

        /* Stop for now if there are not enough iovecs available. */
        if (iov >= iov_end) {
            return false;
        }

        iov->iov_base = phys_to_host(vring, desc.addr);
        iov->iov_len  = desc.len;
        iov++;

		/* If this is an input descriptor, increment that count. */
		if (desc.flags & VRING_DESC_F_WRITE) {
			*in_num += 1;
		} else {
			/* If it's an output descriptor, they're all supposed
			 * to come before any input descriptors. */
			if (unlikely(*in_num)) {
				fprintf(stderr, "Indirect descriptor "
				       "has out after in: idx %d\n", i);
                exit(1);
			}
			*out_num += 1;
		}
        i = desc.next;
	} while (desc.flags & VRING_DESC_F_NEXT);
    return true;
}

/* This looks in the virtqueue and for the first available buffer, and converts
 * it to an iovec for convenient access.  Since descriptors consist of some
 * number of output then some number of input descriptors, it's actually two
 * iovecs, but we pack them into one and note how many of each there were.
 *
 * This function returns the descriptor number found, or vq->num (which is
 * never a valid descriptor number) if none was found.  A negative code is
 * returned on error.
 *
 * Stolen from linux-2.6/drivers/vhost/vhost.c.
 */
static unsigned int vring_pop(Vring *vring,
		      struct iovec iov[], struct iovec *iov_end,
		      unsigned int *out_num, unsigned int *in_num)
{
	struct vring_desc desc;
	unsigned int i, head, found = 0, num = vring->vr.num;
    __u16 avail_idx, last_avail_idx;

	/* Check it isn't doing very strange things with descriptor numbers. */
	last_avail_idx = vring->last_avail_idx;
    avail_idx = vring->vr.avail->idx;

	if (unlikely((__u16)(avail_idx - last_avail_idx) > num)) {
		fprintf(stderr, "Guest moved used index from %u to %u\n",
		        last_avail_idx, avail_idx);
		exit(1);
	}

	/* If there's nothing new since last we looked, return invalid. */
	if (avail_idx == last_avail_idx)
		return num;

	/* Only get avail ring entries after they have been exposed by guest. */
	__sync_synchronize(); /* smp_rmb() */

	/* Grab the next descriptor number they're advertising, and increment
	 * the index we've seen. */
	head = vring->vr.avail->ring[last_avail_idx % num];

	/* If their number is silly, that's an error. */
	if (unlikely(head >= num)) {
		fprintf(stderr, "Guest says index %u > %u is available\n",
		        head, num);
		exit(1);
	}

	/* When we start there are none of either input nor output. */
	*out_num = *in_num = 0;

	i = head;
	do {
		if (unlikely(i >= num)) {
			fprintf(stderr, "Desc index is %u > %u, head = %u\n",
			        i, num, head);
            exit(1);
		}
		if (unlikely(++found > num)) {
			fprintf(stderr, "Loop detected: last one at %u "
			        "vq size %u head %u\n",
			        i, num, head);
            exit(1);
		}
        desc = vring->vr.desc[i];
		if (desc.flags & VRING_DESC_F_INDIRECT) {
			if (!get_indirect(vring, iov, iov_end, out_num, in_num, &desc)) {
                return num; /* not enough iovecs, stop for now */
            }
            continue;
		}

        /* If there are not enough iovecs left, stop for now.  The caller
         * should check if there are more descs available once they have dealt
         * with the current set.
         */
        if (iov >= iov_end) {
            return num;
        }

        iov->iov_base = phys_to_host(vring, desc.addr);
        iov->iov_len  = desc.len;
        iov++;

		if (desc.flags & VRING_DESC_F_WRITE) {
			/* If this is an input descriptor,
			 * increment that count. */
			*in_num += 1;
		} else {
			/* If it's an output descriptor, they're all supposed
			 * to come before any input descriptors. */
			if (unlikely(*in_num)) {
				fprintf(stderr, "Descriptor has out after in: "
				        "idx %d\n", i);
                exit(1);
			}
			*out_num += 1;
		}
        i = desc.next;
	} while (desc.flags & VRING_DESC_F_NEXT);

	/* On success, increment avail index. */
	vring->last_avail_idx++;
	return head;
}

/* After we've used one of their buffers, we tell them about it.
 *
 * Stolen from linux-2.6/drivers/vhost/vhost.c.
 */
static void vring_push(Vring *vring, unsigned int head, int len)
{
	struct vring_used_elem *used;

	/* The virtqueue contains a ring of used buffers.  Get a pointer to the
	 * next entry in that used ring. */
	used = &vring->vr.used->ring[vring->last_used_idx % vring->vr.num];
    used->id = head;
    used->len = len;

	/* Make sure buffer is written before we update index. */
	__sync_synchronize(); /* smp_wmb() */

    vring->vr.used->idx = ++vring->last_used_idx;
}

#endif /* VRING_H */
