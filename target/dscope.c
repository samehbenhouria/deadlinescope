/*
 * DeadlineScope — target collector for QNX 8.0
 *
 * A real-time quality gate for periodic tasks:
 *  - tasks register with a budget (period + max activation jitter)
 *    via native QNX message passing,
 *  - the collector drives activations with timer pulses on its own
 *    channel and measures every activation with ClockCycles(),
 *  - statistics are exposed live through the resource manager
 *    /dev/deadlinescope (read the current verdict table as text),
 *  - every sample is appended to a binary journal that the host tool
 *    (deadlinescope check) verifies against the budgets file.
 *
 * Demo mode (--demo): registers two periodic tasks, injects a CPU hog
 * at a higher priority halfway through, and prints the session table.
 *
 * Copyright (c) 2026 Sameh Ben Houria — MIT License (see LICENSE).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/resmgr.h>

/* ------------------------------------------------------------------ */
/* Shared constants                                                    */
/* ------------------------------------------------------------------ */

#define DS_MAX_TASKS        8
#define DS_NAME_LEN         24
#define DS_PULSE_BASE       (_PULSE_CODE_MINAVAIL + 4)
#define DS_MSG_REGISTER     0xD500
#define DS_JOURNAL_PATH     "/data/dscope.journal"
#define DS_HIST_BUCKETS     10

/* Histogram bucket upper bounds, nanoseconds */
static const int64_t ds_hist_bounds[DS_HIST_BUCKETS] = {
    10000, 50000, 100000, 250000, 500000,
    1000000, 2000000, 5000000, 10000000, INT64_MAX
};

/* Binary journal: 64-byte header, then fixed 32-byte records. */
struct ds_journal_header {
    char     magic[4];          /* "DSJ1" */
    uint32_t version;           /* 1 */
    uint64_t cycles_per_sec;
    uint64_t start_cycles;      /* collector start, cycles */
    uint8_t  reserved[40];
};                              /* exactly 64 bytes, no hidden padding */
#define DS_REC_NAME   1u
#define DS_REC_SAMPLE 2u
struct ds_journal_record {
    uint32_t type;              /* DS_REC_* */
    uint32_t task_id;
    union {
        char name[24];                      /* DS_REC_NAME */
        struct {                            /* DS_REC_SAMPLE */
            uint64_t expected_cycles;
            uint64_t actual_cycles;
            int64_t  jitter_ns;
        } sample;
    } u;
};

_Static_assert(sizeof(struct ds_journal_header) == 64, "journal header size");
_Static_assert(sizeof(struct ds_journal_record) == 32, "journal record size");

/* Registration message (client -> collector, MsgSend) */
struct ds_register_msg {
    uint16_t type;              /* DS_MSG_REGISTER */
    uint16_t reserved;
    char     name[DS_NAME_LEN];
    uint64_t period_ns;
    uint64_t jitter_budget_ns;
};
struct ds_register_reply {
    int32_t task_id;            /* <0 on error */
};

/* ------------------------------------------------------------------ */
/* Collector state                                                     */
/* ------------------------------------------------------------------ */

struct ds_task {
    int       used;
    char      name[DS_NAME_LEN];
    uint64_t  period_ns;
    uint64_t  jitter_budget_ns;
    timer_t   timer;
    uint64_t  next_expected_cycles;
    /* statistics */
    uint64_t  samples;
    int64_t   jitter_min_ns;
    int64_t   jitter_max_ns;
    int64_t   jitter_sum_ns;
    int64_t   worst_jitter_ns;
    uint64_t  worst_at_ms;      /* ms since collector start */
    uint32_t  hist[DS_HIST_BUCKETS];
    uint64_t  overruns;         /* samples beyond budget */
};

static struct ds_task  g_tasks[DS_MAX_TASKS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        g_cycles_per_sec;
static uint64_t        g_start_cycles;
static FILE           *g_journal;
static volatile int    g_stop;

static uint64_t cycles_to_ns(uint64_t cycles)
{
    return cycles * 1000000000ull / g_cycles_per_sec;
}

/* ------------------------------------------------------------------ */
/* Journal                                                             */
/* ------------------------------------------------------------------ */

static void journal_open(void)
{
    struct ds_journal_header hdr;

    g_journal = fopen(DS_JOURNAL_PATH, "wb");
    if (g_journal == NULL) {
        fprintf(stderr, "dscope: cannot open journal %s: %s\n",
                DS_JOURNAL_PATH, strerror(errno));
        return;
    }
    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, "DSJ1", 4);
    hdr.version = 1;
    hdr.cycles_per_sec = g_cycles_per_sec;
    hdr.start_cycles   = g_start_cycles;
    fwrite(&hdr, sizeof hdr, 1, g_journal);
}

