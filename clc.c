/**
 * Command-Line Client
 * Sean Middleditch <elanthis@sourcemud.org>
 * THIS CODE IS PUBLIC DOMAIN
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <arpa/telnet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <ncurses.h>

/* telnet protocol */
typedef enum { TELNET_TEXT, TELNET_IAC, TELNET_DO, TELNET_DONT, TELNET_WILL, TELNET_WONT, TELNET_SUB, TELNET_SUBIAC } telnet_state_t;

#define TELNET_MAX_SUB (1024*8)
#define TELNET_MAX_ZMP_ARGS 32

#define TELNET_FLAG_ZMP (1<<0)
#define TELNET_FLAG_NAWS (1<<1)
#define TELNET_FLAGS_DEFAULT 0

struct TELNET {
	telnet_state_t state;
	char sub_buf[TELNET_MAX_SUB];
	size_t sub_size;
	char flags;
} telnet;

static void telnet_on_line(const char* line, size_t len);
static void telnet_on_recv(const char* bytes, size_t len);
static void telnet_on_resize(int w, int h);

static void telnet_send_cmd(int cmd);
static void telnet_send_opt(int type, int opt);
static void telnet_send_esc(const char* bytes, size_t len);
static void telnet_send_zmp(const char* cmd, ...);
static void telnet_do_subreq(void);
static void telnet_do_zmp(char* bytes, size_t len);

/* websock protocol */
#define WEBSOCK_MAX_MSG 2048

static struct WEBSOCK {
	char msg[WEBSOCK_MAX_MSG];
	size_t msg_size;
} websock;

static void websock_on_line(const char* line, size_t len);
static void websock_on_recv(const char* bytes, size_t len);
static void websock_on_resize(int w, int h);

/* protocol handler */
typedef enum { PROTOCOL_TELNET, PROTOCOL_WEBSOCK } protocol_type_t;

static struct PROTOCOL {
	protocol_type_t type;

	void (*on_line)(const char* line, size_t len);
	void (*on_recv)(const char* bytes, size_t len);
	void (*on_resize)(int w, int h);
} protocol;

static void protocol_init(protocol_type_t type);

/* terminal processing */
typedef enum { TERM_ASCII, TERM_ESC, TERM_ESCRUN } term_state_t;

#define TERM_MAX_ESC 16
#define TERM_COLOR_DEFAULT 9

#define TERM_FLAG_ECHO (1<<0)
#define TERM_FLAGS_DEFAULT (TERM_FLAG_ECHO)

static struct TERMINAL {
	term_state_t state;
	int esc_buf[TERM_MAX_ESC];
	size_t esc_cnt;
	char flags;
	int color;
} terminal;

/* edit buffer */

#define EDITBUF_MAX 1024

static struct EDITBUF {
	char buf[EDITBUF_MAX];
	size_t size;
	size_t pos;
} editbuf;

static void editbuf_set(const char*);
static void editbuf_insert(int);
static void editbuf_bs();
static void editbuf_del();
static void editbuf_curleft();
static void editbuf_curright();
static void editbuf_display();
static void editbuf_home();
static void editbuf_end();

/* running flag; when 0, exit main loop */
static int running = 1;

/* banner buffer */
static char banner[1024];
static int autobanner = 1;

/* windows */
static WINDOW* win_main = 0;
static WINDOW* win_input = 0;
static WINDOW* win_banner = 0;

/* last interrupt */
volatile int have_sigwinch = 0;
volatile int have_sigint = 0;

/* server socket */
const char* host = NULL;
const char* port = NULL;
static int sock;
static size_t sent_bytes = 0;
static size_t recv_bytes = 0;

/* core functions */
static void on_text_plain (const char* text, size_t len);
static void on_text_ansi (const char* text, size_t len);

/* ======= CORE ======= */

/* cleanup function */
static void cleanup (void) {
	/* cleanup curses */
	endwin();
}

