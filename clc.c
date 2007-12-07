/**
 * Command-Line Client
 * Sean Middleditch, AwesomePlay Productions Inc.
 * elanthis@awemud.net
 * PUBLIC DOMAIN
 */

#include <termio.h>
#include <signal.h>
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
#include <netdb.h>

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
static void telnet_do_subreq (void);

/* websock protocol */
#define WEBSOCK_MAX_MSG 2048

struct WEBSOCK {
	char msg[WEBSOCK_MAX_MSG];
	size_t msg_size;
} websock;

static void websock_on_line(const char* line, size_t len);
static void websock_on_recv(const char* bytes, size_t len);
static void websock_on_resize(int w, int h);

/* protocol handler */
typedef enum { PROTOCOL_TELNET, PROTOCOL_WEBSOCK } protocol_type_t;

struct PROTOCOL {
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

struct TERMINAL {
	term_state_t state;
	int esc_buf[TERM_MAX_ESC];
	size_t esc_cnt;
	char flags;
	int color;
} terminal;

/* running flag; when 0, exit main loop */
static int running = 1;

/* line buffer */
static char user_line[1024];

/* banner buffer */
static char banner[1024];

/* windows */
static WINDOW* win_main = 0;
static WINDOW* win_input = 0;
static WINDOW* win_banner = 0;

/* last interrupt */
volatile int have_sigwinch = 0;
volatile int have_sigint = 0;

/* server socket */
static int sock;

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

/* paint banner */
static void paint_banner (void) {
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

	/* refresh */
	wnoutrefresh(win_main);
	wnoutrefresh(win_banner);
	wnoutrefresh(win_input);
	doupdate();
}

/* force-send bytes to websock server */
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
			bytes += ret;
			len -= ret;
		}
	}
}

/* process user input */
static void on_key (int key) {
	size_t len = strlen(user_line);

	/* send */
	if (key == '\n' || key == '\r' || key == KEY_ENTER) {
		/* send line to server */
		protocol.on_line(user_line, len);
		/* reset input */
		user_line[0] = 0;
	}

	/* backspace; remove last character */
	else if (key == KEY_BACKSPACE) {
		if (len > 0)
			user_line[len-1] = 0;
	}
	
	/* attempt to add */
	else if (key < KEY_MIN && isprint(key)) {
		/* only if we're not full */
		if (len <= sizeof(user_line) - 1) {
			user_line[len] = key;
			user_line[len+1] = 0;
		}
	}

	/* draw input */
	wclear(win_input);
	if (terminal.flags & TERM_FLAG_ECHO) {
		mvwaddstr(win_input, 0, 0, user_line);
	} else {
		wmove(win_input, 0, 0);
		int i;
		for (i = strlen(user_line); i > 0; --i)
			waddch(win_input, '*');
	}
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
		if (text[i] != 27)
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
	/* configuration */
	protocol_type_t type = PROTOCOL_TELNET;
	const char* host = NULL;
	const char* port = NULL;
	const char* default_port = "23";

	/* process command line args
	 *
	 * usage:
	 *  client [options] <host> [<port>]
	 * 
	 * options:
	 *  -t  use telnet (port 23)
	 *  -w  use websock (port 4747)
	 */
	int i;
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

		/* other unknown option */
		if (argv[i][0] == '-') {
			fprintf(stderr, "Unknown option %s\n", argv[i]);
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
		fprintf(stderr, "No host was given\n");
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
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGWINCH, &sa, NULL);

	/* initialize user line buffer */
	user_line[0] = 0;

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
				protocol.on_recv(buffer, ret);
			}
		}

		/* flush output */
		wnoutrefresh(win_main);
		wnoutrefresh(win_banner);
		wnoutrefresh(win_input);
		doupdate();
	}

	/* final display */
	snprintf(banner, sizeof(banner), "CLC - %s:%s (disconnected)", host, port);
	redraw_display();
	wgetch(win_input);
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
					snprintf(banner, sizeof(banner), "%.*s", (int)websock.msg_size - 1, &websock.msg[1]);
					paint_banner();
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
	/* send with proper newline */
	char nl[] = { '\n', '\r' };
	telnet_send_esc(line, len);
	telnet_send_esc(nl, sizeof(nl));

	/* echo output */
	if (terminal.flags & TERM_FLAG_ECHO) {
		wattron(win_main, COLOR_PAIR(COLOR_YELLOW));
		waddnstr(win_main, user_line, len+1);
		waddch(win_main, '\n');
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
					waddstr(win_main, buf);
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
			/* on_zmp(&telnet.sub_buf[1], telnet.sub_size - 1); */
			break;
	}
}
