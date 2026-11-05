//! Noise IK exchange and one-time key handoff (TECHNICAL_DESIGN.md 3, 4.2, 5.3).
//!
//! The controller runs only the two handshake messages. After the exchange it
//! extracts the raw Split once, orients it by role and hands it on; snow's
//! transport ciphers are never used, because only the kernel owns counters.

use std::fmt;

use snow::params::{CipherChoice, DHChoice, HashChoice};
use snow::resolvers::{CryptoResolver, DefaultResolver};
use snow::types::{Cipher, Dh, Hash, Random};
use snow::{Builder, HandshakeState};
use subtle::ConstantTimeEq;
use thiserror::Error;
use zeroize::{Zeroize, Zeroizing};

use crate::identity::{KEY_LEN, PrivateKey, PublicKey};
use crate::wire::{
    ATTEMPT_ID_LEN, HASH_LEN, HandshakePayload, INCARNATION_LEN, INIT_NOISE_LEN, Kind, Mode,
    RESPONSE_NOISE_LEN, WireError, encode_handshake, parse_handshake,
};

pub const NOISE_PARAMS: &str = "Noise_IK_25519_ChaChaPoly_BLAKE2s";
const PROLOGUE_DOMAIN: &[u8; 12] = b"VPNTUNNEL-V1";
pub const PROLOGUE_LEN: usize = 80;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum ExchangeError {
    #[error("framing: {0}")]
    Wire(#[from] WireError),
    #[error("all-zero or malformed Diffie-Hellman input rejected")]
    DhRejected,
    #[error("handshake authentication failed")]
    Authentication,
    #[error("authenticated initiator is not the identity pinned for this endpoint")]
    IdentityMismatch,
    #[error("path budget mismatch: local {local}, remote {remote}")]
    PathBudgetMismatch { local: u16, remote: u16 },
    #[error("correlation: {0}")]
    Correlation(&'static str),
    #[error("rekey has no matching continuity grant")]
    ContinuityRejected,
    #[error("exchange already completed")]
    Completed,
    #[error("noise state: {0}")]
    State(String),
}

impl From<snow::Error> for ExchangeError {
    fn from(e: snow::Error) -> Self {
        match e {
            snow::Error::Dh => ExchangeError::DhRejected,
            snow::Error::Decrypt => ExchangeError::Authentication,
            other => ExchangeError::State(other.to_string()),
        }
    }
}

// ---- Validating resolver (section 3.1) -------------------------------------

/// Delegates every primitive to snow's default resolver, but wraps DH so an
/// all-zero result never reaches the Noise key schedule.
pub struct ValidatingResolver(DefaultResolver);

impl ValidatingResolver {
    pub fn new() -> Self {
        Self(DefaultResolver)
    }
}

impl Default for ValidatingResolver {
    fn default() -> Self {
        Self::new()
    }
}

impl CryptoResolver for ValidatingResolver {
    fn resolve_rng(&self) -> Option<Box<dyn Random>> {
        self.0.resolve_rng()
    }

    fn resolve_dh(&self, choice: &DHChoice) -> Option<Box<dyn Dh>> {
        self.0
            .resolve_dh(choice)
            .map(|inner| Box::new(ValidatingDh(inner)) as Box<dyn Dh>)
    }

    fn resolve_hash(&self, choice: &HashChoice) -> Option<Box<dyn Hash>> {
        self.0.resolve_hash(choice)
    }

    fn resolve_cipher(&self, choice: &CipherChoice) -> Option<Box<dyn Cipher>> {
        self.0.resolve_cipher(choice)
    }
}

struct ValidatingDh(Box<dyn Dh>);

impl Dh for ValidatingDh {
    fn name(&self) -> &'static str {
        self.0.name()
    }

    fn pub_len(&self) -> usize {
        self.0.pub_len()
    }

    fn priv_len(&self) -> usize {
        self.0.priv_len()
    }

    fn set(&mut self, privkey: &[u8]) {
        self.0.set(privkey);
    }

    fn generate(&mut self, rng: &mut dyn Random) -> Result<(), snow::Error> {
        self.0.generate(rng)
    }

    fn pubkey(&self) -> &[u8] {
        self.0.pubkey()
    }

    fn privkey(&self) -> &[u8] {
        self.0.privkey()
    }

    fn dh(&self, pubkey: &[u8], out: &mut [u8]) -> Result<(), snow::Error> {
        // snow 0.10.0 passes its fixed MAXDHLEN buffer and the provider reads
        // the first pub_len() bytes, asserting (panicking) on anything short.
        // Turn a short input into an error instead.
        let n = self.0.dh_len();
        if pubkey.len() < self.0.pub_len() || out.len() < n {
            return Err(snow::Error::Dh);
        }
        self.0.dh(pubkey, out)?;
        if bool::from(out[..n].ct_eq(&vec![0u8; n])) {
            out[..n].zeroize();
            return Err(snow::Error::Dh);
        }
        Ok(())
    }

    fn dh_len(&self) -> usize {
        self.0.dh_len()
    }
}

// ---- Prologue and keys -----------------------------------------------------

/// `"VPNTUNNEL-V1" || 0u32 || initiator_static || responder_static`.
pub fn prologue(initiator: &PublicKey, responder: &PublicKey) -> [u8; PROLOGUE_LEN] {
    let mut p = [0u8; PROLOGUE_LEN];
    p[..12].copy_from_slice(PROLOGUE_DOMAIN);
    // 12..16 stay zero.
    p[16..48].copy_from_slice(initiator.as_bytes());
    p[48..80].copy_from_slice(responder.as_bytes());
    p
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Initiator,
    Responder,
}

/// Directional keys and bindings for one completed attempt: exactly what the
/// kernel handoff needs. Keys are erased on drop and never printed.
pub struct SessionKeys {
    pub role: Role,
    pub tx: Zeroizing<[u8; KEY_LEN]>,
    pub rx: Zeroizing<[u8; KEY_LEN]>,
    pub handshake_hash: [u8; HASH_LEN],
    pub attempt_id: [u8; ATTEMPT_ID_LEN],
    pub local_locator: u64,
    pub remote_locator: u64,
    pub remote_incarnation: [u8; INCARNATION_LEN],
    pub mode: Mode,
    /// The handshake hash of the session a `Mode::Rekey` attempt continues; zero for `Mode::Fresh`.
    pub parent: [u8; HASH_LEN],
}

impl fmt::Debug for SessionKeys {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SessionKeys")
            .field("role", &self.role)
            .field("tx", &"<redacted>")
            .field("rx", &"<redacted>")
            .field("handshake_hash", &hex::encode(self.handshake_hash))
            .field("local_locator", &self.local_locator)
            .field("remote_locator", &self.remote_locator)
            .field("mode", &self.mode)
            .finish()
    }
}

