#include <ncurses.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <arpa/telnet.h>

/* telnet info */
typedef enum { TELNET_TEXT, TELNET_IAC, TELNET_DO, TELNET_DONT, TELNET_WILL, TELNET_WONT, TELNET_SUB, TELNET_SUBIAC } telnet_state_t;

#define TELNET_MAX_SUB (1024*8)
#define TELNET_MAX_ZMP_ARGS 32

#define TELNET_FLAG_ZMP (1<<0)
#define TELNET_FLAGS_DEFAULT 0

struct TELNET {
	telnet_state_t state;
	int sock;
	char sub_buf[TELNET_MAX_SUB];
	size_t sub_size;
	char flags;
} telnet;

/* ZMP setup */

typedef void (*zmp_cb_t)(int argc, char** argv);

struct ZMP {
	const char* name;
	zmp_cb_t cb;
	struct ZMP* next;
};
struct ZMP* zmp_list;

/* ZMP handlers */

static void zmp_check (int argc, char** argv);

/* terminal output parser */
typedef enum { TERM_ASCII, TERM_ESC, TERM_ESCRUN } term_state_t;

#define TERM_MAX_ESC 16

#define TERM_FLAG_ECHO (1<<0)
#define TERM_FLAG_GA (1<<1)
#define TERM_FLAGS_DEFAULT (TERM_FLAG_ECHO)

struct TERMINAL {
	term_state_t state;
	int esc_buf[TERM_MAX_ESC];
	size_t esc_cnt;
	char flags;
} terminal;

/* line buffer */
char user_line[1024];

/* windows */
static WINDOW* win_main;
static WINDOW* win_input;
static WINDOW* win_banner;

/* cleanup function */
static void cleanup (void) {
	/* cleanup curses */
	endwin();
}

/* force-send bytes to telnet server */
static void do_send (const char* bytes, size_t len) {
	int ret;
	while (len > 0) {
		ret = send(telnet.sock, bytes, len, 0);
		if (ret == -1) {
			if (ret != EAGAIN && ret != EINTR) {
				endwin();
				fprintf(stderr, "send() failed: %s\n", strerror(errno));
				exit(1);
			}
			continue;
		} else if (ret == 0) {
			endwin();
			printf("Disconnected.\n");
			exit(0);
		} else {
			bytes += ret;
			len -= ret;
		}
	}
}

/* force-send bytes to telnet server, escaping IAC */
static void do_send_esc (const char* bytes, size_t len) {
	size_t last = 0;
	size_t i;
	char esc[2] = { IAC, IAC };
	for (i = 0; i < len; ++i) {
		/* found an IAC?  dump text so far, send esc */
		if ((unsigned char)bytes[i] == IAC) {
			if (i > last)
				do_send(bytes + last, i - last);
			do_send(esc, 2);
			last = i + 1;
		}
	}
	/* send remainder */
	if (i > last)
		do_send(bytes + last, i - last);
}

/* send a telnet option */
static void telnet_send_opt(int type, int opt) {
	char bytes[3] = { IAC, type, opt };
	do_send(bytes, 3);
}

/* process user input */
static void on_key (int key) {
	size_t len = strlen(user_line);

	/* send */
	if (key == '\n' || key == '\r') {
		user_line[len] = '\n';
		/* send line to server */
		do_send_esc(user_line, len+1);
		/* echo output */
		if (terminal.flags & TERM_FLAG_ECHO)
			waddnstr(win_main, user_line, len+1);
		/* reset input */
		user_line[0] = 0;
	}

	/* backspace; remove last character */
	else if (key == KEY_BACKSPACE) {
		if (len > 0)
			user_line[len-1] = 0;
	}
	
	/* attempt to add */
	else if (isprint(key)) {
		/* only if we're not full */
		if (len <= sizeof(user_line) - 1) {
			user_line[len] = key;
			user_line[len+1] = 0;
		}
	}

	/* draw input */
	wclear(win_input);
	if (terminal.flags & TERM_FLAG_ECHO)
		mvwaddstr(win_input, 0, 0, user_line);
}

