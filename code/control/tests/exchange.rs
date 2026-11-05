//! VPT1 IK exchange behaviour: round trip, Split direction, exact sizes,
//! retransmission bytes, and every rejection the design names for RJ2.

use snow::params::DHChoice;
use snow::resolvers::CryptoResolver;

use vpt_control::exchange::{
    ExchangeError, Initiator, ResponderParams, ResponseCache, Role, ValidatingResolver, prologue,
    respond,
};
use vpt_control::identity::{PrivateKey, PublicKey};
use vpt_control::wire::{HandshakePayload, INIT_LEN, Mode, RESPONSE_LEN};

/// X25519 u-coordinate 1: a low-order point whose DH output is all zero.
const LOW_ORDER: &str = "0100000000000000000000000000000000000000000000000000000000000000";

struct Pair {
    a: PrivateKey,
    b: PrivateKey,
}

fn pair() -> Pair {
    Pair {
        a: PrivateKey::generate().unwrap(),
        b: PrivateKey::generate().unwrap(),
    }
}

fn init_payload() -> HandshakePayload {
    HandshakePayload {
        mode: Mode::Fresh,
        attempt_id: [0x11; 16],
        incarnation: [0xa1; 16],
        initiator_locator: 0x1111_2222_3333_4444,
        responder_locator: 0,
        parent: [0; 32],
        path_budget: 1500,
    }
}

fn params() -> ResponderParams {
    ResponderParams {
        incarnation: [0xb2; 16],
        responder_locator: 0x5555_6666_7777_8888,
        path_budget: 1500,
        continuity_grant: None,
    }
}

#[test]
fn fresh_exchange_round_trip() {
    let p = pair();
    let mut ini = Initiator::start(&p.a, &p.b.public_key(), init_payload()).unwrap();
    assert_eq!(ini.init_bytes().len(), INIT_LEN);

    let r = respond(&p.b, &p.a.public_key(), ini.init_bytes(), &params()).unwrap();
    assert_eq!(r.response.len(), RESPONSE_LEN);
    assert_eq!(r.init, init_payload());

    let ki = ini.finish(&r.response).unwrap();
    let kr = r.keys;

    // Split direction: initiator TX=k1 is responder RX, and vice versa.
    assert_eq!(ki.role, Role::Initiator);
    assert_eq!(kr.role, Role::Responder);
    assert_eq!(*ki.tx, *kr.rx);
    assert_eq!(*ki.rx, *kr.tx);
    assert_ne!(*ki.tx, *ki.rx);
    assert_eq!(ki.handshake_hash, kr.handshake_hash);

    // Locators and incarnations land on the right side.
    assert_eq!(ki.local_locator, 0x1111_2222_3333_4444);
    assert_eq!(ki.remote_locator, 0x5555_6666_7777_8888);
    assert_eq!(kr.local_locator, ki.remote_locator);
    assert_eq!(kr.remote_locator, ki.local_locator);
    assert_eq!(ki.remote_incarnation, [0xb2; 16]);
    assert_eq!(kr.remote_incarnation, [0xa1; 16]);

    // RESPONSE envelope carries the initiator's locator.
    assert_eq!(&r.response[8..16], &0x1111_2222_3333_4444u64.to_be_bytes());

    // Completed: a second RESPONSE cannot produce another key set.
    assert_eq!(
        ini.finish(&r.response).unwrap_err(),
        ExchangeError::Completed
    );
}

#[test]
fn keys_are_never_printed() {
    let p = pair();
    let mut ini = Initiator::start(&p.a, &p.b.public_key(), init_payload()).unwrap();
    let r = respond(&p.b, &p.a.public_key(), ini.init_bytes(), &params()).unwrap();
    let k = ini.finish(&r.response).unwrap();
    let shown = format!("{k:?} {r:?}");
    assert!(!shown.contains(&hex::encode(*k.tx)));
    assert!(!shown.contains(&hex::encode(*k.rx)));
    assert!(shown.contains("<redacted>"));
}

#[test]
fn prologue_layout() {
    let p = pair();
    let (a, b) = (p.a.public_key(), p.b.public_key());
    let pro = prologue(&a, &b);
    assert_eq!(&pro[..16], b"VPNTUNNEL-V1\0\0\0\0");
    assert_eq!(&pro[16..48], a.as_bytes());
    assert_eq!(&pro[48..80], b.as_bytes());
}

