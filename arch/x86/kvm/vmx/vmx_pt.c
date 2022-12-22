/*
 * Kernel AFL Intel PT / VMX Driver (KVM Extension)
 * vmx_pt enables tracing for kvm vcpus.
 * (c) Sergej Schumilo 2016 - sergej@schumilo.de
 *
 * Partially inspired by Andi Kleen's simple-pt kernel driver.
 * (https://github.com/andikleen/simple-pt/)
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
 * Intel PT is specified in the Intel Architecture Instruction Set Extensions
 * Programming Reference:
 * http://software.intel.com/en-us/intel-isa-extensions
 */

#include <linux/types.h>
#include <asm/nmi.h>
#include <asm/page.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include "vmx.h"
#include <linux/kvm.h>
#include <linux/anon_inodes.h>
#include <linux/spinlock.h>

#define PRINT_FEATURE(msr_val, bit_val, str) \
	if(msr_val & bit_val){ PRINT_INFO("\t [*] " str ":"); } \
	else { PRINT_INFO("\t [ ] " str ":"); }

#define HEXDUMP_RANGE 128

#define PT_TRACE_END					0b01010101

#define MSR_IA32_PERF_GLOBAL_STATUS		0x0000038e
#define TRACE_TOPA_PMI					0x80000000000000

#define TOPA_MASK_OR_TABLE_OFFSET		0x00000000FFFFFF80
#define TOPA_OUTPUT_OFFSET				0xFFFFFFFF00000000

#define MSR_IA32_RTIT_OUTPUT_BASE		0x00000560
#define MSR_IA32_RTIT_OUTPUT_MASK_PTRS	0x00000561
#define MSR_IA32_RTIT_CTL				0x00000570
#define MSR_IA32_RTIT_STATUS			0x00000571
#define MSR_IA32_CR3_MATCH				0x00000572 
#define MSR_IA32_ADDR0_START			0x00000580 
#define MSR_IA32_ADDR0_END				0x00000581 
#define MSR_IA32_ADDR1_START			0x00000582
#define MSR_IA32_ADDR1_END				0x00000583 
#define MSR_IA32_ADDR2_START			0x00000584 
#define MSR_IA32_ADDR2_END				0x00000585
#define MSR_IA32_ADDR3_START			0x00000586 
#define MSR_IA32_ADDR3_END				0x00000587

#define TRACE_EN						BIT_ULL(0)
#define CYC_EN							BIT_ULL(1)
#define CTL_OS							BIT_ULL(2)
#define CTL_USER						BIT_ULL(3)
#define PT_ERROR						BIT_ULL(4)
#define CR3_FILTER						BIT_ULL(7)
#define TO_PA							BIT_ULL(8)
#define MTC_EN							BIT_ULL(9)
#define TSC_EN							BIT_ULL(10)
#define DIS_RETC						BIT_ULL(11)
#define BRANCH_EN						BIT_ULL(13)

#define ADDR0_EN						BIT_ULL(32)
#define ADDR1_EN						BIT_ULL(36)
#define ADDR2_EN						BIT_ULL(40)
#define ADDR3_EN						BIT_ULL(44)

#define MTC_MASK						(0xf << 14)
#define CYC_MASK						(0xf << 19)
#define PSB_MASK						(0x0 << 24)
#define ADDR0_SHIFT						32
#define ADDR1_SHIFT						32
#define ADDR0_MASK						(0xfULL << ADDR0_SHIFT)
#define ADDR1_MASK						(0xfULL << ADDR1_SHIFT)
#define TOPA_STOP						BIT_ULL(4)
#define TOPA_INT						BIT_ULL(2)
#define TOPA_END						BIT_ULL(0)
#define TOPA_SIZE_SHIFT 				6

#define NMI_HANDLER 					"pt_topa_pmi_handler_kafl"
#define PRINT_INFO(msg)					printk(KERN_INFO "[KVM-NYX] Info:\t%s\n",  (msg))
#define PRINT_ERROR(msg)				printk(KERN_INFO "[KVM-NYX] Error:\t%s\n", (msg))


#define TOPA_MAIN_ORDER					10	/*  1MB */
#define TOPA_FALLBACK_ORDER				4	/* 64KB */

#define TOPA_MAIN_SIZE					((1 << TOPA_MAIN_ORDER)*(1 << PAGE_SHIFT))
#define TOPA_FALLBACK_SIZE				((1 << TOPA_FALLBACK_ORDER)*(1 << PAGE_SHIFT))

//#define DEBUG


#define IF_VMX_PT_NOT_CONFIGURED(value, X) if(!value){X}

#define HYPERCALL_HOOK_DISABLED_CR3		0x0000000000000000
#define HYPERCALL_HOOK_DISABLED_RIP 	0xffffffffffffffff


struct vcpu_vmx_pt {
	/* hacky vcpu reverse reference */
	struct vcpu_vmx *vmx;
	
	/* configuration */
	u64 ia32_rtit_ctrl_msr;

	/* IP-Filtering */
	bool ia32_rtit_addr_configured[4][2];
	u64 ia32_rtit_addr_0[2];
	u64 ia32_rtit_addr_1[2];
	u64 ia32_rtit_addr_2[2];
	u64 ia32_rtit_addr_3[2];
	
	/* CR3-Filtering */
	u64 ia32_rtit_cr3_match;

	/* Multi CR3-Filtering */
	u64 ia32_rtit_multi_cr3_0_match;
	u64 ia32_rtit_multi_cr3_1_match;
	u64 ia32_rtit_multi_cr3_2_match;
	u64 ia32_rtit_multi_cr3_3_match;
	u8 ia32_rtit_multi_cr3_num;
	bool multi_cr3_enabled; 

	/* ToPA */
	u64 topa_pt_region;
	u64 ia32_rtit_output_base;
	u64 ia32_rtit_output_mask_ptrs;

	u64 ia32_rtit_output_base_init;
	u64 ia32_rtit_output_mask_ptrs_init;


	void* topa_main_buf_virt_addr;
	void* topa_fallback_buf_virt_addr;
	void* topa_virt_addr;
		
	bool configured;
	uint8_t cpu;
	bool reset;


	bool state_change_pending;
	bool state; 

	spinlock_t spinlock;
}; 


void vmx_pt_setup_vmx_autoload_msr(struct vcpu_vmx_pt *vmx_pt_config, bool enable_vmx_pt);

