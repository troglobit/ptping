#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ctrl.h"
#include "test.h"

static void roundtrip(const struct ctrl_msg *in, struct ctrl_msg *out)
{
	uint8_t buf[CTRL_MSG_MAX];
	size_t len = ctrl_encode(in, buf);

	check(len <= CTRL_MSG_MAX);
	check(len >= CTRL_HDR_LEN && buf[0] == in->type);
	check(ctrl_decode(buf, len - 1, out) == 0);
	check(ctrl_decode(buf, len, out) == (ssize_t)len);
	check(ctrl_decode(buf, len + 10, out) == (ssize_t)len);
	check(out->type == in->type);
}

static void test_hello(void)
{
	struct ctrl_msg in = {
		.type = CTRL_HELLO,
		.hello = {
			.flags = CTRL_HELLO_BIDIR,
			.domain = 254,
			.tstamp = CTRL_TS_ANY,
			.src = { .clock_id = { 1, 2, 3, 0xff, 0xfe, 4, 5, 6 }, .port = 1234 },
			.rev = {
				.period_ns = 10000000,
				.count = 5000,
				.size = 100,
				.dscp = 40,
			},
		},
	}, out;
	uint8_t buf[CTRL_MSG_MAX];

	roundtrip(&in, &out);
	check(!memcmp(&in.hello, &out.hello, sizeof(in.hello)));

	/* Wrong version is rejected. */
	ctrl_encode(&in, buf);
	buf[4] = 2;
	check(ctrl_decode(buf, sizeof(buf), &out) == -1);
}

static void test_welcome(void)
{
	struct ctrl_msg in = {
		.type = CTRL_WELCOME,
		.welcome = {
			.status = 0,
			.tstamp = CTRL_TS_HW,
			.src = { .clock_id = { 8, 7, 6, 5, 4, 3, 2, 1 }, .port = 1 },
			.text = "",
		},
	}, out;
	size_t i;

	roundtrip(&in, &out);
	check(!memcmp(&in.welcome, &out.welcome, sizeof(in.welcome)));

	in.welcome.status = 1;
	strcpy(in.welcome.text, "no hardware timestamping on eth0");
	roundtrip(&in, &out);
	check(out.welcome.status == 1);
	check(!strcmp(out.welcome.text, in.welcome.text));

	/* Longest text. */
	for (i = 0; i < CTRL_TEXT_MAX; i++)
		in.welcome.text[i] = 'a' + i % 26;
	in.welcome.text[i] = '\0';
	roundtrip(&in, &out);
	check(strlen(out.welcome.text) == CTRL_TEXT_MAX);
	check(!strcmp(out.welcome.text, in.welcome.text));
}

static void test_ts(void)
{
	struct ctrl_msg in = {
		.type = CTRL_TS,
		.ts = { .flow = CTRL_FLOW_REV, .kind = CTRL_KIND_TX },
	}, out;
	uint8_t buf[CTRL_MSG_MAX];
	unsigned i;

	roundtrip(&in, &out);
	check(out.ts.flow == CTRL_FLOW_REV && out.ts.kind == CTRL_KIND_TX);
	check(out.ts.count == 0);

	for (i = 0; i < CTRL_TS_BATCH; i++) {
		in.ts.rec[i].seq = 0x100000000ull + i;
		in.ts.rec[i].ts = -1 - (int64_t)i;
		in.ts.rec[i].correction = (int64_t)i << 16;
	}
	in.ts.count = CTRL_TS_BATCH;
	roundtrip(&in, &out);
	check(out.ts.count == CTRL_TS_BATCH);
	check(!memcmp(in.ts.rec, out.ts.rec, sizeof(in.ts.rec)));

	/* Count larger than the batch, or inconsistent with the length. */
	ctrl_encode(&in, buf);
	buf[6] = 0; buf[7] = CTRL_TS_BATCH + 1;
	check(ctrl_decode(buf, sizeof(buf), &out) == -1);
	buf[7] = CTRL_TS_BATCH - 1;
	check(ctrl_decode(buf, sizeof(buf), &out) == -1);
}

static void test_bye_error(void)
{
	struct ctrl_msg in = { .type = CTRL_BYE }, out;
	uint8_t buf[CTRL_MSG_MAX];

	roundtrip(&in, &out);

	in.type = CTRL_ERROR;
	strcpy(in.text, "went wrong");
	roundtrip(&in, &out);
	check(!strcmp(out.text, "went wrong"));

	/* Unknown type, and a length that exceeds the maximum. */
	memset(buf, 0, sizeof(buf));
	buf[0] = 99;
	check(ctrl_decode(buf, sizeof(buf), &out) == -1);
	buf[0] = CTRL_BYE;
	buf[2] = 0xff; buf[3] = 0xff;
	check(ctrl_decode(buf, sizeof(buf), &out) == -1);
}

static void test_conn(void)
{
	struct ctrl_msg in = { .type = CTRL_ERROR, .text = "x" }, out;
	struct ctrl_conn a, b;
	uint8_t buf[CTRL_MSG_MAX];
	size_t len;
	int sv[2];

	check(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	ctrl_conn_init(&a, sv[0]);
	ctrl_conn_init(&b, sv[1]);

	/* Nothing yet. */
	check(ctrl_recv(&b, &out) == 0);

	/* Two messages in one segment, then a partial one. */
	check(!ctrl_send(&a, &in));
	in.type = CTRL_BYE;
	check(!ctrl_send(&a, &in));
	in.type = CTRL_ERROR;
	len = ctrl_encode(&in, buf);
	check(write(sv[0], buf, len - 1) == (ssize_t)(len - 1));

	check(ctrl_recv(&b, &out) == 1 && out.type == CTRL_ERROR);
	check(ctrl_recv(&b, &out) == 1 && out.type == CTRL_BYE);
	check(ctrl_recv(&b, &out) == 0);
	check(write(sv[0], buf + len - 1, 1) == 1);
	check(ctrl_recv(&b, &out) == 1 && out.type == CTRL_ERROR);
	check(!strcmp(out.text, "x"));

	/* EOF */
	close(sv[0]);
	check(ctrl_recv(&b, &out) == -1 && errno == 0);
	close(sv[1]);
}

int main(void)
{
	test_hello();
	test_welcome();
	test_ts();
	test_bye_error();
	test_conn();
	return 0;
}
