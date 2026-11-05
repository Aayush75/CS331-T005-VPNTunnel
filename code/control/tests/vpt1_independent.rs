//! Test-only deterministic VPT1 vector generated independently of snow.

use std::fs;
use std::os::unix::fs::PermissionsExt;

use serde_json::Value;
use snow::Builder;
use vpt_control::exchange::{NOISE_PARAMS, ResponderParams, ValidatingResolver, prologue, respond};
use vpt_control::identity::{PrivateKey, PublicKey};
use vpt_control::wire::{HandshakePayload, Kind, encode_handshake, parse_handshake};

fn bytes(vector: &Value, field: &str) -> Vec<u8> {
    hex::decode(vector[field].as_str().unwrap()).unwrap()
}

#[test]
fn independent_vpt1_ik_transcript_and_framing() {
    let v: Value = serde_json::from_str(include_str!("vectors/vpt1-ik-independent.json")).unwrap();
    let i_static = bytes(&v, "init_static");
    let i_ephemeral = bytes(&v, "init_ephemeral");
    let r_static = bytes(&v, "resp_static");
    let r_ephemeral = bytes(&v, "resp_ephemeral");
    let i_public = bytes(&v, "init_public");
    let r_public = bytes(&v, "resp_public");
    let expected_prologue = bytes(&v, "prologue");
    let first_payload = bytes(&v, "init_payload");
    let second_payload = bytes(&v, "response_payload");
    let first_datagram = bytes(&v, "init");
    let second_datagram = bytes(&v, "response");
    let initiator_key: PublicKey = v["init_public"].as_str().unwrap().parse().unwrap();
    let responder_key: PublicKey = v["resp_public"].as_str().unwrap().parse().unwrap();
    assert_eq!(
        prologue(&initiator_key, &responder_key),
        expected_prologue[..]
    );

    fn builder<'a>(private: &'a [u8], ephemeral: &'a [u8], pro: &'a [u8]) -> Builder<'a> {
        Builder::with_resolver(
            NOISE_PARAMS.parse().unwrap(),
            Box::new(ValidatingResolver::new()),
        )
        .local_private_key(private)
        .unwrap()
        .fixed_ephemeral_key_for_testing_only(ephemeral)
        .prologue(pro)
        .unwrap()
    }
    let mut initiator = builder(&i_static, &i_ephemeral, &expected_prologue)
        .remote_public_key(&r_public)
        .unwrap()
        .build_initiator()
        .unwrap();
    let mut responder = builder(&r_static, &r_ephemeral, &expected_prologue)
        .build_responder()
        .unwrap();
    let mut noise = [0u8; 256];
    let mut plaintext = [0u8; 256];
    let init = parse_handshake(&first_datagram).unwrap();
    assert_eq!(init.kind, Kind::Init);
    let len = initiator.write_message(&first_payload, &mut noise).unwrap();
    assert_eq!(init.noise, &noise[..len]);
    assert_eq!(
        encode_handshake(Kind::Init, 0, &init.attempt_id, &noise[..len]),
        first_datagram
    );
    let read = responder.read_message(init.noise, &mut plaintext).unwrap();
    assert_eq!(&plaintext[..read], first_payload);
    let init_payload = HandshakePayload::decode(&first_payload).unwrap();
    assert_eq!(init_payload.encode().as_slice(), first_payload);

    let response = parse_handshake(&second_datagram).unwrap();
    assert_eq!(response.kind, Kind::Response);
    let len = responder
        .write_message(&second_payload, &mut noise)
        .unwrap();
    assert_eq!(response.noise, &noise[..len]);
    assert_eq!(
        encode_handshake(
            Kind::Response,
            init_payload.initiator_locator,
            &response.attempt_id,
            &noise[..len]
        ),
        second_datagram
    );
    let read = initiator
        .read_message(response.noise, &mut plaintext)
        .unwrap();
    assert_eq!(&plaintext[..read], second_payload);
    assert_eq!(
        HandshakePayload::decode(&second_payload)
            .unwrap()
            .encode()
            .as_slice(),
        second_payload
    );
    assert_eq!(initiator.get_handshake_hash(), bytes(&v, "handshake_hash"));
    assert_eq!(responder.get_handshake_hash(), bytes(&v, "handshake_hash"));
    let (tx, rx) = initiator.dangerously_get_raw_split();
    assert_eq!(tx.as_slice(), bytes(&v, "initiator_tx"));
    assert_eq!(rx.as_slice(), bytes(&v, "initiator_rx"));
    assert_eq!(responder.dangerously_get_raw_split(), (tx, rx));

    // The production responder must accept the independent INIT, even though
    // its fresh ephemeral key makes its RESPONSE different from this vector.
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("public-test-key");
    fs::write(&path, v["resp_static"].as_str().unwrap()).unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
    let responder_private = PrivateKey::load(&path).unwrap();
    assert_eq!(
        responder_private.public_key().as_bytes().as_slice(),
        r_public
    );
    assert_eq!(initiator_key.as_bytes().as_slice(), i_public);
    let params = ResponderParams {
        incarnation: [0xb2; 16],
        responder_locator: 0x5555_6666_7777_8888,
        path_budget: 1500,
        continuity_grant: None,
    };
    assert_eq!(
        respond(&responder_private, &initiator_key, &first_datagram, &params)
            .unwrap()
            .init,
        init_payload
    );
    let mut tampered = first_datagram;
    *tampered.last_mut().unwrap() ^= 1;
    assert!(respond(&responder_private, &initiator_key, &tampered, &params).is_err());
}