#[test]
fn retransmission_reuses_exact_bytes_and_cache_replays() {
    let p = pair();
    let ini = Initiator::start(&p.a, &p.b.public_key(), init_payload()).unwrap();
    let first = ini.init_bytes().to_vec();
    assert_eq!(ini.init_bytes(), &first[..]);

    // A new attempt uses a fresh ephemeral, so different bytes.
    let other = Initiator::start(&p.a, &p.b.public_key(), init_payload()).unwrap();
    assert_ne!(other.init_bytes(), &first[..]);

    let r = respond(&p.b, &p.a.public_key(), &first, &params()).unwrap();
    let cache = ResponseCache::new(&first, &r.response);
    assert_eq!(cache.replay_for(&first), Some(&r.response[..]));
    assert_eq!(cache.replay_for(other.init_bytes()), None);
    assert_eq!(cache.replay_for(&first[..INIT_LEN - 1]), None);

    // Responding again without the cache uses fresh responder ephemeral state.
    let again = respond(&p.b, &p.a.public_key(), &first, &params()).unwrap();
    assert_ne!(again.response, r.response);
    assert_ne!(*again.keys.tx, *r.keys.tx);
}

#[test]
fn responder_rejects_unpinned_initiator() {
    let p = pair();
    let mallory = PrivateKey::generate().unwrap();
    // Mallory knows B's key and can claim to be anyone in the prologue,
    // but cannot authenticate as A.
    let ini = Initiator::start(&mallory, &p.b.public_key(), init_payload()).unwrap();
    let err = respond(&p.b, &p.a.public_key(), ini.init_bytes(), &params()).unwrap_err();
    assert_eq!(err, ExchangeError::Authentication);
}

#[test]
fn responder_identity_check_after_matching_prologue() {
    // Build INIT by hand with a prologue that names A, but sign as Mallory:
    // decryption succeeds, so the explicit pinned-key comparison must reject.
    let p = pair();
    let mallory = PrivateKey::generate().unwrap();
    let pro = prologue(&p.a.public_key(), &p.b.public_key());
    let mut hs = snow::Builder::with_resolver(
        vpt_control::exchange::NOISE_PARAMS.parse().unwrap(),
        Box::new(ValidatingResolver::new()),
    )
    .local_private_key(mallory.expose_secret())
    .unwrap()
    .remote_public_key(p.b.public_key().as_bytes())
    .unwrap()
    .prologue(&pro)
    .unwrap()
    .build_initiator()
    .unwrap();
    let mut noise = [0u8; 192];
    hs.write_message(&init_payload().encode(), &mut noise)
        .unwrap();
    let init =
        vpt_control::wire::encode_handshake(vpt_control::wire::Kind::Init, 0, &[0x11; 16], &noise);
    let err = respond(&p.b, &p.a.public_key(), &init, &params()).unwrap_err();
    assert_eq!(err, ExchangeError::IdentityMismatch);
}

#[test]
fn initiator_rejects_wrong_responder() {
    let p = pair();
    let other = PrivateKey::generate().unwrap();
    let mut ini = Initiator::start(&p.a, &p.b.public_key(), init_payload()).unwrap();
    // `other` cannot even read an INIT encrypted to B.
    assert_eq!(
        respond(&other, &p.a.public_key(), ini.init_bytes(), &params()).unwrap_err(),
        ExchangeError::Authentication
    );
    // A RESPONSE from a different attempt with B fails authentication and
    // leaves our attempt pending.
    let ini2 = Initiator::start(&p.a, &p.b.public_key(), init_payload()).unwrap();
    let stray = respond(&p.b, &p.a.public_key(), ini2.init_bytes(), &params()).unwrap();
    assert_eq!(
        ini.finish(&stray.response).unwrap_err(),
        ExchangeError::Authentication
    );
    let good = respond(&p.b, &p.a.public_key(), ini.init_bytes(), &params()).unwrap();
    assert!(ini.finish(&good.response).is_ok());
}

#[test]
fn all_zero_dh_rejected_at_initiator() {
    let p = pair();
    let low: PublicKey = LOW_ORDER.parse().unwrap();
    assert_eq!(
        Initiator::start(&p.a, &low, init_payload()).err(),
        Some(ExchangeError::DhRejected)
    );
}

#[test]
fn all_zero_dh_rejected_at_responder() {
    let p = pair();
    let ini = Initiator::start(&p.a, &p.b.public_key(), init_payload()).unwrap();
    let mut init = ini.init_bytes().to_vec();
    // Replace the clear ephemeral (first 32 Noise bytes) with a low-order point.
    init[32..64].copy_from_slice(&hex::decode(LOW_ORDER).unwrap());
    assert_eq!(
        respond(&p.b, &p.a.public_key(), &init, &params()).unwrap_err(),
        ExchangeError::DhRejected
    );
}

