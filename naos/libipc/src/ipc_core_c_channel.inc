{
    if (!valid_domain_config(config))
        return nullptr;
    void *storage =
        config->memory->allocate(config->memory->context, sizeof(naos_ipc_domain_t), alignof(naos_ipc_domain_t));
    if (storage == nullptr)
        return nullptr;
    auto *result = ::new (storage) naos_ipc_domain_t(*config);
    if (result->domain == nullptr)
    {
        result->~naos_ipc_domain_t();
        config->memory->deallocate(config->memory->context, storage, sizeof(naos_ipc_domain_t),
                                   alignof(naos_ipc_domain_t));
        return nullptr;
    }
    return result;
}

void naos_ipc_domain_destroy(naos_ipc_domain_t *domain)
{
    if (domain == nullptr)
        return;
    auto &memory = domain->memory;
    domain->domain->destroy();
    domain->~naos_ipc_domain_t();
    memory.deallocate(domain, sizeof(naos_ipc_domain_t), alignof(naos_ipc_domain_t));
}

naos_ipc_channel_t *naos_ipc_channel_create(const naos_ipc_channel_config_t *config)
{
    if (!valid_channel_config(config))
        return nullptr;
    void *storage = config->control_memory->allocate(config->control_memory->context, sizeof(naos_ipc_channel_t),
                                                     alignof(naos_ipc_channel_t));
    if (storage == nullptr)
        return nullptr;
    auto *result = ::new (storage) naos_ipc_channel_t(*config);
    if (result->channel == nullptr)
    {
        result->~naos_ipc_channel_t();
        config->control_memory->deallocate(config->control_memory->context, storage, sizeof(naos_ipc_channel_t),
                                           alignof(naos_ipc_channel_t));
        return nullptr;
    }
    return result;
}

void naos_ipc_channel_destroy(naos_ipc_channel_t *channel)
{
    if (channel == nullptr || channel->channel == nullptr)
        return;
    /* Drain first so the C wrapper allocated for each queued message is freed. */
    for (uint8_t side = 0; side < 2; side++)
    {
        for (;;)
        {
            core_message *message = nullptr;
            if (!channel->channel->discard(side, message))
                break;
            auto *wrapper = message == nullptr ? nullptr : static_cast<naos_ipc_message_t *>(message->user_context());
            if (wrapper != nullptr)
            {
                wrapper->message = message;
                naos_ipc_message_destroy(wrapper);
            }
            else
            {
                destroy_message_without_wrapper(message);
            }
        }
    }
    channel->channel->destroy();
    channel->channel = nullptr;
    auto &memory = channel->control_memory;
    channel->~naos_ipc_channel_t();
    memory.deallocate(channel, sizeof(naos_ipc_channel_t), alignof(naos_ipc_channel_t));
}

int naos_ipc_channel_valid(const naos_ipc_channel_t *channel)
{
    return channel != nullptr && channel->channel != nullptr;
}

uint64_t naos_ipc_channel_max_messages(const naos_ipc_channel_t *channel)
{
    return naos_ipc_channel_valid(channel) ? channel->channel->max_messages() : 0;
}

uint64_t naos_ipc_channel_queued_messages(const naos_ipc_channel_t *channel, uint8_t side)
{
    return naos_ipc_channel_valid(channel) ? channel->channel->queued_messages(side) : 0;
}

naos_ipc_signal_t naos_ipc_channel_signals(const naos_ipc_channel_t *channel, uint8_t side)
{
    return naos_ipc_channel_valid(channel) ? channel->channel->signals(side) : 0;
}

int naos_ipc_channel_can_reap(const naos_ipc_channel_t *channel)
{
    return naos_ipc_channel_valid(channel) && channel->channel->can_reap();
}

void naos_ipc_channel_side_reference_acquired(naos_ipc_channel_t *channel, uint8_t side)
{
    if (naos_ipc_channel_valid(channel))
        channel->channel->side_reference_acquired(side);
}

void naos_ipc_channel_side_reference_released(naos_ipc_channel_t *channel, uint8_t side)
{
    if (naos_ipc_channel_valid(channel))
        channel->channel->side_reference_released(side);
}

void naos_ipc_channel_begin_operation(naos_ipc_channel_t *channel)
{
    if (naos_ipc_channel_valid(channel))
        channel->channel->begin_operation();
}

