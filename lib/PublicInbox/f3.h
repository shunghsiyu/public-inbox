/* Copyright (C) all contributors <meta@public-inbox.org> */
/* License: AGPL-3.0+ <https://www.gnu.org/licenses/agpl-3.0.txt> */
/*
 * "lei.fuse" shim for use via "lei mount -d $DOMAIN MOINTPOINT)
 * This is built Just-Ahead-Of-Time by lib/PublicInbox/LeiF3.pm
 * It communicates over local SOCK_SEQPACKET sockets with worker processes
 * running lib/PublicInbox/LeiVf.pm
 */

/* another project may use this: */
#define F3_NS "lei"

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE_SOURCE 64
#define _REENTRANT
#define _POSIX_C_SOURCE 200809L
#define _LGPL_SOURCE /* allow URCU to inline some stuff */
#include <urcu-bp.h>
#include <urcu/rculfhash.h>
#include <urcu/uatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#define FUSE_USE_VERSION 35
#include <fuse_lowlevel.h>
/* TODO: lockless allocator support */
_Static_assert(sizeof(fuse_ino_t) >= sizeof(uintptr_t), "fuse_ino_t too small");

#ifndef F3_TRACE_REQ
#  define F3_TRACE_REQ 0
#endif

enum f3_omode { F3_RDONLY = 0, F3_RDWR = 2, F3_DIRTY = 3 };

/* both XATTR_*_MAX values are actually 65536 */
#if XATTR_SIZE_MAX > XATTR_LIST_MAX
#  define F3_XATTR_MAX XATTR_SIZE_MAX
#else
#  define F3_XATTR_MAX XATTR_LIST_MAX
#endif

struct f3_inode {
	int64_t vid;
	uint64_t refcount;
	struct cds_lfht_node nd;
	struct rcu_head rh;
	int fd; /* stable for struct lifetime once set >= 0 */
	enum f3_omode rw; /* F3_RDONLY, F3_RDWR, F3_DIRTY */
};

struct f3_data {
	struct f3_inode vroot;
	struct cds_lfht *vid2inode;
	double entry_timeout;
	double attr_timeout;
	unsigned keep_cache:1;
	unsigned direct_io:1;
	unsigned cache_readdir:1;
	int rfd;
	int wfd;
};

struct f3_attr_res {
	int err;
	int pad_;
	struct stat sb;
};

struct f3_rm_res {
	int err;
	int pad_;
	int64_t dead_vid;
};

struct f3_xattr_res {
	int err;
	int pad_;
	uint32_t len;
	char buf[F3_XATTR_MAX];
};

struct f3_req_res {
	FILE *wfp;
	int sock_fd; /* f3.rfd or f3.wfd */
	int send_fd; /* for write requests only */
	int sk[2];
	char wbuf[F3_XATTR_MAX + NAME_MAX];
};

union padded_mutex {
	pthread_mutex_t mtx;
	char pad[64]; /* cache alignment for common CPUs */
};

/*
 * this is off-stack since destructors and call_rcu may be firing during
 * abort and we'd have already left main()
 */
static struct f3_data f3 = {
	.rfd = -1,
	.wfd = -1,
	.attr_timeout = 86400, /* screws up kernel jiffies calc if too high */
	.entry_timeout = 86400, /* screws up kernel jiffies calc if too high */
	.keep_cache = 1,
	.cache_readdir = 1,
	.vroot = { .refcount = 42, /* never freed */ .fd = -1 }
};

static const struct fuse_opt f3_opt[] = {
	/* *-fd and root-vid are internal knobs */
	{ "reader-fd=%d", offsetof(struct f3_data, rfd) },
	{ "worker-fd=%d", offsetof(struct f3_data, wfd) },
	{ "root-vid=%"PRId64, offsetof(struct f3_data, vroot.vid) },
	FUSE_OPT_END
};

/* a pool of mutexes for all "struct f3_inode" */
#define MUTEX_NR   (1 << 6)
#define MUTEX_MASK (MUTEX_NR - 1)
static union padded_mutex mutexes[MUTEX_NR] = {
	[0 ... (MUTEX_NR-1)].mtx = PTHREAD_MUTEX_INITIALIZER
};

static struct f3_inode *f3_inode(fuse_ino_t ino)
{
	return ino == FUSE_ROOT_ID ? &f3.vroot :
			(struct f3_inode *)(uintptr_t)ino;
}

static int64_t f3_vid(fuse_ino_t ino)
{
	return f3_inode(ino)->vid;
}

