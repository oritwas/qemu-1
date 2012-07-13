/*
 * Image mirroring
 *
 * Copyright Red Hat, Inc. 2012
 *
 * Authors:
 *  Paolo Bonzini  <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "trace.h"
#include "blockjob.h"
#include "block_int.h"
#include "qemu/ratelimit.h"
#include "bitmap.h"

#define SLICE_TIME 100000000ULL /* ns */

typedef struct MirrorBlockJob {
    BlockJob common;
    RateLimit limit;
    BlockDriverState *target;
    MirrorSyncMode mode;
    BlockdevOnError on_source_error, on_target_error;
    bool synced;
    bool complete;
    int64_t sector_num;
    int64_t granularity;
    size_t buf_size;
    unsigned long *cow_bitmap;
    HBitmapIter hbi;
    uint8_t *buf;

    int in_flight;
    int ret;
} MirrorBlockJob;

typedef struct MirrorOp {
    MirrorBlockJob *s;
    QEMUIOVector qiov;
    struct iovec iov;
    int64_t sector_num;
    int nb_sectors;
} MirrorOp;

static BlockErrorAction mirror_error_action(MirrorBlockJob *s, bool read,
                                            int error)
{
    s->synced = false;
    if (read) {
        return block_job_error_action(&s->common, s->common.bs,
                                      s->on_source_error, true, error);
    } else {
        return block_job_error_action(&s->common, s->target,
                                      s->on_target_error, false, error);
    }
}

static void mirror_iteration_done(MirrorOp *op)
{
    MirrorBlockJob *s = op->s;

    s->in_flight--;
    trace_mirror_iteration_done(s, op->sector_num, op->nb_sectors);
    g_slice_free(MirrorOp, op);
    qemu_coroutine_enter(s->common.co, NULL);
}

static void mirror_write_complete(void *opaque, int ret)
{
    MirrorOp *op = opaque;
    MirrorBlockJob *s = op->s;
    if (ret < 0) {
        BlockDriverState *source = s->common.bs;
        BlockErrorAction action;

        bdrv_set_dirty(source, op->sector_num, op->nb_sectors);
        action = mirror_error_action(s, false, -ret);
        if (action == BDRV_ACTION_REPORT && s->ret >= 0) {
            s->ret = ret;
        }
    }
    mirror_iteration_done(op);
}

static void mirror_read_complete(void *opaque, int ret)
{
    MirrorOp *op = opaque;
    MirrorBlockJob *s = op->s;
    if (ret < 0) {
        BlockDriverState *source = s->common.bs;
        BlockErrorAction action;

        bdrv_set_dirty(source, op->sector_num, op->nb_sectors);
        action = mirror_error_action(s, true, -ret);
        if (action == BDRV_ACTION_REPORT && s->ret >= 0) {
            s->ret = ret;
        }

        mirror_iteration_done(op);
        return;
    }
    bdrv_aio_writev(s->target, op->sector_num, &op->qiov, op->nb_sectors,
                    mirror_write_complete, op);
}

