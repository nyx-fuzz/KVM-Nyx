/*
 * Kernel AFL Fast Dirty Page Logging Driver (KVM Extension)
 * (c) Sergej Schumilo 2017 - sergej@schumilo.de
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include "vmx_fdl.h"
#include <linux/types.h>
#include <asm/nmi.h>
#include <asm/page.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/anon_inodes.h>
#include <linux/vmalloc.h>
#include <linux/kvm_host.h>


/*===========================================================================* 
 *                              Fast Reload Mechanism                        * 
 *===========================================================================*/ 

//#define DEBUG_FDL 

#define FAST_IN_RANGE(address, start, end) (address < end && address >= start)
#define PAGE_ALIGNED_SIZE(x) ((x) + (0x1000-((x)%0x1000)))
#define FDL_BITMAP_SIZE(x) ((x/0x1000)/8)
#define FDL_STACK_SIZE(x) ((x/0x1000)*sizeof(uint64_t))

#define FDL_MAX_AREAS 8

struct fdl_area{
	uint64_t base_address;
	uint64_t size;
	uint64_t mmap_bitmap_offset;						/* set by kernel */
	uint64_t mmap_stack_offset;							/* set by kernel */
	uint64_t mmap_bitmap_size;							/* set by kernel */
	uint64_t mmap_stack_size;								/* set by kernel */
};

struct fdl_conf{
	uint8_t num;
	uint64_t mmap_size;											/* set by kernel */
	struct fdl_area areas[FDL_MAX_AREAS];
};

struct fdl_result{
	uint8_t num;
	uint64_t values[FDL_MAX_AREAS];
};

struct vmx_fdl_area{
	uint64_t base;				/* base address of this ram area */
	uint64_t size;				/* size of this ram area */

	uint64_t* stack;			/* pointer to fdl stack */
	uint8_t* bitmap;			/* pointer to bitmap */

	uint64_t stack_max;  	/* max stack slots */
	uint64_t stack_index; /* current stack position */
};

struct vm_vmx_fdl{
	bool configured;
	void* alloc_buf;														/* pointer to the allocated mmapped buffer */
	uint64_t total_alloc_size; 									/* total size of the mmapped buffer (page aligend) */ 

	uint8_t num_areas;													/* number of tracked ram arease */
	struct vmx_fdl_area areas[FDL_MAX_AREAS];		/* our ram areas */
};


static void vmx_fdl_set_addr(struct vm_vmx_fdl* data, u64 gpfn){
	struct vmx_fdl_area* area;

	if(data->alloc_buf){

		uint8_t ram_area = 0xff;

		switch(FDL_MAX_AREAS-data->num_areas){
			case 0:
				ram_area = FAST_IN_RANGE(gpfn, data->areas[7].base, data->areas[7].base+(data->areas[7].size-1ULL)) ? 7 : ram_area;
				// fall through
			case 1:
				ram_area = FAST_IN_RANGE(gpfn, data->areas[6].base, data->areas[6].base+(data->areas[6].size-1ULL)) ? 6 : ram_area;
				// fall through
			case 2: 
				ram_area = FAST_IN_RANGE(gpfn, data->areas[5].base, data->areas[5].base+(data->areas[5].size-1ULL)) ? 5 : ram_area;
				// fall through
			case 3:
				ram_area = FAST_IN_RANGE(gpfn, data->areas[4].base, data->areas[4].base+(data->areas[4].size-1ULL)) ? 4 : ram_area;
				// fall through
			case 4:
				ram_area = FAST_IN_RANGE(gpfn, data->areas[3].base, data->areas[3].base+(data->areas[3].size-1ULL)) ? 3 : ram_area;
				// fall through
			case 5:
				ram_area = FAST_IN_RANGE(gpfn, data->areas[2].base, data->areas[2].base+(data->areas[2].size-1ULL)) ? 2 : ram_area;
				// fall through
			case 6:
				ram_area = FAST_IN_RANGE(gpfn, data->areas[1].base, data->areas[1].base+(data->areas[1].size-1ULL)) ? 1 : ram_area;
				// fall through
			case 7:
				ram_area = FAST_IN_RANGE(gpfn, data->areas[0].base, data->areas[0].base+(data->areas[0].size-1ULL)) ? 0 : ram_area;
				// fall through
			default:
				break;
		}
		if(ram_area == 0xff){
			printk("ERROR: %s %llx [%d]\n", __func__, gpfn, ram_area);
			return;
		}

		area = &data->areas[ram_area];

		if(!test_and_set_bit_le((gpfn-area->base)>>12, area->bitmap)){
			//printk("%s %llx [%d]\n", __func__, gpfn, ram_area);

			if(area->stack_index >= area->stack_max){
				printk("ERROR stack_max reached\n");
				return;
			}

			area->stack[area->stack_index] = gpfn;
			area->stack_index += 1;
		}
	}
}