static void journal_name(uint32_t task_id, const char *name)
{
    struct ds_journal_record rec;

    if (g_journal == NULL) return;
    memset(&rec, 0, sizeof rec);
    rec.type = DS_REC_NAME;
    rec.task_id = task_id;
    strncpy(rec.u.name, name, sizeof rec.u.name - 1);
    fwrite(&rec, sizeof rec, 1, g_journal);
}

static void journal_sample(uint32_t task_id, uint64_t expected,
                           uint64_t actual, int64_t jitter_ns)
{
    struct ds_journal_record rec;

    if (g_journal == NULL) return;
    memset(&rec, 0, sizeof rec);
    rec.type = DS_REC_SAMPLE;
    rec.task_id = task_id;
    rec.u.sample.expected_cycles = expected;
    rec.u.sample.actual_cycles   = actual;
    rec.u.sample.jitter_ns       = jitter_ns;
    fwrite(&rec, sizeof rec, 1, g_journal);
}

/* ------------------------------------------------------------------ */
/* Measurement core                                                    */
/* ------------------------------------------------------------------ */

static void record_sample(struct ds_task *t, uint32_t id,
                          uint64_t expected, uint64_t actual)
{
    int64_t jitter_ns = (int64_t)cycles_to_ns(actual - expected);
    int b;

    if ((int64_t)(actual - expected) < 0)
        jitter_ns = -(int64_t)cycles_to_ns(expected - actual);

    t->samples++;
    if (t->samples == 1 || jitter_ns < t->jitter_min_ns)
        t->jitter_min_ns = jitter_ns;
    if (jitter_ns > t->jitter_max_ns)
        t->jitter_max_ns = jitter_ns;
    t->jitter_sum_ns += jitter_ns;
    if (jitter_ns > t->worst_jitter_ns) {
        t->worst_jitter_ns = jitter_ns;
        t->worst_at_ms = cycles_to_ns(actual - g_start_cycles) / 1000000ull;
    }
    if ((uint64_t)jitter_ns > t->jitter_budget_ns)
        t->overruns++;
    for (b = 0; b < DS_HIST_BUCKETS; b++) {
        if (jitter_ns <= ds_hist_bounds[b]) { t->hist[b]++; break; }
    }
    journal_sample(id, expected, actual, jitter_ns);
}

/*
 * Measurement thread: receives timer pulses and registration messages
 * on the collector channel. This is the only writer of task statistics
 * (journal writes happen here too, so no extra locking is needed on
 * the sample path; the resource manager reads under g_lock).
 */
static int g_chid;
static int g_pulse_coid;    /* connection to our own channel, for pulses */