u8 enabled = 0; 
u8 supported_addrn = 0;

#ifdef DEBUG
static inline void vmx_pt_dump_trace_data(struct vcpu_vmx_pt *vmx_pt);
#endif
void vmx_pt_enable(struct vcpu_vmx_pt *vmx_pt_config);
void vmx_pt_disable(struct vcpu_vmx_pt *vmx_pt_config);


/*===========================================================================* 
 *                           vmx/pt userspace interface                      * 
 *===========================================================================*/ 

static int vmx_pt_release(struct inode *inode, struct file *filp);
static long vmx_pt_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg);
static int vmx_pt_mmap(struct file *filp, struct vm_area_struct *vma);

static struct file_operations vmx_pt_fops = {
	.release        = vmx_pt_release,
	.unlocked_ioctl = vmx_pt_ioctl, 
	.mmap           = vmx_pt_mmap,
	.llseek         = noop_llseek,
};

/*
static void prepare_topa(struct vcpu_vmx_pt *vmx_pt_config){
	vmx_pt_config->ia32_rtit_output_mask_ptrs = 0x7fLL;
}
*/

inline static int vmx_pt_get_data_size(struct vcpu_vmx_pt *vmx_pt){
	/* get size of traced data */
	u64 topa_base = READ_ONCE(vmx_pt->ia32_rtit_output_mask_ptrs);
	//rdmsrl(MSR_IA32_RTIT_OUTPUT_MASK_PTRS, topa_base);

	if ((topa_base & TOPA_MASK_OR_TABLE_OFFSET)){ 

		if((topa_base>>32) < TOPA_FALLBACK_SIZE){
			((uint8_t*)vmx_pt->topa_fallback_buf_virt_addr)[(topa_base>>32)] = PT_TRACE_END;
		}
		return (TOPA_MAIN_SIZE + (topa_base>>32));
	}
	((uint8_t*)vmx_pt->topa_main_buf_virt_addr)[(topa_base>>32)] = PT_TRACE_END;
	return (topa_base>>32);
}

static void topa_reset(struct vcpu_vmx_pt *vmx_pt_config){
	//printk("RELOAD! %lx (%lx)\n", vmx_pt_get_data_size(vmx_pt_config), TOPA_MAIN_SIZE+TOPA_FALLBACK_SIZE);
	vmx_pt_config->ia32_rtit_output_base = vmx_pt_config->ia32_rtit_output_base_init;
	vmx_pt_config->ia32_rtit_output_mask_ptrs = vmx_pt_config->ia32_rtit_output_mask_ptrs_init;
	//printk("New! %lx (%lx)\n", vmx_pt_get_data_size(vmx_pt_config), TOPA_MAIN_SIZE+TOPA_FALLBACK_SIZE);

	//printk("MSR_IA32_RTIT_OUTPUT_BASE:\t%lx\n", vmx_pt_config->ia32_rtit_output_base);
	//printk("MSR_IA32_RTIT_OUTPUT_MASK_PTRS:\t%lx\n", vmx_pt_config->ia32_rtit_output_mask_ptrs);
}


static int vmx_pt_check_overflow(struct vcpu_vmx_pt *vmx_pt){ 
	/* check for upcoming ToPA entry overflow and / or raised PMI */
	int bytes = vmx_pt_get_data_size(vmx_pt);

	if(bytes >= TOPA_MAIN_SIZE){
		topa_reset(vmx_pt);
		return bytes;
	}
	return 0;
		//wrmsrl(MSR_IA32_RTIT_OUTPUT_MASK_PTRS, vmx_pt->ia32_rtit_output_mask_ptrs);
	
	//}
	//return 0;
}

static int vmx_pt_mmap(struct file *filp, struct vm_area_struct *vma)
{	
	struct vcpu_vmx_pt *vmx_pt_config = filp->private_data;
	
	if (!vmx_pt_config->topa_main_buf_virt_addr) {
		return -ENOMEM;
	}

	/* refrence http://www.makelinux.net/books/lkd2/ch14lev1sec2 */                                     	
	if ((vma->vm_end-vma->vm_start) > (TOPA_MAIN_SIZE+TOPA_FALLBACK_SIZE)){
		return -EINVAL;
	}
	vma->vm_flags = (VM_READ | VM_SHARED | VM_DENYWRITE); 
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if ((vma->vm_end-vma->vm_start) > (TOPA_MAIN_SIZE)){
		if(remap_pfn_range(vma, vma->vm_start, (__pa((void *)vmx_pt_config->topa_main_buf_virt_addr) >> PAGE_SHIFT), TOPA_MAIN_SIZE, vma->vm_page_prot)){
			return -EAGAIN;                                               
		}
		if(remap_pfn_range(vma, vma->vm_start+TOPA_MAIN_SIZE, (__pa((void *)vmx_pt_config->topa_fallback_buf_virt_addr) >> PAGE_SHIFT), (vma->vm_end-vma->vm_start)-TOPA_MAIN_SIZE, vma->vm_page_prot)){
			return -EAGAIN;                                               
		}
	} 
	else {
		if(remap_pfn_range(vma, vma->vm_start, (__pa((void *)vmx_pt_config->topa_main_buf_virt_addr) >> PAGE_SHIFT), (vma->vm_end-vma->vm_start), vma->vm_page_prot)){
			return -EAGAIN;                                               
		}
	}
	
	return 0;   
}


static int vmx_pt_release(struct inode *inode, struct file *filp)
{
	/* do not free any resources until this vpcu is destroyed */
#ifdef DEBUG
	PRINT_INFO("file closed ...");
#endif
	return 0;
}

#define X64_48_BIT_HOLE_START			0x0000800000000000ULL
#define X64_48_BIT_HOLE_END				0xffff800000000000ULL

static bool range_check(uint64_t addr_a, uint64_t addr_b){
	if(addr_a == addr_b || addr_a > addr_b){
		return false;
	}

	if(addr_a >= X64_48_BIT_HOLE_START && addr_a < X64_48_BIT_HOLE_END){
		return false;
	}

	if(addr_b >= X64_48_BIT_HOLE_START && addr_b < X64_48_BIT_HOLE_END){
		return false;
	}

	return true;
}

