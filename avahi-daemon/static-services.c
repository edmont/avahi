/***
  This file is part of avahi.

  avahi is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  avahi is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/stat.h>
#include <glob.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef USE_EXPAT_H
#include <expat.h>
#endif /* USE_EXPAT_H */

#ifdef USE_BSDXML_H
#include <bsdxml.h>
#endif /* USE_BSDXML_H */

#include <avahi-common/llist.h>
#include <avahi-common/malloc.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/domain.h>
#include <avahi-core/log.h>
#include <avahi-core/publish.h>

#include "main.h"
#include "static-services.h"

typedef struct StaticService StaticService;
typedef struct StaticServiceGroup StaticServiceGroup;

struct StaticService {
    StaticServiceGroup *group;

    char *type;
    char *domain_name;
    char *host_name;
    uint16_t port;
    int protocol;

    AvahiStringList *subtypes;

    AvahiStringList *txt_records;

    AVAHI_LLIST_FIELDS(StaticService, services);
};

struct StaticServiceGroup {
    char *filename;
    time_t mtime;

    char *name, *chosen_name;
    int replace_wildcards;

    AvahiSEntryGroup *entry_group;
    AVAHI_LLIST_HEAD(StaticService, services);
    AVAHI_LLIST_FIELDS(StaticServiceGroup, groups);
};

static AVAHI_LLIST_HEAD(StaticServiceGroup, groups) = NULL;

static char *replacestr(const char *pattern, const char *a, const char *b) {
    char *r = NULL, *e, *n;

    while ((e = strstr(pattern, a))) {
        char *k;

        k = avahi_strndup(pattern, e - pattern);
        if (r)
            n = avahi_strdup_printf("%s%s%s", r, k, b);
        else
            n = avahi_strdup_printf("%s%s", k, b);

        avahi_free(k);
        avahi_free(r);
        r = n;

        pattern = e + strlen(a);
    }

    if (!r)
        return avahi_strdup(pattern);

    n = avahi_strdup_printf("%s%s", r, pattern);
    avahi_free(r);

    return n;
}

static void add_static_service_group_to_server(StaticServiceGroup *g);
static void remove_static_service_group_from_server(StaticServiceGroup *g);

static StaticService *static_service_new(StaticServiceGroup *group) {
    StaticService *s;

    assert(group);
    s = avahi_new(StaticService, 1);
    s->group = group;

    s->type = s->host_name = s->domain_name = NULL;
    s->port = 0;
    s->protocol = AVAHI_PROTO_UNSPEC;

    s->txt_records = NULL;
    s->subtypes = NULL;

    AVAHI_LLIST_PREPEND(StaticService, services, group->services, s);

    return s;
}

static StaticServiceGroup *static_service_group_new(char *filename) {
    StaticServiceGroup *g;
    assert(filename);

    g = avahi_new(StaticServiceGroup, 1);
    g->filename = avahi_strdup(filename);
    g->mtime = 0;
    g->name = g->chosen_name = NULL;
    g->replace_wildcards = 0;
    g->entry_group = NULL;

    AVAHI_LLIST_HEAD_INIT(StaticService, g->services);
    AVAHI_LLIST_PREPEND(StaticServiceGroup, groups, groups, g);

    return g;
}

static void static_service_free(StaticService *s) {
    assert(s);

    AVAHI_LLIST_REMOVE(StaticService, services, s->group->services, s);

    avahi_free(s->type);
    avahi_free(s->host_name);
    avahi_free(s->domain_name);

    avahi_string_list_free(s->txt_records);
    avahi_string_list_free(s->subtypes);

    avahi_free(s);
}

static void static_service_group_free(StaticServiceGroup *g) {
    assert(g);

    if (g->entry_group)
        avahi_s_entry_group_free(g->entry_group);

    while (g->services)
        static_service_free(g->services);

    AVAHI_LLIST_REMOVE(StaticServiceGroup, groups, groups, g);

    avahi_free(g->filename);
    avahi_free(g->name);
    avahi_free(g->chosen_name);
    avahi_free(g);
}

