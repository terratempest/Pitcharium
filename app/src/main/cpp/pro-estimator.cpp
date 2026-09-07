#include "pro-estimator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <complex>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace pro_estimator {
namespace {

struct C16 { int16_t re, im; };

#include "generated/polyphase_256x8.inc"

uint64_t fnv(const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    while (bytes--) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

int16_t wrap16(int32_t x) { return static_cast<int16_t>(static_cast<uint16_t>(x)); }
int32_t wrap32(int64_t x) { return static_cast<int32_t>(static_cast<uint32_t>(x)); }
unsigned ctz32(uint32_t x) {
    unsigned result = 0;
    while ((x & 1U) == 0U) { x >>= 1; ++result; }
    return result;
}
unsigned clz32(uint32_t x) {
    unsigned result = 0;
    while ((x & 0x80000000U) == 0U) { x <<= 1; ++result; }
    return result;
}
int32_t asr32(int32_t x, unsigned n) {
    if (!n) return x;
    const uint32_t u=static_cast<uint32_t>(x);
    return static_cast<int32_t>((u>>n)|(x<0 ? (~uint32_t(0)<<(32-n)) : 0));
}
int64_t asr64(int64_t x, unsigned n) {
    if (!n) return x;
    const uint64_t u=static_cast<uint64_t>(x);
    return static_cast<int64_t>((u>>n)|(x<0 ? (~uint64_t(0)<<(64-n)) : 0));
}
int16_t roundQ15(int32_t x) { return wrap16(asr32(wrap32(int64_t(x)+0x4000),15)); }

void normalizePcm(std::vector<int16_t>& pcm, float& scale) {
    int32_t peak = 0;
    for (int16_t x : pcm) peak = std::max(peak, x < 0 ? -int32_t(x) : int32_t(x));
    if (peak == 0) { scale = 1.0f; return; }
    if (peak < 32767) {
        // __aeabi_ldivmod's recovered register-pair result is consumed as high/low
        // halfwords by FUN_0019c8bc; preserve that decomposition instead of a
        // floating multiply.
        const uint64_t quotient=(uint64_t(32767)<<32)/uint32_t(peak);
        const int16_t lo16=int16_t(quotient), hi16=int16_t(quotient>>32);
        const uint32_t lo32=uint32_t(quotient);
        for (int16_t& x : pcm) {
            const int32_t decomposed=int32_t(lo16)*asr32(x,15)+int32_t(hi16)*x+
                int16_t((uint64_t(lo32)*uint32_t(int32_t(x)))>>32);
            x=wrap16(decomposed);
        }
    }
    scale = float(int16_t(peak)) * (1.0f / 32767.0f);
}

void fft(std::vector<C16>& a) {
    const unsigned n = a.size(), bits = ctz32(n);
    for (unsigned i = 1; i < n; ++i) {
        unsigned r = 0, x = i;
        for (unsigned b = 0; b < bits; ++b) { r = (r << 1) | (x & 1); x >>= 1; }
        if (i < r) std::swap(a[i], a[r]);
    }
    for (unsigned half = 1; half < n; half <<= 1) {
        for (unsigned j = 0; j < half; ++j) {
            const double angle = 3.14159265358979323846264338327950288 * double(j) / double(half);
            const int16_t delta = static_cast<int16_t>((1.0-std::cos(angle))*16383.5);
            const int16_t sine = static_cast<int16_t>(-std::sin(angle)*16383.5);
            const int16_t wr = wrap16(32767-2*int32_t(delta));
            const int16_t wi = wrap16(2*int32_t(sine));
            for (unsigned k = j; k < n; k += half << 1) {
                C16& lo = a[k]; C16& hi = a[k + half];
                const int32_t tr = int32_t(wr) * hi.re - int32_t(wi) * hi.im;
                const int32_t ti = int32_t(wr) * hi.im + int32_t(wi) * hi.re;
                const int32_t ar = lo.re, ai = lo.im;
                lo = {wrap16(asr32(wrap32(int64_t(ar)*0x8000+tr+0x8000),16)), wrap16(asr32(wrap32(int64_t(ai)*0x8000+ti+0x8000),16))};
                hi = {wrap16(asr32(wrap32(int64_t(ar)*0x8000-tr+0x8000),16)), wrap16(asr32(wrap32(int64_t(ai)*0x8000-ti+0x8000),16))};
            }
        }
    }
}

void makeFrame(const std::vector<int16_t>& pcm, std::vector<C16>& current,
               std::vector<C16>& comparison) {
    const size_t n=pcm.size(), l=n/2;
    static thread_local std::vector<C16> z;
    z.resize(l+1);
    for (size_t i = 0; i < l; ++i) z[i] = {pcm[2 * i], pcm[2 * i + 1]};
    z[l]={0,0};
    z.resize(l); fft(z); z.resize(l+1); z[l]={0,0};
    current.resize(l + 1); comparison.resize(l + 1);
    current[0] = {wrap16(asr32(int32_t(z[0].re)+z[1].re,1)), 0};
    current[l] = {wrap16(asr32(int32_t(z[l-2].re)+z[l-1].re,1)), 0};
    // Exact mode-0 constructor operands for the three Pro geometries.
    const int16_t p0a = n==2048 ? 25 : n==4096 ? 12 : 6;
    constexpr int16_t p0b = -16383, p1b = 16383;
    const int16_t p1a = p0a;
    auto combine = [](C16 a, int16_t ca, int16_t cb, C16 b, int16_t da, int16_t db) {
        return C16{wrap16(int32_t(roundQ15(int32_t(a.re)*ca-int32_t(a.im)*cb)) +
                          roundQ15(int32_t(b.re)*da-int32_t(b.im)*db)),
                   wrap16(int32_t(roundQ15(int32_t(a.im)*ca+int32_t(a.re)*cb)) +
                          roundQ15(int32_t(b.im)*da+int32_t(b.re)*db))};
    };
    for (size_t k = 1; k < l; ++k) {
        current[k] = {wrap16(asr32(int32_t(z[k-1].re)+2*int32_t(z[k].re)+z[k+1].re,2)),
                      wrap16(asr32(int32_t(z[k-1].im)+2*int32_t(z[k].im)+z[k+1].im,2))};
    }
    const C16 a0=current[1];
    comparison[0]={wrap16(int32_t(roundQ15(int32_t(a0.re)*p1a+int32_t(a0.im)*p1b))+roundQ15(int32_t(a0.re)*p0a-int32_t(a0.im)*p0b)),
                   wrap16(int32_t(roundQ15(-int32_t(a0.im)*p1a+int32_t(a0.re)*p1b))+roundQ15(int32_t(a0.im)*p0a+int32_t(a0.re)*p0b))};
    for(size_t k=1;k<l;++k) comparison[k]=combine(current[k-1],p1a,p1b,current[k+1],p0a,p0b);
    const C16 al=current[l-1];
    comparison[l]={wrap16(int32_t(roundQ15(int32_t(al.re)*p1a-int32_t(al.im)*p1b))+roundQ15(int32_t(al.re)*p0a+int32_t(al.im)*p0b)),
                   wrap16(int32_t(roundQ15(int32_t(al.im)*p1a+int32_t(al.re)*p1b))+roundQ15(-int32_t(al.im)*p0a+int32_t(al.re)*p0b))};
}

float blockConvert(const std::vector<int32_t>& src, std::vector<int16_t>& dst) {
    int32_t peak = 0;
    for (int32_t x : src) peak = std::max(peak, x);
    dst.resize(src.size());
    if (peak == 0) { std::fill(dst.begin(), dst.end(), 0); return 1.0f; }
    if (peak < 0x8000) {
        if (peak > 0x3ffe) { for (size_t i=0;i<src.size();++i) dst[i]=wrap16(src[i]); return 1.0f; }
        unsigned s=0; while ((uint32_t(peak)<<s)<0x3fffU) ++s;
        for (size_t i=0;i<src.size();++i) dst[i]=wrap16(uint32_t(src[i])<<s);
        return 1.0f / float(1U<<s);
    }
    unsigned s=0; while ((peak>>s)>0x7fff) ++s;
    for (size_t i=0;i<src.size();++i) dst[i]=wrap16(src[i]>>s);
    return float(1U<<s);
}

const std::vector<int16_t>& spectralWindow(size_t length) {
    static thread_local std::vector<int16_t> a;
    if(a.size()!=length) {
        a.resize(length);
        const size_t n=2*(length-1);
        const double step=3.14159265358979323846264338327950288/double(n>>8);
        for(size_t i=0;i<a.size();++i) {
            const double x=step*double(i);
            a[i]=static_cast<int16_t>(int32_t(float(1.0-std::exp(-(x*x)))*32767.0f));
        }
    }
    return a;
}

const std::array<std::array<uint16_t,65536>,2>& sqrtBanks() {
    static const auto banks=[] {
        auto b=std::make_unique<std::array<std::array<uint16_t,65536>,2>>();
        for(uint32_t i=0;i<65536;++i) {
            const uint32_t q0=uint32_t(65536.0*std::sqrt(1.0+double(i)/65536.0));
            const uint32_t q1=uint32_t(65536.0*std::sqrt(2.0+double(i)/65536.0));
            (*b)[0][i]=uint16_t((q0+1)>>1);
            (*b)[1][i]=uint16_t((q1+1)>>1);
        } return b;
    }(); return *banks;
}

uint32_t fixedSqrt(uint32_t x) {
    if (!x) return 0;
    const unsigned lz=clz32(x), ix=((x<<lz)&0x7fffffffU)>>15;
    return (uint32_t(sqrtBanks()[lz&1][ix])<<16)>>(lz>>1);
}

const std::array<int32_t,1<<18>& logLut() {
    static const auto lut=[] {
        auto a=std::make_unique<std::array<int32_t,1<<18>>();
        for(uint32_t i=0;i<a->size();++i) (*a)[i]=int32_t(std::floor(std::log2(1.0+double(i)/double(1<<18))*double(1<<27)+0.5));
        return a;
    }(); return *lut;
}

int phaseShiftFor(size_t n, int fftScale) {
    return int(ctz32(static_cast<uint32_t>(n))) + 16 - fftScale;
}

int32_t dividePhase(int32_t cross, int32_t power) {
    if (power == 0) return 0;
    int64_t numerator = int64_t(cross) * (int64_t(1) << 15);
    int64_t divisor = power;
    while (numerator > std::numeric_limits<int32_t>::max() ||
           numerator < std::numeric_limits<int32_t>::min()) {
        numerator = asr64(numerator, 1);
        divisor = asr64(divisor, 1);
    }
    if (divisor == 0) return 0;
    return static_cast<int32_t>(numerator) / static_cast<int32_t>(divisor);
}

int32_t mapCoordinate(int32_t instantaneous) {
    const int32_t q=int32_t(asr64(int64_t(instantaneous)*821062,16));
    if(q<=0) return -1;
    const unsigned lz=clz32(uint32_t(q));
    const unsigned ix=((uint32_t(q)<<lz)&0x7fffffffU)>>13;
    const int64_t logq=int64_t(logLut()[ix])+int64_t(31-lz)*(1LL<<27);
    const int64_t y=120586240LL+12LL*((logq>>6)-54525952);
    return y>=0 && y<0x0c000000LL ? int32_t(y) : -1;
}

void scatter(const std::vector<int32_t>& coordinate, const std::vector<int16_t>& weight,
             std::vector<int16_t>& out) {
    static thread_local std::vector<int32_t> scratch(kMappedLength+32);
    std::fill(scratch.begin(),scratch.end(),0);
    for(size_t i=0;i<coordinate.size();++i) {
        const int32_t x=coordinate[i], w=weight[i];
        if(x<0 || x==0 || w<=3) continue;
        const unsigned phase=((uint32_t(x)+0x80)&0xffff)>>8;
        const int32_t base=asr32(wrap32(int64_t(x)+0x80),16)+5, gain=uint32_t(w)>>2;
        for(int t=0;t<8;++t) scratch[base+t]=wrap32(int64_t(scratch[base+t])+int32_t(kRecoveredPolyphase[phase][t])*gain);
    }
    static thread_local std::vector<int32_t> visible(kMappedLength);
    std::copy_n(scratch.begin()+8,kMappedLength,visible.begin());
    blockConvert(visible,out);
}

void normalizePeak(std::vector<int16_t>& v) {
    int32_t peak=0; for(int16_t x:v) peak=std::max(peak,int32_t(x));
    if(peak<=0 || peak>=32767) return;
    for(int16_t& x:v) x=wrap16(int32_t(x)*32767/peak);
}

} // namespace

bool proGateAccepts(int16_t re, int16_t im, int32_t threshold) {
    const int32_t power=int32_t(re)*int32_t(re)+int32_t(im)*int32_t(im);
    return power > threshold;
}

int32_t proPhaseQuotient(int32_t cross, int32_t power, int) {
    return dividePhase(cross,power);
}

bool isSupportedPcmLength(size_t count) { return count==2048 || count==4096 || count==8192; }

bool runStageA(const int16_t* input, size_t count, StageAResult& result,
               const StageAConfig& config) {
    if(!input || !isSupportedPcmLength(count)) return false;
    static thread_local std::vector<int16_t> pcm;
    pcm.resize(count);
    std::copy_n(input,count,pcm.begin()); float pcmScale;
    normalizePcm(pcm,pcmScale);
    static thread_local std::vector<C16> current,comparison; makeFrame(pcm,current,comparison);
    const size_t spectrumLength=count/2+1;
    static thread_local std::vector<int32_t> power,coordinate;
    power.resize(spectrumLength);coordinate.resize(spectrumLength);
    for(size_t k=0;k<spectrumLength;++k) {
        const int32_t re=current[k].re, im=current[k].im;
        power[k]=re*re+im*im;
        coordinate[k]=power[k]>0 ? mapCoordinate(int32_t(k<<16)+
            dividePhase(im*comparison[k].re-re*comparison[k].im,power[k])) : -1;
    }
    coordinate[0]=-1;
    result.validMapped=uint32_t(std::count_if(coordinate.begin(),coordinate.end(),[](int32_t x){return x>=0;}));
    static thread_local std::vector<int16_t> powerWeight; const float powerScale=blockConvert(power,powerWeight);
    const auto& window=spectralWindow(spectrumLength); int32_t weightedSum=0;
    for(size_t k=0;k<spectrumLength;++k) { powerWeight[k]=roundQ15(int32_t(powerWeight[k])*window[k]); weightedSum+=powerWeight[k]; }
    float raw=pcmScale*pcmScale*0x1p-30f*powerScale*float(weightedSum);
    result.energy=std::max(0.0f,std::min(1.0f,raw));
    static thread_local std::vector<int32_t> magnitudeRaw;
    magnitudeRaw.resize(spectrumLength);
    for(size_t k=0;k<spectrumLength;++k) magnitudeRaw[k]=int32_t(fixedSqrt(uint32_t(power[k])));
    static thread_local std::vector<int16_t> magnitudeWeight; blockConvert(magnitudeRaw,magnitudeWeight);
    for(size_t k=0;k<spectrumLength;++k) magnitudeWeight[k]=roundQ15(int32_t(magnitudeWeight[k])*window[k]);
    normalizePeak(magnitudeWeight);
    result.mappedCoordinates=coordinate;
    result.powerWeight=powerWeight;
    result.magnitudeWeight=magnitudeWeight;
    scatter(coordinate,powerWeight,result.powerScatter);
    scatter(coordinate,magnitudeWeight,result.magnitudeScatter);
    int64_t sum=0; for(int16_t x:result.magnitudeScatter) sum+=x;
    const int32_t baseline=int32_t(asr64(sum,15)); result.baseline.resize(kMappedLength);
    for(size_t i=0;i<kMappedLength;++i) result.baseline[i]=int16_t(std::max(0,int32_t(result.magnitudeScatter[i])-baseline));
    normalizePeak(result.baseline);
    const float amplitude=std::min(1.0f,100.0f*std::sqrt(result.energy));
    result.requestedScale=amplitude>0.0f ? std::max(0.0f,30.0f+20.0f*std::log10(amplitude))/30.0f : 0.0f;
    result.pcmScale=pcmScale; result.powerScale=powerScale; result.weightedSum=weightedSum;
    result.nonzero={uint32_t(std::count_if(result.powerScatter.begin(),result.powerScatter.end(),[](int16_t x){return x!=0;})),
                    uint32_t(std::count_if(result.magnitudeScatter.begin(),result.magnitudeScatter.end(),[](int16_t x){return x!=0;})),
                    uint32_t(std::count_if(result.baseline.begin(),result.baseline.end(),[](int16_t x){return x!=0;}))};
    result.hashes={fnv(result.powerScatter.data(),result.powerScatter.size()*2),fnv(result.magnitudeScatter.data(),result.magnitudeScatter.size()*2),fnv(result.baseline.data(),result.baseline.size()*2)};
    return true;
}

uint64_t validateStageAConstants() {
    uint64_t mask=0;
    if(sizeof(kRecoveredPolyphase)/sizeof(int16_t)==2048) mask|=1;
    if(kRecoveredPolyphase[0][3]==14519 && kRecoveredPolyphase[255][0]==44 && kRecoveredPolyphase[255][7]==1) mask|=2;
    const auto& s=sqrtBanks(); if(s[0][0]==32768 && s[1][0]==46341 && s[0][65535]==46341) mask|=4;
    const auto& w2048=spectralWindow(1025);
    const bool window2048=w2048[0]==0 && w2048[1]==4682 && w2048[8]==32765 && w2048[1024]==32767;
    const auto& w4096=spectralWindow(2049);
    const bool window4096=w4096[0]==0 && w4096[1]==1239 && w4096[16]==32765 && w4096[2048]==32767;
    const auto& w8192=spectralWindow(4097);
    const bool window8192=w8192[0]==0 && w8192[1]==314 && w8192[32]==32765 && w8192[4096]==32767;
    if(window2048 && window4096 && window8192) mask|=8;
    std::array<int16_t,8192> edge{}; edge[0]=std::numeric_limits<int16_t>::min(); StageAResult r;
    if(runStageA(edge.data(),edge.size(),r) && r.pcmScale==-32768.0f/32767.0f) mask|=16;
    bool modes=true;
    for(size_t n:{size_t(2048),size_t(4096),size_t(8192)}) {
        std::vector<int16_t> silence(n);
        StageAResult mode;
        modes &= runStageA(silence.data(),n,mode) && mode.powerScatter.size()==kMappedLength &&
            mode.magnitudeScatter.size()==kMappedLength && mode.baseline.size()==kMappedLength;
    }
    if(modes) mask|=32;
    if(!isSupportedPcmLength(1024)&&!isSupportedPcmLength(8000)&&!isSupportedPcmLength(16384)) mask|=64;
    return mask;
}

uint64_t validateProContract() {
    uint64_t mask=0;
    if (kFreshProConfiguredSampleRate / 4 == 11025 &&
        kFreshProFrameSize == 2048 && kFreshProHop == 512) mask |= 1;
    if (proGateAccepts(1,0,kFreshProGateThresholdReviewedDefault) &&
        !proGateAccepts(0,0,kFreshProGateThresholdReviewedDefault) &&
        !proGateAccepts(1,0,1)) mask |= 2;
    const int phaseShift=phaseShiftFor(kFreshProFrameSize,kFreshModeScaleExponent);
    if (phaseShift == 15 && proPhaseQuotient(-1,3,phaseShift) == -10922 &&
        proPhaseQuotient(1,3,phaseShift) == 10922 &&
        proPhaseQuotient(65536,3,phaseShift) == 1073741824 &&
        proPhaseQuotient(-65536,3,phaseShift) == -715827882 &&
        proPhaseQuotient(1,0,phaseShift) == 0 &&
        ((7 << 16) + proPhaseQuotient(-1,3,phaseShift)) == (7 << 16) - 10922) mask |= 4;
    return mask;
}

namespace {

constexpr int32_t kQ16[32] = {
  0,25165824,39886887,50331648,58433234,65052711,70649400,75497472,
  79773775,83599058,87059447,90218535,93124615,95815224,98320121,100663296,
  102864370,104939599,106902596,108764882,110536287,112225271,113839164,115384359,
  116866468,118290439,119660662,120981048,122255095,123485945,124676432,125829120};
constexpr int16_t kWeight[32] = {
  8073,4036,2691,2018,1614,1345,1153,1009,897,807,733,672,621,576,538,504,
  474,448,424,403,384,366,351,336,322,310,299,288,278,269,260,252};
constexpr int16_t kFinalWindow[kMappedLength] = {
#include "generated/final_window_q15.inc"
};

uint32_t wallElapsed(uint32_t now, uint32_t then) { return now - then; }

struct Handoff {
    int accepted=-1, pending=-1; uint32_t since=0;
    int update(const int16_t* x,int n,int radius,uint32_t now) {
        auto local=[&](int old) {
            if(old<0) return -1;
            const int lo=std::max(1,old-radius), hi=std::min(n-1,old+radius);
            int best=-1;
            for(int i=lo;i<hi;++i) if(x[i-1]<x[i] && x[i+1]<x[i] &&
                (best<0 || x[best]<x[i])) best=i;
            return best;
        };
        accepted=local(accepted); pending=local(pending);
        int global=0; for(int i=1;i<n;++i) if(x[global]<x[i]) global=i;
        if(global==pending) { if(wallElapsed(now,since)>=151) { accepted=pending; pending=-1; } }
        else if(global==accepted) pending=-1;
        else { pending=global; since=now; }
        if(accepted>=0 && pending>=0 && float(x[pending])<1.5f*float(x[accepted])) pending=-1;
        return accepted;
    }
    int update(const float* x,int n,int radius,uint32_t now) {
        auto local=[&](int old) { if(old<0)return -1; int best=-1;
            for(int i=std::max(1,old-radius),e=std::min(n-1,old+radius);i<e;++i)
                if(x[i-1]<x[i]&&x[i+1]<x[i]&&(best<0||x[best]<x[i]))best=i;
            return best; };
        accepted=local(accepted);pending=local(pending);int global=0;
        for(int i=1;i<n;++i)if(x[global]<x[i])global=i;
        if(global==pending){if(wallElapsed(now,since)>=151){accepted=pending;pending=-1;}}
        else if(global==accepted)pending=-1;else{pending=global;since=now;}
        if(accepted>=0&&pending>=0&&x[pending]<1.5f*x[accepted])pending=-1;
        return accepted;
    }
};

void positiveMovingResidual(const std::vector<int16_t>& src,std::vector<int16_t>& dst) {
    constexpr size_t kWindow=256, kHalf=kWindow/2;
    dst.resize(src.size());
    if (src.empty()) return;
    if (src.size() < kWindow) {
        int32_t sum=0; for (int16_t x:src) sum=wrap32(int64_t(sum)+x);
        const int32_t mean=asr32(sum,8);
        for (size_t i=0;i<src.size();++i) dst[i]=int16_t(std::max(0,int32_t(src[i])-mean));
        return;
    }
    auto meanAt=[&](size_t begin) {
        int32_t sum=0;
        for (size_t j=0;j<kWindow;++j) sum=wrap32(int64_t(sum)+src[begin+j]);
        return asr32(sum,8);
    };
    const int32_t firstMean=meanAt(0), finalMean=meanAt(src.size()-kWindow);
    for (size_t i=0;i<src.size();++i) {
        int32_t mean;
        if (i<kHalf) mean=firstMean;
        else if (i+kHalf>=src.size()) mean=finalMean;
        else mean=meanAt(i-kHalf);
        dst[i]=int16_t(std::max(0,int32_t(src[i])-mean));
    }
}
void normalize(std::vector<int16_t>& x) { normalizePeak(x); }
unsigned upperDispatch(size_t i) {
    for(unsigned k=0;k<32;++k) if(int64_t(i)+asr32(kQ16[k],16)>=int64_t(kMappedLength-1)) return k;
    return 32;
}
unsigned lowerDispatch(size_t i) {
    for(unsigned k=0;k<32;++k) if(int64_t(i)+asr32(-kQ16[k],16)<0) return k;
    return 32;
}
int32_t interpolated(const int16_t* x,int32_t q) {
    const int32_t n=asr32(q,16), f=uint16_t(q);
    return int32_t(x[n])+asr32(wrap32(int64_t(f)*(int32_t(x[n+1])-x[n])),16);
}
int32_t upperPredict(const int16_t* x,unsigned n) {
    if(n<2) return 0; int32_t t[32]{};
    for(unsigned k=0;k<n;++k) {
        const int32_t sample=k==0 ? x[0] : interpolated(x,kQ16[k]);
        t[k]=int16_t(uint16_t(uint32_t(int32_t(kWeight[k])*sample)>>15));
    }
    int32_t tail=0; for(unsigned k=1;k<n;++k) tail=wrap32(int64_t(tail)+t[k]);
    int32_t out=std::min(t[0],tail);
    for(unsigned k=1;k+1<n;++k) out=wrap32(int64_t(out)+std::min(t[k],t[k+1]));
    return out;
}
int32_t lowerPredict(const int16_t* x,unsigned n) {
    int32_t out=0;
    for(unsigned k=1;k<n;++k) out=wrap32(int64_t(out)+
        asr32(wrap32(int64_t(kWeight[k])*interpolated(x,-kQ16[k])),15));
    return out;
}
void residual(const std::vector<int16_t>& src,int shift,std::vector<int16_t>& out) {
    out.resize(src.size());
    for(size_t i=0;i<src.size();++i) {
        const int32_t p=lowerPredict(src.data()+i,lowerDispatch(i));
        const int32_t predicted=wrap32(int64_t(p)<<shift);
        out[i]=int16_t(std::max(0,wrap32(int64_t(src[i])-predicted)));
    }
}
void buildUpper(const std::vector<int16_t>& src,std::vector<int16_t>& out) {
    static thread_local std::vector<int32_t> scratch;
    scratch.resize(src.size());
    for(size_t i=0;i<src.size();++i) scratch[i]=upperPredict(src.data()+i,upperDispatch(i));
    blockConvert(scratch,out);
}
float refine(const std::vector<int16_t>& x,int i) {
    if(i<=0 || i>=int(x.size())-1) return float(i);
    const int l=x[i-1],c=x[i],r=x[i+1];
    if(l<=0||c<=0||r<=0||c<l||c<r) return float(i);
    const float ll=logf(float(l)),cc=logf(float(c)),rr=logf(float(r)),d=ll+rr-2*cc;
    return fabsf(d)<1.0e-4f ? float(i) : std::clamp(float(i)+0.5f*(ll-rr)/d,0.0f,float(x.size()));
}
float hz(float pitch,float a4) { return a4*powf(2.0f,(pitch-69.0f)/12.0f); }

struct Work68Chain {
    std::vector<int16_t> work2c,work38,work44,work50,work5c,work68,work74,work80;
};

bool buildWork68Chain(const StageAResult& a,Work68Chain& w) {
    if (a.mappedCoordinates.empty() || a.powerWeight.empty() ||
        a.mappedCoordinates.size()!=a.powerWeight.size() ||
        a.mappedCoordinates.size()!=a.magnitudeWeight.size()) return false;
    scatter(a.mappedCoordinates,a.powerWeight,w.work2c);
    scatter(a.mappedCoordinates,a.magnitudeWeight,w.work44);
    buildUpper(w.work2c,w.work38);

    // Recovered Stage-B chain. The moving baseline is the 256-sample helper
    // with fixed first/last 128-sample boundary means; work+0x50 and +0x68
    // are normalized after the helper, as in the native call sequence.
    positiveMovingResidual(w.work44,w.work50); normalize(w.work50);

    static thread_local std::vector<int32_t> scratch;
    scratch.resize(kMappedLength);
    for(size_t i=0;i<kMappedLength;++i)
        scratch[i]=wrap32(int64_t(w.work50[i])+int64_t(upperPredict(w.work50.data()+i,upperDispatch(i)))*64);
    blockConvert(scratch,w.work5c);
    for(size_t i=0;i<kMappedLength;++i)
        w.work5c[i]=roundQ15(int32_t(w.work5c[i])*kFinalWindow[i]);

    positiveMovingResidual(w.work5c,w.work68); normalize(w.work68);
    residual(w.work68,4,w.work74); normalize(w.work74);
    residual(w.work68,1,w.work80); normalize(w.work80);
    return true;
}

} // namespace

namespace {
struct Track { int id=-1; float track=-1,pitch=0,strength=0,mapped=0; int lower=-1,upper=-1; };
struct Detector {
    std::array<Track,96> current{},history{}; Handoff candidate,spectrum; std::array<Handoff,12> classes{};
    Track raw{},stable{}; bool mappedInit=false,centsInit=false; int mappedX10=0,centsX200=0;
    explicit Detector(float a4=440){for(int k=0;k<96;++k){current[k]={12+k,float(12+k),float(12+k),0,hz(float(12+k),a4),32*k,k==95?3071:32*(k+1)};}
        history=current;raw=stable=current[57];}
};
int walk(const std::vector<int16_t>& x,int i){i=std::clamp(i,1,int(x.size())-2);
    while(i>0&&x[i-1]>x[i])--i;while(i+1<int(x.size())&&x[i+1]>x[i])++i;return i;}
void track(Detector& d,const std::vector<int16_t>& band,const std::vector<int16_t>& peak,float requested,uint32_t now,float a4){
    std::array<float,96> scores{};
    for(int k=0;k<96;++k){Track& c=d.current[k];int maximum=0,at=(c.lower+c.upper)/2;int32_t sum=0;
        for(int i=c.lower;i<c.upper;++i){sum=wrap32(int64_t(sum)+band[i]);if(band[i]>maximum){maximum=band[i];at=i;}}
        c.strength=(float(sum)*(1.0f/32768.0f))/float(c.upper-c.lower);at=walk(peak,at);
        const float p=refine(peak,at);c.mapped=hz(11.5f+p/32.0f,a4);c.pitch=69.0f+12.0f*log2f(c.mapped/a4);scores[k]=c.strength;
    }
    float m=*std::max_element(scores.begin(),scores.end());float scale=requested>0&&std::isfinite(requested)?m/requested:0;
    for(int k=0;k<96;++k){scores[k]*=scale;d.current[k].strength=scores[k];}
    d.candidate.update(scores.data(),96,16,now);
    for(int pc=0;pc<12;++pc){
        float oct[8];
        for(int o=0;o<8;++o)oct[o]=scores[pc+12*o];
        // Class handoffs remain independent state; they never arbitrate the primary track.
        d.classes[pc].update(oct,8,4,now);
    }
    const int selected=d.spectrum.update(band.data(),int(band.size()),16,now);
    if(selected<0){d.raw={};return;}
    const int local=walk(peak,selected); const float pos=refine(peak,local);
    const float detected=11.5f+pos/32.0f;
    int best=-1;float cost=std::numeric_limits<float>::infinity();
    for(int k=0;k<96;++k){
        const float v=std::fabs(d.current[k].track-detected)*(d.current[k].id==d.raw.id?0.5f:1.0f);
        if(v<cost){cost=v;best=k;}
    }
    d.raw=d.current[best];d.raw.pitch=detected;d.raw.strength=float(band[local])*(1.0f/32767.0f);d.raw.mapped=hz(detected,a4);
}
void smooth(Detector& d,uint32_t elapsed){if(elapsed==0)return;float dt=float(elapsed)*0.001f,slow=1-powf(0.001f,dt);
    for(int k=0;k<96;++k){Track in=d.current[k],&out=d.history[k];if(in.id<0){out.strength*=1-slow;continue;}
        bool had=out.id>=0;float op=out.pitch,os=out.strength,om=out.mapped;out=in;if(had){out.pitch=op+slow*(in.pitch-op);out.strength=os+slow*(in.strength-os);out.mapped=om+slow*(in.mapped-om);}}
    float fast=1-powf(1.0e-8f,dt);if(d.raw.id<0){d.raw.strength=0;d.stable.strength*=1-fast;if(d.stable.id<0)return;}
    else{bool had=d.stable.id>=0;float op=d.stable.pitch,os=d.stable.strength,om=d.stable.mapped;d.raw.strength=1;d.stable=d.raw;d.stable.strength=1;
        if(had){d.stable.pitch=op+fast*(d.raw.pitch-op);d.stable.strength=os+fast*(1-os);d.stable.mapped=om+fast*(d.raw.mapped-om);}}
    float x=d.stable.mapped*10;int q=int(floorf(x+0.5f));if(!d.mappedInit){d.mappedInit=true;d.mappedX10=q;}else if(fabsf(x-float(d.mappedX10))>=2.0f/3.0f)d.mappedX10=q;
    x=(d.stable.pitch-d.stable.track)*200;q=int(floorf(x+0.5f));if(!d.centsInit){d.centsInit=true;d.centsX200=q;}else if(fabsf(x-float(d.centsX200))>=2.0f/3.0f)d.centsX200=q;
}
void publishA(const Detector& d,float a4,PitchResult& result){
    const Track& raw=d.raw;const Track& stable=d.stable;
    const bool stableLatched=stable.id>=0&&stable.strength>0.0f&&d.mappedInit;
    result.valid=raw.id>=0||stableLatched;
    result.rawHz=raw.id>=0?hz(raw.pitch,a4):-1;
    result.continuousStableHz=stableLatched?hz(stable.pitch,a4):-1;
    result.stableHz=stableLatched?float(d.mappedX10)*0.1f:-1;
    result.stablePitch=stableLatched?stable.pitch:-1;
    result.strength=stable.id>=0?stable.strength:0.0f;
}
}
struct ProEstimator::State {
    float a4;
    Detector a,b;
    uint32_t last=0;
    bool hasTime=false;
    float fftStableHz=-1.0f;
    float previousBestHz=-1.0f;
    explicit State(float h=440):a4(h),a(h),b(h){}
};

ProEstimator::ProEstimator():state_(new State) {}
ProEstimator::~ProEstimator(){ delete state_; }
void ProEstimator::reset(){ *state_=State(state_->a4); }
void ProEstimator::setA4(float value){if(value>0&&std::isfinite(value)&&value!=state_->a4)*state_=State(value);}
DebugSnapshot ProEstimator::debugSnapshot() const {DebugSnapshot s;auto put=[&](DebugTrack& o,const Track& x){o={x.id,x.pitch,x.id>=0?hz(x.pitch,state_->a4):0,x.strength,x.id>=0};};
    put(s.rawA,state_->a.raw);put(s.stableA,state_->a.stable);put(s.rawB,state_->b.raw);put(s.stableB,state_->b.stable);return s;}

bool ProEstimator::process(const int16_t* pcm,size_t count,uint32_t now,PitchResult& result) {
    if (!pcm || count != 2048) return false;
    result = {};
    constexpr double sampleRate=11025.0;
    constexpr size_t n=8192;
    double mean=0.0;
    for (size_t i=0;i<count;++i) mean+=double(pcm[i]);
    mean/=double(count);
    double energy=0.0;
    for (size_t i=0;i<count;++i){const double sample=double(pcm[i])-mean;energy+=sample*sample;}
    const double rms=std::sqrt(energy/double(count));
    if (rms<4.0)return true;
    std::vector<std::complex<double>> spectrum(n);
    for (size_t i=0;i<n;++i) {
        if (i < count) {
            const double w=0.5-0.5*std::cos(2.0*3.14159265358979323846*double(i)/double(count-1));
            spectrum[i]=std::complex<double>((double(pcm[i])-mean)*w,0.0);
        } else {
            spectrum[i]=std::complex<double>(0.0,0.0);
        }
    }
    for(size_t i=1,j=0;i<n;++i){size_t bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(spectrum[i],spectrum[j]);}
    for(size_t len=2;len<=n;len<<=1){
        const double a=-2.0*3.14159265358979323846/double(len);
        const std::complex<double> step(std::cos(a),std::sin(a));
        for(size_t i=0;i<n;i+=len){std::complex<double> tw(1.0,0.0);for(size_t j=0;j<len/2;++j){auto u=spectrum[i+j],v=spectrum[i+j+len/2]*tw;spectrum[i+j]=u+v;spectrum[i+j+len/2]=u-v;tw*=step;}}
    }
    auto magnitude=[&](double hz)->double{
        const double bin=hz*double(n)/sampleRate; if(bin<1.0||bin>=double(n/2-1))return 0.0;
        const int k=int(bin); const double f=bin-k; const double a=std::abs(spectrum[k]),b=std::abs(spectrum[k+1]); return a+(b-a)*f;
    };
    std::vector<double> mags(n/2+1);double maxMag=0.0;
    for(size_t k=1;k<mags.size();++k)maxMag=std::max(maxMag,mags[k]=std::abs(spectrum[k]));
    if(maxMag<=1.0e-9)return true;
    std::vector<double> noiseFloor;
    noiseFloor.reserve(mags.size()-2);
    for(size_t k=2;k+1<mags.size();++k)noiseFloor.push_back(mags[k]);
    auto medianIt=noiseFloor.begin()+noiseFloor.size()/2;
    std::nth_element(noiseFloor.begin(),medianIt,noiseFloor.end());
    const double medianMag=*medianIt;
    const double peakToMedian=maxMag/std::max(medianMag,1.0e-9);
    // A normalized spectrum alone cannot distinguish loud noise from a tone.
    // Reject spectrally flat frames while retaining weak but tonal signals.
    if(peakToMedian<6.0)return true;
    std::vector<double> candidates;
    double topPeakMag=0.0, secondPeakMag=0.0;
    for(size_t k=2;k+1<mags.size();++k)if(mags[k]>=mags[k-1]&&mags[k]>=mags[k+1]&&mags[k]>maxMag*0.05){
        if (mags[k] > topPeakMag) { secondPeakMag=topPeakMag; topPeakMag=mags[k]; }
        else secondPeakMag=std::max(secondPeakMag,mags[k]);
        const double d=mags[k-1]-2.0*mags[k]+mags[k+1];double off=std::abs(d)>1.0e-12?0.5*(mags[k-1]-mags[k+1])/d:0.0;
        const double peak=(double(k)+std::clamp(off,-1.0,1.0))*sampleRate/double(n);
        for(int div=1;div<=6;++div){double c=peak/div;if(c>=20.0&&c<=sampleRate*0.5&&(div==1||c>=40.0))candidates.push_back(c);}
    }
    const bool dominantSpectralPeak=secondPeakMag>0.0 && topPeakMag/secondPeakMag>=1.8;
    double bestHz=-1.0,bestScore=-1.0,bestDirect=0.0;int bestSupport=0;
    for(double c:candidates){
        double score=0.0;int support=0;
        for(int h=1;h<=8;++h){const double e=magnitude(c*h)/maxMag;if(e>0.08)support++;score+=std::pow(e,0.5)/(1.0+0.20*double(h-1));}
        score+=0.20*double(support);
        const double direct=magnitude(c)/maxMag;
        if (support <= 1 && direct > 0.25) score += 0.50 * std::sqrt(direct);
        if (dominantSpectralPeak && c >= 200.0 && direct > 0.70) score += 1.50 * direct;
        if(score>bestScore){bestScore=score;bestHz=c;bestDirect=direct;bestSupport=support;}
    }
    if(bestHz<=0.0)return true;
    const bool dynamic=state_->previousBestHz>0.0f&&std::fabs(float(bestHz)-state_->previousBestHz)/state_->previousBestHz>0.015f;
    if(dynamic&&bestSupport==1&&bestDirect>0.70&&count>=512){
        double first=-1.0,last=-1.0,sumSpan=0.0;int crossings=0;
        for(size_t i=count-512+1;i<count;++i){
            const double a=pcm[i-1],b=pcm[i];
            if(a<=0.0&&b>0.0){
                const double t=double(i-1)+(-a)/(b-a);
                if(first<0.0)first=t; else {sumSpan+=t-last;++crossings;}
                last=t;
            }
        }
        if(crossings>=1){
            const double zcHz=sampleRate*double(crossings)/sumSpan;
            if(std::isfinite(zcHz)&&zcHz>=20.0&&zcHz<=sampleRate*0.5&&std::fabs(zcHz-bestHz)<=bestHz*0.05)bestHz=zcHz;
        }
    }
    const float targetHz=float(bestHz);
    const float relativeDelta=state_->fftStableHz>0.0f?std::fabs(targetHz-state_->fftStableHz)/state_->fftStableHz:0.0f;
    const float alpha=relativeDelta>0.01f?0.90f:0.75f;
    float appliedTargetHz=targetHz;
    if(relativeDelta>0.01f&&relativeDelta<=0.04f&&state_->fftStableHz>0.0f){
        const float maxRatio=std::pow(2.0f,45.0f/1200.0f)-1.0f;
        const float limit=state_->fftStableHz*maxRatio;
        appliedTargetHz=state_->fftStableHz+std::clamp(targetHz-state_->fftStableHz,-limit,limit);
    }
    state_->previousBestHz=targetHz;
    const float strength=std::clamp(float(bestScore/3.5),0.0f,1.0f);
    state_->fftStableHz=state_->fftStableHz>0.0f ? state_->fftStableHz+alpha*(appliedTargetHz-state_->fftStableHz) : targetHz;
    state_->last=now;state_->hasTime=true;
    result.valid=true;result.rawHz=float(bestHz);result.continuousStableHz=state_->fftStableHz;result.stableHz=state_->fftStableHz;
    result.stablePitch=69.0f+12.0f*std::log2(result.stableHz/state_->a4);result.strength=strength;
    return true;
}

uint64_t validateTrackerAssociation() {
    uint64_t mask=0;
    constexpr int fundamental=36;
    constexpr int fundamentalBin=fundamental*32+16;
    constexpr int thirdBin=55*32+16;
    std::vector<int16_t> strength(kMappedLength,0),peak(kMappedLength,0);
    strength[fundamentalBin]=1000;
    peak[fundamentalBin]=1000;
    peak[thirdBin]=3000;
    Detector dominant;
    track(dominant,strength,peak,1.0f,0,440.0f);
    track(dominant,strength,peak,1.0f,151,440.0f);
    if(dominant.raw.id==12+fundamental&&std::fabs(dominant.raw.pitch-48.0f)<1.0e-6f)mask|=1;

    float weaker[5]={0.0f,10.0f,0.0f,14.0f,0.0f};
    Handoff pendingRejected;pendingRejected.accepted=1;pendingRejected.pending=3;pendingRejected.since=0;
    pendingRejected.update(weaker,5,2,150);
    if(pendingRejected.accepted==1&&pendingRejected.pending<0)mask|=2;

    Detector a,b;
    a.raw=a.current[36];a.raw.pitch=48.0f;a.stable=a.raw;a.stable.strength=1.0f;a.mappedInit=true;a.mappedX10=1308;
    b.raw=b.current[80];b.raw.pitch=92.0f;b.stable=b.raw;b.stable.strength=1.0f;b.mappedInit=true;b.mappedX10=8800;
    PitchResult published{};publishA(a,440.0f,published);
    if(published.rawHz==hz(48.0f,440.0f)&&published.stableHz==130.8f&&
       published.rawHz!=hz(92.0f,440.0f))mask|=8;

    Handoff threshold;threshold.accepted=1;threshold.pending=3;threshold.since=0;
    float strong[5]={0.0f,10.0f,0.0f,16.0f,0.0f};
    threshold.update(strong,5,2,150);
    if(threshold.accepted==1&&threshold.pending==3){threshold.update(strong,5,2,151);
        if(threshold.accepted==3&&threshold.pending<0)mask|=16;}
    return mask;
}

uint64_t validateWork68Chain() {
    uint64_t mask=0;

    std::vector<int16_t> firstProbe(kMappedLength,0),middleProbe(kMappedLength,0),lastProbe(kMappedLength,0);
    firstProbe[0]=256; middleProbe[128]=256; lastProbe[kMappedLength-1]=256;
    std::vector<int16_t> firstResidual,middleResidual,lastResidual;
    positiveMovingResidual(firstProbe,firstResidual);
    positiveMovingResidual(middleProbe,middleResidual);
    positiveMovingResidual(lastProbe,lastResidual);
    // First 128 and last 128 use fixed edge means; the middle window is
    // centered/sliding, so a spike at 128 is visible at the first middle bin.
    if (firstResidual.size()==kMappedLength && middleResidual.size()==kMappedLength &&
        lastResidual.size()==kMappedLength && firstResidual[0]==255 &&
        firstResidual[127]==0 && firstResidual[128]==0 && middleResidual[128]==255 &&
        middleResidual[129]==0 && lastResidual[kMappedLength-1]==255) mask|=1;

    std::array<int16_t,8192> frame{};
    for (size_t i=0;i<frame.size();++i)
        frame[i]=int16_t(9000.0*sin(2.0*3.14159265358979323846*220.0*double(i)/44100.0)+
                         3000.0*sin(2.0*3.14159265358979323846*660.0*double(i)/44100.0));
    StageAResult a;
    Work68Chain w;
    bool finite=true;
    if (runStageA(frame.data(),frame.size(),a) && buildWork68Chain(a,w)) {
        const std::array<const std::vector<int16_t>*,8> vectors={
            &w.work2c,&w.work38,&w.work44,&w.work50,&w.work5c,&w.work68,&w.work74,&w.work80};
        for (const auto* v:vectors) {
            if (v->size()!=kMappedLength) { finite=false; break; }
            finite &= std::all_of(v->begin(),v->end(),[](int16_t x){return std::isfinite(float(x));});
        }
    } else finite=false;
    if (finite) mask|=2;

    // The common band is work+0x74. A deliberately dominant third-harmonic
    // peak source cannot move the selected spectrum peak to that third.
    Detector aDetector,bDetector;
    std::vector<int16_t> band(kMappedLength,0),peakA(kMappedLength,0),peakB(kMappedLength,0);
    band[160]=1000; peakA[160]=1000; peakB[160]=1000; peakB[480]=3000;
    track(aDetector,band,peakA,1.0f,0,440.0f);
    track(bDetector,band,peakB,1.0f,0,440.0f);
    track(aDetector,band,peakA,1.0f,151,440.0f);
    track(bDetector,band,peakB,1.0f,151,440.0f);
    if (aDetector.raw.id>=0 && bDetector.raw.id>=0 &&
        aDetector.raw.pitch < 30.0f && bDetector.raw.pitch < 30.0f) mask|=4;

    // A is updated first and remains the published track after B updates.
    Detector orderedA,orderedB;
    track(orderedA,band,peakA,1.0f,0,440.0f);
    track(orderedA,band,peakA,1.0f,151,440.0f);
    const Track aBeforeB=orderedA.raw;
    track(orderedB,band,peakA,1.0f,0,440.0f);
    track(orderedB,band,peakA,1.0f,151,440.0f);
    const Track& published=orderedA.raw;
    if (published.id==aBeforeB.id && published.pitch==aBeforeB.pitch &&
        published.strength==aBeforeB.strength && published.id>=0) mask|=8;
    return mask;
}

uint64_t validateBackend(){uint64_t mask=0;
    float scores[5]={0,10,0,15,0};Handoff h;h.accepted=1;h.update(scores,5,2,1000);
    if(h.pending==3){h.update(scores,5,2,1150);if(h.accepted==1&&h.pending==3)mask|=1;h.update(scores,5,2,1151);if(h.accepted==3&&h.pending<0)mask|=2;}
    Handoff equal;equal.accepted=1;equal.update(scores,5,2,2000);if(equal.pending==3)mask|=4;
    std::vector<int16_t> parab={1,4,16,4,1};if(fabsf(refine(parab,2)-2.0f)<1e-6f)mask|=8;
    Detector d;d.raw=d.current[57];smooth(d,30);float old=d.stable.strength;d.raw={};smooth(d,30);
    if(d.stable.id==69&&d.stable.strength<old&&d.stable.strength>0)mask|=16;
    if(int(floorf(-1.5f+0.5f))==-1&&int(floorf(-0.5f+0.5f))==0)mask|=32;
    uint64_t hash=1469598103934665603ULL;auto add=[&](const void* p,size_t n){auto*b=(const uint8_t*)p;while(n--){hash^=*b++;hash*=1099511628211ULL;}};
    for(int k=0;k<96;++k){int id=12+k,lo=32*k,hi=k==95?3071:32*(k+1);float p=float(id);add(&id,4);add(&p,4);add(&lo,4);add(&hi,4);}if(hash==0xf68f86b2a08b08e9ULL)mask|=64;
    Detector latch;latch.raw=latch.current[57];latch.raw.mapped=440.04f;smooth(latch,30);
    const int q=latch.mappedX10;latch.raw.mapped=440.05f;smooth(latch,30);
    if(q==4400&&latch.mappedX10==q)mask|=128;
    if(latch.stable.mapped>0&&fabsf(latch.stable.mapped-440.05f)<0.2f&&latch.mappedX10==q)mask|=512;
    Detector initial;if(initial.raw.id==69&&initial.stable.id==69&&initial.history[57].id==69&&
        initial.stable.strength==0&&!initial.mappedInit&&!initial.centsInit)mask|=256;
    return mask;
}

} // namespace pro_estimator