static long vmx_pt_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{

	void __user *argp;
	struct vmx_pt_filter_iprs filter_iprs;
	struct vmx_pt_multi_cr3 multi_cr3_addr;
	bool is_configured;
	long r;

	struct vcpu_vmx_pt *vmx_pt_config = filp->private_data;

	if(!vmx_pt_config){
		return -EINVAL;
	}
	
	is_configured = vmx_pt_config->configured;
	r = -EINVAL;
	
	/*
	if (vmx_pt_config->reset){
		vmx_pt_config->reset = false;
		prepare_topa(vmx_pt_config);
	}
	*/
	spin_lock(&vmx_pt_config->spinlock);
	
	switch (ioctl) {
		case KVM_VMX_PT_CONFIGURE_ADDR0:
			if(!is_configured && supported_addrn >= 1) {
				argp = (void __user *)arg;		
				if (!copy_from_user(&filter_iprs, argp, sizeof(filter_iprs))){
#ifdef DEBUG
					printk("Intel PT ADDR0 configured... (%llx / %llx)\n", filter_iprs.a, filter_iprs.b);
#endif
					if(range_check(filter_iprs.a, filter_iprs.b)){
						vmx_pt_config->ia32_rtit_addr_0[0] = filter_iprs.a;
						vmx_pt_config->ia32_rtit_addr_0[1] = filter_iprs.b;
						vmx_pt_config->ia32_rtit_addr_configured[0][0] = true;
						vmx_pt_config->ia32_rtit_addr_configured[0][1] = true;
						r = 0;
#ifdef DEBUG
						printk("Intel PT ADDR0 configured... (%llx / %llx)\n", filter_iprs.a, filter_iprs.b);						
#endif
					}
					else{
						printk("Intel PT ADDR0 configuration failed... (%llx / %llx)\n", filter_iprs.a, filter_iprs.b);
					}
				}
			}
			break;
		case KVM_VMX_PT_CONFIGURE_ADDR1:
			if(!is_configured && supported_addrn >= 2) {
				argp = (void __user *)arg;		
				if (!copy_from_user(&filter_iprs, argp, sizeof(filter_iprs))){
					if(range_check(filter_iprs.a, filter_iprs.b)){
						vmx_pt_config->ia32_rtit_addr_1[0] = filter_iprs.a;
						vmx_pt_config->ia32_rtit_addr_1[1] = filter_iprs.b;
						vmx_pt_config->ia32_rtit_addr_configured[1][0] = true;
						vmx_pt_config->ia32_rtit_addr_configured[1][1] = true;
						r = 0;
					}
					else{
						printk("Intel PT ADDR1 configuration failed... (%llx / %llx)\n", filter_iprs.a, filter_iprs.b);						
					}	
				}
			}
			break;
		case KVM_VMX_PT_CONFIGURE_ADDR2:
			if(!is_configured && supported_addrn >= 3) {
				argp = (void __user *)arg;		
				if (!copy_from_user(&filter_iprs, argp, sizeof(filter_iprs))){
					if(range_check(filter_iprs.a, filter_iprs.b)){
						vmx_pt_config->ia32_rtit_addr_2[0] = filter_iprs.a;
						vmx_pt_config->ia32_rtit_addr_2[1] = filter_iprs.b;
						vmx_pt_config->ia32_rtit_addr_configured[2][0] = true;
						vmx_pt_config->ia32_rtit_addr_configured[2][1] = true;
						r = 0;
					}
					else{
						printk("Intel PT ADDR2 configuration failed... (%llx / %llx)\n", filter_iprs.a, filter_iprs.b);						
					}
				}
			}
			break;
		case KVM_VMX_PT_CONFIGURE_ADDR3:
			if(!is_configured && supported_addrn >= 4) {
				argp = (void __user *)arg;		
				if (!copy_from_user(&filter_iprs, argp, sizeof(filter_iprs))){
					if(range_check(filter_iprs.a, filter_iprs.b)){
						vmx_pt_config->ia32_rtit_addr_3[0] = filter_iprs.a;
						vmx_pt_config->ia32_rtit_addr_3[1] = filter_iprs.b;
						vmx_pt_config->ia32_rtit_addr_configured[3][0] = true;
						vmx_pt_config->ia32_rtit_addr_configured[3][1] = true;
						r = 0;
					}
					else{
						printk("Intel PT ADDR3 configuration failed... (%llx / %llx)\n", filter_iprs.a, filter_iprs.b);						
					}
				}
			}
			break;	
		case KVM_VMX_PT_ENABLE_ADDR0:
			if((!is_configured) && vmx_pt_config->ia32_rtit_addr_configured[0][0] && vmx_pt_config->ia32_rtit_addr_configured[0][1] && supported_addrn >= 1){
#ifdef DEBUG
				printk("Intel PT ADDR0 enabled...\n");
#endif
				vmx_pt_config->ia32_rtit_ctrl_msr |= ADDR0_EN;
				r = 0;
			}
			break;
		case KVM_VMX_PT_ENABLE_ADDR1:
			if((!is_configured) && vmx_pt_config->ia32_rtit_addr_configured[1][0] && vmx_pt_config->ia32_rtit_addr_configured[1][1] && supported_addrn >= 2){
				vmx_pt_config->ia32_rtit_ctrl_msr |= ADDR1_EN;
				r = 0;
			}
			break;
		case KVM_VMX_PT_ENABLE_ADDR2:
			if((!is_configured) && vmx_pt_config->ia32_rtit_addr_configured[2][0] && vmx_pt_config->ia32_rtit_addr_configured[2][1] && supported_addrn >= 3){
				vmx_pt_config->ia32_rtit_ctrl_msr |= ADDR2_EN;
				r = 0;
			}
			break;
		case KVM_VMX_PT_ENABLE_ADDR3:
			if((!is_configured) && vmx_pt_config->ia32_rtit_addr_configured[3][0] && vmx_pt_config->ia32_rtit_addr_configured[3][1] && supported_addrn >= 4){
				vmx_pt_config->ia32_rtit_ctrl_msr |= ADDR3_EN;
				r = 0;
			}
			break;
		case KVM_VMX_PT_DISABLE_ADDR0:
			if((!is_configured) && (vmx_pt_config->ia32_rtit_ctrl_msr & ADDR0_EN)){
				vmx_pt_config->ia32_rtit_ctrl_msr ^= ADDR0_EN;
				r = 0;
			printk("Intel PT ADDR0 disabled...\n");

			}
			break;
		case KVM_VMX_PT_DISABLE_ADDR1:
			if((!is_configured) && (vmx_pt_config->ia32_rtit_ctrl_msr & ADDR1_EN)){
				vmx_pt_config->ia32_rtit_ctrl_msr ^= ADDR1_EN;
				r = 0;
			}
			break;
		case KVM_VMX_PT_DISABLE_ADDR2:
			if((!is_configured) && (vmx_pt_config->ia32_rtit_ctrl_msr & ADDR2_EN)){
				vmx_pt_config->ia32_rtit_ctrl_msr ^= ADDR2_EN;
				r = 0;
			}
			break;
		case KVM_VMX_PT_DISABLE_ADDR3:
			if((!is_configured) && (vmx_pt_config->ia32_rtit_ctrl_msr & ADDR3_EN)){
				vmx_pt_config->ia32_rtit_ctrl_msr ^= ADDR3_EN;
				r = 0;
			}
			break;

		case KVM_VMX_PT_CONFIGURE_MULTI_CR3:
			PRINT_INFO("KVM_VMX_PT_CONFIGURE_MULTI_CR3...");
			if(!is_configured) {
				argp = (void __user *)arg;		
				if (!copy_from_user(&multi_cr3_addr, argp, sizeof(multi_cr3_addr))){
					PRINT_INFO("COPYING...");
					vmx_pt_config->ia32_rtit_multi_cr3_0_match = multi_cr3_addr.cr3_0;
					vmx_pt_config->ia32_rtit_multi_cr3_1_match = multi_cr3_addr.cr3_1;
					vmx_pt_config->ia32_rtit_multi_cr3_2_match = multi_cr3_addr.cr3_2;
					vmx_pt_config->ia32_rtit_multi_cr3_3_match = multi_cr3_addr.cr3_3;
					vmx_pt_config->ia32_rtit_multi_cr3_num = multi_cr3_addr.num;
					r = 0;
				}
			}
			break;

		case KVM_VMX_PT_ENABLE_MULTI_CR3:
			if((!is_configured) && !(vmx_pt_config->ia32_rtit_ctrl_msr & CR3_FILTER) && vmx_pt_config->ia32_rtit_multi_cr3_num && !vmx_pt_config->multi_cr3_enabled ){
				vmx_pt_config->multi_cr3_enabled = true;
				r = 0;
			}
			break;

		case KVM_VMX_PT_DISABLE_MULTI_CR3:
			if((!is_configured) && vmx_pt_config->multi_cr3_enabled){
				vmx_pt_config->multi_cr3_enabled = false;
				r = 0;
			}
			break;
			
		case KVM_VMX_PT_CONFIGURE_CR3:
#ifdef DEBUG
			PRINT_INFO("KVM_VMX_PT_CONFIGURE_CR3...");
#endif
			if(!is_configured) {
				vmx_pt_config->ia32_rtit_cr3_match = arg; 
				r = 0;
			}
			else{
				printk("Intel PT KVM_VMX_PT_CONFIGURE_CR3 (is_configured: true) ...\n");
			}
			break;
		case KVM_VMX_PT_ENABLE_CR3:
			/* we just assume that cr3=NULL is invalid... */
			if((!is_configured) && vmx_pt_config->ia32_rtit_cr3_match && !vmx_pt_config->multi_cr3_enabled){
				vmx_pt_config->ia32_rtit_ctrl_msr |= CR3_FILTER;
				r = 0;
			}
			else{
				printk("Intel PT KVM_VMX_PT_ENABLE_CR3 failed!..\n");
			}
			//printk("Intel PT KVM_VMX_PT_ENABLE_CR3 ...");

			break;
		case KVM_VMX_PT_DISABLE_CR3:
			if((!is_configured) && (vmx_pt_config->ia32_rtit_ctrl_msr & CR3_FILTER)){
				vmx_pt_config->ia32_rtit_ctrl_msr ^= CR3_FILTER;
				r = 0;
			}
			else{
				printk("Intel PT KVM_VMX_PT_DISABLE_CR3 failed!..\n");
			}
			break;
		case KVM_VMX_PT_ENABLE:
			if(!is_configured) {
				if(!vmx_pt_config->state_change_pending){
					//PRINT_INFO("Intel PT enabled...");
					//vmx_pt_enable(vmx_pt_config);
					vmx_pt_config->state_change_pending = true;
					vmx_pt_config->state = true;
				}
				else{
					r = -EINVAL;
				}
			}
			r = 0;
			break;
		case KVM_VMX_PT_DISABLE:
			if(is_configured) {
				if(!vmx_pt_config->state_change_pending){
					//PRINT_INFO("Intel PT disabled...");
					r = vmx_pt_get_data_size(vmx_pt_config);

					vmx_pt_config->state_change_pending = true;
					vmx_pt_config->state = false;

					//vmx_pt_disable(vmx_pt_config);
					vmx_pt_config->reset = true;
				}
				else{
					r = -EINVAL;
				}
			}
			else{
				r = -EINVAL;
			}
			break;
		case KVM_VMX_PT_CHECK_TOPA_OVERFLOW:
			r = vmx_pt_check_overflow(vmx_pt_config);
			if(r){
				vmx_pt_config->reset = true;
#ifdef DEBUG
				printk("KVM_VMX_PT_CHECK_TOPA_OVERFLOW %ld\n", r);
#endif
			}
			break;
		case KVM_VMX_PT_GET_TOPA_SIZE:
			if (vmx_pt_config->topa_main_buf_virt_addr) {
				r = (TOPA_MAIN_SIZE + TOPA_FALLBACK_SIZE);
			} else {
				r = -ENOMEM;
			}
			break;
	}
	spin_unlock(&vmx_pt_config->spinlock);
	return r;
}

