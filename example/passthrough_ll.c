/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

/** @file
 *
 * This file system mirrors the existing file system hierarchy of the
 * system, starting at the root file system. This is implemented by
 * just "passing through" all requests to the corresponding user-space
 * libc functions. In contrast to passthrough.c and passthrough_fh.c,
 * this implementation uses the low-level API. Its performance should
 * be the least bad among the three, but many operations are not
 * implemented. In particular, it is not possible to remove files (or
 * directories) because the code necessary to defer actual removal
 * until the file is not opened anymore would make the example much
 * more complicated.
 *
 * When writeback caching is enabled (-o writeback mount option), it
 * is only possible to write to files for which the mounting user has
 * read permissions. This is because the writeback cache requires the
 * kernel to be able to issue read requests for all files (which the
 * passthrough filesystem cannot satisfy if it can't read the file in
 * the underlying filesystem).
 *
 * Compile with:
 *
 *     gcc -Wall passthrough_ll.c `pkg-config fuse3 --cflags --libs` -o passthrough_ll
 *
 * ## Source code ##
 * \include passthrough_ll.c
 */

#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include "config.h"

#include <fuse_lowlevel.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <semaphore.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/file.h>
#include <sys/xattr.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ireg.h"
#include "uthash.h"

/* We are re-using pointers to our `struct lo_inode` and `struct
   lo_dirp` elements as inodes. This means that we must be able to
   store uintptr_t values in a fuse_ino_t variable. The following
   incantation checks this condition at compile time. */
#if defined(__GNUC__) && (__GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__ >= 6) && !defined __cplusplus
_Static_assert(sizeof(fuse_ino_t) >= sizeof(uintptr_t),
	       "fuse_ino_t too small to hold uintptr_t values!");
#else
struct _uintptr_to_must_hold_fuse_ino_t_dummy_struct \
	{ unsigned _uintptr_to_must_hold_fuse_ino_t:
			((sizeof(fuse_ino_t) >= sizeof(uintptr_t)) ? 1 : -1); };
#endif

struct lo_key {
	ino_t ino;
	dev_t dev;
};

struct lo_inode {
	UT_hash_handle hh;  /* protected by lo->mutex */
	int fd;
	bool is_symlink;
	struct lo_key key;
	uint64_t refcount; /* protected by lo->mutex */
	uint64_t version_offset;
	uint64_t ireg_refid;
};

struct lo_cred {
	uid_t euid;
	gid_t egid;
};

enum {
	CACHE_NONE,
	CACHE_AUTO,
	CACHE_ALWAYS,
};

struct lo_data {
	pthread_mutex_t mutex;
	int debug;
	int norace;
	int writeback;
	int flock;
	int xattr;
	const char *source;
	double timeout;
	int cache;
	int shared;
	int timeout_set;
	int readdirplus_set;
	int readdirplus_clear;
	struct lo_inode root;
	struct lo_inode *inodes; /* protected by lo->mutex */
	int ireg_sock;
	int64_t *version_table;
	uint64_t version_table_size;
};

static const struct fuse_opt lo_opts[] = {
	{ "writeback",
	  offsetof(struct lo_data, writeback), 1 },
	{ "no_writeback",
	  offsetof(struct lo_data, writeback), 0 },
	{ "source=%s",
	  offsetof(struct lo_data, source), 0 },
	{ "flock",
	  offsetof(struct lo_data, flock), 1 },
	{ "no_flock",
	  offsetof(struct lo_data, flock), 0 },
	{ "xattr",
	  offsetof(struct lo_data, xattr), 1 },
	{ "no_xattr",
	  offsetof(struct lo_data, xattr), 0 },
	{ "timeout=%lf",
	  offsetof(struct lo_data, timeout), 0 },
	{ "timeout=",
	  offsetof(struct lo_data, timeout_set), 1 },
	{ "cache=none",
	  offsetof(struct lo_data, cache), CACHE_NONE },
	{ "cache=auto",
	  offsetof(struct lo_data, cache), CACHE_AUTO },
	{ "cache=always",
	  offsetof(struct lo_data, cache), CACHE_ALWAYS },
	{ "shared",
	  offsetof(struct lo_data, shared), 1 },
	{ "no_shared",
	  offsetof(struct lo_data, shared), 0 },
	{ "norace",
	  offsetof(struct lo_data, norace), 1 },
	{ "readdirplus",
	  offsetof(struct lo_data, readdirplus_set), 1 },
	{ "no_readdirplus",
	  offsetof(struct lo_data, readdirplus_clear), 1 },
	FUSE_OPT_END
};
static void unref_inode(struct lo_data *lo, struct lo_inode *inode, uint64_t n);

static struct lo_inode *lo_find(struct lo_data *lo, struct stat *st);


static struct lo_data *lo_data(fuse_req_t req)
{
	return (struct lo_data *) fuse_req_userdata(req);
}

static struct lo_inode *lo_inode(fuse_req_t req, fuse_ino_t ino)
{
	if (ino == FUSE_ROOT_ID)
		return &lo_data(req)->root;
	else
		return (struct lo_inode *) (uintptr_t) ino;
}

static int lo_fd(fuse_req_t req, fuse_ino_t ino)
{
	return lo_inode(req, ino)->fd;
}

static bool lo_debug(fuse_req_t req)
{
	return lo_data(req)->debug != 0;
}

static void lo_init(void *userdata,
		    struct fuse_conn_info *conn)
{
	struct lo_data *lo = (struct lo_data*) userdata;

	if(conn->capable & FUSE_CAP_EXPORT_SUPPORT)
		conn->want |= FUSE_CAP_EXPORT_SUPPORT;

	if (lo->writeback &&
	    conn->capable & FUSE_CAP_WRITEBACK_CACHE) {
		if (lo->debug)
			fprintf(stderr, "lo_init: activating writeback\n");
		conn->want |= FUSE_CAP_WRITEBACK_CACHE;
	}
	if (lo->flock && conn->capable & FUSE_CAP_FLOCK_LOCKS) {
		if (lo->debug)
			fprintf(stderr, "lo_init: activating flock locks\n");
		conn->want |= FUSE_CAP_FLOCK_LOCKS;
	}
	/* TODO: shared version support for readdirplus */
	if ((lo->cache == CACHE_NONE && !lo->readdirplus_set) ||
	    lo->readdirplus_clear || lo->shared) {
		if (lo->debug)
			fprintf(stderr, "lo_init: disabling readdirplus\n");
		conn->want &= ~FUSE_CAP_READDIRPLUS;
	}

}

static int64_t *version_ptr(struct lo_data *lo, struct lo_inode *inode)
{
	return lo->version_table + inode->version_offset;
}