/* handle signals */
static void handle_signal (int sig) {
	switch (sig) {
		case SIGWINCH:
			have_sigwinch = 1;
			break;
		case SIGINT:
			have_sigint = 1;
			break;
	}
}

/* set the edit buffer to contain the given text */
static void editbuf_set (const char* text) {
	snprintf(editbuf.buf, EDITBUF_MAX, "%s", text);
	editbuf.pos = editbuf.size = strlen(text);
}

/* insert/replace a character at the current location */
static void editbuf_insert (int ch) {
	/* ensure we have space */
	if (editbuf.size == EDITBUF_MAX)
		return;

	/* if we're at the end, just append the character */
	if (editbuf.pos == editbuf.size) {
		editbuf.buf[editbuf.pos] = ch;
		editbuf.pos = ++editbuf.size;
		return;
	}

	/* move data, insert character */
	memmove(editbuf.buf + editbuf.pos + 1, editbuf.buf + editbuf.pos, editbuf.size - editbuf.pos);
	editbuf.buf[editbuf.pos] = ch;
	++editbuf.pos;
	++editbuf.size;
}

/* delete character one position to the left */
static void editbuf_bs () {
	/* if we're at the beginning, do nothing */
	if (editbuf.pos == 0)
		return;

	/* if we're at the end, just decrement pos and size */
	if (editbuf.pos == editbuf.size) {
		editbuf.pos = --editbuf.size;
		return;
	}

	/* chop out the previous character */
	memmove(editbuf.buf + editbuf.pos - 1, editbuf.buf + editbuf.pos, editbuf.size - editbuf.pos);
	--editbuf.pos;
	--editbuf.size;
}

/* delete word under cursor */
static void editbuf_del () {
	/* if we're at the end, do nothing */
	if (editbuf.pos == editbuf.size)
		return;

	/* if we're at the end, just decrement pos and size */
	if (editbuf.pos == editbuf.size - 1) {
		--editbuf.pos;
		--editbuf.size;
		return;
	}

	/* chop out the current character */
	memmove(editbuf.buf + editbuf.pos, editbuf.buf + editbuf.pos + 1, editbuf.size - editbuf.pos - 1);
	--editbuf.size;
}

/* move to home position */
static void editbuf_home () {
	editbuf.pos = 0;
}

/* move to end position */
static void editbuf_end () {
	editbuf.pos = editbuf.size;
}

/* move cursor left */
static void editbuf_curleft () {
	if (editbuf.pos > 0)
		--editbuf.pos;
}

/* move cursor right */
static void editbuf_curright () {
	if (editbuf.pos < editbuf.size)
		++editbuf.pos;
}

/* display the edit buffer in win_input */
static void editbuf_display () {
	wclear(win_input);
	if (terminal.flags & TERM_FLAG_ECHO) {
		mvwaddnstr(win_input, 0, 0, editbuf.buf, editbuf.size);
	} else {
		wmove(win_input, 0, 0);
		size_t i;
		for (i = 0; i != editbuf.size; ++i)
			waddch(win_input, '*');
	}
	wmove(win_input, 0, editbuf.pos);
}

/* paint banner */
static void paint_banner (void) {
	/* if autobanner is on, build our banner buffer */
	if (autobanner) {
		snprintf(banner, sizeof(banner), "%s:%s - (%s)", host, port, sock == -1 ? "disconnected" : "connected");
	}

	/* paint */
	wclear(win_banner);
	mvwaddstr(win_banner, 0, 0, banner);
}

/* redraw all windows */
static void redraw_display (void) {
	/* get size */
	struct winsize ws;
	if (ioctl(0, TIOCGWINSZ, &ws))
		return;

	/* resize */
	resizeterm(ws.ws_row, ws.ws_col);
	mvwin(win_input, LINES-1, 0);
	wresize(win_input, 1, COLS);
	mvwin(win_banner, LINES-2, 0);
	wresize(win_banner, 1, COLS);
	wresize(win_main, LINES-2, COLS);

	/* update */
	paint_banner();

	/* update size on protocol */
	if (running) {
		unsigned short w = htons(COLS), h = htons(LINES);
		protocol.on_resize(w, h);
	}

	/* input display */
	editbuf_display();

	/* refresh */
	wnoutrefresh(win_main);
	wnoutrefresh(win_banner);
	wnoutrefresh(win_input);
	doupdate();
}

