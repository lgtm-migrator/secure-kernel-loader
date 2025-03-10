/*
 * Copyright (c) 2019 Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <defs.h>
#include <types.h>
#include <boot.h>
#include <pci.h>
#include <iommu.h>
#include "tpmlib/tpm.h"
#include "tpmlib/tpm2_constants.h"
#include <sha1sum.h>
#include <sha256.h>
#include <linux-bootparams.h>
#include <event_log.h>
#include <multiboot2.h>
#include <tags.h>
#include <string.h>
#include <printk.h>
#include <dev.h>

u32 boot_protocol;

skl_info_t __section(".skl_info") __used skl_info = {
    .uuid = {
        0x78, 0xf1, 0x26, 0x8e, 0x04, 0x92, 0x11, 0xe9,
        0x83, 0x2a, 0xc8, 0x5b, 0x76, 0xc4, 0xcc, 0x02,
    },
    .version = 0,
    .msb_key_algo = 0x14,
    .msb_key_hash = { 0 },
};

static void extend_pcr(struct tpm *tpm, void *data, u32 size, u32 pcr, char *ev)
{
    u8 hash[SHA1_DIGEST_SIZE];
    sha1sum(hash, data, size);
    print("shasum calculated:\n");
    hexdump(hash, SHA1_DIGEST_SIZE);
    tpm_extend_pcr(tpm, pcr, TPM_ALG_SHA1, hash);

    if ( tpm->family == TPM12 )
    {
        log_event_tpm12(pcr, hash, ev);
    }
    else if ( tpm->family == TPM20 )
    {
        u8 sha256_hash[SHA256_DIGEST_SIZE];

        sha256sum(sha256_hash, data, size);
        print("shasum calculated:\n");
        hexdump(sha256_hash, SHA256_DIGEST_SIZE);
        tpm_extend_pcr(tpm, pcr, TPM_ALG_SHA256, &sha256_hash[0]);

        log_event_tpm20(pcr, hash, sha256_hash, ev);
    }

    print("PCR extended\n");
}

/*
 * Checks if ptr points to *uncompressed* part of the kernel
 */
static inline void *is_in_kernel(struct boot_params *bp, void *ptr)
{
    if ( ptr < _p(bp->code32_start) ||
         ptr >= _p(bp->code32_start + (bp->syssize << 4)) ||
         (ptr >= _p(bp->code32_start + bp->payload_offset) &&
          ptr < _p(bp->code32_start + bp->payload_offset + bp->payload_length)) )
        return NULL;
    return ptr;
}

static inline struct kernel_info *get_kernel_info(struct boot_params *bp)
{
    return is_in_kernel(bp, _p(bp->code32_start + bp->kern_info_offset));
}

static inline struct mle_header *get_mle_hdr(struct boot_params *bp,
                                      struct kernel_info *ki)
{
    return is_in_kernel(bp, _p(bp->code32_start + ki->mle_header_offset));
}

static inline void *get_kernel_entry(struct boot_params *bp,
                                     struct mle_header *mle_hdr)
{
    return is_in_kernel(bp, _p(bp->code32_start + mle_hdr->sl_stub_entry));
}

/*
 * Even though die() has both __attribute__((noreturn)) and unreachable(),
 * Clang still complains if it isn't repeated here.
 */
static void __attribute__((noreturn)) reboot(void)
{
    print("Rebooting now...");
    die();
    unreachable();
}