static int64_t get_version(struct lo_data *lo, struct lo_inode *inode)
{
	if (!inode->version_offset)
		return 0;

	return __atomic_load_8(version_ptr(lo, inode), __ATOMIC_SEQ_CST);
}

static void update_version(struct lo_data *lo, struct lo_inode *inode)
{
	if (!inode->version_offset)
		return;

	__atomic_add_fetch(version_ptr(lo, inode), 1, __ATOMIC_SEQ_CST);
}

static void lo_getattr(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi)
{
	int res;
	struct stat buf;
	struct lo_data *lo = lo_data(req);

	(void) fi;

	res = fstatat(lo_fd(req, ino), "", &buf, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return (void) fuse_reply_err(req, errno);

	fuse_reply_attr(req, &buf, lo->timeout);
}

static int lo_parent_and_name(struct lo_data *lo, struct lo_inode *inode,
			      char path[PATH_MAX], struct lo_inode **parent)
{
	char procname[64];
	char *last;
	struct stat stat;
	struct lo_inode *p;
	int retries = 2;
	int res;

retry:
	sprintf(procname, "/proc/self/fd/%i", inode->fd);

	res = readlink(procname, path, PATH_MAX);
	if (res < 0) {
		warn("lo_parent_and_name: readlink failed");
		goto fail_noretry;
	}

	if (res >= PATH_MAX) {
		warnx("lo_parent_and_name: readlink overflowed");
		goto fail_noretry;
	}
	path[res] = '\0';

	last = strrchr(path, '/');
	if (last == NULL) {
		/* Shouldn't happen */
		warnx("lo_parent_and_name: INTERNAL ERROR: bad path read from proc");
		goto fail_noretry;
	}
	if (last == path) {
		p = &lo->root;
		pthread_mutex_lock(&lo->mutex);
		p->refcount++;
		pthread_mutex_unlock(&lo->mutex);
	} else {
		*last = '\0';
		res = fstatat(AT_FDCWD, last == path ? "/" : path, &stat, 0);
		if (res == -1) {
			if (!retries)
				warn("lo_parent_and_name: failed to stat parent");
			goto fail;
		}
		p = lo_find(lo, &stat);
		if (p == NULL) {
			if (!retries)
				warnx("lo_parent_and_name: failed to find parent");
			goto fail;
		}
	}
	last++;
	res = fstatat(p->fd, last, &stat, AT_SYMLINK_NOFOLLOW);
	if (res == -1) {
		if (!retries)
			warn("lo_parent_and_name: failed to stat last");
		goto fail_unref;
	}
	if (stat.st_dev != inode->key.dev || stat.st_ino != inode->key.ino) {
		if (!retries)
			warnx("lo_parent_and_name: filed to match last");
		goto fail_unref;
	}
	*parent = p;
	memmove(path, last, strlen(last) + 1);

	return 0;

fail_unref:
	unref_inode(lo, p, 1);
fail:
	if (retries) {
		retries--;
		goto retry;
	}
fail_noretry:
	errno = EIO;
	return -1;
}

static int utimensat_empty(struct lo_data *lo, struct lo_inode *inode,
			   const struct timespec *tv)
{
	int res;
	struct lo_inode *parent;
	char path[PATH_MAX];

	if (inode->is_symlink) {
		res = utimensat(inode->fd, "", tv, AT_EMPTY_PATH);
		if (res == -1 && errno == EINVAL) {
			/* Sorry, no race free way to set times on symlink. */
			if (lo->norace)
				errno = EPERM;
			else
				goto fallback;
		}
		return res;
	}
	sprintf(path, "/proc/self/fd/%i", inode->fd);

	return utimensat(AT_FDCWD, path, tv, 0);

fallback:
	res = lo_parent_and_name(lo, inode, path, &parent);
	if (res != -1)
		res = utimensat(parent->fd, path, tv, AT_SYMLINK_NOFOLLOW);

	return res;
}

static void lo_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
		       int valid, struct fuse_file_info *fi)
{
	int saverr;
	char procname[64];
	struct lo_inode *inode = lo_inode(req, ino);
	int ifd = inode->fd;
	struct lo_data *lo = lo_data(req);
	int res;

	if (valid & FUSE_SET_ATTR_MODE) {
		if (fi) {
			res = fchmod(fi->fh, attr->st_mode);
		} else {
			sprintf(procname, "/proc/self/fd/%i", ifd);
			res = chmod(procname, attr->st_mode);
		}
		if (res == -1)
			goto out_err;
	}
	if (valid & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
		uid_t uid = (valid & FUSE_SET_ATTR_UID) ?
			attr->st_uid : (uid_t) -1;
		gid_t gid = (valid & FUSE_SET_ATTR_GID) ?
			attr->st_gid : (gid_t) -1;

		res = fchownat(ifd, "", uid, gid,
			       AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
		if (res == -1)
			goto out_err;
	}
	if (valid & FUSE_SET_ATTR_SIZE) {
		if (fi) {
			res = ftruncate(fi->fh, attr->st_size);
		} else {
			sprintf(procname, "/proc/self/fd/%i", ifd);
			res = truncate(procname, attr->st_size);
		}
		if (res == -1)
			goto out_err;
	}
	if (valid & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) {
		struct timespec tv[2];

		tv[0].tv_sec = 0;
		tv[1].tv_sec = 0;
		tv[0].tv_nsec = UTIME_OMIT;
		tv[1].tv_nsec = UTIME_OMIT;

		if (valid & FUSE_SET_ATTR_ATIME_NOW)
			tv[0].tv_nsec = UTIME_NOW;
		else if (valid & FUSE_SET_ATTR_ATIME)
			tv[0] = attr->st_atim;

		if (valid & FUSE_SET_ATTR_MTIME_NOW)
			tv[1].tv_nsec = UTIME_NOW;
		else if (valid & FUSE_SET_ATTR_MTIME)
			tv[1] = attr->st_mtim;

		if (fi)
			res = futimens(fi->fh, tv);
		else
			res = utimensat_empty(lo, inode, tv);
		if (res == -1)
			goto out_err;
	}
	update_version(lo, inode);

	return lo_getattr(req, ino, fi);

out_err:
	saverr = errno;
	fuse_reply_err(req, saverr);
}

static struct lo_inode *lo_find(struct lo_data *lo, struct stat *st)
{
	struct lo_inode *p;
	struct lo_key key = {
		.ino = st->st_ino,
		.dev = st->st_dev,
	};

	pthread_mutex_lock(&lo->mutex);
	HASH_FIND(hh, lo->inodes, &key, sizeof(key), p);
	if (p) {
		assert(p->refcount > 0);
		p->refcount++;
	}
	pthread_mutex_unlock(&lo->mutex);

	return p;
}

struct msgreply {
	struct lo_inode *inode;
	sem_t ready;
};

static void get_shared(struct lo_data *lo, struct lo_inode *inode)
{
	int res;
	struct msgreply rep = {
		.inode = inode,
	};
	struct ireg_msg msg = {
		.op = IREG_GET,
		.handle = (uintptr_t) &rep,
		.get = {
			.ino = inode->key.ino,
			.dev = inode->key.dev,
		},
	};

	if (lo->ireg_sock == -1) {
		inode->version_offset = 0;
		return;
	}

	sem_init(&rep.ready, 0, 0);

	res = write(lo->ireg_sock, &msg, sizeof(msg));
	if (res != sizeof(msg)) {
		if (res == -1)
			warn("write(lo->ireg_sock, {IREG_GET, ...})");
		else
			warnx("short write to ireg_sock: %i", res);
		return;
	}

	while (sem_wait(&rep.ready));
	sem_destroy(&rep.ready);
}

static void put_shared(struct lo_data *lo, struct lo_inode *inode)
{
	int res;
	struct ireg_msg msg = {
		.op = IREG_PUT,
		.put.refid = inode->ireg_refid,
	};

	if (lo->ireg_sock == -1)
		return;

	res = write(lo->ireg_sock, &msg, sizeof(msg));
	if (res != sizeof(msg)) {
		if (res == -1)
			warn("write(lo->ireg_sock, {IREG_PUT, ...})");
		else
			warnx("short write to ireg_sock: %i", res);
		return;
	}
}

static int lo_do_lookup(fuse_req_t req, fuse_ino_t parent, const char *name,
			 struct fuse_entry_param *e)
{
	int newfd;
	int res;
	int saverr;
	struct lo_data *lo = lo_data(req);
	struct lo_inode *inode, *dir = lo_inode(req, parent);

	memset(e, 0, sizeof(*e));
	e->attr_timeout = lo->timeout;
	e->entry_timeout = lo->timeout;

	newfd = openat(dir->fd, name, O_PATH | O_NOFOLLOW);
	if (newfd == -1)
		goto out_err;

	res = fstatat(newfd, "", &e->attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		goto out_err;

	inode = lo_find(lo, &e->attr);
	if (inode) {
		close(newfd);
		newfd = -1;
	} else {
		saverr = ENOMEM;
		inode = calloc(1, sizeof(struct lo_inode));
		if (!inode)
			goto out_err;

		inode->is_symlink = S_ISLNK(e->attr.st_mode);
		inode->refcount = 1;
		inode->fd = newfd;
		newfd = -1;
		inode->key.ino = e->attr.st_ino;
		inode->key.dev = e->attr.st_dev;

		get_shared(lo, inode);

		pthread_mutex_lock(&lo->mutex);
		HASH_ADD(hh, lo->inodes, key, sizeof(struct lo_key), inode);
		pthread_mutex_unlock(&lo->mutex);
	}

	e->initial_version = get_version(lo, inode);
	res = fstatat(inode->fd, "", &e->attr,
		      AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1) {
		unref_inode(lo, inode, 1);
		goto out_err;
	}

	e->ino = (uintptr_t) inode;
	e->version_offset = inode->version_offset;

	if (lo_debug(req))
		fprintf(stderr, "  %lli/%s -> %lli (version_table[%lli]=%lli)\n",
			(unsigned long long) parent, name,
			(unsigned long long) e->ino,
			(unsigned long long) e->version_offset,
			(unsigned long long) e->initial_version);

	return 0;

out_err:
	saverr = errno;
	if (newfd != -1)
		close(newfd);
	return saverr;
}

static void lo_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e;
	int err;

	if (lo_debug(req))
		fprintf(stderr, "lo_lookup(parent=%" PRIu64 ", name=%s)\n",
			parent, name);

	err = lo_do_lookup(req, parent, name, &e);
	if (err)
		fuse_reply_err(req, err);
	else
		fuse_reply_entry(req, &e);
}

/*
 * Change to uid/gid of caller so that file is created with
 * ownership of caller.
 * TODO: What about selinux context?
 */
static int lo_change_cred(fuse_req_t req, struct lo_cred *old)
{
	int res;

	old->euid = geteuid();
	old->egid = getegid();

	res = syscall(SYS_setresgid, -1,fuse_req_ctx(req)->gid, -1);
	if (res == -1)
		return errno;

	res = syscall(SYS_setresuid, -1, fuse_req_ctx(req)->uid, -1);
	if (res == -1) {
		int errno_save = errno;

		syscall(SYS_setresgid, -1, old->egid, -1);
		return errno_save;
	}

	return 0;
}

/* Regain Privileges */
static void lo_restore_cred(struct lo_cred *old)
{
	int res;

	res = syscall(SYS_setresuid, -1, old->euid, -1);
	if (res == -1)
		err(1, "seteuid(%u)", old->euid);

	res = syscall(SYS_setresgid, -1, old->egid, -1);
	if (res == -1)
		err(1, "setegid(%u)", old->egid);
}

static void lo_mknod_symlink(fuse_req_t req, fuse_ino_t parent,
			     const char *name, mode_t mode, dev_t rdev,
			     const char *link)
{
	int newfd = -1;
	int res;
	int saverr;
	struct lo_inode *inode;
	struct lo_inode *dir = lo_inode(req, parent);
	struct fuse_entry_param e;
	struct lo_cred old = {};
	struct lo_data *lo = lo_data(req);

	saverr = ENOMEM;
	inode = calloc(1, sizeof(struct lo_inode));
	if (!inode)
		goto out;

	saverr = lo_change_cred(req, &old);
	if (saverr)
		goto out;

	if (S_ISDIR(mode))
		res = mkdirat(dir->fd, name, mode);
	else if (S_ISLNK(mode))
		res = symlinkat(link, dir->fd, name);
	else
		res = mknodat(dir->fd, name, mode, rdev);
	saverr = errno;

	lo_restore_cred(&old);

	if (res == -1)
		goto out;

	update_version(lo, lo_inode(req, parent));

	saverr = lo_do_lookup(req, parent, name, &e);
	if (saverr)
		goto out;

	if (lo_debug(req))
		fprintf(stderr, "  %lli/%s -> %lli\n",
			(unsigned long long) parent, name, (unsigned long long) e.ino);

	fuse_reply_entry(req, &e);
	return;

out:
	if (newfd != -1)
		close(newfd);
	free(inode);
	fuse_reply_err(req, saverr);
}

static void lo_mknod(fuse_req_t req, fuse_ino_t parent,
		     const char *name, mode_t mode, dev_t rdev)
{
	lo_mknod_symlink(req, parent, name, mode, rdev, NULL);
}

static void lo_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
		     mode_t mode)
{
	lo_mknod_symlink(req, parent, name, S_IFDIR | mode, 0, NULL);
}

