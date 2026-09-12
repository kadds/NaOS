use naos_sys::{
    BootstrapFrame, ChannelOptions, FailFrame, Handle, HandleInfo, HandleRestriction,
    MemoryMapFrame, MemoryUnmapFrame, ProtocolDescriptor, ProtocolEndpointOptions, ReplyFrame,
    ResourceDisposition, ResultFrame, Status, SubmitFrame,
};
use std::mem::{align_of, size_of};

#[test]
fn public_abi_layout_matches_naos_headers() {
    assert_eq!(size_of::<Status>(), 4);
    assert_eq!(size_of::<Handle>(), 8);
    assert_eq!(size_of::<BootstrapFrame>(), 64);
    assert_eq!(align_of::<BootstrapFrame>(), 8);
    assert_eq!(size_of::<ChannelOptions>(), 40);
    assert_eq!(size_of::<HandleInfo>(), 112);
    assert_eq!(size_of::<HandleRestriction>(), 64);
    assert_eq!(size_of::<ResourceDisposition>(), 32);
    assert_eq!(size_of::<ProtocolDescriptor>(), 2216);
    assert_eq!(size_of::<ProtocolEndpointOptions>(), 72);
    assert_eq!(size_of::<SubmitFrame>(), 72);
    assert_eq!(size_of::<ResultFrame>(), 96);
    assert_eq!(size_of::<ReplyFrame>(), 56);
    assert_eq!(size_of::<FailFrame>(), 24);
    assert_eq!(size_of::<MemoryMapFrame>(), 72);
    assert_eq!(size_of::<MemoryUnmapFrame>(), 40);
    assert_eq!(size_of::<naos_sys::EpollEvent>(), 16);
}

#[test]
fn public_status_values_match_naos_abi() {
    assert_eq!(naos_sys::STATUS_OK, 0);
    assert_eq!(naos_sys::STATUS_WRONG_BINDING, 2);
    assert_eq!(naos_sys::STATUS_WRONG_SCOPE, 3);
    assert_eq!(naos_sys::STATUS_ACCESS_DENIED, 4);
    assert_eq!(naos_sys::STATUS_BUFFER_TOO_SMALL, 7);
    assert_eq!(naos_sys::STATUS_WOULD_BLOCK, 8);
    assert_eq!(naos_sys::STATUS_WAIT_TIMED_OUT, 9);
    assert_eq!(naos_sys::STATUS_OBJECT_REVOKED, 12);
    assert_eq!(naos_sys::STATUS_PEER_CLOSED, 13);
    assert_eq!(naos_sys::STATUS_ALREADY_CONSUMED, 14);
    assert_eq!(naos_sys::STATUS_IO_ERROR, 16);
    assert_eq!(naos_sys::EPOLL_CTL_ADD, 1);
    assert_eq!(naos_sys::EPOLL_CTL_MOD, 2);
    assert_eq!(naos_sys::EPOLL_CTL_DEL, 3);
}
