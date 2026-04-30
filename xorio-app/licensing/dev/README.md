# Vendor master keypair — DEVELOPMENT ONLY

> [!WARNING]
> This keypair is committed to the repository for pre-launch
> development. **It MUST be rotated before the first production
> release.** Anyone with `vendor_master.key` can issue valid
> licence tokens for any user, tier, and duration.

## Files

| File | Contents |
|---|---|
| `vendor_master.key` | Ed25519 private key (PEM). Signs licence tokens. |
| `vendor_master.pub` | Ed25519 public key (PEM). For reference / inspection. |
| `vendor_master.pub.raw` | Raw 32-byte Ed25519 public key. **Embedded in the `xorio-ui` binary** via `xorio-app/cmake/EmbedBinary.cmake`. This is the key the app verifies licences against. |

## Regenerate

```bash
openssl genpkey -algorithm Ed25519 -out vendor_master.key
openssl pkey -in vendor_master.key -pubout -out vendor_master.pub
openssl pkey -in vendor_master.key -pubout -outform DER | tail -c 32 > vendor_master.pub.raw
```

A clean rebuild of `xorio-ui` picks up the new public key automatically.
After regeneration every previously-issued licence token is invalid.

## Issue a licence token

Build the CLI:

```bash
xorio-app/packaging/docker/build-linux.sh
```

Sign a token:

```bash
xorio-app/dist/linux-x64/xorio-license-tool \
    --key xorio-app/licensing/dev/vendor_master.key \
    --customer user@example.com \
    --tier pro \
    --duration 5d \
    > /tmp/licence.txt
```

> [!NOTE]
> The runtime app's Access tab currently uses a hardcoded gate
> key (see `xorio/src/orchestrator/orchestrator.cpp`). Real
> token verification lands on a follow-up branch and will accept
> the `LIC1`-prefixed string this CLI produces directly.

## Before launch — rotation procedure

1. Generate a fresh keypair on an air-gapped / HSM-backed machine.
2. Move the private half into a secrets vault that the build
   pipeline can reach at release time only.
3. Update `vendor_master.pub.raw` in the repo with the new public
   half; rebuild and ship.
4. Wipe the old `vendor_master.key` from git history
   (`git filter-repo` or similar).
5. Any licence issued before the rotation becomes invalid.