void vmx_fdl_set_addr_kvm(void* data, u64 gpa){
	//printk("%s %llx\n", __func__, gpa);
	struct vm_vmx_fdl* fdl_data = ((struct kvm*)data)->arch.fdl_opaque;
	struct kvm_memory_slot* slot = gfn_to_memslot(((struct kvm*)data), gpa>>12);
	//printk("%p\n", slot);
	if (slot && slot->dirty_bitmap) {
		//printk("---> %p\n", slot);
		vmx_fdl_set_addr(fdl_data, gpa&0xFFFFFFFFFFFFF000);
	}
}

void vmx_fdl_set_addr_vpcu(void* data, u64 gpa){
	vmx_fdl_set_addr_kvm((void*)(((struct kvm_vcpu*)data)->kvm), gpa);
}

static int vmx_fdl_release(struct inode *inode, struct file *filp)
{
#ifdef DEBUG
	PRINT_INFO("fast_reload_release: file closed ...");
#endif
	return 0;
}

static long vmx_fdl_realloc_memory(struct vm_vmx_fdl* data, unsigned long arg){
	long r = -EINVAL;
	struct fdl_conf configuration;
	void __user *argp;
	uint8_t i; 

	if(data->configured){
		/* configuration is only allowed once */
		return r;
	}

	argp = (void __user *)arg;		
	if (!copy_from_user(&configuration, argp, sizeof(struct fdl_conf))){

		data->num_areas = 0;

		data->total_alloc_size = 0;

		if(configuration.num){
#ifdef DEBUG_FDL
			printk("configuration.num: %d\n", configuration.num);
#endif
			for(i = 0; i < configuration.num; i++){

				data->areas[i].base = configuration.areas[i].base_address;
				data->areas[i].size = configuration.areas[i].size;
				data->areas[i].stack_index = 0;
				data->areas[i].stack_max = (data->areas[i].size/0x1000);

				configuration.areas[i].mmap_bitmap_offset = data->total_alloc_size;
				configuration.areas[i].mmap_bitmap_size = PAGE_ALIGNED_SIZE(FDL_BITMAP_SIZE(data->areas[i].size));
				data->total_alloc_size += configuration.areas[i].mmap_bitmap_size;

				configuration.areas[i].mmap_stack_offset = data->total_alloc_size;
				configuration.areas[i].mmap_stack_size = PAGE_ALIGNED_SIZE(FDL_STACK_SIZE(data->areas[i].size));
				data->total_alloc_size += configuration.areas[i].mmap_stack_size;

				data->num_areas++;
			}

			data->alloc_buf = vmalloc(data->total_alloc_size);
			memset(data->alloc_buf, 0, data->total_alloc_size);

			for(i = 0; i < data->num_areas; i++){
				data->areas[i].bitmap = data->alloc_buf + configuration.areas[i].mmap_bitmap_offset;
				data->areas[i].stack = data->alloc_buf + configuration.areas[i].mmap_stack_offset;
			}
			configuration.mmap_size = data->total_alloc_size;
			data->configured = true;

#ifdef DEBUG_FDL
			printk("Total Alloc Size: 0x%llx\n", data->total_alloc_size);
			for(i = 0; i < data->num_areas; i++){
				printk("area[%d]\n", i);
				printk("\tbase:\t0x%llx\n", data->areas[i].base);
				printk("\tsize:\t0x%llx\n", data->areas[i].size);
				printk("\tstack_index:\t0x%llx\n", data->areas[i].stack_index);
				printk("\tstack_max:\t0x%llx\n", data->areas[i].stack_max);
				printk("\tbitmap:\t0x%p\n", data->areas[i].bitmap);
				printk("\tstack:\t0x%p\n", data->areas[i].stack);
			}
#endif

			if(copy_to_user(argp, &configuration, sizeof(struct fdl_conf))){
				printk("%s: copy_to_user failed\n", __func__);
			}
			else{
				r = 0;
			}
		}
	}
	else{
		printk("%s: copy_from_user failed\n", __func__);
	}
	return r;
}