static void lo_symlink(fuse_req_t req, const char *link,
		       fuse_ino_t parent, const char *name)
{
	lo_mknod_symlink(req, parent, name, S_IFLNK, 0, link);
}

static int linkat_empty_nofollow(struct lo_data *lo, struct lo_inode *inode,
				 int dfd, const char *name)
{
	int res;
	struct lo_inode *parent;
	char path[PATH_MAX];

	if (inode->is_symlink) {
		res = linkat(inode->fd, "", dfd, name, AT_EMPTY_PATH);
		if (res == -1 && (errno == ENOENT || errno == EINVAL)) {
			/* Sorry, no race free way to hard-link a symlink. */
			if (lo->norace)
				errno = EPERM;
			else
				goto fallback;
		}
		return res;
	}

	sprintf(path, "/proc/self/fd/%i", inode->fd);

	return linkat(AT_FDCWD, path, dfd, name, AT_SYMLINK_FOLLOW);

fallback:
	res = lo_parent_and_name(lo, inode, path, &parent);
	if (res != -1)
		res = linkat(parent->fd, path, dfd, name, 0);

	return res;
}

static void lo_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t parent,
		    const char *name)
{
	int res;
	struct lo_data *lo = lo_data(req);
	struct lo_inode *inode = lo_inode(req, ino);
	struct fuse_entry_param e;
	int saverr;

	memset(&e, 0, sizeof(struct fuse_entry_param));
	e.attr_timeout = lo->timeout;
	e.entry_timeout = lo->timeout;

	res = linkat_empty_nofollow(lo, inode, lo_fd(req, parent), name);
	if (res == -1)
		goto out_err;

	res = fstatat(inode->fd, "", &e.attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		goto out_err;

	pthread_mutex_lock(&lo->mutex);
	inode->refcount++;
	pthread_mutex_unlock(&lo->mutex);
	e.ino = (uintptr_t) inode;
	update_version(lo, inode);
	update_version(lo, lo_inode(req, parent));

	if (lo_debug(req))
		fprintf(stderr, "  %lli/%s -> %lli\n",
			(unsigned long long) parent, name,
			(unsigned long long) e.ino);

	fuse_reply_entry(req, &e);
	return;

out_err:
	saverr = errno;
	fuse_reply_err(req, saverr);
}

static struct lo_inode *lookup_name(fuse_req_t req, fuse_ino_t parent,
				    const char *name)
{
	int res;
	struct stat attr;

	res = fstatat(lo_fd(req, parent), name, &attr,
		      AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return NULL;

	return lo_find(lo_data(req), &attr);
}

static void lo_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	int res;
	struct lo_inode *inode = lookup_name(req, parent, name);
	struct lo_data *lo = lo_data(req);

	if (!inode) {
		fuse_reply_err(req, EIO);
		return;
	}

	res = unlinkat(lo_fd(req, parent), name, AT_REMOVEDIR);
	if (res == -1) {
		fuse_reply_err(req, errno);
	} else {
		update_version(lo, inode);
		update_version(lo, lo_inode(req, parent));
		fuse_reply_err(req, 0);
	}
	unref_inode(lo, inode, 1);
}

static void lo_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
		      fuse_ino_t newparent, const char *newname,
		      unsigned int flags)
{
	int res;
	struct lo_inode *oldinode = lookup_name(req, parent, name);
	struct lo_inode *newinode = lookup_name(req, newparent, newname);
	struct lo_data *lo = lo_data(req);

	if (!oldinode) {
		fuse_reply_err(req, EIO);
		goto out;
	}


	if (flags) {
#ifndef SYS_renameat2
		fuse_reply_err(req, EINVAL);
#else
		res = syscall(SYS_renameat2, lo_fd(req, parent), name,
			      lo_fd(req, newparent), newname, flags);
		if (res == -1 && errno == ENOSYS)
			fuse_reply_err(req, EINVAL);
		else
			fuse_reply_err(req, res == -1 ? errno : 0);
#endif
		goto out;
	}

	res = renameat(lo_fd(req, parent), name,
			lo_fd(req, newparent), newname);
	if (res == -1) {
		fuse_reply_err(req, errno);
	} else {
		update_version(lo, oldinode);
		if (newinode)
			update_version(lo, newinode);
		update_version(lo, lo_inode(req, parent));
		update_version(lo, lo_inode(req, newparent));
		fuse_reply_err(req, 0);
	}
out:
	unref_inode(lo, oldinode, 1);
	unref_inode(lo, newinode, 1);
}

