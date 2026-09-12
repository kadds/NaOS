use std::env;
use std::fs;

const RUST_REQUEST: &[u8] = b"rust-cxx-interop";
const CXX_REQUEST: &[u8] = b"cxx-rust-interop";

fn encode_request(output: &str) {
    let value = naos_idl::echo::echo_request {
        payload: RUST_REQUEST,
    };
    let mut wire = [0_u8; 128];
    let written =
        naos_idl::echo::encode_echo_request(&value, &mut wire).expect("encode echo request");
    fs::write(output, &wire[..written]).expect("write fixture");
}

fn decode_request(input: &str) {
    let wire = fs::read(input).expect("read C++ request");
    let value = naos_idl::echo::decode_echo_request(&wire).expect("decode echo request");
    assert_eq!(value.payload, CXX_REQUEST, "C++ request payload mismatch");
    println!("OK decode-request");
}

fn decode_response(input: &str) {
    let wire = fs::read(input).expect("read C++ response");
    let value = naos_idl::echo::decode_echo_response(&wire).expect("decode echo response");
    assert_eq!(value.payload, CXX_REQUEST, "C++ response payload mismatch");
    println!("OK decode-response");
}

fn main() {
    let mut arguments = env::args().skip(1);
    match (arguments.next().as_deref(), arguments.next()) {
        (Some("encode-request"), Some(output)) => encode_request(&output),
        (Some("decode-request"), Some(input)) => decode_request(&input),
        (Some("decode-response"), Some(input)) => decode_response(&input),
        _ => panic!("usage: fixture encode-request|decode-request|decode-response <wire-file>"),
    }
}