#ifdef TEST_DMA
static void do_dma(void)
{
    /* Set up the DMA channel so we can use it.  This tells the DMA */
    /* that we're going to be using this channel.  (It's masked) */
    outb(0x0a, 0x05);

    /* Clear any data transfers that are currently executing. */
    outb(0x0c, 0x00);

    /* Send the specified mode to the DMA. */
    outb(0x0b, 0x45);

    /* Send the offset address.  The first byte is the low base offset, the */
    /* second byte is the high offset. */
    //~ outportb(AddrPort[DMA_channel], LOW_BYTE(blk->offset));
    //~ outportb(AddrPort[DMA_channel], HI_BYTE(blk->offset));
    outb(0x02, 0x00);
    outb(0x02, 0x00);

    /* Send the physical page that the data lies on. */
    //~ outportb(PagePort[DMA_channel], blk->page);
    outb(0x83, 0x00);

    /* Send the length of the data.  Again, low byte first. */
    //~ outportb(CountPort[DMA_channel], LOW_BYTE(blk->length));
    //~ outportb(CountPort[DMA_channel], HI_BYTE(blk->length));
    outb(0x03, 0x20);
    outb(0x03, 0x00);

    /* Ok, we're done.  Enable the DMA channel (clear the mask). */
    //~ outportb(MaskReg[DMA_channel], DMA_channel);
    outb(0x0a, 0x01);

    // "Device" says that it is ready to send data. As there is no device
    // physically sending the data, this reads idle bus lines.
    outb(0x09, 0x05);
}
#endif

static void iommu_setup(void)
{
    u32 iommu_cap;
    volatile u64 iommu_done __attribute__ ((aligned (8))) = 0;

#ifdef TEST_DMA
    memset(_p(1), 0xcc, 0x20); //_p(0) gives a null-pointer error
    print("before DMA:\n");
    hexdump(_p(0), 0x30);
    do_dma();
    /* Important line, it delays hexdump */
    print("after DMA:              \n");
    hexdump(_p(0), 0x30);
    memset(_p(1), 0xcc, 0x20);
    print("before DMA2\n");
    hexdump(_p(0), 0x30);
    do_dma();
    /* Important line, it delays hexdump */
    print("after DMA2              \n");
    hexdump(_p(0), 0x30);
#endif

    pci_init();
    iommu_cap = iommu_locate();

    /*
     * SKINIT enables protection against DMA access from devices for SLB
     * (whole 64K, not just the measured part). This ensures that no device
     * can overwrite code or data of SL. Unfortunately, it also means that
     * IOMMU, being a PCI device, also cannot read from this memory region.
     * When IOMMU is trying to read a command from buffer located in SLB it
     * receives COMMAND_HARDWARE_ERROR (master abort).
     *
     * Luckily, after that error it enters a fail-safe state in which all
     * operations originating from devices are blocked. The IOMMU itself can
     * still access the memory, so after the SLB protection is lifted, it can
     * try to read the data located inside SLB and set up a proper protection.
     *
     * TODO: split iommu_load_device_table() into two parts, before and after
     *       DEV disabling
     *
     * TODO2: check if IOMMU always blocks the devices, even when it was
     *        configured before SKINIT
     */

    if ( iommu_cap == 0 || iommu_load_device_table(iommu_cap, &iommu_done) )
    {
        if ( iommu_cap )
            print("IOMMU disabled by a firmware, please check your settings\n");

        print("Couldn't set up IOMMU, DMA attacks possible!\n");
    }
    else
    {
        /* Turn off SLB protection, try again */
        print("Disabling SLB protection\n");
        disable_memory_protection();

#ifdef TEST_DMA
        memset(_p(1), 0xcc, 0x20);
        print("before DMA:\n");
        hexdump(_p(0), 0x30);
        do_dma();
        /* Important line, it delays hexdump */
        print("after DMA:              \n");
        hexdump(_p(0), 0x30);
        /* Important line, it delays hexdump */
        print("and again\n");
        hexdump(_p(0), 0x30);

        memset(_p(1), 0xcc, 0x20);
        print("before DMA2\n");
        hexdump(_p(0), 0x30);
        do_dma();
        /* Important line, it delays hexdump */
        print("after DMA2              \n");
        hexdump(_p(0), 0x30);
        /* Important line, it delays hexdump */
        print("and again2\n");
        hexdump(_p(0), 0x30);
#endif

        iommu_load_device_table(iommu_cap, &iommu_done);
        print("Flushing IOMMU cache");
        while ( !iommu_done )
            print(".");

        print("\nIOMMU set\n");
    }

#ifdef TEST_DMA
    memset(_p(1), 0xcc, 0x20);
    print("before DMA:\n");
    hexdump(_p(0), 0x30);
    do_dma();
    /* Important line, it delays hexdump */
    print("after DMA:              \n");
    hexdump(_p(0), 0x30);
    /* Important line, it delays hexdump */
    print("and again\n");
    hexdump(_p(0), 0x30);

    memset(_p(1), 0xcc, 0x20);
    print("before DMA2\n");
    hexdump(_p(0), 0x30);
    do_dma();
    /* Important line, it delays hexdump */
    print("after DMA2              \n");
    hexdump(_p(0), 0x30);
    /* Important line, it delays hexdump */
    print("and again2\n");
    hexdump(_p(0), 0x30);
#endif
}

