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

struct TELNET {
	telnet_state_t state;
	int sock;
} telnet;

/* terminal output parser */
typedef enum { TERM_ASCII, TERM_ESC, TERM_ESCRUN } term_state_t;

#define TERM_MAX_ESC 16

struct TERMINAL {
	term_state_t state;
	int esc_buf[TERM_MAX_ESC];
	size_t esc_cnt;
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

/* process user input */
static void on_key (int key) {
	size_t len = strlen(user_line);

	/* send */
	if (key == '\n' || key == '\r') {
		user_line[len] = '\n';
		int ret = send(telnet.sock, user_line, len+1, 0);
		if (ret == -1) {
			endwin();
			fprintf(stderr, "send() failed: %s\n", strerror(errno));
			exit(1);
		}
		waddnstr(win_main, user_line, len+1);
		user_line[0] = 0;
		wclear(win_input);
	}
	
	/* attempt to add; don't do it we're full */
	else if (len <= sizeof(user_line) - 1) {
		user_line[len] = key;
		user_line[len+1] = 0;
		waddch(win_input, key);
	}
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
				else if ((unsigned char)data[i] == SB)
					telnet.state = TELNET_SUB;
				/* something else; error, just print it */
				else {
					char buf[64];
					snprintf(buf, sizeof(buf), "<IAC:%d>", (int)data[i]);
					waddstr(win_main, buf);
					telnet.state = TELNET_TEXT;
				}
				break;
			case TELNET_DO:
			case TELNET_DONT:
			case TELNET_WILL:
			case TELNET_WONT:
				telnet.state = TELNET_TEXT;
				break;
			case TELNET_SUB:
				/* IAC escape */
				if ((unsigned char)data[i] == IAC)
					telnet.state = TELNET_SUBIAC;
				break;
			case TELNET_SUBIAC:
				/* IAC IAC escape */
				if ((unsigned char)data[i] == IAC)
					{}	
				/* end, process */
				else if ((unsigned char)data[i] == SE)
					telnet.state = TELNET_TEXT;
				/* something else; error */
				else
					telnet.state = TELNET_TEXT;
				break;
		}
	}
	char buf[64];
	snprintf(buf, sizeof(buf), "Input: %d", len);
	mvwaddstr(win_banner, 0, 0, buf);
}

int main (int argc, char** argv) {
	/* cleanup on any failure */
	atexit(cleanup);

	/* set terminal defaults */
	terminal.state = TERM_ASCII;

	/* set telnet defaults */
	telnet.state = TELNET_TEXT;

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
