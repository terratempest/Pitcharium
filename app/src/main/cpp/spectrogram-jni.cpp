#include <jni.h>
#include "pro-estimator.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <android/log.h>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <vector>
#include <chrono>
#include <atomic>
#include <array>
#include <limits>
#include <sys/time.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#if !defined(__aarch64__)
inline float32x4_t vsqrtq_f32(float32x4_t x) {
    float temp[4];
    vst1q_f32(temp, x);
    temp[0] = sqrtf(temp[0]);
    temp[1] = sqrtf(temp[1]);
    temp[2] = sqrtf(temp[2]);
    temp[3] = sqrtf(temp[3]);
    return vld1q_f32(temp);
}
#endif
#endif

#define LOG_TAG "SpectrogramEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Complex representation
typedef struct {
    int16_t real;
    int16_t imag;
} Complex16;

// Engine state
static constexpr int kRecoveredProRateDivisor =
    pro_estimator::kFreshProConfiguredSampleRate / pro_estimator::kFreshProEffectiveSampleRate;
static int gSampleRate = 44100;
static int gStep = 8;
static int gN = 8192;
static int gHop = 1024;
static int gA4 = 440;

// Dynamic arrays based on N
static int32_t* gFrequencyToPitchQ4 = nullptr;
static int gPitchMapLength = 0;
static uint8_t gPalette[192][3];
static int gDerivativeCoefficientScale = 0;

// Last computed folded octave spectrum for pitch candidate analysis
static int32_t gLastFolded[192] = {0};
static uint64_t gRecoveredProTotalSamples = 0;
static uint64_t gRecoveredProNextSample = 8192;
static bool gRecoveredProPrimed = false;

struct RecoveredProResult {
    bool valid = false;
    float rawHz = -1.0f;
    float stabilizedHz = -1.0f;
    float pitch = -1.0f;
    float confidence = 0.0f;
};
static std::mutex gRecoveredProMutex;
static RecoveredProResult gRecoveredProResult;
static pro_estimator::ProEstimator gProEstimator;
static std::vector<int16_t> gRecoveredProRawFifo;
static std::vector<int16_t> gRecoveredProRawFrame;
static std::vector<int16_t> gRecoveredProEffectiveFrame;

// Direct 4:1 ingress is retained for Pro timing/parity. Reject frames with
// strong high-frequency content that would create a large folded alias.
static constexpr double kRecoveredProAliasGuardRatio = 0.70;

// Audio circular buffer
static int16_t* gAudioBuffer = nullptr;
static constexpr int kRecoveredProAudioBufferCapacity = 44100;
static int gAudioBufferCapacity = kRecoveredProAudioBufferCapacity;
static int gWritePosition = 0;
static int gReadPosition = 0;
static int gAvailableSamples = 0;

// Texture history (256 x 256, ARGB packed)
static uint32_t gHistoryTexture[256][256];
static int gWriteColumn = 0;
static std::mutex gEngineMutex;

static void configureAudioBufferCapacity() {
    const int capacity = kRecoveredProAudioBufferCapacity;
    if (gAudioBuffer && capacity == gAudioBufferCapacity) return;
    if (gAudioBuffer) free(gAudioBuffer);
    gAudioBufferCapacity = capacity;
    gAudioBuffer = (int16_t*)malloc(gAudioBufferCapacity * sizeof(int16_t));
    gWritePosition = 0;
    gReadPosition = 0;
    gAvailableSamples = 0;
    gRecoveredProRawFifo.reserve(65536);
    gRecoveredProRawFrame.reserve(8192);
    gRecoveredProEffectiveFrame.reserve(2048);
}

struct SwapPair { uint16_t left; uint16_t right; };
struct QueuedColumn {
    uint64_t sequence;
    uint64_t samplePosition;
    uint32_t pixels[256];
};

static std::vector<SwapPair> gFftSwaps;
static int gFftOrder = 0;

// Floating point renderer buffers
static float* gFrameFloat = nullptr;
static float* gInputAFloat = nullptr;
static float* gInputBFloat = nullptr;
static float* gCoefficientAFloat = nullptr;
static float* gCoefficientBFloat = nullptr;
static float* gFftTwiddlesFloat = nullptr;
static float* gAccumulatorFloat = nullptr;
static float* gSpectrumFloat = nullptr;
static float* gSmoothAFloat = nullptr;
static float* gSmoothBFloat = nullptr;
static std::mutex gQueueMutex;
static std::vector<QueuedColumn> gColumnQueue;
static size_t gQueueHead = 0, gQueueCount = 0, gQueueMaxDepth = 0;
static uint64_t gGeneratedColumns = 0, gNextSamplePosition = 0, gResetEpoch = 0;
static std::atomic<uint64_t> gTimingNanos[5];

static int recoveredProWindow();
static int recoveredProHop();
static int recoveredProRawWindow();
static int recoveredProRawHop();

static bool recoveredProFramePassesAliasGuard(const std::vector<int16_t>& raw) {
    if (raw.size() < 2) return false;
    double mean = 0.0;
    for (int16_t sample : raw) mean += double(sample);
    mean /= double(raw.size());
    double energy = 0.0;
    double differenceEnergy = 0.0;
    double previous = double(raw.front()) - mean;
    for (int16_t value : raw) {
        const double sample = double(value) - mean;
        energy += sample * sample;
        const double difference = sample - previous;
        differenceEnergy += difference * difference;
        previous = sample;
    }
    return energy > 1.0e-9 && differenceEnergy / energy < kRecoveredProAliasGuardRatio;
}

static uint64_t nowNanos() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void resetRecoveredProState() {
    std::lock_guard<std::mutex> lock(gRecoveredProMutex);
    gRecoveredProResult = {};
    gProEstimator.reset();
    gRecoveredProTotalSamples = 0;
    const int window = recoveredProRawWindow();
    gRecoveredProNextSample = window > 0 ? uint64_t(window) : 8192ULL;
    gRecoveredProPrimed = false;
    gRecoveredProRawFifo.clear();
    gRecoveredProRawFrame.clear();
    gRecoveredProEffectiveFrame.clear();
    if (gAudioBuffer) {
        std::memset(gAudioBuffer, 0, size_t(gAudioBufferCapacity) * sizeof(int16_t));
        gWritePosition = 0;
        gReadPosition = 0;
        gAvailableSamples = 0;
    }
}

static void resetFrequencyDetectionState() {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    resetRecoveredProState();
}

static int recoveredProWindow() {
    // Fresh Pro config: configured Fs 44100, rateMode 0 -> Fs_eff 11025,
    // N=8192/4. This is independent of Android AudioRecord's capture Fs.
    return pro_estimator::kFreshProFrameSize;
}

static int recoveredProHop() {
    return pro_estimator::kFreshProHop;
}

static int recoveredProRawWindow() {
    return recoveredProWindow() * kRecoveredProRateDivisor;
}

static int recoveredProRawHop() {
    return recoveredProHop() * kRecoveredProRateDivisor;
}

static void decimateRecoveredProFrame(const std::vector<int16_t>& raw,
                                      std::vector<int16_t>& effective) {
    const int effectiveCount = recoveredProWindow();
    effective.resize(effectiveCount);
    for (int i = 0; i < effectiveCount; ++i) {
        // Divisor-4 mode-0 ingress: preserve the documented frame/window geometry;
        // no extra interpolation or window is introduced here.
        effective[i] = raw[size_t(i) * size_t(kRecoveredProRateDivisor)];
    }
}

