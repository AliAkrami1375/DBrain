/*
 * Version numbers that gate interoperability.
 *
 * DBRAIN_VERSION          Human-readable release of the whole project.
 * DBRAIN_PROTO_VERSION    Wire protocol. Carried in the second byte of every
 *                         frame; nodes ignore frames of a different version.
 * DBRAIN_ARTIFACT_VERSION Layout of compiled shard images (docs/artifacts.md).
 * DBRAIN_MODEL_VERSION    Numerical scheme of the simulation core. Two nodes
 *                         with different model versions would compute
 *                         different bit patterns for the same network, so the
 *                         loader refuses images built for another version.
 */
#ifndef DBRAIN_VERSION_H
#define DBRAIN_VERSION_H

#define DBRAIN_VERSION          "0.1.0"
#define DBRAIN_PROTO_VERSION    1
#define DBRAIN_ARTIFACT_VERSION 1
#define DBRAIN_MODEL_VERSION    1

#endif /* DBRAIN_VERSION_H */
