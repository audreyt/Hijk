#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <errno.h>
#include "http-parser/http_parser.h"
#define DONE 1

struct response {
    SV *body;
    HV *header;
    SV *current_header;
    SV *current_value;
    int status;
    int flags;
    int timeout_ms;
};

static int header_complete_cb(http_parser *p);
static int message_complete_cb (http_parser *p);
static int body_cb(http_parser *p, const char *buf, size_t len);
static int header_field_cb(http_parser *p, const char *buf, size_t len);
static int header_value_cb(http_parser *p, const char *buf, size_t len);
static void cleanup_mid_header_build(struct response *r);

static http_parser_settings settings = {
    .on_message_begin = NULL,
    .on_header_field = header_field_cb,
    .on_header_value = header_value_cb,
    .on_url = NULL,
    .on_body = body_cb,
    .on_headers_complete = header_complete_cb,
    .on_message_complete = message_complete_cb
};

static int read_and_store(int fd, struct response *r) {
    struct http_parser parser;
    char buf[BUFSIZ];
    int rc, nparsed, len, error = 0;
    struct pollfd pfd[1];
    memset(&parser,0,sizeof(parser));
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = r;
    pfd[0].fd = fd;
    pfd[0].events = POLLIN|POLLERR|POLLNVAL;

    if (r->timeout_ms == 0)
        r->timeout_ms = -1;

    for (;;) {
        int rc = poll(pfd,1,r->timeout_ms);

        if (rc == -1)
            die("POLL FAILED: %s",strerror(errno));

        if (rc == 0 || (rc == 1 && (pfd[0].revents & POLLNVAL))) {
          error = 2; // Hijk::Error::READ_TIMEOUT
          break;
        }

        int len = read(fd,buf,sizeof(buf));
        if (len == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            else
                die("READ FAILED: %s",strerror(errno));
        }

        if ((nparsed = http_parser_execute(&parser,&settings,buf,len)) != len) {
            r->status = 0;
            sv_setpv(r->body,http_errno_description(parser.http_errno));
            break;
        }

        if (nparsed == 0 && !(r->flags & DONE))
            die("Got 0 bytes back, which shouldn't happen, probably keep-alive issue");

        if (r->flags & DONE)
            break;
    }
    cleanup_mid_header_build(r);
    return error;
}

static void cleanup_mid_header_build(struct response *r) {
    if (r->current_header) {
        if (r->current_value)
            hv_store_ent(r->header,r->current_header,r->current_value,0);
        else
            SvREFCNT_dec(r->current_header);
    } else {
        if (r->current_value)
            SvREFCNT_dec(r->current_value);
    }
}
static int header_complete_cb(http_parser *p) {
    struct response *r = (struct response *) p->data;
    r->status = p->status_code;
    return 0;
}

static int message_complete_cb (http_parser *p) {
    struct response *r = (struct response *) p->data;
    r->flags |= DONE;
    return 0;
}

static int body_cb(http_parser *p, const char *buf, size_t len) {
    struct response *r = (struct response *) p->data;
    sv_catpvn(r->body,buf,len);
    return 0;
}

static int header_value_cb(http_parser *p, const char *buf, size_t len) {
    struct response *r = (struct response *) p->data;
    if (r->current_header) {
        if (!r->current_value)
            r->current_value = newSVpv("",0);

        sv_catpvn(r->current_value, buf,len);
    }
    return 0;
}

static int header_field_cb(http_parser *p, const char *buf, size_t len) {
    struct response *r = (struct response *) p->data;
    if (r->current_value && r->current_header) {
        hv_store_ent(r->header,r->current_header,r->current_value,0);
        r->current_value = NULL;
        r->current_header = NULL;
    }

    if (!r->current_header)
        r->current_header = newSVpv("",0);

    sv_catpvn(r->current_header, buf,len);
    return 0;
}
