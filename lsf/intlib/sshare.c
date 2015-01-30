/*
 * Copyright (C) 2014 - 2015 David Bigagli
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA
 */

/* sshare = support share
 * Routines and data structure to support hierarchical
 * fairshare in MBD. Implemented in lsf/intlib for eventual
 * reuse somewhere, someday, somehow.
 */

#include "sshare.h"
#include <assert.h>

static link_t *parse_user_shares(const char *);
static link_t *parse_group_member(const char *,
                                  uint32_t,
                                  struct group_acct *);
static struct share_acct *get_sacct(const char *,
                                    const char *);
static uint32_t compute_slots(struct tree_node_ *, uint32_t, uint32_t);
static void sort_siblings(struct tree_node_ *,
                          int (*cmp)(const void *, const void *));
static void tokenize(char *);
static char *get_next_word(char **);
static int node_cmp(const void *, const void *);
static int node_cmp2(const void *, const void *);
static int print_node(struct tree_node_ *, struct tree_ *);
static uint32_t harvest_leafs(struct tree_ *);
static void distribute_more(struct tree_ *, uint32_t);
static uint32_t compute_distance(struct tree_node_ *, uint32_t, uint32_t);
static void sort_tree_by_ran(struct tree_ *);

/* sshare_make_tree()
 */
struct tree_ *sshare_make_tree(const char *user_shares,
                               uint32_t num_grp,
                               struct group_acct *grp)
{
    struct tree_ *t;
    link_t *l;
    link_t *stack;
    linkiter_t iter;
    struct share_acct *sacct;
    struct tree_node_ *n;
    struct tree_node_ *root;

    stack = make_link();
    t = tree_init("");
    /* root summarizes all the tree counters
     */
    t->root->data = make_sacct("/", 1);
    root = NULL;

    l = parse_user_shares(user_shares);

z:
    if (root)
        l = parse_group_member(n->path, num_grp, grp);
    else
        root = t->root;

    traverse_init(l, &iter);
    while ((sacct = traverse_link(&iter))) {

        n = calloc(1, sizeof(struct tree_node_));
        n->path = sacct->name;

        n = tree_insert_node(root, n);
        enqueue_link(stack, n);
        n->data = sacct;
    }

    /* Sort by shares so the tree
     * is always sorted by share
     * priority
     */
    sort_siblings(root, node_cmp);

    fin_link(l);

    n = pop_link(stack);
    if (n) {
        root = n;
        goto z;
    }

    fin_link(stack);

    n = t->root;
    while ((n = tree_next_node(n))) {
        char buf[BUFSIZ];
        /* Create the hash table of nodes and their
         * immediate parent.
         */
        if (n->child == NULL) {
            sprintf(buf, "%s/%s", n->parent->path, n->path);
            hash_install(t->node_tab, buf, n, NULL);
        }
        sprintf(buf, "%s", n->path);
        hash_install(t->node_tab, buf, n, NULL);
        print_node(n, t);
    }

    traverse_init(t->leafs, &iter);
    while ((n = traverse_link(&iter)))
        print_node(n, t);

    /* Fairshare tree is built and sorted
     * by decreasing shares, the scheduler
     * algorithm will fill it up with
     * slots from now on.
     */
    tree_walk2(t, print_node);

    return t;
}

/* sshare_distribute_slots()
 */
int
sshare_distribute_slots(struct tree_ *t,
                        uint32_t slots)
{
    struct tree_node_ *n;
    link_t *stack;
    struct share_acct *sacct;
    uint32_t avail;
    uint32_t free;

    stack = make_link();
    n = t->root->child;
    /* This must be emptied after every scheduling
     * cycle. There could be still some leafs
     * if not all jobs got dispatched.
     */
    while (pop_link(t->leafs))
        ;
    avail = slots;

znovu:

    /* Iterate at each tree level but
     * don't traverse branches without
     * tokens.
     */
    while (n) {

        /* enqueue as we want to traverse
         * the tree by priority
         */
        if (n->child)
            enqueue_link(stack, n);

        sacct = n->data;
        sacct->sent = 0;
        sacct->dsrv = compute_slots(n, slots, avail);
        avail = avail - sacct->dsrv;

        assert(avail >= 0);
        /* As we traverse in priority order
         * the leafs are also sorted
         */
        if (n->child == NULL)
            enqueue_link(t->leafs, n);

        n = n->right;
    }

    n = pop_link(stack);
    if (n) {
        /* tokens come from the parent
         */
        sacct = n->data;
        avail = slots = sacct->dsrv;
        n = n->child;
        goto znovu;
    }

    fin_link(stack);

    free = harvest_leafs(t);

    if (0 && free > 0)
        distribute_more(t, free);

    return 0;
}

