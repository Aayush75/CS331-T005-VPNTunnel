//! Upstream Noise vector through our validating resolver, plus an independent
//! check of raw Split direction.
//!
//! The transport messages after the handshake are decrypted with the
//! `chacha20poly1305` crate directly, not snow's transport state, using the
//! keys from `dangerously_get_raw_split`. That fixes which Split half is the
//! initiator's send key without trusting snow's own orientation.

use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{ChaCha20Poly1305, Nonce};
use serde_json::Value;
use snow::Builder;

use vpt_control::exchange::{NOISE_PARAMS, ValidatingResolver};

fn h(v: &Value, k: &str) -> Vec<u8> {
    hex::decode(v[k].as_str().unwrap()).unwrap()
}

/// Noise ChaChaPoly nonce: 4 zero bytes || little-endian u64 counter.
fn nonce(n: u64) -> Nonce {
    let mut b = [0u8; 12];
    b[4..].copy_from_slice(&n.to_le_bytes());
    Nonce::from(b)
}

#[test]
fn cacophony_ik_vector() {
    let file: Value = serde_json::from_str(include_str!(
        "vectors/cacophony-ik-25519-chachapoly-blake2s.json"
    ))
    .unwrap();
    let v = &file["vector"];
    assert_eq!(v["protocol_name"], NOISE_PARAMS);

    let (i_s, i_e, i_rs, i_pro) = (
        h(v, "init_static"),
        h(v, "init_ephemeral"),
        h(v, "init_remote_static"),
        h(v, "init_prologue"),
    );
    let (r_s, r_e, r_pro) = (
        h(v, "resp_static"),
        h(v, "resp_ephemeral"),
        h(v, "resp_prologue"),
    );
    fn build<'a>(s: &'a [u8], e: &'a [u8], pro: &'a [u8]) -> Builder<'a> {
        Builder::with_resolver(
            NOISE_PARAMS.parse().unwrap(),
            Box::new(ValidatingResolver::new()),
        )
        .local_private_key(s)
        .unwrap()
        .fixed_ephemeral_key_for_testing_only(e)
        .prologue(pro)
        .unwrap()
    }
    let mut ini = build(&i_s, &i_e, &i_pro)
        .remote_public_key(&i_rs)
        .unwrap()
        .build_initiator()
        .unwrap();
    let mut res = build(&r_s, &r_e, &r_pro).build_responder().unwrap();

    let msgs = v["messages"].as_array().unwrap();
    let mut buf = [0u8; 1024];
    let mut out = [0u8; 1024];

    // Handshake: message 0 is I->R, message 1 is R->I.
    for (i, m) in msgs[..2].iter().enumerate() {
        let (tx, rx) = if i == 0 {
            (&mut ini, &mut res)
        } else {
            (&mut res, &mut ini)
        };
        let n = tx.write_message(&h(m, "payload"), &mut buf).unwrap();
        assert_eq!(&buf[..n], &h(m, "ciphertext")[..], "message {i}");
        let p = rx.read_message(&buf[..n], &mut out).unwrap();
        assert_eq!(&out[..p], &h(m, "payload")[..]);
    }
    assert!(ini.is_handshake_finished() && res.is_handshake_finished());
    let hash = h(v, "handshake_hash");
    assert_eq!(ini.get_handshake_hash(), &hash[..]);
    assert_eq!(res.get_handshake_hash(), &hash[..]);

    let (k1, k2) = ini.dangerously_get_raw_split();
    assert_eq!(res.dangerously_get_raw_split(), (k1, k2));

    // Transport messages alternate I->R, R->I. Initiator sends with k1.
    let i_to_r = ChaCha20Poly1305::new(&k1.into());
    let r_to_i = ChaCha20Poly1305::new(&k2.into());
    let (mut ni, mut nr) = (0u64, 0u64);
    for (i, m) in msgs[2..].iter().enumerate() {
        let (cipher, n) = if i % 2 == 0 {
            (&i_to_r, &mut ni)
        } else {
            (&r_to_i, &mut nr)
        };
        let plain = cipher
            .decrypt(
                &nonce(*n),
                Payload {
                    msg: &h(m, "ciphertext"),
                    aad: &[],
                },
            )
            .unwrap_or_else(|_| panic!("transport message {} did not decrypt", i + 2));
        assert_eq!(plain, h(m, "payload"));
        *n += 1;
    }
}