static void f3_init(void *userdata, struct fuse_conn_info *c)
{
	int e = 0;
	struct rlimit r;

	c->time_gran = 1000000; /* millisecond */

#define force(f) do { \
	if (c->capable & f) c->want |= f; \
	else { ++e; warnx(#f " missing"); } \
} while (0)
	/*
	 * No FUSE_CAP_EXPORT_SUPPORT for now, I don't imagine ever using NFS
	 * again with sshfs, nowadays.
	 * FUSE_CAP_POSIX_ACL requires way more effort to support,
	 * probably not worth it
	 */
	force(FUSE_CAP_WRITEBACK_CACHE);
	force(FUSE_CAP_READDIRPLUS);
#ifdef FUSE_CAP_CACHE_SYMLINKS
	force(FUSE_CAP_CACHE_SYMLINKS);
#endif
	force(FUSE_CAP_SPLICE_WRITE);
	force(FUSE_CAP_SPLICE_MOVE);
#undef force
	if (e) exit(1);

	if (getrlimit(RLIMIT_NOFILE, &r)) {
		warn("getrlimit(RLIMIT_NOFILE)");
	} else {
		r.rlim_cur = r.rlim_max > UINT_MAX ? UINT_MAX : r.rlim_max;
		if (setrlimit(RLIMIT_NOFILE, &r) < 0)
			warn("setrlimit(RLIMIT_NOFILE)");
	}

	/* the auto-resize thread must be started after daemonization */
	f3.vid2inode = cds_lfht_new(1024, 2, 0,
				CDS_LFHT_AUTO_RESIZE|CDS_LFHT_ACCOUNTING, 0);
	if (!f3.vid2inode)
		errx(1, "cds_lfht_new failed");
}

static void wclose(int *err, int fd) /* for regular files */
{
	if (close(fd) == 0 || errno == EINTR) return;
	if (errno == EBADF) {
		warn("BUG: close");
		abort();
	}
	if (err && !*err)
		*err = errno;
	warn("close");
}

static void xclose(int fd) /* for sockets */
{
	wclose(NULL, fd);
}

static int sendmsg_sleep_wait(unsigned *tries)
{
	const struct timespec req = { 0, 100000000 }; /* 100ms */
	switch (errno) {
	case EINTR:
		return 1;
	case ENOBUFS: case ENOMEM: case ETOOMANYREFS:
		if (++*tries < 50) {
			warnx("ugh, sleeping on sendmsg: %m (#%u)", *tries);
			nanosleep(&req, NULL);
			return 1;
		}
		/* fall-through */
	default:
		warn("sendmsg");
		return 0;
	}
}

#define SEND_FD_CAPA 2
#define SEND_FD_SPACE (SEND_FD_CAPA * sizeof(int))
union my_cmsg {
	struct cmsghdr hdr;
	char pad[sizeof(struct cmsghdr) + 16 + SEND_FD_SPACE];
};

static int send_req(struct f3_req_res *rr)
{
	ssize_t sent;
	unsigned tries = 0;
	struct iovec iov = { .iov_base = rr->wbuf };
	union my_cmsg cmsg = {
		.hdr.cmsg_level = SOL_SOCKET,
		.hdr.cmsg_type = SCM_RIGHTS,
		.hdr.cmsg_len = 0,
	};
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = 0,
		.msg_controllen = 0,
	};
	int *fdp = (int *)CMSG_DATA(&cmsg.hdr);
	int e = 0;
	long foff = ftell(rr->wfp);

	if (foff < 0)
		err(1, "ftell");

	iov.iov_len = (size_t)foff;
	if (rr->sk[1] >= 0) {
		*fdp++ = rr->sk[1];
		msg.msg_controllen += CMSG_SPACE(sizeof(int));
		cmsg.hdr.cmsg_len += CMSG_LEN(sizeof(int));
	}
	if (rr->send_fd >= 0) {
		*fdp++ = rr->send_fd;
		msg.msg_controllen += CMSG_SPACE(sizeof(int));
		cmsg.hdr.cmsg_len += CMSG_LEN(sizeof(int));
	}
	if (cmsg.hdr.cmsg_len)
		msg.msg_control = &cmsg.hdr;
	do {
		sent = sendmsg(rr->sock_fd, &msg, MSG_EOR);
	} while (sent < 0 && sendmsg_sleep_wait(&tries));
	if (sent < 0)
		e = EIO;
	if (F3_TRACE_REQ) {
		size_t n;

		for (n = 0; n < iov.iov_len; n++) {
			if (rr->wbuf[n] == '\0')
				rr->wbuf[n] = ' ';
		}
		rr->wbuf[iov.iov_len] = 0;
		warnx("req: %s", rr->wbuf);
	}
	if (rr->sk[1] >= 0)
		xclose(rr->sk[1]);
	return e;
}

static int64_t ts2ms(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * 1000LL + (int64_t)ts->tv_nsec / 1000000LL;
}

static int rr_init_(struct f3_req_res *rr, int fd)
{
	int err = 0;

	rr->send_fd = fd;
	rr->wfp = fmemopen(rr->wbuf, sizeof(rr->wbuf), "w+");
	if (!rr->wfp) {
		err = errno;
		warn("fmemopen");
	}
	return err;
}

static int ro_init(struct f3_req_res *rr, int fd)
{
	rr->sock_fd = f3.rfd;
	return rr_init_(rr, fd);
}

static int rw_init(struct f3_req_res *rr, int fd)
{
	if (f3.wfd < 0) return EROFS;
	rr->sock_fd = f3.wfd;
	return rr_init_(rr, fd);
}

static int recv_res(int err, struct f3_req_res *rr, void *rbuf, size_t *rlen)
{
	union my_cmsg cmsg;
	ssize_t r = -1;
	struct iovec iov = { .iov_base = rbuf, .iov_len = *rlen };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = &cmsg.hdr,
		.msg_controllen = CMSG_SPACE(SEND_FD_SPACE),
	};
	if (!*rlen) /* don't want a response */
		return 0;
	assert(*rlen >= sizeof(int) && "rlen too small");
	if (!err) {
		do {
			r = recvmsg(rr->sk[0], &msg, 0);
		} while (r < 0 && errno == EINTR);
		if (r < 0) {
			warn("recvmsg");
			err = EIO;
		}
	}
	xclose(rr->sk[0]);
	if (r >= sizeof(int)) {
		memcpy(&err, rbuf, sizeof(int));
		*rlen = r - sizeof(int);
	} else if (r >= 0) {
		warnx("recvmsg short read: %zd", r);
		err = EIO;
	} /* else: r < 0 already handled */
	if (r > 0 && cmsg.hdr.cmsg_level == SOL_SOCKET &&
			cmsg.hdr.cmsg_type == SCM_RIGHTS) {
		size_t len = cmsg.hdr.cmsg_len;
		int *fdp = (int *)CMSG_DATA(&cmsg.hdr);
		size_t i;

		/*
		 * n.b. rr->sk[1] is already closed after sendmsg, so we may
		 * reuse if needed
		 */
		for (i = 0; CMSG_LEN((i + 1) * sizeof(int)) <= len; i++)
			rr->sk[i] = *fdp++;
	}
	return err;
}

static int rr_send(struct f3_req_res *rr, size_t rlen)
{
	int err = 0;
	int type = SOCK_SEQPACKET|SOCK_CLOEXEC;

	if (fflush(rr->wfp) || ferror(rr->wfp)) {
		err = errno;
		warn("fflush+ferror");
	} else if (rlen) {
		if (socketpair(AF_UNIX, type, 0, rr->sk) < 0) {
			err = errno;
			warn("socketpair");

			/*
			 * ENOMEM is a valid error for many FS syscalls, while
			 * EMFILE+ENFILE are only valid for open(2) and like
			 */
			if (err == EMFILE || err == ENFILE)
				err = ENOMEM;
		}
	} else {
		rr->sk[0] = rr->sk[1] = -1;
	}
	if (!err)
		err = send_req(rr);
	if (fclose(rr->wfp))
		warn("fclose");
	return err;
}

/* closes rr->sk[0..1], rr->wfp */
static int rr_do(struct f3_req_res *rr, void *rbuf, size_t *rlen)
{
	return recv_res(rr_send(rr, *rlen), rr, rbuf, rlen);
}

static void inode_acq(const struct f3_inode *inode)
{
	pthread_mutex_t *mtx = &mutexes[inode->vid & MUTEX_MASK].mtx;
	int ret = pthread_mutex_lock(mtx);
	assert(ret == 0);
}

static void inode_rel(const struct f3_inode *inode)
{
	pthread_mutex_t *mtx = &mutexes[inode->vid & MUTEX_MASK].mtx;
	int ret = pthread_mutex_unlock(mtx);
	assert(ret == 0);
}

static void
merge_rw_inode(struct f3_attr_res *far, struct f3_inode *inode)
{
	struct stat sb;

	if (inode->rw == F3_RDONLY) return;
	if (fstat(inode->fd, &sb)) {
		far->err = errno;
		warn("fstat (BUG?)");
	} else {
		assert(S_ISREG(sb.st_mode));
		far->sb.st_size = sb.st_size;
		far->sb.st_blocks = sb.st_blocks;
	}
}