/* make_saccount()
 */
struct share_acct *
make_sacct(const char *name, uint32_t shares)
{
    struct share_acct *s;

    s = calloc(1, sizeof(struct share_acct));
    s->name = strdup(name);
    s->shares = shares;

    return s;
}

/* free_sacct()
 */
void
free_sacct(struct share_acct *sacct)
{
    _free_(sacct->name);
    _free_(sacct);
}

/* compute_slots()
 */
static uint32_t
compute_slots(struct tree_node_ *n, uint32_t total, uint32_t avail)
{
    struct share_acct *s;
    double q;
    uint32_t u;
    uint32_t dsrv;

    s = n->data;

    q = s->dshares * (double)total;
    u = (uint32_t)ceil(q);
    dsrv = MIN(u, avail);

    return dsrv;
}

/* harvest_leafs()
 */
static uint32_t
harvest_leafs(struct tree_ *t)
{
    struct share_acct *s;
    uint32_t free;
    int32_t z;
    int32_t d;
    linkiter_t iter;
    struct tree_node_ *n;

    free = 0;
    traverse_init(t->leafs, &iter);
    while ((n = traverse_link(&iter))) {

        s = n->data;
        /* if allocated or over
         * skip
         */
        d = s->dsrv - s->numRUN;
        if (d <= 0)
            continue;

        /* if pend more or equal
         * than d skip
         */
        z = d - s->numPEND;
        if (z <= 0) {
            s->sent = d;
            continue;
        }
        s->sent = s->numPEND;
        free = free + z;
    }

    return free;
}

/* distribute_more()
 *
 * First sort the tree by total ran job
 * the redistribute the eventual free
 */
static void
distribute_more(struct tree_ *t, uint32_t free)
{
    sort_tree_by_ran(t);
}

/* sort_tree_by_ran()
 */
static void
sort_tree_by_ran(struct tree_ *t)
{
    struct tree_node_ *n;
    struct tree_node_ *n2;
    link_t *stack;
    struct share_acct *s;
    uint64_t sum;
    uint64_t avail;

    stack = make_link();
    n = t->root->child;

znovu:
    n2 = n;
    sum = 0;
    while (n) {

        if (n->child)
            push_link(stack, n);

        s = n->data;
        sum = sum + s->numRAN;
        n = n->right;
    }

    avail = sum;
    n = n2;
    while (n) {

        avail = avail - compute_distance(n, sum, avail);
        n = n->right;
    }

    sort_siblings(n2, node_cmp2);

    n = pop_link(stack);
    if (n)
        goto znovu;

    fin_link(stack);
}

/* compute_distance()
 */
static uint32_t
compute_distance(struct tree_node_ *n,
                 uint32_t sum,
                 uint32_t avail)
{
    double q;
    struct share_acct *s;
    uint32_t u;
    uint32_t use;

    s = n->data;

    q = s->dshares * (double)sum;
    u = (int32_t)ceil(q);
    use = MIN(u, avail);
    s->dsrv2 = s->numRAN - use;

    return use;
}

/* sort_siblings()
 */
static void
sort_siblings(struct tree_node_ *root,
              int (*cmp)(const void *, const void *))

{
    struct tree_node_ *n;
    int num;
    int i;
    struct tree_node_ **v;

    if (root->child == NULL)
        return;

    if (root->child->right == NULL)
        return;

    num = 0;
    n = root->child;
    while (n) {
        ++num;
        n = n->right;
    }

    v = calloc(num, sizeof(struct tree_node_ *));
    n = root->child;
    num = 0;
    while (n) {
        tree_rm_node(n);
        v[num] = n;
        ++num;
        n = n->right;
    }

    /* We want to sort in ascending order as we use
     * tree_inser_node() which always inserts
     * node in the left most position.
     */
    qsort(v, num, sizeof(struct tree_node_ *), cmp);

    for (i = 0; i < num; i++) {
        tree_insert_node(root, v[i]);
    }

    free(v);
}

/* parse_user_shares()
 *
 * parse user_shares[[g, 1] [e,1]]
 */
static link_t *
parse_user_shares(const char *user_shares)
{
    link_t *l;
    char *p;
    char *u;
    int cc;
    int n;
    struct share_acct *sacct;
    uint32_t sum_shares;
    linkiter_t iter;

    u = strdup(user_shares);
    assert(u);

    p = strchr(u, '[');
    *p++ = 0;

    tokenize(p);

    l = make_link();
    sum_shares = 0;

    while (1 ) {
        char name[128];
        uint32_t shares;

        cc = sscanf(p, "%s%u%n", name, &shares, &n);
        if (cc == EOF)
            break;
        if (cc != 2)
            goto bail;
        p = p + n;

        sacct = make_sacct(name, shares);
        assert(sacct);

        sum_shares = sum_shares + sacct->shares;

        enqueue_link(l, sacct);
    }

    traverse_init(l, &iter);
    while ((sacct = traverse_link(&iter))) {
        sacct->dshares = (double)sacct->shares/(double)sum_shares;
    }

    _free_(u);
    return l;

bail:

    _free_(u);
    traverse_init(l, &iter);
    while ((sacct = traverse_link(&iter)))
        free_sacct(sacct);
    fin_link(l);

    return NULL;
}