static void entry_group_callback(AvahiServer *s, AVAHI_GCC_UNUSED AvahiSEntryGroup *eg, AvahiEntryGroupState state, void* userdata) {
    StaticServiceGroup *g = userdata;

    assert(s);
    assert(g);

    switch (state) {

        case AVAHI_ENTRY_GROUP_COLLISION: {
            char *n;

            remove_static_service_group_from_server(g);

            n = avahi_alternative_service_name(g->chosen_name);
            avahi_free(g->chosen_name);
            g->chosen_name = n;

            avahi_log_notice("Service name conflict for \"%s\" (%s), retrying with \"%s\".", g->name, g->filename, g->chosen_name);

            add_static_service_group_to_server(g);
            break;
        }

        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            avahi_log_info("Service \"%s\" (%s) successfully established.", g->chosen_name, g->filename);
            break;

        case AVAHI_ENTRY_GROUP_FAILURE:
            avahi_log_warn("Failed to publish service \"%s\" (%s): %s", g->chosen_name, g->filename, avahi_strerror(avahi_server_errno(s)));
            remove_static_service_group_from_server(g);
            break;

        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:
            ;
    }
}

static void add_static_service_group_to_server(StaticServiceGroup *g) {
    StaticService *s;

    assert(g);

    if (g->entry_group && !avahi_s_entry_group_is_empty(g->entry_group))
        /* This service group is already registered in the server */
        return;

    if (!g->chosen_name || (g->replace_wildcards && strstr(g->name, "%h"))) {

        avahi_free(g->chosen_name);

        if (g->replace_wildcards) {
            char label[AVAHI_LABEL_MAX];
            const char *p;

            p = avahi_server_get_host_name(avahi_server);
            avahi_unescape_label(&p, label, sizeof(label));

            g->chosen_name = replacestr(g->name, "%h", label);
        } else
            g->chosen_name = avahi_strdup(g->name);

    }

    if (!g->entry_group)
        g->entry_group = avahi_s_entry_group_new(avahi_server, entry_group_callback, g);

    assert(avahi_s_entry_group_is_empty(g->entry_group));

    for (s = g->services; s; s = s->services_next) {
        AvahiStringList *i;

        if (avahi_server_add_service_strlst(
                avahi_server,
                g->entry_group,
                AVAHI_IF_UNSPEC, s->protocol,
                0,
                g->chosen_name, s->type, s->domain_name,
                s->host_name, s->port,
                s->txt_records) < 0) {
            avahi_log_error("Failed to add service '%s' of type '%s', ignoring service group (%s): %s",
                            g->chosen_name, s->type, g->filename,
                            avahi_strerror(avahi_server_errno(avahi_server)));
            remove_static_service_group_from_server(g);
            return;
        }

        for (i = s->subtypes; i; i = i->next) {

            if (avahi_server_add_service_subtype(
                    avahi_server,
                    g->entry_group,
                    AVAHI_IF_UNSPEC, s->protocol,
                    0,
                    g->chosen_name, s->type, s->domain_name,
                    (char*) i->text) < 0) {

                avahi_log_error("Failed to add subtype '%s' for service '%s' of type '%s', ignoring subtype (%s): %s",
                                i->text, g->chosen_name, s->type, g->filename,
                                avahi_strerror(avahi_server_errno(avahi_server)));
            }
        }
    }

    avahi_s_entry_group_commit(g->entry_group);
}

static void remove_static_service_group_from_server(StaticServiceGroup *g) {
    assert(g);

    if (g->entry_group)
        avahi_s_entry_group_reset(g->entry_group);
}

typedef enum {
    XML_TAG_INVALID,
    XML_TAG_SERVICE_GROUP,
    XML_TAG_NAME,
    XML_TAG_SERVICE,
    XML_TAG_TYPE,
    XML_TAG_SUBTYPE,
    XML_TAG_DOMAIN_NAME,
    XML_TAG_HOST_NAME,
    XML_TAG_PORT,
    XML_TAG_TXT_RECORD,
    XML_TAG_TXT_BINARY
} xml_tag_name;

struct xml_userdata {
    StaticServiceGroup *group;
    StaticService *service;
    xml_tag_name current_tag;
    int failed;
    char *buf;
};

#ifndef XMLCALL
#define XMLCALL
#endif

