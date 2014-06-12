/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <getopt.h>

#include "util.h"
#include "hashmap.h"
#include "specifier.h"
#include "path-util.h"
#include "build.h"
#include "strv.h"
#include "conf-files.h"
#include "copy.h"
#include "utf8.h"

typedef enum ItemType {
        ADD_USER = 'u',
        ADD_GROUP = 'g',
} ItemType;
typedef struct Item {
        ItemType type;

        char *name;
        char *uid_path;
        char *gid_path;
        char *description;

        gid_t gid;
        uid_t uid;

        bool gid_set:1;
        bool uid_set:1;

        bool todo:1;
} Item;

static char *arg_root = NULL;

static const char conf_file_dirs[] =
        "/usr/local/lib/sysusers.d\0"
        "/usr/lib/sysusers.d\0"
#ifdef HAVE_SPLIT_USR
        "/lib/sysusers.d\0"
#endif
        ;

static Hashmap *users = NULL, *groups = NULL;
static Hashmap *todo_uids = NULL, *todo_gids = NULL;

static Hashmap *database_uid = NULL, *database_user = NULL;
static Hashmap *database_gid = NULL, *database_group = NULL;

static uid_t search_uid = SYSTEM_UID_MAX;
static gid_t search_gid = SYSTEM_GID_MAX;

#define UID_TO_PTR(u) (ULONG_TO_PTR(u+1))
#define PTR_TO_UID(u) ((uid_t) (PTR_TO_ULONG(u)-1))

#define GID_TO_PTR(g) (ULONG_TO_PTR(g+1))
#define PTR_TO_GID(g) ((gid_t) (PTR_TO_ULONG(g)-1))

#define fix_root(x) (arg_root ? strappenda(arg_root, x) : x)

static int load_user_database(void) {
        _cleanup_fclose_ FILE *f = NULL;
        const char *passwd_path;
        struct passwd *pw;
        int r;

        passwd_path = fix_root("/etc/passwd");
        f = fopen(passwd_path, "re");
        if (!f)
                return errno == ENOENT ? 0 : -errno;

        r = hashmap_ensure_allocated(&database_user, string_hash_func, string_compare_func);
        if (r < 0)
                return r;

        r = hashmap_ensure_allocated(&database_uid, trivial_hash_func, trivial_compare_func);
        if (r < 0)
                return r;

        errno = 0;
        while ((pw = fgetpwent(f))) {
                char *n;
                int k, q;

                n = strdup(pw->pw_name);
                if (!n)
                        return -ENOMEM;

                k = hashmap_put(database_user, n, UID_TO_PTR(pw->pw_uid));
                if (k < 0 && k != -EEXIST) {
                        free(n);
                        return k;
                }

                q = hashmap_put(database_uid, UID_TO_PTR(pw->pw_uid), n);
                if (q < 0 && q != -EEXIST) {
                        if (k < 0)
                                free(n);
                        return q;
                }

                if (q < 0 && k < 0)
                        free(n);

                errno = 0;
        }
        if (!IN_SET(errno, 0, ENOENT))
                return -errno;

        return 0;
}

static int load_group_database(void) {
        _cleanup_fclose_ FILE *f = NULL;
        const char *group_path;
        struct group *gr;
        int r;

        group_path = fix_root("/etc/group");
        f = fopen(group_path, "re");
        if (!f)
                return errno == ENOENT ? 0 : -errno;

        r = hashmap_ensure_allocated(&database_group, string_hash_func, string_compare_func);
        if (r < 0)
                return r;

        r = hashmap_ensure_allocated(&database_gid, trivial_hash_func, trivial_compare_func);
        if (r < 0)
                return r;

        errno = 0;
        while ((gr = fgetgrent(f))) {
                char *n;
                int k, q;

                n = strdup(gr->gr_name);
                if (!n)
                        return -ENOMEM;

                k = hashmap_put(database_group, n, GID_TO_PTR(gr->gr_gid));
                if (k < 0 && k != -EEXIST) {
                        free(n);
                        return k;
                }

                q = hashmap_put(database_gid, GID_TO_PTR(gr->gr_gid), n);
                if (q < 0 && q != -EEXIST) {
                        if (k < 0)
                                free(n);
                        return q;
                }

                if (q < 0 && k < 0)
                        free(n);

                errno = 0;
        }
        if (!IN_SET(errno, 0, ENOENT))
                return -errno;

        return 0;
}