static void lo_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	int res;
	struct lo_inode *inode = lookup_name(req, parent, name);
	struct lo_data *lo = lo_data(req);

	if (!inode) {
		fuse_reply_err(req, EIO);
		return;
	}

	res = unlinkat(lo_fd(req, parent), name, 0);
	if (res == -1) {
		fuse_reply_err(req, errno);
	} else {
		update_version(lo, inode);
		update_version(lo, lo_inode(req, parent));
		fuse_reply_err(req, 0);
	}
	unref_inode(lo, inode, 1);
}

static void unref_inode(struct lo_data *lo, struct lo_inode *inode, uint64_t n)
{
	if (!inode)
		return;

	pthread_mutex_lock(&lo->mutex);
	assert(inode->refcount >= n);
	inode->refcount -= n;
	if (!inode->refcount) {
		HASH_DEL(lo->inodes, inode);
		pthread_mutex_unlock(&lo->mutex);
		close(inode->fd);
		put_shared(lo, inode);
		free(inode);

	} else {
		pthread_mutex_unlock(&lo->mutex);
	}
}

static void lo_forget_one(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
	struct lo_data *lo = lo_data(req);
	struct lo_inode *inode = lo_inode(req, ino);

	if (lo_debug(req)) {
		fprintf(stderr, "  forget %lli %lli -%lli\n",
			(unsigned long long) ino,
			(unsigned long long) inode->refcount,
			(unsigned long long) nlookup);
	}

	unref_inode(lo, inode, nlookup);
}

static void lo_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
	lo_forget_one(req, ino, nlookup);
	fuse_reply_none(req);
}

static void lo_forget_multi(fuse_req_t req, size_t count,
				struct fuse_forget_data *forgets)
{
	int i;

	for (i = 0; i < count; i++)
		lo_forget_one(req, forgets[i].ino, forgets[i].nlookup);
	fuse_reply_none(req);
}

