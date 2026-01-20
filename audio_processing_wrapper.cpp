#include "audio_processing_wrapper.h"

#include <memory>
#include <vector>
#include <cstring>

#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>
#include <webrtc/common.h>

struct AecHandle {
    std::unique_ptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig stream_config;
    int sample_rate;
};

AecHandle* aec_create(int sample_rate) {
    if (sample_rate <= 0) return nullptr;

    auto apm = std::unique_ptr<webrtc::AudioProcessing>(webrtc::AudioProcessing::Create());
    if (!apm) return nullptr;

    // Configure modules (minimal processing to reduce artifacts)
    apm->echo_cancellation()->Enable(true);
    apm->echo_cancellation()->set_suppression_level(
        webrtc::EchoCancellation::kLowSuppression);
    apm->echo_cancellation()->enable_drift_compensation(true);

    // Disable NS/AGC/HPF to avoid muffled or phantom noise
    apm->noise_suppression()->Enable(false);
    apm->gain_control()->Enable(false);
    apm->high_pass_filter()->Enable(false);

    webrtc::ProcessingConfig proc_config;
    proc_config.input_stream() = webrtc::StreamConfig(sample_rate, 1);
    proc_config.output_stream() = webrtc::StreamConfig(sample_rate, 1);
    proc_config.reverse_input_stream() = webrtc::StreamConfig(sample_rate, 1);
    proc_config.reverse_output_stream() = webrtc::StreamConfig(sample_rate, 1);
    apm->Initialize(proc_config);

    webrtc::Config extra;
    extra.Set(new webrtc::ExtendedFilter(true));
    extra.Set(new webrtc::DelayAgnostic(true));
    apm->SetExtraOptions(extra);

    auto *handle = new AecHandle{
        std::move(apm),
        webrtc::StreamConfig(sample_rate, 1),
        sample_rate
    };

    return handle;
}

void aec_destroy(AecHandle* handle) {
    delete handle;
}

int aec_process(AecHandle* handle, int16_t* near_end, const int16_t* far_end, int samples) {
    if (!handle || !near_end || !far_end || samples <= 0) return -1;

    const int expected = handle->sample_rate / 100;
    if (samples != expected) return -1;

    webrtc::AudioFrame render_frame;
    render_frame.UpdateFrame(0, 0, far_end,
                             samples, handle->sample_rate,
                             webrtc::AudioFrame::kNormalSpeech,
                             webrtc::AudioFrame::kVadUnknown, 1);
    if (handle->apm->ProcessReverseStream(&render_frame) != 0) {
        return -1;
    }

    webrtc::AudioFrame capture_frame;
    capture_frame.UpdateFrame(0, 0, near_end,
                              samples, handle->sample_rate,
                              webrtc::AudioFrame::kNormalSpeech,
                              webrtc::AudioFrame::kVadUnknown, 1);
    if (handle->apm->ProcessStream(&capture_frame) != 0) {
        return -1;
    }

    std::memcpy(near_end, capture_frame.data_, samples * sizeof(int16_t));

    return 0;
}