/*
 * Function return ABI magic:
 *
 * By returning a simple object of two pointers, the SYSV ABI splits it across
 * %rax and %rdx rather than spilling it to the stack.  This is far more
 * convenient for our asm caller to deal with.
 */
typedef struct {
    void *pm_kernel_entry; /* %eax */
    void *zero_page;       /* %edx */
} asm_return_t;

static asm_return_t skl_linux(struct tpm *tpm, struct skl_tag_boot_linux *skl_tag)
{
    struct boot_params *bp;
    struct kernel_info *ki;
    struct mle_header *mle_header;
    void *pm_kernel_entry;

    /* The Zero Page with the boot_params and legacy header */
    bp = _p(skl_tag->zero_page);

    print("\ncode32_start ");
    print_p(_p(bp->code32_start));

    if ( bp->version                            < 0x020f
         || (ki = get_kernel_info(bp))         == NULL
         || ki->header                         != KERNEL_INFO_HEADER
         || (mle_header = get_mle_hdr(bp, ki)) == NULL
         || mle_header->uuid[0]                != MLE_UUID0
         || mle_header->uuid[1]                != MLE_UUID1
         || mle_header->uuid[2]                != MLE_UUID2
         || mle_header->uuid[3]                != MLE_UUID3 )
    {
        print("\nKernel is too old or MLE header not present.\n");
        reboot();
    }

    print("\nmle_header\n");
    hexdump(mle_header, sizeof(struct mle_header));

    pm_kernel_entry = get_kernel_entry(bp, mle_header);

    if ( pm_kernel_entry == NULL )
    {
        print("\nBad kernel entry in MLE header.\n");
        reboot();
    }

    /* extend TB Loader code segment into PCR17 */
    extend_pcr(tpm, _p(bp->code32_start), bp->syssize << 4, 17,
               "Measured Kernel into PCR17");

    tpm_relinquish_locality(tpm);
    free_tpm(tpm);

    /* End of the line, off to the protected mode entry into the kernel */
    print("pm_kernel_entry:\n");
    hexdump(pm_kernel_entry, 0x100);
    print("zero_page:\n");
    hexdump(bp, 0x280);
    print("skl_base:\n");
    hexdump(_start, 0x100);
    print("device_table:\n");
    hexdump(device_table, 0x100);
    print("command_buf:\n");
    hexdump(command_buf, 0x1000);
    print("event_log:\n");
    hexdump(event_log, 0x1000);

    print("skl_main() is about to exit\n");

    return (asm_return_t){ pm_kernel_entry, bp };
}