/// Take the raw Split once and orient it: initiator TX=k1, responder TX=k2.
fn split(
    mut hs: HandshakeState,
    role: Role,
    sent: &HandshakePayload,
    received: &HandshakePayload,
) -> Result<SessionKeys, ExchangeError> {
    if !hs.is_handshake_finished() {
        return Err(ExchangeError::State("handshake not finished".into()));
    }
    let handshake_hash: [u8; HASH_LEN] = hs
        .get_handshake_hash()
        .try_into()
        .map_err(|_| ExchangeError::State("unexpected hash length".into()))?;
    let (mut k1, mut k2) = hs.dangerously_get_raw_split();
    drop(hs);
    let (mut tx, mut rx) = match role {
        Role::Initiator => (k1, k2),
        Role::Responder => (k2, k1),
    };
    k1.zeroize();
    k2.zeroize();
    let (local_locator, remote_locator) = match role {
        Role::Initiator => (sent.initiator_locator, received.responder_locator),
        Role::Responder => (sent.responder_locator, received.initiator_locator),
    };
    let keys = SessionKeys {
        role,
        tx: Zeroizing::new(tx),
        rx: Zeroizing::new(rx),
        handshake_hash,
        attempt_id: sent.attempt_id,
        local_locator,
        remote_locator,
        remote_incarnation: received.incarnation,
        mode: sent.mode,
        parent: sent.parent,
    };
    // The arrays are `Copy`: wrapping them copied the bytes, so erase these stack copies too.
    tx.zeroize();
    rx.zeroize();
    Ok(keys)
}