static void f3_getattr(fuse_req_t req, fuse_ino_t ino,
			struct fuse_file_info *fi)
{
	struct f3_req_res rr;
	struct f3_attr_res far = { .err = ro_init(&rr, -1) };
	size_t rlen = sizeof(far);
	struct f3_inode *inode = f3_inode(ino);

	if (!far.err) {
		fprintf(rr.wfp, "getattr%c%"PRIi64, 0, inode->vid);
		far.err = rr_do(&rr, &far, &rlen);
	}
	/* n.b. @fi is always NULL in current (3.10.x) libfuse */
	if (!far.err) {
		inode_acq(inode);
		merge_rw_inode(&far, inode);
		inode_rel(inode);
	}
	far.err ? fuse_reply_err(req, far.err)
		: fuse_reply_attr(req, &far.sb, f3.attr_timeout);
}

static int replace_fd(struct f3_inode *inode, int fd)
{
	int err = 0;
	int fl = fcntl(fd, F_GETFL, 0);

	if (fl == -1) {
		err = errno;
		warn("F_GETFL");
		abort();
		return err;
	}
	assert((fl & O_RDWR) == O_RDWR && "O_RDWR set");

	if (dup3(fd, inode->fd, O_CLOEXEC) < 0) {
		warn("dup3");
		err = EIO;
	}
	wclose(&err, fd);
	if (!err)
		inode->rw = F3_RDWR;
	return err;
}

static int upgrade_rw(fuse_req_t req, struct f3_inode *inode)
{
	/* inode must be locked */
	if (inode->rw == F3_RDONLY) { /* this doesn't touch SQLite */
		struct f3_req_res rr;
		int err = ro_init(&rr, inode->fd);

		if (!err) {
			size_t rlen = sizeof(err);
			fprintf(rr.wfp, "upgrade_rw%c%"PRId64, 0, inode->vid);
			err = rr_do(&rr, &err, &rlen);
		}
		if (!err)
			err = replace_fd(inode, rr.sk[0]);
		if (err) {
			inode_rel(inode);
			fuse_reply_err(req, err);
			return err;
		}
	}
	return 0;
}

static void f3_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *sb,
			int fl, struct fuse_file_info *fi)
{
	struct f3_inode *inode = f3_inode(ino);
	struct f3_attr_res far = { 0 };
	struct f3_req_res rr;
	size_t rlen = sizeof(far);

	far.err = rw_init(&rr, -1);
	if (far.err)
		return (void)fuse_reply_err(req, far.err);

	fprintf(rr.wfp, "setattr%c%"PRId64, 0, inode->vid);
	if (fl & FUSE_SET_ATTR_MODE)
		fprintf(rr.wfp, "%cmode=0%o", 0, sb->st_mode);
	if (fl & FUSE_SET_ATTR_UID)
		fprintf(rr.wfp, "%cuid=%u", 0, sb->st_uid);
	if (fl & FUSE_SET_ATTR_GID)
		fprintf(rr.wfp, "%cgid=%u", 0, sb->st_gid);
	if (fl & FUSE_SET_ATTR_SIZE)
		fprintf(rr.wfp, "%csize=%"PRId64, 0, (int64_t)sb->st_size);
	if (fl & FUSE_SET_ATTR_ATIME_NOW)
		fprintf(rr.wfp, "%catime=now", 0);
	else if (fl & FUSE_SET_ATTR_ATIME)
		fprintf(rr.wfp, "%catime=%"PRId64, 0, ts2ms(&sb->st_atim));
	if (fl & FUSE_SET_ATTR_MTIME_NOW)
		fprintf(rr.wfp, "%cmtime=now", 0);
	else if (fl & FUSE_SET_ATTR_MTIME)
		fprintf(rr.wfp, "%cmtime=%"PRId64, 0, ts2ms(&sb->st_mtim));
	if (fl & FUSE_SET_ATTR_CTIME)
		fprintf(rr.wfp, "%cctime=%"PRId64,
			0, ts2ms(&sb->st_ctim));

	inode_acq(inode);
	rr.send_fd = fl & FUSE_SET_ATTR_SIZE ? inode->fd : -1;
	far.err = rr_do(&rr, &far, &rlen);
	if (!far.err && rr.send_fd >= 0) {
		assert(rr.sk[0] >= 0);
		far.err = replace_fd(inode, rr.sk[0]);
		inode->rw = F3_DIRTY;
	}
	inode_rel(inode);

	far.err ? fuse_reply_err(req, far.err)
		: fuse_reply_attr(req, &far.sb, f3.attr_timeout);
}

static struct f3_inode *
inode_new(const struct stat *sb, const struct fuse_file_info *fi)
{
	struct f3_inode *inode = malloc(sizeof(*inode));

	if (!inode) {
		warn("malloc");
	} else {
		inode->vid = sb->st_ino;
		inode->refcount = 1;
		inode->rw = fi && sb->st_size == 0 ? F3_RDWR : F3_RDONLY;
		inode->fd = fi ? (int)fi->fh : -1;
		if (fi)
			assert(S_ISREG(sb->st_mode));
	}
	return inode;
}

/* Thomas Wang's 64-bit hash (TODO: evaluate hash quality) */
static unsigned long hash64shift(int64_t k)
{
	uint64_t key = (uint64_t)k;

	key = (~key) + (key << 21); // key = (key << 21) - key - 1
	key = key ^ (key >> 24);
	key = (key + (key << 3)) + (key << 8);	// key * 265
	key = key ^ (key >> 14);
	key = (key + (key << 2)) + (key << 4);	// key * 21
	key = key ^ (key >> 28);
	key = key + (key << 31);

	return (unsigned long)key;
}

/* equality function for rculfhash */
static int vid_eq(struct cds_lfht_node *nd, const void *key)
{
	const struct f3_inode *k = key;
	const struct f3_inode *cur = caa_container_of(nd, struct f3_inode, nd);
	assert(cur->refcount >= 0);

	return k->vid == cur->vid;
}

static void set_fd_once(struct f3_inode *inode, const struct stat *sb,
			struct fuse_file_info *fi)
{
	int fd = (int)fi->fh;

	assert(fd >= 0);
	/* inode must be locked */
	if (inode->fd >= 0) {
		xclose(fd); /* lost race to another thread, not an error */
		fi->fh = (uint64_t)inode->fd;
	} else {
		inode->fd = fd;
		if (sb->st_size == 0) /* zero size is always writable */
			inode->rw = F3_RDWR;
	}
}

static int ref_inode(struct fuse_entry_param *e, const struct stat *sb,
			struct fuse_file_info *fi)
{
	struct cds_lfht_node *cur;
	struct f3_inode *to_free = NULL;
	struct f3_inode *inode = inode_new(sb, fi);
	unsigned long hash = hash64shift(sb->st_ino);

