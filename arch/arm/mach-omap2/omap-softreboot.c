#include <linux/proc_fs.h>

#include <linux/fs.h>
#include <linux/icmp.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/ip.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/stat.h>
#include <linux/sysrq.h>
#include <net/protocol.h>
#include <net/sock.h>

#include <mach/control.h>
#include <mach/sram.h>


#define MODULE_NAME "omap_softreboot"
#define MODULE_AUTH "Vitaliy Filippov <vitalif|dog|mail.ru>"
#define MODULE_DESC "OMAP34xx/OMAP36xx software boot configuration and reset"
#define MODULE_VER "0.1"

#define OMAP_PROC_SOFTREBOOT "omap_softreboot"
#define OMAP_SOFTREBOOT_PROC_DESCR \
    "OMAP Software Boot Configuration allows you to set boot mode " \
    "and then reboot your device based on OMAP34xx or OMAP36xx programmatically.\n" \
    "\n" \
    "USAGE: echo dev1,dev2,dev3,dev4 > /proc/omap_softreboot\n" \
    "This sets the boot sequence and immediately reboots your device.\n" \
    "You can specify up to 4 boot devices.\n" \
    "PLEASE NOTE that if all devices are unbootable, OMAP will enter dead \n" \
    "loop and wait for hardware reset.\n" \
    "\n" \
    "Each of devX can be one of:\n" \
    "  mmc1     : First MMC/SD interface\n" \
    "  mmc2     : Second MMC/SD interface\n" \
    "  usb      : Remote boot via USB\n" \
    "  uart     : Remote boot via UART3\n" \
    "  nand     : NAND flash memory\n" \
    "  onenand  : OneNAND/Flex-OneNAND\n" \
    "  doc      : DiskOnChip\n" \
    "  xip      : NOR flash memory\n" \
    "  xip_wait : NOR flash memory with wait monitoring"

#define OMAP3_BOOT_MAGIC    0xCF00AA01

#define OMAP3_MASK_CHSETTINGS   0x1
#define OMAP3_MASK_CHRAM        0x2
#define OMAP3_MASK_CHFLASH      0x4
#define OMAP3_MASK_CHMMCSD      0x8

#define OMAP3_DEV_VOID      0x00
#define OMAP3_DEV_XIP       0x01
#define OMAP3_DEV_NAND      0x02
#define OMAP3_DEV_ONENAND   0x03
#define OMAP3_DEV_DOC       0x04
#define OMAP3_DEV_MMC2      0x05
#define OMAP3_DEV_MMC1      0x06
#define OMAP3_DEV_XIP_WAIT  0x07
#define OMAP3_DEV_UART      0x10
#define OMAP3_DEV_USB       0x11

#define OMAP3_MAX_SEQ       36
#define OMAP3_DEV_COUNT     0x12
static char *omap3_dev_names[] = {
    NULL,
    "xip",
    "nand",
    "onenand",
    "doc",
    "mmc2",
    "mmc1",
    "xip_wait",
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    "uart", "usb"
};

#define PWARN(fmt, args...) printk(KERN_WARNING "%s: " fmt, MODULE_NAME, ## args)

MODULE_LICENSE("GPL");
MODULE_AUTHOR(MODULE_AUTH);
MODULE_DESCRIPTION(MODULE_DESC);
MODULE_VERSION(MODULE_VER);

struct omap3_scratchpad
{
    u32 boot_config_ptr;
    u32 public_restore_ptr;
    u32 secure_ram_restore_ptr;
    u32 sdrc_module_semaphore;
    u32 prcm_block_offset;
    u32 sdrc_block_offset;
};

struct omap3_boot_config
{
    u32 magic;
    u32 size; // 0xC
    u16 ch_mask;
    u16 devices[4];
    u16 rsvd; // reserved
};

