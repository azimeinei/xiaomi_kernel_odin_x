// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/fsnotify_backend.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/rculist.h>
#include <linux/task_work.h>
#include <linux/version.h>
#include "klog.h" // IWYU pragma: keep
#include "manager/throne_tracker.h"

// Older kernels use a boolean task_work notification argument.
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0) && !defined(TWA_RESUME)
#define TWA_RESUME true
#endif

// fsnotify passes the name as a qstr only since 5.2; before that it is a
// raw byte string.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
typedef const struct qstr *ksu_fname_t;
#define ksu_fname_len(f) ((f)->len)
#define ksu_fname_arg(f) ((f)->name)
#else
typedef const unsigned char *ksu_fname_t;
#define ksu_fname_len(f) (strlen(f))
#define ksu_fname_arg(f) (f)
#endif

#define MASK_SYSTEM (FS_CREATE | FS_MOVE | FS_EVENT_ON_CHILD)

struct watch_dir {
    const char *path;
    u32 mask;
    struct path kpath;
    struct inode *inode;
    struct fsnotify_mark *mark;
};

static struct fsnotify_group *g;

static void ksu_track_throne_tw_func(struct callback_head *cb)
{
    kfree(cb);
    // Runs after exit_fs() if the task is exiting; path lookups would oops
    if (current->flags & PF_EXITING)
        return;
    track_throne(false);
}

// Never scan from the fsnotify handler: fsnotify_mark_srcu and the rename's dir
// locks would be held for the whole scan. Defer with task_work, not a kthread
// or kworker: the scan must finish before the rename returns, or the manager
// can be opened before it is crowned, and it must run in the renamer's
// (Android's) namespaces, which differ from init_task's on WSA/Waydroid.
static void ksu_defer_track_throne(void)
{
    struct callback_head *cb;

    if (current->flags & PF_KTHREAD)
        goto skipped;

    cb = kzalloc(sizeof(*cb), GFP_KERNEL);
    if (!cb)
        goto skipped;

    cb->func = ksu_track_throne_tw_func;
    if (!task_work_add(current, cb, TWA_RESUME))
        return;

    kfree(cb);
skipped:
    pr_warn("defer track_throne failed, skipping scan\n");
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
#define KSU_DECL_FSNOTIFY_OPS(name)                                            \
	int name(struct fsnotify_mark *mark, u32 mask, struct inode *inode,    \
		 struct inode *dir, const struct qstr *file_name, u32 cookie)
#else
#define KSU_DECL_FSNOTIFY_OPS(name)                                            \
	int name(struct fsnotify_group *group, struct inode *inode, u32 mask,  \
		 const void *data, int data_type, ksu_fname_t file_name,       \
		 u32 cookie, struct fsnotify_iter_info *iter_info)
#endif

static KSU_DECL_FSNOTIFY_OPS(ksu_handle_inode_event)
{
    if (!file_name || (mask & FS_ISDIR))
        return 0;

    if (ksu_fname_len(file_name) == 13 &&
        !memcmp(ksu_fname_arg(file_name), "packages.list", 13)) {
        pr_info("packages.list detected: %d\n", mask);
        ksu_defer_track_throne();
    }
    return 0;
}

static const struct fsnotify_ops ksu_ops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
    .handle_inode_event = ksu_handle_inode_event,
#else
    .handle_event = ksu_handle_inode_event,
#endif
};

static int add_mark_on_inode(struct inode *inode, u32 mask, struct fsnotify_mark **out)
{
    struct fsnotify_mark *m;

    m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m)
        return -ENOMEM;

    fsnotify_init_mark(m, g);
    m->mask = mask;

    if (fsnotify_add_inode_mark(m, inode, 0)) {
        fsnotify_put_mark(m);
        return -EINVAL;
    }
    *out = m;
    return 0;
}

static int watch_one_dir(struct watch_dir *wd)
{
    int ret = kern_path(wd->path, LOOKUP_FOLLOW, &wd->kpath);
    if (ret) {
        pr_info("path not ready: %s (%d)\n", wd->path, ret);
        return ret;
    }
    wd->inode = d_inode(wd->kpath.dentry);
    ihold(wd->inode);

    ret = add_mark_on_inode(wd->inode, wd->mask, &wd->mark);
    if (ret) {
        pr_err("Add mark failed for %s (%d)\n", wd->path, ret);
        path_put(&wd->kpath);
        iput(wd->inode);
        wd->inode = NULL;
        return ret;
    }
    pr_info("watching %s\n", wd->path);
    return 0;
}

static void unwatch_one_dir(struct watch_dir *wd)
{
    if (wd->mark) {
        fsnotify_destroy_mark(wd->mark, g);
        fsnotify_put_mark(wd->mark);
        wd->mark = NULL;
    }
    if (wd->inode) {
        iput(wd->inode);
        wd->inode = NULL;
    }
    if (wd->kpath.dentry) {
        path_put(&wd->kpath);
        memset(&wd->kpath, 0, sizeof(wd->kpath));
    }
}

static struct watch_dir g_watch = { .path = "/data/system", .mask = MASK_SYSTEM };

int ksu_observer_init(void)
{
    int ret = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    g = fsnotify_alloc_group(&ksu_ops, 0);
#else
    g = fsnotify_alloc_group(&ksu_ops);
#endif
    if (IS_ERR(g))
        return PTR_ERR(g);

    ret = watch_one_dir(&g_watch);
    pr_info("observer init done\n");
    return 0;
}

void __exit ksu_observer_exit(void)
{
    unwatch_one_dir(&g_watch);
    fsnotify_put_group(g);
    pr_info("observer exit done\n");
}