static void XMLCALL xml_start(void *data, const char *el, const char *attr[]) {
    struct xml_userdata *u = data;

    assert(u);

    if (u->failed)
        return;

    if (u->current_tag == XML_TAG_INVALID && strcmp(el, "service-group") == 0) {

        if (attr[0])
            goto invalid_attr;

        u->current_tag = XML_TAG_SERVICE_GROUP;
    } else if (u->current_tag == XML_TAG_SERVICE_GROUP && strcmp(el, "name") == 0) {
        u->current_tag = XML_TAG_NAME;

        if (attr[0]) {
            if (strcmp(attr[0], "replace-wildcards") == 0)
                u->group->replace_wildcards = strcmp(attr[1], "yes") == 0;
            else
                goto invalid_attr;

            if (attr[2])
                goto invalid_attr;
        }

    } else if (u->current_tag == XML_TAG_SERVICE_GROUP && strcmp(el, "service") == 0) {
        u->current_tag = XML_TAG_SERVICE;

        assert(!u->service);
        u->service = static_service_new(u->group);

        if (attr[0]) {
            if (strcmp(attr[0], "protocol") == 0) {
                AvahiProtocol protocol;

                if (strcmp(attr[1], "ipv4") == 0) {
                    protocol = AVAHI_PROTO_INET;
                } else if (strcmp(attr[1], "ipv6") == 0) {
                    protocol = AVAHI_PROTO_INET6;
                } else if (strcmp(attr[1], "any") == 0) {
                    protocol = AVAHI_PROTO_UNSPEC;
                } else {
                    avahi_log_error("%s: parse failure: invalid protocol specification \"%s\".", u->group->filename, attr[1]);
                    u->failed = 1;
                    return;
                }

                u->service->protocol = protocol;
            } else
                goto invalid_attr;

            if (attr[2])
                goto invalid_attr;
        }

    } else if (u->current_tag == XML_TAG_SERVICE && strcmp(el, "type") == 0) {
        if (attr[0])
            goto invalid_attr;

        u->current_tag = XML_TAG_TYPE;
    } else if (u->current_tag == XML_TAG_SERVICE && strcmp(el, "subtype") == 0) {
        if (attr[0])
            goto invalid_attr;

        u->current_tag = XML_TAG_SUBTYPE;
    } else if (u->current_tag == XML_TAG_SERVICE && strcmp(el, "domain-name") == 0) {
        if (attr[0])
            goto invalid_attr;

        u->current_tag = XML_TAG_DOMAIN_NAME;
    } else if (u->current_tag == XML_TAG_SERVICE && strcmp(el, "host-name") == 0) {
        if (attr[0])
            goto invalid_attr;

        u->current_tag = XML_TAG_HOST_NAME;
    } else if (u->current_tag == XML_TAG_SERVICE && strcmp(el, "port") == 0) {
        if (attr[0])
            goto invalid_attr;

        u->current_tag = XML_TAG_PORT;
    } else if (u->current_tag == XML_TAG_SERVICE && strcmp(el, "txt-record") == 0) {
        if (attr[0])
            goto invalid_attr;

        u->current_tag = XML_TAG_TXT_RECORD;
    } else if (u->current_tag == XML_TAG_SERVICE && strcmp(el, "txt-binary") == 0) {
        if (attr[0])
            goto invalid_attr;

        u->current_tag = XML_TAG_TXT_BINARY;
    } else {
        avahi_log_error("%s: parse failure: didn't expect element <%s>.", u->group->filename, el);
        u->failed = 1;
    }

    return;

invalid_attr:
    avahi_log_error("%s: parse failure: invalid attribute for element <%s>.", u->group->filename, el);
    u->failed = 1;
    return;
}