void omap_reset_to(u16 devices[4])
{
    struct omap3_boot_config boot_config;
    struct omap3_scratchpad scratchpad;

    if (cpu_is_omap34xx())
    {
#ifdef CONFIG_MAGIC_SYSRQ
        /* emergency sync */
        PWARN("calling 'sysrq + s' for emergency sync\n");
        handle_sysrq('s', NULL);

        /* emergency umount */
        PWARN("calling 'sysrq + u' for emergency umount\n");
        handle_sysrq('u', NULL);
#endif
        // Disable IRQ
        local_irq_disable();
        local_fiq_disable();

        // 0xAC boot config offset = reserve for 0x1C wakeup header + 0x38 PRCM block + 0x58 SDRC block
        scratchpad.boot_config_ptr = OMAP343X_SCRATCHPAD + 0xAC;
        scratchpad.public_restore_ptr = 0;
        scratchpad.secure_ram_restore_ptr = 0;
        scratchpad.sdrc_module_semaphore = 0;
        scratchpad.prcm_block_offset = 0;
        scratchpad.sdrc_block_offset = 0;
        memcpy_toio(OMAP2_IO_ADDRESS(OMAP343X_SCRATCHPAD), &scratchpad, sizeof(scratchpad));

        boot_config.magic = OMAP3_BOOT_MAGIC;
        boot_config.size = 0xC;
        boot_config.ch_mask = 0;
        boot_config.devices[0] = devices[0];
        boot_config.devices[1] = devices[1];
        boot_config.devices[2] = devices[2];
        boot_config.devices[3] = devices[3];
        memcpy_toio(OMAP2_IO_ADDRESS(OMAP343X_SCRATCHPAD) + 0xAC, &boot_config, sizeof(boot_config));

#define GLOBAL_REG_PRM      0x48307200
        __raw_writel(0x04, OMAP2_IO_ADDRESS(GLOBAL_REG_PRM) + 0x50);
    }
    else
        WARN_ON(1);
}

/*
#define SCRATCH_MEM                     0x48002910
#define GLOBAL_REG_PRM                  0x48307200

scratch_mem = ioremap(SCRATCH_MEM, 240);
global_reg_prm = ioremap(GLOBAL_REG_PRM, 256);*/

ssize_t proc_handle_softreboot(struct file *file, const char __user *buffer,
    unsigned long count, void *data)
{
    int len = count, i, start, k, cdev;
    u16 devs[4] = { 0 };
    char buf[OMAP3_MAX_SEQ];

    if (len > OMAP3_MAX_SEQ-1)
    {
        PWARN("Too long OMAP boot sequence\n");
        return -EFAULT;
    }
    if (copy_from_user(buf, buffer, len))
        return -EFAULT;

    buf[len++] = 0;
    for (i = 0, start = 0, cdev = 0; i < len; i++)
    {
        if (buf[i] == ',' || !buf[i] || buf[i] == '\n')
        {
            for (k = 0; k < OMAP3_DEV_COUNT; k++)
            {
                if (omap3_dev_names[k] &&
                    !strncmp(buf+start, omap3_dev_names[k], i-start))
                {
                    if (cdev >= 4)
                    {
                        PWARN("You can specify only up to 4 OMAP boot devices\n");
                        return -EFAULT;
                    }
                    devs[cdev++] = k;
                    break;
                }
            }
            if (k >= OMAP3_DEV_COUNT)
            {
                buf[i] = 0;
                PWARN("Incorrect OMAP boot device name \"%s\"\n", buf+start);
                return -EFAULT;
            }
            start = i+1;
            if (!buf[i] || buf[i] == '\n')
                break;
        }
    }

    // Reboot device with new configuration
    PWARN("New config: %x %x %x %x\n", devs[0], devs[1], devs[2], devs[3]);
    omap_reset_to(devs);

    return count;
}

ssize_t proc_softreboot_info(char *buffer, char **buffer_location,
    off_t offset, int buffer_length, int *eof, void *data)
{
    int len = 0;

    if (offset > 0) {
        *eof = 1;
        return len;
    }

    len += sprintf(buffer, "%s\n", OMAP_SOFTREBOOT_PROC_DESCR);

    return len;
}

static int __init omap_softreboot_init(void)
{
    struct proc_dir_entry *entry = NULL;

    PWARN("initialized, version %s (%s)\n", MODULE_VER, MODULE_AUTH);
    PWARN("See help in /proc/%s\n", OMAP_PROC_SOFTREBOOT);

    /* create PROC entry */
    entry = create_proc_entry(OMAP_PROC_SOFTREBOOT, S_IRUGO | S_IWUSR, NULL);

    if (!entry)
        return -ENODEV;
    else
    {
        /* set IO handles for proc entry */
        entry->owner = THIS_MODULE;
        entry->read_proc = proc_softreboot_info;
        entry->write_proc = proc_handle_softreboot;
    }

    return 0;
}

static void __exit omap_softreboot_exit(void)
{
    PWARN("module unloaded\n");
    remove_proc_entry(OMAP_PROC_SOFTREBOOT, NULL);
}

module_init(omap_softreboot_init);
module_exit(omap_softreboot_exit);
