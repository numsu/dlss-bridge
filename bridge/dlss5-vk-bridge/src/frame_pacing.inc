// Slow-only presentation telemetry. A queue key keeps unrelated swapchains
// and devices from being combined into one misleading frame interval.
struct PresentPaceRec {
    VkQueue queue;
    LARGE_INTEGER last_start;
    UINT64 count;
    UINT64 window_count, gaps_25, gaps_50, calls_8;
    double interval_sum, interval_max, call_sum, call_max;
};
static PresentPaceRec g_present_pace[64] = {};
static SRWLOCK g_present_pace_lock = SRWLOCK_INIT;

static void ForgetPresentPace(VkQueue queue)
{
    AcquireSRWLockExclusive(&g_present_pace_lock);
    for (PresentPaceRec &entry : g_present_pace)
        if (entry.queue == queue) { entry = {}; break; }
    ReleaseSRWLockExclusive(&g_present_pace_lock);
}

static void NotePresentPace(VkQueue queue, const VkPresentInfoKHR *info,
                            VkResult result, const LARGE_INTEGER &started,
                            const LARGE_INTEGER &finished)
{
    LARGE_INTEGER frequency = {};
    if (!started.QuadPart || !finished.QuadPart ||
        !QueryPerformanceFrequency(&frequency) || !frequency.QuadPart) return;

    const double scale = 1000.0 / (double)frequency.QuadPart;
    const double blocked = (double)(finished.QuadPart - started.QuadPart) * scale;
    UINT64 sequence = 0;
    double interval = 0.0;
    bool report_window = false;
    PresentPaceRec window = {};
    AcquireSRWLockExclusive(&g_present_pace_lock);
    PresentPaceRec *rec = nullptr;
    for (PresentPaceRec &entry : g_present_pace)
        if (entry.queue == queue) { rec = &entry; break; }
    if (!rec)
        for (PresentPaceRec &entry : g_present_pace)
            if (entry.queue == VK_NULL_HANDLE) { rec = &entry; rec->queue = queue; break; }
    if (rec)
    {
        interval = rec->last_start.QuadPart ?
            (double)(started.QuadPart - rec->last_start.QuadPart) * scale : 0.0;
        rec->last_start = started;
        sequence = ++rec->count;
        if (sequence > 1)
        {
            ++rec->window_count;
            rec->interval_sum += interval;
            rec->call_sum += blocked;
            if (interval > rec->interval_max) rec->interval_max = interval;
            if (blocked > rec->call_max) rec->call_max = blocked;
            if (interval >= 25.0) ++rec->gaps_25;
            if (interval >= 50.0) ++rec->gaps_50;
            if (blocked >= 8.0) ++rec->calls_8;
            if (rec->window_count == 128)
            {
                window = *rec;
                rec->window_count = rec->gaps_25 = rec->gaps_50 = rec->calls_8 = 0;
                rec->interval_sum = rec->interval_max = rec->call_sum = rec->call_max = 0.0;
                report_window = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_present_pace_lock);

    if (sequence == 1)
        Log("[pace] first vkQueuePresentKHR queue=%p swapchains=%u bridge-frame=%llu",
            queue, info ? info->swapchainCount : 0,
            (unsigned long long)g_bridge.timeline);
    // 30 FPS's ordinary 33 ms interval is summarized instead of logged on
    // every frame. Individual records are reserved for real long gaps.
    if (interval >= 50.0 || blocked >= 25.0 || result < 0)
        Log("[pace] present #%llu queue=%p interval=%.3fms call=%.3fms result=%d swapchains=%u bridge-frame=%llu completed=%llu",
            (unsigned long long)sequence, queue, interval, blocked, (int)result,
            info ? info->swapchainCount : 0,
            (unsigned long long)g_bridge.timeline,
            (unsigned long long)g_bridge.frames_done);
    if (report_window)
        Log("[pace] queue=%p presents=%llu interval avg/max=%.3f/%.3fms gaps>=25/50ms=%llu/%llu call avg/max=%.3f/%.3fms calls>=8ms=%llu bridge-frame=%llu completed=%llu",
            queue, (unsigned long long)window.count,
            window.interval_sum / 128.0, window.interval_max,
            (unsigned long long)window.gaps_25, (unsigned long long)window.gaps_50,
            window.call_sum / 128.0, window.call_max,
            (unsigned long long)window.calls_8,
            (unsigned long long)g_bridge.timeline,
            (unsigned long long)g_bridge.frames_done);
}

// Evaluate cadence is a backup clock for engines that resolve present outside
// the injected hook. It also reveals gaps that happen before the DLSS pass.
struct NgxPaceRec {
    const NVSDK_NGX_Handle *feature;
    LARGE_INTEGER last_start;
    UINT64 count, window_count, gaps_25, gaps_50;
    double interval_sum, interval_max;
};
static NgxPaceRec g_ngx_pace[32] = {};
static SRWLOCK g_ngx_pace_lock = SRWLOCK_INIT;

static void NoteNgxPace(const NVSDK_NGX_Handle *feature, const LARGE_INTEGER &started)
{
    if (!feature || !started.QuadPart) return;
    LARGE_INTEGER frequency = {};
    if (!QueryPerformanceFrequency(&frequency) || !frequency.QuadPart) return;
    UINT64 sequence = 0;
    double interval = 0.0;
    bool report_window = false;
    NgxPaceRec window = {};
    AcquireSRWLockExclusive(&g_ngx_pace_lock);
    NgxPaceRec *rec = nullptr;
    for (NgxPaceRec &entry : g_ngx_pace)
        if (entry.feature == feature) { rec = &entry; break; }
    if (!rec)
        for (NgxPaceRec &entry : g_ngx_pace)
            if (!entry.feature) { rec = &entry; rec->feature = feature; break; }
    if (rec)
    {
        interval = rec->last_start.QuadPart ?
            (double)(started.QuadPart - rec->last_start.QuadPart) * 1000.0 /
            (double)frequency.QuadPart : 0.0;
        rec->last_start = started;
        sequence = ++rec->count;
        if (sequence > 1)
        {
            ++rec->window_count;
            rec->interval_sum += interval;
            if (interval > rec->interval_max) rec->interval_max = interval;
            if (interval >= 25.0) ++rec->gaps_25;
            if (interval >= 50.0) ++rec->gaps_50;
            if (rec->window_count == 128)
            {
                window = *rec;
                rec->window_count = rec->gaps_25 = rec->gaps_50 = 0;
                rec->interval_sum = rec->interval_max = 0.0;
                report_window = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_ngx_pace_lock);
    if (sequence == 1)
        Log("[pace] first game NGX evaluate feature=%p", feature);
    if (interval >= 50.0)
        Log("[pace] game NGX evaluate #%llu feature=%p interval=%.3fms bridge-frame=%llu",
            (unsigned long long)sequence, feature, interval,
            (unsigned long long)g_bridge.timeline);
    if (report_window)
        Log("[pace] game NGX feature=%p evaluates=%llu interval avg/max=%.3f/%.3fms gaps>=25/50ms=%llu/%llu bridge-frame=%llu",
            feature, (unsigned long long)window.count,
            window.interval_sum / 128.0, window.interval_max,
            (unsigned long long)window.gaps_25, (unsigned long long)window.gaps_50,
            (unsigned long long)g_bridge.timeline);
}
