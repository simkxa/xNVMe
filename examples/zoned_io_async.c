// Copyright (C) Simon A. F. Lund <simon.lund@samsung.com>
// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <errno.h>
#include <libxnvme.h>
#include <libxnvmec.h>
#include <libxnvme_util.h>
#include <libznd.h>
#include <time.h>

#define DEFAULT_QD 8

struct cb_args {
	uint32_t ecount;
	uint32_t completed;
	uint32_t submitted;
};

static void
cb_pool(struct xnvme_req *req, void *cb_arg)
{
	struct cb_args *cb_args = cb_arg;

	cb_args->completed += 1;

	if (xnvme_req_cpl_status(req)) {
		xnvme_req_pr(req, XNVME_PR_DEF);
		cb_args->ecount += 1;
	}

	SLIST_INSERT_HEAD(&req->pool->head, req, link);
}

static int
sub_async_read(struct xnvmec *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	const struct xnvme_geo *geo = cli->args.geo;
	uint32_t nsid = cli->args.nsid;

	const uint32_t qd = cli->args.qdepth ? cli->args.qdepth : DEFAULT_QD;
	struct znd_descr zone = { 0 };

	int cmd_opts = XNVME_CMD_ASYNC;
	struct cb_args cb_args = { 0 };
	struct xnvme_async_ctx *ctx = NULL;
	struct xnvme_req_pool *reqs = NULL;

	size_t buf_nbytes;
	char *buf = NULL;
	char *payload = NULL;
	int err;

	if (!cli->given[XNVMEC_OPT_NSID]) {
		nsid = xnvme_dev_get_nsid(cli->args.dev);
	}
	if (cli->given[XNVMEC_OPT_SLBA]) {
		err = znd_descr_from_dev(dev, cli->args.slba, &zone);
		if (err) {
			xnvmec_perr("znd_descr_from_dev()", -err);
			goto exit;
		}
	} else {
		err = znd_descr_from_dev_in_state(dev, ZND_STATE_FULL, &zone);
		if (err) {
			xnvmec_perr("znd_descr_from_dev()", -err);
			goto exit;
		}
	}
	xnvmec_pinf("Using the following zone:");
	znd_descr_pr(&zone, XNVME_PR_DEF);

	buf_nbytes = zone.zcap * geo->lba_nbytes;

	xnvmec_pinf("Allocating and filling buf_nbytes: %zu", buf_nbytes);
	buf = xnvme_buf_alloc(dev, buf_nbytes, NULL);
	if (!buf) {
		err = -errno;
		xnvmec_perr("xnvme_buf_alloc()", err);
		goto exit;
	}
	err = xnvmec_buf_fill(buf, buf_nbytes, "zero");
	if (err) {
		xnvmec_perr("xnvmec_buf_fill()", err);
		goto exit;
	}

	xnvmec_pinf("Initializing async. context + alloc/init requests");
	err = xnvme_async_init(dev, &ctx, qd, 0);
	if (err) {
		xnvmec_perr("xnvme_async_init()", err);
		goto exit;
	}
	err = xnvme_req_pool_alloc(&reqs, qd + 1);
	if (err) {
		xnvmec_perr("xnvme_req_pool_alloc()", err);
		goto exit;
	}
	err = xnvme_req_pool_init(reqs, ctx, cb_pool, &cb_args);
	if (err) {
		xnvmec_perr("xnvme_req_pool_init()", err);
		goto exit;
	}

	xnvmec_pinf("Read at qdepth: %u to uri: '%s'", qd, cli->args.uri);

	xnvmec_timer_start(cli);

	payload = buf;
	for (uint64_t sect = 0; (sect < zone.zcap) && !cb_args.ecount;) {
		struct xnvme_req *req = SLIST_FIRST(&reqs->head);

		SLIST_REMOVE_HEAD(&reqs->head, link);

submit:
		err = xnvme_cmd_read(dev, nsid, zone.zslba + sect, 0, payload, NULL,
				     cmd_opts, req);
		switch (err) {
		case 0:
			cb_args.submitted += 1;
			goto next;

		case -EBUSY:
		case -EAGAIN:
			xnvme_async_poke(dev, ctx, 0);
			goto submit;

		default:
			xnvmec_perr("submission-error", EIO);
			goto exit;
		}

next:
		++sect;
		payload += geo->lba_nbytes;
	}

	err = xnvme_async_wait(dev, ctx);
	if (err < 0) {
		xnvmec_perr("xnvme_async_wait()", err);
		goto exit;
	}

	xnvmec_timer_stop(cli);

	if (cb_args.ecount) {
		err = -EIO;
		xnvmec_perr("got completion errors", err);
		goto exit;
	}

	xnvmec_timer_bw_pr(cli, "Wall-clock", zone.zcap * geo->lba_nbytes);

	if (cli->args.data_output) {
		xnvmec_pinf("Dumping nbytes: %zu, to: '%s'",
			    buf_nbytes, cli->args.data_output);
		err = xnvmec_buf_to_file(buf, buf_nbytes,
					 cli->args.data_output);
		if (err) {
			xnvmec_perr("xnvmec_buf_to_file()", err);
		}
	}

exit:
	xnvmec_pinf("cb_args: {submitted: %u, completed: %u, ecount: %u}",
		    cb_args.submitted, cb_args.completed,
		    cb_args.ecount);

	if (ctx) {
		int err_exit = xnvme_async_term(dev, ctx);
		if (err_exit) {
			xnvmec_perr("xnvme_async_term()", err_exit);
		}
	}
	xnvme_req_pool_free(reqs);
	xnvme_buf_free(dev, buf);

	return err < 0 ? err : 0;
}