static int make_backup(const char *x) {
        _cleanup_close_ int src = -1, dst = -1;
        char *backup, *temp;
        struct timespec ts[2];
        struct stat st;
        int r;

        src = open(x, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (src < 0) {
                if (errno == ENOENT) /* No backup necessary... */
                        return 0;

                return -errno;
        }

        if (fstat(src, &st) < 0)
                return -errno;

        temp = strappenda(x, ".XXXXXX");
        dst = mkostemp_safe(temp, O_WRONLY|O_CLOEXEC|O_NOCTTY);
        if (dst < 0)
                return dst;

        r = copy_bytes(src, dst);
        if (r < 0)
                goto fail;

        /* Copy over the access mask */
        if (fchmod(dst, st.st_mode & 07777) < 0) {
                r = -errno;
                goto fail;
        }

        /* Don't fail on chmod(). If it stays owned by us, then it
         * isn't too bad... */
        fchown(dst, st.st_uid, st.st_gid);

        ts[0] = st.st_atim;
        ts[1] = st.st_mtim;
        futimens(dst, ts);

        backup = strappenda(x, "-");
        if (rename(temp, backup) < 0)
                goto fail;

        return 0;

fail:
        unlink(temp);
        return r;
}

static int write_files(void) {

        _cleanup_fclose_ FILE *passwd = NULL, *group = NULL;
        _cleanup_free_ char *passwd_tmp = NULL, *group_tmp = NULL;
        const char *passwd_path = NULL, *group_path = NULL;
        Iterator iterator;
        Item *i;
        int r;

        /* We don't patch /etc/shadow or /etc/gshadow here, since we
         * only create user accounts without passwords anyway. */

        if (hashmap_size(todo_gids) > 0) {
                _cleanup_fclose_ FILE *original = NULL;

                group_path = fix_root("/etc/group");
                r = fopen_temporary(group_path, &group, &group_tmp);
                if (r < 0)
                        goto finish;

                if (fchmod(fileno(group), 0644) < 0) {
                        r = -errno;
                        goto finish;
                }

                original = fopen(group_path, "re");
                if (original) {
                        struct group *gr;

                        errno = 0;
                        while ((gr = fgetgrent(original))) {
                                /* Safety checks against name and GID
                                 * collisions. Normally, this should
                                 * be unnecessary, but given that we
                                 * look at the entries anyway here,
                                 * let's make an extra verification
                                 * step that we don't generate
                                 * duplicate entries. */

                                i = hashmap_get(groups, gr->gr_name);
                                if (i && i->todo) {
                                        r = -EEXIST;
                                        goto finish;
                                }

                                if (hashmap_contains(todo_gids, GID_TO_PTR(gr->gr_gid))) {
                                        r = -EEXIST;
                                        goto finish;
                                }

                                if (putgrent(gr, group) < 0) {
                                        r = -errno;
                                        goto finish;
                                }

                                errno = 0;
                        }
                        if (!IN_SET(errno, 0, ENOENT)) {
                                r = -errno;
                                goto finish;
                        }

                } else if (errno != ENOENT) {
                        r = -errno;
                        goto finish;
                }

                HASHMAP_FOREACH(i, todo_gids, iterator) {
                        struct group n = {
                                .gr_name = i->name,
                                .gr_gid = i->gid,
                                .gr_passwd = (char*) "x",
                        };

                        if (putgrent(&n, group) < 0) {
                                r = -errno;
                                goto finish;
                        }
                }

                r = fflush_and_check(group);
                if (r < 0)
                        goto finish;
        }

        if (hashmap_size(todo_uids) > 0) {
                _cleanup_fclose_ FILE *original = NULL;

                passwd_path = fix_root("/etc/passwd");
                r = fopen_temporary(passwd_path, &passwd, &passwd_tmp);
                if (r < 0)
                        goto finish;

                if (fchmod(fileno(passwd), 0644) < 0) {
                        r = -errno;
                        goto finish;
                }

                original = fopen(passwd_path, "re");
                if (original) {
                        struct passwd *pw;

                        errno = 0;
                        while ((pw = fgetpwent(original))) {

                                i = hashmap_get(users, pw->pw_name);
                                if (i && i->todo) {
                                        r = -EEXIST;
                                        goto finish;
                                }

                                if (hashmap_contains(todo_uids, UID_TO_PTR(pw->pw_uid))) {
                                        r = -EEXIST;
                                        goto finish;
                                }

                                if (putpwent(pw, passwd) < 0) {
                                        r = -errno;
                                        goto finish;
                                }

                                errno = 0;
                        }
                        if (!IN_SET(errno, 0, ENOENT)) {
                                r = -errno;
                                goto finish;
                        }

                } else if (errno != ENOENT) {
                        r = -errno;
                        goto finish;
                }

                HASHMAP_FOREACH(i, todo_uids, iterator) {
                        struct passwd n = {
                                .pw_name = i->name,
                                .pw_uid = i->uid,
                                .pw_gid = i->gid,
                                .pw_gecos = i->description,
                                .pw_passwd = (char*) "x",
                        };

                        /* Initialize the home directory and the shell
                         * to nologin, with one exception: for root we
                         * patch in something special */
                        if (i->uid == 0) {
                                n.pw_shell = (char*) "/bin/sh";
                                n.pw_dir = (char*) "/root";
                        } else {
                                n.pw_shell = (char*) "/sbin/nologin";
                                n.pw_dir = (char*) "/";
                        }

                        if (putpwent(&n, passwd) < 0) {
                                r = -r;
                                goto finish;
                        }
                }

                r = fflush_and_check(passwd);
                if (r < 0)
                        goto finish;
        }

        /* Make a backup of the old files */
        if (group) {
                r = make_backup(group_path);
                if (r < 0)
                        goto finish;
        }

        if (passwd) {
                r = make_backup(passwd_path);
                if (r < 0)
                        goto finish;
        }

        /* And make the new files count */
        if (group) {
                if (rename(group_tmp, group_path) < 0) {
                        r = -errno;
                        goto finish;
                }

                free(group_tmp);
                group_tmp = NULL;
        }

        if (passwd) {
                if (rename(passwd_tmp, passwd_path) < 0) {
                        r = -errno;
                        goto finish;
                }

                free(passwd_tmp);
                passwd_tmp = NULL;
        }

        return 0;

finish:
        if (r < 0) {
                if (passwd_tmp)
                        unlink(passwd_tmp);
                if (group_tmp)
                        unlink(group_tmp);
        }

        return r;
}

static int uid_is_ok(uid_t uid, const char *name) {
        struct passwd *p;
        struct group *g;
        const char *n;
        Item *i;

        /* Let's see if we already have assigned the UID a second time */
        if (hashmap_get(todo_uids, UID_TO_PTR(uid)))
                return 0;

        /* Try to avoid using uids that are already used by a group
         * that doesn't have the same name as our new user. */
        i = hashmap_get(todo_gids, GID_TO_PTR(uid));
        if (i && !streq(i->name, name))
                return 0;

        /* Let's check the files directly */
        if (hashmap_contains(database_uid, UID_TO_PTR(uid)))
                return 0;

        n = hashmap_get(database_gid, GID_TO_PTR(uid));
        if (n && !streq(n, name))
                return 0;

        /* Let's also check via NSS, to avoid UID clashes over LDAP and such, just in case */
        if (!arg_root) {
                errno = 0;
                p = getpwuid(uid);
                if (p)
                        return 0;
                if (errno != 0)
                        return -errno;

                errno = 0;
                g = getgrgid((gid_t) uid);
                if (g) {
                        if (!streq(g->gr_name, name))
                                return 0;
                } else if (errno != 0)
                        return -errno;
        }

        return 1;
}

static int root_stat(const char *p, struct stat *st) {
        const char *fix;

        fix = fix_root(p);
        if (stat(fix, st) < 0)
                return -errno;

        return 0;
}

static int read_id_from_file(Item *i, uid_t *_uid, gid_t *_gid) {
        struct stat st;
        bool found_uid = false, found_gid = false;
        uid_t uid;
        gid_t gid;

        assert(i);

        /* First, try to get the gid directly */
        if (_gid && i->gid_path && root_stat(i->gid_path, &st) >= 0) {
                gid = st.st_gid;
                found_gid = true;
        }

        /* Then, try to get the uid directly */
        if ((_uid || (_gid && !found_gid))
            && i->uid_path
            && root_stat(i->uid_path, &st) >= 0) {

                uid = st.st_uid;
                found_uid = true;

                /* If we need the gid, but had no success yet, also derive it from the uid path */
                if (_gid && !found_gid) {
                        gid = st.st_gid;
                        found_gid = true;
                }
        }

        /* If that didn't work yet, then let's reuse the gid as uid */
        if (_uid && !found_uid && i->gid_path) {

                if (found_gid) {
                        uid = (uid_t) gid;
                        found_uid = true;
                } else if (root_stat(i->gid_path, &st) >= 0) {
                        uid = (uid_t) st.st_gid;
                        found_uid = true;
                }
        }

        if (_uid) {
                if (!found_uid)
                        return 0;

                *_uid = uid;
        }

        if (_gid) {
                if (!found_gid)
                        return 0;

                *_gid = gid;
        }

        return 1;
}

static int add_user(Item *i) {
        void *z;
        int r;

        assert(i);

        /* Check the database directly */
        z = hashmap_get(database_user, i->name);
        if (z) {
                log_debug("User %s already exists.", i->name);
                i->uid = PTR_TO_GID(z);
                i->uid_set = true;
                return 0;
        }

        if (!arg_root) {
                struct passwd *p;
                struct spwd *sp;

                /* Also check NSS */
                errno = 0;
                p = getpwnam(i->name);
                if (p) {
                        log_debug("User %s already exists.", i->name);
                        i->uid = p->pw_uid;
                        i->uid_set = true;

                        free(i->description);
                        i->description = strdup(p->pw_gecos);
                        return 0;
                }
                if (errno != 0) {
                        log_error("Failed to check if user %s already exists: %m", i->name);
                        return -errno;
                }

                /* And shadow too, just to be sure */
                errno = 0;
                sp = getspnam(i->name);
                if (sp) {
                        log_error("User %s already exists in shadow database, but not in user database.", i->name);
                        return -EBADMSG;
                }
                if (errno != 0) {
                        log_error("Failed to check if user %s already exists in shadow database: %m", i->name);
                        return -errno;
                }
        }

        /* Try to use the suggested numeric uid */
        if (i->uid_set) {
                r = uid_is_ok(i->uid, i->name);
                if (r < 0) {
                        log_error("Failed to verify uid " UID_FMT ": %s", i->uid, strerror(-r));
                        return r;
                }
                if (r == 0) {
                        log_debug("Suggested user ID " UID_FMT " for %s already used.", i->uid, i->name);
                        i->uid_set = false;
                }
        }

        /* If that didn't work, try to read it from the specified path */
        if (!i->uid_set) {
                uid_t c;

                if (read_id_from_file(i, &c, NULL) > 0) {

                        if (c <= 0 || c > SYSTEM_UID_MAX)
                                log_debug("User ID " UID_FMT " of file not suitable for %s.", c, i->name);
                        else {
                                r = uid_is_ok(c, i->name);
                                if (r < 0) {
                                        log_error("Failed to verify uid " UID_FMT ": %s", i->uid, strerror(-r));
                                        return r;
                                } else if (r > 0) {
                                        i->uid = c;
                                        i->uid_set = true;
                                } else
                                        log_debug("User ID " UID_FMT " of file for %s is already used.", c, i->name);
                        }
                }
        }

        /* Otherwise try to reuse the group ID */
        if (!i->uid_set && i->gid_set) {
                r = uid_is_ok((uid_t) i->gid, i->name);
                if (r < 0) {
                        log_error("Failed to verify uid " UID_FMT ": %s", i->uid, strerror(-r));
                        return r;
                }
                if (r > 0) {
                        i->uid = (uid_t) i->gid;
                        i->uid_set = true;
                }
        }

        /* And if that didn't work either, let's try to find a free one */
        if (!i->uid_set) {
                for (; search_uid > 0; search_uid--) {

                        r = uid_is_ok(search_uid, i->name);
                        if (r < 0) {
                                log_error("Failed to verify uid " UID_FMT ": %s", i->uid, strerror(-r));
                                return r;
                        } else if (r > 0)
                                break;
                }

                if (search_uid <= 0) {
                        log_error("No free user ID available for %s.", i->name);
                        return -E2BIG;
                }

                i->uid_set = true;
                i->uid = search_uid;

                search_uid--;
        }

        r = hashmap_ensure_allocated(&todo_uids, trivial_hash_func, trivial_compare_func);
        if (r < 0)
                return log_oom();

        r = hashmap_put(todo_uids, UID_TO_PTR(i->uid), i);
        if (r < 0)
                return log_oom();

        i->todo = true;
        log_info("Creating user %s (%s) with uid " UID_FMT " and gid " GID_FMT ".", i->name, strna(i->description), i->uid, i->gid);

        return 0;
}

static int gid_is_ok(gid_t gid) {
        struct group *g;
        struct passwd *p;

        if (hashmap_get(todo_gids, GID_TO_PTR(gid)))
                return 0;

        /* Avoid reusing gids that are already used by a different user */
        if (hashmap_get(todo_uids, UID_TO_PTR(gid)))
                return 0;

        if (hashmap_contains(database_gid, GID_TO_PTR(gid)))
                return 0;

        if (hashmap_contains(database_uid, UID_TO_PTR(gid)))
                return 0;

        if (!arg_root) {
                errno = 0;
                g = getgrgid(gid);
                if (g)
                        return 0;
                if (errno != 0)
                        return -errno;

                errno = 0;
                p = getpwuid((uid_t) gid);
                if (p)
                        return 0;
                if (errno != 0)
                        return -errno;
        }

        return 1;
}

static int add_group(Item *i) {
        void *z;
        int r;

        assert(i);

        /* Check the database directly */
        z = hashmap_get(database_group, i->name);
        if (z) {
                log_debug("Group %s already exists.", i->name);
                i->gid = PTR_TO_GID(z);
                i->gid_set = true;
                return 0;
        }

        /* Also check NSS */
        if (!arg_root) {
                struct group *g;

                errno = 0;
                g = getgrnam(i->name);
                if (g) {
                        log_debug("Group %s already exists.", i->name);
                        i->gid = g->gr_gid;
                        i->gid_set = true;
                        return 0;
                }
                if (errno != 0) {
                        log_error("Failed to check if group %s already exists: %m", i->name);
                        return -errno;
                }
        }

        /* Try to use the suggested numeric gid */
        if (i->gid_set) {
                r = gid_is_ok(i->gid);
                if (r < 0) {
                        log_error("Failed to verify gid " GID_FMT ": %s", i->gid, strerror(-r));
                        return r;
                }
                if (r == 0) {
                        log_debug("Suggested group ID " GID_FMT " for %s already used.", i->gid, i->name);
                        i->gid_set = false;
                }
        }

        /* Try to reuse the numeric uid, if there's one */
        if (!i->gid_set && i->uid_set) {
                r = gid_is_ok((gid_t) i->uid);
                if (r < 0) {
                        log_error("Failed to verify gid " GID_FMT ": %s", i->gid, strerror(-r));
                        return r;
                }
                if (r > 0) {
                        i->gid = (gid_t) i->uid;
                        i->gid_set = true;
                }
        }

        /* If that didn't work, try to read it from the specified path */
        if (!i->gid_set) {
                gid_t c;

                if (read_id_from_file(i, NULL, &c) > 0) {

                        if (c <= 0 || c > SYSTEM_GID_MAX)
                                log_debug("Group ID " GID_FMT " of file not suitable for %s.", c, i->name);
                        else {
                                r = gid_is_ok(c);
                                if (r < 0) {
                                        log_error("Failed to verify gid " GID_FMT ": %s", i->gid, strerror(-r));
                                        return r;
                                } else if (r > 0) {
                                        i->gid = c;
                                        i->gid_set = true;
                                } else
                                        log_debug("Group ID " GID_FMT " of file for %s already used.", c, i->name);
                        }
                }
        }

        /* And if that didn't work either, let's try to find a free one */
        if (!i->gid_set) {
                for (; search_gid > 0; search_gid--) {
                        r = gid_is_ok(search_gid);
                        if (r < 0) {
                                log_error("Failed to verify gid " GID_FMT ": %s", i->gid, strerror(-r));
                                return r;
                        } else if (r > 0)
                                break;
                }

                if (search_gid <= 0) {
                        log_error("No free group ID available for %s.", i->name);
                        return -E2BIG;
                }

                i->gid_set = true;
                i->gid = search_gid;

                search_gid--;
        }

        r = hashmap_ensure_allocated(&todo_gids, trivial_hash_func, trivial_compare_func);
        if (r < 0)
                return log_oom();

        r = hashmap_put(todo_gids, GID_TO_PTR(i->gid), i);
        if (r < 0)
                return log_oom();

        i->todo = true;
        log_info("Creating group %s with gid " GID_FMT ".", i->name, i->gid);

        return 0;
}

static int process_item(Item *i) {
        int r;

        assert(i);

        switch (i->type) {

        case ADD_USER:
                r = add_group(i);
                if (r < 0)
                        return r;

                return add_user(i);

        case ADD_GROUP: {
                Item *j;

                j = hashmap_get(users, i->name);
                if (j) {
                        /* There's already user to be created for this
                         * name, let's process that in one step */

                        if (i->gid_set) {
                                j->gid = i->gid;
                                j->gid_set = true;
                        }

                        if (i->gid_path) {
                                free(j->gid_path);
                                j->gid_path = strdup(i->gid_path);
                                if (!j->gid_path)
                                        return log_oom();
                        }

                        return 0;
                }

                return add_group(i);
        }
        }

        assert_not_reached("Unknown item type");
}

static void item_free(Item *i) {

        if (!i)
                return;

        free(i->name);
        free(i->uid_path);
        free(i->gid_path);
        free(i->description);
        free(i);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(Item*, item_free);

static bool item_equal(Item *a, Item *b) {
        assert(a);
        assert(b);

        if (a->type != b->type)
                return false;

        if (!streq_ptr(a->name, b->name))
                return false;

        if (!streq_ptr(a->uid_path, b->uid_path))
                return false;

        if (!streq_ptr(a->gid_path, b->gid_path))
                return false;

        if (!streq_ptr(a->description, b->description))
                return false;

        if (a->uid_set != b->uid_set)
                return false;

        if (a->uid_set && a->uid != b->uid)
                return false;

        if (a->gid_set != b->gid_set)
                return false;

        if (a->gid_set && a->gid != b->gid)
                return false;

        return true;
}

static bool valid_user_group_name(const char *u) {
        const char *i;
        long sz;

        if (isempty(u) < 0)
                return false;

        if (!(u[0] >= 'a' && u[0] <= 'z') &&
            !(u[0] >= 'A' && u[0] <= 'Z') &&
            u[0] != '_')
                return false;

        for (i = u+1; *i; i++) {
                if (!(*i >= 'a' && *i <= 'z') &&
                    !(*i >= 'A' && *i <= 'Z') &&
                    !(*i >= '0' && *i <= '9') &&
                    *i != '_' &&
                    *i != '-')
                        return false;
        }

        sz = sysconf(_SC_LOGIN_NAME_MAX);
        assert_se(sz > 0);

        if ((size_t) (i-u) > (size_t) sz)
                return false;

        return true;
}

static bool valid_gecos(const char *d) {

        if (!utf8_is_valid(d))
                return false;

        if (strpbrk(d, ":\n"))
                return false;

        return true;
}

static int parse_line(const char *fname, unsigned line, const char *buffer) {

        static const Specifier specifier_table[] = {
                { 'm', specifier_machine_id, NULL },
                { 'b', specifier_boot_id, NULL },
                { 'H', specifier_host_name, NULL },
                { 'v', specifier_kernel_release, NULL },
                {}
        };

        _cleanup_free_ char *action = NULL, *name = NULL, *id = NULL;
        _cleanup_(item_freep) Item *i = NULL;
        Item *existing;
        Hashmap *h;
        int r, n = -1;

        assert(fname);
        assert(line >= 1);
        assert(buffer);

        r = sscanf(buffer,
                   "%ms %ms %ms %n",
                   &action,
                   &name,
                   &id,
                   &n);
        if (r < 2) {
                log_error("[%s:%u] Syntax error.", fname, line);
                return -EIO;
        }

        if (strlen(action) != 1) {
                log_error("[%s:%u] Unknown modifier '%s'", fname, line, action);
                return -EINVAL;
        }

        if (!IN_SET(action[0], ADD_USER, ADD_GROUP)) {
                log_error("[%s:%u] Unknown command command type '%c'.", fname, line, action[0]);
                return -EBADMSG;
        }

        i = new0(Item, 1);
        if (!i)
                return log_oom();

        i->type = action[0];

        r = specifier_printf(name, specifier_table, NULL, &i->name);
        if (r < 0) {
                log_error("[%s:%u] Failed to replace specifiers: %s", fname, line, name);
                return r;
        }

        if (!valid_user_group_name(i->name)) {
                log_error("[%s:%u] '%s' is not a valid user or group name.", fname, line, i->name);
                return -EINVAL;
        }

        if (n >= 0) {
                n += strspn(buffer+n, WHITESPACE);
                if (buffer[n] != 0 && (buffer[n] != '-' || buffer[n+1] != 0)) {
                        i->description = unquote(buffer+n, "\"");
                        if (!i->description)
                                return log_oom();

                        if (!valid_gecos(i->description)) {
                                log_error("[%s:%u] '%s' is not a valid GECOS field.", fname, line, i->description);
                                return -EINVAL;
                        }
                }
        }

        if (id && !streq(id, "-")) {

                if (path_is_absolute(id)) {
                        char *p;

                        p = strdup(id);
                        if (!p)
                                return log_oom();

                        path_kill_slashes(p);

                        if (i->type == ADD_USER)
                                i->uid_path = p;
                        else
                                i->gid_path = p;

                } else if (i->type == ADD_USER) {
                        r = parse_uid(id, &i->uid);
                        if (r < 0) {
                                log_error("Failed to parse UID: %s", id);
                                return -EBADMSG;
                        }

                        i->uid_set = true;

                } else {
                        assert(i->type == ADD_GROUP);

                        r = parse_gid(id, &i->gid);
                        if (r < 0) {
                                log_error("Failed to parse GID: %s", id);
                                return -EBADMSG;
                        }

                        i->gid_set = true;
                }
        }

        if (i->type == ADD_USER) {
                r = hashmap_ensure_allocated(&users, string_hash_func, string_compare_func);
                h = users;
        } else {
                assert(i->type == ADD_GROUP);
                r = hashmap_ensure_allocated(&groups, string_hash_func, string_compare_func);
                h = groups;
        }
        if (r < 0)
                return log_oom();

        existing = hashmap_get(h, i->name);
        if (existing) {

                /* Two identical items are fine */
                if (!item_equal(existing, i))
                        log_warning("Two or more conflicting lines for %s configured, ignoring.", i->name);

                return 0;
        }

        r = hashmap_put(h, i->name, i);
        if (r < 0) {
                log_error("Failed to insert item %s: %s", i->name, strerror(-r));
                return r;
        }

        i = NULL;
        return 0;
}

static int read_config_file(const char *fn, bool ignore_enoent) {
        _cleanup_fclose_ FILE *f = NULL;
        char line[LINE_MAX];
        unsigned v = 0;
        int r;

        assert(fn);

        r = search_and_fopen_nulstr(fn, "re", arg_root, conf_file_dirs, &f);
        if (r < 0) {
                if (ignore_enoent && r == -ENOENT)
                        return 0;

                log_error("Failed to open '%s', ignoring: %s", fn, strerror(-r));
                return r;
        }

        FOREACH_LINE(line, f, break) {
                char *l;
                int k;

                v++;

                l = strstrip(line);
                if (*l == '#' || *l == 0)
                        continue;

                k = parse_line(fn, v, l);
                if (k < 0 && r == 0)
                        r = k;
        }

        if (ferror(f)) {
                log_error("Failed to read from file %s: %m", fn);
                if (r == 0)
                        r = -EIO;
        }

        return r;
}

static int take_lock(void) {

        struct flock flock = {
                .l_type = F_WRLCK,
                .l_whence = SEEK_SET,
                .l_start = 0,
                .l_len = 0,
        };

        const char *path;
        int fd, r;

        /* This is roughly the same as lckpwdf(), but not as awful. We
         * don't want to use alarm() and signals, hence we implement
         * our own trivial version of this.
         *
         * Note that shadow-utils also takes per-database locks in
         * addition to lckpwdf(). However, we don't given that they
         * are redundant as they they invoke lckpwdf() first and keep
         * it during everything they do. The per-database locks are
         * awfully racy, and thus we just won't do them. */

        path = fix_root("/etc/.pwd.lock");
        fd = open(path, O_WRONLY|O_CREAT|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW, 0600);
        if (fd < 0)
                return -errno;

        r = fcntl(fd, F_SETLKW, &flock);
        if (r < 0) {
                safe_close(fd);
                return -errno;
        }

        return fd;
}

static void free_database(Hashmap *by_name, Hashmap *by_id) {
        char *name;

        for (;;) {
                name = hashmap_first(by_id);
                if (!name)
                        break;

                hashmap_remove(by_name, name);

                hashmap_steal_first_key(by_id);
                free(name);
        }

        while ((name = hashmap_steal_first_key(by_name)))
                free(name);

        hashmap_free(by_name);
        hashmap_free(by_id);
}

static int help(void) {

        printf("%s [OPTIONS...] [CONFIGURATION FILE...]\n\n"
               "Creates system user accounts.\n\n"
               "  -h --help                 Show this help\n"
               "     --version              Show package version\n"
               "     --root=PATH            Operate on an alternate filesystem root\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_ROOT,
        };

        static const struct option options[] = {
                { "help",    no_argument,       NULL, 'h'         },
                { "version", no_argument,       NULL, ARG_VERSION },
                { "root",    required_argument, NULL, ARG_ROOT    },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case ARG_ROOT:
                        free(arg_root);
                        arg_root = path_make_absolute_cwd(optarg);
                        if (!arg_root)
                                return log_oom();

                        path_kill_slashes(arg_root);
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }
        }

        return 1;
}

int main(int argc, char *argv[]) {

        _cleanup_close_ int lock = -1;
        Iterator iterator;
        int r, k;
        Item *i;

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        r = 0;

        if (optind < argc) {
                int j;

                for (j = optind; j < argc; j++) {
                        k = read_config_file(argv[j], false);
                        if (k < 0 && r == 0)
                                r = k;
                }
        } else {
                _cleanup_strv_free_ char **files = NULL;
                char **f;

                r = conf_files_list_nulstr(&files, ".conf", arg_root, conf_file_dirs);
                if (r < 0) {
                        log_error("Failed to enumerate sysusers.d files: %s", strerror(-r));
                        goto finish;
                }

                STRV_FOREACH(f, files) {
                        k = read_config_file(*f, true);
                        if (k < 0 && r == 0)
                                r = k;
                }
        }

        lock = take_lock();
        if (lock < 0) {
                log_error("Failed to take lock: %s", strerror(-lock));
                goto finish;
        }

        r = load_user_database();
        if (r < 0) {
                log_error("Failed to load user database: %s", strerror(-r));
                goto finish;
        }

        r = load_group_database();
        if (r < 0) {
                log_error("Failed to read group database: %s", strerror(-r));
                goto finish;
        }

        HASHMAP_FOREACH(i, groups, iterator)
                process_item(i);

        HASHMAP_FOREACH(i, users, iterator)
                process_item(i);

        r = write_files();
        if (r < 0)
                log_error("Failed to write files: %s", strerror(-r));

finish:
        while ((i = hashmap_steal_first(groups)))
                item_free(i);

        while ((i = hashmap_steal_first(users)))
                item_free(i);

        hashmap_free(groups);
        hashmap_free(users);
        hashmap_free(todo_uids);
        hashmap_free(todo_gids);

        free_database(database_user, database_uid);
        free_database(database_group, database_gid);

        free(arg_root);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}