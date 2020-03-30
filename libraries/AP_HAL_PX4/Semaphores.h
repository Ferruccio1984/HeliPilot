#pragma once

#include <AP_HAL/AP_HAL_Boards.h>

#if CONFIG_HAL_BOARD == HAL_BOARD_PX4
#include "AP_HAL_PX4.h"
#include <pthread.h>

class PX4::Semaphore : public AP_HAL::Semaphore {
public:
    Semaphore() {
        pthread_mutex_init(&_lock, nullptr);
    }
    bool give();
    bool take(uint32_t timeout_ms);
    bool take_nonblocking();
protected:
    pthread_mutex_t _lock;
};

// a recursive semaphore, allowing for a thread to take it more than
// once. It must be released the same number of times it is taken
class PX4::Semaphore_Recursive : public PX4::Semaphore {
public:
    Semaphore_Recursive();
    bool give() override;
    bool take(uint32_t timeout_ms) override;
    bool take_nonblocking() override;
private:
    uint32_t count;
};
#endif // CONFIG_HAL_BOARD