/* force-send bytes to server */
static void do_send (const char* bytes, size_t len) {
	int ret;

	/* keep sending bytes until they're all sent */
	while (len > 0) {
		ret = send(sock, bytes, len, 0);
		if (ret == -1) {
			if (ret != EAGAIN && ret != EINTR) {
				endwin();
				fprintf(stderr, "send() failed: %s\n", strerror(errno));
				exit(1);
			}
			continue;
		} else if (ret == 0) {
			endwin();
			printf("Disconnected from server\n");
			exit(0);
		} else {
			sent_bytes += ret;
			bytes += ret;
			len -= ret;
		}
	}
}

/* process user input */
static void on_key (int key) {
	/* special keys */
	if (key >= KEY_MIN && key <= KEY_MAX) {
		/* send */
		if (key == KEY_ENTER) {
			/* send line to server */
			protocol.on_line(editbuf.buf, editbuf.size);
			/* reset input */
			editbuf_set("");
		}

		/* backspace/delete */
		else if (key == KEY_BACKSPACE) {
			editbuf_bs();
		}
		else if (key == KEY_DC) {
			editbuf_del();
		}

		/* move cursor left/right */
		else if (key == KEY_LEFT) {
			editbuf_curleft();
		}
		else if (key == KEY_RIGHT) {
			editbuf_curright();
		}
		else if (key == KEY_HOME) {
			editbuf_home();
		}
		else if (key == KEY_END) {
			editbuf_end();
		}

	/* regular text */
	} else {
		/* send */
		if (key == '\n' || key == '\r') {
			/* send line to server */
			protocol.on_line(editbuf.buf, editbuf.size);
			/* reset input */
			editbuf_set("");

		/* add key to edit buffer */
		} else {
			editbuf_insert(key);
		}
	}

	/* draw input */
	editbuf_display();
}

/* perform a terminal escape */
static void on_term_esc(char cmd) {
	size_t i;

	switch (cmd) {
		/* mode set: */
		case 'm':
			for (i = 0; i < terminal.esc_cnt; ++i) {
				/* default */
				if (terminal.esc_buf[i] == 0) {
					terminal.color = TERM_COLOR_DEFAULT;
					wattron(win_main, COLOR_PAIR(terminal.color));
				}
				/* color */
				else if (terminal.esc_buf[i] >= 31 && terminal.esc_buf[i] <= 37) {
					terminal.color = terminal.esc_buf[i] - 30;
					wattron(win_main, COLOR_PAIR(terminal.color));
				}
			}
			break;
		/* clear */
		case 'J':
			/* clear whole screen */
			if (terminal.esc_buf[0] == 2)
				wclear(win_main);
			break;
	}
}

/* process text into virtual terminal, no ANSI */
static void on_text_plain (const char* text, size_t len) {
	size_t i;
	for (i = 0; i < len; ++i) {
		/* don't send ESC codes, for safety */
		if (text[i] != 27 && text[i] != '\r')
			waddch(win_main, text[i]);
	}
}

/* process text into virtual terminal */
static void on_text_ansi (const char* text, size_t len) {
	size_t i;
	for (i = 0; i < len; ++i) {
		switch (terminal.state) {
			case TERM_ASCII:
				/* begin escape sequence */
				if (text[i] == 27)
					terminal.state = TERM_ESC;
				/* just show it */
				else if (text[i] != '\r')
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
					if (terminal.esc_cnt == 0)
						terminal.esc_cnt = 1;
					terminal.esc_buf[terminal.esc_cnt-1] *= 10;
					terminal.esc_buf[terminal.esc_cnt-1] += text[i] - '0';
				}
				/* semi-colon, go to next option */
				else if (text[i] == ';') {
					if (terminal.esc_cnt < TERM_MAX_ESC) {
						terminal.esc_cnt++;
						terminal.esc_buf[terminal.esc_cnt-1] = 0;
					}
				}
				/* anything-else; perform option */
				else {
					on_term_esc(text[i]);
					terminal.state = TERM_ASCII;
				}
				break;
		}
	}
}