/* process text into virtual terminal */
static void on_text (const char* text, size_t len) {
	size_t i;
	for (i = 0; i < len; ++i) {
		switch (terminal.state) {
			case TERM_ASCII:
				/* begin escape sequence */
				if (text[i] == 27)
					terminal.state = TERM_ESC;
				/* just show it */
				else
					waddch(win_main, text[i]);
				break;
			case TERM_ESC:
				/* run of mod setting commands */
				if (text[i] == '[') {
					terminal.state = TERM_ESCRUN;
					terminal.esc_cnt = 0;
					terminal.esc_buf[0] = 0;
				}
				/* something else we don't support */
				else
					terminal.state = TERM_ASCII;
				break;
			case TERM_ESCRUN:
				/* number, add to option */
				if (isdigit(text[i])) {
					terminal.esc_buf[terminal.esc_cnt] *= 10;
					terminal.esc_buf[terminal.esc_cnt] += text[i] - '0';
				}
				/* semi-colon, go to next option */
				else if (text[i] == ';') {
					if (terminal.esc_cnt < TERM_MAX_ESC-1)
						terminal.esc_cnt++;
				}
				/* anything-else; perform option */
				else {
					terminal.state = TERM_ASCII;
				}
				break;
		}
	}
}

/* process ZMP chunk in subrequest */
static void on_zmp (const char* data, size_t len) {
	const char* argv[TELNET_MAX_ZMP_ARGS];
	size_t argc;

	/* initial arg */
	argc = 1;
	argv[0] = data;

	/* find additional args */
	const char* next = argv[0] + strlen(argv[0]);
	endwin();
	while (next - data != len) {
		/* fail on overflow */
		if (argc == TELNET_MAX_ZMP_ARGS)
			return;

		/* store it */
		argv[argc] = next + 1;
		next = argv[argc] + strlen(argv[argc]);
		++argc;
	}

	/* find the command */
	struct ZMP* zmp = zmp_list;
	while (zmp != NULL) {
		/* if match, execute and end */
		if (strcmp(zmp->name, argv[0]) == 0) {
			zmp->cb(argc, (char**)argv);
			break;
		}
		zmp = zmp->next;
	}
}

/* process telnet subrequest */
static void on_subreq (void) {
	/* must have at least one byte */
	if (telnet.sub_size == 0)
		return;

	switch (telnet.sub_buf[0]) {
		/* ZMP command */
		case 93:
			/* ZMP not turned on?  ignore */
			if ((telnet.flags & TELNET_FLAG_ZMP) == 0)
				return;

			/* sanity check */
			if (telnet.sub_size < 3 || !isalpha(telnet.sub_buf[1]) || telnet.sub_buf[telnet.sub_size-1] != 0)
				return;

			/* invoke ZMP */
			on_zmp(&telnet.sub_buf[1], telnet.sub_size - 1);
			break;
	}
}

