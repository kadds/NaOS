#include <naos/generated/system/Directory_server.hpp>

namespace
{
struct probe
{
    na_handle_info_t info{};
    na_handle_t closed[4]{};
    std::uint32_t close_count = 0;
    na_submit_frame_t submitted{};
};

na_status_t handle_close(void *context, na_handle_t handle)
{
    auto &state = *static_cast<probe *>(context);
    if (state.close_count < 4)
        state.closed[state.close_count++] = handle;
    return NA_STATUS_OK;
}

na_status_t handle_get_info(void *context, na_handle_t, na_handle_info_t *info)
{
    *info = static_cast<probe *>(context)->info;
    return NA_STATUS_OK;
}

na_status_t invoke_submit(void *context, na_handle_t, const na_submit_frame_t *frame, na_handle_t *invocation)
{
    auto &state = *static_cast<probe *>(context);
    state.submitted = *frame;
    *invocation = 123;
    return NA_STATUS_OK;
}

na_status_t channel_receive(void *, na_handle_t, na_channel_receive_frame_t *frame)
{
    if (frame->struct_size != sizeof(*frame) || frame->flags != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    frame->method_id = 7;
    frame->responder = 123;
    return NA_STATUS_OK;
}
} // namespace

int main()
{
    probe state{};
    state.info.struct_size = sizeof(state.info);
    state.info.binding = NA_BINDING_KERNEL_VIEW;
    state.info.scope = NA_SCOPE_DIRECTORY;
    state.info.meta_rights = NA_RIGHT_TRANSFER;

    naoidl::native_transport_api api{};
    api.context = &state;
    api.handle_close = handle_close;
    api.handle_get_info = handle_get_info;
    api.invoke_submit = invoke_submit;
    api.channel_receive = channel_receive;
    naoidl::native_transport native(api);

    auto client = native.async();
    na_handle_t invocation = NA_HANDLE_INVALID;
    if (client.submit(client.context, 11, 7, nullptr, 0, nullptr, 0, 99, &invocation) != NA_STATUS_OK ||
        invocation != 123 || state.submitted.method_id != 7 || state.submitted.operation_budget != 99)
        return 1;

    na_channel_receive_frame_t receive_frame{};
    if (native.receive(11, receive_frame) != NA_STATUS_OK || receive_frame.method_id != 7 ||
        receive_frame.responder != 123)
        return 2;

    auto server = native.responder();
    if (!server.validate_resource(server.context, 42, NA_BINDING_NONE, NA_SCOPE_DIRECTORY, NA_RIGHT_TRANSFER))
        return 3;
    server.close_resource(server.context, 42);
    if (state.close_count != 1 || state.closed[0] != 42)
        return 4;

    naos::system::Directory::open_response response{};
    const na_handle_t handles[] = {42};
    if (!naos::system::Directory::validate_open_response_resource_metadata(
            response, handles, 1, server.validate_resource, server.close_resource, server.context))
        return 5;
    response.object.value = 1;
    if (naos::system::Directory::validate_open_response_resource_metadata(
            response, handles, 1, server.validate_resource, server.close_resource, server.context))
        return 6;
    if (state.close_count != 2 || state.closed[1] != 42)
        return 7;
    return 0;
}
