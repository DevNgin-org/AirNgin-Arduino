# TLS, AES and security configuration

## TLS and AES are different layers

AirNgin's MQTT transport remains TLS-protected on port 8883. PLAIN means application-layer AES is disabled; it does **not** mean clear-text networking.

```text
PLAIN = MQTT/TLS + normal AirNgin JSON
AES   = MQTT/TLS + AES-256-GCM AirNgin payload security
```

## Enable AES

In sketch `build_opt.h`:

```text
-DCONFIG_AIRNGIN_USE_AES=1
```

An AES build requires a valid `encryptionKey` (Base64 of exactly 32 bytes). External mode supplies it in `airngin_cloud_credentials_t.encryption_key`; Managed mode receives it through provisioning.

## Diagnostics

```text
-DCONFIG_AIRNGIN_DIAGNOSTIC_LOGS=1
-DCONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE=1
-DCONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS=0
```

PLAIN RX payloads can be printed for debugging. In AES mode decrypted plaintext is only printed when secret diagnostics are explicitly enabled. AES keys are never printed.

## Certificates in the Arduino port

- MQTT: AirNgin protocol Root CA embedded in the library.
- Cloud Database: embedded ISRG Root X1 with hostname verification enabled.

Do not weaken TLS using insecure/skip-verification modes.