	if (!inode)
		return EIO;
	rcu_read_lock(); /* for cds_lfht_* */
	cur = cds_lfht_add_unique(f3.vid2inode, hash, vid_eq,
					inode, &inode->nd);
	if (cur && cur != &inode->nd) { /* reuse existing, maybe */
		struct f3_inode *cur_inode;

		cur_inode = caa_container_of(cur, struct f3_inode, nd);
		inode_acq(cur_inode);
		if (cur_inode->refcount) {
			cur_inode->refcount++;
			if (fi)
				set_fd_once(cur_inode, sb, fi);
			to_free = inode;
			inode = cur_inode;
		}
		inode_rel(cur_inode);
		if (!to_free) /* existing entry was invalid, replace it */
			(void)cds_lfht_add_replace(f3.vid2inode, hash, vid_eq,
							inode, &inode->nd);
	}
	rcu_read_unlock();
	e->attr = *sb;
	e->ino = (uintptr_t)inode;
	e->entry_timeout = f3.entry_timeout;
	e->attr_timeout = f3.attr_timeout;

	/* reusing existing, free what we just allocated */
	free(to_free);
	return 0;
}

static int fsync_inode(struct f3_inode *inode)
{
	int err = 0;

	inode_acq(inode);
	if (inode->rw == F3_DIRTY) {
		struct f3_req_res rr;
		size_t rlen = sizeof(err);

		err = rw_init(&rr, inode->fd);
		if (!err) {
			fprintf(rr.wfp, "fsync%c%"PRId64, 0, inode->vid);
			err = rr_do(&rr, &err, &rlen);
			if (!err)
				err = replace_fd(inode, rr.sk[0]);
		}
	}
	inode_rel(inode);
	return err;
}

static void syncfs_internal(void)
{
	struct cds_lfht_iter iter;
	struct f3_inode *inode;

	rcu_read_lock();
	cds_lfht_for_each_entry(f3.vid2inode, &iter, inode, nd) {
		errno = fsync_inode(inode);
		if (errno)
			warn("syncfs_internal");
	}
	rcu_read_unlock();
}

static void f3_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e = { 0 };
	struct f3_req_res rr;
	struct f3_attr_res far = { .err = ro_init(&rr, -1) };
	size_t rlen = sizeof(far);

	if (!far.err) {
		fprintf(rr.wfp, "lookup%c%"PRId64"%c%s",
			0, f3_vid(parent), 0, name);
		far.err = rr_do(&rr, &far, &rlen);
	}
	if (!far.err)
		far.err = ref_inode(&e, &far.sb, NULL);
	far.err ? fuse_reply_err(req, far.err) : fuse_reply_entry(req, &e);
	if (parent == FUSE_ROOT_ID && !strcmp("#"F3_NS"_syncfs", name))
		syncfs_internal();
}

static void flush_rw(int64_t vid, int fd)
{
	struct f3_req_res rr;
	int err = rw_init(&rr, fd);
	size_t rlen = 0; /* don't want a response */

	if (err) {
		warnx("rw_init (flush_rw:%"PRId64")", vid);
	} else {
		fprintf(rr.wfp, "flush_rw%c%"PRId64, 0, vid);
		err = rr_do(&rr, NULL, &rlen);
		if (err)
			warnx("flush_rw:%"PRId64" failed", vid);
	}
}

static void close_and_free_inode(struct rcu_head *rh)
{
	struct f3_inode *inode = caa_container_of(rh, struct f3_inode, rh);
	int fd = inode->fd;

	if (fd < 0) {
		assert(inode->rw == F3_RDONLY);
	} else {
		if (inode->rw == F3_DIRTY)
			flush_rw(inode->vid, fd);
		xclose(fd);
	}
	free(inode);
}

static void delete_inode(struct f3_inode *inode)
{
	int ret = cds_lfht_del(f3.vid2inode, &inode->nd);

	if (ret == 0)
		call_rcu(&inode->rh, close_and_free_inode);
	else
		warnx("can't free %p, race?", inode);
}

static uint64_t unref_inode(struct f3_inode *inode, uint64_t n)
{
	int64_t rc;

	inode_acq(inode);
	rc = --inode->refcount;
	inode_rel(inode);
	if (rc) return rc;

	rcu_read_lock();
	delete_inode(inode);
	rcu_read_unlock();
	return 0;
}

static void mkfoo(fuse_req_t req, fuse_ino_t parent,
			const char *name, mode_t mode, dev_t rdev,
			const char *linktgt)
{
	struct f3_req_res rr;
	struct f3_attr_res far = { .err = rw_init(&rr, -1) };
	size_t rlen = sizeof(far);
	struct fuse_entry_param e = { 0 };
	int64_t pvid = f3_vid(parent);
	if (!far.err) {
		if (S_ISLNK(mode)) {
			fprintf(rr.wfp, "symlink%c%s%c%"PRId64"%c%s",
				0, linktgt, 0, pvid, 0, name);
		} else { /* directories, FIFO, devices */
			fprintf(rr.wfp, "mknod%c%"PRId64"%c%s%c0%o%c%"PRIu64,
				0, pvid, 0, name, 0, mode, 0, (uint64_t)rdev);
		}
	}
	if (!far.err)
		far.err = rr_do(&rr, &far, &rlen);
	if (!far.err)
		far.err = ref_inode(&e, &far.sb, NULL);
	far.err ? fuse_reply_err(req, far.err) : fuse_reply_entry(req, &e);
}

static void f3_mkdir(fuse_req_t req, fuse_ino_t parent,
			const char *name, mode_t mode)
{
	mkfoo(req, parent, name, S_IFDIR | mode, 0, NULL);
}

static void f3_symlink(fuse_req_t req, const char *linktgt,
			fuse_ino_t parent, const char *name)
{
	mkfoo(req, parent, name, S_IFLNK, 0, linktgt);
}

static void f3_readlink(fuse_req_t req, fuse_ino_t ino)
{
	const struct f3_inode *inode = f3_inode(ino);
	struct f3_req_res rr;
	struct {
		int err;
		int _pad;
		char buf[PATH_MAX + 1];
	} rlr = { .err = ro_init(&rr, -1) };
	size_t rlen = sizeof(rlr);

	if (!rlr.err) {
		fprintf(rr.wfp, "readlink%c%"PRId64, 0, inode->vid);
		rlr.err = rr_do(&rr, &rlr, &rlen);
	}
	rlr.err ? fuse_reply_err(req, rlr.err) :
		fuse_reply_readlink(req, rlr.buf);
}

static void f3_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
		       mode_t mode, dev_t rdev)
{
	mkfoo(req, parent, name, mode, rdev, NULL);
}

