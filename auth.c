/* Application-layer player authentication.
 *
 * Independent of the DDS Security cert used for transport (certs/clientN.*
 * only prove "a valid connection slot", not who is behind the keyboard,
 * since all client keys ship in the same repo). Usernames self-register on
 * first join; the password is only ever hashed (PBKDF2-HMAC-SHA256, random
 * per-account salt) - the plaintext is never written to disk or logged.
 */

#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <sys/stat.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#define AUTH_MAX_USERS 256
#define AUTH_MAX_USERNAME_LEN 32
#define AUTH_SALT_LEN 16
#define AUTH_HASH_LEN 32
#define AUTH_ITERATIONS 100000

typedef struct
{
	char username[AUTH_MAX_USERNAME_LEN + 1];
	unsigned char salt[AUTH_SALT_LEN];
	unsigned int iterations;
	unsigned char hash[AUTH_HASH_LEN];
} AuthEntry;

static AuthEntry g_entries[AUTH_MAX_USERS];
static int g_num_entries = 0;
static char g_path[512];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void bytes_to_hex(const unsigned char *bytes, size_t len, char *out_hex)
{
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < len; i++)
	{
		out_hex[i * 2] = digits[(bytes[i] >> 4) & 0xF];
		out_hex[i * 2 + 1] = digits[bytes[i] & 0xF];
	}
	out_hex[len * 2] = '\0';
}

static bool hex_to_bytes(const char *hex, unsigned char *out, size_t out_len)
{
	if (strlen(hex) != out_len * 2)
		return false;

	for (size_t i = 0; i < out_len; i++)
	{
		unsigned int byte;
		if (sscanf(hex + i * 2, "%2x", &byte) != 1)
			return false;
		out[i] = (unsigned char)byte;
	}
	return true;
}

static int find_entry(const char *username)
{
	for (int i = 0; i < g_num_entries; i++)
	{
		if (strcasecmp(g_entries[i].username, username) == 0)
			return i;
	}
	return -1;
}

static bool derive_hash(const char *password, const unsigned char *salt,
						 unsigned int iterations, unsigned char *out_hash)
{
	return PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
							  salt, AUTH_SALT_LEN,
							  (int)iterations, EVP_sha256(),
							  AUTH_HASH_LEN, out_hash) == 1;
}

// Caller must hold g_mutex.
static void load_from_file(void)
{
	FILE *fp = fopen(g_path, "r");
	if (fp == NULL)
		return;

	char line[512];
	while (g_num_entries < AUTH_MAX_USERS && fgets(line, sizeof(line), fp) != NULL)
	{
		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '\0')
			continue;

		char username[AUTH_MAX_USERNAME_LEN + 1];
		char salt_hex[AUTH_SALT_LEN * 2 + 1];
		char hash_hex[AUTH_HASH_LEN * 2 + 1];
		unsigned int iterations;

		// Field widths below must match AUTH_MAX_USERNAME_LEN/AUTH_SALT_LEN/AUTH_HASH_LEN.
		if (sscanf(line, "%32[^:]:%32[^:]:%u:%64[^:\n]",
				   username, salt_hex, &iterations, hash_hex) != 4)
		{
			continue;
		}

		AuthEntry *entry = &g_entries[g_num_entries];
		strncpy(entry->username, username, AUTH_MAX_USERNAME_LEN);
		entry->username[AUTH_MAX_USERNAME_LEN] = '\0';
		entry->iterations = iterations;

		if (!hex_to_bytes(salt_hex, entry->salt, AUTH_SALT_LEN))
			continue;
		if (!hex_to_bytes(hash_hex, entry->hash, AUTH_HASH_LEN))
			continue;

		g_num_entries++;
	}

	fclose(fp);
}

bool auth_init(const char *path)
{
	strncpy(g_path, path, sizeof(g_path) - 1);
	g_path[sizeof(g_path) - 1] = '\0';

	pthread_mutex_lock(&g_mutex);

	g_num_entries = 0;
	load_from_file();

	// Touch the file into existence (if needed) and lock down its
	// permissions, since it holds password salts/hashes.
	FILE *fp = fopen(g_path, "a");
	bool b_ok = (fp != NULL);
	if (fp != NULL)
	{
		fclose(fp);
		chmod(g_path, S_IRUSR | S_IWUSR);
	}

	pthread_mutex_unlock(&g_mutex);

	return b_ok;
}

// Caller must hold g_mutex.
static bool append_entry(const AuthEntry *entry)
{
	FILE *fp = fopen(g_path, "a");
	if (fp == NULL)
		return false;

	char salt_hex[AUTH_SALT_LEN * 2 + 1];
	char hash_hex[AUTH_HASH_LEN * 2 + 1];
	bytes_to_hex(entry->salt, AUTH_SALT_LEN, salt_hex);
	bytes_to_hex(entry->hash, AUTH_HASH_LEN, hash_hex);

	fprintf(fp, "%s:%s:%u:%s\n", entry->username, salt_hex, entry->iterations, hash_hex);
	fclose(fp);
	chmod(g_path, S_IRUSR | S_IWUSR);
	return true;
}

AuthResult auth_authenticate(const char *username, const char *password)
{
	AuthResult result;

	pthread_mutex_lock(&g_mutex);

	int index = find_entry(username);

	if (index < 0)
	{
		// First time this username has been seen - register it.
		if (g_num_entries >= AUTH_MAX_USERS)
		{
			result = AUTH_ERROR;
		}
		else
		{
			AuthEntry entry;
			memset(&entry, 0, sizeof(entry));
			strncpy(entry.username, username, AUTH_MAX_USERNAME_LEN);
			entry.username[AUTH_MAX_USERNAME_LEN] = '\0';
			entry.iterations = AUTH_ITERATIONS;

			if (RAND_bytes(entry.salt, AUTH_SALT_LEN) == 1 &&
				derive_hash(password, entry.salt, entry.iterations, entry.hash) &&
				append_entry(&entry))
			{
				g_entries[g_num_entries++] = entry;
				result = AUTH_OK;
			}
			else
			{
				result = AUTH_ERROR;
			}
		}
	}
	else
	{
		AuthEntry *entry = &g_entries[index];
		unsigned char computed[AUTH_HASH_LEN];

		if (derive_hash(password, entry->salt, entry->iterations, computed))
		{
			result = (CRYPTO_memcmp(computed, entry->hash, AUTH_HASH_LEN) == 0)
						 ? AUTH_OK
						 : AUTH_BAD_PASSWORD;
		}
		else
		{
			result = AUTH_ERROR;
		}
	}

	pthread_mutex_unlock(&g_mutex);

	return result;
}