static asm_return_t skl_multiboot2(struct tpm *tpm, struct skl_tag_boot_mb2 *skl_tag)
{
    void *kernel_entry;
    u32 kernel_size, mbi_len;
    struct multiboot_tag *tag;
    int i;

    /* This is MBI header, not a tag, but their structures are similar enough.
     * Note that 'size' offsets are reversed in those two! */
    tag = _p(skl_tag->mbi);

    /* skl_tag->kernel_size is either passed size of kernel from bootloader
     * or 0 */
    kernel_size = skl_tag->kernel_size;
    kernel_entry = _p(skl_tag->kernel_entry);

    /* Extend PCR18 with MBI structure's hash; this includes all cmdlines.
     * Use 'type' and not 'size', as their offsets are swapped in the header! */
    mbi_len = tag->type;
    extend_pcr(tpm, &tag, mbi_len, 18, "Measured MBI into PCR18");

    tag++;

    while ( tag->type )
    {
        if ( kernel_entry && kernel_size )
            break;

        /* If the entry point wasn't passed by a bootloader, we can only assume
         * that it starts at the kernel base address (true at least for Xen) */
        if ( !kernel_entry && tag->type == MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR )
        {
            struct multiboot_tag_load_base_addr *ba = (void *)tag;
            kernel_entry = _p(ba->load_base_addr);
            print("kernel_entry ");
            print_p(kernel_entry);
            print("\n");
        }

        /* This assumes that ELF has only one PROGBITS section, and that section
         * is the first one (i.e. it is loaded at load_base_addr). It is true
         * for Xen, but may not always the case.
         *
         * Also, GRUB2 creates this tag after all module tags, so separate loop
         * is needed for consistent order of PCR extension operations. */
        if ( !kernel_size && tag->type == MULTIBOOT_TAG_TYPE_ELF_SECTIONS )
        {
            struct multiboot_tag_elf_sections *es_tag = (void *)tag;
            for ( i = 0; i < es_tag->num; i++ )
            {
                Elf32_Shdr *sh = (void *)&es_tag->sections[es_tag->entsize * i];
                if ( sh->sh_type == SHT_PROGBITS )
                {
                    kernel_size = sh->sh_size;
                    print("kernel_size ");
                    print_p(_p(kernel_size));
                    print("\n");
                    break;
                }
            }
        }

        tag = multiboot_next_tag(tag);
    }

    extend_pcr(tpm, kernel_entry, kernel_size, 17,
               "Measured Kernel into PCR17");

    tag = _p(skl_tag->mbi);
    tag++;

    while ( tag->type )
    {
        if ( tag->type == MULTIBOOT_TAG_TYPE_MODULE )
        {
            struct multiboot_tag_module *mod = (void *)tag;
            print("Module '");
            print(mod->cmdline);
            print("' [");
            print_p(_p(mod->mod_start));
            print_p(_p(mod->mod_end));
            print("]\n");
            extend_pcr(tpm, _p(mod->mod_start), mod->mod_end - mod->mod_start,
                       17, mod->cmdline);
        }

        tag = multiboot_next_tag(tag);
    }

    /* Safety checks */
    if ( tag->size != 8
         || _p(multiboot_next_tag(tag)) > _p(skl_tag->mbi) + mbi_len )
    {
        print("MBI safety checks failed\n");
        reboot();
    }

    boot_protocol = MULTIBOOT2;

    return (asm_return_t){ kernel_entry, _p(skl_tag->mbi) };
}

static asm_return_t skl_simple_payload(struct tpm *tpm, struct skl_tag_boot_simple_payload *skl_tag)
{
    extend_pcr(tpm, _p(skl_tag->base), skl_tag->size, 17, "Measured payload into PCR17");

    boot_protocol = SIMPLE_PAYLOAD;

    return (asm_return_t){ _p(skl_tag->entry), _p(skl_tag->arg) };
}