static void
f3_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t parent, const char *name)
{
	struct f3_inode *inode = f3_inode(ino);
	struct fuse_entry_param e = { 0 };
	struct f3_req_res rr;
	struct f3_attr_res far = { .err = rw_init(&rr, -1) };
	size_t rlen = sizeof(far);

	if (!far.err) {
		fprintf(rr.wfp, "link%c%"PRId64"%c%"PRId64"%c%s",
			0, inode->vid, 0, f3_vid(parent), 0, name);
		far.err = rr_do(&rr, &far, &rlen);
	}
	if (far.err) {
		fuse_reply_err(req, far.err);
	} else {
		uint64_t n;

		inode_acq(inode);
		n = ++inode->refcount;
		merge_rw_inode(&far, inode);
		inode_rel(inode);
		assert(n > 1);
		e.attr = far.sb;
		e.ino = (uintptr_t)inode;
		e.entry_timeout = f3.entry_timeout;
		e.attr_timeout = f3.attr_timeout;
		fuse_reply_entry(req, &e);
	}
}

static void drop_vnode(int64_t vid)
{
	struct f3_req_res rr;
	int err = rw_init(&rr, -1);
	size_t rlen = 0; /* don't want a response */

	if (!err) {
		fprintf(rr.wfp, "forget%c%"PRId64, 0, vid);
		err = rr_do(&rr, NULL, &rlen);
	}
	if (err)
		warnx("forget vid:%"PRId64" failed", vid);
}

static void drop_if_uncached(const struct f3_rm_res *rm)
{
	const struct f3_inode k = { .vid = rm->dead_vid };
	struct cds_lfht_iter iter;
	struct cds_lfht_node *cur;
	unsigned long hash = hash64shift(k.vid);

	if (k.vid <= 0)
		return;
	rcu_read_lock();
	cds_lfht_lookup(f3.vid2inode, hash, vid_eq, &k, &iter);
	cur = cds_lfht_iter_get_node(&iter);
	rcu_read_unlock();
	if (!cur)
		drop_vnode(k.vid);
}

static void
do_rm(const char *cmd, fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct f3_req_res rr;
	struct f3_rm_res rm = { .err = rw_init(&rr, -1) };
	size_t rlen = sizeof(rm);

	if (!rm.err) {
		fprintf(rr.wfp, "%s%c%"PRId64"%c%s",
			cmd, 0, f3_vid(parent), 0, name);
		rm.err = rr_do(&rr, &rm, &rlen);
	}
	fuse_reply_err(req, rm.err);
	drop_if_uncached(&rm);
}

static void f3_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	do_rm("unlink", req, parent, name);
}

static void f3_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	do_rm("rmdir", req, parent, name);
}

static void f3_rename(fuse_req_t req,
			fuse_ino_t oldparent, const char *oldname,
			fuse_ino_t newparent, const char *newname,
			unsigned int flags)
{
	struct f3_req_res rr;
	struct f3_rm_res rm = { .err = rw_init(&rr, -1) };
	size_t rlen = sizeof(rm);

	if (!rm.err) {
		fprintf(rr.wfp, "rename" "%c%"PRId64"%c%s" "%c%"PRId64"%c%s",
			0, f3_vid(oldparent), 0, oldname,
			0, f3_vid(newparent), 0, newname);
		switch (flags) {
		case RENAME_NOREPLACE: fprintf(rr.wfp, "%cNOREPLACE", 0); break;
		case RENAME_EXCHANGE: fprintf(rr.wfp, "%cEXCHANGE", 0); break;
		case 0: break;
		default: /* RENAME_WHITEOUT */
			fclose(rr.wfp);
			rm.err = EINVAL;
		}
		if (!rm.err)
			rm.err = rr_do(&rr, &rm, &rlen);
	}
	fuse_reply_err(req, rm.err);
	drop_if_uncached(&rm);
}

static void f3_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
	struct f3_inode *inode = f3_inode(ino);
	int64_t vid = inode->vid;
	uint64_t rc = unref_inode(inode, nlookup);

	assert(vid > 0);
	fuse_reply_none(req);
	if (rc == 0)
		drop_vnode(vid);
}

static void f3_forget_multi(fuse_req_t req, size_t nr,
				struct fuse_forget_data *ffd)
{
	size_t i;
	union { int64_t *vids; void *x; } tmp;
	size_t v_off = 0;
	size_t rlen = 0; /* don't want a response */
	struct f3_req_res rr;
	int err = 0;
	int rdwr[2];

	tmp.x = ffd;
	for (i = 0; i < nr; i++) {
		struct f3_inode *inode = f3_inode(ffd[i].ino);
		int64_t vid = inode->vid;
		uint64_t rc = unref_inode(inode, ffd[i].nlookup);

		if (!rc)
			tmp.vids[v_off++] = vid;
	}
	fuse_reply_none(req);
	if (!v_off)
		return;
	if (pipe2(rdwr, O_CLOEXEC)) {
		err = errno;
		warn("pipe2");
		rdwr[0] = rdwr[1] = -1;
	}
	if (!err)
		err = rw_init(&rr, rdwr[0]);
	if (!err) {
		fprintf(rr.wfp, "forget_multi");
		err = rr_do(&rr, NULL, &rlen);
	}
	if (rdwr[0] >= 0)
		xclose(rdwr[0]);
	if (!err) {
		size_t to_write = sizeof(int64_t) * v_off;
		const char *p = tmp.x;

		do {
			ssize_t w = write(rdwr[1], p, to_write);
			if (w > 0) {
				p += w;
				to_write -= w;
			} else if (w < 0) {
				if (errno != EINTR) {
					warn("write");
					break;
				}
			} else {
				warnx("wrote 0 bytes to pipe");
				break;
			}
		} while (to_write);
	}
	if (rdwr[1] >= 0)
		xclose(rdwr[1]);
}

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 8)
static void f3_lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence,
		     struct fuse_file_info *fi)
{
	off_t res = lseek((int)fi->fh, off, whence);
	res < 0 ? fuse_reply_err(req, errno) : fuse_reply_lseek(req, res);
}
#endif /* FUSE >= 3.8 */

static void f3_copy_file_range(fuse_req_t req, fuse_ino_t ino_in,
			 off_t off_in, struct fuse_file_info *fi_in,
			 fuse_ino_t ino_out, off_t off_out,
			 struct fuse_file_info *fi_out, size_t len, int flags)
{
	struct f3_inode *dst = f3_inode(ino_out);
	ssize_t n;
	int src_fd = (int)fi_in->fh;
	int dst_fd = (int)fi_out->fh;

	inode_acq(dst);
	if (upgrade_rw(req, dst)) return;
	n = copy_file_range(src_fd, &off_in, dst_fd, &off_out, len, flags);
	dst->rw = F3_DIRTY;
	inode_rel(dst);
	n < 0 ? fuse_reply_err(req, errno) : fuse_reply_write(req, (size_t)n);
}