static void processRecoveredProFrames() {
    if (gSampleRate != pro_estimator::kFreshProConfiguredSampleRate) return;
    const int rawWindowSamples = recoveredProRawWindow();
    const int rawHopSamples = recoveredProRawHop();
    if (rawWindowSamples <= 0 || rawHopSamples <= 0) return;

    if (gRecoveredProRawFifo.size() < size_t(rawWindowSamples)) return;
    gRecoveredProRawFrame.resize(rawWindowSamples);
    gRecoveredProEffectiveFrame.resize(recoveredProWindow());
    while (gRecoveredProRawFifo.size() >= size_t(rawWindowSamples)) {
        std::memcpy(gRecoveredProRawFrame.data(), gRecoveredProRawFifo.data(),
                    size_t(rawWindowSamples) * sizeof(int16_t));
        for (int i = 0; i < recoveredProWindow(); ++i) {
            gRecoveredProEffectiveFrame[i] =
                gRecoveredProRawFrame[size_t(i) * size_t(kRecoveredProRateDivisor)];
        }
        pro_estimator::PitchResult pitch;
        const uint32_t ms = uint32_t(
            (gRecoveredProNextSample * 1000ULL) /
            uint64_t(pro_estimator::kFreshProConfiguredSampleRate));
        {
            std::lock_guard<std::mutex> resultLock(gRecoveredProMutex);
            if (recoveredProFramePassesAliasGuard(gRecoveredProRawFrame) &&
                gProEstimator.process(gRecoveredProEffectiveFrame.data(),
                                      gRecoveredProEffectiveFrame.size(), ms, pitch)) {
                gRecoveredProResult = {pitch.valid, pitch.rawHz, pitch.stableHz,
                                       pitch.stablePitch, pitch.strength};
                if ((gRecoveredProTotalSamples / uint64_t(rawHopSamples)) % 4 == 0 ||
                    pitch.rawHz > 800.0f || pitch.stableHz > 800.0f) {
                    LOGI("pro-trace samples=%llu valid=%d raw=%.3f stable=%.3f pitch=%.3f conf=%.3f window=%d hop=%d",
                         (unsigned long long)gRecoveredProTotalSamples, pitch.valid ? 1 : 0,
                         pitch.rawHz, pitch.stableHz, pitch.stablePitch, pitch.strength,
                         recoveredProWindow(), recoveredProHop());
                }
            } else {
                gRecoveredProResult = {};
            }
        }
        gRecoveredProPrimed = true;
        gRecoveredProNextSample += uint64_t(rawHopSamples);
        gRecoveredProRawFifo.erase(gRecoveredProRawFifo.begin(),
                                   gRecoveredProRawFifo.begin() + rawHopSamples);
    }
    if (!gRecoveredProPrimed) {
        std::lock_guard<std::mutex> resultLock(gRecoveredProMutex);
        gRecoveredProResult = {};
    }
}

static void resetColumnQueue() {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    if (gColumnQueue.size() < 512) gColumnQueue.resize(512);
    gQueueHead = gQueueCount = gQueueMaxDepth = 0;
    gGeneratedColumns = 0;
    gNextSamplePosition = (uint64_t)gN;
    gResetEpoch++;
    for (auto& timing : gTimingNanos) timing.store(0, std::memory_order_relaxed);
}

static void enqueueColumn(int physicalX) {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    if (gQueueCount == gColumnQueue.size()) {
        size_t oldSize = gColumnQueue.size(), newSize = oldSize ? oldSize * 2 : 512;
        std::vector<QueuedColumn> grown(newSize);
        for (size_t i = 0; i < gQueueCount; i++) grown[i] = gColumnQueue[(gQueueHead + i) % oldSize];
        gColumnQueue.swap(grown);
        gQueueHead = 0;
    }
    size_t tail = (gQueueHead + gQueueCount) % gColumnQueue.size();
    QueuedColumn& column = gColumnQueue[tail];
    column.sequence = gGeneratedColumns++;
    column.samplePosition = gNextSamplePosition;
    gNextSamplePosition += (uint64_t)gHop;
    for (int row = 0; row < 256; row++) column.pixels[row] = gHistoryTexture[row][physicalX];
    gQueueCount++;
    gQueueMaxDepth = std::max(gQueueMaxDepth, gQueueCount);
}

// Reversal bits helper
static uint32_t reverseBits(uint32_t x, int stage_count) {
    uint32_t result = 0;
    for (int i = 0; i < stage_count; i++) {
        result = (result << 1) | (x & 1);
        x >>= 1;
    }
    return result;
}

static void floatRadix2FFT(float* data) {
    const int size = gN / 2; // number of complex numbers
    // Bit reversal swap using precomputed gFftSwaps
    for (const SwapPair& pair : gFftSwaps) {
        std::swap(data[2 * pair.left], data[2 * pair.right]);
        std::swap(data[2 * pair.left + 1], data[2 * pair.right + 1]);
    }

    // Stage 1 (half = 1, butterfly without twiddles since twiddle is 1.0)
    for (int i = 0; i < size; i += 2) {
        int even = 2 * i, odd = even + 2;
        float er = data[even], ei = data[even + 1];
        float or_ = data[odd], oi = data[odd + 1];
        data[even] = (er + or_) * 0.5f;
        data[even + 1] = (ei + oi) * 0.5f;
        data[odd] = (er - or_) * 0.5f;
        data[odd + 1] = (ei - oi) * 0.5f;
    }

    // Stages 2+
    int twiddle = 2;
    for (int half = 2; half < size; half <<= 1) {
        int span = half << 1;
        for (int j = 0; j < half; j++) {
            float wr = gFftTwiddlesFloat[twiddle++];
            float wi = gFftTwiddlesFloat[twiddle++];
            for (int start = j; start < size; start += span) {
                int even = 2 * start, odd = 2 * (start + half);
                float er = data[even], ei = data[even + 1];
                float or_ = data[odd], oi = data[odd + 1];

                float crossRe = or_ * wr - oi * wi;
                float crossIm = or_ * wi + oi * wr;

                data[odd] = (er - crossRe) * 0.5f;
                data[odd + 1] = (ei - crossIm) * 0.5f;
                data[even] = (er + crossRe) * 0.5f;
                data[even + 1] = (ei + crossIm) * 0.5f;
            }
        }
    }
}

static void floatSmoothing(const float* input, float* output) {
    const float coeff[9] = { 1.0f, 8.0f, 28.0f, 56.0f, 70.0f, 56.0f, 28.0f, 8.0f, 1.0f };

    // 1. Left boundary (with bounds checks)
    for (int i = 0; i < 4; i++) {
        float sum = 0.0f;
        for (int k = -4; k <= 4; k++) {
            int idx = i + k;
            if (idx >= 0 && idx < 1536) {
                sum += input[idx] * coeff[k + 4];
            }
        }
        output[i] = sum * (1.0f / 256.0f);
    }

    // 2. Main body (no bounds check, branchless & vectorizable)
    #pragma clang loop vectorize(enable)
    for (int i = 4; i < 1532; i++) {
        float sum = input[i - 4] * coeff[0]
                  + input[i - 3] * coeff[1]
                  + input[i - 2] * coeff[2]
                  + input[i - 1] * coeff[3]
                  + input[i]     * coeff[4]
                  + input[i + 1] * coeff[5]
                  + input[i + 2] * coeff[6]
                  + input[i + 3] * coeff[7]
                  + input[i + 4] * coeff[8];
        output[i] = sum * (1.0f / 256.0f);
    }

    // 3. Right boundary (with bounds checks)
    for (int i = 1532; i < 1536; i++) {
        float sum = 0.0f;
        for (int k = -4; k <= 4; k++) {
            int idx = i + k;
            if (idx >= 0 && idx < 1536) {
                sum += input[idx] * coeff[k + 4];
            }
        }
        output[i] = sum * (1.0f / 256.0f);
    }
}

enum SlotState {
    STATE_EMPTY,
    STATE_PREPARED,
    STATE_FFT_DONE
};

struct PipelineSlot {
    SlotState state = STATE_EMPTY;
    bool fftA_done = false;
    bool fftB_done = false;

    int frameIndex = -1;
    int writeColumn = -1;
    uint64_t samplePosition = 0;
    int readPosition = 0;

    // Float buffers (allocated dynamically when N changes)
    float* inputAFloat = nullptr;
    float* inputBFloat = nullptr;

};

static PipelineSlot gSlots[2];
static std::mutex gPipelineMutex;
static std::condition_variable gCvMain;
static std::condition_variable gCvFFT;
static std::condition_variable gCvPost;

static int gMainFrameIndex = 0;
static int gFFTAFrameIndex = 0;
static int gFFTBFrameIndex = 0;
static int gPostFrameIndex = 0;
static bool gUseMultithreading = false;
static bool gThreadsRunning = false;
static std::thread gThreadA;
static std::thread gThreadB;
static std::thread gThreadPost;