/* process input from telnet */
static void on_input (const char* data, size_t len) {
	size_t i;
	for (i = 0; i < len; ++i) {
		switch (telnet.state) {
			case TELNET_TEXT:
				/* IAC */
				if ((unsigned char)data[i] == IAC)
					telnet.state = TELNET_IAC;
				/* text */
				else
					on_text(&data[i], 1);
				break;
			case TELNET_IAC:
				/* IAC IAC escape */
				if ((unsigned char)data[i] == IAC)
					on_text(&data[i], 1);
				/* DO/DONT/WILL/WONT */
				else if ((unsigned char)data[i] == DO)
					telnet.state = TELNET_DO;
				else if ((unsigned char)data[i] == DONT)
					telnet.state = TELNET_DONT;
				else if ((unsigned char)data[i] == WILL)
					telnet.state = TELNET_WILL;
				else if ((unsigned char)data[i] == WONT)
					telnet.state = TELNET_WONT;
				/* sub-request */
				else if ((unsigned char)data[i] == SB) {
					telnet.state = TELNET_SUB;
					telnet.sub_size = 0;
				}
				/* something else; error, just print it */
				else {
					char buf[64];
					snprintf(buf, sizeof(buf), "<IAC:%d>", (int)data[i]);
					waddstr(win_main, buf);
					telnet.state = TELNET_TEXT;
				}
				break;
			case TELNET_DO:
				telnet.state = TELNET_TEXT;
				break;
			case TELNET_DONT:
				telnet.state = TELNET_TEXT;
				break;
			case TELNET_WILL:
				/* turn off echo */
				if ((unsigned char)data[i] == TELOPT_ECHO) {
					terminal.flags &= ~TERM_FLAG_ECHO;
					telnet_send_opt(DO, TELOPT_ECHO);
				}
				/* enable ZMP */
				else if ((unsigned char)data[i] == 93) {
					telnet.flags |= TELNET_FLAG_ZMP;
					telnet_send_opt(DO, 93);
				}
				telnet.state = TELNET_TEXT;
				break;
			case TELNET_WONT:
				/* turn on echo */
				if ((unsigned char)data[i] == TELOPT_ECHO)
					terminal.flags |= TERM_FLAG_ECHO;
				telnet.state = TELNET_TEXT;
				break;
			case TELNET_SUB:
				/* IAC escape */
				if ((unsigned char)data[i] == IAC)
					telnet.state = TELNET_SUBIAC;
				/* overflow; error out and reset to TEXT mode */
				else if (telnet.sub_size == TELNET_MAX_SUB)
					telnet.state = TELNET_TEXT;
				/* another byte */
				else
					telnet.sub_buf[telnet.sub_size++] = data[i];
				break;
			case TELNET_SUBIAC:
				/* IAC IAC escape */
				if ((unsigned char)data[i] == IAC) {
					/* overflow; error out and reset to TEXT mode */
					if (telnet.sub_size == TELNET_MAX_SUB)
						telnet.state = TELNET_TEXT;
					/* add the IAC byte */
					else
						telnet.sub_buf[telnet.sub_size++] = data[i];
				}
				/* end, process */
				else if ((unsigned char)data[i] == SE) {
					telnet.state = TELNET_TEXT;
					on_subreq();
				}
				/* something else; error */
				else
					telnet.state = TELNET_TEXT;
				break;
		}
	}
	char buf[64];
	snprintf(buf, sizeof(buf), "Input: %d %d", len, telnet.flags);
	wclear(win_banner);
	mvwaddstr(win_banner, 0, 0, buf);
}

/* register a ZMP handler */
static void zmp_register (const char* name, zmp_cb_t cb) {
	struct ZMP* zmp;

	zmp = (struct ZMP*)malloc(sizeof(struct ZMP));
	if (zmp == NULL) {
		endwin();
		fprintf(stderr, "malloc() failed: %s\n", strerror(errno));
		exit(1);
	}
	zmp->next = zmp_list;
	zmp->name = name;
	zmp->cb = cb;
	zmp_list = zmp;
}

/* check if a given command/package is supported */
static int zmp_do_check (const char* name) {
	size_t len = strlen(name);

	/* empty name?  no, not supported... */
	if (len == 0)
		return 0;

	/* package check */
	if (name[len-1] == '.') {
		struct ZMP* zmp = zmp_list;
		while (zmp != NULL) {
			if (strncmp(zmp->name, name, len) == 0)
				return 1;
			zmp = zmp->next;
		}
	}
	/* command check */
	else {
		struct ZMP* zmp = zmp_list;
		while (zmp != NULL) {
			if (strcmp(zmp->name, name) == 0)
				return 1;
			zmp = zmp->next;
		}
	}

	/* no match found */
	return 0;
}

/* send a ZMP command */
static void zmp_send (int argc, char** argv) {
	/* no args... ignore */
	if (argc < 1)
		return;

	/* send header */
	char hdr[3] = { IAC, SB, 93 };
	do_send(hdr, 3);

	/* send args */
	size_t i;
	for (i = 0; i < argc; ++i) {
		/* remember to include NUL byte */
		do_send_esc(argv[i], strlen(argv[i]) + 1);
	}

	/* send footer */
	char ftr[2] = { IAC, SE };
	do_send(ftr, 2);
}