int vmx_pt_create_fd(struct vcpu_vmx_pt *vmx_pt_config){
	if (enabled){
		return anon_inode_getfd("vmx-pt", &vmx_pt_fops, vmx_pt_config, O_RDWR | O_CLOEXEC); 
	}
	else {
		return 0;
	}
}

/*===========================================================================* 

 *                          vmx/pt vcpu entry/exit                           * 
 *===========================================================================*/ 

static inline void vmx_pt_reconfigure_cpu(struct vcpu_vmx_pt *vmx_pt){ 
	
	uint64_t status;
	rdmsrl(MSR_IA32_RTIT_STATUS, status);
	if ((status & BIT_ULL(5))){
			//PRINT_INFO("Intel PT error detected...");
			status ^= BIT_ULL(5);
			wrmsrl(MSR_IA32_RTIT_STATUS, status);
	}
	if ((status & BIT_ULL(4))){
			//PRINT_INFO("Intel PT TOPA error detected...");
			status ^= BIT_ULL(4);
			wrmsrl(MSR_IA32_RTIT_STATUS, status);
	}
	
	/* set PacketByteBnt = 0 */
	status &= 0xFFFE0000FFFFFFFF;
	wrmsrl(MSR_IA32_RTIT_STATUS, status);
		
	/* reconfigure CR3-Filtering */
	if(vmx_pt->ia32_rtit_cr3_match)
		wrmsrl(MSR_IA32_CR3_MATCH, vmx_pt->ia32_rtit_cr3_match);
	
	/* reconfigure IP-Filtering  */
	if(vmx_pt->ia32_rtit_addr_configured[0][0]){
		wrmsrl(MSR_IA32_ADDR0_START, vmx_pt->ia32_rtit_addr_0[0]); 
		wrmsrl(MSR_IA32_ADDR0_END,   vmx_pt->ia32_rtit_addr_0[1]); 
	}
	if(vmx_pt->ia32_rtit_addr_configured[1][0]){
		wrmsrl(MSR_IA32_ADDR1_START, vmx_pt->ia32_rtit_addr_1[0]);
		wrmsrl(MSR_IA32_ADDR1_END,   vmx_pt->ia32_rtit_addr_1[1]); 
	}
	if(vmx_pt->ia32_rtit_addr_configured[2][0]){
		wrmsrl(MSR_IA32_ADDR2_START, vmx_pt->ia32_rtit_addr_2[0]);
		wrmsrl(MSR_IA32_ADDR2_END,   vmx_pt->ia32_rtit_addr_2[1]);
	}
	if(vmx_pt->ia32_rtit_addr_configured[3][0]){
		wrmsrl(MSR_IA32_ADDR3_START, vmx_pt->ia32_rtit_addr_3[0]);
		wrmsrl(MSR_IA32_ADDR3_END,   vmx_pt->ia32_rtit_addr_3[1]);
	}
	/* reconfigure ToPA */
	//printk("MSR_IA32_RTIT_OUTPUT_BASE:\t%lx\n", vmx_pt->ia32_rtit_output_base);
	//printk("MSR_IA32_RTIT_OUTPUT_MASK_PTRS:\t%lx\n", vmx_pt->ia32_rtit_output_mask_ptrs);
	
	//printk("CURRENT TOPA SIZE: %lx (%lx)\n", vmx_pt_get_data_size(vmx_pt), TOPA_MAIN_SIZE+TOPA_FALLBACK_SIZE);

	wrmsrl(MSR_IA32_RTIT_OUTPUT_BASE, vmx_pt->ia32_rtit_output_base);
	wrmsrl(MSR_IA32_RTIT_OUTPUT_MASK_PTRS, vmx_pt->ia32_rtit_output_mask_ptrs);
}