static void lo_readlink(fuse_req_t req, fuse_ino_t ino)
{
	char buf[PATH_MAX + 1];
	int res;

	res = readlinkat(lo_fd(req, ino), "", buf, sizeof(buf));
	if (res == -1)
		return (void) fuse_reply_err(req, errno);

	if (res == sizeof(buf))
		return (void) fuse_reply_err(req, ENAMETOOLONG);

	buf[res] = '\0';

	fuse_reply_readlink(req, buf);
}

struct lo_dirp {
	int fd;
	DIR *dp;
	struct dirent *entry;
	off_t offset;
};

static struct lo_dirp *lo_dirp(struct fuse_file_info *fi)
{
	return (struct lo_dirp *) (uintptr_t) fi->fh;
}

static void lo_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int error = ENOMEM;
	struct lo_data *lo = lo_data(req);
	struct lo_dirp *d = calloc(1, sizeof(struct lo_dirp));
	if (d == NULL)
		goto out_err;

	d->fd = openat(lo_fd(req, ino), ".", O_RDONLY);
	if (d->fd == -1)
		goto out_errno;

	d->dp = fdopendir(d->fd);
	if (d->dp == NULL)
		goto out_errno;

	d->offset = 0;
	d->entry = NULL;

	fi->fh = (uintptr_t) d;
	if (lo->cache == CACHE_ALWAYS)
		fi->keep_cache = 1;
	fuse_reply_open(req, fi);
	return;

out_errno:
	error = errno;
out_err:
	if (d) {
		if (d->fd != -1)
			close(d->fd);
		free(d);
	}
	fuse_reply_err(req, error);
}

static int is_dot_or_dotdot(const char *name)
{
	return name[0] == '.' && (name[1] == '\0' ||
				  (name[1] == '.' && name[2] == '\0'));
}

static void lo_do_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
			  off_t offset, struct fuse_file_info *fi, int plus)
{
	struct lo_dirp *d = lo_dirp(fi);
	char *buf;
	char *p;
	size_t rem = size;
	int err;

	(void) ino;

	buf = calloc(1, size);
	if (!buf) {
		err = ENOMEM;
		goto error;
	}
	p = buf;

	if (offset != d->offset) {
		seekdir(d->dp, offset);
		d->entry = NULL;
		d->offset = offset;
	}
	while (1) {
		size_t entsize;
		off_t nextoff;
		const char *name;

		if (!d->entry) {
			errno = 0;
			d->entry = readdir(d->dp);
			if (!d->entry) {
				if (errno) {  // Error
					err = errno;
					goto error;
				} else {  // End of stream
					break; 
				}
			}
		}
		nextoff = d->entry->d_off;
		name = d->entry->d_name;
		fuse_ino_t entry_ino = 0;
		if (plus) {
			struct fuse_entry_param e;
			if (is_dot_or_dotdot(name)) {
				e = (struct fuse_entry_param) {
					.attr.st_ino = d->entry->d_ino,
					.attr.st_mode = d->entry->d_type << 12,
				};
			} else {
				err = lo_do_lookup(req, ino, name, &e);
				if (err)
					goto error;
				entry_ino = e.ino;
			}

			entsize = fuse_add_direntry_plus(req, p, rem, name,
							 &e, nextoff);
		} else {
			struct stat st = {
				.st_ino = d->entry->d_ino,
				.st_mode = d->entry->d_type << 12,
			};
			entsize = fuse_add_direntry(req, p, rem, name,
						    &st, nextoff);
		}
		if (entsize > rem) {
			if (entry_ino != 0) 
				lo_forget_one(req, entry_ino, 1);
			break;
		}
		
		p += entsize;
		rem -= entsize;

		d->entry = NULL;
		d->offset = nextoff;
	}

    err = 0;
error:
    // If there's an error, we can only signal it if we haven't stored
    // any entries yet - otherwise we'd end up with wrong lookup
    // counts for the entries that are already in the buffer. So we
    // return what we've collected until that point.
    if (err && rem == size)
	    fuse_reply_err(req, err);
    else
	    fuse_reply_buf(req, buf, size - rem);
    free(buf);
}

static void lo_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
		       off_t offset, struct fuse_file_info *fi)
{
	lo_do_readdir(req, ino, size, offset, fi, 0);
}

static void lo_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
			   off_t offset, struct fuse_file_info *fi)
{
	lo_do_readdir(req, ino, size, offset, fi, 1);
}

static void lo_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct lo_dirp *d = lo_dirp(fi);
	(void) ino;
	closedir(d->dp);
	free(d);
	fuse_reply_err(req, 0);
}

static void lo_create(fuse_req_t req, fuse_ino_t parent, const char *name,
		      mode_t mode, struct fuse_file_info *fi)
{
	int fd;
	struct fuse_entry_param e;
	int err;
	struct lo_cred old = {};
	struct lo_data *lo = lo_data(req);

	if (lo_debug(req))
		fprintf(stderr, "lo_create(parent=%" PRIu64 ", name=%s)\n",
			parent, name);

	err = lo_change_cred(req, &old);
	if (err)
		goto out;

	/* Promote O_WRONLY to O_RDWR. Otherwise later mmap(PROT_WRITE) fails */
	if ((fi->flags & O_ACCMODE) == O_WRONLY) {
		fi->flags &= ~O_ACCMODE;
		fi->flags |= O_RDWR;
	}

	fd = openat(lo_fd(req, parent), name,
		    (fi->flags | O_CREAT) & ~O_NOFOLLOW, mode);
	err = fd == -1 ? errno : 0;
	lo_restore_cred(&old);

	if (!err) {
		update_version(lo, lo_inode(req, parent));

		fi->fh = fd;
		err = lo_do_lookup(req, parent, name, &e);
	}

out:
	if (err)
		fuse_reply_err(req, err);
	else
		fuse_reply_create(req, &e, fi);
}

static void lo_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
			struct fuse_file_info *fi)
{
	int res;
	int fd = dirfd(lo_dirp(fi)->dp);
	(void) ino;
	if (datasync)
		res = fdatasync(fd);
	else
		res = fsync(fd);
	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int fd;
	char buf[64];
	struct lo_data *lo = lo_data(req);

	if (lo_debug(req))
		fprintf(stderr, "lo_open(ino=%" PRIu64 ", flags=%d)\n",
			ino, fi->flags);

	/* Promote O_WRONLY to O_RDWR. Otherwise later mmap(PROT_WRITE) fails */
	if ((fi->flags & O_ACCMODE) == O_WRONLY) {
		fi->flags &= ~O_ACCMODE;
		fi->flags |= O_RDWR;
	}