static void *measure_thread(void *arg)
{
    struct _pulse pulse;
    struct ds_register_msg msg;
    struct ds_register_reply reply;
    struct iovec iov;
    struct sched_param sp;
    int rcvid;

    (void)arg;
    /* The measurer must run promptly when a pulse lands: FIFO 28,
     * below the fault-injection hog (40), above everything else. */
    sp.sched_priority = 28;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    for (;;) {
        /* Registration messages and pulses share the channel; use a
         * raw MsgReceive and discriminate by size/type. */
        memset(&msg, 0, sizeof msg);
        SETIOV(&iov, &msg, sizeof msg);
        rcvid = MsgReceivev(g_chid, &iov, 1, NULL);
        if (rcvid == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (rcvid == 0) {
            /* Pulse: code identifies the task. */
            int id;
            uint64_t now, expected;
            struct ds_task *t;

            memcpy(&pulse, &msg, sizeof pulse);
            id = pulse.code - DS_PULSE_BASE;
            if (id < 0 || id >= DS_MAX_TASKS) continue;
            t = &g_tasks[id];
            if (!t->used) continue;

            now = ClockCycles();
            expected = t->next_expected_cycles;
            t->next_expected_cycles +=
                t->period_ns * g_cycles_per_sec / 1000000000ull;
            pthread_mutex_lock(&g_lock);
            record_sample(t, (uint32_t)id, expected, now);
            pthread_mutex_unlock(&g_lock);
            if (g_stop) break;
            continue;
        }
        /* A real message: only registrations are defined in v1. */
        if (msg.type == DS_MSG_REGISTER) {
            int id = -1, i;
            pthread_mutex_lock(&g_lock);
            for (i = 0; i < DS_MAX_TASKS; i++) {
                if (!g_tasks[i].used) { id = i; break; }
            }
            if (id >= 0) {
                struct ds_task *t = &g_tasks[id];
                struct sigevent ev;
                struct _itimer it;

                memset(t, 0, sizeof *t);
                t->used = 1;
                strncpy(t->name, msg.name, DS_NAME_LEN - 1);
                t->period_ns = msg.period_ns;
                t->jitter_budget_ns = msg.jitter_budget_ns;
                t->jitter_min_ns = INT64_MAX;

                /* The pulse event needs a connection id to the
                 * channel, not the channel id itself. Pulse priority
                 * 28 matches the measurement thread's FIFO level. */
                SIGEV_PULSE_INIT(&ev, g_pulse_coid, 28,
                                 DS_PULSE_BASE + id, 0);
                /* QNX 8: TimerCreate returns the timer id directly. */
                t->timer = TimerCreate(CLOCK_MONOTONIC, &ev);
                if (t->timer != -1) {
                    uint64_t first = ClockCycles() +
                        t->period_ns * g_cycles_per_sec / 1000000000ull;
                    t->next_expected_cycles = first;
                    /* QNX 8: TimerSettime takes struct _itimer
                     * (nsec + interval, both in nanoseconds).
                     * Relative first expiry; next_expected_cycles was
                     * set one period ahead, matching this schedule. */
                    it.nsec = t->period_ns;
                    it.interval_nsec = t->period_ns;
                    if (TimerSettime(t->timer, 0, &it, NULL) == -1) {
                        t->used = 0;
                        id = -1;
                    } else {
                        journal_name((uint32_t)id, t->name);
                    }
                } else {
                    t->used = 0;
                    id = -1;
                }
            }
            pthread_mutex_unlock(&g_lock);
            reply.task_id = id;
            MsgReply(rcvid, EOK, &reply, sizeof reply);
        } else {
            MsgError(rcvid, ENOSYS);
        }
    }
    return NULL;
}

/* Client-side helper: register a task with the collector process. */
static int dscope_register(int coid, const char *name,
                           uint64_t period_ns, uint64_t jitter_budget_ns)
{
    struct ds_register_msg msg;
    struct ds_register_reply reply;

    memset(&msg, 0, sizeof msg);
    msg.type = DS_MSG_REGISTER;
    strncpy(msg.name, name, DS_NAME_LEN - 1);
    msg.period_ns = period_ns;
    msg.jitter_budget_ns = jitter_budget_ns;
    if (MsgSend(coid, &msg, sizeof msg, &reply, sizeof reply) == -1)
        return -1;
    return reply.task_id;
}

/* ------------------------------------------------------------------ */
/* Resource manager /dev/deadlinescope                                */
/* ------------------------------------------------------------------ */

static char g_snapshot[8192];

static int build_snapshot(void)
{
    int n = 0, i;

    pthread_mutex_lock(&g_lock);
    n += snprintf(g_snapshot + n, sizeof g_snapshot - n,
        "DeadlineScope — live verdicts (budgets in microseconds)\n"
        "%-16s %8s %9s %9s %9s %9s %8s  %s\n",
        "task", "samples", "budget", "min", "avg", "worst", "overrun",
        "verdict");
    for (i = 0; i < DS_MAX_TASKS; i++) {
        struct ds_task *t = &g_tasks[i];
        int64_t avg;
        if (!t->used) continue;
        avg = t->samples ? t->jitter_sum_ns / (int64_t)t->samples : 0;
        n += snprintf(g_snapshot + n, sizeof g_snapshot - n,
            "%-16s %8" PRIu64 " %9" PRIu64 " %9" PRId64 " %9" PRId64
            " %9" PRId64 " %8" PRIu64 "  %s\n",
            t->name, t->samples,
            t->jitter_budget_ns / 1000,
            t->samples ? t->jitter_min_ns / 1000 : 0,
            avg / 1000, t->worst_jitter_ns / 1000, t->overruns,
            /* Gate rule (defaults; the host tool applies the exact
             * budgets-file values): FAIL when the overrun ratio
             * exceeds 1% or the worst sample breaches 3x budget. */
            (t->samples && (t->overruns * 100 > t->samples ||
             (uint64_t)t->worst_jitter_ns > 3 * t->jitter_budget_ns))
                ? "FAIL" : "PASS");
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg,
                   RESMGR_OCB_T *ocb_arg)
{
    iofunc_ocb_t *ocb = (iofunc_ocb_t *)ocb_arg;
    int n, len;

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if ((msg->i.nbytes == 0))
        return _RESMGR_NPARTS(0);

    if (ocb->offset == 0)
        build_snapshot();
    len = (int)strlen(g_snapshot);
    n = len - (int)ocb->offset;
    if (n <= 0)
        return _RESMGR_NPARTS(0);
    if (n > (int)msg->i.nbytes)
        n = msg->i.nbytes;
    _IO_SET_READ_NBYTES(ctp, n);
    SETIOV(ctp->iov, g_snapshot + ocb->offset, n);
    ocb->offset += n;
    /* EOF is signalled by returning 0 parts on the next read. */
    return _RESMGR_NPARTS(1);
}

static void *resmgr_thread(void *arg)
{
    dispatch_t *dpp;
    resmgr_attr_t attr;
    resmgr_connect_funcs_t connect;
    resmgr_io_funcs_t io;
    dispatch_context_t *ctx;
    int id;

    (void)arg;
    dpp = dispatch_create();
    if (dpp == NULL) { perror("dispatch_create"); return NULL; }
    memset(&attr, 0, sizeof attr);
    attr.nparts_max = 1;
    attr.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect,
                     _RESMGR_IO_NFUNCS, &io);
    io.read = io_read;
    id = resmgr_attach(dpp, &attr, "/dev/deadlinescope", _FTYPE_ANY, 0,
                       &connect, &io, NULL);
    if (id == -1) { perror("resmgr_attach"); return NULL; }
    ctx = dispatch_context_alloc(dpp);
    while (!g_stop) {
        if (dispatch_block(ctx) != NULL)
            dispatch_handler(ctx);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Journal dump (base64 to console, for serial-log capture)            */
/* ------------------------------------------------------------------ */

static void dump_journal_b64(void)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    FILE *f = fopen(DS_JOURNAL_PATH, "rb");
    unsigned char buf[3 * 25];      /* 75 bytes -> 100 b64 chars */
    size_t n, i;

    if (f == NULL) return;
    printf("===DSCOPE JOURNAL B64===\n");
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        for (i = 0; i < n; i += 3) {
            uint32_t v = buf[i] << 16;
            int rem = (int)(n - i);
            if (rem > 1) v |= buf[i + 1] << 8;
            if (rem > 2) v |= buf[i + 2];
            putchar(b64[(v >> 18) & 63]);
            putchar(b64[(v >> 12) & 63]);
            putchar(rem > 1 ? b64[(v >> 6) & 63] : '=');
            putchar(rem > 2 ? b64[v & 63] : '=');
        }
        putchar('\n');
        fflush(stdout);
    }
    printf("===END DSCOPE JOURNAL B64===\n");
    fflush(stdout);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Demo workload                                                       */
/* ------------------------------------------------------------------ */

/* A higher-priority spinner: the injected fault. */
static volatile int g_hog_on;
static void *hog_thread(void *arg)
{
    struct sched_param sp;
    uint64_t cps = g_cycles_per_sec;

    (void)arg;
    sp.sched_priority = 40;             /* above the demo tasks */
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    while (!g_stop) {
        if (g_hog_on) {
            uint64_t start = ClockCycles();
            /* Fault mode: ~5 ms burn slices with a 1 ms breather
             * (~83% duty). A 25% duty hog was measured nearly
             * invisible on this rig; a 100% duty hog starved the
             * demo driver itself. The breather keeps the system
             * alive while every activation that lands inside a
             * slice waits it out. */
            while (ClockCycles() - start < cps / 200)
                ;
            {
                struct timespec ts = { 0, 1000000 };
                nanosleep(&ts, NULL);
            }
        } else {
            struct timespec ts = { 0, 15000000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* Simulated periodic work executed by worker threads. In v1 the
 * workers simply burn a short, bounded amount of CPU per activation
 * notice; the measured quantity is the activation jitter itself,
 * recorded by the collector from the timer pulses. */
static void burn_cycles(uint64_t cycles)
{
    uint64_t start = ClockCycles();
    while (ClockCycles() - start < cycles)
        ;
}

int main(int argc, char **argv)
{
    int demo = 0, seconds = 20, i, no_fault = 0;
    pthread_t th_measure, th_resmgr, th_hog;
    int coid;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) demo = 1;
        else if (strcmp(argv[i], "--no-fault") == 0) no_fault = 1;
        else if (strncmp(argv[i], "--seconds=", 10) == 0)
            seconds = atoi(argv[i] + 10);
    }

    g_cycles_per_sec = SYSPAGE_ENTRY(qtime)->cycles_per_sec;

    /* The stock 1 ms tick quantizes every timer activation and sets
     * a ~1.6 ms jitter floor under virtualization. Ask the kernel
     * for a 100 us clock period; keep going if refused. */
    {
        struct _clockperiod period;
        period.nsec = 100000;
        period.fract = 0;
        if (ClockPeriod(CLOCK_MONOTONIC, &period, NULL, 0) == -1)
            fprintf(stderr, "dscope: ClockPeriod(100us) refused: %s\n",
                    strerror(errno));
    }

    g_start_cycles = ClockCycles();
    g_chid = ChannelCreate(0);
    if (g_chid == -1) { perror("ChannelCreate"); return 1; }
    g_pulse_coid = ConnectAttach(0, 0, g_chid, _NTO_SIDE_CHANNEL, 0);
    if (g_pulse_coid == -1) { perror("ConnectAttach"); return 1; }
    journal_open();

    pthread_create(&th_measure, NULL, measure_thread, NULL);
    pthread_create(&th_resmgr, NULL, resmgr_thread, NULL);

    printf("dscope: collector up, cycles_per_sec=%" PRIu64 "\n",
           g_cycles_per_sec);
    fflush(stdout);

    if (demo) {
        int ctl, tel;
        struct timespec ts;
        int half = seconds / 2;

        coid = ConnectAttach(0, 0, g_chid, _NTO_SIDE_CHANNEL, 0);
        ctl = dscope_register(coid, "control-loop",
                              20000000ull, 3500000ull);   /* 20 ms, 3.5 ms */
        tel = dscope_register(coid, "telemetry",
                              100000000ull, 10000000ull); /* 100 ms, 10 ms */
        printf("dscope: demo tasks registered (control-loop=%d, telemetry=%d)\n",
               ctl, tel);
        fflush(stdout);

        if (!no_fault) {
            /* One hog per CPU: on an SMP guest a single spinner only
             * occupies one core and the measurer escapes to another. */
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            long c;
            if (ncpu < 1) ncpu = 1;
            for (c = 0; c < ncpu; c++)
                pthread_create(&th_hog, NULL, hog_thread, NULL);
        }

        ts.tv_sec = half; ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
        if (!no_fault) {
            printf("dscope: injecting fault (runaway hog, priority 40, all CPUs)\n");
            fflush(stdout);
            g_hog_on = 1;
        }
        /* Workers burn a little CPU so the interference is visible
         * in real work, not only in pulse delivery. */
        {
            uint64_t until = ClockCycles() +
                (uint64_t)(seconds - half) * g_cycles_per_sec;
            while (ClockCycles() < until) {
                burn_cycles(g_cycles_per_sec / 1000);   /* 1 ms */
                ts.tv_sec = 0; ts.tv_nsec = 9000000;    /* 9 ms */
                nanosleep(&ts, NULL);
            }
        }
        g_hog_on = 0;

        /* Freeze the sample stream before the verdict: destroy the
         * timers, let in-flight pulses drain, then snapshot. Without
         * this the console table and the journal can disagree by the
         * samples that land in between. */
        {
            int ti;
            struct timespec drain = { 0, 150000000 };
            pthread_mutex_lock(&g_lock);
            for (ti = 0; ti < DS_MAX_TASKS; ti++)
                if (g_tasks[ti].used)
                    TimerDestroy(g_tasks[ti].timer);
            pthread_mutex_unlock(&g_lock);
            nanosleep(&drain, NULL);
        }
        g_stop = 1;

        /* Final verdict table on the console (serial log keeps it). */
        build_snapshot();
        printf("\n===DSCOPE SESSION===\n%s===END DSCOPE SESSION===\n",
               g_snapshot);
        fflush(stdout);
        if (g_journal) { fflush(g_journal); fclose(g_journal); g_journal = NULL; }
        dump_journal_b64();
    } else {
        /* Service mode: run until killed. */
        for (;;) pause();
    }

    if (g_journal) { fflush(g_journal); fclose(g_journal); }
    return 0;
}
