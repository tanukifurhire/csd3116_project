#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>

// Result of auth_authenticate(): AUTH_OK (registered or password matched),
// AUTH_BAD_PASSWORD (known user, wrong password), AUTH_ERROR (internal failure:
// credential table full, RNG/hash error or file write failed).
typedef enum
{
	AUTH_OK,
	AUTH_BAD_PASSWORD,
	AUTH_ERROR
} AuthResult;

// Loads (or creates, mode 0600) the credentials file at 'path' into memory.
// Must be called once before auth_authenticate().
// Returns false if the file cannot be opened/created for appending.
bool auth_init(const char *path);

// Registers 'username' with 'password' on first use, or verifies 'password'
// against the credentials already on file for 'username'. Username lookup
// is case-insensitive.
// Thread-safe (internal mutex). Never logs or stores the plaintext password.
AuthResult auth_authenticate(const char *username, const char *password);

#endif