static void coroutine_fn mirror_iteration(MirrorBlockJob *s)
{
    BlockDriverState *source = s->common.bs;
    int nb_sectors, nb_sectors_chunk;
    int64_t end, sector_num, cluster_num;
    MirrorOp *op;

    s->sector_num = hbitmap_iter_next(&s->hbi);
    if (s->sector_num < 0) {
        bdrv_dirty_iter_init(source, &s->hbi);
        s->sector_num = hbitmap_iter_next(&s->hbi);
        trace_mirror_restart_iter(s, bdrv_get_dirty_count(source));
        assert(s->sector_num >= 0);
    }

    /* If we have no backing file yet in the destination, and the cluster size
     * is very large, we need to do COW ourselves.  The first time a cluster is
     * copied, copy it entirely.
     *
     * Because both the granularity and the cluster size are powers of two, the
     * number of sectors to copy cannot exceed one cluster.
     */
    sector_num = s->sector_num;
    nb_sectors_chunk = nb_sectors = s->granularity >> BDRV_SECTOR_BITS;
    cluster_num = sector_num / nb_sectors_chunk;
    if (s->cow_bitmap && !test_bit(cluster_num, s->cow_bitmap)) {
        trace_mirror_cow(s, sector_num);
        bdrv_round_to_clusters(s->target,
                               sector_num, nb_sectors_chunk,
                               &sector_num, &nb_sectors);
        bitmap_set(s->cow_bitmap, sector_num / nb_sectors_chunk,
                   nb_sectors / nb_sectors_chunk);
    }

    end = s->common.len >> BDRV_SECTOR_BITS;
    nb_sectors = MIN(nb_sectors, end - sector_num);

    /* Allocate a MirrorOp that is used as an AIO callback.  */
    op = g_slice_new(MirrorOp);
    op->s = s;
    op->iov.iov_base = s->buf;
    op->iov.iov_len  = nb_sectors * 512;
    op->sector_num = sector_num;
    op->nb_sectors = nb_sectors;
    qemu_iovec_init_external(&op->qiov, &op->iov, 1);

    bdrv_reset_dirty(source, sector_num, nb_sectors);

    /* Copy the dirty cluster.  */
    s->in_flight++;
    trace_mirror_one_iteration(s, sector_num, nb_sectors);
    bdrv_aio_readv(source, sector_num, &op->qiov, nb_sectors,
                   mirror_read_complete, op);
}

static void mirror_drain(MirrorBlockJob *s)
{
    while (s->in_flight > 0) {
        qemu_coroutine_yield();
    }
}

