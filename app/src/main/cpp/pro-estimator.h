#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pro_estimator {

constexpr size_t kMappedLength = 24576;
constexpr int kFreshProConfiguredSampleRate = 44100;
constexpr int kFreshProEffectiveSampleRate = kFreshProConfiguredSampleRate / 4;
constexpr int kFreshProFrameSize = 8192 / 4;
constexpr int kFreshProHop = kFreshProFrameSize / 4;
// Fresh mode-0 constructor parameter used by the current transform model. The
// restored/mode-1 constructor-selected exponent remains unresolved.
constexpr int kFreshModeScaleExponent = 12;
// Reviewed fresh-callsite default only; restored/runtime threshold is unresolved.
constexpr int32_t kFreshProGateThresholdReviewedDefault = 0;

struct StageAConfig {
    int32_t gateThreshold = kFreshProGateThresholdReviewedDefault;
    int freshModeScaleExponent = kFreshModeScaleExponent;
};

bool isSupportedPcmLength(size_t count);

struct StageAResult {
    // These are the two distinct pre-scatter sources corresponding to the
    // recovered work+0x14 (power weight) and work+0x20 (magnitude weight).
    // They share mapped coordinates but are not aliases.
    std::vector<int32_t> mappedCoordinates;
    std::vector<int16_t> powerWeight;
    std::vector<int16_t> magnitudeWeight;
    std::vector<int16_t> powerScatter;
    std::vector<int16_t> magnitudeScatter;
    std::vector<int16_t> baseline;
    float energy = 0.0f;
    float requestedScale = 0.0f;
    float pcmScale = 0.0f;
    float powerScale = 0.0f;
    int32_t weightedSum = 0;
    uint32_t validMapped = 0;
    std::array<uint32_t, 3> nonzero{};
    std::array<uint64_t, 3> hashes{};
};

struct PitchResult {
    bool valid = false;
    float rawHz = -1.0f;
    float stableHz = -1.0f;
    float continuousStableHz = -1.0f;
    float stablePitch = -1.0f;
    float strength = 0.0f;
};
struct DebugTrack { int32_t id=-1; float pitch=0,hz=0,strength=0; bool valid=false; };
struct DebugSnapshot { DebugTrack rawA,stableA,rawB,stableB; };

class ProEstimator {
public:
    ProEstimator();
    ~ProEstimator();
    ProEstimator(const ProEstimator&) = delete;
    ProEstimator& operator=(const ProEstimator&) = delete;
    bool process(const int16_t* pcm, size_t count, uint32_t wallClockMs, PitchResult& result);
    void setA4(float hz);
    DebugSnapshot debugSnapshot() const;
    void reset();
private:
    struct State;
    State* state_;
};

bool runStageA(const int16_t* pcm, size_t count, StageAResult& result,
               const StageAConfig& config = {});
uint64_t validateStageAConstants();
uint64_t validateBackend();
uint64_t validateWork68Chain();
uint64_t validateTrackerAssociation();
bool proGateAccepts(int16_t re, int16_t im, int32_t threshold);
int32_t proPhaseQuotient(int32_t cross, int32_t power, int phaseShift);
uint64_t validateProContract();

}  // namespace pro_estimator
