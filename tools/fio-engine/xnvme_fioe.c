/*
 * fio xNVMe IO Engine
 *
 * IO engine using the asynchronous interface of the xNVMe C API.
 *
 * See: http://xnvme.io/
 *
 * -----------------------------------------------------------------------------
 *
 * Notes on the implementation and fio IO engines in general
 * =========================================================
 *
 * Built-in engine interface:
 *
 * - static void fio_init xnvme_fioe_register(void)
 * - static void fio_exit xnvme_fioe_unregister(void)
 * - static struct ioengine_ops ioengine
 * - Usage: '--ioengine=myengine'
 *
 * External engine interface:
 *
 * - struct ioengine_ops ioengine
 * - Usage: '--ioengine=external:/path/to/myengine.so'
 *
 * When writing an external engine you actually have two choices, you can:
 *
 * 1) Follow the "External engine interface" as described above
 * 2) Fake an internal engine
 *    - Implement the "Built-in engine interface"
 *    - Inject the engine via LD_PRELOAD=/path/to/myengine.so
 *    - NOTE: by injecting you are potentially overwriting more symbols than
 *      just those required by the "Built-in engine interface"
 *
 * It seems like the "cleanest" approach is to implement en engine following the
 * "External engine interface", however, there is some spurious behavior/race
 * causing a segfault when accessing `td->io_ops` in `_queue()`.
 *
 * However, for some reason, this segfault does not occur if `td->io_ops` is
 * touched during `_init()` which is why `_init()` echoes the value of
 * `td->io_ops`.
 *
 * CAVEAT: Multi-device support
 *
 * Support is here, however, there is one limiting caveat, and two others noted
 * in case issues should arise.
 *
 * - 1) iomem_{alloc/free} introduces a limitation with regards to multiple
 *   devices. Specifically, the devices opened must use backends which share
 *   memory allocators. E.g. using be:laio + be:liou is fine, using be:liou +
 *   be:spdk is not.
 *   This is due to the fio 'io_memem_*' helpers are not tied to devices, as
 *   such, it is required that all devices opened use compatible
 *   buffer-allocators. Currently, the implementation does dot check for this
 *   unsupported use-case, and will thus lead to a runtime error.
 *
 * - 2) The implementation assumes that 'thread_data.o.nr_files' is available
 *   and that instances of 'fio_file.fileno' are valued [0,
 *   thread_data.o.nr_files -1].
 *   This is to pre-allocate file-wrapping-structures, xnvme_fioe_fwrap, at I/O
 *   engine initialization time and to reference file-wrapping with
 *   constant-time lookup
 *
 * - 3) The _open() and _close() functions do not implement the "real"
 *   device/file opening, this is done in _init() and torn down in _cleanup() as
 *   the io-engine needs device handles ready for iomem_{alloc/free}
 *
 * CAVEAT: Supporting NVMe devices formatted with extended-LBA
 *
 * To support extended-lba initial work has been done in xNVMe, however, further
 * work is probably need for this to trickle up from the fio I/O engine
 */
#include <stdlib.h>
#include <assert.h>
#include <fio.h>
#include <zbd_types.h>
#include <optgroup.h>
#include <libxnvme.h>
#include <libznd.h>

static pthread_mutex_t g_serialize = PTHREAD_MUTEX_INITIALIZER;

struct xnvme_fioe_fwrap {
	///< fio file representation
	struct fio_file *fio_file;

	///< xNVMe device handle
	struct xnvme_dev *dev;
	///< xNVMe device geometry
	const struct xnvme_geo *geo;

	struct xnvme_async_ctx *ctx;
	struct xnvme_req_pool *reqs;

	uint32_t ssw;
	uint32_t lba_nbytes;

	uint8_t _pad[16];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fioe_fwrap) == 64, "Incorrect size")

struct xnvme_fioe_data {
	///< I/O completion queue
	struct io_u **iocq;

	///< # of iocq entries; incremented via getevents()/cb_pool()
	uint64_t completed;

	///< # of errors; incremented when observed on completion via getevents()/cb_pool()
	uint64_t ecount;

	///< Controller which device/file to select
	int32_t prev;
	int32_t cur;

	///< Number of devices/files for which open() has been called
	uint64_t nopen;
	///< Number of devices/files allocated in files[]
	uint64_t nallocated;

	uint8_t _pad[16];

	struct xnvme_fioe_fwrap files[];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fioe_data) == 64, "Incorrect size")