static void coroutine_fn mirror_run(void *opaque)
{
    MirrorBlockJob *s = opaque;
    BlockDriverState *bs = s->common.bs;
    int64_t sector_num, end, nb_sectors_chunk, length;
    uint64_t last_pause_ns;
    BlockDriverInfo bdi;
    char backing_filename[1024];
    int ret = 0;
    int n;

    if (block_job_is_cancelled(&s->common)) {
        goto immediate_exit;
    }

    s->common.len = bdrv_getlength(bs);
    if (s->common.len < 0) {
        block_job_completed(&s->common, s->common.len);
        return;
    }

    /* If we have no backing file yet in the destination, we cannot let
     * the destination do COW.  Instead, we copy sectors around the
     * dirty data if needed.  We need a bitmap to do that.
     */
    bdrv_get_backing_filename(s->target, backing_filename,
                              sizeof(backing_filename));
    if (backing_filename[0] && !s->target->backing_hd) {
        bdrv_get_info(s->target, &bdi);
        if (s->buf_size < bdi.cluster_size) {
            s->buf_size = bdi.cluster_size;
            length = (bdrv_getlength(bs) + s->granularity - 1) / s->granularity;
            s->cow_bitmap = bitmap_new(length);
        }
    }

    end = s->common.len >> BDRV_SECTOR_BITS;
    s->buf = qemu_blockalign(bs, s->buf_size);
    nb_sectors_chunk = s->granularity >> BDRV_SECTOR_BITS;

    if (s->mode != MIRROR_SYNC_MODE_NONE) {
        /* First part, loop on the sectors and initialize the dirty bitmap.  */
        BlockDriverState *base;
        base = s->mode == MIRROR_SYNC_MODE_FULL ? NULL : bs->backing_hd;
        for (sector_num = 0; sector_num < end; ) {
            int64_t next = (sector_num | (nb_sectors_chunk - 1)) + 1;
            ret = bdrv_co_is_allocated_above(bs, base,
                                             sector_num, next - sector_num, &n);

            if (ret < 0) {
                goto immediate_exit;
            }

            assert(n > 0);
            if (ret == 1) {
                bdrv_set_dirty(bs, sector_num, n);
                sector_num = next;
            } else {
                sector_num += n;
            }
        }
    }

    bdrv_dirty_iter_init(bs, &s->hbi);
    last_pause_ns = qemu_get_clock_ns(rt_clock);
    for (;;) {
        uint64_t delay_ns;
        int64_t cnt;
        bool should_complete;

        if (s->ret < 0) {
            ret = s->ret;
            break;
        }

        cnt = bdrv_get_dirty_count(bs);

        /* Note that even when no rate limit is applied we need to yield
         * periodically with no pending I/O so that qemu_aio_flush() returns.
         * We do so every SLICE_TIME milliseconds, or when there is an error,
         * or when the source is clean, whichever comes first.
         */
        if (qemu_get_clock_ns(rt_clock) - last_pause_ns < SLICE_TIME &&
            s->common.iostatus == BLOCK_DEVICE_IO_STATUS_OK) {
            if (s->in_flight > 0) {
                trace_mirror_yield(s, s->in_flight, cnt);
                qemu_coroutine_yield();
                continue;
            } else if (cnt != 0) {
                mirror_iteration(s);
                continue;
            }
        }

        should_complete = false;
        if (s->in_flight == 0 && cnt == 0) {
            trace_mirror_before_flush(s);
            ret = bdrv_flush(s->target);
            if (ret < 0) {
                if (mirror_error_action(s, false, -ret) == BDRV_ACTION_REPORT) {
                    break;
                }
            } else {
                /* We're out of the streaming phase.  From now on, if the job
                 * is cancelled we will actually complete all pending I/O and
                 * report completion.  This way, block-job-cancel will leave
                 * the target in a consistent state.
                 */
                s->common.offset = end * BDRV_SECTOR_SIZE;
                if (!s->synced) {
                    block_job_ready(&s->common);
                    s->synced = true;
                }

                should_complete = block_job_is_cancelled(&s->common) || s->complete;
                cnt = bdrv_get_dirty_count(bs);
            }
        }

        if (cnt == 0 && should_complete) {
            /* The dirty bitmap is not updated while operations are pending.
             * If we're about to exit, wait for pending operations before
             * calling bdrv_get_dirty_count(bs), or we may exit while the
             * source has dirty data to copy!
             *
             * Note that I/O can be submitted by the guest while
             * mirror_populate runs.
             */
            trace_mirror_before_drain(s, cnt);
            bdrv_drain_all();
            cnt = bdrv_get_dirty_count(bs);
        }

        ret = 0;
        trace_mirror_before_sleep(s, cnt, s->synced);
        if (!s->synced) {
            /* Publish progress */
            s->common.offset = (end - cnt) * BDRV_SECTOR_SIZE;

            if (s->common.speed) {
                delay_ns = ratelimit_calculate_delay(&s->limit, nb_sectors_chunk);
            } else {
                delay_ns = 0;
            }

            block_job_sleep_ns(&s->common, rt_clock, delay_ns);
            if (block_job_is_cancelled(&s->common)) {
                break;
            }
        } else if (!should_complete) {
            delay_ns = (s->in_flight == 0 && cnt == 0 ? SLICE_TIME : 0);
            block_job_sleep_ns(&s->common, rt_clock, delay_ns);
        } else if (cnt == 0) {
            /* The two disks are in sync.  Exit and report successful
             * completion.
             */
            assert(QLIST_EMPTY(&bs->tracked_requests));
            s->common.cancelled = false;
            break;
        }
        last_pause_ns = qemu_get_clock_ns(rt_clock);
    }

immediate_exit:
    if (s->in_flight > 0) {
        /* We get here only if something went wrong.  Either the job failed,
         * or it was cancelled prematurely so that we do not guarantee that
         * the target is a copy of the source.
         */
        assert(ret < 0 || (!s->synced && block_job_is_cancelled(&s->common)));
        mirror_drain(s);
    }

    assert(s->in_flight == 0);
    g_free(s->buf);
    g_free(s->cow_bitmap);
    bdrv_set_dirty_tracking(bs, 0);
    bdrv_iostatus_disable(s->target);
    if (s->complete && ret == 0) {
        bdrv_swap(s->target, s->common.bs);
    }
    bdrv_close(s->target);
    bdrv_delete(s->target);
    block_job_completed(&s->common, ret);
}

