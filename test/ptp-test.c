#include <string.h>

#include "ptp.h"
#include "test.h"

static const uint8_t mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };

static void test_build_parse(void)
{
	struct ptp_port_id src;
	struct ptp_sync sync;
	uint8_t buf[128];
	char str[PTP_PORT_ID_STRLEN];

	ptp_port_id_from_mac(&src, mac, 7);
	check(!memcmp(src.clock_id, "\x00\x11\x22\xff\xfe\x33\x44\x55", 8));
	check(!strcmp(ptp_port_id_str(&src, str), "001122fffe334455.7"));

	check(!ptp_sync_build(buf, PTP_SYNC_LEN, &src, 254, 0x1234));
	check(buf[0] == 0 && buf[1] == 2);
	check(buf[2] == 0 && buf[3] == PTP_SYNC_LEN);
	check(buf[6] == 0x06 && buf[7] == 0x00);
	check(buf[33] == 0x7f);

	check(!ptp_sync_parse(buf, PTP_SYNC_LEN, &sync));
	check(sync.domain == 254);
	check(!ptp_port_id_cmp(&sync.src, &src));
	check(sync.seq == 0x1234);
	check(sync.correction == 0);

	ptp_sync_set_seq(buf, 0x4321);
	check(!ptp_sync_parse(buf, PTP_SYNC_LEN, &sync));
	check(sync.seq == 0x4321);

	/* Trailing bytes beyond messageLength are fine. */
	check(!ptp_sync_parse(buf, sizeof(buf), &sync));

	/* A truncated message is not. */
	check(ptp_sync_parse(buf, PTP_SYNC_LEN - 1, &sync));
	buf[3] = 100;
	check(ptp_sync_parse(buf, PTP_SYNC_LEN, &sync));

	/* Delay_Req (type 1) and version 1 are rejected. */
	buf[3] = PTP_SYNC_LEN;
	buf[0] = 1;
	check(ptp_sync_parse(buf, PTP_SYNC_LEN, &sync));
	buf[0] = 0;
	buf[1] = 1;
	check(ptp_sync_parse(buf, PTP_SYNC_LEN, &sync));
}

static void test_padding(void)
{
	struct ptp_port_id src = { .port = 1 };
	struct ptp_sync sync;
	uint8_t buf[128];

	check(ptp_sync_build(buf, PTP_SYNC_LEN - 2, &src, 0, 0));
	check(ptp_sync_build(buf, PTP_SYNC_LEN + 2, &src, 0, 0));
	check(ptp_sync_build(buf, PTP_SYNC_LEN + 5, &src, 0, 0));

	check(!ptp_sync_build(buf, PTP_SYNC_LEN + 4, &src, 0, 0));
	check(buf[3] == PTP_SYNC_LEN + 4);
	check(buf[44] == 0x80 && buf[45] == 0x08);
	check(buf[46] == 0 && buf[47] == 0);

	check(!ptp_sync_build(buf, 100, &src, 0, 0));
	check(buf[3] == 100);
	check(buf[46] == 0 && buf[47] == 52);
	check(!ptp_sync_parse(buf, 100, &sync));
}

static void test_correction(void)
{
	struct ptp_port_id src = { .port = 1 };
	struct ptp_sync sync;
	uint8_t buf[PTP_SYNC_LEN];

	check(!ptp_sync_build(buf, sizeof(buf), &src, 0, 0));
	memcpy(buf + 8, "\xff\xff\xff\xff\xff\xff\xff\xfe", 8);
	check(!ptp_sync_parse(buf, sizeof(buf), &sync));
	check(sync.correction == -2);
}

static void test_seq_extend(void)
{
	uint64_t ext = 0;

	ext = ptp_seq_extend(ext, 1);
	check(ext == 1);
	ext = ptp_seq_extend(ext, 30000);
	check(ext == 30000);
	ext = ptp_seq_extend(ext, 60000);
	check(ext == 60000);
	ext = ptp_seq_extend(ext, 65535);
	check(ext == 65535);
	ext = ptp_seq_extend(ext, 0);
	check(ext == 65536);
	ext = ptp_seq_extend(ext, 3);
	check(ext == 65539);

	/* Reordered: an older probe arrives late. */
	check(ptp_seq_extend(ext, 65534) == 65534);
	/* Far ahead: a burst of loss. */
	check(ptp_seq_extend(ext, 3 + 1000) == 66539);
	/* Many wraps in. */
	ext = 10 * 65536 + 5;
	check(ptp_seq_extend(ext, 6) == 10 * 65536 + 6);
	check(ptp_seq_extend(ext, 4) == 10 * 65536 + 4);
}

int main(void)
{
	test_build_parse();
	test_padding();
	test_correction();
	test_seq_extend();
	return 0;
}