#ifdef DEBUG
static inline void vmx_pt_dump_trace_data(struct vcpu_vmx_pt *vmx_pt){
	int i;
	u64 cksum = 0;
	PRINT_INFO("Intel PT trace data:");
	print_hex_dump(KERN_DEBUG, "ToPA data: ", DUMP_PREFIX_OFFSET, 16, 8, vmx_pt->topa_main_buf_virt_addr, HEXDUMP_RANGE, true);
	
	for(i = 0; i < HEXDUMP_RANGE; i++){
		cksum += ((u8*)vmx_pt->topa_main_buf_virt_addr)[i];
	}
	printk("CHECKSUM: %lld\n", cksum);
}
#endif

static inline void vmx_pt_check_error(void){	
	u64 status;
	rdmsrl(MSR_IA32_RTIT_STATUS, status);
	if (status == 0x20){
		PRINT_ERROR("MSR_IA32_RTIT_STATUS -> STOPPED");
	}
	
	if(status == 0x10){
		PRINT_ERROR("MSR_IA32_RTIT_STATUS -> ERROR");
	}
}

void vmx_pt_vmentry(struct vcpu_vmx_pt *vmx_pt){
	uint64_t data;
	rdmsrl(MSR_IA32_RTIT_STATUS, data);
	if ((data & BIT(3))){
		printk("ERROR: Tracing is on during root-operations!\n");
	}
	
	if ((data & BIT(5))){
		printk("ERROR: TOPA STOP fml!\n");
	}


	if(enabled && vmx_pt && vmx_pt->state_change_pending){
		if(vmx_pt->state){
			vmx_pt_enable(vmx_pt);
		}
		else{
			vmx_pt_disable(vmx_pt);
		}
		vmx_pt->state_change_pending = false;
	}

	if (enabled && vmx_pt && vmx_pt->configured){
			spin_lock(&vmx_pt->spinlock);
			//vmx_pt_setup_vmx_autoload_msr(vmx_pt, true);
			/*
			if (vmx_pt->reset){
				vmx_pt->reset = false;
				prepare_topa(vmx_pt);
			}
			*/
			vmx_pt->cpu = raw_smp_processor_id();
			//printk("vmentry on cpu %d\n", raw_smp_processor_id());

			//if (vmx_pt_get_data_size(vmx_pt) >= TOPA_MAIN_SIZE/2){
			//	printk("[%d] vmx_pt_entry:\t%lx\n", raw_smp_processor_id(), vmx_pt_get_data_size(vmx_pt));
			//	return true;
			//}

			vmx_pt_reconfigure_cpu(vmx_pt);

			//wrmsrl(MSR_IA32_RTIT_OUTPUT_MASK_PTRS, vmx_pt->ia32_rtit_output_mask_ptrs);
			/*if (vmx_pt_check_overflow(vmx_pt)){
				// reconfigure ToPA 
				vmx_pt->ia32_rtit_output_mask_ptrs = 0x7fLL;
				wrmsrl(MSR_IA32_RTIT_OUTPUT_MASK_PTRS, vmx_pt->ia32_rtit_output_mask_ptrs);
			}
			*/
	}
}