static void stopThreads() {
    if (gThreadsRunning) {
        {
            std::lock_guard<std::mutex> lock(gPipelineMutex);
            gThreadsRunning = false;
        }
        gCvMain.notify_all();
        gCvFFT.notify_all();
        gCvPost.notify_all();

        if (gThreadA.joinable()) gThreadA.join();
        if (gThreadB.joinable()) gThreadB.join();
        if (gThreadPost.joinable()) gThreadPost.join();
    }
}

static void threadA_loop() {
    while (true) {
        int index = -1;
        {
            std::unique_lock<std::mutex> lock(gPipelineMutex);
            gCvFFT.wait(lock, [] {
                if (!gThreadsRunning) return true;
                int slotIdx = gFFTAFrameIndex % 2;
                return gSlots[slotIdx].state == STATE_PREPARED && gMainFrameIndex > gFFTAFrameIndex;
            });
            if (!gThreadsRunning) break;
            index = gFFTAFrameIndex;
        }

        int slotIdx = index % 2;
        // Run FFT A
        floatRadix2FFT(gSlots[slotIdx].inputAFloat);

        {
            std::lock_guard<std::mutex> lock(gPipelineMutex);
            gSlots[slotIdx].fftA_done = true;
            if (gSlots[slotIdx].fftB_done) {
                gSlots[slotIdx].state = STATE_FFT_DONE;
                gCvPost.notify_all();
            }
            gFFTAFrameIndex++;
        }
        gCvFFT.notify_all();
    }
}

static void threadB_loop() {
    while (true) {
        int index = -1;
        {
            std::unique_lock<std::mutex> lock(gPipelineMutex);
            gCvFFT.wait(lock, [] {
                if (!gThreadsRunning) return true;
                int slotIdx = gFFTBFrameIndex % 2;
                return gSlots[slotIdx].state == STATE_PREPARED && gMainFrameIndex > gFFTBFrameIndex;
            });
            if (!gThreadsRunning) break;
            index = gFFTBFrameIndex;
        }

        int slotIdx = index % 2;
        // Run FFT B
        floatRadix2FFT(gSlots[slotIdx].inputBFloat);

        {
            std::lock_guard<std::mutex> lock(gPipelineMutex);
            gSlots[slotIdx].fftB_done = true;
            if (gSlots[slotIdx].fftA_done) {
                gSlots[slotIdx].state = STATE_FFT_DONE;
                gCvPost.notify_all();
            }
            gFFTBFrameIndex++;
        }
        gCvFFT.notify_all();
    }
}