struct xnvme_fioe_options {
	void *padding;
	unsigned int hipri;
	unsigned int sqpoll_thread;
	char *be;
};

static struct fio_option options[] = {
	{
		.name   = "hipri",
		.lname  = "High Priority",
		.type   = FIO_OPT_STR_SET,
		.off1   = offsetof(struct xnvme_fioe_options, hipri),
		.help   = "Use polled IO completions",
		.category = FIO_OPT_C_ENGINE,
		.group  = FIO_OPT_G_IOURING,
	},
	{
		.name   = "sqthread_poll",
		.lname  = "Kernel SQ thread polling",
		.type   = FIO_OPT_INT,
		.off1   = offsetof(struct xnvme_fioe_options, sqpoll_thread),
		.help   = "Offload submission/completion to kernel thread",
		.category = FIO_OPT_C_ENGINE,
		.group  = FIO_OPT_G_IOURING,
	},
	{
		.name   = "be",
		.lname  = "xNVMe Backend",
		.type   = FIO_OPT_STR_STORE,
		.off1   = offsetof(struct xnvme_fioe_options, be),
		.help   = "Default backend when none is provided e.g. /dev/nvme0n1",
		.category = FIO_OPT_C_ENGINE,
		.group  = FIO_OPT_G_NBD,
	},
	{
		.name   = NULL,
	},
};

static void
cb_pool(struct xnvme_req *req, void *cb_arg)
{
	struct io_u *io_u = cb_arg;
	struct xnvme_fioe_data *xd = io_u->engine_data;

	if (xnvme_req_cpl_status(req)) {
		xnvme_req_pr(req, XNVME_PR_DEF);
		xd->ecount += 1;
		io_u->error = EIO;
	}

	xd->iocq[xd->completed++] = io_u;
	SLIST_INSERT_HEAD(&req->pool->head, req, link);
}

#ifdef XNVME_DEBUG_ENABLED
static void
_fio_file_pr(struct fio_file *f)
{
	log_info("fio_file: { ");
	log_info("file_name: '%s', ", f->file_name);
	log_info("fileno: %d, ", f->fileno);
	log_info("io_size: %zu, ", f->io_size);
	log_info("real_file_size: %zu, ", f->real_file_size);
	log_info("file_offset: %zu", f->file_offset);
	log_info("}\n");
}
#endif

static int
_dev_close(struct thread_data *td, struct xnvme_fioe_fwrap *fwrap)
{
	if (fwrap->dev) {
		xnvme_async_term(fwrap->dev, fwrap->ctx);
	}
	xnvme_req_pool_free(fwrap->reqs);
	xnvme_dev_close(fwrap->dev);

	memset(fwrap, 0, sizeof(*fwrap));

	return 0;
}

static void
xnvme_fioe_cleanup(struct thread_data *td)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	for (uint64_t i = 0; i < xd->nallocated; ++i) {
		int err;

		pthread_mutex_lock(&g_serialize);
		err = _dev_close(td, &xd->files[i]);
		pthread_mutex_unlock(&g_serialize);
		if (err) {
			XNVME_DEBUG("xnvme_fioe: cleanup(): Unexpected error");
		}
	}

	free(xd->iocq);
	free(xd);
	td->io_ops_data = NULL;
}

