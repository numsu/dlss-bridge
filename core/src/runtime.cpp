#include "dlss_bridge/runtime.hpp"

#include <ctype.h>
#include <errno.h>
#include <cmath>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace dlss_bridge {

static bool EqualNoCase(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false;
    }
    return *a == 0 && *b == 0;
}

static void CopyText(char *dst, size_t size, const char *src)
{
    if (!size) return;
    if (!src) src = "";
    strncpy(dst, src, size - 1);
    dst[size - 1] = 0;
}

static bool ParseInt(const char *text, int *out)
{
    if (!text || !*text || !out) return false;
    errno = 0;
    char *end = nullptr;
    const long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < INT_MIN || value > INT_MAX) return false;
    *out = static_cast<int>(value);
    return true;
}

bool ParseAdapterSelector(const char *text, AdapterSelector *out)
{
    if (!out || !text) return false;
    AdapterSelector parsed = {};
    parsed.kind = SelectorKind::Auto;
    parsed.index = -1;
    CopyText(parsed.text, sizeof(parsed.text), text);
    if (EqualNoCase(text, "auto")) parsed.kind = SelectorKind::Auto;
    else if (EqualNoCase(text, "game")) parsed.kind = SelectorKind::Game;
    else if (sscanf(text, "index:%d", &parsed.index) == 1 && parsed.index >= 0)
        parsed.kind = SelectorKind::Index;
    else {
        unsigned long hi = 0, lo = 0;
        if (sscanf(text, "luid:%lx:%lx", &hi, &lo) == 2) {
            parsed.kind = SelectorKind::Luid;
            parsed.luid_high = (uint32_t)hi;
            parsed.luid_low = (uint32_t)lo;
        } else return false;
    }
    *out = parsed;
    return true;
}

RuntimeConfig::RuntimeConfig()
    : verbose(0),
      ring_slots(3), neural_pipeline_frames(2),
      // Viewports allocate lazily per observed NGX handle, so the default is
      // the static capacity: single-feature games behave exactly as before,
      // split-screen needs no per-game configuration. Lower it only to bound
      // resource use against pathological multi-feature processes.
      latency_budget_ms(16), gpu_timestamps(1), max_viewports(4),
      follow_children(0),
      execution_mode(ExecutionMode::Auto),
      output_transport(OutputTransport::Native), neural_queue_mode(NeuralQueueMode::Split),
      compute_adapter{}
{
    ParseAdapterSelector("auto", &compute_adapter);
    target_executable[0] = 0;
}

bool RuntimeConfig::Apply(const char *key, const char *value)
{
    if (!key || !value) return false;
    int n = 0;
    if (EqualNoCase(key, "verbose")) {
        if (!ParseInt(value, &n) || (n != 0 && n != 1)) return false;
        verbose = n;
    }
    else if (EqualNoCase(key, "ring_slots")) {
        if (!ParseInt(value, &n)) return false;
        if (n < 2 || n > 8) return false;
        ring_slots = n;
    }
    else if (EqualNoCase(key, "neural_pipeline_frames")) {
        if (!ParseInt(value, &n)) return false;
        if (n < 1 || n > 7) return false;
        neural_pipeline_frames = n;
    }
    else if (EqualNoCase(key, "latency_budget_ms")) {
        if (!ParseInt(value, &n) || n < 1 || n > 125) return false;
        latency_budget_ms = n;
    }
    else if (EqualNoCase(key, "gpu_timestamps")) {
        if (!ParseInt(value, &n) || (n != 0 && n != 1)) return false;
        gpu_timestamps = n;
    }
    else if (EqualNoCase(key, "max_viewports")) {
        if (!ParseInt(value, &n)) return false;
        if (n < 1 || n > 4) return false;
        max_viewports = n;
    }
    else if (EqualNoCase(key, "follow_children")) {
        if (!ParseInt(value, &n) || (n != 0 && n != 1)) return false;
        follow_children = n;
    }
    else if (EqualNoCase(key, "target_executable")) {
        // Empty means unset (follow disabled); the follow gate also requires
        // follow_children, so an empty name can never arm child injection.
        CopyText(target_executable, sizeof(target_executable), value ? value : "");
    }
    else if (EqualNoCase(key, "execution_mode")) {
        if (EqualNoCase(value, "auto")) execution_mode = ExecutionMode::Auto;
        else if (EqualNoCase(value, "same_gpu")) execution_mode = ExecutionMode::SameGpu;
        else if (EqualNoCase(value, "secondary_gpu")) execution_mode = ExecutionMode::SecondaryGpu;
        else return false;
    }
    else if (EqualNoCase(key, "output_transport")) {
        if (EqualNoCase(value, "native")) output_transport = OutputTransport::Native;
        else if (EqualNoCase(value, "r11g11b10_float")) output_transport = OutputTransport::R11G11B10Float;
        else return false;
    }
    else if (EqualNoCase(key, "neural_queue_mode")) {
        if (EqualNoCase(value, "split")) neural_queue_mode = NeuralQueueMode::Split;
        else if (EqualNoCase(value, "unified")) neural_queue_mode = NeuralQueueMode::Unified;
        else return false;
    }
    else if (EqualNoCase(key, "compute_adapter")) return ParseAdapterSelector(value, &compute_adapter);
    else return false;
    return true;
}