static void threadPost_loop() {
    while (true) {
        int index = -1;
        {
            std::unique_lock<std::mutex> lock(gPipelineMutex);
            gCvPost.wait(lock, [] {
                if (!gThreadsRunning) return true;
                int slotIdx = gPostFrameIndex % 2;
                return gSlots[slotIdx].state == STATE_FFT_DONE;
            });
            if (!gThreadsRunning) break;
            index = gPostFrameIndex;
        }

        int slotIdx = index % 2;
        int physicalX = gSlots[slotIdx].writeColumn;
        const int stages = gFftOrder + 1;

        {
            uint64_t stageStart = nowNanos();
            std::memset(gAccumulatorFloat, 0, 1536 * sizeof(float));

            int maxBin = gN / 2;
            int bin = 1;

            #if defined(__ARM_NEON)
            int neonLimit = maxBin - 4;
            for (; bin <= neonLimit; bin += 4) {
                float32x4x2_t compA = vld2q_f32(&gSlots[slotIdx].inputAFloat[2 * bin]);
                float32x4_t realA_vec = compA.val[0];
                float32x4_t imagA_vec = compA.val[1];

                float32x4x2_t compB = vld2q_f32(&gSlots[slotIdx].inputBFloat[2 * bin]);
                float32x4_t realB_vec = compB.val[0];
                float32x4_t imagB_vec = compB.val[1];

                float32x4_t powerA_vec = vmlaq_f32(vmulq_f32(realA_vec, realA_vec), imagA_vec, imagA_vec);
                float32x4_t magnitude_vec = vsqrtq_f32(powerA_vec);
                float32x4_t cross_vec = vsubq_f32(vmulq_f32(realB_vec, imagA_vec), vmulq_f32(imagB_vec, realA_vec));

                float powerA_arr[4];
                float magnitude_arr[4];
                float cross_arr[4];

                vst1q_f32(powerA_arr, powerA_vec);
                vst1q_f32(magnitude_arr, magnitude_vec);
                vst1q_f32(cross_arr, cross_vec);

                for (int i = 0; i < 4; i++) {
                    int currentBin = bin + i;
                    float pA = powerA_arr[i];
                    if (pA <= 1e-12f) continue;

                    float mag = magnitude_arr[i];
                    if (mag == 0.0f) continue;

                    float cr = cross_arr[i];
                    float correctionQ4 = (cr / pA) * (float)gN * 16.0f / (float)(1 << gDerivativeCoefficientScale);
                    int32_t subbinQ4 = (int32_t)lroundf((float)currentBin * 16.0f + correctionQ4);

                    if (subbinQ4 < 0 || subbinQ4 >= gPitchMapLength) continue;

                    int32_t destinationQ4 = gFrequencyToPitchQ4[subbinQ4];
                    if (destinationQ4 < 0) continue;
                    int32_t destination = destinationQ4 >> 4;
                    int32_t fraction = destinationQ4 & 15;

                    if (destination >= 0 && destination < 1535) {
                        gAccumulatorFloat[destination] += (16.0f - (float)fraction) * mag;
                        gAccumulatorFloat[destination + 1] += (float)fraction * mag;
                    }
                }
            }
            #endif

            for (; bin < maxBin; bin++) {
                float realA = gSlots[slotIdx].inputAFloat[2 * bin];
                float imagA = gSlots[slotIdx].inputAFloat[2 * bin + 1];
                float realB = gSlots[slotIdx].inputBFloat[2 * bin];
                float imagB = gSlots[slotIdx].inputBFloat[2 * bin + 1];

                float powerA = realA * realA + imagA * imagA;
                if (powerA <= 1e-12f) continue;

                float mag = sqrtf(powerA);
                if (mag == 0.0f) continue;

                float cross = realB * imagA - imagB * realA;
                float correctionQ4 = (cross / powerA) * (float)gN * 16.0f / (float)(1 << gDerivativeCoefficientScale);
                int32_t subbinQ4 = (int32_t)lroundf((float)bin * 16.0f + correctionQ4);

                if (subbinQ4 < 0 || subbinQ4 >= gPitchMapLength) continue;

                int32_t destinationQ4 = gFrequencyToPitchQ4[subbinQ4];
                if (destinationQ4 < 0) continue;
                int32_t destination = destinationQ4 >> 4;
                int32_t fraction = destinationQ4 & 15;

                if (destination >= 0 && destination < 1535) {
                    gAccumulatorFloat[destination] += (16.0f - (float)fraction) * mag;
                    gAccumulatorFloat[destination + 1] += (float)fraction * mag;
                }
            }
            gTimingNanos[2].fetch_add(nowNanos() - stageStart, std::memory_order_relaxed);
            stageStart = nowNanos();

            float max_accum = 0.0f;
            for (int i = 0; i < 1536; i++) {
                if (gAccumulatorFloat[i] > max_accum) max_accum = gAccumulatorFloat[i];
            }

            if (max_accum > 0.0f) {
                float scale = 32767.0f / max_accum;
                for (int i = 0; i < 1536; i++) {
                    gSpectrumFloat[i] = gAccumulatorFloat[i] * scale;
                }
            } else {
                std::memset(gSpectrumFloat, 0, 1536 * sizeof(float));
            }


            for (int i = 0; i < 1536; i++) {
                float val = gSpectrumFloat[i];
                if (val <= 0.0f) {
                    gSpectrumFloat[i] = 0.0f;
                } else {
                    float ratio = val / 32768.0f;
                    float db = 20.0f * log10f(ratio) + 30.0f;
                    if (db < 0.0f) {
                        gSpectrumFloat[i] = 0.0f;
                    } else {
                        gSpectrumFloat[i] = db * (32767.0f / 30.0f);
                    }
                }
            }

            floatSmoothing(gSpectrumFloat, gSmoothAFloat);

            float maximum = 0.0f;
            for (int i = 0; i < 1536; i++) {
                if (gSmoothAFloat[i] > maximum) maximum = gSmoothAFloat[i];
            }

            if (maximum > 0.0f) {
                float scale = 32767.0f / maximum;
                for (int i = 0; i < 1536; i++) {
                    gSpectrumFloat[i] = gSmoothAFloat[i] * scale;
                }
            } else {
                std::memset(gSpectrumFloat, 0, 1536 * sizeof(float));
            }

            float foldedFloat[192];
            for (int row = 0; row < 192; row++) {
                foldedFloat[row] = 0.0f;
                for (int octave = 0; octave < 8; octave++) {
                    foldedFloat[row] += gSpectrumFloat[row + octave * 192];
                }
                gLastFolded[row] = (int32_t)foldedFloat[row];
            }

            float foldedMax = 0.0f;
            for (int row = 0; row < 192; row++) {
                if (foldedFloat[row] > foldedMax) foldedMax = foldedFloat[row];
            }

            float scalarFloat[192];
            for (int row = 0; row < 192; row++) {
                if (foldedMax > 0.0f) {
                    scalarFloat[row] = (foldedFloat[row] * 256.0f) / foldedMax;
                } else {
                    scalarFloat[row] = 0.0f;
                }
            }

            for (int row = 0; row < 192; row++) {
                float r = (scalarFloat[row] * (float)gPalette[row][0] * 320.0f) / 65536.0f;
                float g = (scalarFloat[row] * (float)gPalette[row][1] * 320.0f) / 65536.0f;
                float b = (scalarFloat[row] * (float)gPalette[row][2] * 320.0f) / 65536.0f;

                int32_t ri = (int32_t)r;
                int32_t gi = (int32_t)g;
                int32_t bi = (int32_t)b;

                if (ri > 255) ri = 255; else if (ri < 0) ri = 0;
                if (gi > 255) gi = 255; else if (gi < 0) gi = 0;
                if (bi > 255) bi = 255; else if (bi < 0) bi = 0;

                uint32_t color = (0xFFu << 24) | ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
                gHistoryTexture[row][physicalX] = color;
            }

            gHistoryTexture[192][physicalX] = gHistoryTexture[0][physicalX];

            for (int row = 193; row < 256; row++) {
                gHistoryTexture[row][physicalX] = 0;
            }

            enqueueColumn(physicalX);
            gTimingNanos[3].fetch_add(nowNanos() - stageStart, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lock(gPipelineMutex);
            gSlots[slotIdx].state = STATE_EMPTY;
            gSlots[slotIdx].fftA_done = false;
            gSlots[slotIdx].fftB_done = false;
            gPostFrameIndex++;
        }
        gCvMain.notify_all();
    }
}

static void startThreads() {
    stopThreads();
    {
        std::lock_guard<std::mutex> lock(gPipelineMutex);
        gThreadsRunning = true;
        gMainFrameIndex = 0;
        gFFTAFrameIndex = 0;
        gFFTBFrameIndex = 0;
        gPostFrameIndex = 0;

        for (int i = 0; i < 2; i++) {
            gSlots[i].state = STATE_EMPTY;
            gSlots[i].fftA_done = false;
            gSlots[i].fftB_done = false;
        }
    }
    gThreadA = std::thread(threadA_loop);
    gThreadB = std::thread(threadB_loop);
    gThreadPost = std::thread(threadPost_loop);
}

// Precomputes all static tables and configures parameters based on sample rate and step size
static void precomputeAll() {
    switch (gSampleRate) {
        case 8000:
        case 11025: gN = 2048; break;
        case 22050: gN = 4096; break;
        case 44100: gN = 8192; break;
    }

    gHop = gN / gStep;
    LOGI("Precomputing tables for sampleRate=%d, step=%d -> N=%d, hop=%d", gSampleRate, gStep, gN, gHop);

    // Free previous dynamic allocations
    if (gFrequencyToPitchQ4) free(gFrequencyToPitchQ4);

    if (gFrameFloat) free(gFrameFloat);
    if (gInputAFloat) free(gInputAFloat);
    if (gInputBFloat) free(gInputBFloat);
    if (gCoefficientAFloat) free(gCoefficientAFloat);
    if (gCoefficientBFloat) free(gCoefficientBFloat);
    if (gFftTwiddlesFloat) free(gFftTwiddlesFloat);
    if (gAccumulatorFloat) free(gAccumulatorFloat);
    if (gSpectrumFloat) free(gSpectrumFloat);
    if (gSmoothAFloat) free(gSmoothAFloat);
    if (gSmoothBFloat) free(gSmoothBFloat);

    for (int i = 0; i < 2; i++) {
        if (gSlots[i].inputAFloat) free(gSlots[i].inputAFloat);
        if (gSlots[i].inputBFloat) free(gSlots[i].inputBFloat);
        gSlots[i].inputAFloat = nullptr;
        gSlots[i].inputBFloat = nullptr;
    }

    gPitchMapLength = ((gN / 2) + 1) * 16;
    gFrequencyToPitchQ4 = (int32_t*)malloc(gPitchMapLength * sizeof(int32_t));

    gFrameFloat = (float*)malloc(gN * sizeof(float));
    gInputAFloat = (float*)malloc(gN * sizeof(float));
    gInputBFloat = (float*)malloc(gN * sizeof(float));
    gCoefficientAFloat = (float*)malloc(gN * sizeof(float));
    gCoefficientBFloat = (float*)malloc(gN * sizeof(float));
    gFftTwiddlesFloat = (float*)malloc((gN - 2) * sizeof(float));
    gAccumulatorFloat = (float*)malloc(1536 * sizeof(float));
    gSpectrumFloat = (float*)malloc(1536 * sizeof(float));
    gSmoothAFloat = (float*)malloc(1536 * sizeof(float));
    gSmoothBFloat = (float*)malloc(1536 * sizeof(float));

    for (int i = 0; i < 2; i++) {
        gSlots[i].inputAFloat = (float*)malloc(gN * sizeof(float));
        gSlots[i].inputBFloat = (float*)malloc(gN * sizeof(float));
    }

    int twiddleFloat = 0;
    const int fftSize = gN / 2;
    gFftOrder = 0;
    while ((1 << gFftOrder) < fftSize) gFftOrder++;
    gFftSwaps.clear();
    gFftSwaps.reserve(fftSize / 2);
    for (int i = 1; i < fftSize; i++) {
        int reversed = (int)reverseBits((uint32_t)i, gFftOrder);
        if (i < reversed) gFftSwaps.push_back({(uint16_t)i, (uint16_t)reversed});
    }
    for (int half = 1; half < fftSize; half <<= 1) {
        const float step = (float)M_PI / (float)half;
        for (int i = 0; i < half; i++) {
            const float angle = step * (float)i;
            gFftTwiddlesFloat[twiddleFloat++] = cosf(angle);
            gFftTwiddlesFloat[twiddleFloat++] = -sinf(angle);
        }
    }

    // 2. Gaussian window coefficients (Section 4)
    double* gaussian = (double*)malloc(gN * sizeof(double));
    double halfN = gN / 2.0;
    for (int i = 0; i < gN; i++) {
        double x = 3.0 * (i - halfN) / halfN;
        gaussian[i] = exp(-(x * x));
        gCoefficientAFloat[i] = (float)gaussian[i];
    }

    // Derivative first-difference (Section 4 and Section 6)
    double* difference = (double*)malloc(gN * sizeof(double));
    double peak = 0.0;
    for (int i = 0; i < gN; i++) {
        int next_idx = (i + 1) % gN;
        difference[i] = (gaussian[next_idx] - gaussian[i]) / (2.0 * M_PI);
        double abs_diff = std::abs(difference[i]);
        if (abs_diff > peak) {
            peak = abs_diff;
        }
    }

    int s = 0;
    while (peak * (double)(1 << (s + 1)) <= 1.0) {
        s++;
    }
    gDerivativeCoefficientScale = s;

    for (int i = 0; i < gN; i++) {
        gCoefficientBFloat[i] = (float)(difference[i] * (double)(1 << s));
    }
    free(gaussian);
    free(difference);

    // Frequency-to-pitch mapping Q4 table
    for (int subbin = 0; subbin < gPitchMapLength; subbin++) {
        double freq = (double)gSampleRate * subbin / (16.0 * gN);
        gFrequencyToPitchQ4[subbin] = -1;
        if (freq > 0.0) {
            double note = 12.0 * log2(freq / gA4) + 57.0;
            double pitchPosition = (note + 0.5) * 16.0;
            if (pitchPosition >= 0.0 && pitchPosition < 1536.0)
                gFrequencyToPitchQ4[subbin] = (int32_t)(pitchPosition * 16.0 + 0.5);
        }
    }

    // 5. Cyclic Pitch Palette (Section 14)
    uint8_t baseColor[12][3] = {
        {0xB2, 0xFF, 0x66}, // 0
        {0x66, 0xFF, 0x66}, // 1
        {0x66, 0xFF, 0xB2}, // 2
        {0x66, 0xFF, 0xFF}, // 3
        {0x66, 0xB3, 0xFF}, // 4
        {0x66, 0x66, 0xFF}, // 5
        {0xB2, 0x66, 0xFF}, // 6
        {0xFF, 0x66, 0xFF}, // 7
        {0xFF, 0x66, 0xB2}, // 8
        {0xFF, 0x66, 0x66}, // 9
        {0xFF, 0xB2, 0x66}, // 10
        {0xFF, 0xFF, 0x66}  // 11
    };

    for (int row = 0; row < 192; row++) {
        double phase = (double)row / 16.0 - 0.5;
        while (phase < 0.0) phase += 12.0;
        while (phase >= 12.0) phase -= 12.0;
        int base = (int)floor(phase);
        int next = (base + 1) % 12;
        double fraction = phase - base;
        for (int ch = 0; ch < 3; ch++) {
            double val = (double)baseColor[base][ch] + 1.0 + fraction * ((double)baseColor[next][ch] - (double)baseColor[base][ch]);
            if (val < 0.0) val = 0.0;
            if (val > 255.0) val = 255.0;
            gPalette[row][ch] = (uint8_t)val;
        }
    }
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_pitcharium_SpectrogramEngine_validateProEstimatorStageA(JNIEnv*, jobject) {
    return static_cast<jlong>(pro_estimator::validateStageAConstants());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_pitcharium_SpectrogramEngine_validateProEstimatorBackend(JNIEnv*, jobject) {
    return static_cast<jlong>(pro_estimator::validateBackend());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_pitcharium_SpectrogramEngine_validateProWork68Chain(JNIEnv*, jobject) {
    return static_cast<jlong>(pro_estimator::validateWork68Chain());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_pitcharium_SpectrogramEngine_validateProTrackerAssociation(JNIEnv*, jobject) {
    return static_cast<jlong>(pro_estimator::validateTrackerAssociation());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_pitcharium_SpectrogramEngine_validateProEstimatorContract(JNIEnv*, jobject) {
    return static_cast<jlong>(pro_estimator::validateProContract());
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_pitcharium_SpectrogramEngine_validateProIngressContract(JNIEnv*, jobject) {
    uint64_t mask = 0;
    if (kRecoveredProRateDivisor == 4 &&
        recoveredProRawWindow() == 8192 && recoveredProRawHop() == 2048 &&
        recoveredProWindow() == 2048 && recoveredProHop() == 512) {
        mask |= 1;
    }
    std::vector<int16_t> raw(recoveredProRawWindow());
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<int16_t>(i);
    std::vector<int16_t> effective;
    decimateRecoveredProFrame(raw, effective);
    bool deterministic = effective.size() == size_t(recoveredProWindow());
    for (size_t i = 0; deterministic && i < effective.size(); ++i) {
        deterministic = effective[i] == raw[i * size_t(kRecoveredProRateDivisor)];
    }
    if (deterministic) mask |= 2;
    return static_cast<jlong>(mask);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_pitcharium_SpectrogramEngine_validateProEstimatorEndToEnd(JNIEnv*, jobject) {
    pro_estimator::ProEstimator estimator;pro_estimator::PitchResult r;
    std::array<int16_t,8192> frame{};jlong mask=0;
    for(size_t i=0;i<frame.size();++i){double t=double(i)/44100.0;frame[i]=int16_t(10000.0*sin(2*M_PI*220*t)+16000.0*sin(2*M_PI*440*t));}
    if(estimator.process(frame.data(),2048,0,r))mask|=1;
    if(estimator.process(frame.data(),2048,151,r))mask|=2;
    if(estimator.process(frame.data(),2048,302,r))mask|=4;
    const auto before=estimator.debugSnapshot();
    if(before.rawA.valid&&before.stableA.valid&&before.rawB.valid&&before.stableB.valid)mask|=16;
    if(before.rawA.id>=12&&before.rawA.id<=107&&before.rawB.id>=12&&before.rawB.id<=107)mask|=32;
    const auto canonical=r;if(estimator.process(frame.data(),2048,302,r))mask|=64;
    const auto zeroDt=estimator.debugSnapshot();if(zeroDt.stableA.id==before.stableA.id&&zeroDt.stableA.pitch==before.stableA.pitch&&zeroDt.stableA.strength==before.stableA.strength)mask|=128;
    std::fill(frame.begin(),frame.end(),0);if(estimator.process(frame.data(),2048,332,r))mask|=8;
    const auto after=estimator.debugSnapshot();if(after.stableA.id==before.stableA.id&&after.stableA.hz==before.stableA.hz&&after.stableA.strength<before.stableA.strength)mask|=256;
    if(canonical.stableHz>0&&canonical.continuousStableHz>0&&fabsf(canonical.stableHz-canonical.continuousStableHz)<1.0e-6f)mask|=512;
    if(canonical.stablePitch==before.stableA.pitch)mask|=1024;
    return mask|(jlong(before.rawA.id&0xff)<<16)|(jlong(before.rawB.id&0xff)<<24);
}

JNIEXPORT jboolean JNICALL
Java_com_pitcharium_SpectrogramEngine_runProEstimatorStageA(
        JNIEnv* env, jobject, jshortArray samples, jlongArray hashes, jfloatArray scalars) {
    if (!samples || env->GetArrayLength(samples) != 8192 ||
        !hashes || env->GetArrayLength(hashes) < 7 || !scalars || env->GetArrayLength(scalars) < 5) {
        return JNI_FALSE;
    }
    jshort* pcm = env->GetShortArrayElements(samples, nullptr);
    if (!pcm) return JNI_FALSE;
    static thread_local pro_estimator::StageAResult result;
    const bool ok = pro_estimator::runStageA(reinterpret_cast<int16_t*>(pcm), 8192, result);
    env->ReleaseShortArrayElements(samples, pcm, JNI_ABORT);
    if (!ok) return JNI_FALSE;
    const jlong outHashes[7] = {static_cast<jlong>(result.hashes[0]), static_cast<jlong>(result.hashes[1]), static_cast<jlong>(result.hashes[2]),
        result.validMapped,result.nonzero[0],result.nonzero[1],result.nonzero[2]};
    const jfloat outScalars[5] = {result.energy,result.requestedScale,result.pcmScale,result.powerScale,float(result.weightedSum)};
    env->SetLongArrayRegion(hashes, 0, 7, outHashes);
    env->SetFloatArrayRegion(scalars, 0, 5, outScalars);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_pitcharium_SpectrogramEngine_init(JNIEnv* env, jobject thiz, jint sampleRate, jint step, jboolean useMultithreading) {
    if (!((sampleRate == 8000) || (sampleRate == 11025) || (sampleRate == 22050) || (sampleRate == 44100)) ||
        !((step == 4) || (step == 8) || (step == 16))) {
        jclass type = env->FindClass("java/lang/IllegalArgumentException");
        env->ThrowNew(type, "Unsupported sample rate or analysis step");
        return;
    }
    std::lock_guard<std::mutex> lock(gEngineMutex);
    stopThreads();
    gSampleRate = sampleRate;
    gStep = step;
    gUseMultithreading = useMultithreading;
    precomputeAll();
    resetRecoveredProState();

    // Allocate / reset audio buffer
    if (gAudioBuffer) {
        free(gAudioBuffer);
        gAudioBuffer = nullptr;
    }
    configureAudioBufferCapacity();
    gWritePosition = 0;
    gReadPosition = 0;
    gAvailableSamples = 0;

    // Clear history texture
    std::memset(gHistoryTexture, 0, sizeof(gHistoryTexture));
    gWriteColumn = 0;
    resetColumnQueue();

    if (gUseMultithreading) {
        startThreads();
    }
}

JNIEXPORT void JNICALL
Java_com_pitcharium_SpectrogramEngine_setA4Reference(JNIEnv* env, jobject thiz, jint a4) {
    if (a4 <= 0) {
        jclass type = env->FindClass("java/lang/IllegalArgumentException");
        env->ThrowNew(type, "A4 reference must be positive");
        return;
    }
    std::lock_guard<std::mutex> lock(gEngineMutex);
    gA4 = a4;
    gProEstimator.setA4(float(a4));
    resetRecoveredProState();
    // Recompute frequency mapping
    if (gFrequencyToPitchQ4) {
        for (int subbin = 0; subbin < gPitchMapLength; subbin++) {
            double freq = (double)gSampleRate * subbin / (16.0 * gN);
            gFrequencyToPitchQ4[subbin] = -1;
            if (freq > 0.0) {
                double note = 12.0 * log2(freq / gA4) + 57.0;
                double pitchPosition = (note + 0.5) * 16.0;
                if (pitchPosition >= 0.0 && pitchPosition < 1536.0)
                    gFrequencyToPitchQ4[subbin] = (int32_t)(pitchPosition * 16.0 + 0.5);
            }
        }
    }
}

JNIEXPORT jint JNICALL
Java_com_pitcharium_SpectrogramEngine_getA4Reference(JNIEnv* env, jobject thiz) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    return gA4;
}

JNIEXPORT jboolean JNICALL
Java_com_pitcharium_SpectrogramEngine_getProInspiredResult(JNIEnv* env, jobject, jfloatArray outResult) {
    if (!outResult || env->GetArrayLength(outResult) < 5) return JNI_FALSE;
    RecoveredProResult result;
    {
        std::lock_guard<std::mutex> lock(gRecoveredProMutex);
        result = gRecoveredProResult;
    }
    const jfloat values[5] = {result.valid ? 1.0f : 0.0f, result.rawHz, result.stabilizedHz,
                              result.pitch, result.confidence};
    env->SetFloatArrayRegion(outResult, 0, 5, values);
    return result.valid ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_pitcharium_SpectrogramEngine_resetFrequencyDetectionState(JNIEnv*, jobject) {
    resetFrequencyDetectionState();
}

JNIEXPORT jint JNICALL
Java_com_pitcharium_SpectrogramEngine_processAudio(JNIEnv* env, jobject thiz, jshortArray samples, jint length) {
    if (!samples || length <= 0 || length > env->GetArrayLength(samples)) return 0;
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (!gAudioBuffer) return 0;
    if (length > gAudioBufferCapacity) {
        // an incoming block larger than total ring capacity is ignored (Section 1)
        return 0;
    }

    jshort* src = env->GetShortArrayElements(samples, nullptr);
    if (!src) return 0;

    int free_space = gAudioBufferCapacity - gAvailableSamples;
    if (length > free_space) {
        // when existing pending data leaves insufficient room, read position advances past pending data and pending count resets before new block is written
        gReadPosition = gWritePosition;
        gAvailableSamples = 0;
    }

    // a block that crosses ring end is copied in two contiguous pieces (Section 1)
    int piece1 = std::min((int)length, gAudioBufferCapacity - gWritePosition);
    std::memcpy(&gAudioBuffer[gWritePosition], src, piece1 * sizeof(int16_t));
    if (piece1 < length) {
        int piece2 = length - piece1;
        std::memcpy(&gAudioBuffer[0], src + piece1, piece2 * sizeof(int16_t));
    }
    gWritePosition = (gWritePosition + length) % gAudioBufferCapacity;
    gAvailableSamples += length;
    gRecoveredProTotalSamples += (uint64_t)length;

    gRecoveredProRawFifo.insert(gRecoveredProRawFifo.end(), src, src + length);

    env->ReleaseShortArrayElements(samples, src, JNI_ABORT);

    int processedColumns = 0;
    const int stages = gFftOrder + 1;

    processRecoveredProFrames();

    // Process all available complete N-sample frames
    while (gAvailableSamples >= gN) {
        if (gUseMultithreading) {
            int slotIdx = gMainFrameIndex % 2;
            {
                std::unique_lock<std::mutex> lock(gPipelineMutex);
                gCvMain.wait(lock, [slotIdx] {
                    return !gThreadsRunning || gSlots[slotIdx].state == STATE_EMPTY;
                });
                if (!gThreadsRunning) return processedColumns;
            }

            // Core 1 (Main thread): Fetch and prepare inputs
            uint64_t stageStart = nowNanos();
                // Fetch frame & normalize
                int first = std::min(gN, gAudioBufferCapacity - gReadPosition);
                for (int i = 0; i < first; i++) {
                    gFrameFloat[i] = (float)gAudioBuffer[gReadPosition + i];
                }
                if (first < gN) {
                    int rem = gN - first;
                    for (int i = 0; i < rem; i++) {
                        gFrameFloat[first + i] = (float)gAudioBuffer[i];
                    }
                }

                float peak = 0.0f;
                for (int i = 0; i < gN; i++) {
                    float abs_val = std::abs(gFrameFloat[i]);
                    if (abs_val > peak) peak = abs_val;
                }

                if (peak > 0.0f) {
                    float inv_peak = 1.0f / peak;
                    for (int i = 0; i < gN; i++) {
                        gFrameFloat[i] *= inv_peak;
                    }
                } else {
                    std::memset(gFrameFloat, 0, gN * sizeof(float));
                }

                // Window & prepare inputs
                float gainFloat = 0.95001220703125f;
                for (int i = 0; i < gN; i++) {
                    float gained = gFrameFloat[i] * gainFloat;
                    gSlots[slotIdx].inputAFloat[i] = gained * gCoefficientAFloat[i];
                    gSlots[slotIdx].inputBFloat[i] = gained * gCoefficientBFloat[i];
                }
            gTimingNanos[0].fetch_add(nowNanos() - stageStart, std::memory_order_relaxed);

            // Set Slot Info and transition state
            {
                std::lock_guard<std::mutex> lock(gPipelineMutex);
                gSlots[slotIdx].frameIndex = gMainFrameIndex;
                gSlots[slotIdx].writeColumn = gWriteColumn;
                gSlots[slotIdx].state = STATE_PREPARED;
                gSlots[slotIdx].fftA_done = false;
                gSlots[slotIdx].fftB_done = false;

                gMainFrameIndex++;
            }
            gCvFFT.notify_all();

            gWriteColumn = (gWriteColumn + 1) & 255;
            gReadPosition = (gReadPosition + gHop) % gAudioBufferCapacity;
            gAvailableSamples -= gHop;

            processedColumns++;
        } else {
            uint64_t stageStart = nowNanos();
            // 1. Copy N-sample frame & normalize to [-1.0f, 1.0f]
            int first = std::min(gN, gAudioBufferCapacity - gReadPosition);
            for (int i = 0; i < first; i++) {
                gFrameFloat[i] = (float)gAudioBuffer[gReadPosition + i];
            }
            if (first < gN) {
                int rem = gN - first;
                for (int i = 0; i < rem; i++) {
                    gFrameFloat[first + i] = (float)gAudioBuffer[i];
                }
            }

            float peak = 0.0f;
            for (int i = 0; i < gN; i++) {
                float abs_val = std::abs(gFrameFloat[i]);
                if (abs_val > peak) peak = abs_val;
            }

            if (peak > 0.0f) {
                float inv_peak = 1.0f / peak;
                for (int i = 0; i < gN; i++) {
                    gFrameFloat[i] *= inv_peak;
                }
            } else {
                std::memset(gFrameFloat, 0, gN * sizeof(float));
            }

            // 3. Multiply by fixed gain 0.95001220703125 and window
            float gainFloat = 0.95001220703125f;
            for (int i = 0; i < gN; i++) {
                float gained = gFrameFloat[i] * gainFloat;
                gInputAFloat[i] = gained * gCoefficientAFloat[i];
                gInputBFloat[i] = gained * gCoefficientBFloat[i];
            }
            gTimingNanos[0].fetch_add(nowNanos() - stageStart, std::memory_order_relaxed);

            // 4. Run dual transforms
            stageStart = nowNanos();
            floatRadix2FFT(gInputAFloat);
            floatRadix2FFT(gInputBFloat);
            gTimingNanos[1].fetch_add(nowNanos() - stageStart, std::memory_order_relaxed);

            // 5. Magnitude and fractional-frequency estimation, scatter into pitch accumulators
            stageStart = nowNanos();
            std::memset(gAccumulatorFloat, 0, 1536 * sizeof(float));

            int maxBin = gN / 2;
            int bin = 1;

            #if defined(__ARM_NEON)
            int neonLimit = maxBin - 4;
            for (; bin <= neonLimit; bin += 4) {
                float32x4x2_t compA = vld2q_f32(&gInputAFloat[2 * bin]);
                float32x4_t realA_vec = compA.val[0];
                float32x4_t imagA_vec = compA.val[1];

                float32x4x2_t compB = vld2q_f32(&gInputBFloat[2 * bin]);
                float32x4_t realB_vec = compB.val[0];
                float32x4_t imagB_vec = compB.val[1];

                float32x4_t powerA_vec = vmlaq_f32(vmulq_f32(realA_vec, realA_vec), imagA_vec, imagA_vec);
                float32x4_t magnitude_vec = vsqrtq_f32(powerA_vec);
                float32x4_t cross_vec = vsubq_f32(vmulq_f32(realB_vec, imagA_vec), vmulq_f32(imagB_vec, realA_vec));

                float powerA_arr[4];
                float magnitude_arr[4];
                float cross_arr[4];

                vst1q_f32(powerA_arr, powerA_vec);
                vst1q_f32(magnitude_arr, magnitude_vec);
                vst1q_f32(cross_arr, cross_vec);

                for (int i = 0; i < 4; i++) {
                    int currentBin = bin + i;
                    float pA = powerA_arr[i];
                    if (pA <= 1e-12f) continue;

                    float mag = magnitude_arr[i];
                    if (mag == 0.0f) continue;

                    float cr = cross_arr[i];
                    float correctionQ4 = (cr / pA) * (float)gN * 16.0f / (float)(1 << gDerivativeCoefficientScale);
                    int32_t subbinQ4 = (int32_t)lroundf((float)currentBin * 16.0f + correctionQ4);

                    if (subbinQ4 < 0 || subbinQ4 >= gPitchMapLength) continue;

                    int32_t destinationQ4 = gFrequencyToPitchQ4[subbinQ4];
                    if (destinationQ4 < 0) continue;
                    int32_t destination = destinationQ4 >> 4;
                    int32_t fraction = destinationQ4 & 15;

                    if (destination >= 0 && destination < 1535) {
                        gAccumulatorFloat[destination] += (16.0f - (float)fraction) * mag;
                        gAccumulatorFloat[destination + 1] += (float)fraction * mag;
                    }
                }
            }
            #endif

            for (; bin < maxBin; bin++) {
                float realA = gInputAFloat[2 * bin];
                float imagA = gInputAFloat[2 * bin + 1];
                float realB = gInputBFloat[2 * bin];
                float imagB = gInputBFloat[2 * bin + 1];

                float powerA = realA * realA + imagA * imagA;
                if (powerA <= 1e-12f) continue;

                float mag = sqrtf(powerA);
                if (mag == 0.0f) continue;

                float cross = realB * imagA - imagB * realA;
                float correctionQ4 = (cross / powerA) * (float)gN * 16.0f / (float)(1 << gDerivativeCoefficientScale);
                int32_t subbinQ4 = (int32_t)lroundf((float)bin * 16.0f + correctionQ4);

                if (subbinQ4 < 0 || subbinQ4 >= gPitchMapLength) continue;

                int32_t destinationQ4 = gFrequencyToPitchQ4[subbinQ4];
                if (destinationQ4 < 0) continue;
                int32_t destination = destinationQ4 >> 4;
                int32_t fraction = destinationQ4 & 15;

                if (destination >= 0 && destination < 1535) {
                    gAccumulatorFloat[destination] += (16.0f - (float)fraction) * mag;
                    gAccumulatorFloat[destination + 1] += (float)fraction * mag;
                }
            }
            gTimingNanos[2].fetch_add(nowNanos() - stageStart, std::memory_order_relaxed);
            stageStart = nowNanos();

            // 6. Scale accumulators to Q15 range
            float max_accum = 0.0f;
            for (int i = 0; i < 1536; i++) {
                if (gAccumulatorFloat[i] > max_accum) max_accum = gAccumulatorFloat[i];
            }

            if (max_accum > 0.0f) {
                float scale = 32767.0f / max_accum;
                for (int i = 0; i < 1536; i++) {
                    gSpectrumFloat[i] = gAccumulatorFloat[i] * scale;
                }
            } else {
                std::memset(gSpectrumFloat, 0, 1536 * sizeof(float));
            }


            // 8. 30 dB Logarithmic compression
            for (int i = 0; i < 1536; i++) {
                float val = gSpectrumFloat[i];
                if (val <= 0.0f) {
                    gSpectrumFloat[i] = 0.0f;
                } else {
                    float ratio = val / 32768.0f;
                    float db = 20.0f * log10f(ratio) + 30.0f;
                    if (db < 0.0f) {
                        gSpectrumFloat[i] = 0.0f;
                    } else {
                        gSpectrumFloat[i] = db * (32767.0f / 30.0f);
                    }
                }
            }

            // 9. Single-Pass Pitch-axis smoothing in float
            floatSmoothing(gSpectrumFloat, gSmoothAFloat);

            // 10. Post-smoothing positive-peak normalization
            float maximum = 0.0f;
            for (int i = 0; i < 1536; i++) {
                if (gSmoothAFloat[i] > maximum) maximum = gSmoothAFloat[i];
            }

            if (maximum > 0.0f) {
                float scale = 32767.0f / maximum;
                for (int i = 0; i < 1536; i++) {
                    gSpectrumFloat[i] = gSmoothAFloat[i] * scale;
                }
            } else {
                std::memset(gSpectrumFloat, 0, 1536 * sizeof(float));
            }

            // 11. Octave Folding
            float foldedFloat[192];
            for (int row = 0; row < 192; row++) {
                foldedFloat[row] = 0.0f;
                for (int octave = 0; octave < 8; octave++) {
                    foldedFloat[row] += gSpectrumFloat[row + octave * 192];
                }
                gLastFolded[row] = (int32_t)foldedFloat[row];
            }

            // 12. Display-Column Normalization to range 0..256
            float foldedMax = 0.0f;
            for (int row = 0; row < 192; row++) {
                if (foldedFloat[row] > foldedMax) foldedMax = foldedFloat[row];
            }

            float scalarFloat[192];
            for (int row = 0; row < 192; row++) {
                if (foldedMax > 0.0f) {
                    scalarFloat[row] = (foldedFloat[row] * 256.0f) / foldedMax;
                } else {
                    scalarFloat[row] = 0.0f;
                }
            }

            // 13. Intensity-to-Color Conversion and Texture Column Construction
            for (int row = 0; row < 192; row++) {
                float r = (scalarFloat[row] * (float)gPalette[row][0] * 320.0f) / 65536.0f;
                float g = (scalarFloat[row] * (float)gPalette[row][1] * 320.0f) / 65536.0f;
                float b = (scalarFloat[row] * (float)gPalette[row][2] * 320.0f) / 65536.0f;

                int32_t ri = (int32_t)r;
                int32_t gi = (int32_t)g;
                int32_t bi = (int32_t)b;

                if (ri > 255) ri = 255; else if (ri < 0) ri = 0;
                if (gi > 255) gi = 255; else if (gi < 0) gi = 0;
                if (bi > 255) bi = 255; else if (bi < 0) bi = 0;

                uint32_t color = (0xFFu << 24) | ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
                gHistoryTexture[row][gWriteColumn] = color;
            }

            gHistoryTexture[192][gWriteColumn] = gHistoryTexture[0][gWriteColumn];

            for (int row = 193; row < 256; row++) {
                gHistoryTexture[row][gWriteColumn] = 0;
            }

            enqueueColumn(gWriteColumn);
            gTimingNanos[3].fetch_add(nowNanos() - stageStart, std::memory_order_relaxed);

            // Advance writeColumn (circular modulo 256)
            gWriteColumn = (gWriteColumn + 1) & 255;

            // Advance readCursor and available counts
            gReadPosition = (gReadPosition + gHop) % gAudioBufferCapacity;
            gAvailableSamples -= gHop;

            processedColumns++;
        }
    }

    return processedColumns;
}

JNIEXPORT jint JNICALL
Java_com_pitcharium_SpectrogramEngine_drainColumns(JNIEnv* env, jobject thiz,
        jintArray outPixels, jlongArray outSequences, jlongArray outSamplePositions,
        jlongArray outStatus, jint maxColumns) {
    if (!outPixels || !outSequences || !outSamplePositions || !outStatus || maxColumns <= 0 ||
        env->GetArrayLength(outPixels) < maxColumns * 256 ||
        env->GetArrayLength(outSequences) < maxColumns ||
        env->GetArrayLength(outSamplePositions) < maxColumns || env->GetArrayLength(outStatus) < 9) return 0;

    jint* pixels = env->GetIntArrayElements(outPixels, nullptr);
    jlong* sequences = env->GetLongArrayElements(outSequences, nullptr);
    jlong* positions = env->GetLongArrayElements(outSamplePositions, nullptr);
    jlong* status = env->GetLongArrayElements(outStatus, nullptr);
    if (!pixels || !sequences || !positions || !status) {
        if (pixels) env->ReleaseIntArrayElements(outPixels, pixels, 0);
        if (sequences) env->ReleaseLongArrayElements(outSequences, sequences, 0);
        if (positions) env->ReleaseLongArrayElements(outSamplePositions, positions, 0);
        if (status) env->ReleaseLongArrayElements(outStatus, status, 0);
        return 0;
    }

    int count;
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        count = std::min((size_t)maxColumns, gQueueCount);
        for (int i = 0; i < count; i++) {
            const QueuedColumn& column = gColumnQueue[(gQueueHead + i) % gColumnQueue.size()];
            sequences[i] = (jlong)column.sequence;
            positions[i] = (jlong)column.samplePosition;
            for (int row = 0; row < 256; row++) pixels[i * 256 + row] = (jint)column.pixels[row];
        }
        if (count > 0) gQueueHead = (gQueueHead + count) % gColumnQueue.size();
        gQueueCount -= count;
        status[0] = (jlong)gQueueCount;
        status[1] = (jlong)gQueueMaxDepth;
        status[2] = (jlong)gGeneratedColumns;
        status[3] = (jlong)gResetEpoch;
    }
    for (int i = 0; i < 5; i++) status[4 + i] = (jlong)gTimingNanos[i].load(std::memory_order_relaxed);

    env->ReleaseIntArrayElements(outPixels, pixels, 0);
    env->ReleaseLongArrayElements(outSequences, sequences, 0);
    env->ReleaseLongArrayElements(outSamplePositions, positions, 0);
    env->ReleaseLongArrayElements(outStatus, status, 0);
    return count;
}

JNIEXPORT void JNICALL
Java_com_pitcharium_SpectrogramEngine_resetPresentationQueue(JNIEnv* env, jobject thiz) {
    std::lock_guard<std::mutex> engineLock(gEngineMutex);
    resetRecoveredProState();
    std::lock_guard<std::mutex> lock(gQueueMutex);
    gQueueHead = gQueueCount = 0;
    gQueueMaxDepth = 0;
    gGeneratedColumns = 0;
    gResetEpoch++;
}

JNIEXPORT void JNICALL
Java_com_pitcharium_SpectrogramEngine_getHistoryTexture(JNIEnv* env, jobject thiz, jintArray outPixels) {
    if (!outPixels || env->GetArrayLength(outPixels) < 256 * 256) return;
    std::lock_guard<std::mutex> lock(gEngineMutex);
    jint* dest = env->GetIntArrayElements(outPixels, nullptr);
    if (!dest) return;

    // Unwrap circular texture around gWriteColumn (Section 17)
    // Newest column is at index 255, oldest at index 0.
    // logical newest on right edge means:
    // x = 0 represents the oldest column in history, which is at index gWriteColumn
    // x = 255 represents the newest column, which is at index (gWriteColumn - 1 + 256) % 256
    for (int x = 0; x < 256; x++) {
        int srcX = (gWriteColumn + x) & 255;
        for (int y = 0; y < 256; y++) {
            dest[y * 256 + x] = (jint)gHistoryTexture[y][srcX];
        }
    }

    env->ReleaseIntArrayElements(outPixels, dest, 0);
}

JNIEXPORT void JNICALL
Java_com_pitcharium_SpectrogramEngine_getFoldedSpectrum(JNIEnv* env, jobject thiz, jintArray outFolded) {
    if (!outFolded || env->GetArrayLength(outFolded) < 192) return;
    std::lock_guard<std::mutex> lock(gEngineMutex);
    jint* dest = env->GetIntArrayElements(outFolded, nullptr);
    if (!dest) return;
    for (int i = 0; i < 192; i++) {
        dest[i] = (jint)gLastFolded[i];
    }
    env->ReleaseIntArrayElements(outFolded, dest, 0);
}

JNIEXPORT void JNICALL
Java_com_pitcharium_SpectrogramEngine_cleanup(JNIEnv* env, jobject thiz) {
    stopThreads();
    std::lock_guard<std::mutex> lock(gEngineMutex);
    resetRecoveredProState();
    for (int i = 0; i < 2; i++) {
        if (gSlots[i].inputAFloat) { free(gSlots[i].inputAFloat); gSlots[i].inputAFloat = nullptr; }
        if (gSlots[i].inputBFloat) { free(gSlots[i].inputBFloat); gSlots[i].inputBFloat = nullptr; }
    }
    if (gFrequencyToPitchQ4) { free(gFrequencyToPitchQ4); gFrequencyToPitchQ4 = nullptr; }

    if (gFrameFloat) { free(gFrameFloat); gFrameFloat = nullptr; }
    if (gInputAFloat) { free(gInputAFloat); gInputAFloat = nullptr; }
    if (gInputBFloat) { free(gInputBFloat); gInputBFloat = nullptr; }
    if (gCoefficientAFloat) { free(gCoefficientAFloat); gCoefficientAFloat = nullptr; }
    if (gCoefficientBFloat) { free(gCoefficientBFloat); gCoefficientBFloat = nullptr; }
    if (gFftTwiddlesFloat) { free(gFftTwiddlesFloat); gFftTwiddlesFloat = nullptr; }
    if (gAccumulatorFloat) { free(gAccumulatorFloat); gAccumulatorFloat = nullptr; }
    if (gSpectrumFloat) { free(gSpectrumFloat); gSpectrumFloat = nullptr; }
    if (gSmoothAFloat) { free(gSmoothAFloat); gSmoothAFloat = nullptr; }
    if (gSmoothBFloat) { free(gSmoothBFloat); gSmoothBFloat = nullptr; }

    gFftSwaps.clear();
    if (gAudioBuffer) { free(gAudioBuffer); gAudioBuffer = nullptr; }
    std::lock_guard<std::mutex> queueLock(gQueueMutex);
    gColumnQueue.clear();
    gQueueHead = gQueueCount = 0;
}

}
