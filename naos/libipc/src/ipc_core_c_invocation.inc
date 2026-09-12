extern "C" {
naos_ipc_invocation_t *naos_ipc_invocation_create(const naos_ipc_invocation_config_t *config)
{
    if (!valid_invocation_config(config))
        return nullptr;
    void *storage = config->control_memory->allocate(config->control_memory->context, sizeof(naos_ipc_invocation_t),
                                                     alignof(naos_ipc_invocation_t));
    if (storage == nullptr)
        return nullptr;
    auto *result = ::new (storage) naos_ipc_invocation_t(*config);
    if (result->invocation == nullptr)
    {
        result->~naos_ipc_invocation_t();
        config->control_memory->deallocate(config->control_memory->context, storage, sizeof(naos_ipc_invocation_t),
                                           alignof(naos_ipc_invocation_t));
        return nullptr;
    }
    return result;
}

void naos_ipc_invocation_destroy(naos_ipc_invocation_t *invocation)
{
    if (invocation == nullptr)
        return;
    auto &memory = invocation->control_memory;
    if (invocation->invocation != nullptr)
        invocation->invocation->destroy();
    invocation->invocation = nullptr;
    invocation->~naos_ipc_invocation_t();
    memory.deallocate(invocation, sizeof(naos_ipc_invocation_t), alignof(naos_ipc_invocation_t));
}

int naos_ipc_invocation_valid(const naos_ipc_invocation_t *invocation)
{
    return invocation != nullptr && invocation->invocation != nullptr && invocation->invocation->valid();
}

uint64_t naos_ipc_invocation_method_id(const naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) ? invocation->invocation->method_id() : 0;
}

uint64_t naos_ipc_invocation_operation_deadline(const naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) ? invocation->invocation->operation_deadline() : 0;
}

naos_ipc_signal_t naos_ipc_invocation_signals(const naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) ? invocation->invocation->signals() : 0;
}

int naos_ipc_invocation_begin_receive(naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) && invocation->invocation->begin_receive();
}

void naos_ipc_invocation_rollback_receive(naos_ipc_invocation_t *invocation)
{
    if (naos_ipc_invocation_valid(invocation))
        invocation->invocation->rollback_receive();
}

int naos_ipc_invocation_finish_dispatch(naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) && invocation->invocation->finish_dispatch();
}

void naos_ipc_invocation_mark_dispatched(naos_ipc_invocation_t *invocation)
{
    if (naos_ipc_invocation_valid(invocation))
        invocation->invocation->mark_dispatched();
}

int naos_ipc_invocation_cancellation_requested(const naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) && invocation->invocation->cancellation_requested();
}

int naos_ipc_invocation_execution_interrupted(const naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) && invocation->invocation->execution_interrupted();
}

int naos_ipc_invocation_cancel(naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) && invocation->invocation->cancel();
}

int naos_ipc_invocation_expire_if_due(naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) && invocation->invocation->expire_if_due();
}

void naos_ipc_invocation_close_client(naos_ipc_invocation_t *invocation)
{
    if (naos_ipc_invocation_valid(invocation))
        invocation->invocation->close_client();
}

void naos_ipc_invocation_abandon_responder(naos_ipc_invocation_t *invocation)
{
    if (naos_ipc_invocation_valid(invocation))
        invocation->invocation->abandon_responder();
}

int naos_ipc_invocation_consume_responder(naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) && invocation->invocation->consume_responder();
}

int naos_ipc_invocation_reserve_result_budget(naos_ipc_invocation_t *invocation)
{
    return naos_ipc_invocation_valid(invocation) && invocation->invocation->reserve_result_budget();
}

int naos_ipc_invocation_response_within_limits(const naos_ipc_invocation_t *invocation, uint64_t bytes,
                                               uint64_t resources)
{
    return naos_ipc_invocation_valid(invocation) && invocation->invocation->response_within_limits(bytes, resources);
}

int naos_ipc_invocation_complete_reply(naos_ipc_invocation_t *invocation, const uint8_t *bytes, uint64_t byte_count,
                                       naos_ipc_resource_t *resources, size_t resource_count, int64_t protocol_error)
{
    if (!naos_ipc_invocation_valid(invocation) || (byte_count != 0 && bytes == nullptr) ||
        (resource_count != 0 && resources == nullptr))
        return 0;
    core_resource *converted = nullptr;
    if (!c_resources_to_core(invocation->control_memory, resources, resource_count, converted, false))
        return 0;
    const bool completed =
        invocation->invocation->complete_reply(bytes, byte_count, converted, resource_count, protocol_error);
    if (!completed)
        restore_core_resources(invocation->control_memory, converted, resources, resource_count);
    return completed;
}

int naos_ipc_invocation_failure_valid(uint32_t execution_outcome, uint32_t outcome_reason, int64_t protocol_error)
{
    return core_invocation::valid_failure(static_cast<na_execution_outcome_t>(execution_outcome),
                                          static_cast<na_outcome_reason_t>(outcome_reason), protocol_error);
}

