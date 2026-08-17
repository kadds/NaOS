use naos_sys::{BootstrapFrame, ChannelOptions, Handle, HandleInfo, Status};
use std::mem::{align_of, size_of};

#[test]
fn public_abi_layout_matches_naos_headers() {
    assert_eq!(size_of::<Status>(), 4);
    assert_eq!(size_of::<Handle>(), 8);
    assert_eq!(size_of::<BootstrapFrame>(), 200);
    assert_eq!(align_of::<BootstrapFrame>(), 8);
    assert_eq!(size_of::<ChannelOptions>(), 40);
    assert_eq!(size_of::<HandleInfo>(), 96);
}