static void XMLCALL xml_end(void *data, AVAHI_GCC_UNUSED const char *el) {
    struct xml_userdata *u = data;
    assert(u);

    if (u->failed)
        return;

    switch (u->current_tag) {
        case XML_TAG_SERVICE_GROUP:

            if (!u->group->name || !u->group->services) {
                avahi_log_error("%s: parse failure: service group incomplete.", u->group->filename);
                u->failed = 1;
                return;
            }

            u->current_tag = XML_TAG_INVALID;
            break;

        case XML_TAG_SERVICE:

            if (!u->service->type) {
                avahi_log_error("%s: parse failure: service incomplete.", u->group->filename);
                u->failed = 1;
                return;
            }

            u->service = NULL;
            u->current_tag = XML_TAG_SERVICE_GROUP;
            break;

        case XML_TAG_NAME:
            u->current_tag = XML_TAG_SERVICE_GROUP;
            break;

        case XML_TAG_PORT: {
            int p;
            assert(u->service);

            p = u->buf ? atoi(u->buf) : 0;

            if (p < 0 || p > 0xFFFF) {
                avahi_log_error("%s: parse failure: invalid port specification \"%s\".", u->group->filename, u->buf);
                u->failed = 1;
                return;
            }

            u->service->port = (uint16_t) p;

            u->current_tag = XML_TAG_SERVICE;
            break;
        }

        case XML_TAG_TXT_RECORD: {
            assert(u->service);

            u->service->txt_records = avahi_string_list_add(u->service->txt_records, u->buf ? u->buf : "");
            u->current_tag = XML_TAG_SERVICE;
            break;
        }

        case XML_TAG_TXT_BINARY: {
            char * value_hex;
            char * equals;
            int key_size, value_size = 0;
            assert(u->service);

            equals = strchr(u->buf, '=');
            if (equals) {
                if ((strlen(equals)-1)%2) {
                    avahi_log_error("%s: parse failure: invalid binary value \"%s\".", u->group->filename, equals);
                    u->failed = 1;
                    return;
                }
                key_size = equals - u->buf;
                value_hex = malloc(strlen(equals)-1);
                strcpy(value_hex, equals+1);
                while (sscanf(&value_hex[2*value_size], "%02x", &equals[value_size+1]) != EOF) {
                    value_size++;
                }
            } else {
                avahi_log_error("%s: parse failure: '=' not found in binary TXT record \"%s\".", u->group->filename, u->buf);
                u->failed = 1;
                return;
            }
            u->service->txt_records = avahi_string_list_add_arbitrary(u->service->txt_records, u->buf, key_size+1+value_size);
            u->current_tag = XML_TAG_SERVICE;
            break;
        }

        case XML_TAG_SUBTYPE: {
            assert(u->service);

            u->service->subtypes = avahi_string_list_add(u->service->subtypes, u->buf ? u->buf : "");
            u->current_tag = XML_TAG_SERVICE;
            break;
        }

        case XML_TAG_TYPE:
        case XML_TAG_DOMAIN_NAME:
        case XML_TAG_HOST_NAME:
            u->current_tag = XML_TAG_SERVICE;
            break;

        case XML_TAG_INVALID:
            ;
    }

    avahi_free(u->buf);
    u->buf = NULL;
}

static char *append_cdata(char *t, const char *n, int length) {
    char *r, *k;

    if (!length)
        return t;


    k = avahi_strndup(n, length);

    if (t) {
        r = avahi_strdup_printf("%s%s", t, k);
        avahi_free(k);
        avahi_free(t);
    } else
        r = k;

    return r;
}

static void XMLCALL xml_cdata(void *data, const XML_Char *s, int len) {
    struct xml_userdata *u = data;
    assert(u);

    if (u->failed)
        return;

    switch (u->current_tag) {
        case XML_TAG_NAME:
            u->group->name = append_cdata(u->group->name, s, len);
            break;

        case XML_TAG_TYPE:
            assert(u->service);
            u->service->type = append_cdata(u->service->type, s, len);
            break;

        case XML_TAG_DOMAIN_NAME:
            assert(u->service);
            u->service->domain_name = append_cdata(u->service->domain_name, s, len);
            break;

        case XML_TAG_HOST_NAME:
            assert(u->service);
            u->service->host_name = append_cdata(u->service->host_name, s, len);
            break;

        case XML_TAG_PORT:
        case XML_TAG_TXT_RECORD:
        case XML_TAG_TXT_BINARY:
        case XML_TAG_SUBTYPE:
            assert(u->service);
            u->buf = append_cdata(u->buf, s, len);
            break;

        case XML_TAG_SERVICE_GROUP:
        case XML_TAG_SERVICE:
        case XML_TAG_INVALID:
            ;
    }
}