static long vmx_fdl_get_index(struct vm_vmx_fdl* data, unsigned long arg){
	long r = -EINVAL;
	uint8_t i;
	void __user *argp;
	struct fdl_result result;
	if(data->configured){
		argp = (void __user *)arg;		
		result.num = data->num_areas;
		for(i = 0; i < result.num; i++){
			//printk("[%d] -> stack_index: %lld\n", i, data->areas[i].stack_index);
			result.values[i] = data->areas[i].stack_index;
			data->areas[i].stack_index = 0; /* reset */
		}
		if(copy_to_user(argp, &result, sizeof(struct fdl_result))){
			printk("%s: copy_to_user failed\n", __func__);
		}
		else{
			r = 0;
		}
	}
	return r;
}

static long vmx_fdl_flush(struct vm_vmx_fdl* data){
	long r = -EINVAL;
	uint8_t i;
	if(data->configured){
		for(i = 0; i < data->num_areas; i++){
			data->areas[i].stack_index = 0; /* reset */
		}
		r = 0;
	}
	return r;
}

static long vmx_fdl_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg){
	long r = -EINVAL;
	struct vm_vmx_fdl *data = filp->private_data;

	if(!data){
		return r;
	}

	switch (ioctl) {
		case KVM_VMX_FDL_SET:
			r = vmx_fdl_realloc_memory(data, arg);
			break;

		case KVM_VMX_FDL_FLUSH:
			/* reset the index to zero */
			vmx_fdl_flush(data);
			r = 0;
			break;

		case KVM_VMX_FDL_GET_INDEX:
			r = vmx_fdl_get_index(data, arg);
			break;

		default:
			break;
	}
	return r; 
}

static int vmx_fdl_mmap(struct file *filp, struct vm_area_struct *vma)
{	
	u64 uaddr, vaddr;
	struct vm_vmx_fdl *data = filp->private_data;
	struct page * pageptr;

	if ((vma->vm_end-vma->vm_start) > (data->total_alloc_size)){
		return -EINVAL;
	}
	vma->vm_flags = (VM_READ | VM_SHARED | VM_WRITE); // VM_DENYWRITE); 
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	uaddr = (u64)vma->vm_start;
	vaddr = (u64)data->alloc_buf;
	do {
	    pageptr = vmalloc_to_page((void*)vaddr);
	    vm_insert_page(vma, uaddr, pageptr);
	    //printk("REMAPPING: %llx - %llx\n", vaddr, uaddr);
	    vaddr += PAGE_SIZE;
	    uaddr += PAGE_SIZE;
	} while(uaddr < vma->vm_end || vaddr < data->total_alloc_size);

	return 0;   
}

static struct file_operations vmx_fdl_fops = {
	.release        = vmx_fdl_release,
	.unlocked_ioctl = vmx_fdl_ioctl, 
	.mmap           = vmx_fdl_mmap,
	.llseek         = noop_llseek,
};

int vmx_fdl_create_fd(void* vmx_fdl_opaque){
	if(vmx_fdl_opaque){
		//printk("vmx-fdl\n");
		return anon_inode_getfd("vmx-fdl", &vmx_fdl_fops, vmx_fdl_opaque, O_RDWR | O_CLOEXEC); 
	}
	return 0;
}

void vmx_fdl_setup(void** vmx_fdl_opaque){
	//printk("%s\n", __func__);
	if(!(*vmx_fdl_opaque)){
		*vmx_fdl_opaque = (void*)kmalloc(sizeof(struct vm_vmx_fdl), GFP_KERNEL);
		memset(*vmx_fdl_opaque, 0, sizeof(struct vm_vmx_fdl));
		((struct vm_vmx_fdl*)(*vmx_fdl_opaque))->configured = false;
	}
}

void vmx_fdl_destroy(void* vmx_fdl_opaque){ 
	//printk("%s\n", __func__);
	
	if(vmx_fdl_opaque){
		if(((struct vm_vmx_fdl*)(vmx_fdl_opaque))->alloc_buf){
#ifdef DEBUG_FDL
			printk("vfree alloc_buf\n");
#endif
			vfree(((struct vm_vmx_fdl*)(vmx_fdl_opaque))->alloc_buf);
		}
		kfree(vmx_fdl_opaque);
	}
}

