/*
 * Copyright 2010 Volkan Yazıcı <volkan.yazici@gmail.com>
 * Copyright 2006-2010 The Apache Software Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../../memory_.h"

#include "pqueue.h"

/* Hard-coded cmp/get/set function. */

static inline priority_queue_pri_t getpri(hlt_timer* a)
{
    return a->time;
}

static inline void setpri(hlt_timer* a, priority_queue_pri_t pri)
{
    a->time = pri;
}

static inline int cmppri(priority_queue_pri_t next, priority_queue_pri_t curr)
{
    return next > curr; // Inverse sorting. 
}

static inline size_t getpos(hlt_timer* a)
{
    return a->queue_pos;
}

static inline void setpos(hlt_timer* a, size_t pos)
{
    a->queue_pos = pos;
}

#define left(i)   ((i) << 1)
#define right(i)  (((i) << 1) + 1)
#define parent(i) ((i) >> 1)

priority_queue_t *
priority_queue_init(size_t n)
{
    priority_queue_t *q;

    if (!(q = hlt_malloc(sizeof(priority_queue_t))))
        return NULL;

    /* Need to allocate n+1 elements since element 0 isn't used. */
    if (!(q->d = hlt_malloc((n + 1) * sizeof(void *)))) {
        hlt_free(q);
        return NULL;
    }

    q->size = 1;
    q->avail = q->step = (n+1);  /* see comment above about n+1 */

    return q;
}


void
priority_queue_free(priority_queue_t *q)
{
    hlt_free(q->d);
    hlt_free(q);
}

size_t
priority_queue_size(priority_queue_t *q)
{
    /* queue element 0 exists but doesn't count since it isn't used. */
    return (q->size - 1);
}


static void
bubble_up(priority_queue_t *q, size_t i)
{
    size_t parent_node;
    void *moving_node = q->d[i];
    priority_queue_pri_t moving_pri = getpri(moving_node);

    for (parent_node = parent(i);
         ((i > 1) && cmppri(getpri(q->d[parent_node]), moving_pri));
         i = parent_node, parent_node = parent(i))
    {
        q->d[i] = q->d[parent_node];
        setpos(q->d[i], i);
    }

    q->d[i] = moving_node;
    setpos(moving_node, i);
}


static size_t
maxchild(priority_queue_t *q, size_t i)
{
    size_t child_node = left(i);

    if (child_node >= q->size)
        return 0;

    if ((child_node+1) < q->size &&
        cmppri(getpri(q->d[child_node]), getpri(q->d[child_node+1])))
        child_node++; /* use right child instead of left */

    return child_node;
}


static void
percolate_down(priority_queue_t *q, size_t i)
{
    size_t child_node;
    void *moving_node = q->d[i];
    priority_queue_pri_t moving_pri = getpri(moving_node);

    while ((child_node = maxchild(q, i)) &&
           cmppri(moving_pri, getpri(q->d[child_node])))
    {
        q->d[i] = q->d[child_node];
        setpos(q->d[i], i);
        i = child_node;
    }

    q->d[i] = moving_node;
    setpos(moving_node, i);
}


int
priority_queue_insert(priority_queue_t *q, void *d)
{
    void *tmp;
    size_t i;
    size_t newsize;

    if (!q) return 1;

    /* allocate more memory if necessary */
    if (q->size >= q->avail) {
        newsize = q->size + q->step;
        if (!(tmp = hlt_realloc_no_init(q->d, sizeof(void *) * newsize)))
            return 1;
        q->d = tmp;
        q->avail = newsize;
    }

    /* insert item */
    i = q->size++;
    q->d[i] = d;
    bubble_up(q, i);

    return 0;
}


void
priority_queue_change_priority(priority_queue_t *q,
                       priority_queue_pri_t new_pri,
                       void *d)
{
    size_t posn;
    priority_queue_pri_t old_pri = getpri(d);

    setpri(d, new_pri);
    posn = getpos(d);
    if (cmppri(old_pri, new_pri))
        bubble_up(q, posn);
    else
        percolate_down(q, posn);
}


int
priority_queue_remove(priority_queue_t *q, void *d)
{
    size_t posn = getpos(d);
    q->d[posn] = q->d[--q->size];
    if (cmppri(getpri(d), getpri(q->d[posn])))
        bubble_up(q, posn);
    else
        percolate_down(q, posn);

    return 0;
}


void *
priority_queue_pop(priority_queue_t *q)
{
    void *head;

    if (!q || q->size == 1)
        return NULL;

    head = q->d[1];
    q->d[1] = q->d[--q->size];
    percolate_down(q, 1);

    return head;
}


void *
priority_queue_peek(priority_queue_t *q)
{
    void *d;
    if (!q || q->size == 1)
        return NULL;
    d = q->d[1];
    return d;
}

#if 0 
void
priority_queue_dump(priority_queue_t *q,
            FILE *out,
            priority_queue_print_entry_f print)
{
    int i;

    fprintf(stdout,"posn\tleft\tright\tparent\tmaxchild\t...\n");
    for (i = 1; i < q->size ;i++) {
        fprintf(stdout,
                "%d\t%d\t%d\t%d\t%ul\t",
                i,
                left(i), right(i), parent(i),
                (unsigned int)maxchild(q, i));
        print(out, q->d[i]);
    }
}
#endif


#if 0
static void
set_pos(void *d, size_t val)
{
    /* do nothing */
}


static void
set_pri(void *d, priority_queue_pri_t pri)
{
    /* do nothing */
}
#endif


#if 0
void
priority_queue_print(priority_queue_t *q,
             FILE *out,
             priority_queue_print_entry_f print)
{
    priority_queue_t *dup;
	void *e;

    dup = priority_queue_init(q->size);
    dup->size = q->size;
    dup->avail = q->avail;
    dup->step = q->step;

    memcpy(dup->d, q->d, (q->size * sizeof(void *)));

    while ((e = priority_queue_pop(dup)))
		print(out, e);

    priority_queue_free(dup);
}
#endif


static int
subtree_is_valid(priority_queue_t *q, int pos)
{
    if (left(pos) < q->size) {
        /* has a left child */
        if (cmppri(getpri(q->d[pos]), getpri(q->d[left(pos)])))
            return 0;
        if (!subtree_is_valid(q, left(pos)))
            return 0;
    }
    if (right(pos) < q->size) {
        /* has a right child */
        if (cmppri(getpri(q->d[pos]), getpri(q->d[right(pos)])))
            return 0;
        if (!subtree_is_valid(q, right(pos)))
            return 0;
    }
    return 1;
}


int
priority_queue_is_valid(priority_queue_t *q)
{
    return subtree_is_valid(q, 1);
}