static int
_dev_open(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_fioe_options *o = td->eo;
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap;
	char dev_uri[XNVME_IDENT_URI_LEN] = { 0 };
	int fn_has_scheme;
	int flags = 0;

	XNVME_DEBUG("o->be: '%s'", o->be);

	if (o->be && (strlen(o->be) > XNVME_IDENT_SCHM_LEN)) {
		log_err("xnvme_fioe: invalid --be=%s\n", o->be);
		return 1;
	}
	if (o->hipri) {
		flags |= XNVME_ASYNC_IOPOLL;
	}
	if (o->sqpoll_thread) {
		flags |= XNVME_ASYNC_SQPOLL;
	}
	if (f->fileno > (int)xd->nallocated) {
		log_err("xnvme_fioe: _dev_open(); invalid assumption\n");
		return 1;
	}

	{
		char schm[XNVME_IDENT_SCHM_LEN];
		char sep[1];
		int matches;

		matches = sscanf(f->file_name, "%4[a-z]%1[:]", schm, sep) == 2;
		fn_has_scheme = matches == 2;
	}

	if (o->be && (!fn_has_scheme)) {
		snprintf(dev_uri, XNVME_IDENT_URI_LEN - 2, "%s:", o->be);
	}
	strncat(dev_uri, f->file_name, XNVME_IDENT_URI_LEN - strlen(dev_uri));

	XNVME_DEBUG("INFO: dev_uri: '%s'", dev_uri);

	fwrap = &xd->files[f->fileno];

	pthread_mutex_lock(&g_serialize);
	fwrap->dev = xnvme_dev_open(dev_uri);
	pthread_mutex_unlock(&g_serialize);
	if (!fwrap->dev) {
		log_err("xnvme_fioe: init(): {uri: '%s', err: '%s'}\n",
			dev_uri, strerror(errno));
		return 1;
	}
	fwrap->geo = xnvme_dev_get_geo(fwrap->dev);

	if (xnvme_async_init(fwrap->dev, &(fwrap->ctx), td->o.iodepth, flags)) {
		log_err("xnvme_fioe: init(): failed xnvme_async_init()\n");
		return 1;
	}
	if (xnvme_req_pool_alloc(&fwrap->reqs, td->o.iodepth + 1)) {
		log_err("xnvme_fioe: init(): xnvme_req_pool_alloc()\n");
		return 1;
	}
	// NOTE: cb_args are assigned in _queue()
	if (xnvme_req_pool_init(fwrap->reqs, fwrap->ctx, cb_pool, NULL)) {
		log_err("xnvme_fioe: init(): xnvme_req_pool_init()\n");
		return 1;
	}

	fwrap->ssw = xnvme_dev_get_ssw(fwrap->dev);
	fwrap->lba_nbytes = fwrap->geo->lba_nbytes;

	fwrap->fio_file = f;
	fwrap->fio_file->filetype = FIO_TYPE_BLOCK;
	fwrap->fio_file->real_file_size = fwrap->geo->tbytes;
	fio_file_set_size_known(fwrap->fio_file);

	return 0;
}

static int
xnvme_fioe_init(struct thread_data *td)
{
	struct xnvme_fioe_data *xd = NULL;
	struct fio_file *f;
	unsigned int i;

	log_info("xnvme_fioe: init(): td->io_ops: %p\n", td->io_ops);

	if (!td->o.use_thread) {
		log_err("xnvme_fioe: init(): --thread=1 is required\n");
		return 1;
	}
	if (!td->io_ops) {
		log_err("xnvme_fioe: init(): !td->io_ops\n");
		log_err("xnvme_fioe: init(): Check fio version\n");
		log_err("xnvme_fioe: init(): I/O engine running with: '%s'\n",
			fio_version_string);
		log_err("xnvme_fioe: init(): I/O engine built with:\n");
		xnvme_3p_ver_fpr(stderr, xnvme_3p_ver, XNVME_PR_DEF);
		return 1;
	}

	// Allocate and zero-fill xd
	xd = malloc(sizeof(*xd) + sizeof(*xd->files) * td->o.nr_files);
	if (!xd) {
		log_err("xnvme_fioe: init(): !malloc()\n");
		return 1;
	}
	memset(xd, 0, sizeof(*xd) + sizeof(*xd->files) * td->o.nr_files);

	xd->iocq = malloc(td->o.iodepth * sizeof(struct io_u *));
	memset(xd->iocq, 0, td->o.iodepth * sizeof(struct io_u *));

	xd->prev = -1;
	td->io_ops_data = xd;

	for_each_file(td, f, i) {
		if (_dev_open(td, f)) {
			log_err("xnvme_fioe: init(): _dev_open(%s)\n",
				f->file_name);
			return 1;
		}

		++(xd->nallocated);
	}

	if (xd->nallocated != td->o.nr_files) {
		log_err("xnvme_fioe: init(): nallocated != td->o.nr_files\n");
		return 1;
	}

	return 0;
}

// NOTE: using the first device for buffer-allocators, see CAVEAT 2)
static int
xnvme_fioe_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = &xd->files[0];

	if (!fwrap->dev) {
		log_err("xnvme_fioe: failed iomem_alloc(); no dev-handle\n");
		return 1;
	}

	td->orig_buffer = xnvme_buf_alloc(fwrap->dev, total_mem, NULL);

	return td->orig_buffer == NULL;
}

// NOTE: using the first device for buffer-allocators, see CAVEAT 2)
static void
xnvme_fioe_iomem_free(struct thread_data *td)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = &xd->files[0];

	if (!fwrap->dev) {
		log_err("xnvme_fioe: failed iomem_free(); no dev-handle\n");
		return;
	}

	xnvme_buf_free(fwrap->dev, td->orig_buffer);
}