static void mirror_set_speed(BlockJob *job, int64_t speed, Error **errp)
{
    MirrorBlockJob *s = container_of(job, MirrorBlockJob, common);

    if (speed < 0) {
        error_set(errp, QERR_INVALID_PARAMETER, "speed");
        return;
    }
    ratelimit_set_speed(&s->limit, speed / BDRV_SECTOR_SIZE, SLICE_TIME);
}

static void mirror_iostatus_reset(BlockJob *job)
{
    MirrorBlockJob *s = container_of(job, MirrorBlockJob, common);

    bdrv_iostatus_reset(s->target);
}

static void mirror_complete(BlockJob *job, Error **errp)
{
    MirrorBlockJob *s = container_of(job, MirrorBlockJob, common);
    int ret;

    ret = bdrv_open_backing_file(s->target);
    if (ret < 0) {
        char backing_filename[PATH_MAX];
        bdrv_get_full_backing_filename(s->target, backing_filename,
                                       sizeof(backing_filename));
        error_set(errp, QERR_OPEN_FILE_FAILED, backing_filename);
        return;
    }
    if (!s->synced) {
        error_set(errp, QERR_BLOCK_JOB_NOT_READY, job->bs->device_name);
        return;
    }

    s->complete = true;
    block_job_resume(job);
}

static BlockJobType mirror_job_type = {
    .instance_size = sizeof(MirrorBlockJob),
    .job_type      = "mirror",
    .set_speed     = mirror_set_speed,
    .iostatus_reset= mirror_iostatus_reset,
    .complete      = mirror_complete,
};

void mirror_start(BlockDriverState *bs, BlockDriverState *target,
                  int64_t speed, int64_t granularity, int64_t buf_size,
                  MirrorSyncMode mode, BlockdevOnError on_source_error,
                  BlockdevOnError on_target_error,
                  BlockDriverCompletionFunc *cb,
                  void *opaque, Error **errp)
{
    MirrorBlockJob *s;

    if (granularity == 0) {
        /* Choose the default granularity based on the target file's cluster
         * size, clamped between 4k and 64k.  */
        BlockDriverInfo bdi;
        if (bdrv_get_info(target, &bdi) >= 0 && bdi.cluster_size != 0) {
            granularity = MAX(4096, bdi.cluster_size);
            granularity = MIN(65536, granularity);
        } else {
            granularity = 65536;
        }
    }

    assert ((granularity & (granularity - 1)) == 0);

    if ((on_source_error == BLOCKDEV_ON_ERROR_STOP ||
         on_source_error == BLOCKDEV_ON_ERROR_ENOSPC) &&
        !bdrv_iostatus_is_enabled(bs)) {
        error_set(errp, QERR_INVALID_PARAMETER, "on-source-error");
        return;
    }

    s = block_job_create(&mirror_job_type, bs, speed, cb, opaque, errp);
    if (!s) {
        return;
    }

    s->on_source_error = on_source_error;
    s->on_target_error = on_target_error;
    s->target = target;
    s->mode = mode;
    s->granularity = granularity;
    s->buf_size = MAX(buf_size, granularity);

    bdrv_set_dirty_tracking(bs, granularity);
    bdrv_set_enable_write_cache(s->target, true);
    bdrv_set_on_error(s->target, on_target_error, on_target_error);
    bdrv_iostatus_enable(s->target);
    s->common.co = qemu_coroutine_create(mirror_run);
    trace_mirror_start(bs, s, s->common.co, opaque);
    qemu_coroutine_enter(s->common.co, s);
}