#[test]
fn validating_dh_rejects_short_and_zero_inputs_without_panic() {
    let mut dh = ValidatingResolver::new()
        .resolve_dh(&DHChoice::Curve25519)
        .unwrap();
    dh.set(PrivateKey::generate().unwrap().expose_secret());
    let mut out = [0u8; 56];
    assert_eq!(dh.dh(&[9u8; 31], &mut out), Err(snow::Error::Dh));
    assert_eq!(dh.dh(&[0u8; 32], &mut out), Err(snow::Error::Dh));
    assert_eq!(out, [0u8; 56]);
    let mut base = [0u8; 32];
    base[0] = 9;
    assert!(dh.dh(&base, &mut out).is_ok());
}

#[test]
fn path_budget_mismatch_is_failure_not_downgrade() {
    let p = pair();
    let mut payload = init_payload();
    payload.path_budget = 1400;
    let ini = Initiator::start(&p.a, &p.b.public_key(), payload).unwrap();
    assert_eq!(
        respond(&p.b, &p.a.public_key(), ini.init_bytes(), &params()).unwrap_err(),
        ExchangeError::PathBudgetMismatch {
            local: 1500,
            remote: 1400
        }
    );
}

#[test]
fn rekey_requires_matching_grant() {
    let p = pair();
    let mut payload = init_payload();
    payload.mode = Mode::Rekey;
    payload.parent = [0x77; 32];
    let ini = Initiator::start(&p.a, &p.b.public_key(), payload).unwrap();
    let init = ini.init_bytes();

    let mut prm = params();
    assert_eq!(
        respond(&p.b, &p.a.public_key(), init, &prm).unwrap_err(),
        ExchangeError::ContinuityRejected
    );
    prm.continuity_grant = Some([0x78; 32]);
    assert_eq!(
        respond(&p.b, &p.a.public_key(), init, &prm).unwrap_err(),
        ExchangeError::ContinuityRejected
    );
    prm.continuity_grant = Some([0x77; 32]);
    let r = respond(&p.b, &p.a.public_key(), init, &prm).unwrap();
    assert_eq!(r.keys.mode, Mode::Rekey);
}

#[test]
fn tampering_and_correlation_failures() {
    let p = pair();
    let mut ini = Initiator::start(&p.a, &p.b.public_key(), init_payload()).unwrap();

    // Clear attempt ID that disagrees with the authenticated copy.
    let mut init = ini.init_bytes().to_vec();
    init[16] ^= 1;
    assert!(matches!(
        respond(&p.b, &p.a.public_key(), &init, &params()).unwrap_err(),
        ExchangeError::Correlation(_)
    ));

    // Any flipped Noise byte fails authentication (or DH for the ephemeral).
    for at in [64, 100, 150, INIT_LEN - 1] {
        let mut init = ini.init_bytes().to_vec();
        init[at] ^= 0x40;
        assert_eq!(
            respond(&p.b, &p.a.public_key(), &init, &params()).unwrap_err(),
            ExchangeError::Authentication,
            "byte {at}"
        );
    }

    let r = respond(&p.b, &p.a.public_key(), ini.init_bytes(), &params()).unwrap();

    // RESPONSE to a different locator or attempt is ignored before Noise.
    let mut wrong_loc = r.response.clone();
    wrong_loc[15] ^= 1;
    assert!(matches!(
        ini.finish(&wrong_loc).unwrap_err(),
        ExchangeError::Correlation(_)
    ));
    let mut wrong_attempt = r.response.clone();
    wrong_attempt[20] ^= 1;
    assert!(matches!(
        ini.finish(&wrong_attempt).unwrap_err(),
        ExchangeError::Correlation(_)
    ));
    // An INIT fed back to the initiator is the wrong kind.
    let own_init = ini.init_bytes().to_vec();
    assert!(ini.finish(&own_init).is_err());
    // Truncated framing.
    assert!(matches!(
        ini.finish(&r.response[..RESPONSE_LEN - 1]).unwrap_err(),
        ExchangeError::Wire(_)
    ));

    // After all the rejected inputs, the genuine RESPONSE still completes.
    assert!(ini.finish(&r.response).is_ok());
}

#[test]
fn local_misuse_is_rejected() {
    let p = pair();
    let mut bad = init_payload();
    bad.responder_locator = 1;
    assert!(Initiator::start(&p.a, &p.b.public_key(), bad).is_err());
    let mut bad = init_payload();
    bad.initiator_locator = 0;
    assert!(Initiator::start(&p.a, &p.b.public_key(), bad).is_err());

    let ini = Initiator::start(&p.a, &p.b.public_key(), init_payload()).unwrap();
    let mut prm = params();
    prm.responder_locator = 0;
    assert!(respond(&p.b, &p.a.public_key(), ini.init_bytes(), &prm).is_err());
}
