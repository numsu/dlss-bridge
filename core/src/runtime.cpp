#include "dlss_bridge/runtime.hpp"

#include <ctype.h>
#include <cmath>
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
        } else if (!strncmp(text, "uuid:", 5) && text[5]) parsed.kind = SelectorKind::Uuid;
        else if (!strncmp(text, "pci:", 4) && text[4]) parsed.kind = SelectorKind::Pci;
        else return false;
    }
    *out = parsed;
    return true;
}

RuntimeConfig::RuntimeConfig()
    : mode(0), flags(-1), subrects(1), verbose(0), sync(1), game_eval(0),
      neural_adapter(-1), require_neural_result(1), ring_slots(3),
      latency_budget_ms(16), execution_mode(ExecutionMode::Auto),
      output_transport(OutputTransport::Native), neural_queue_mode(NeuralQueueMode::Split),
      neural_placement(NeuralPlacement::AfterSr), neural_working_scale(1.0f), neural_passes(1),
      neural_runtime_variant{}, compute_adapter{}
{
    ParseAdapterSelector("auto", &compute_adapter);
    CopyText(neural_runtime_variant, sizeof(neural_runtime_variant), "stable");
}

bool RuntimeConfig::Apply(const char *key, const char *value)
{
    if (!key || !value) return false;
    const int n = atoi(value);
    if (EqualNoCase(key, "mode")) mode = n;
    else if (EqualNoCase(key, "flags")) flags = n;
    else if (EqualNoCase(key, "subrects")) subrects = n;
    else if (EqualNoCase(key, "verbose")) verbose = n;
    else if (EqualNoCase(key, "sync")) sync = n;
    else if (EqualNoCase(key, "game_eval")) game_eval = n;
    else if (EqualNoCase(key, "neural_adapter")) {
        neural_adapter = n;
        if (n >= 0) {
            char selector[32];
            snprintf(selector, sizeof(selector), "index:%d", n);
            ParseAdapterSelector(selector, &compute_adapter);
        }
    }
    else if (EqualNoCase(key, "require_neural_result")) require_neural_result = n != 0;
    else if (EqualNoCase(key, "ring_slots")) ring_slots = n;
    else if (EqualNoCase(key, "latency_budget_ms")) latency_budget_ms = n;
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
    else if (EqualNoCase(key, "neural_placement")) {
        if (EqualNoCase(value, "after_sr")) neural_placement = NeuralPlacement::AfterSr;
        else if (EqualNoCase(value, "before_sr")) neural_placement = NeuralPlacement::BeforeSr;
        else if (EqualNoCase(value, "deferred_dlss")) neural_placement = NeuralPlacement::DeferredDlss;
        else return false;
    }
    else if (EqualNoCase(key, "neural_working_scale")) {
        char *end = nullptr;
        const float scale = strtof(value, &end);
        if (end == value || *end != 0 || !std::isfinite(scale) || scale < 0.25f || scale > 2.0f) return false;
        neural_working_scale = scale;
    }
    else if (EqualNoCase(key, "neural_passes")) {
        if (n < 1 || n > 3) return false;
        neural_passes = n;
    }
    else if (EqualNoCase(key, "neural_runtime_variant")) {
        if (!*value || strlen(value) >= sizeof(neural_runtime_variant)) return false;
        for (const char *p = value; *p; ++p)
            if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) return false;
        CopyText(neural_runtime_variant, sizeof(neural_runtime_variant), value);
    }
    else if (EqualNoCase(key, "compute_adapter")) return ParseAdapterSelector(value, &compute_adapter);
    else return false;
    return true;
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

const char *NeuralPlacementName(NeuralPlacement placement)
{
    switch (placement) {
    case NeuralPlacement::BeforeSr: return "before_sr";
    case NeuralPlacement::DeferredDlss: return "deferred_dlss";
    default: return "after_sr";
    }
}

bool IsStrictNeural(const RuntimeConfig &cfg)
{
    return cfg.require_neural_result != 0;
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
