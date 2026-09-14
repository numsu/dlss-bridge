#pragma once

#include <stddef.h>
#include <stdint.h>

namespace dlss_bridge {

enum class ExecutionMode : uint8_t { Auto = 0, SameGpu = 1, SecondaryGpu = 2 };
enum class OutputTransport : uint8_t { Native = 0, R11G11B10Float = 1 };
enum class NeuralQueueMode : uint8_t { Split = 0, Unified = 1 };
enum class SelectorKind : uint8_t { Auto = 0, Game = 1, Index = 2, Luid = 3 };
enum class FrameState : uint8_t {
    Free, Captured, InputsReady, Uploading, Executing, Downloading,
    Reinserted, Retired, NeuralRepeat, Abandoned
};

struct AdapterSelector {
    SelectorKind kind;
    int index;
    uint32_t luid_high;
    uint32_t luid_low;
    char text[96];
};

struct RuntimeConfig {
    int verbose;
    int ring_slots;
    int neural_pipeline_frames;
    int latency_budget_ms;
    int gpu_timestamps;
    ExecutionMode execution_mode;
    OutputTransport output_transport;
    NeuralQueueMode neural_queue_mode;
    AdapterSelector compute_adapter;

    RuntimeConfig();
    bool Apply(const char *key, const char *value);
    bool Valid() const;
};

struct FrameSlot {
    uint64_t frame_id;
    uint64_t generation;
    FrameState state;
    int result;
};

class Scheduler {
public:
    Scheduler(FrameSlot *slots, size_t slot_count);
    FrameSlot *Acquire(uint64_t frame_id, uint64_t generation);
    bool Transition(FrameSlot &slot, FrameState next);
    void Retire(FrameSlot &slot);
    size_t Busy() const;
private:
    FrameSlot *slots_;
    size_t count_;
    size_t cursor_;
};

bool ParseAdapterSelector(const char *text, AdapterSelector *out);
const char *ExecutionModeName(ExecutionMode mode);
const char *OutputTransportName(OutputTransport format);
const char *NeuralQueueModeName(NeuralQueueMode mode);

} // namespace dlss_bridge