asm_return_t skl_main(void)
{
    asm_return_t ret;
    struct tpm *tpm;
    struct skl_tag_hdr *t = (struct skl_tag_hdr*) &bootloader_data;

    /*
     * Now in 64b mode, paging is setup. This is the launching point. We can
     * now do what we want. First order of business is to setup
     * DEV to cover memory from the start of bzImage to the end of the SKL
     * "kernel". At the end, trampoline to the PM entry point which will
     * include the Secure Launch stub.
     */
    pci_init();

    /* Disable memory protection and setup IOMMU */
    iommu_setup();

    if ( t->type                              != SKL_TAG_TAGS_SIZE
         || t->len                            != sizeof(struct skl_tag_tags_size)
         || end_of_tags()                      > _p(_start + SLB_SIZE)
         || (t = next_of_type(t, SKL_TAG_END)) == NULL
         || _p(t) + t->len                    != end_of_tags())
    {
        print("Bad bootloader data format\n");
        reboot();
    }

    /*
     * TODO Note these functions can fail but there is no clear way to
     * report the error unless SKINIT has some resource to do this. For
     * now, if an error is returned, this code will most likely just crash.
     */
    tpm = enable_tpm();
    tpm_request_locality(tpm, 2);
    event_log_init(tpm);

    /* Now that we have TPM and event log, measure bootloader data */
    extend_pcr(tpm, &bootloader_data, bootloader_data.size, 18,
               "Measured bootloader data into PCR18");

    t = next_of_class(&bootloader_data, SKL_TAG_BOOT_CLASS);
    if ( t == NULL || next_of_class(t, SKL_TAG_BOOT_CLASS) != NULL )
    {
        print("No boot tag or multiple boot tags\n");
        reboot();
    }

    switch( t->type )
    {
    case SKL_TAG_BOOT_LINUX:
        ret = skl_linux(tpm, (struct skl_tag_boot_linux *)t);
        break;
    case SKL_TAG_BOOT_MB2:
        ret = skl_multiboot2(tpm, (struct skl_tag_boot_mb2 *)t);
        break;
    case SKL_TAG_BOOT_SIMPLE:
        ret = skl_simple_payload(tpm, (struct skl_tag_boot_simple_payload *)t);
        break;
    default:
        print("Unknown kernel boot protocol\n");
        reboot();
    }

    tpm_relinquish_locality(tpm);
    free_tpm(tpm);

    /* End of the line, off to the protected mode entry into the kernel */
    print("pm_kernel_entry:\n");
    hexdump(ret.pm_kernel_entry, 0x100);
    print("zero_page:\n");
    hexdump(ret.zero_page, 0x280);
    print("skl_base:\n");
    hexdump(_start, 0x100);
    print("bootloader_data:\n");
    hexdump(&bootloader_data, bootloader_data.size);

    t = next_of_type(&bootloader_data, SKL_TAG_EVENT_LOG);
    if ( t != NULL )
    {
        print("TPM event log:\n");
        hexdump(_p(((struct skl_tag_evtlog *)t)->address),
                ((struct skl_tag_evtlog *)t)->size);
    }

    if ( skl_stack_canary != STACK_CANARY )
    {
        print("Stack is too small, possible corruption\n");
        reboot();
    }

    print("skl_main() is about to exit\n");

    return ret;
}

static void __maybe_unused build_assertions(void)
{
    struct boot_params b;
    struct kernel_info k;

    BUILD_BUG_ON(offsetof(typeof(b), tb_dev_map)        != 0x0d8);
    BUILD_BUG_ON(offsetof(typeof(b), syssize)           != 0x1f4);
    BUILD_BUG_ON(offsetof(typeof(b), version)           != 0x206);
    BUILD_BUG_ON(offsetof(typeof(b), code32_start)      != 0x214);
    BUILD_BUG_ON(offsetof(typeof(b), cmd_line_ptr)      != 0x228);
    BUILD_BUG_ON(offsetof(typeof(b), cmdline_size)      != 0x238);
    BUILD_BUG_ON(offsetof(typeof(b), payload_offset)    != 0x248);
    BUILD_BUG_ON(offsetof(typeof(b), payload_length)    != 0x24c);
    BUILD_BUG_ON(offsetof(typeof(b), kern_info_offset)  != 0x268);

    BUILD_BUG_ON(offsetof(typeof(k), mle_header_offset) != 0x010);
}