static void
f3_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct f3_req_res rr;
	int err = ro_init(&rr, -1);
	size_t rlen = sizeof(err);

	if (!err) {
		fprintf(rr.wfp, "opendir%c%"PRId64, 0, f3_vid(ino));
		err = rr_do(&rr, &err, &rlen);
	}
	if (err) {
		fuse_reply_err(req, err);
	} else {
		fi->fh = -1;
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 5)
		fi->cache_readdir = f3.cache_readdir;
#endif
		fuse_reply_open(req, fi);
	}
}

static void f3_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
			off_t off, struct fuse_file_info *fi)
{
	size_t rem = size;
	struct f3_req_res rr;
	struct {
		int err;
		int pad_;
		struct stat sb;
		char vname[NAME_MAX + 1];
	} frr = { 0 };
	struct iovec iov = { .iov_base = &frr, .iov_len = sizeof(frr) };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	char *buf;
	char *p = buf = malloc(size);

	if (!p) warn("malloc(%zu)", size);
	frr.err = p ? ro_init(&rr, -1) : EIO;
	if (!frr.err) {
		fprintf(rr.wfp, "readdirplus%c%"PRId64"%c%"PRId64,
			0, f3_vid(ino), 0, (int64_t)off);
		frr.err = rr_send(&rr, sizeof(frr));
	}
	while (!frr.err) {
		size_t entsize = 0;
		struct f3_inode *ent_ino = NULL; /* for rollback */
		struct fuse_entry_param e = { 0 };
		ssize_t r;

		do {
			r = recvmsg(rr.sk[0], &msg, MSG_CMSG_CLOEXEC);
		} while (r < 0 && errno == EINTR);
		if (r < 0) {
			warn("recvmsg");
			frr.err = EIO;
		}
		if (r <= 0 || frr.err) {
			xclose(rr.sk[0]);
			break;
		}
		/*
		 * off=0 => ".", off=1 => ".."
		 * fuse won't ref "." and "..", so we can't, either
		 */
		if (off <= 1) {
			e.attr = frr.sb;
		} else { /* ref_inode sets e.attr: */
			frr.err = ref_inode(&e, &frr.sb, NULL);
			if (!frr.err) {
				ent_ino = (struct f3_inode *)(uintptr_t)e.ino;
				off = frr.sb.st_dev + 1; /* rowid offset */
			}
		}
		if (!frr.err)
			entsize = fuse_add_direntry_plus(req, p, rem,
							frr.vname, &e, ++off);
		if (entsize > rem) {
			if (ent_ino)
				unref_inode(ent_ino, 1);
			xclose(rr.sk[0]);
			break;
		}
		p += entsize;
		rem -= entsize;
	}
	if (frr.err == EOF)
		frr.err = 0;
	/*
	 * If there's an error, we can only signal it if we haven't stored
	 * any entries yet - otherwise we'd end up with wrong lookup
	 * counts for the entries that are already in the buffer. So we
	 * return what we've collected until that point.
	 */
	if (frr.err && rem == size)
		fuse_reply_err(req, frr.err);
	else
		fuse_reply_buf(req, buf, size - rem);
	free(buf);
}

static int fi_prepare(struct fuse_file_info *fi)
{
	/*
	 * I don't think true O_DIRECT to our internal store can ever be
	 * supported since we support different backing FSes.  Using
	 * fi->direct_io when O_DIRECT is in fi->flags doesn't seem to work
	 * well when mixed with buffered I/O (e.g.  * xfstests:generic/647).
	 */
	if (fi->flags & O_DIRECT)
		return EINVAL;
	fi->keep_cache = f3.keep_cache;
	fi->direct_io = f3.direct_io;
	return 0;
}

static void f3_create(fuse_req_t req, fuse_ino_t parent, const char *name,
		      mode_t mode, struct fuse_file_info *fi)
{
	struct f3_req_res rr;
	struct f3_attr_res far = { .err = fi_prepare(fi) };
	struct fuse_entry_param e = { 0 };
	size_t rlen = sizeof(far);

	if (!far.err)
		far.err = rw_init(&rr, -1);
	if (!far.err) {
		fprintf(rr.wfp, "create%c%"PRId64"%c%s%c0%o%c%u",
			0, f3_vid(parent), 0, name, 0, mode, 0, fi->flags);
		far.err = rr_do(&rr, &far, &rlen);
	}
	if (!far.err) {
		fi->fh = (uint64_t)rr.sk[0];
		far.err = ref_inode(&e, &far.sb, fi);
	}
	if (far.err)
		return (void)fuse_reply_err(req, far.err);

	fuse_reply_create(req, &e, fi);
}

static void
f3_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int err = fi_prepare(fi);
	struct f3_inode *inode = f3_inode(ino);

	if (err)
		return (void)fuse_reply_err(req, err);

	/*
	 * n.b.: FUSE handles FIFOs internally, all we needed to do was
	 * support mknod; so there's no special code path needed for them.
	 * We always a read-only handle for non-empty files, upgrade_rw
	 * happens lazily on first write.  Empty files are always R/W.
	 */
	inode_acq(inode);
	if (inode->fd >= 0) {
		fi->fh = (uint64_t)inode->fd;
	} else {
		struct f3_req_res rr;
		size_t rlen = sizeof(err);

		if (!err)
			err = ro_init(&rr, -1);
		if (!err) {
			fprintf(rr.wfp, "open_rdonly%c%"PRId64, 0, inode->vid);
			err = rr_do(&rr, &err, &rlen);
		}
		if (!err) {
			struct stat sb;

			if (!fstat(rr.sk[0], &sb)) {
				assert(S_ISREG(sb.st_mode));
				fi->fh = (uint64_t)rr.sk[0];
				set_fd_once(inode, &sb, fi);
			} else {
				warn("(open) fstat");
				err = EIO;
			}
		}
	}
	inode_rel(inode);
	err ? fuse_reply_err(req, err) : fuse_reply_open(req, fi);
}

static void f3_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int err = 0;
	int fd = (int)fi->fh;
	int newfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);

	if (newfd >= 0)
		wclose(&err, newfd);
	else
		warn("F_DUPFD_CLOEXEC(%d)", fd);
	fuse_reply_err(req, err);
}

static void
f3_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct f3_inode *inode = f3_inode(ino);
	int err = 0;

	inode_acq(inode);
	if (inode->rw == F3_DIRTY) {
		struct f3_req_res rr;
		size_t rlen = sizeof(err);

		err = rw_init(&rr, (int)fi->fh);
		if (!err) {
			fprintf(rr.wfp, "release%c%"PRId64, 0, inode->vid);
			err = rr_do(&rr, &err, &rlen);
			if (!err)
				err = replace_fd(inode, rr.sk[0]);
		}
	}
	inode_rel(inode);
	fuse_reply_err(req, err);
}

static void f3_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
		     struct fuse_file_info *fi)
{
	fuse_reply_err(req, fsync_inode(f3_inode(ino)));
}