/* initialize the protocol */
static void protocol_init (protocol_type_t type) {
	protocol.type = type;

	if (type == PROTOCOL_TELNET) {
		protocol.on_line = telnet_on_line;
		protocol.on_recv = telnet_on_recv;
		protocol.on_resize = telnet_on_resize;
	} else if (type == PROTOCOL_WEBSOCK) {
		protocol.on_line = websock_on_line;
		protocol.on_recv = websock_on_recv;
		protocol.on_resize = websock_on_resize;
	}
}

/* attempt to connect to the requested hostname on the request port */
static int do_connect (const char* host, const char* port) {
	struct addrinfo hints;
	struct addrinfo *results;
	struct addrinfo *ai;
	int ret;
	int sock;

	/* lookup host */
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((ret = getaddrinfo(host, port, &hints, &results)) != 0) {
		fprintf(stderr, "Host lookup failed: %s\n", gai_strerror(ret));
		return -1;
	}

	/* loop through hosts, trying to connect */
	for (ai = results; ai != NULL; ai = ai->ai_next) {
		/* create socket */
		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock == -1) {
			fprintf(stderr, "socket() failed: %s\n", strerror(errno));
			exit(1);
		}

		/* connect */
		if (connect(sock, ai->ai_addr, ai->ai_addrlen) != -1) {
			freeaddrinfo(results);
			return sock;
		}

		/* release socket */
		shutdown(sock, SHUT_RDWR);
	}

	/* could not connect */
	freeaddrinfo(results);
	return -1;
}