fn builder<'a>(resolver_params: &str) -> Result<Builder<'a>, ExchangeError> {
    let params = resolver_params
        .parse()
        .map_err(|e: snow::Error| ExchangeError::State(e.to_string()))?;
    Ok(Builder::with_resolver(
        params,
        Box::new(ValidatingResolver::new()),
    ))
}

// ---- Initiator -------------------------------------------------------------

/// One initiator attempt. Holds the exact INIT bytes for retransmission.
pub struct Initiator {
    state: Option<HandshakeState>,
    init: Vec<u8>,
    sent: HandshakePayload,
}

impl Initiator {
    /// Build and serialize INIT. The kernel supplies `payload`'s attempt ID
    /// and initiator locator; the responder locator must be zero.
    pub fn start(
        local: &PrivateKey,
        responder: &PublicKey,
        payload: HandshakePayload,
    ) -> Result<Self, ExchangeError> {
        if payload.responder_locator != 0 {
            return Err(ExchangeError::Correlation(
                "INIT responder locator must be zero",
            ));
        }
        if payload.initiator_locator == 0 || payload.incarnation == [0; INCARNATION_LEN] {
            return Err(ExchangeError::Correlation(
                "INIT needs a locator and incarnation",
            ));
        }
        let pro = prologue(&local.public_key(), responder);
        let mut state = builder(NOISE_PARAMS)?
            .local_private_key(local.expose_secret())?
            .remote_public_key(responder.as_bytes())?
            .prologue(&pro)?
            .build_initiator()?;
        let mut noise = [0u8; INIT_NOISE_LEN];
        let n = state.write_message(&payload.encode(), &mut noise)?;
        if n != INIT_NOISE_LEN {
            return Err(ExchangeError::State(format!("INIT noise length {n}")));
        }
        let init = encode_handshake(Kind::Init, 0, &payload.attempt_id, &noise);
        Ok(Self {
            state: Some(state),
            init,
            sent: payload,
        })
    }

    /// The exact INIT datagram; retransmissions reuse these bytes (5.3).
    pub fn init_bytes(&self) -> &[u8] {
        &self.init
    }

    /// Process a RESPONSE. A rejected datagram leaves the attempt pending, so
    /// an injected forgery cannot cancel it; success consumes it.
    pub fn finish(&mut self, response: &[u8]) -> Result<SessionKeys, ExchangeError> {
        let state = self.state.as_mut().ok_or(ExchangeError::Completed)?;
        let d = parse_handshake(response)?;
        if d.kind != Kind::Response {
            return Err(ExchangeError::Correlation("expected RESPONSE"));
        }
        if d.locator != self.sent.initiator_locator {
            return Err(ExchangeError::Correlation("envelope locator is not ours"));
        }
        if d.attempt_id != self.sent.attempt_id {
            return Err(ExchangeError::Correlation("attempt ID mismatch"));
        }
        let mut plain = Zeroizing::new([0u8; RESPONSE_NOISE_LEN]);
        let n = state.read_message(d.noise, &mut plain[..])?;
        let got = HandshakePayload::decode(&plain[..n])?;
        let s = &self.sent;
        if got.attempt_id != d.attempt_id {
            return Err(ExchangeError::Correlation(
                "payload attempt differs from clear copy",
            ));
        }
        if got.mode != s.mode || got.initiator_locator != s.initiator_locator {
            return Err(ExchangeError::Correlation("RESPONSE does not echo INIT"));
        }
        if got.parent != s.parent {
            return Err(ExchangeError::Correlation("RESPONSE parent differs"));
        }
        if got.responder_locator == 0 {
            return Err(ExchangeError::Correlation(
                "RESPONSE responder locator is zero",
            ));
        }
        if got.path_budget != s.path_budget {
            return Err(ExchangeError::PathBudgetMismatch {
                local: s.path_budget,
                remote: got.path_budget,
            });
        }
        let state = self.state.take().ok_or(ExchangeError::Completed)?;
        split(state, Role::Initiator, s, &got)
    }
}