static int
xnvme_fioe_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	io_u->engine_data = td->io_ops_data;

	return 0;
}

static void
xnvme_fioe_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	io_u->engine_data = NULL;
}

static struct io_u *
xnvme_fioe_event(struct thread_data *td, int event)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	assert(event >= 0);
	assert((unsigned)event < xd->completed);

	return xd->iocq[event];
}

static int
xnvme_fioe_getevents(struct thread_data *td, unsigned int min,
		     unsigned int max, const struct timespec *t)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = NULL;
	int nfiles = xd->nallocated;
	int err = 0;

	if (t) {
		assert(false);
	}

	if (xd->prev != -1 && ++xd->prev < nfiles) {
		fwrap = &xd->files[xd->prev];
		xd->cur = xd->prev;
	}

	xd->completed = 0;
	for (; ;) {
		if (fwrap == NULL || xd->cur == nfiles) {
			fwrap = &xd->files[0];
			xd->cur = 0;
		}

		while (fwrap != NULL && xd->cur < nfiles && err >= 0) {
			err = xnvme_async_poke(fwrap->dev, fwrap->ctx,
					       max - xd->completed);
			if (err < 0) {
				switch (err) {
				case -EBUSY:
				case -EAGAIN:
					usleep(1);
					break;

				default:
					XNVME_DEBUG("Oh my");
					assert(false);
					return 0;
				}
			}
			if (xd->completed >= min) {
				xd->prev = xd->cur;
				return xd->completed;
			}
			xd->cur++;
			fwrap = &xd->files[xd->cur];

			if (err < 0) {
				switch (err) {
				case -EBUSY:
				case -EAGAIN:
					usleep(1);
					break;
				}
			}
		}
	}

	xd->cur = 0;

	return xd->completed;
}

static enum fio_q_status
xnvme_fioe_queue(struct thread_data *td, struct io_u *io_u)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap;
	struct xnvme_req *req;
	uint32_t nsid;
	uint64_t slba;
	uint16_t nlb;
	int err;

	fio_ro_check(td, io_u);

	fwrap = &xd->files[io_u->file->fileno];
	nsid = xnvme_dev_get_nsid(fwrap->dev);

	slba = io_u->offset >> fwrap->ssw;
	nlb = (io_u->xfer_buflen / fwrap->lba_nbytes) - 1;

	if (td->io_ops->flags & FIO_SYNCIO) {
		log_err("xnvme_fioe: queue(): Got sync...\n");
		assert(false);
		return FIO_Q_COMPLETED;
	}

	req = SLIST_FIRST(&fwrap->reqs->head);
	SLIST_REMOVE_HEAD(&fwrap->reqs->head, link);

	req->async.cb_arg = io_u;

	switch (io_u->ddir) {
	case DDIR_READ:
		err = xnvme_cmd_read(fwrap->dev, nsid, slba, nlb,
				     io_u->xfer_buf, NULL, XNVME_CMD_ASYNC,
				     req);
		break;

	case DDIR_WRITE:
		err = xnvme_cmd_write(fwrap->dev, nsid, slba, nlb,
				      io_u->xfer_buf, NULL, XNVME_CMD_ASYNC,
				      req);
		break;

	default:
		log_err("xnvme_fioe: queue(): ENOSYS: %u\n", io_u->ddir);
		err = -1;
		assert(false);
		break;
	}

	switch (err) {
	case 0:
		return FIO_Q_QUEUED;

	case -EBUSY:
	case -EAGAIN:
		SLIST_INSERT_HEAD(&req->pool->head, req, link);
		return FIO_Q_BUSY;

	default:
		log_err("xnvme_fioe: queue(): err: '%d'\n", err);

		SLIST_INSERT_HEAD(&req->pool->head, req, link);
		io_u->error = abs(err);
		assert(false);
		return FIO_Q_COMPLETED;
	}
}

// See CAVEAT for explanation and _cleanup() + _dev_close() for implementation
static int
xnvme_fioe_close(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	XNVME_DEBUG_FCALL(_fio_file_pr(f);)

	--(xd->nopen);

	return 0;
}