int main (int argc, char** argv) {
	protocol_type_t type = PROTOCOL_TELNET;
	const char* default_port = "23";
	struct sigaction sa;
	int i;

	/* process command line args
	 *
	 * usage:
	 *  client [options] <host> [<port>]
	 * 
	 * options:
	 *  -t  use telnet (port 23)
	 *  -w  use websock (port 4747)
	 */
	for (i = 1; i < argc; ++i) {
		/* set protocol to telnet */
		if (strcmp(argv[i], "-t") == 0) {
			type = PROTOCOL_TELNET;
			default_port = "23";
			continue;
		}

		/* set protocol to websock */
		if (strcmp(argv[i], "-w") == 0) {
			type = PROTOCOL_WEBSOCK;
			default_port = "4747";
			continue;
		}

		/* help */
		if (strcmp(argv[i], "-h") == 0) {
			printf(
				"CLC by Sean Middleditch <elanthis@sourcemud.org>\n"
				"This program has been released into the PUBLIC DOMAIN.\n\n"
				"Usage:\n"
				"  clc [-w] [-t] [-h] <host> [<port>]\n\n"
				"Options:\n"
				"  -w   WebSock protocol\n"
				"  -t   TELNET protocol\n"
				"  -h   display help\n\n"
				"Notes:\n"
				"  The TELNET protocol is the default if none is specified.\n"
				"  WebSock connects to port 4747 by default, and TELNET\n"
				"  connects to port 23 by default.  CLC is 8-bit clean by\n"
				"  default in TELNET mode, and supports UTF-8 if both the\n"
				"  terminal and server support it.\n"
			);
			return 0;
		}

		/* other unknown option */
		if (argv[i][0] == '-') {
			fprintf(stderr, "Unknown option %s.\nUse -h to see available options.\n", argv[i]);
			exit(1);
		}

		/* if host is unset, this is the host */
		if (host == NULL) {
			host = argv[i];
		/* otherwise, it's a port */
		} else {
			port = argv[i];
		}
	}

	/* ensure we have a host */
	if (host == NULL) {
		fprintf(stderr, "No host was given.\nUse -h to see command format.\n");
		exit(1);
	}

	/* set default port if none was given */
	if (port == NULL)
		port = default_port;

	/* cleanup on any failure */
	atexit(cleanup);

	/* set terminal defaults */
	memset(&terminal, 0, sizeof(struct TERMINAL));
	terminal.state = TERM_ASCII;
	terminal.flags = TERM_FLAGS_DEFAULT;
	terminal.color = TERM_COLOR_DEFAULT;

	/* initial websock and telnet handlers */
	memset(&telnet, 0, sizeof(struct TELNET));
	memset(&websock, 0, sizeof(struct WEBSOCK));

	/* configure protocol */
	protocol_init(type);

	/* connect to server */
	sock = do_connect(host, port);
	if (sock == -1) {
		fprintf(stderr, "Failed to connect to %s:%s\n", host, port);
		exit(1);
	}
	printf("Connected to %s:%s\n", host, port);

	/* set initial banner */
	snprintf(banner, sizeof(banner), "CLC - %s:%s (connected)", host, port);

	/* configure curses */
	initscr();
	start_color();
	nonl();
	cbreak();
	noecho();

	win_main = newwin(LINES-2, COLS, 0, 0);
	win_banner = newwin(1, COLS, LINES-2, 0);
	win_input = newwin(1, COLS, LINES-1, 0);

	idlok(win_main, TRUE);
	scrollok(win_main, TRUE);

	nodelay(win_input, FALSE);
	keypad(win_input, TRUE);

	use_default_colors();

	init_pair(COLOR_RED, COLOR_RED, -1);
	init_pair(COLOR_BLUE, COLOR_BLUE, -1);
	init_pair(COLOR_GREEN, COLOR_GREEN, -1);
	init_pair(COLOR_CYAN, COLOR_CYAN, -1);
	init_pair(COLOR_MAGENTA, COLOR_MAGENTA, -1);
	init_pair(COLOR_YELLOW, COLOR_YELLOW, -1);
	init_pair(COLOR_WHITE, COLOR_WHITE, -1);

	init_pair(TERM_COLOR_DEFAULT, -1, -1);
	wbkgd(win_main, COLOR_PAIR(TERM_COLOR_DEFAULT));
	wclear(win_main);
	init_pair(10, COLOR_WHITE, COLOR_BLUE);
	wbkgd(win_banner, COLOR_PAIR(10));
	wclear(win_banner);
	init_pair(11, -1, -1);
	wbkgd(win_input, COLOR_PAIR(11));
	wclear(win_input);

	redraw_display();

	/* set signal handlers */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGWINCH, &sa, NULL);

	/* initial edit buffer */
	memset(&editbuf, 0, sizeof(struct EDITBUF));

	/* setup poll info */
	struct pollfd fds[2];
	fds[0].fd = 1;
	fds[0].events = POLLIN;
	fds[1].fd = sock;
	fds[1].events = POLLIN;

	/* main loop */
	while (running) {
		/* poll sockets */
		if (poll(fds, 2, -1) == -1) {
			if (errno != EAGAIN && errno != EINTR) {
				endwin();
				fprintf(stderr, "poll() failed: %s\n", strerror(errno));
				return 1;
			}
		}

		/* resize event? */
		if (have_sigwinch) {
			have_sigwinch = 0;
			redraw_display();
		}

		/* escape? */
		if (have_sigint) {
			exit(0);
		}

		/* input? */
		if (fds[0].revents & POLLIN) {
			int key = wgetch(win_input);
			if (key != ERR)
				on_key(key);
		}

		/* websock data */
		if (fds[1].revents & POLLIN) {
			char buffer[2048];
			int ret = recv(sock, buffer, sizeof(buffer), 0);
			if (ret == -1) {
				if (errno != EAGAIN && errno != EINTR) {
					endwin();
					fprintf(stderr, "recv() failed: %s\n", strerror(errno));
					return 1;
				}
			} else if (ret == 0) {
				running = 0;
			} else {
				recv_bytes += ret;
				protocol.on_recv(buffer, ret);
			}
		}

		/* flush output */
		paint_banner();
		wnoutrefresh(win_main);
		wnoutrefresh(win_banner);
		wnoutrefresh(win_input);
		doupdate();
	}

	/* final display, pause */
	sock = -1;
	autobanner = 1;
	paint_banner();
	wnoutrefresh(win_banner);
	doupdate();
	wgetch(win_input);

	/* clean up */
	endwin();
	printf("Disconnected.\n");

	return 0;
}

