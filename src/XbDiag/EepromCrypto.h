#pragma once
// EepromCrypto.h
// XbDiag - Xbox EEPROM crypto (SHA1-HMAC + RC4).
//
// Implements the Xbox custom two-pass HMAC-SHA1 key derivation and RC4
// encryption/decryption used to protect the security section (0x14-0x2F).
// All functions operate on a 256-byte EEPROM buffer passed by pointer.
// No module state — purely functional.

#include <xtl.h>

// Decrypt security section (0x14-0x2F) in-place.
// Tries all 4 version-specific IV sets (indices 0-3).
// Returns the version index (0-3) on success, -1 on failure.
int  EepCrypto_Decrypt(BYTE* buf);

// Re-encrypt security section in-place using the given version index (0-3).
// Pass the value returned by EepCrypto_Decrypt, or a known valid index.
void EepCrypto_Encrypt(BYTE* buf, int ver);