int naos_ipc_invocation_complete_failure(naos_ipc_invocation_t *invocation, uint32_t execution_outcome,
                                         uint32_t outcome_reason, int64_t protocol_error)
{
    if (!naos_ipc_invocation_valid(invocation))
        return 0;
    return invocation->invocation->complete_failure(static_cast<na_execution_outcome_t>(execution_outcome),
                                                    static_cast<na_outcome_reason_t>(outcome_reason), protocol_error);
}

int naos_ipc_invocation_complete_not_delivered(naos_ipc_invocation_t *invocation, uint32_t outcome_reason)
{
    if (!naos_ipc_invocation_valid(invocation))
        return 0;
    return invocation->invocation->complete_not_delivered(static_cast<na_outcome_reason_t>(outcome_reason));
}

naos_ipc_status_t naos_ipc_invocation_claim_result(naos_ipc_invocation_t *invocation, uint64_t byte_capacity,
                                                   uint64_t resource_capacity, naos_ipc_invocation_result_info_t *info)
{
    if (!naos_ipc_invocation_valid(invocation) || info == nullptr)
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    na_execution_outcome_t outcome = NA_EXECUTION_NONE;
    na_outcome_reason_t reason = NA_OUTCOME_REASON_NONE;
    const auto status = invocation->invocation->claim_result(
        byte_capacity, resource_capacity, info->actual_bytes, info->actual_resources, info->required_bytes,
        info->required_resources, outcome, reason, info->protocol_error);
    info->method_id = invocation->invocation->method_id();
    info->execution_outcome = outcome;
    info->outcome_reason = reason;
    return static_cast<naos_ipc_status_t>(status);
}

naos_ipc_status_t naos_ipc_invocation_take_result_bytes(naos_ipc_invocation_t *invocation, uint8_t **bytes,
                                                        uint64_t *byte_count)
{
    if (!naos_ipc_invocation_valid(invocation) || bytes == nullptr || byte_count == nullptr)
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    std::uint8_t *value = nullptr;
    std::uint64_t count = 0;
    const auto status = invocation->invocation->take_result_bytes(value, count);
    *bytes = value;
    *byte_count = count;
    return static_cast<naos_ipc_status_t>(status);
}

naos_ipc_status_t naos_ipc_invocation_take_result_resource(naos_ipc_invocation_t *invocation, uint64_t index,
                                                           naos_ipc_resource_t *resource)
{
    if (!naos_ipc_invocation_valid(invocation) || resource == nullptr || naos_ipc_resource_valid(resource))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    core_resource value;
    const auto status = invocation->invocation->take_result_resource(index, value);
    if (status != NA_STATUS_OK)
        return static_cast<naos_ipc_status_t>(status);
    copy_resource(resource, value);
    value.take_value();
    return NAOS_IPC_STATUS_OK;
}

naos_ipc_status_t naos_ipc_invocation_restore_result(naos_ipc_invocation_t *invocation, uint8_t *bytes,
                                                     uint64_t byte_count, naos_ipc_resource_t *resources,
                                                     size_t resource_count)
{
    if (!naos_ipc_invocation_valid(invocation) || (resource_count != 0 && resources == nullptr))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    core_resource *converted = nullptr;
    if (!c_resources_to_core(invocation->control_memory, resources, resource_count, converted, true))
        return NAOS_IPC_STATUS_RESOURCE_EXHAUSTED;
    const auto status = invocation->invocation->restore_result(bytes, byte_count, converted, resource_count);
    if (status != NA_STATUS_OK)
        restore_core_resources(invocation->control_memory, converted, resources, resource_count);
    else if (converted != nullptr)
    {
        for (size_t index = 0; index < resource_count; index++)
            converted[index].~resource();
        invocation->control_memory.deallocate(converted, sizeof(core_resource) * resource_count,
                                              alignof(core_resource));
    }
    return static_cast<naos_ipc_status_t>(status);
}

naos_ipc_status_t naos_ipc_invocation_commit_result(naos_ipc_invocation_t *invocation)
{
    if (!naos_ipc_invocation_valid(invocation))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    return static_cast<naos_ipc_status_t>(invocation->invocation->commit_result());
}

naos_ipc_status_t naos_ipc_validate_wait_set(const naos_ipc_handle_table_t *handles, const uint64_t *values,
                                             size_t count)
{
    if (handles == nullptr || handles->validate_waitable == nullptr || (count != 0 && values == nullptr))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    for (size_t index = 0; index < count; index++)
    {
        const auto status = handles->validate_waitable(handles->context, values[index]);
        if (status != NAOS_IPC_STATUS_OK)
            return status;
    }
    return NAOS_IPC_STATUS_OK;
}

uint64_t naos_ipc_clock_now(const naos_ipc_clock_t *clock)
{
    return clock == nullptr || clock->now == nullptr ? 0 : clock->now(clock->context);
}
}

