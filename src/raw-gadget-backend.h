#ifndef RAW_GADGET_BACKEND_H
#define RAW_GADGET_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct RawGadgetDevice {
    int fd;
    int ep_in;
    int ep_out;
    int ep_in_addr;
    int ep_out_addr;
    bool device_open;
    int target_type;
    bool eps_enabled;

    pthread_t ep_out_thread;
    bool ep_out_thread_spawned;
    pthread_t ep0_thread;
    bool ep0_thread_spawned;
    bool configured;
};

bool raw_gadget_init(struct RawGadgetDevice *dev, int target_type);
void raw_gadget_close(struct RawGadgetDevice *dev);
bool raw_gadget_send_input_report(struct RawGadgetDevice *dev, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // RAW_GADGET_BACKEND_H