// See CAVEAT for explanation and _init() + _dev_open() for implementation
static int
xnvme_fioe_open(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	XNVME_DEBUG_FCALL(_fio_file_pr(f);)

	if (f->fileno > (int)xd->nallocated) {
		XNVME_DEBUG("f->fileno > xd->nallocated; invalid assumption");
		return 1;
	}
	if (xd->files[f->fileno].fio_file != f) {
		XNVME_DEBUG("well... that is off..");
		return 1;
	}

	++(xd->nopen);

	return 0;
}

static int
xnvme_fioe_invalidate(struct thread_data *td, struct fio_file *f)
{
	// Consider only doing this with be:spdk
	return 0;
}

/**
 * Currently, this function is called before of I/O engine initialization, so,
 * we cannot consult the file-wrapping done when 'fioe' initializes.
 * Instead we just open base don the given filename.
 *
 * TODO: unify the different setup methods, consider keeping the handle around,
 * and consider how to support the --be option in this usecase
 */
static int
xnvme_fioe_get_zoned_model(struct thread_data *XNVME_UNUSED(td),
			   struct fio_file *f, enum zbd_zoned_model *model)
{
	struct xnvme_dev *dev;

	XNVME_DEBUG("Getting the zoned model for: '%s'", f->file_name);

	if (f->filetype != FIO_TYPE_BLOCK && f->filetype != FIO_TYPE_CHAR) {
		*model = ZBD_IGNORE;
		XNVME_DEBUG("INFO: ignoring filetype");
		return 0;
	}

	pthread_mutex_lock(&g_serialize);
	dev = xnvme_dev_open(f->file_name);
	pthread_mutex_unlock(&g_serialize);
	if (!dev) {
		XNVME_DEBUG("FAILED: retrieving device handle");
		return 1;
	}

	switch(xnvme_dev_get_geo(dev)->type) {
	case XNVME_GEO_UNKNOWN:
		XNVME_DEBUG("INFO: got 'unknown', assigning ZBD_NONE");
		*model = ZBD_NONE;
		break;

	case XNVME_GEO_CONVENTIONAL:
		XNVME_DEBUG("INFO: got 'conventional', assigning ZBD_NONE");
		*model = ZBD_NONE;
		break;

	case XNVME_GEO_ZONED:
		XNVME_DEBUG("INFO: got 'zoned', assigning ZBD_HOST_MANAGED");
		*model = ZBD_HOST_MANAGED;
		break;

	default:
		XNVME_DEBUG("FAILED:: got 'zoned', assigning ZBD_HOST_MANAGED");
		*model = ZBD_NONE;
		return -EINVAL;
	}

	pthread_mutex_lock(&g_serialize);
	xnvme_dev_close(dev);
	pthread_mutex_unlock(&g_serialize);
	
	XNVME_DEBUG("INFO: so good to far...");

	return 0;
}

/**
 * Currently, this function is called before of I/O engine initialization, so,
 * we cannot consult the file-wrapping done when 'fioe' initializes.
 * Instead we just open base don the given filename.
 *
 * TODO: unify the different setup methods, consider keeping the handle around,
 * and consider how to support the --be option in this usecase
 */
static int
xnvme_fioe_report_zones(struct thread_data *XNVME_UNUSED(td),
			struct fio_file *f, uint64_t offset,
			struct zbd_zone *zbdz, unsigned int nr_zones)
{
	struct xnvme_dev *dev = NULL;
	const struct xnvme_geo *geo = NULL;
	struct znd_report *rprt = NULL;
	uint32_t ssw;
	uint64_t slba;
	int err = 0;

	XNVME_DEBUG("report_zones(): '%s', offset: %zu, nr_zones: %u",
		    f->file_name, offset, nr_zones);

	pthread_mutex_lock(&g_serialize);
	dev = xnvme_dev_open(f->file_name);
	pthread_mutex_unlock(&g_serialize);
	if (!dev) {
		XNVME_DEBUG("FAILED: xnvme_dev_open(), errno: %d", errno);
		goto exit;
	}

	geo = xnvme_dev_get_geo(dev);
	ssw = xnvme_dev_get_ssw(dev);

	slba = ((offset >> ssw) / geo->nsect) * geo->nsect;

	rprt = znd_report_from_dev(dev, slba, nr_zones, 0);
	if (!rprt) {
		XNVME_DEBUG("FAILED: znd_report_from_dev(), errno: %d", errno);
		err = -errno;
		goto exit;
	}
	if (rprt->nentries != nr_zones) {
		XNVME_DEBUG("FAILED: nentries != nr_zones");
		err = 1;
		goto exit;
	}
	if (offset > geo->tbytes) {
		XNVME_DEBUG("INFO: out-of-bounds");
		goto exit;
	}

