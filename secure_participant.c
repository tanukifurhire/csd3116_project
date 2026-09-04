/* ------------------------------------------------------------------ *
 * Add this function to both client.c and server.c (e.g. just above
 * worker()), then replace the existing:
 *
 *     participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
 *
 * with:
 *
 *     participant = create_secure_participant(
 *         "certs/ca.pem",
 *         "certs/server.pem",        // or certs/clientN.pem on a client
 *         "certs/server.key",        // or certs/clientN.key on a client
 *         "certs/governance.p7s",
 *         "certs/permissions.p7s"
 *     );
 *
 * Adjust the paths to wherever you actually copy the certs/ folder on
 * each machine (server needs ca.pem + server.pem + server.key +
 * governance.p7s + permissions.p7s; each client needs ca.pem +
 * clientN.pem + clientN.key + governance.p7s + permissions.p7s).
 * ------------------------------------------------------------------ */

#include <stdlib.h>
#include <string.h>

static char *file_uri(const char *path)
{
    /* CycloneDDS security properties expect a "file:" URI */
    size_t len = strlen(path) + 6; /* "file:" + path + NUL */
    char *uri = malloc(len);
    snprintf(uri, len, "file:%s", path);
    return uri;
}

dds_entity_t create_secure_participant(
    const char *identity_ca_path,
    const char *identity_cert_path,
    const char *private_key_path,
    const char *governance_path,
    const char *permissions_path)
{
    dds_qos_t *qos = dds_create_qos();

    char *identity_ca   = file_uri(identity_ca_path);
    char *identity_cert = file_uri(identity_cert_path);
    char *private_key   = file_uri(private_key_path);
    char *permissions_ca = file_uri(identity_ca_path); /* same CA reused */
    char *governance    = file_uri(governance_path);
    char *permissions   = file_uri(permissions_path);

    /* --- Authentication plugin --- */
    dds_qset_prop(qos, "dds.sec.auth.identity_ca", identity_ca);
    dds_qset_prop(qos, "dds.sec.auth.identity_certificate", identity_cert);
    dds_qset_prop(qos, "dds.sec.auth.private_key", private_key);
    dds_qset_prop(qos, "dds.sec.auth.library.path", "dds_security_auth");
    dds_qset_prop(qos, "dds.sec.auth.library.init", "init_authentication");
    dds_qset_prop(qos, "dds.sec.auth.library.finalize", "finalize_authentication");

    /* --- Access Control plugin --- */
    dds_qset_prop(qos, "dds.sec.access.permissions_ca", permissions_ca);
    dds_qset_prop(qos, "dds.sec.access.governance", governance);
    dds_qset_prop(qos, "dds.sec.access.permissions", permissions);
    dds_qset_prop(qos, "dds.sec.access.library.path", "dds_security_ac");
    dds_qset_prop(qos, "dds.sec.access.library.init", "init_access_control");
    dds_qset_prop(qos, "dds.sec.access.library.finalize", "finalize_access_control");

    /* --- Cryptographic plugin --- */
    dds_qset_prop(qos, "dds.sec.crypto.library.path", "dds_security_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.init", "init_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.finalize", "finalize_crypto");

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, qos, NULL);

    dds_delete_qos(qos);
    free(identity_ca);
    free(identity_cert);
    free(private_key);
    free(permissions_ca);
    free(governance);
    free(permissions);

    if (participant < 0)
    {
        printf("Failed to create secure participant: %s\n", dds_strretcode(-participant));
    }

    return participant;
}
