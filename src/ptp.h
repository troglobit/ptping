#ifndef PTPING_PTP_H
#define PTPING_PTP_H

#include <stddef.h>
#include <stdint.h>

#include "ts.h"

/* Length of a Sync message without TLVs. */
#define PTP_SYNC_LEN 44

struct ptp_port_id {
	uint8_t clock_id[8];
	uint16_t port;
};

/* The fields of a received Sync message that ptping cares about. */
struct ptp_sync {
	uint8_t domain;
	struct ptp_port_id src;
	uint16_t seq;
	int64_t correction;	/* raw correctionField, units of 2^-16 ns */
};

/* Fill buf with a Sync message of exactly len bytes, padding with a
 * PAD TLV. len must be PTP_SYNC_LEN, or PTP_SYNC_LEN + 4 or more and
 * even, since the TLV needs a four byte header and an even length.
 * Returns 0, or -1 if len cannot be honoured. */
int ptp_sync_build(uint8_t *buf, size_t len, const struct ptp_port_id *src,
		   uint8_t domain, uint16_t seq);

/* Update the sequenceId of a message built by ptp_sync_build, which
 * is all that changes between the probes of one flow. */
void ptp_sync_set_seq(uint8_t *buf, uint16_t seq);

/* Returns 0 and fills sync if buf holds a PTPv2 Sync, otherwise -1. */
int ptp_sync_parse(const uint8_t *buf, size_t len, struct ptp_sync *sync);

/* Extend a 16-bit sequenceId to 64 bits, given the previous extended
 * value of the same flow. Differences are interpreted as signed, so
 * reordering within 32767 probes is tolerated. */
uint64_t ptp_seq_extend(uint64_t last, uint16_t seq);

/* EUI-64 from a MAC address, the way ptp4l derives clockIdentity. */
void ptp_port_id_from_mac(struct ptp_port_id *pid, const uint8_t mac[6],
			  uint16_t port);

/* The number of identities a process can have on one interface. */
#define PTP_SELF_PORTS 32

/* Identity n, below PTP_SELF_PORTS, of this process on an interface:
 * the EUI-64 of its MAC address, and a port number from the pid and n,
 * so that the sessions of one process, and of several processes on
 * one host, can be told apart. */
void ptp_port_id_self(struct ptp_port_id *pid, const uint8_t mac[6], unsigned n);

int ptp_port_id_cmp(const struct ptp_port_id *a, const struct ptp_port_id *b);

/* Take one Sync message off a UDP socket, with its reception
 * timestamps. Datagrams that are not Sync messages are skipped.
 * Returns 1 if sync was filled, 0 if the socket is empty, or -1 on
 * error. */
int ptp_recv_sync(int fd, struct ptp_sync *sync, struct ts_pair *rx);

/* Wire form: clockIdentity followed by big-endian portNumber. */
#define PTP_PORT_ID_LEN 10
void ptp_port_id_put(uint8_t *buf, const struct ptp_port_id *pid);
void ptp_port_id_get(const uint8_t *buf, struct ptp_port_id *pid);

/* "0011223344556677.1", for logging. buf must hold PTP_PORT_ID_STRLEN. */
#define PTP_PORT_ID_STRLEN 24
const char *ptp_port_id_str(const struct ptp_port_id *pid, char *buf);

#endif
