use std::env;
use std::fs;

fn main() {
    let output = env::args().nth(1).expect("fixture output path");
    let value = naos_idl::echo::echo_request {
        payload: b"rust-cxx-interop",
    };
    let mut wire = [0_u8; 128];
    let written =
        naos_idl::echo::encode_echo_request(&value, &mut wire).expect("encode echo request");
    fs::write(output, &wire[..written]).expect("write fixture");
}