void naos_ipc_channel_end_operation(naos_ipc_channel_t *channel)
{
    if (naos_ipc_channel_valid(channel))
        channel->channel->end_operation();
}

naos_ipc_message_t *naos_ipc_message_create(naos_ipc_channel_t *channel, uint64_t byte_count,
                                            uint64_t resource_capacity)
{
    if (!naos_ipc_channel_valid(channel))
        return nullptr;
    void *storage = channel->control_memory.allocate(sizeof(naos_ipc_message_t), alignof(naos_ipc_message_t));
    if (storage == nullptr)
        return nullptr;
    auto *result = ::new (storage) naos_ipc_message_t(channel);
    result->message = core_message::create(*channel->channel, byte_count, resource_capacity);
    if (result->message == nullptr)
    {
        result->~naos_ipc_message_t();
        channel->control_memory.deallocate(storage, sizeof(naos_ipc_message_t), alignof(naos_ipc_message_t));
        return nullptr;
    }
    result->message->set_user_context(result);
    return result;
}

void naos_ipc_message_destroy(naos_ipc_message_t *message)
{
    if (message == nullptr)
        return;
    auto *owner = message->owner;
    auto &memory = owner->control_memory;
    if (message->message != nullptr)
        message->message->destroy();
    message->message = nullptr;
    message->~naos_ipc_message_t();
    memory.deallocate(message, sizeof(naos_ipc_message_t), alignof(naos_ipc_message_t));
}

int naos_ipc_message_valid(const naos_ipc_message_t *message)
{
    return message != nullptr && message->message != nullptr && message->message->valid();
}

uint8_t *naos_ipc_message_bytes(naos_ipc_message_t *message)
{
    return naos_ipc_message_valid(message) ? message->message->bytes() : nullptr;
}

const uint8_t *naos_ipc_message_const_bytes(const naos_ipc_message_t *message)
{
    return naos_ipc_message_valid(message) ? message->message->bytes() : nullptr;
}

uint64_t naos_ipc_message_byte_count(const naos_ipc_message_t *message)
{
    return naos_ipc_message_valid(message) ? message->message->byte_count() : 0;
}

uint64_t naos_ipc_message_resource_count(const naos_ipc_message_t *message)
{
    return naos_ipc_message_valid(message) ? message->message->resource_count() : 0;
}

uint64_t naos_ipc_message_resource_capacity(const naos_ipc_message_t *message)
{
    return naos_ipc_message_valid(message) ? message->message->resource_capacity() : 0;
}

void naos_ipc_message_set_user_context(naos_ipc_message_t *message, void *context)
{
    if (message != nullptr)
        message->user_context = context;
}

void *naos_ipc_message_user_context(const naos_ipc_message_t *message)
{
    return message == nullptr ? nullptr : message->user_context;
}

void naos_ipc_message_visit_resources(const naos_ipc_message_t *message, naos_ipc_resource_visitor_fn visitor,
                                      void *context)
{
    if (!naos_ipc_message_valid(message) || visitor == nullptr)
        return;
    for (uint64_t index = 0; index < message->message->resource_count(); index++)
    {
        const auto *resource = message->message->resource_at(index);
        if (resource != nullptr)
            visitor(context, index, resource->context(), resource->value());
    }
}

naos_ipc_status_t naos_ipc_message_take_resource(naos_ipc_message_t *message, uint64_t index,
                                                 naos_ipc_resource_t *resource)
{
    if (!naos_ipc_message_valid(message) || resource == nullptr || naos_ipc_resource_valid(resource))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    core_resource value;
    const auto status = message->message->take_resource(index, value);
    if (status != NA_STATUS_OK)
        return status;
    copy_resource(resource, value);
    value.take_value();
    return NAOS_IPC_STATUS_OK;
}

naos_ipc_status_t naos_ipc_message_restore_resource(naos_ipc_message_t *message, uint64_t index,
                                                    naos_ipc_resource_t *resource)
{
    if (!naos_ipc_message_valid(message) || resource == nullptr || !naos_ipc_resource_valid(resource))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    core_resource value(resource->context, resource->value, resource->release);
    clear_resource(resource);
    const auto status = message->message->restore_resource(index, std::move(value));
    if (status != NA_STATUS_OK && value.valid())
    {
        copy_resource(resource, value);
        value.take_value();
    }
    return status;
}