/* return true if ToPA main region is full */
void vmx_pt_vmexit(struct vcpu_vmx_pt *vmx_pt){
	uint64_t data;
	rdmsrl(MSR_IA32_RTIT_STATUS, data);
	if ((data & BIT(3))){
		printk("ERROR: Tracing is on during root-operations!\n");
	}

	if ((data & BIT(5))){
		printk("ERROR: TOPA STOP fml!\n");
	}
	
	if (enabled && vmx_pt && vmx_pt->configured){
		if (vmx_pt->cpu != raw_smp_processor_id()){
			printk("CPU ERROR! %d != %d", vmx_pt->cpu, raw_smp_processor_id());
		}
		vmx_pt_check_error();
		rdmsrl(MSR_IA32_RTIT_OUTPUT_BASE, vmx_pt->ia32_rtit_output_base);
		rdmsrl(MSR_IA32_RTIT_OUTPUT_MASK_PTRS, vmx_pt->ia32_rtit_output_mask_ptrs);
		wmb();

		/*
		if (vmx_pt_get_data_size(vmx_pt) >= TOPA_MAIN_SIZE){
			//printk("[%d] vmx_pt_exit:\t%lx\n", raw_smp_processor_id(), vmx_pt_get_data_size(vmx_pt));
			return true;
		}
		*/

		//WRITE_ONCE(vmx_pt->ia32_rtit_output_base, topa_base);
		//WRITE_ONCE(vmx_pt->ia32_rtit_output_mask_ptrs, topa_mask_ptrs);

		/*
			u64 test;

		if (test&0x3F && (test&0x3F) == 3){
			rdmsrl(MSR_IA32_RTIT_CTL, test);
			printk("MSR_IA32_RTIT_CTL %lx\n", test);
		}

		if (vmx_pt_get_data_size(vmx_pt) >= TOPA_MAIN_SIZE/2){
			printk("[%d] vmx_pt_exit:\t%lx\n", raw_smp_processor_id(), vmx_pt_get_data_size(vmx_pt));
			return true;
		}
		*/
		//vmx_pt_setup_vmx_autoload_msr(vmx_pt, false);
		spin_unlock(&vmx_pt->spinlock);
	}
	//return false;
}

bool topa_full(struct vcpu_vmx_pt *vmx_pt){
	if(vmx_pt_get_data_size(vmx_pt) >= TOPA_MAIN_SIZE){
		return true;
	}
	return false;
}

/*===========================================================================*
 *                               vmx/pt vcpu setup                           *
 *===========================================================================*/

static int vmx_pt_setup_topa(struct vcpu_vmx_pt *vmx_pt)
{
	unsigned long main_buffer, fallback_buffer;
	u64 *topa;
	int n;
		
	main_buffer = __get_free_pages(GFP_KERNEL|__GFP_RETRY_MAYFAIL|__GFP_ZERO, TOPA_MAIN_ORDER);
	if (!main_buffer) {
		PRINT_ERROR("Cannot allocate main ToPA buffer!");
		return -ENOMEM;
	}	
	
	fallback_buffer = __get_free_pages(GFP_KERNEL|__GFP_RETRY_MAYFAIL|__GFP_ZERO, TOPA_FALLBACK_ORDER);
	if (!fallback_buffer) {
		PRINT_ERROR("Cannot allocate fallback ToPA buffer!");
		goto free_topa_buffer1;
	}

	topa = (u64 *)__get_free_page(GFP_KERNEL|__GFP_RETRY_MAYFAIL|__GFP_ZERO);
	if (!topa) {
		PRINT_ERROR("Cannot allocate ToPA table!");
		goto free_topa_buffer2;
	}

	memset((u64 *)main_buffer, 0x0, TOPA_MAIN_SIZE);
	memset((u64 *)fallback_buffer, 0x0, TOPA_FALLBACK_SIZE);

	/* VMX / PT ToPA
	*  +---------------------------------------+
	*  |ToPA Entry_A (TOPA_ORDER/2, INT)       | <------------------------\ (1. start tracing, send LVT PMI if this region is full)
	*  |ToPA Entry_B (TOPA_ORDER/2) [Fallback] | ptr to intial ToPA entry | (2. fallback area)
	*  |Topa Entry_C (PTR, END)                |--------------------------/ (3. force tracing stop, ptr to Entry_A)
	*  +---------------------------------------+
	*/
	
	n = 0;
	topa[n++] = (u64)__pa(main_buffer)	| (TOPA_MAIN_ORDER << TOPA_SIZE_SHIFT) | TOPA_INT;
	topa[n++] = (u64)__pa(fallback_buffer)	| (TOPA_FALLBACK_ORDER << TOPA_SIZE_SHIFT) | TOPA_STOP;
	topa[n] =   (u64)__pa(topa)		| TOPA_END;
		
	vmx_pt->topa_pt_region = (u64)topa;
	vmx_pt->ia32_rtit_output_base = __pa(topa);
	vmx_pt->ia32_rtit_output_mask_ptrs = 0x7fLL;	

	vmx_pt->ia32_rtit_output_base_init = __pa(topa);
	vmx_pt->ia32_rtit_output_mask_ptrs_init = 0x7fLL;	

	vmx_pt->topa_main_buf_virt_addr = (void*)main_buffer;
	vmx_pt->topa_fallback_buf_virt_addr = (void*)fallback_buffer;
	vmx_pt->topa_virt_addr = (void*)topa;
	
	//wrmsrl(MSR_IA32_RTIT_OUTPUT_BASE, vmx_pt->ia32_rtit_output_base);
	//wrmsrl(MSR_IA32_RTIT_OUTPUT_MASK_PTRS, vmx_pt->ia32_rtit_output_mask_ptrs);
	
	//prepare_topa(vmx_pt);
	vmx_pt->reset = true;
	
	return 0;
	
	free_topa_buffer2:
		free_pages(fallback_buffer, TOPA_FALLBACK_ORDER);
	free_topa_buffer1:
		free_pages(main_buffer, TOPA_MAIN_ORDER);

	vmx_pt->topa_main_buf_virt_addr = NULL;
	return -ENOMEM;
}
 
