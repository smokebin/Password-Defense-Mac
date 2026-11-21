// cred_type.h
// Shared Credential type enum (included by credential.h and UI.h)
#pragma once

// Passkey (4): a WebAuthn credential stored as a vault item (created on iOS). Its
// fields (rp_id, user_handle, credential_id, private_key, ...) aren't modeled here;
// they ride in Credential::extra_json. cred_ops::load_all hides this type.
enum class CredType : int { Password = 0, CreditCard = 1, Identity = 2, SecureNote = 3, Passkey = 4 };

// Types this app lists, edits and autofills. Anything else (passkeys, types from
// newer clients) is kept in the vault untouched but never shown.
inline bool IsUserVisibleType(CredType t)
{
    return t == CredType::Password || t == CredType::CreditCard
        || t == CredType::Identity || t == CredType::SecureNote;
}

inline const char* CredTypeLabel(CredType t)
{
    switch (t) {
    case CredType::CreditCard: return "Credit Card";
    case CredType::Identity:   return "Identity";
    case CredType::SecureNote: return "Secure Note";
    case CredType::Passkey:    return "Passkey";
    default:                   return "Password";
    }
}
