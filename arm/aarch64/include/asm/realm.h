#ifndef __ASM_REALM_H_
#define __ASM_REALM_H_

#include "kvm/kvm.h"

struct kvm;

static inline bool kvm__is_realm(struct kvm *kvm)
{
	return kvm->cfg.arch.is_realm;
}

void kvm_arm_realm_create_realm_descriptor(struct kvm *kvm);
void kvm_arm_realm_populate_initrd(struct kvm *kvm);
void kvm_arm_realm_populate_dtb(struct kvm *kvm);
void kvm_arm_realm_populate_kernel(struct kvm *kvm,
				   unsigned long file_size,
				   unsigned long mem_size);

#endif
