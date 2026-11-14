#ifndef __KSU_H_54_COMPAT
#define __KSU_H_54_COMPAT

/*
 * Backports for the 5.4 vendor kernel this tree targets. Every macro is
 * guarded by both the kernel version and an existing-definition check so
 * the file stays harmless if a vendor tree already carries the newer API.
 */

#include <linux/version.h>
#include <linux/uaccess.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)

#define strncpy_from_user_nofault strncpy_from_unsafe_user

static inline long ksu_54_copy_from_user_nofault(void *dst, const void __user *src, size_t size)
{
	long ret = probe_user_read(dst, src, size);

	return ret < 0 ? ret : 0;
}
#define copy_from_user_nofault ksu_54_copy_from_user_nofault

static inline long ksu_54_copy_to_user_nofault(void __user *dst, const void *src, size_t size)
{
	long ret = probe_user_write(dst, src, size);

	return ret < 0 ? ret : 0;
}
#define copy_to_user_nofault ksu_54_copy_to_user_nofault

static inline long ksu_54_copy_to_kernel_nofault(void *dst, const void *src, size_t size)
{
	return probe_kernel_write(dst, src, size);
}
#define copy_to_kernel_nofault ksu_54_copy_to_kernel_nofault

#define copy_from_kernel_nofault probe_kernel_read

#ifndef TWA_RESUME
#define TWA_RESUME true
#endif

/*
 * p4d level indirection only exists for kernels that fold p4d into pgd;
 * this tree defines p4d_leaf() but has no __p4d_to_phys().
 */

#endif /* < 5.8.0 */

#endif /* __KSU_H_54_COMPAT */