bool RuntimeConfig::Valid() const
{
    return ring_slots >= 2 && ring_slots <= 8 &&
        neural_pipeline_frames >= 1 && neural_pipeline_frames < ring_slots &&
        latency_budget_ms >= 1 && latency_budget_ms <= 125 &&
        max_viewports >= 1 && max_viewports <= 4;
}

const char *ExecutionModeName(ExecutionMode mode)
{
    switch (mode) {
    case ExecutionMode::SameGpu: return "same_gpu";
    case ExecutionMode::SecondaryGpu: return "secondary_gpu";
    default: return "auto";
    }
}

const char *OutputTransportName(OutputTransport format)
{
    return format == OutputTransport::R11G11B10Float ? "r11g11b10_float" : "native";
}

const char *NeuralQueueModeName(NeuralQueueMode mode)
{
    return mode == NeuralQueueMode::Unified ? "unified" : "split";
}

Scheduler::Scheduler(FrameSlot *slots, size_t slot_count)
    : slots_(slots), count_(slot_count), cursor_(0)
{
    for (size_t i = 0; i < count_; ++i) slots_[i] = {};
}

FrameSlot *Scheduler::Acquire(uint64_t frame_id, uint64_t generation)
{
    if (!slots_ || !count_) return nullptr;
    for (size_t n = 0; n < count_; ++n) {
        const size_t i = (cursor_ + n) % count_;
        if (slots_[i].state != FrameState::Free && slots_[i].state != FrameState::Retired) continue;
        slots_[i].frame_id = frame_id;
        slots_[i].generation = generation;
        slots_[i].state = FrameState::Captured;
        slots_[i].result = 0;
        cursor_ = (i + 1) % count_;
        return &slots_[i];
    }
    return nullptr;
}

static bool IsForwardTransition(FrameState from, FrameState to)
{
    if (to == FrameState::Abandoned || to == FrameState::NeuralRepeat) return from != FrameState::Free;
    if (from == FrameState::Captured) return to == FrameState::InputsReady;
    if (from == FrameState::InputsReady) return to == FrameState::Uploading;
    if (from == FrameState::Uploading) return to == FrameState::Executing;
    if (from == FrameState::Executing) return to == FrameState::Downloading;
    if (from == FrameState::Downloading) return to == FrameState::Reinserted;
    if (from == FrameState::Reinserted) return to == FrameState::Retired;
    return false;
}

bool Scheduler::Transition(FrameSlot &slot, FrameState next)
{
    if (!IsForwardTransition(slot.state, next)) return false;
    slot.state = next;
    return true;
}

void Scheduler::Retire(FrameSlot &slot)
{
    if (slot.state != FrameState::Free) slot.state = FrameState::Retired;
}

size_t Scheduler::Busy() const
{
    size_t busy = 0;
    for (size_t i = 0; i < count_; ++i)
        if (slots_[i].state != FrameState::Free && slots_[i].state != FrameState::Retired) ++busy;
    return busy;
}

} // namespace dlss_bridge