#ifdef DEBUG
static void vmx_pt_print_msrs(u64 ia32_rtit_ctrl_msr){
	PRINT_FEATURE(ia32_rtit_ctrl_msr, TRACE_EN,		"TRACE_EN");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, CTL_OS,		"CTL_OS");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, CTL_USER,		"CTL_USER");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, TO_PA,		"TO_PA");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, BRANCH_EN,		"BRANCH_EN");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, CR3_FILTER, 		"CR3_FILTER");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, MTC_EN, 		"MTC_EN");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, TSC_EN, 		"TSC_EN");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, ADDR0_EN,		"ADDR0_CFG");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, ADDR1_EN,		"ADDR1_CFG");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, ADDR2_EN,		"ADDR2_CFG");
	PRINT_FEATURE(ia32_rtit_ctrl_msr, ADDR3_EN,		"ADDR3_CFG");
}
#endif
		
static inline void vmx_pt_setup_msrs(struct vcpu_vmx_pt *vmx_pt){
	/* Disable: MTCEn, TSCEn, DisRTC, CYCEn, TraceEN
	 * Enable:  OS, CR3Filtering, ToPA, BranchEN
	 */ 
	int i;
	
	//vmx_pt->ia32_rtit_ctrl_msr = 0ULL;
	WRITE_ONCE(vmx_pt->ia32_rtit_ctrl_msr, (!TRACE_EN) | CTL_OS | CTL_USER | TO_PA | BRANCH_EN | DIS_RETC | PSB_MASK);
	//vmx_pt->ia32_rtit_ctrl_msr = (!TRACE_EN) | CTL_OS | CTL_USER | TO_PA | BRANCH_EN | DIS_RETC | PSB_MASK;
	
	for (i = 0; i < 4; i++){
		WRITE_ONCE(vmx_pt->ia32_rtit_addr_configured[i][0], false);
		WRITE_ONCE(vmx_pt->ia32_rtit_addr_configured[i][1], false);	
	}

	vmx_pt->ia32_rtit_addr_0[0] = (u64)NULL;
	vmx_pt->ia32_rtit_addr_0[1] = (u64)NULL;
	vmx_pt->ia32_rtit_addr_1[0] = (u64)NULL;
	vmx_pt->ia32_rtit_addr_1[1] = (u64)NULL;
	vmx_pt->ia32_rtit_addr_2[0] = (u64)NULL;
	vmx_pt->ia32_rtit_addr_2[1] = (u64)NULL;
	vmx_pt->ia32_rtit_addr_3[0] = (u64)NULL;
	vmx_pt->ia32_rtit_addr_3[1] = (u64)NULL;
	
	vmx_pt->ia32_rtit_cr3_match = (u64)NULL;

	vmx_pt->ia32_rtit_multi_cr3_0_match = (u64)NULL;
	vmx_pt->ia32_rtit_multi_cr3_1_match = (u64)NULL;
	vmx_pt->ia32_rtit_multi_cr3_2_match = (u64)NULL;
	vmx_pt->ia32_rtit_multi_cr3_3_match = (u64)NULL;
	vmx_pt->ia32_rtit_multi_cr3_num = 0;
	vmx_pt->multi_cr3_enabled = false;
}

void vmx_pt_setup_vmx_autoload_msr(struct vcpu_vmx_pt *vmx_pt_config, bool enable_vmx_pt){


	u64 guest_val, host_val;
	host_val = READ_ONCE(vmx_pt_config->ia32_rtit_ctrl_msr);
	
	/* check and unset IA32_RTIT_CTL_MSR.TraceEn for host msr */
	if (host_val & TRACE_EN){
		host_val ^= TRACE_EN;
	}

	/* set IA32_RTIT_CTL_MSR.TraceEn for guest msr (if intended) */
	guest_val = host_val;
	if(enable_vmx_pt)
		guest_val |= TRACE_EN;
	
#ifdef DEBUG
/*
	printk("GUEST: %llx\n", guest_val);
	PRINT_INFO("MSR_IA32_RTIT_CTL guest features:");
	vmx_pt_print_msrs(guest_val);
	PRINT_INFO("MSR_IA32_RTIT_CTL host features:");
	vmx_pt_print_msrs(host_val);
	*/
#endif
	if(!enable_vmx_pt){
		add_atomic_switch_msr(vmx_pt_config->vmx, MSR_IA32_RTIT_CTL, host_val, host_val, false);
	}
	else{
		add_atomic_switch_msr(vmx_pt_config->vmx, MSR_IA32_RTIT_CTL, guest_val, host_val, false);
	}
}

bool vmx_pt_multi_cr3_enabled(struct vcpu_vmx_pt *vmx_pt_config){
	return vmx_pt_config->configured && vmx_pt_config->multi_cr3_enabled;
}

void vmx_pt_toggle_entry(struct vcpu_vmx_pt *vmx_pt_config){
	if (vmx_pt_config->configured){
		spin_lock(&vmx_pt_config->spinlock);
		vmx_pt_setup_vmx_autoload_msr(vmx_pt_config, false);
	}
}

void vmx_pt_toggle_exit(struct vcpu_vmx_pt *vmx_pt_config){
	if (vmx_pt_config->configured){
		vmx_pt_setup_vmx_autoload_msr(vmx_pt_config, true);
		spin_unlock(&vmx_pt_config->spinlock);
	}
}

void vmx_pt_enable(struct vcpu_vmx_pt *vmx_pt_config){
	if (!vmx_pt_config->configured){
		vmx_pt_config->configured = true;
		vmx_pt_setup_vmx_autoload_msr(vmx_pt_config, true);
	}
}

void vmx_pt_disable(struct vcpu_vmx_pt *vmx_pt_config){
	if (vmx_pt_config->configured){
		vmx_pt_config->configured = false;
		vmx_pt_setup_vmx_autoload_msr(vmx_pt_config, false);
		wmb();
		topa_reset(vmx_pt_config);
		//vmx_pt_config->ia32_rtit_output_mask_ptrs = 0x7fLL;
		//wrmsrl(MSR_IA32_RTIT_OUTPUT_MASK_PTRS, vmx_pt_config->ia32_rtit_output_mask_ptrs);
	}
}