	/* With writeback cache, kernel may send read requests even
	   when userspace opened write-only */
	if (lo->writeback && (fi->flags & O_ACCMODE) == O_WRONLY) {
		fi->flags &= ~O_ACCMODE;
		fi->flags |= O_RDWR;
	}

	/* With writeback cache, O_APPEND is handled by the kernel.
	   This breaks atomicity (since the file may change in the
	   underlying filesystem, so that the kernel's idea of the
	   end of the file isn't accurate anymore). In this example,
	   we just accept that. A more rigorous filesystem may want
	   to return an error here */
	if (lo->writeback && (fi->flags & O_APPEND))
		fi->flags &= ~O_APPEND;

	sprintf(buf, "/proc/self/fd/%i", lo_fd(req, ino));
	fd = open(buf, fi->flags & ~O_NOFOLLOW);
	if (fd == -1)
		return (void) fuse_reply_err(req, errno);

	fi->fh = fd;
	if (lo->cache == CACHE_NONE)
		fi->direct_io = 1;
	else if (lo->cache == CACHE_ALWAYS)
		fi->keep_cache = 1;
	fuse_reply_open(req, fi);
}

static void lo_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	(void) ino;

	close(fi->fh);
	fuse_reply_err(req, 0);
}

static void lo_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int res;
	(void) ino;
	res = close(dup(fi->fh));
	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
		     struct fuse_file_info *fi)
{
	int res;
	(void) ino;
	int fd;
	char *buf;

	if (lo_debug(req))
		fprintf(stderr, "lo_fsync(ino=%" PRIu64 ", fi=0x%p)\n", ino,
				(void *)fi);

	if (!fi) {
		res = asprintf(&buf, "/proc/self/fd/%i", lo_fd(req, ino));
		if (res == -1)
			return (void) fuse_reply_err(req, errno);

		fd = open(buf, O_RDWR);
		free(buf);
		if (fd == -1)
			return (void) fuse_reply_err(req, errno);
	} else
		fd = fi->fh;

	if (datasync)
		res = fdatasync(fd);
	else
		res = fsync(fd);
	if (!fi)
		close(fd);
	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_read(fuse_req_t req, fuse_ino_t ino, size_t size,
		    off_t offset, struct fuse_file_info *fi)
{
	struct fuse_bufvec buf = FUSE_BUFVEC_INIT(size);

	if (lo_debug(req))
		fprintf(stderr, "lo_read(ino=%" PRIu64 ", size=%zd, "
			"off=%lu)\n", ino, size, (unsigned long) offset);

	buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	buf.buf[0].fd = fi->fh;
	buf.buf[0].pos = offset;

	fuse_reply_data(req, &buf, FUSE_BUF_SPLICE_MOVE);
}

static void lo_write_buf(fuse_req_t req, fuse_ino_t ino,
			 struct fuse_bufvec *in_buf, off_t off,
			 struct fuse_file_info *fi)
{
	(void) ino;
	ssize_t res;
	struct fuse_bufvec out_buf = FUSE_BUFVEC_INIT(fuse_buf_size(in_buf));
	struct lo_data *lo = lo_data(req);

	out_buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	out_buf.buf[0].fd = fi->fh;
	out_buf.buf[0].pos = off;

	if (lo_debug(req))
		fprintf(stderr, "lo_write(ino=%" PRIu64 ", size=%zd, off=%lu)\n",
			ino, out_buf.buf[0].size, (unsigned long) off);

	res = fuse_buf_copy(&out_buf, in_buf, 0);
	if(res < 0) {
		fuse_reply_err(req, -res);
	} else {
		update_version(lo, lo_inode(req, ino));
		fuse_reply_write(req, (size_t) res);
	}
}

static void lo_statfs(fuse_req_t req, fuse_ino_t ino)
{
	int res;
	struct statvfs stbuf;

	res = fstatvfs(lo_fd(req, ino), &stbuf);
	if (res == -1)
		fuse_reply_err(req, errno);
	else
		fuse_reply_statfs(req, &stbuf);
}

static void lo_fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
			 off_t offset, off_t length, struct fuse_file_info *fi)
{
	int err;
	struct lo_data *lo = lo_data(req);

	(void) ino;

	if (mode) {
		fuse_reply_err(req, EOPNOTSUPP);
		return;
	}

	err = posix_fallocate(fi->fh, offset, length);
	if (!err)
		update_version(lo, lo_inode(req, ino));

	fuse_reply_err(req, err);
}

static void lo_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
		     int op)
{
	int res;
	(void) ino;

	res = flock(fi->fh, op);

	fuse_reply_err(req, res == -1 ? errno : 0);
}

static void lo_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
			size_t size)
{
	char *value = NULL;
	char procname[64];
	struct lo_inode *inode = lo_inode(req, ino);
	ssize_t ret;
	int saverr;

	saverr = ENOSYS;
	if (!lo_data(req)->xattr)
		goto out;

	if (lo_debug(req)) {
		fprintf(stderr, "lo_getxattr(ino=%" PRIu64 ", name=%s size=%zd)\n",
			ino, name, size);
	}

	if (inode->is_symlink) {
		/* Sorry, no race free way to getxattr on symlink. */
		saverr = EPERM;
		goto out;
	}

	sprintf(procname, "/proc/self/fd/%i", inode->fd);

	if (size) {
		value = malloc(size);
		if (!value)
			goto out_err;

		ret = getxattr(procname, name, value, size);
		if (ret == -1)
			goto out_err;
		saverr = 0;
		if (ret == 0)
			goto out;

		fuse_reply_buf(req, value, ret);
	} else {
		ret = getxattr(procname, name, NULL, 0);
		if (ret == -1)
			goto out_err;

		fuse_reply_xattr(req, ret);
	}
out_free:
	free(value);
	return;

out_err:
	saverr = errno;
out:
	fuse_reply_err(req, saverr);
	goto out_free;
}

static void lo_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
	char *value = NULL;
	char procname[64];
	struct lo_inode *inode = lo_inode(req, ino);
	ssize_t ret;
	int saverr;

	saverr = ENOSYS;
	if (!lo_data(req)->xattr)
		goto out;

	if (lo_debug(req)) {
		fprintf(stderr, "lo_listxattr(ino=%" PRIu64 ", size=%zd)\n",
			ino, size);
	}

	if (inode->is_symlink) {
		/* Sorry, no race free way to listxattr on symlink. */
		saverr = EPERM;
		goto out;
	}

	sprintf(procname, "/proc/self/fd/%i", inode->fd);

	if (size) {
		value = malloc(size);
		if (!value)
			goto out_err;

		ret = listxattr(procname, value, size);
		if (ret == -1)
			goto out_err;
		saverr = 0;
		if (ret == 0)
			goto out;

		fuse_reply_buf(req, value, ret);
	} else {
		ret = listxattr(procname, NULL, 0);
		if (ret == -1)
			goto out_err;

		fuse_reply_xattr(req, ret);
	}
