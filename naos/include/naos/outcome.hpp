#pragma once

#include <errno.h>

#include <naos/abi.h>

namespace naos
{
inline int result_errno(const na_result_frame_t &result)
{
    if (result.protocol_error != 0)
        return result.protocol_error < 0 ? static_cast<int>(-result.protocol_error) : EIO;
    if (result.execution_outcome == NA_EXECUTION_NONE)
        return 0;
    switch (result.outcome_reason)
    {
        case NA_OUTCOME_REASON_CANCEL_REQUESTED:
            return ECANCELED;
        case NA_OUTCOME_REASON_OPERATION_DEADLINE:
            return ETIMEDOUT;
        case NA_OUTCOME_REASON_PEER_CLOSED:
        case NA_OUTCOME_REASON_RESPONDER_ABANDONED:
            return EPIPE;
        case NA_OUTCOME_REASON_REQUEST_DISCARDED:
            return EAGAIN;
        case NA_OUTCOME_REASON_PROTOCOL_VIOLATION:
            return EPROTO;
        case NA_OUTCOME_REASON_UNSUPPORTED:
            return ENOTSUP;
        case NA_OUTCOME_REASON_OBJECT_REVOKED:
        case NA_OUTCOME_REASON_BROKER_FAILURE:
        default:
            return EIO;
    }
}
} // namespace naos