static int static_service_group_load(StaticServiceGroup *g) {
    XML_Parser parser = NULL;
    int fd = -1;
    struct xml_userdata u;
    int r = -1;
    struct stat st;
    ssize_t n;

    assert(g);

    u.buf = NULL;
    u.group = g;
    u.service = NULL;
    u.current_tag = XML_TAG_INVALID;
    u.failed = 0;

    /* Cleanup old data in this service group, if available */
    remove_static_service_group_from_server(g);
    while (g->services)
        static_service_free(g->services);

    avahi_free(g->name);
    avahi_free(g->chosen_name);
    g->name = g->chosen_name = NULL;
    g->replace_wildcards = 0;

    if (!(parser = XML_ParserCreate(NULL))) {
        avahi_log_error("XML_ParserCreate() failed.");
        goto finish;
    }

    if ((fd = open(g->filename, O_RDONLY)) < 0) {
        avahi_log_error("open(\"%s\", O_RDONLY): %s", g->filename, strerror(errno));
        goto finish;
    }

    if (fstat(fd, &st) < 0) {
        avahi_log_error("fstat(): %s", strerror(errno));
        goto finish;
    }

    g->mtime = st.st_mtime;

    XML_SetUserData(parser, &u);

    XML_SetElementHandler(parser, xml_start, xml_end);
    XML_SetCharacterDataHandler(parser, xml_cdata);

    do {
        void *buffer;

#define BUFSIZE (10*1024)

        if (!(buffer = XML_GetBuffer(parser, BUFSIZE))) {
            avahi_log_error("XML_GetBuffer() failed.");
            goto finish;
        }

        if ((n = read(fd, buffer, BUFSIZE)) < 0) {
            avahi_log_error("read(): %s\n", strerror(errno));
            goto finish;
        }

        if (!XML_ParseBuffer(parser, n, n == 0)) {
            avahi_log_error("XML_ParseBuffer() failed at line %d: %s.\n", (int) XML_GetCurrentLineNumber(parser), XML_ErrorString(XML_GetErrorCode(parser)));
            goto finish;
        }

    } while (n != 0);

    if (!u.failed)
        r = 0;

finish:

    if (fd >= 0)
        close(fd);

    if (parser)
        XML_ParserFree(parser);

    avahi_free(u.buf);

    return r;
}

static void load_file(char *n) {
    StaticServiceGroup *g;
    assert(n);

    for (g = groups; g; g = g->groups_next)
        if (strcmp(g->filename, n) == 0)
            return;

    avahi_log_info("Loading service file %s.", n);

    g = static_service_group_new(n);
    if (static_service_group_load(g) < 0) {
        avahi_log_error("Failed to load service group file %s, ignoring.", g->filename);
        static_service_group_free(g);
    }
}

void static_service_load(int in_chroot) {
    StaticServiceGroup *g, *n;
    glob_t globbuf;
    int globret;
    char **p;

    for (g = groups; g; g = n) {
        struct stat st;

        n = g->groups_next;

        if (stat(g->filename, &st) < 0) {

            if (errno == ENOENT)
                avahi_log_info("Service group file %s vanished, removing services.", g->filename);
            else
                avahi_log_warn("Failed to stat() file %s, ignoring: %s", g->filename, strerror(errno));

            static_service_group_free(g);
        } else if (st.st_mtime != g->mtime) {
            avahi_log_info("Service group file %s changed, reloading.", g->filename);

            if (static_service_group_load(g) < 0) {
                avahi_log_warn("Failed to load service group file %s, removing service.", g->filename);
                static_service_group_free(g);
            }
        }
    }

    memset(&globbuf, 0, sizeof(globbuf));

    if ((globret = glob(in_chroot ? "/services/*.service" : AVAHI_SERVICE_DIR "/*.service", GLOB_ERR, NULL, &globbuf)) != 0)

        switch (globret) {
#ifdef GLOB_NOSPACE
        case GLOB_NOSPACE:
            avahi_log_error("Not enough memory to read service directory "AVAHI_SERVICE_DIR".");
            break;
#endif
#ifdef GLOB_NOMATCH
            case GLOB_NOMATCH:
            avahi_log_info("No service file found in "AVAHI_SERVICE_DIR".");
            break;
#endif
            default:
            avahi_log_error("Failed to read "AVAHI_SERVICE_DIR".");
            break;
        }

    else {
        for (p = globbuf.gl_pathv; *p; p++)
            load_file(*p);

        globfree(&globbuf);
    }
}

void static_service_free_all(void) {

    while (groups)
        static_service_group_free(groups);
}

void static_service_add_to_server(void) {
    StaticServiceGroup *g;

    for (g = groups; g; g = g->groups_next)
        add_static_service_group_to_server(g);
}

void static_service_remove_from_server(void) {
    StaticServiceGroup *g;

    for (g = groups; g; g = g->groups_next)
        remove_static_service_group_from_server(g);
}