static void f3_read(fuse_req_t req, fuse_ino_t ino, size_t size,
			off_t off, struct fuse_file_info *fi)
{
	struct fuse_bufvec buf = FUSE_BUFVEC_INIT(size);

	buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	buf.buf[0].fd = (int)fi->fh;
	buf.buf[0].pos = off;

	fuse_reply_data(req, &buf, FUSE_BUF_SPLICE_MOVE);
}

static void f3_write_buf(fuse_req_t req, fuse_ino_t ino,
			struct fuse_bufvec *in_buf, off_t off,
			struct fuse_file_info *fi)
{
	struct f3_inode *inode = f3_inode(ino);
	ssize_t n;
	struct fuse_bufvec out_buf = FUSE_BUFVEC_INIT(fuse_buf_size(in_buf));

	out_buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	out_buf.buf[0].fd = (int)fi->fh;
	out_buf.buf[0].pos = off;

	inode_acq(inode);
	if (upgrade_rw(req, inode))
		return;
	n = fuse_buf_copy(&out_buf, in_buf, FUSE_BUF_SPLICE_MOVE);
	inode->rw = F3_DIRTY;
	inode_rel(inode);
	n < 0 ? fuse_reply_err(req, -n) : fuse_reply_write(req, (size_t)n);
}

static void f3_statfs(fuse_req_t req, fuse_ino_t ino)
{
	struct f3_req_res rr;
	struct {
		int err;
		int pad_;
		struct statvfs v;
	} res = { .err = ro_init(&rr, -1) };
	size_t rlen = sizeof(res);

	if (!res.err) {
		fprintf(rr.wfp, "statfs");
		res.err = rr_do(&rr, &res, &rlen);
	}
	res.err ? fuse_reply_err(req, res.err) : fuse_reply_statfs(req, &res.v);
}

static void
f3_fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
		 off_t offset, off_t length, struct fuse_file_info *fi)
{
	struct f3_inode *inode = f3_inode(ino);
	int err = 0;

	inode_acq(inode);
	if (upgrade_rw(req, inode))
		return;
	if (fallocate((int)fi->fh, mode, offset, length))
		err = errno;
	inode->rw = F3_DIRTY;
	inode_rel(inode);
	fuse_reply_err(req, err);
}

static void
f3_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
	const struct f3_inode *inode = f3_inode(ino);
	struct f3_req_res rr;
	struct f3_xattr_res fxr;
	size_t rlen = sizeof(fxr);

	if (ino == FUSE_ROOT_ID) { /* show f3 internal vars */
		char x[80];
		int n = INT_MAX;

		if (!strcmp(name, F3_NS".entry_timeout"))
			n = snprintf(x, sizeof(x), "%lf", f3.entry_timeout);
		else if (!strcmp(name, F3_NS".attr_timeout"))
			n = snprintf(x, sizeof(x), "%lf", f3.attr_timeout);
		else if (!strcmp(name, F3_NS".cache_readdir"))
			n = snprintf(x, sizeof(x), "%u", f3.cache_readdir);
		else if (!strcmp(name, F3_NS".keep_cache"))
			n = snprintf(x, sizeof(x), "%u", f3.keep_cache);
		else if (!strcmp(name, F3_NS".direct_io"))
			n = snprintf(x, sizeof(x), "%u", f3.direct_io);
		assert(n > 0 && "we suck a snprintf");
		if (n != INT_MAX && n > 0)
			return size ? (void)fuse_reply_buf(req, x, n)
					: (void)fuse_reply_xattr(req, n);
	}

	fxr.err = ro_init(&rr, -1);
	if (!fxr.err) {
		fprintf(rr.wfp, "getxattr%c%"PRId64"%c%s%c%zu",
			0, inode->vid, 0, name, 0, size);
		fxr.err = rr_do(&rr, &fxr, &rlen);
	}
	if (fxr.err) return (void)fuse_reply_err(req, fxr.err);
	size ? fuse_reply_buf(req, fxr.buf, fxr.len) :
		fuse_reply_xattr(req, fxr.len);
}

static void f3_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
	const struct f3_inode *inode = f3_inode(ino);
	struct f3_req_res rr;
	struct f3_xattr_res fxr = { .err = ro_init(&rr, -1) };
	size_t rlen = sizeof(fxr);

	if (!fxr.err) {
		fprintf(rr.wfp, "listxattr%c%"PRId64"%c%zu",
			0, inode->vid, 0, size);
		fxr.err = rr_do(&rr, &fxr, &rlen);
	}
	if (fxr.err) return (void)fuse_reply_err(req, fxr.err);
	size ? fuse_reply_buf(req, fxr.buf, fxr.len) :
		fuse_reply_xattr(req, fxr.len);
}

static void f3_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
			const char *value, size_t size, int flags)
{
	const struct f3_inode *inode = f3_inode(ino);
	struct f3_req_res rr;
	int err = 0;
	size_t rlen = sizeof(err);
	char op = 's';

	if (XATTR_CREATE & flags)
		op = 'c';
	else if (XATTR_REPLACE & flags)
		op = 'r';

	/*
	 * mount -o remount doesn't work with FUSE, so allow tweaking f3 knobs
	 * via setxattr
	 */
	if (ino == FUSE_ROOT_ID) {
		double d;
		unsigned u;

		if (op == 'c')
			return (void)fuse_reply_err(req, EEXIST);

		if (!strcmp(name, F3_NS".entry_timeout")) {
			if (sscanf(value, "%lf", &d) != 1)
				return (void)fuse_reply_err(req, EINVAL);
			f3.entry_timeout = d;
			return (void)fuse_reply_err(req, 0);
		} else if (!strcmp(name, F3_NS".attr_timeout")) {
			if (sscanf(value, "%lf", &d) != 1)
				return (void)fuse_reply_err(req, EINVAL);
			f3.attr_timeout = d;
			return (void)fuse_reply_err(req, 0);
		} else if (!strcmp(name, F3_NS".cache_readdir")) {
			if (sscanf(value, "%u", &u) != 1)
				return (void)fuse_reply_err(req, EINVAL);
			f3.cache_readdir = !!u;
			return (void)fuse_reply_err(req, 0);
		} else if (!strcmp(name, F3_NS".keep_cache")) {
			if (sscanf(value, "%u", &u) != 1)
				return (void)fuse_reply_err(req, EINVAL);
			f3.keep_cache = !!u;
			return (void)fuse_reply_err(req, 0);
		} else if (!strcmp(name, F3_NS".direct_io")) {
			if (sscanf(value, "%u", &u) != 1)
				return (void)fuse_reply_err(req, EINVAL);
			f3.direct_io = !!u;
			return (void)fuse_reply_err(req, 0);
		}
	}
	if (!err)
		err = rw_init(&rr, -1);
	if (!err) {
		fprintf(rr.wfp, "setxattr%c%c%c%"PRId64"%c%s%c%zu%c",
			0, op, 0, inode->vid, 0, name, 0, size, 0);
		fwrite(value, 1, size, rr.wfp);
		err = rr_do(&rr, &err, &rlen);
	}
	fuse_reply_err(req, err);
}