/* ======= WEBSOCK ======= */

/* send a command line to the server */
static void websock_on_line (const char* line, size_t len) {
	char cmd = '=';
	char nul = 0;

	/* send =(line)NUL */
	do_send(&cmd, 1);
	do_send(line, len);
	do_send(&nul, 1);
}

/* process input from websock */
static void websock_on_recv(const char* data, size_t len) {
	size_t i;
	for (i = 0; i < len; ++i) {
		if (data[i] != 0) {
			/* don't allow overflow */
			if (websock.msg_size == WEBSOCK_MAX_MSG) {
				/* ignore FIXME */
			} else {
				websock.msg[websock.msg_size++] = data[i];
			}
		} else {
			/* process the message */
			switch (websock.msg[0]) {
				/* text */
				case '"':
					on_text_plain(&websock.msg[1], websock.msg_size - 1);
					break;
				/* prompt */
				case '>':
					autobanner = 0;
					snprintf(banner, sizeof(banner), "%.*s", (int)websock.msg_size - 1, &websock.msg[1]);
					paint_banner();
					break;
				/* clear screen */
				case 'C':
					wclear(win_main);
					break;
				/* password mode */
				case 'p':
					/* data must be 1 for on, 0 for off, else no effect */
					if (websock.msg_size == 2 && websock.msg[1] == '1')
						terminal.flags &= ~TERM_FLAG_ECHO;
					else if (websock.msg_size == 2 && websock.msg[1] == '0')
						terminal.flags |= TERM_FLAG_ECHO;
					break;
			}

			/* reset buffer */
			websock.msg[0] = 0;
			websock.msg_size = 0;
		}
	}
}

/* websock has no sizing commands; ignore */
static void websock_on_resize (int w, int h) {
	/* do nothing */
}

/* ======= TELNET ======= */

/* send a line to the server */
static void telnet_on_line (const char* line, size_t len) {
	/* use zmp.input if ZMP is enabled */
	if ((telnet.flags & TELNET_FLAG_ZMP) != 0) {
		/* we need a buffer... for the NUL byte. */
		char buf[EDITBUF_MAX+1];
		snprintf(buf, sizeof(buf), "%.*s", (int)len, line);
		telnet_send_zmp("zmp.input", buf, NULL);

	/* regular way of sending line */
	} else {
		/* send with proper newline */
		char nl[] = { '\n', '\r' };
		telnet_send_esc(line, len);
		telnet_send_esc(nl, sizeof(nl));
	}

	/* echo output */
	if (terminal.flags & TERM_FLAG_ECHO) {
		wattron(win_main, COLOR_PAIR(COLOR_YELLOW));
		on_text_plain(line, len);
		on_text_plain("\n", 1);
		wattron(win_main, COLOR_PAIR(terminal.color));
	}
}