	// Transform the zone-report
	for (uint32_t idx = 0; idx < rprt->nentries; ++idx) {
		struct znd_descr *descr = ZND_REPORT_DESCR(rprt, idx);

		zbdz[idx].start = descr->zslba << ssw;
		zbdz[idx].len = descr->zcap << ssw;
		zbdz[idx].wp = descr->wp << ssw;

		switch (descr->zt) {
		case ZND_TYPE_SEQWR:
			zbdz[idx].type = ZBD_ZONE_TYPE_SWR;
			break;

		default:
			log_err("%s: invalid type for zone at offset%zu.\n",
				f->file_name, zbdz[idx].start);
			err = -EIO;
			goto exit;
		}

		switch (descr->zs) {
		case ZND_STATE_EMPTY:
			zbdz[idx].cond = ZBD_ZONE_COND_EMPTY;
			break;
		case ZND_STATE_IOPEN:
			zbdz[idx].cond = ZBD_ZONE_COND_IMP_OPEN;
			break;
		case ZND_STATE_EOPEN:
			zbdz[idx].cond = ZBD_ZONE_COND_EXP_OPEN;
			break;
		case ZND_STATE_CLOSED:
			zbdz[idx].cond = ZBD_ZONE_COND_CLOSED;
			break;
		case ZND_STATE_FULL:
			zbdz[idx].cond = ZBD_ZONE_COND_FULL;
			break;

		case ZND_STATE_RONLY:
		case ZND_STATE_OFFLINE:
		default:
			zbdz[idx].cond = ZBD_ZONE_COND_OFFLINE;
			break;
		}
	}

exit:
	xnvme_buf_virt_free(rprt);

	pthread_mutex_lock(&g_serialize);
	xnvme_dev_close(dev);
	pthread_mutex_unlock(&g_serialize);

	XNVME_DEBUG("err: %d, nr_zones: %d", err, (int)nr_zones);

	return err ? err : (int)nr_zones;
}

static int
xnvme_fioe_reset_wp(struct thread_data *td, struct fio_file *f, uint64_t offset,
		    uint64_t length)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = &xd->files[f->fileno];
	uint64_t first, last;
	uint32_t nsid;
	int err = 0;

	XNVME_DEBUG("Resetting the write-pointer...");

	assert(fwrap->dev);
	assert(fwrap->geo);

	nsid = xnvme_dev_get_nsid(fwrap->dev);

	first = ((offset >> fwrap->ssw) / fwrap->geo->nsect) * fwrap->geo->nsect;
	last = (((offset + length) >> fwrap->ssw) / fwrap->geo->nsect) * fwrap->geo->nsect;
	for (uint64_t zslba = first; zslba <= last; zslba += fwrap->geo->nsect) {
		struct xnvme_req req = { 0 };

		err = znd_cmd_mgmt_send(fwrap->dev, nsid, zslba, ZND_SEND_RESET,
					0x0, NULL, XNVME_CMD_SYNC, &req);
		if (err || xnvme_req_cpl_status(&req)) {
			err = err ? err : -EIO;
			goto exit;
		}
	}

exit:
	return err;
}

struct ioengine_ops ioengine = {
	.name			= "xnvme",
	.version		= FIO_IOOPS_VERSION,
	.options		= options,
	.option_struct_size	= sizeof(struct xnvme_fioe_options),
	.flags			= \
		FIO_DISKLESSIO | \
		FIO_NODISKUTIL | \
		FIO_NOEXTEND | \
		FIO_MEMALIGN | \
		FIO_RAWIO,

	.cleanup	= xnvme_fioe_cleanup,
	.init		= xnvme_fioe_init,

	.iomem_free	= xnvme_fioe_iomem_free,
	.iomem_alloc	= xnvme_fioe_iomem_alloc,

	.io_u_free	= xnvme_fioe_io_u_free,
	.io_u_init	= xnvme_fioe_io_u_init,

	.event		= xnvme_fioe_event,
	.getevents	= xnvme_fioe_getevents,
	.queue		= xnvme_fioe_queue,

	.close_file	= xnvme_fioe_close,
	.open_file	= xnvme_fioe_open,

	.invalidate		= xnvme_fioe_invalidate,
	.get_zoned_model	= xnvme_fioe_get_zoned_model,
	.report_zones		= xnvme_fioe_report_zones,
	.reset_wp		= xnvme_fioe_reset_wp,
};