static int
sub_async_write(struct xnvmec *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	const struct xnvme_geo *geo = cli->args.geo;
	uint32_t nsid = cli->args.nsid;

	const uint32_t qd = cli->args.qdepth ? cli->args.qdepth : DEFAULT_QD;
	struct znd_descr zone = { 0 };

	int cmd_opts = XNVME_CMD_ASYNC;
	struct cb_args cb_args = { 0 };
	struct xnvme_async_ctx *ctx = NULL;
	struct xnvme_req_pool *reqs = NULL;

	size_t buf_nbytes;
	char *buf = NULL;
	char *payload = NULL;
	int err;

	if (!cli->given[XNVMEC_OPT_NSID]) {
		nsid = xnvme_dev_get_nsid(cli->args.dev);
	}
	if (cli->given[XNVMEC_OPT_SLBA]) {
		err = znd_descr_from_dev(dev, cli->args.slba, &zone);
		if (err) {
			xnvmec_perr("znd_descr_from_dev()", -err);
			goto exit;
		}
	} else {
		err = znd_descr_from_dev_in_state(dev, ZND_STATE_EMPTY, &zone);
		if (err) {
			xnvmec_perr("znd_descr_from_dev()", -err);
			goto exit;
		}
	}
	xnvmec_pinf("Using the following zone:");
	znd_descr_pr(&zone, XNVME_PR_DEF);

	buf_nbytes = zone.zcap * geo->lba_nbytes;

	xnvmec_pinf("Allocating and filling buf_nbytes: %zu", buf_nbytes);
	buf = xnvme_buf_alloc(dev, buf_nbytes, NULL);
	if (!buf) {
		err = -errno;
		xnvmec_perr("xnvme_buf_alloc()", err);
		goto exit;
	}
	err = xnvmec_buf_fill(buf, buf_nbytes, cli->args.data_input ? cli->args.data_input : "anum");
	if (err) {
		xnvmec_perr("xnvmec_buf_fill()", err);
		goto exit;
	}

	xnvmec_pinf("Initializing async. context + alloc/init requests");
	err = xnvme_async_init(dev, &ctx, qd, 0);
	if (err) {
		xnvmec_perr("xnvme_async_init()", err);
		goto exit;
	}
	err = xnvme_req_pool_alloc(&reqs, qd + 1);
	if (err) {
		xnvmec_perr("xnvme_req_pool_alloc()", err);
		goto exit;
	}
	err = xnvme_req_pool_init(reqs, ctx, cb_pool, &cb_args);
	if (err) {
		xnvmec_perr("xnvme_req_pool_init()", err);
		goto exit;
	}

	xnvmec_pinf("Write at qdepth: %u to uri: '%s'", qd, cli->args.uri);
	xnvmec_timer_start(cli);

	payload = buf;
	for (uint64_t sect = 0; (sect < zone.zcap) && !cb_args.ecount;) {
		struct xnvme_req *req = SLIST_FIRST(&reqs->head);

		SLIST_REMOVE_HEAD(&reqs->head, link);

submit:
		err = xnvme_cmd_write(dev, nsid, zone.zslba + sect, 0, payload,
				      NULL, cmd_opts, req);
		switch (err) {
		case 0:
			cb_args.submitted += 1;
			goto next;

		case -EBUSY:
		case -EAGAIN:
			xnvme_async_poke(dev, ctx, 0);
			goto submit;

		default:
			xnvmec_perr("submission-error", EIO);
			goto exit;
		}

next:
		// Wait for completion to avoid racing zone.wp
		err = xnvme_async_wait(dev, ctx);
		if (err < 0) {
			xnvmec_perr("xnvme_async_wait()", err);
			goto exit;
		}

		++sect;
		payload += geo->lba_nbytes;
	}

	err = xnvme_async_wait(dev, ctx);
	if (err < 0) {
		xnvmec_perr("xnvme_async_wait()", err);
		goto exit;
	}

	xnvmec_timer_stop(cli);

	if (cb_args.ecount) {
		err = -EIO;
		xnvmec_perr("got completion errors", err);
		goto exit;
	}

	xnvmec_timer_bw_pr(cli, "Wall-clock", zone.zcap * geo->lba_nbytes);

exit:
	xnvmec_pinf("cb_args: {submitted: %u, completed: %u, ecount: %u}",
		    cb_args.submitted, cb_args.completed,
		    cb_args.ecount);

	if (ctx) {
		int err_exit = xnvme_async_term(dev, ctx);
		if (err_exit) {
			xnvmec_perr("xnvme_async_term()", err_exit);
		}
	}
	xnvme_req_pool_free(reqs);
	xnvme_buf_free(dev, buf);

	return err < 0 ? err : 0;
}

