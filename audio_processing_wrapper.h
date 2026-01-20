#ifndef AUDIO_PROCESSING_WRAPPER_H
#define AUDIO_PROCESSING_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct AecHandle AecHandle;

// Create AEC instance
// sample_rate: e.g. 8000 or 16000
AecHandle* aec_create(int sample_rate);

// Destroy AEC instance
void aec_destroy(AecHandle* handle);

// Process audio frames
// near_end: mic input (modified in-place)
// far_end: speaker output (reference signal)
// samples: number of samples per channel (e.g. 80 for 10ms at 8kHz)
// Returns 0 on success, -1 on error
int aec_process(AecHandle* handle, int16_t* near_end, const int16_t* far_end, int samples);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_PROCESSING_WRAPPER_H