int vmx_pt_setup(struct vcpu_vmx *vmx, struct vcpu_vmx_pt **vmx_pt_config){ 
	int ret_val;
	if (enabled){
		ret_val = 0;
		*vmx_pt_config = kmalloc(sizeof(struct vcpu_vmx_pt), GFP_KERNEL);
		memset(*vmx_pt_config, 0x0, sizeof(struct vcpu_vmx_pt));
		(*vmx_pt_config)->vmx = vmx;
		(*vmx_pt_config)->configured = false;

		vmx_pt_setup_msrs(*vmx_pt_config);
		ret_val = vmx_pt_setup_topa(*vmx_pt_config);


		(*vmx_pt_config)->state_change_pending = false;
		(*vmx_pt_config)->state = false;


		spin_lock_init(&((*vmx_pt_config)->spinlock));
		if (ret_val)
			goto setup_fail1;
#ifdef DEBUG
		PRINT_INFO("Setup finished...");
#endif
		return 0;
		
		setup_fail1:
			PRINT_INFO("ToPA setup failed...");
			
		kfree(*vmx_pt_config);
		*vmx_pt_config = NULL;
		return ret_val;
	}
	*vmx_pt_config = NULL;
	return 0;
}

void vmx_pt_destroy(struct vcpu_vmx *vmx, struct vcpu_vmx_pt **vmx_pt_config){ 
	u64 value;
	rdmsrl(MSR_IA32_RTIT_CTL, value);

	if(!vmx_pt_config){
		return;
	}

	if(!(*vmx_pt_config)){
		return;
	}

#ifdef DEBUG
	vmx_pt_print_msrs(value);
	vmx_pt_dump_trace_data(*vmx_pt_config);
#endif

	free_pages((u64)(*vmx_pt_config)->topa_main_buf_virt_addr, TOPA_MAIN_ORDER);
	free_pages((u64)(*vmx_pt_config)->topa_fallback_buf_virt_addr, TOPA_FALLBACK_ORDER);
	free_page((u64)(*vmx_pt_config)->topa_virt_addr);

	kfree(*vmx_pt_config);
	*vmx_pt_config = NULL;
#ifdef DEBUG
	PRINT_INFO("Struct freed...");
#endif
}

/*===========================================================================*
 *			      vmx/pt initialization			     * 
 *===========================================================================*/

static int pt_topa_pmi_handler(unsigned int val, struct pt_regs *regs)
{
	/*
	 * Since this pmi results in vmexit during guest-mode execution and it is 
	 * possible to detect a nearly overflowed topa region later during runtime
	 * (by reading msrs), this nmi handler just 'handles' the pmi without doing something. 
	 * Otherwise, since this pmi is not 'precise', it might be executed after a task switch to another vcpu.
	 * This might result in a racy condition...
	 */
	u64 msr_value;
	rdmsrl(MSR_IA32_PERF_GLOBAL_STATUS, msr_value);
			//printk("CPU %d: <Intel PT PMI>\n", raw_smp_processor_id());


/*
			u64 data;
				rdmsrl(MSR_IA32_RTIT_CTL, data);
	if (data & TRACE_EN){
		printk("FUCK !! \n");
	}

*/
	//printk("%x\n", MSR_IA32_PERF_GLOBAL_STATUS);
	if (msr_value & TRACE_TOPA_PMI){
//#ifdef DEBUG
		printk("CPU %d: <Intel PT PMI>\n", raw_smp_processor_id());
//#endif
		return NMI_HANDLED;
	}
	//printk("NO TOPA PMI @ CPU %d: <Intel PT PMI> (%llx)\n", raw_smp_processor_id(), msr_value);
	return NMI_HANDLED;
	return NMI_DONE;
}

static int setup_vmx_pt_pmi_handler(void){
#ifdef DEBUG
	/* must be 0x400 -> NMI */
	printk("APIC_LVTPC: %lx\n", (long unsigned int)apic_read(APIC_LVTPC));
#endif
	
	if (register_nmi_handler(NMI_LOCAL, pt_topa_pmi_handler, 0, NMI_HANDLER)){
		//printk("%s: 1\n", __func__);
		PRINT_ERROR("LVT PMI handler registration failed!");
		return 1;
	}
	//printk("%s: 0\n", __func__);
	PRINT_INFO("LVT PMI handler registrated!");
	return 0;
}

static void disable_nmi_handler(void){
	unregister_nmi_handler(NMI_LOCAL, NMI_HANDLER);
	printk("LVT PMI handler disabled!");

	synchronize_rcu();
	PRINT_INFO("LVT PMI handler disabled!");
}

static int vmx_pt_check_support(void){
	/* 
	 *  Let's assume that all presented logical cpu cores provide the following features:
	 *  - Intel PT
	 *	- VMX supported tracing
	 *	- ToPA support for multiple reagions (otherwise it is hard to prevent ToPA overflows)
	 *	- Payloads are stored as IP not as LIP
	 *	- ADDRn_CFG = 3
	 */
	unsigned a, b, c, d;
	//unsigned a1, b1, c1, d1;
	u64 msr_value;

	cpuid(0, &a, &b, &c, &d);
	if (a < 0x14) {
		PRINT_ERROR("Not enough CPUID support for PT!");
		return -EIO;
	}
	cpuid_count(0x07, 0, &a, &b, &c, &d);
	if ((b & BIT(25)) == 0) {
		PRINT_ERROR("No PT support!");
		return -EIO;
	}
	cpuid_count(0x14, 0, &a, &b, &c, &d);
	if (!(c & BIT(0))) {
		PRINT_ERROR("No ToPA support!");
		return -EIO;
	}

	if ((c & BIT(31))){
		PRINT_ERROR("IP Payloads are LIP!");
		return -EIO;
	}

	if (!(c & BIT(1))) {
		PRINT_ERROR("Only one ToPA block supported!");  
		return -EIO;
	}

	if (!(b & BIT(2))) {   
		PRINT_ERROR("No IP-Filtering support!");  
		return -EIO;
	}

	cpuid_count(0x14, 1, &a, &b, &c, &d);
	//printk("Supported IP Filter Regions: %lx\n", a&0x7);
	supported_addrn = a&0x7;

	rdmsrl(MSR_IA32_VMX_MISC, msr_value);
	if (!(msr_value & BIT(14))){
		PRINT_ERROR("VMX operations are not supported in Intel PT tracing mode!");
		return -EIO; 
	}

	/* todo check ADDRn_CFG support */	
	return 0;
}

int vmx_pt_enabled(void){
	return (int)enabled; 
}

int vmx_pt_get_addrn_value(void){
	return supported_addrn;
}

void vmx_pt_init(void){
	enabled = !vmx_pt_check_support();
	if (enabled){
		PRINT_INFO("CPU is supported!");
		setup_vmx_pt_pmi_handler();
	}
}

void vmx_pt_exit(void){
	if (enabled){
		disable_nmi_handler();
	}
}