out_free:
	free(value);
	return;

out_err:
	saverr = errno;
out:
	fuse_reply_err(req, saverr);
	goto out_free;
}

static void lo_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
			const char *value, size_t size, int flags)
{
	char procname[64];
	struct lo_data *lo = lo_data(req);
	struct lo_inode *inode = lo_inode(req, ino);
	ssize_t ret;
	int saverr;

	saverr = ENOSYS;
	if (!lo_data(req)->xattr)
		goto out;

	if (lo_debug(req)) {
		fprintf(stderr, "lo_setxattr(ino=%" PRIu64 ", name=%s value=%s size=%zd)\n",
			ino, name, value, size);
	}

	if (inode->is_symlink) {
		/* Sorry, no race free way to removexattr on symlink. */
		saverr = EPERM;
		goto out;
	}

	sprintf(procname, "/proc/self/fd/%i", inode->fd);

	ret = setxattr(procname, name, value, size, flags);
	saverr = ret == -1 ? errno : 0;

	if (!saverr)
		update_version(lo, inode);
out:
	fuse_reply_err(req, saverr);
}

static void lo_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name)
{
	char procname[64];
	struct lo_data *lo = lo_data(req);
	struct lo_inode *inode = lo_inode(req, ino);
	ssize_t ret;
	int saverr;

	saverr = ENOSYS;
	if (!lo_data(req)->xattr)
		goto out;

	if (lo_debug(req)) {
		fprintf(stderr, "lo_removexattr(ino=%" PRIu64 ", name=%s)\n",
			ino, name);
	}

	if (inode->is_symlink) {
		/* Sorry, no race free way to setxattr on symlink. */
		saverr = EPERM;
		goto out;
	}

	sprintf(procname, "/proc/self/fd/%i", inode->fd);

	ret = removexattr(procname, name);
	saverr = ret == -1 ? errno : 0;

	if (!saverr)
		update_version(lo, inode);
out:
	fuse_reply_err(req, saverr);
}

#ifdef HAVE_COPY_FILE_RANGE
static void lo_copy_file_range(fuse_req_t req, fuse_ino_t ino_in, off_t off_in,
			       struct fuse_file_info *fi_in,
			       fuse_ino_t ino_out, off_t off_out,
			       struct fuse_file_info *fi_out, size_t len,
			       int flags)
{
	ssize_t res;

	if (lo_debug(req))
		fprintf(stderr, "lo_copy_file_range(ino=%" PRIu64 "/fd=%lu, "
				"off=%lu, ino=%" PRIu64 "/fd=%lu, "
				"off=%lu, size=%zd, flags=0x%x)\n",
			ino_in, fi_in->fh, off_in, ino_out, fi_out->fh, off_out,
			len, flags);

	res = copy_file_range(fi_in->fh, &off_in, fi_out->fh, &off_out, len,
			      flags);
	if (res < 0)
		fuse_reply_err(req, -errno);
	else
		fuse_reply_write(req, res);
}
#endif

static void lo_setupmapping(fuse_req_t req, fuse_ino_t ino, uint64_t foffset,
                            uint64_t len, uint64_t moffset, uint64_t flags,
                            struct fuse_file_info *fi)
{
        int ret = 0, fd, res;
        VhostUserFSSlaveMsg msg = { 0 };
        uint64_t vhu_flags;
	char *buf;

	if (lo_debug(req))
		fprintf(stderr, "lo_setupmapping(ino=%" PRIu64 ", fi=0x%p)\n", ino, (void *)fi);

        vhu_flags = VHOST_USER_FS_FLAG_MAP_R;
        if (flags & O_WRONLY) {
                vhu_flags |= VHOST_USER_FS_FLAG_MAP_W;
        }

	msg.fd_offset[0] = foffset;
	msg.len[0] = len;
	msg.c_offset[0] = moffset;
	msg.flags[0] = vhu_flags;

	if (fi)
		fd = fi->fh;
	else {
		res = asprintf(&buf, "/proc/self/fd/%i", lo_fd(req, ino));
		if (res == -1)
			return (void) fuse_reply_err(req, errno);

		/*
		 * TODO: O_RDWR might not be allowed if file is read only or
		 * write only. Fix it.
		 */
		fd = open(buf, O_RDWR);
		free(buf);
		if (fd == -1)
			return (void) fuse_reply_err(req, errno);

	}

        if (fuse_virtio_map(req, &msg, fd)) {
                fprintf(stderr, "%s: map over virtio failed (ino=%" PRId64 "fd=%d moffset=0x%" PRIx64 ")\n",
                        __func__, ino, fi ? (int)fi->fh : lo_fd(req, ino), moffset);
                ret = EINVAL;
        }

	if (!fi)
		close(fd);
	fuse_reply_err(req, ret);
}

static void lo_removemapping(fuse_req_t req, fuse_ino_t ino, uint64_t moffset,
                             uint64_t len, struct fuse_file_info *fi)
{
        VhostUserFSSlaveMsg msg = { 0 };
	int ret = 0;

	msg.len[0] = len;
	msg.c_offset[0] = moffset;
        if (fuse_virtio_unmap(req, &msg)) {
                fprintf(stderr, "%s: unmap over virtio failed (offset=0x%lx, len=0x%lx)\n", __func__, moffset, len);
                ret = EINVAL;
        }

	fuse_reply_err(req, ret);
}

static struct fuse_lowlevel_ops lo_oper = {
	.init		= lo_init,
	.lookup		= lo_lookup,
	.mkdir		= lo_mkdir,
	.mknod		= lo_mknod,
	.symlink	= lo_symlink,
	.link		= lo_link,
	.unlink		= lo_unlink,
	.rmdir		= lo_rmdir,
	.rename		= lo_rename,
	.forget		= lo_forget,
	.forget_multi	= lo_forget_multi,
	.getattr	= lo_getattr,
	.setattr	= lo_setattr,
	.readlink	= lo_readlink,
	.opendir	= lo_opendir,
	.readdir	= lo_readdir,
	.readdirplus	= lo_readdirplus,
	.releasedir	= lo_releasedir,
	.fsyncdir	= lo_fsyncdir,
	.create		= lo_create,
	.open		= lo_open,
	.release	= lo_release,
	.flush		= lo_flush,
	.fsync		= lo_fsync,
	.read		= lo_read,
	.write_buf      = lo_write_buf,
	.statfs		= lo_statfs,
	.fallocate	= lo_fallocate,
	.flock		= lo_flock,
	.getxattr	= lo_getxattr,
	.listxattr	= lo_listxattr,
	.setxattr	= lo_setxattr,
	.removexattr	= lo_removexattr,
#ifdef HAVE_COPY_FILE_RANGE
	.copy_file_range = lo_copy_file_range,
#endif
        .setupmapping   = lo_setupmapping,
        .removemapping  = lo_removemapping,
};

