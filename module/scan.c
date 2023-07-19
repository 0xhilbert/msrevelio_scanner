#include <linux/atomic.h>
#include <linux/version.h>
#include <linux/smp.h>
#include <linux/sched.h>

#include "scan.h"
#include "msr_file.h"
#include "util.h"

#define QUEUE_COUNT (scan_range / SCAN_RANGE_SIZE + 1)
#define SCAN_BUFFER_SIZE 64

bool scanning = false;
bool scan_error = false;
atomic_t cur_range_id = ATOMIC_INIT(0);
msr_address_t scan_range;
DEFINE_MSR_FILE(result_file);

void setup_scan(msr_address_t range) {
    scan_error = false;
    scanning = true;
    atomic_set(&cur_range_id, 0);
    scan_range = range;
    msr_file_reset(&result_file);
}

static bool get_range(int rid, struct msr_range *range) {
    if (rid >= QUEUE_COUNT) {
        return false;
    }

    range->start_ = rid * SCAN_RANGE_SIZE;
    range->end_ = range->start_ + SCAN_RANGE_SIZE - 1;

    if (rid == (QUEUE_COUNT - 1)) {
        range->end_ = scan_range;
    }

    return true;
}

bool scan_msr_range(struct msr_range *range) {
    struct typed_msr msr = {0};
    bool run = true;
    uint64_t value;
    struct typed_msr buffer[SCAN_BUFFER_SIZE] = {0};
    int last_idx = -1; //-1 means no value

    msr.address_ = range->start_;
    while (run && !scan_error) {
        if (msr.address_ == range->end_) {
            run = false;
        }
        if (!rdmsrl_safe(msr.address_, &value)) {
            msr.type_ = MSR_TYPE_READABLE;
        }
        if (msr.address_ != 0x83f && msr.address_ != 0x79 && !wrmsrl_safe(msr.address_, msr.type_ ? value : 0)) {
            msr.type_ |= MSR_TYPE_WRITEABLE;
        } else if (msr.address_ == 0x83f || msr.address_ == 0x79) { // TODO dynamic blacklist via msrs_detect
            msr.type_ |= MSR_TYPE_WRITEABLE;
        }
        if (msr.type_) {
            if (last_idx < (SCAN_BUFFER_SIZE - 2)) {
                ++last_idx;
                buffer[last_idx] = msr;
            } else {
                buffer[last_idx + 1] = msr;
                if (!msr_file_add_msrs(&result_file, buffer, SCAN_BUFFER_SIZE)) {
                    scan_error = true;
                    return false;
                }
                last_idx = -1;
            }
        }
        msr.address_++;
        msr.type_ = 0;
    }

    if (last_idx > -1) {
        if (!msr_file_add_msrs(&result_file, buffer, last_idx + 1)) {
            scan_error = true;
            return false;
        }
    }

    return true;
}

void scan_msrs(struct work_struct *work) {
    struct msr_range range;
    int rid;

    while (true) {
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 7, 10)
        rid = atomic_fetch_inc(&cur_range_id);
#else
        rid = atomic_inc_return(&cur_range_id);
        rid--;
#endif
        if (!get_range(rid, &range)) {
            break;
        }
        cond_resched(); // give the system a chance to breathe
        scan_msr_range(&range);
    }
}

