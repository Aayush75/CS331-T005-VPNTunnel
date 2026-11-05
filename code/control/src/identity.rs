//! Gateway static identities (TECHNICAL_DESIGN.md section 3).
//!
//! The pinned identity is the raw 32-byte X25519 public key. Its fingerprint
//! is BLAKE2s-256 of that key as 64 lowercase hex digits. Private identity
//! files are owner-only regular files holding the key as 64 lowercase hex
//! digits plus an optional trailing newline.

use std::fmt;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::Path;
use std::str::FromStr;

use blake2::{Blake2s256, Digest};
use curve25519_dalek::montgomery::MontgomeryPoint;
use thiserror::Error;
use zeroize::{Zeroize, Zeroizing};

pub const KEY_LEN: usize = 32;
const HEX_LEN: usize = KEY_LEN * 2;
const NOISE_PARAMS: &str = "Noise_IK_25519_ChaChaPoly_BLAKE2s";

#[derive(Debug, Error)]
pub enum IdentityError {
    #[error("public key must be exactly 64 lowercase hex digits")]
    PublicKeyFormat,
    #[error("public key is all zeros")]
    PublicKeyZero,
    #[error("private identity file {path}: {reason}")]
    PrivateFile { path: String, reason: String },
    #[error("key generation failed: {0}")]
    Generate(String),
    #[error("{path}: {source}")]
    Io {
        path: String,
        #[source]
        source: io::Error,
    },
}

fn parse_hex_key(s: &str, out: &mut [u8; KEY_LEN]) -> bool {
    s.len() == HEX_LEN
        && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
        && hex::decode_to_slice(s, out).is_ok()
}

/// A pinned peer or local identity.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct PublicKey([u8; KEY_LEN]);

impl PublicKey {
    pub fn as_bytes(&self) -> &[u8; KEY_LEN] {
        &self.0
    }

    pub fn fingerprint(&self) -> String {
        hex::encode(Blake2s256::digest(self.0))
    }
}

impl FromStr for PublicKey {
    type Err = IdentityError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut key = [0u8; KEY_LEN];
        if !parse_hex_key(s, &mut key) {
            return Err(IdentityError::PublicKeyFormat);
        }
        if key == [0u8; KEY_LEN] {
            return Err(IdentityError::PublicKeyZero);
        }
        Ok(Self(key))
    }
}

impl fmt::Display for PublicKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&hex::encode(self.0))
    }
}

impl fmt::Debug for PublicKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PublicKey({self})")
    }
}

/// A local static private key. Not `Clone`; erased on drop; never printed.
pub struct PrivateKey(Zeroizing<[u8; KEY_LEN]>);

impl PrivateKey {
    /// Generate from the OS random source through snow's default resolver.
    pub fn generate() -> Result<Self, IdentityError> {
        let builder = snow::Builder::new(
            NOISE_PARAMS
                .parse()
                .map_err(|e| IdentityError::Generate(format!("{e:?}")))?,
        );
        let mut pair = builder
            .generate_keypair()
            .map_err(|e| IdentityError::Generate(format!("{e:?}")))?;
        pair.public.zeroize();
        let result = <[u8; KEY_LEN]>::try_from(pair.private.as_slice())
            .map(|k| Self(Zeroizing::new(k)))
            .map_err(|_| IdentityError::Generate("unexpected private key length".into()));
        pair.private.zeroize();
        result
    }

    pub fn public_key(&self) -> PublicKey {
        PublicKey(MontgomeryPoint::mul_base_clamped(*self.0).to_bytes())
    }

    /// Borrow the raw key for the Noise builder only.
    pub fn expose_secret(&self) -> &[u8; KEY_LEN] {
        &self.0
    }

    /// Load an owner-only private identity file.
    pub fn load(path: &Path) -> Result<Self, IdentityError> {
        let shown = path.display().to_string();
        let reject = |reason: &str| IdentityError::PrivateFile {
            path: shown.clone(),
            reason: reason.to_owned(),
        };
        let mut file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW)
            .open(path)
            .map_err(|source| IdentityError::Io {
                path: shown.clone(),
                source,
            })?;
        let meta = file.metadata().map_err(|source| IdentityError::Io {
            path: shown.clone(),
            source,
        })?;
        if !meta.is_file() {
            return Err(reject("not a regular file"));
        }
        if meta.mode() & 0o077 != 0 {
            return Err(reject("group/other permissions set; chmod 600"));
        }
        if meta.len() > (HEX_LEN + 1) as u64 {
            return Err(reject("wrong length"));
        }
        let mut text = Zeroizing::new(Vec::with_capacity(HEX_LEN + 1));
        (&mut file)
            .take((HEX_LEN + 2) as u64)
            .read_to_end(&mut text)
            .map_err(|source| IdentityError::Io {
                path: shown.clone(),
                source,
            })?;
        let body = text.strip_suffix(b"\n").unwrap_or(&text);
        let body = std::str::from_utf8(body).map_err(|_| reject("not hex text"))?;
        let mut key = Zeroizing::new([0u8; KEY_LEN]);
        if !parse_hex_key(body, &mut key) {
            return Err(reject("expected 64 lowercase hex digits"));
        }
        Ok(Self(key))
    }

    /// Create a new owner-only private identity file; never overwrites.
    pub fn save_new(&self, path: &Path) -> Result<(), IdentityError> {
        let io_err = |source| IdentityError::Io {
            path: path.display().to_string(),
            source,
        };
        let mut file: File = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(path)
            .map_err(io_err)?;
        let mut text = Zeroizing::new(hex::encode(*self.0));
        text.push('\n');
        file.write_all(text.as_bytes()).map_err(io_err)?;
        file.sync_all().map_err(io_err)
    }
}