/* parse_group_member()
 */
static link_t *
parse_group_member(const char *gname,
                   uint32_t num,
                   struct group_acct *grps)
{
    link_t *l;
    linkiter_t iter;
    struct group_acct *g;
    int cc;
    char *w;
    char *p;
    uint32_t sum;
    struct share_acct *sacct;

    g = NULL;
    for (cc = 0; cc < num; cc++) {

        if (strcmp(gname, grps[cc].group) == 0) {
            g = calloc(1, sizeof(struct group_acct));
            assert(g);
            g->group = strdup(grps[cc].group);
            g->memberList = strdup(grps[cc].memberList);
            g->user_shares = strdup(grps[cc].user_shares);
            tokenize(g->user_shares);
            break;
        }
    }

    /* gudness...
     */
    if (g == NULL)
        return NULL;

    p = g->memberList;
    l = make_link();
    sum = 0;
    while ((w = get_next_word(&p))) {

        sacct = get_sacct(w, g->user_shares);
        sum = sum + sacct->shares;
        enqueue_link(l, sacct);
    }

    traverse_init(l, &iter);
    while ((sacct = traverse_link(&iter))) {
        sacct->dshares = (double)sacct->shares/(double)sum;
    }

    _free_(g->group);
    _free_(g->memberList);
    _free_(g->user_shares);
    _free_(g);

    return l;
}

static struct share_acct *
get_sacct(const char *acct_name, const char *user_list)
{
    char name[128];
    uint32_t shares;
    int cc;
    int n;
    struct share_acct *sacct;
    char *p;
    char *p0;

    p0 = p = strdup(user_list);

    while (1) {

        cc = sscanf(p, "%s%u%n", name, &shares, &n);
        if (cc == EOF)
            break;
        if (strcmp(name, acct_name) != 0) {
            p = p + n;
            continue;
        }
        sacct = make_sacct(name, shares);
        break;
    }

    _free_(p0);
    return sacct;
}

static void
tokenize(char *p)
{
    int cc;
    int l;

    l = strlen(p);

    for (cc = 0; cc < l; cc++) {
        if (p[cc] == '['
            || p[cc] == ']'
            || p[cc] == ',')
            p[cc] = ' ';
    }
}

static char *
get_next_word(char **line)
{
    static char word[BUFSIZ];
    char *wordp = word;

    while (isspace(**line))
        (*line)++;

    while (**line && !isspace(**line))
        *wordp++ = *(*line)++;

    if (wordp == word)
        return NULL;

    *wordp = '\0';
    return word;
}

/* node_cmp()
 *
 * Function for qsort()
 */
static int
node_cmp(const void *x, const void *y)
{
    struct tree_node_ *n1;
    struct tree_node_ *n2;
    struct share_acct *s1;
    struct share_acct *s2;

    n1 = *(struct tree_node_ **)x;
    n2 = *(struct tree_node_ **)y;

    s1 = n1->data;
    s2 = n2->data;

    /* We want to sort in ascending order as we use
     * tree_inser_node() which always inserts
     * node in the left most position.
     */
    if (s1->shares > s2->shares)
        return 1;
    if (s1->shares < s2->shares)
        return -1;

    return 0;
}

/* node_cmp2()
 */
static int node_cmp2(const void *x, const void *y)
{
    struct tree_node_ *n1;
    struct tree_node_ *n2;
    struct share_acct *s1;
    struct share_acct *s2;

    n1 = *(struct tree_node_ **)x;
    n2 = *(struct tree_node_ **)y;

    s1 = n1->data;
    s2 = n2->data;

    /* We want to sort in ascending order as we use
     * tree_inser_node() which always inserts
     * node in the left most position.
     */
    if (s1->dsrv2 > s2->dsrv2)
        return 1;
    if (s1->dsrv2 < s2->dsrv2)
        return -1;

    return 0;
}

/* print_node()
 */
static int
print_node(struct tree_node_ *n, struct tree_ *t)
{
    struct share_acct *s;

    if (t == NULL
        || n == NULL)
        return -1;

    s = n->data;

    printf("%s: node %s shares %d dshares %4.2f\n",
           __func__, n->path, s->shares, s->dshares);

    return -1;
}