/* process input from telnet */
static void telnet_on_recv (const char* data, size_t len) {
	size_t i;
	for (i = 0; i < len; ++i) {
		switch (telnet.state) {
			case TELNET_TEXT:
				/* IAC */
				if ((unsigned char)data[i] == IAC)
					telnet.state = TELNET_IAC;
				/* text */
				else
					on_text_ansi(&data[i], 1);
				break;
			case TELNET_IAC:
				/* IAC IAC escape */
				if ((unsigned char)data[i] == IAC)
					on_text_ansi(&data[i], 1);
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
					on_text_plain(buf, strlen(buf));
					telnet.state = TELNET_TEXT;
				}
				break;
			case TELNET_DO:
				switch (data[i]) {
					/* enable NAWS support */
					case TELOPT_NAWS:
					{
						telnet.flags |= TELNET_FLAG_NAWS;
						telnet_send_opt(WILL, TELOPT_NAWS);
						unsigned short w = htons(COLS), h = htons(LINES);
						protocol.on_resize(w, h);
						break;
					}
				}
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
				if ((unsigned char)data[i] == TELOPT_ECHO) {
					terminal.flags |= TERM_FLAG_ECHO;
					telnet_send_opt(DONT, TELOPT_ECHO);
				}
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
					telnet_do_subreq();
				}
				/* something else; error */
				else
					telnet.state = TELNET_TEXT;
				break;
		}
	}
}

/* send NAWS update */
static void telnet_on_resize (int w, int h) {
	/* send NAWS if enabled */
	if (telnet.flags & TELNET_FLAG_NAWS) {
		telnet_send_opt(SB, TELOPT_NAWS);
		telnet_send_esc((char*)&w, 2);
		telnet_send_esc((char*)&h, 2);
		telnet_send_cmd(SE);
	}
}

/* force-send bytes to telnet server, escaping IAC */
static void telnet_send_esc (const char* bytes, size_t len) {
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

/* send a telnet cmd */
static void telnet_send_cmd(int cmd) {
	char bytes[2] = { IAC, cmd };
	do_send(bytes, 2);
}

/* send a telnet option */
static void telnet_send_opt(int type, int opt) {
	char bytes[3] = { IAC, type, opt };
	do_send(bytes, 3);
}

/* send a ZMP command */
static void telnet_send_zmp(const char* cmd, ...) {
	va_list va;
	const char* str;

	/* IAC SE ZMP, command (including NUL byte) */
	telnet_send_opt(SB, 93);
	telnet_send_esc(cmd, strlen(cmd) + 1);
	
	/* send arguments (including NUL byte after each) */
	va_start(va, cmd);
	while ((str = va_arg(va, const char*)) != NULL)
		telnet_send_esc(str, strlen(str) + 1);
	va_end(va);

	/* IAC SE */
	telnet_send_cmd(SE);
}

/* process telnet subrequest */
static void telnet_do_subreq (void) {
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
			telnet_do_zmp(&telnet.sub_buf[1], telnet.sub_size - 1);
			break;
	}
}

/* do ZMP */
static void telnet_do_zmp (char* bytes, size_t len) {
	const size_t MAX_ARGS = 32;
	char* args[MAX_ARGS];
	char* c = bytes;
	size_t i;

	/* parse args */
	for (i = 0; i < MAX_ARGS && c != bytes + len + 1; ++i) {
		args[i] = c;
		c += strlen(c) + 1;
	}

	/* deal with command */

	/* zmp.ping - requests a time result */
	if (strcmp(args[0], "zmp.ping") == 0) {
		char buf[48];
		time_t t;
		time(&t);
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime(&t));

		telnet_send_zmp("zmp.time", buf, NULL);

	/* zmp.time - just reports time (response to zmp.ping) */
	} else if (strcmp(args[0], "zmp.time") == 0) {
		/* do nothing */

	/* zmp.ident - identifies server software */
	} else if (strcmp(args[0], "zmp.ident") == 0) {
		/* do nothing */

	/* zmp.check - asks if pkg/cmd exists, return zmp.support
	 * or zmp.no-support */
	} else if (strcmp(args[0], "zmp.check") == 0) {

	/* zmp.support - response for zmp.check */
	} else if (strcmp(args[0], "zmp.support") == 0) {
		/* do nothing */

	/* zmp.no-support - response for zmp.check */
	} else if (strcmp(args[0], "zmp.no-support") == 0) {
		/* do nothing */

	/* something else entirely */
	} else {
		/* do nothing */
	}
}