// ---- Responder -------------------------------------------------------------

/// What this responder contributes; the kernel supplies the locator.
#[derive(Clone, Copy, Debug)]
pub struct ResponderParams {
    pub incarnation: [u8; INCARNATION_LEN],
    pub responder_locator: u64,
    pub path_budget: u16,
    /// The handshake hash of the session this side last staged: the only parent a mode 1 INIT may
    /// name. The kernel separately requires a live grant for the attempt.
    pub continuity_grant: Option<[u8; HASH_LEN]>,
}

/// A completed responder side: the exact RESPONSE to send and the keys.
#[derive(Debug)]
pub struct Responded {
    pub response: Vec<u8>,
    pub keys: SessionKeys,
    pub init: HandshakePayload,
}

/// Verify INIT from the peer selected by its configured endpoint and build
/// RESPONSE. Each call uses fresh responder ephemeral state.
pub fn respond(
    local: &PrivateKey,
    expected_initiator: &PublicKey,
    init: &[u8],
    params: &ResponderParams,
) -> Result<Responded, ExchangeError> {
    if params.responder_locator == 0 || params.incarnation == [0; INCARNATION_LEN] {
        return Err(ExchangeError::Correlation(
            "RESPONSE needs a locator and incarnation",
        ));
    }
    let d = parse_handshake(init)?;
    if d.kind != Kind::Init {
        return Err(ExchangeError::Correlation("expected INIT"));
    }
    let pro = prologue(expected_initiator, &local.public_key());
    let mut state = builder(NOISE_PARAMS)?
        .local_private_key(local.expose_secret())?
        .prologue(&pro)?
        .build_responder()?;
    let mut plain = Zeroizing::new([0u8; INIT_NOISE_LEN]);
    let n = state.read_message(d.noise, &mut plain[..])?;
    let remote = state
        .get_remote_static()
        .ok_or(ExchangeError::IdentityMismatch)?;
    if !bool::from(remote.ct_eq(expected_initiator.as_bytes())) {
        return Err(ExchangeError::IdentityMismatch);
    }
    let got = HandshakePayload::decode(&plain[..n])?;
    if got.attempt_id != d.attempt_id {
        return Err(ExchangeError::Correlation(
            "payload attempt differs from clear copy",
        ));
    }
    if got.responder_locator != 0 {
        return Err(ExchangeError::Correlation(
            "INIT responder locator must be zero",
        ));
    }
    if got.path_budget != params.path_budget {
        return Err(ExchangeError::PathBudgetMismatch {
            local: params.path_budget,
            remote: got.path_budget,
        });
    }
    if got.mode == Mode::Rekey && params.continuity_grant != Some(got.parent) {
        return Err(ExchangeError::ContinuityRejected);
    }
    let reply = HandshakePayload {
        mode: got.mode,
        attempt_id: got.attempt_id,
        incarnation: params.incarnation,
        initiator_locator: got.initiator_locator,
        responder_locator: params.responder_locator,
        parent: got.parent,
        path_budget: params.path_budget,
    };
    let mut noise = [0u8; RESPONSE_NOISE_LEN];
    let n = state.write_message(&reply.encode(), &mut noise)?;
    if n != RESPONSE_NOISE_LEN {
        return Err(ExchangeError::State(format!("RESPONSE noise length {n}")));
    }
    let response = encode_handshake(
        Kind::Response,
        got.initiator_locator,
        &got.attempt_id,
        &noise,
    );
    let keys = split(state, Role::Responder, &reply, &got)?;
    Ok(Responded {
        response,
        keys,
        init: got,
    })
}

