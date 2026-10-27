/* Fake LD2451 on a pseudo-terminal, for testing cap-radar without hardware.
 * Never deployed.
 *
 *   fake-ld2451 <link> <send_s>
 *
 * Makes a pty, points the symlink <link> at its slave and writes one item every
 * 100 ms for send_s seconds, cycling through: junk + empty frame, empty frame,
 * bad tail, bad length, a 4-byte payload (synthetic: the real target layout is
 * unverified), and an empty frame split across two writes. Then it removes the
 * link and exits. The slave stays open here so cap-radar never sees a hangup.
 *
 * Exit: 0, 64 usage, 70 on any failure.
 */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ITEM_NS 100000000LL
#define SEC_NS  1000000000LL

static const unsigned char EMPTY[]    = { 0xF4, 0xF3, 0xF2, 0xF1, 0x00, 0x00, 0xF8, 0xF7, 0xF6, 0xF5 };
static const unsigned char JUNK[]     = { 0x00, 0xF4, 0xF3, 0x55, 0xF4 };
static const unsigned char BAD_TAIL[] = { 0xF4, 0xF3, 0xF2, 0xF1, 0x00, 0x00, 0xF8, 0xF7, 0xF6, 0x00 };
static const unsigned char BAD_LEN[]  = { 0xF4, 0xF3, 0xF2, 0xF1, 0xFF, 0xFF };
static const unsigned char PAYLOAD[]  = { 0xF4, 0xF3, 0xF2, 0xF1, 0x04, 0x00, 0x01, 0x02, 0x03, 0x04,
					  0xF8, 0xF7, 0xF6, 0xF5 };

static int write_all(int fd, const unsigned char *b, size_t n)
{
	ssize_t w;

	while (n > 0) {
		w = write(fd, b, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		b += w;
		n -= (size_t)w;
	}
	return 0;
}

static int write_item(int fd, unsigned i)
{
	switch (i % 6) {
	case 0:
		return write_all(fd, JUNK, sizeof(JUNK)) < 0 ? -1 : write_all(fd, EMPTY, sizeof(EMPTY));
	case 2:
		return write_all(fd, BAD_TAIL, sizeof(BAD_TAIL));
	case 3:
		return write_all(fd, BAD_LEN, sizeof(BAD_LEN));
	case 4:
		return write_all(fd, PAYLOAD, sizeof(PAYLOAD));
	case 5:
		if (write_all(fd, EMPTY, 3) < 0)
			return -1;
		if (usleep(20000) < 0 && errno != EINTR)
			return -1;
		return write_all(fd, EMPTY + 3, sizeof(EMPTY) - 3);
	default:
		return write_all(fd, EMPTY, sizeof(EMPTY));
	}
}

int main(int argc, char **argv)
{
	struct termios t;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = ITEM_NS };
	const char *slave_name;
	char *end;
	double secs;
	unsigned i, items;
	int master = -1, slave = -1, linked = 0, rc = EX_SOFTWARE;

	log_init("fake-ld2451");
	if (argc != 3) {
		log_msg(LOG_E, "usage args=\"<link> <send_s>\"");
		return EX_USAGE;
	}
	errno = 0;
	secs = strtod(argv[2], &end);
	if (errno || end == argv[2] || *end != '\0' || secs < 0 || secs > 3600) {
		log_msg(LOG_E, "usage args=\"<link> <send_s>\"");
		return EX_USAGE;
	}
	items = (unsigned)(secs * SEC_NS / ITEM_NS);

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
		log_msg(LOG_E, "openpt_failed err=%s", strerror(errno));
		goto out;
	}
	slave_name = ptsname(master);
	if (!slave_name) {
		log_msg(LOG_E, "ptsname_failed err=%s", strerror(errno));
		goto out;
	}
	/* Raw, so the line discipline neither buffers lines nor echoes back. */
	slave = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (slave < 0 || tcgetattr(slave, &t) < 0) {
		log_msg(LOG_E, "slave_failed err=%s", strerror(errno));
		goto out;
	}
	cfmakeraw(&t);
	if (tcsetattr(slave, TCSANOW, &t) < 0) {
		log_msg(LOG_E, "slave_failed err=%s", strerror(errno));
		goto out;
	}
	if (symlink(slave_name, argv[1]) < 0) {
		log_msg(LOG_E, "symlink_failed link=%s err=%s", argv[1], strerror(errno));
		goto out;
	}
	linked = 1;

	for (i = 0; i < items; i++) {
		if (write_item(master, i) < 0) {
			log_msg(LOG_E, "write_failed err=%s", strerror(errno));
			goto out;
		}
		if (nanosleep(&ts, NULL) < 0 && errno != EINTR) {
			log_msg(LOG_E, "sleep_failed err=%s", strerror(errno));
			goto out;
		}
	}
	log_msg(LOG_I, "done items=%u", items);
	rc = EX_OK;

out:
	if (linked && unlink(argv[1]) < 0)
		log_msg(LOG_W, "unlink_failed err=%s", strerror(errno));
	if (slave >= 0 && close(slave) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (master >= 0 && close(master) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	return rc;
}