void naos_ipc_resource_reset(naos_ipc_resource_t *resource)
{
    if (resource == nullptr)
        return;
    if (resource->value != nullptr && resource->release != nullptr)
        resource->release(resource->context, resource->value);
    clear_resource(resource);
}

int naos_ipc_resource_valid(const naos_ipc_resource_t *resource)
{
    return resource != nullptr && resource->value != nullptr;
}

naos_ipc_status_t naos_ipc_channel_enqueue(naos_ipc_channel_t *channel, uint8_t sender, naos_ipc_message_t *message,
                                           naos_ipc_resource_t *resources, size_t resource_count,
                                           uint64_t queue_message_limit)
{
    if (!naos_ipc_channel_valid(channel) || !naos_ipc_message_valid(message) ||
        (resource_count != 0 && resources == nullptr))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    if (resource_count > naos_ipc_message_resource_capacity(message))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    for (size_t index = 0; index < resource_count; index++)
    {
        if (!naos_ipc_resource_valid(&resources[index]) || resources[index].release == nullptr)
            return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    }

    auto *converted = static_cast<core_resource *>(
        resource_count == 0
            ? nullptr
            : channel->control_memory.allocate(sizeof(core_resource) * resource_count, alignof(core_resource)));
    if (resource_count != 0 && converted == nullptr)
        return NAOS_IPC_STATUS_RESOURCE_EXHAUSTED;
    for (size_t index = 0; index < resource_count; index++)
    {
        ::new (converted + index)
            core_resource(resources[index].context, resources[index].value, resources[index].release);
        clear_resource(&resources[index]);
    }

    const auto status =
        channel->channel->enqueue(sender, *message->message, converted, resource_count, queue_message_limit);
    for (size_t index = 0; index < resource_count; index++)
    {
        if (converted[index].valid())
        {
            copy_resource(&resources[index], converted[index]);
            converted[index].take_value();
        }
        converted[index].~resource();
    }
    if (resource_count != 0)
        channel->control_memory.deallocate(converted, sizeof(core_resource) * resource_count, alignof(core_resource));
    return status;
}

naos_ipc_status_t naos_ipc_channel_claim_receive(naos_ipc_channel_t *channel, uint8_t side,
                                                 naos_ipc_message_t **message)
{
    if (message == nullptr)
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    *message = nullptr;
    if (!naos_ipc_channel_valid(channel))
        return NAOS_IPC_STATUS_INVALID_ARGUMENT;
    core_message *value = nullptr;
    const auto status = channel->channel->claim_receive(side, value);
    if (status == NA_STATUS_OK)
        *message = static_cast<naos_ipc_message_t *>(value->user_context());
    return status;
}

int naos_ipc_channel_cancel_receive(naos_ipc_channel_t *channel, uint8_t side, naos_ipc_message_t *message)
{
    return naos_ipc_channel_valid(channel) && naos_ipc_message_valid(message) &&
           channel->channel->cancel_receive(side, *message->message);
}

int naos_ipc_channel_commit_receive(naos_ipc_channel_t *channel, uint8_t side, naos_ipc_message_t *message)
{
    return naos_ipc_channel_valid(channel) && naos_ipc_message_valid(message) &&
           channel->channel->commit_receive(side, *message->message);
}

int naos_ipc_channel_discard(naos_ipc_channel_t *channel, uint8_t side, naos_ipc_message_t **message)
{
    if (message == nullptr)
        return 0;
    *message = nullptr;
    if (!naos_ipc_channel_valid(channel))
        return 0;
    core_message *value = nullptr;
    if (!channel->channel->discard(side, value))
        return 0;
    *message = static_cast<naos_ipc_message_t *>(value->user_context());
    return *message != nullptr;
}

void naos_ipc_channel_visit_queued_messages(const naos_ipc_channel_t *channel, naos_ipc_message_visitor_fn visitor,
                                            void *context)
{
    if (!naos_ipc_channel_valid(channel) || visitor == nullptr)
        return;
    queued_message_visitor_context visitor_context{visitor, context};
    channel->channel->visit_queued_messages(visit_queued_message, &visitor_context);
}


}