/// Nonsecret per-attempt cache of the exact INIT and RESPONSE (5.3): an
/// identical duplicate INIT gets the identical cached RESPONSE without
/// advancing Noise again. Expiry is the caller's (10 s pending, 20 s done).
#[derive(Clone, Debug)]
pub struct ResponseCache {
    init: Vec<u8>,
    response: Vec<u8>,
}

impl ResponseCache {
    pub fn new(init: &[u8], response: &[u8]) -> Self {
        Self {
            init: init.to_vec(),
            response: response.to_vec(),
        }
    }

    /// The cached RESPONSE only for a byte-identical INIT.
    pub fn replay_for(&self, init: &[u8]) -> Option<&[u8]> {
        (self.init.len() == init.len() && bool::from(self.init.ct_eq(init)))
            .then_some(&self.response[..])
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use chacha20poly1305::aead::{Aead, KeyInit, Payload};
    use chacha20poly1305::{ChaCha20Poly1305, Nonce};

    fn h(v: &serde_json::Value, k: &str) -> Vec<u8> {
        hex::decode(v[k].as_str().unwrap()).unwrap()
    }

    /// Tie `split`'s orientation to the upstream vector: the initiator's TX
    /// and the responder's RX must decrypt upstream transport message 2
    /// (initiator to responder, nonce 0), independently of snow's transport.
    #[test]
    fn split_orientation_matches_upstream_transport() {
        let file: serde_json::Value = serde_json::from_str(include_str!(
            "../tests/vectors/cacophony-ik-25519-chachapoly-blake2s.json"
        ))
        .unwrap();
        let v = &file["vector"];
        let (i_s, i_e, i_rs, pro) = (
            h(v, "init_static"),
            h(v, "init_ephemeral"),
            h(v, "init_remote_static"),
            h(v, "init_prologue"),
        );
        let (r_s, r_e) = (h(v, "resp_static"), h(v, "resp_ephemeral"));
        let mut ini = builder(NOISE_PARAMS)
            .unwrap()
            .local_private_key(&i_s)
            .unwrap()
            .fixed_ephemeral_key_for_testing_only(&i_e)
            .remote_public_key(&i_rs)
            .unwrap()
            .prologue(&pro)
            .unwrap()
            .build_initiator()
            .unwrap();
        let mut res = builder(NOISE_PARAMS)
            .unwrap()
            .local_private_key(&r_s)
            .unwrap()
            .fixed_ephemeral_key_for_testing_only(&r_e)
            .prologue(&pro)
            .unwrap()
            .build_responder()
            .unwrap();
        let msgs = v["messages"].as_array().unwrap();
        let (mut buf, mut out) = ([0u8; 512], [0u8; 512]);
        let n = ini
            .write_message(&h(&msgs[0], "payload"), &mut buf)
            .unwrap();
        res.read_message(&buf[..n], &mut out).unwrap();
        let n = res
            .write_message(&h(&msgs[1], "payload"), &mut buf)
            .unwrap();
        ini.read_message(&buf[..n], &mut out).unwrap();

        let p = HandshakePayload {
            mode: Mode::Fresh,
            attempt_id: [1; 16],
            incarnation: [1; 16],
            initiator_locator: 1,
            responder_locator: 2,
            parent: [0; 32],
            path_budget: 1500,
        };
        let ki = split(ini, Role::Initiator, &p, &p).unwrap();
        let kr = split(res, Role::Responder, &p, &p).unwrap();
        assert_eq!(ki.handshake_hash.to_vec(), h(v, "handshake_hash"));

        let decrypt = |key: &[u8; KEY_LEN]| {
            ChaCha20Poly1305::new(key.into()).decrypt(
                &Nonce::from([0u8; 12]),
                Payload {
                    msg: &h(&msgs[2], "ciphertext"),
                    aad: &[],
                },
            )
        };
        assert_eq!(decrypt(&ki.tx).unwrap(), h(&msgs[2], "payload"));
        assert_eq!(decrypt(&kr.rx).unwrap(), h(&msgs[2], "payload"));
        assert!(decrypt(&ki.rx).is_err());
        assert!(decrypt(&kr.tx).is_err());
    }
}