int main (int argc, char** argv) {
	/* cleanup on any failure */
	atexit(cleanup);

	/* set terminal defaults */
	terminal.state = TERM_ASCII;
	terminal.flags = TERM_FLAGS_DEFAULT;

	/* set telnet defaults */
	telnet.state = TELNET_TEXT;
	telnet.flags = TELNET_FLAGS_DEFAULT;

	/* register ZMP handlers */
	zmp_list = NULL;
	zmp_register("zmp.check", zmp_check);

	/* connect to server */
	struct sockaddr_in sockaddr;
	socklen_t socklen;

	telnet.sock = socket(AF_INET, SOCK_STREAM, 0);
	if (telnet.sock == -1) {
		fprintf(stderr, "socket() failed: %s\n", strerror(errno));
		return 1;
	}

	memset(&sockaddr, 0, sizeof(sockaddr));
	socklen = sizeof(sockaddr);

	if (bind(telnet.sock, (struct sockaddr*)&sockaddr, socklen) == -1) {
		fprintf(stderr, "bind() failed: %s\n", strerror(errno));
		return 1;
	}

	memset(&sockaddr, 0, sizeof(sockaddr));
	socklen = sizeof(sockaddr);
	sockaddr.sin_family = AF_INET;
	sockaddr.sin_port = htons(4545);
	inet_pton(AF_INET, "127.0.0.1", &sockaddr.sin_addr);

	if (connect(telnet.sock, (struct sockaddr*)&sockaddr, socklen) == -1) {
		fprintf(stderr, "connect() failed: %s\n", strerror(errno));
		return 1;
	}

	/* configure curses */
	initscr();
	nonl();
	cbreak();
	noecho();

	win_main = newwin(22, 80, 0, 0);
	win_banner = newwin(1, 80, 22, 0);
	win_input = newwin(1, 80, 23, 0);

	idlok(win_main, TRUE);
	scrollok(win_main, TRUE);

	nodelay(win_input, TRUE);
	keypad(win_input, TRUE);

	/* initialize user line buffer */
	user_line[0] = 0;

	/* setup poll info */
	struct pollfd fds[2];
	fds[0].fd = 1;
	fds[0].events = POLLIN;
	fds[1].fd = telnet.sock;
	fds[1].events = POLLIN;

	/* main loop */
	while (true) {
		/* poll sockets */
		if (poll(fds, 2, -1) == -1) {
			if (errno != EAGAIN && errno != EINTR) {
				endwin();
				fprintf(stderr, "poll() failed: %s\n", strerror(errno));
				return 1;
			}
		}

		/* input? */
		if (fds[0].revents & POLLIN) {
			int key;
			while ((key = wgetch(win_input)) != ERR)
				on_key(key);
		}

		/* telnet data */
		if (fds[1].revents & POLLIN) {
			char buffer[2048];
			int ret = recv(telnet.sock, buffer, sizeof(buffer), 0);
			if (ret == -1) {
				if (errno != EAGAIN && errno != EINTR) {
					endwin();
					fprintf(stderr, "recv() failed: %s\n", strerror(errno));
					return 1;
				}
			} else if (ret == 0) {
				endwin();
				printf("Disconnected.\n");
				return 0;
			} else {
				on_input(buffer, ret);
			}
		}

		/* flush output */
		wnoutrefresh(win_main);
		wnoutrefresh(win_banner);
		wnoutrefresh(win_input);
		doupdate();
	}

	return 0;
}

/*** ZMP handlers ***/

static void zmp_check (int argc, char** argv) {
	if (argc != 2) return;
	if (zmp_do_check(argv[1])) {
		char* sargv[2] = { "zmp.support", argv[1] };
		zmp_send(2, sargv);
	} else {
		char* sargv[2] = { "zmp.no-support", argv[1] };
		zmp_send(2, sargv);
	}
}