static void *ireg_do(void *data)
{
	struct lo_data *lo = data;
	int res;
	char buf[100];
	struct srv_msg reply;
	struct msgreply *rep;

	for (;;) {
		res = read(lo->ireg_sock, buf, sizeof(buf));
		if (res <= 0) {
			if (res == -1)
				warn("read(lo->ireg_sock, ...)");
			else
				warnx("disconnected from ireg");
			return NULL;
		}
		if (res != sizeof(reply)) {
			warnx("bad size message: %i", res);
			continue;
		}

		memcpy(&reply, buf, sizeof(reply));
		if (reply.op != SRV_VERSION) {
			warn("bad reply to IREG_GET: %i", reply.op);
			continue;
		}

		rep = (struct msgreply *) reply.handle;
		rep->inode->version_offset = reply.version.offset;
		rep->inode->ireg_refid = reply.version.refid;
		sem_post(&rep->ready);
	}
}

static void setup_shared_versions(struct lo_data *lo)
{
	int fd, sock, res;
	const char *version_path = "/dev/shm/fuse_shared_versions";
	struct stat stat;
	struct sockaddr_un name = { .sun_family = AF_UNIX };
	const char *socket_name = "/tmp/ireg.sock";
	void *addr;

	lo->ireg_sock = -1;
	if (!lo->shared)
		return;

	sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sock == -1)
		err(1, "socket(AF_UNIX, SOCK_SEQPACKET, 0)");

	strncpy(name.sun_path, socket_name, sizeof(name.sun_path) - 1);

	res = connect(sock, (const struct sockaddr *) &name,
		      sizeof(struct sockaddr_un));
	if (res == -1) {
		warn("connect to ireg");
		close(sock);
		lo->ireg_sock = -1;
		return;
	}

	lo->ireg_sock = sock;

	fd = open(version_path, O_RDWR);
	if (sock == -1)
		err(1, "open(%s, O_RDWR)", version_path);

	res = fstat(fd, &stat);
	if (res == -1)
		err(1, "fstat(%i, &stat)", fd);

	lo->version_table_size = stat.st_size / sizeof(lo->version_table[0]);

	addr = mmap(NULL, stat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED)
		err(1, "mmap(NULL, %li, PROT_READ | PROT_WRITE, MAP_SHARED, %i, 0", stat.st_size, fd);

	lo->version_table = addr;
}

static void setup_root(struct lo_data *lo, struct lo_inode *root)
{
	int fd, res;
	struct stat stat;

	fd = open(lo->source, O_PATH);
	if (fd == -1)
		err(1, "open(%s, O_PATH)", lo->source);

	res = fstatat(fd, "", &stat, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		err(1, "fstatat(%s)", lo->source);

	root->fd = fd;
	root->key.ino = stat.st_ino;
	root->key.dev = stat.st_dev;
	root->refcount = 2;
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_session *se;
	struct fuse_cmdline_opts opts;
	struct lo_data lo = { .debug = 0,
	                      .writeback = 0 };
	int ret = -1;

	/* Don't mask creation mode, kernel already did that */
	umask(0);

	pthread_mutex_init(&lo.mutex, NULL);
	lo.inodes = NULL;
	lo.root.fd = -1;
	lo.cache = CACHE_AUTO;

	if (fuse_parse_cmdline(&args, &opts) != 0)
		return 1;
	if (opts.show_help) {
		printf("usage: %s [options] <mountpoint>\n\n", argv[0]);
		fuse_cmdline_help();
		fuse_lowlevel_help();
		ret = 0;
		goto err_out1;
	} else if (opts.show_version) {
		printf("FUSE library version %s\n", fuse_pkgversion());
		fuse_lowlevel_version();
		ret = 0;
		goto err_out1;
	}

	if(opts.mountpoint == NULL) {
		printf("usage: %s [options] <mountpoint>\n", argv[0]);
		printf("       %s --help\n", argv[0]);
		ret = 1;
		goto err_out1;
	}

	if (fuse_opt_parse(&args, &lo, lo_opts, NULL)== -1)
		return 1;

	lo.debug = opts.debug;
	if (lo.source) {
		struct stat stat;
		int res;

		res = lstat(lo.source, &stat);
		if (res == -1)
			err(1, "failed to stat source (\"%s\")", lo.source);
		if (!S_ISDIR(stat.st_mode))
			errx(1, "source is not a directory");

	} else {
		lo.source = "/";
	}
	lo.root.is_symlink = false;
	if (!lo.timeout_set) {
		switch (lo.cache) {
		case CACHE_NONE:
			lo.timeout = 0.0;
			break;

		case CACHE_AUTO:
			lo.timeout = 1.0;
			break;

		case CACHE_ALWAYS:
			lo.timeout = 86400.0;
			break;
		}
	} else if (lo.timeout < 0) {
		errx(1, "timeout is negative (%lf)", lo.timeout);
	}

	setup_shared_versions(&lo);

	setup_root(&lo, &lo.root);

	se = fuse_session_new(&args, &lo_oper, sizeof(lo_oper), &lo);
	if (se == NULL)
	    goto err_out1;

	if (fuse_set_signal_handlers(se) != 0)
	    goto err_out2;

	if (fuse_session_mount(se, opts.mountpoint) != 0)
	    goto err_out3;

	fuse_daemonize(opts.foreground);

	if (lo.ireg_sock != -1) {
		pthread_t ireg_thread;

		ret = pthread_create(&ireg_thread, NULL, ireg_do, &lo);
		if (ret) {
			warnx("pthread_create: %s", strerror(ret));
			ret = 1;
			goto err_out4;
		}

		get_shared(&lo, &lo.root);
	}

	/* Block until ctrl+c or fusermount -u */
	if (opts.singlethread)
		ret = fuse_session_loop(se);
	else
		ret = fuse_session_loop_mt(se, opts.clone_fd);

err_out4:
	fuse_session_unmount(se);
err_out3:
	fuse_remove_signal_handlers(se);
err_out2:
	fuse_session_destroy(se);
err_out1:
	free(opts.mountpoint);
	fuse_opt_free_args(&args);

	if (lo.root.fd >= 0)
		close(lo.root.fd);

	return ret ? 1 : 0;
}