static int
sub_async_append(struct xnvmec *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	const struct xnvme_geo *geo = cli->args.geo;
	uint32_t nsid = cli->args.nsid;

	const uint32_t qd = cli->args.qdepth ? cli->args.qdepth : DEFAULT_QD;
	struct znd_descr zone = { 0 };

	int cmd_opts = XNVME_CMD_ASYNC;
	struct cb_args cb_args = { 0 };
	struct xnvme_async_ctx *ctx = NULL;
	struct xnvme_req_pool *reqs = NULL;

	size_t buf_nbytes;
	char *buf = NULL;
	char *payload = NULL;
	int err;

	if (!cli->given[XNVMEC_OPT_NSID]) {
		nsid = xnvme_dev_get_nsid(cli->args.dev);
	}
	if (cli->given[XNVMEC_OPT_SLBA]) {
		err = znd_descr_from_dev(dev, cli->args.slba, &zone);
		if (err) {
			xnvmec_perr("znd_descr_from_dev()", -err);
			goto exit;
		}
	} else {
		err = znd_descr_from_dev_in_state(dev, ZND_STATE_EMPTY, &zone);
		if (err) {
			xnvmec_perr("znd_descr_from_dev()", -err);
			goto exit;
		}
	}
	xnvmec_pinf("Using the following zone:");
	znd_descr_pr(&zone, XNVME_PR_DEF);

	buf_nbytes = zone.zcap * geo->lba_nbytes;

	xnvmec_pinf("Allocating and filling buf_nbytes: %zu", buf_nbytes);
	buf = xnvme_buf_alloc(dev, buf_nbytes, NULL);
	if (!buf) {
		err = -errno;
		xnvmec_perr("xnvme_buf_alloc()", err);
		goto exit;
	}
	err = xnvmec_buf_fill(buf, buf_nbytes, cli->args.data_input ? cli->args.data_input : "anum");
	if (err) {
		xnvmec_perr("xnvmec_buf_fill()", err);
		goto exit;
	}

	xnvmec_pinf("Initializing async. context + alloc/init requests");
	err = xnvme_async_init(dev, &ctx, qd, 0);
	if (err) {
		xnvmec_perr("xnvme_async_init()", err);
		goto exit;
	}
	err = xnvme_req_pool_alloc(&reqs, qd + 1);
	if (err) {
		xnvmec_perr("xnvme_req_pool_alloc()", err);
		goto exit;
	}
	err = xnvme_req_pool_init(reqs, ctx, cb_pool, &cb_args);
	if (err) {
		xnvmec_perr("xnvme_req_pool_init()", err);
		goto exit;
	}

	xnvmec_pinf("Append at qd(%u) to uri: '%s'", qd, cli->args.uri);

	xnvmec_timer_start(cli);

	payload = buf;
	for (uint64_t sect = 0; (sect < zone.zcap) && !cb_args.ecount;) {
		struct xnvme_req *req = SLIST_FIRST(&reqs->head);

		SLIST_REMOVE_HEAD(&reqs->head, link);

submit:
		err = znd_cmd_append(dev, nsid, zone.zslba, 0, payload,
				     NULL, cmd_opts, req);
		switch (err) {
		case 0:
			cb_args.submitted += 1;
			goto next;

		case -EBUSY:
		case -EAGAIN:
			xnvme_async_poke(dev, ctx, 0);
			goto submit;

		default:
			xnvmec_perr("submission-error", EIO);
			goto exit;
		}

next:
		++sect;
		payload += geo->lba_nbytes;
	}

	err = xnvme_async_wait(dev, ctx);
	if (err < 0) {
		xnvmec_perr("xnvme_async_wait()", err);
		goto exit;
	}

	xnvmec_timer_stop(cli);

	if (cb_args.ecount) {
		err = -EIO;
		xnvmec_perr("got completion errors", err);
		goto exit;
	}

	xnvmec_timer_bw_pr(cli, "Wall-clock", zone.zcap * geo->lba_nbytes);

exit:
	xnvmec_pinf("cb_args: {submitted: %u, completed: %u, ecount: %u}",
		    cb_args.submitted, cb_args.completed,
		    cb_args.ecount);

	if (ctx) {
		int err_exit = xnvme_async_term(dev, ctx);
		if (err_exit) {
			xnvmec_perr("xnvme_async_term()", err_exit);
		}
	}
	xnvme_req_pool_free(reqs);
	xnvme_buf_free(dev, buf);

	return err < 0 ? err : 0;
}

