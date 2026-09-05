#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>

typedef enum
{
	AUTH_OK,
	AUTH_BAD_PASSWORD,
	AUTH_ERROR
} AuthResult;

// Loads (or creates, mode 0600) the credentials file at 'path' into memory.
// Must be called once before auth_authenticate().
bool auth_init(const char *path);

// Registers 'username' with 'password' on first use, or verifies 'password'
// against the credentials already on file for 'username'. Username lookup
// is case-insensitive.
AuthResult auth_authenticate(const char *username, const char *password);

#endif