impl fmt::Debug for PrivateKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("PrivateKey(<redacted>)")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::os::unix::fs::PermissionsExt;

    // RFC 7748 section 6.1 test vector (Alice).
    const ALICE_PRIV: &str = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
    const ALICE_PUB: &str = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";

    fn write_mode(path: &Path, body: &str, mode: u32) {
        fs::write(path, body).unwrap();
        fs::set_permissions(path, fs::Permissions::from_mode(mode)).unwrap();
    }

    #[test]
    fn public_key_matches_rfc7748_vector() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("a.key");
        write_mode(&path, &format!("{ALICE_PRIV}\n"), 0o600);
        let key = PrivateKey::load(&path).unwrap();
        assert_eq!(key.public_key().to_string(), ALICE_PUB);
    }

    #[test]
    fn fingerprint_is_blake2s_of_raw_key() {
        let pk: PublicKey = ALICE_PUB.parse().unwrap();
        let fp = pk.fingerprint();
        assert_eq!(fp.len(), 64);
        assert_eq!(fp, hex::encode(Blake2s256::digest(pk.as_bytes())));
        assert!(fp.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')));
    }

    #[test]
    fn public_key_format_is_strict() {
        let upper = ALICE_PUB.to_uppercase();
        let short = &ALICE_PUB[..62];
        let spaced = format!(" {}", &ALICE_PUB[1..]);
        for bad in [upper.as_str(), short, spaced.as_str()] {
            assert!(matches!(
                bad.parse::<PublicKey>(),
                Err(IdentityError::PublicKeyFormat)
            ));
        }
        assert!(matches!(
            "0".repeat(64).parse::<PublicKey>(),
            Err(IdentityError::PublicKeyZero)
        ));
    }

    #[test]
    fn generate_save_load_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("gw.key");
        let key = PrivateKey::generate().unwrap();
        key.save_new(&path).unwrap();
        assert_eq!(fs::metadata(&path).unwrap().mode() & 0o777, 0o600);
        let loaded = PrivateKey::load(&path).unwrap();
        assert_eq!(loaded.public_key(), key.public_key());
        // Never overwrite an existing identity.
        assert!(key.save_new(&path).is_err());
    }

    #[test]
    fn two_generated_keys_differ() {
        let a = PrivateKey::generate().unwrap().public_key();
        let b = PrivateKey::generate().unwrap().public_key();
        assert_ne!(a, b);
    }

    #[test]
    fn rejects_unsafe_private_files() {
        let dir = tempfile::tempdir().unwrap();
        let loose = dir.path().join("loose.key");
        write_mode(&loose, ALICE_PRIV, 0o644);
        assert!(matches!(
            PrivateKey::load(&loose),
            Err(IdentityError::PrivateFile { .. })
        ));

        let link = dir.path().join("link.key");
        let target = dir.path().join("target.key");
        write_mode(&target, ALICE_PRIV, 0o600);
        std::os::unix::fs::symlink(&target, &link).unwrap();
        assert!(PrivateKey::load(&link).is_err());

        let long = dir.path().join("long.key");
        write_mode(&long, &format!("{ALICE_PRIV}\n\n"), 0o600);
        assert!(PrivateKey::load(&long).is_err());

        let upper = dir.path().join("upper.key");
        write_mode(&upper, &ALICE_PRIV.to_uppercase(), 0o600);
        assert!(PrivateKey::load(&upper).is_err());
    }

    #[test]
    fn debug_never_prints_secret() {
        let key = PrivateKey::generate().unwrap();
        let shown = format!("{key:?}");
        assert_eq!(shown, "PrivateKey(<redacted>)");
        assert!(!shown.contains(&hex::encode(key.expose_secret())));
    }
}
