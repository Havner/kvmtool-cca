#include "kvm/virtio-blk.h"

#include "kvm/virtio-pci.h"

#include "kvm/disk-image.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>
#include <inttypes.h>
#include <assert.h>

#define VIRTIO_BLK_IRQ		14

#define NUM_VIRT_QUEUES		1

#define VIRTIO_BLK_QUEUE_SIZE	128

struct blk_device {
	struct virtio_blk_config	blk_config;
	uint32_t			host_features;
	uint32_t			guest_features;
	uint16_t			config_vector;
	uint8_t				status;

	/* virtio queue */
	uint16_t			queue_selector;

	struct virt_queue		virt_queues[NUM_VIRT_QUEUES];
};

#define DISK_SEG_MAX	126

static struct blk_device blk_device = {
	.blk_config		= (struct virtio_blk_config) {
		/* VIRTIO_BLK_F_SEG_MAX */
		.seg_max		= DISK_SEG_MAX,
	},
	/*
	 * Note we don't set VIRTIO_BLK_F_GEOMETRY here so the
	 * node kernel will compute disk geometry by own, the
	 * same applies to VIRTIO_BLK_F_BLK_SIZE
	 */
	.host_features		= (1UL << VIRTIO_BLK_F_SEG_MAX),
};

static bool virtio_blk_pci_io_device_specific_in(void *data, unsigned long offset, int size, uint32_t count)
{
	uint8_t *config_space = (uint8_t *) &blk_device.blk_config;

	if (size != 1 || count != 1)
		return false;

	ioport__write8(data, config_space[offset - VIRTIO_PCI_CONFIG_NOMSI]);

	return true;
}

static bool virtio_blk_pci_io_in(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset;

	offset		= port - IOPORT_VIRTIO_BLK;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, blk_device.host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		return false;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, blk_device.virt_queues[blk_device.queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_BLK_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		return false;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, blk_device.status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, 0x1);
		kvm__irq_line(self, VIRTIO_BLK_IRQ, 0);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, blk_device.config_vector);
		break;
	default:
		return virtio_blk_pci_io_device_specific_in(data, offset, size, count);
	};

	return true;
}

static bool virtio_blk_request(struct kvm *self, struct virt_queue *queue)
{
	struct virtio_blk_outhdr *req;
	uint16_t desc_block_last;
	struct vring_desc *desc;
	uint16_t desc_status;
	uint16_t desc_block;
	uint32_t block_len;
	uint32_t block_cnt;
	uint16_t desc_hdr;
	uint8_t *status;
	void *block;
	int err;
	int err_cnt;

	/* header */
	desc_hdr		= virt_queue__pop(queue);

	if (desc_hdr >= queue->vring.num) {
		warning("fatal I/O error");
		return false;
	}

	desc			= virt_queue__get_desc(queue, desc_hdr);
	assert(!(desc->flags & VRING_DESC_F_INDIRECT));

	req			= guest_flat_to_host(self, desc->addr);

	/* status */
	desc_status		= desc_hdr;

	do {
		desc_block_last	= desc_status;
		desc_status	= virt_queue__get_desc(queue, desc_status)->next;

		if (desc_status >= queue->vring.num) {
			warning("fatal I/O error");
			return false;
		}

		desc		= virt_queue__get_desc(queue, desc_status);
		assert(!(desc->flags & VRING_DESC_F_INDIRECT));

	} while (desc->flags & VRING_DESC_F_NEXT);

	status			= guest_flat_to_host(self, desc->addr);

	/* block */
	desc_block		= desc_hdr;
	block_cnt		= 0;
	err_cnt			= 0;

	do {
		desc_block	= virt_queue__get_desc(queue, desc_block)->next;

		desc		= virt_queue__get_desc(queue, desc_block);
		assert(!(desc->flags & VRING_DESC_F_INDIRECT));

		block		= guest_flat_to_host(self, desc->addr);
		block_len	= desc->len;

		switch (req->type) {
		case VIRTIO_BLK_T_IN:
			err	= disk_image__read_sector(self->disk_image, req->sector, block, block_len);
			break;
		case VIRTIO_BLK_T_OUT:
			err	= disk_image__write_sector(self->disk_image, req->sector, block, block_len);
			break;
		default:
			warning("request type %d", req->type);
			err	= -1;
		}

		if (err)
			err_cnt++;

		req->sector	+= block_len >> SECTOR_SHIFT;
		block_cnt	+= block_len;

		if (desc_block == desc_block_last)
			break;

		if (desc_block >= queue->vring.num) {
			warning("fatal I/O error");
			return false;
		}

	} while (true);

	*status			= err_cnt ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;

	virt_queue__set_used_elem(queue, desc_hdr, block_cnt);

	return true;
}

static bool virtio_blk_pci_io_out(struct kvm *self, uint16_t port, void *data, int size, uint32_t count)
{
	unsigned long offset;

	offset		= port - IOPORT_VIRTIO_BLK;

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		blk_device.guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		void *p;

		queue			= &blk_device.virt_queues[blk_device.queue_selector];

		queue->pfn		= ioport__read32(data);

		p			= guest_flat_to_host(self, queue->pfn << 12);

		vring_init(&queue->vring, VIRTIO_BLK_QUEUE_SIZE, p, 4096);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		blk_device.queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		struct virt_queue *queue;
		uint16_t queue_index;

		queue_index		= ioport__read16(data);

		queue			= &blk_device.virt_queues[queue_index];

		while (queue->vring.avail->idx != queue->last_avail_idx) {
			if (!virtio_blk_request(self, queue))
				return false;
		}
		kvm__irq_line(self, VIRTIO_BLK_IRQ, 1);

		break;
	}
	case VIRTIO_PCI_STATUS:
		blk_device.status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		blk_device.config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		break;
	default:
		return false;
	};

	return true;
}

static struct ioport_operations virtio_blk_io_ops = {
	.io_in		= virtio_blk_pci_io_in,
	.io_out		= virtio_blk_pci_io_out,
};

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4
#define PCI_DEVICE_ID_VIRTIO_BLK		0x1001
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET	0x1af4
#define PCI_SUBSYSTEM_ID_VIRTIO_BLK		0x0002

static struct pci_device_header virtio_blk_pci_device = {
	.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.device_id		= PCI_DEVICE_ID_VIRTIO_BLK,
	.header_type		= PCI_HEADER_TYPE_NORMAL,
	.revision_id		= 0,
	.class			= 0x010000,
	.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
	.subsys_id		= PCI_SUBSYSTEM_ID_VIRTIO_BLK,
	.bar[0]			= IOPORT_VIRTIO_BLK | PCI_BASE_ADDRESS_SPACE_IO,
	.irq_pin		= 1,
	.irq_line		= VIRTIO_BLK_IRQ,
};

#define PCI_VIRTIO_BLK_DEVNUM 1

void virtio_blk__init(struct kvm *self)
{
	if (!self->disk_image)
		return;

	blk_device.blk_config.capacity = self->disk_image->size / SECTOR_SIZE;

	pci__register(&virtio_blk_pci_device, PCI_VIRTIO_BLK_DEVNUM);

	ioport__register(IOPORT_VIRTIO_BLK, &virtio_blk_io_ops, IOPORT_VIRTIO_BLK_SIZE);
}