static void f3_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name)
{
	const struct f3_inode *inode = f3_inode(ino);
	struct f3_req_res rr;
	int err = rw_init(&rr, -1);
	size_t rlen = sizeof(err);

	if (!err) {
		fprintf(rr.wfp, "removexattr%c%"PRId64"%c%s",
			0, inode->vid, 0, name);
		err = rr_do(&rr, &err, &rlen);
	}
	fuse_reply_err(req, err);
}

static void f3_destroy(void *userdata)
{
	struct cds_lfht_iter iter;
	struct f3_inode *inode;
	FILE *tmp = tmpfile();

	if (!tmp)
		warn("tmpfile failed, manual GC will be needed");

	rcu_read_lock();
	if (tmp) {
		cds_lfht_for_each_entry(f3.vid2inode, &iter, inode, nd) {
			fwrite(&inode->vid, 1, sizeof(inode->vid), tmp);
			delete_inode(inode);
		}
	} else {
		cds_lfht_for_each_entry(f3.vid2inode, &iter, inode, nd)
			delete_inode(inode);
	}
	rcu_read_unlock();
	rcu_barrier(); /* wait for all delete_inode to finish */

	if (!tmp)
		return;
	if (fflush(tmp) || ferror(tmp)) {
		warn("fflush+ferror");
	} else {
		struct f3_req_res rr;
		size_t rlen = 0; /* don't want a response */
		int fd = fileno(tmp);

		if (lseek(fd, SEEK_SET, 0)) {
			warn("lseek");
		} else if (rw_init(&rr, fd) == 0) {
			fprintf(rr.wfp, "forget_multi");
			(void)rr_do(&rr, NULL, &rlen);
		}
	}
	fclose(tmp);
}

static const struct fuse_lowlevel_ops f3_ops = {
	.init = f3_init,
	.destroy = f3_destroy,
	.lookup = f3_lookup,
	.mkdir = f3_mkdir,
	.mknod = f3_mknod,
	.symlink = f3_symlink,
	.link = f3_link,
	.unlink = f3_unlink,
	.rmdir = f3_rmdir,
	.rename = f3_rename,
	.forget = f3_forget,
	.forget_multi = f3_forget_multi,
	.getattr = f3_getattr,
	.setattr = f3_setattr,
	.readlink = f3_readlink,
	.opendir = f3_opendir,
	.readdirplus = f3_readdirplus,
	.create = f3_create,
	.open = f3_open,
	.release = f3_release,
	.flush = f3_flush,
	.fsync = f3_fsync,
	.read = f3_read,
	.write_buf = f3_write_buf,
	.statfs = f3_statfs,
	.fallocate = f3_fallocate,
	/* let the kernel deal with flock, getlk, setlk */
	.getxattr = f3_getxattr,
	.listxattr = f3_listxattr,
	.setxattr = f3_setxattr,
	.removexattr = f3_removexattr,
	.copy_file_range = f3_copy_file_range,
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 8)
	.lseek = f3_lseek,
#endif
};

/*
 * pthread_atfork callbacks, there's a fork in daemonization,
 * and also via fuse_session_unmount which spawns fusermount
 */
static void atfork_prepare(void)
{
	call_rcu_before_fork();
	rcu_bp_before_fork();
}

static void atfork_child(void)
{
	rcu_bp_after_fork_child();
	call_rcu_after_fork_child();
}

static void atfork_parent(void)
{
	rcu_bp_after_fork_parent();
	call_rcu_after_fork_parent();
}

static void my_daemonize(void) /* this keeps stderr for syslog */
{
	int waiter[2];

	if (pipe(waiter)) err(1, "pipe");

	switch (fork()) {
	case -1: err(1, "fork");
	case 0:
		xclose(waiter[0]);
		break;
	default:
		xclose(waiter[1]);
		(void)read(waiter[0], waiter, 1);
		_exit(0);
	}
	if (setsid() < 0) err(1, "setsid");
	xclose(waiter[1]);
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_session *se;
	struct fuse_cmdline_opts opt;
	struct fuse_loop_config cfg;
	int ret = -1;
	if (fuse_parse_cmdline(&args, &opt) != 0)
		return 1;
	if (opt.show_help) {
		printf("usage: %s [options] <mountpoint>\n\n", argv[0]);
		fuse_cmdline_help();
		fuse_lowlevel_help();
		ret = 0;
		goto err_out1;
	} else if (opt.show_version) {
		printf("FUSE library version %s\n", fuse_pkgversion());
		fuse_lowlevel_version();
		ret = 0;
		goto err_out1;
	}
	if (!opt.mountpoint) {
		printf("usage: %s [options] <mountpoint>\n", argv[0]);
		printf("       %s --help\n", argv[0]);
		ret = 1;
		goto err_out1;
	}
	if (fuse_opt_parse(&args, &f3, f3_opt, NULL) == -1)
		return 1;
	if (f3.rfd < 0 && f3.wfd < 0)
		errx(1, "reader-fd and worker-fd both unset or negative");
	if (f3.rfd < 0) f3.rfd = f3.wfd;
	if (f3.vroot.vid < 0) errx(1, "root-vid unset or negative");

	errno = pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
	if (errno)
		err(1, "pthread_atfork");
	if (chdir("/"))
		err(1, "chdir /");

	se = fuse_session_new(&args, &f3_ops, sizeof(f3_ops), &f3);
	if (!se)
		goto err_out1;
	if (fuse_set_signal_handlers(se))
		goto err_out2;
	if (fuse_session_mount(se, opt.mountpoint)) /* may fork */
		goto err_out3;
	if (!opt.foreground)
		my_daemonize();
	if (opt.singlethread) {
		ret = fuse_session_loop(se);
	} else {
		cfg.clone_fd = 1;
		cfg.max_idle_threads = opt.max_idle_threads;
		ret = fuse_session_loop_mt(se, &cfg);
	}
	fuse_session_unmount(se); /* may fork */
err_out3:
	fuse_remove_signal_handlers(se);
err_out2:
	fuse_session_destroy(se);
err_out1:
	while (f3.vid2inode) {
		int ret = cds_lfht_destroy(f3.vid2inode, NULL);

		if (ret == 0) {
			f3.vid2inode = NULL;
		} else {
			static size_t tries;

			f3_destroy(&f3);
			errno = -ret;
			warnx("cds_lfht_destroy: %m (%zu tries)", ++tries);
		}
	}
	free(opt.mountpoint);
	fuse_opt_free_args(&args);

	return ret ? 1 : 0;
}