//
// Command-Line Interface (CLI) definition
//

static struct xnvmec_sub subs[] = {
	{
		"read", "Asynchronous Zone Read of an entire Zone",
		"Asynchronous Zone Read of an entire Zone", sub_async_read, {
			{XNVMEC_OPT_URI, XNVMEC_POSA},
			{XNVMEC_OPT_SLBA, XNVMEC_LOPT},
			{XNVMEC_OPT_QDEPTH, XNVMEC_LOPT},
			{XNVMEC_OPT_DATA_OUTPUT, XNVMEC_LOPT},
		}
	},

	{
		"write", "Asynchronous Zone Write until full",
		"Zone asynchronous Write until full", sub_async_write, {
			{XNVMEC_OPT_URI, XNVMEC_POSA},
			{XNVMEC_OPT_SLBA, XNVMEC_LOPT},
			{XNVMEC_OPT_QDEPTH, XNVMEC_LOPT},
			{XNVMEC_OPT_DATA_INPUT, XNVMEC_LOPT},
		}
	},

	{
		"append", "Asynchronous Zone Append until full",
		"Zone asynchronous Append until full", sub_async_append, {
			{XNVMEC_OPT_URI, XNVMEC_POSA},
			{XNVMEC_OPT_SLBA, XNVMEC_LOPT},
			{XNVMEC_OPT_QDEPTH, XNVMEC_LOPT},
			{XNVMEC_OPT_DATA_INPUT, XNVMEC_LOPT},
		}
	},
};

static struct xnvmec cli = {
	.title = "Zoned Synchronous IO Example",
	.descr_short =	"Synchronous IO: read / write / append, "
	"using 4k payload at QD1",
	.subs = subs,
	.nsubs = sizeof subs / sizeof(*subs),
};

int
main(int argc, char **argv)
{
	return xnvmec(&cli, argc, argv, XNVMEC_INIT_DEV_OPEN);
}
