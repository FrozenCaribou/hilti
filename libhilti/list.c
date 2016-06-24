//
// The list implementation is pretty much a standard double-linked list.
//
// NOTE: Unlike the old libhilti list implementation, there's no free list
// because that doesn't work well with ref'cnt. If we get a problem with too
// many small allocations, we could let the list do its own mem mgt though.

#include <string.h>

#include "autogen/hilti-hlt.h"
#include "list.h"
#include "timer.h"
#include "interval.h"
#include "enum.h"

// Initial allocation size for the free list.
static const uint32_t InitialCapacity = 2;

// Factor by which to growth free array when allocating more memory.
static const float GrowthFactor = 1.5;

struct __hlt_list_node {
    __hlt_gchdr __gchdr;    // Header for memory management.
    __hlt_list_node* next;  // Successor node. Memory-managed.
    __hlt_list_node* prev;  // Predecessor node. Not memory-managed to avoid cycles.
    hlt_timer* timer;       // The entry's timer, or null if none is set. Not memory-managed to avoid cycles.
    const hlt_type_info* type;  // FIXME: Do we get around storing this with each node?
    int64_t invalid;        // True if node has been invalidated.
                            // FIXME: int64_t to align the data; how else to do that?
    char data[];            // Node data starts here, with size determined by elem type.
};

struct __hlt_list {
    __hlt_gchdr __gchdr;         // Header for memory management.
    __hlt_list_node* head;       // First list element. Memory-managed.
    __hlt_list_node* tail;       // Last list element. Not memory-managed to avoid cycles.
    int64_t size;                // Current list size.
    const hlt_type_info* type;   // Element type.
    hlt_timer_mgr* tmgr;         // The timer manager, or null if not used.
    hlt_interval timeout;        // The timeout value, or 0 if disabled.
    hlt_enum strategy;           // Expiration strategy if set; zero otherwise.
};

__HLT_RTTI_GC_TYPE(__hlt_list_node,  HLT_TYPE_LIST_NODE)

void __hlt_list_node_dtor(hlt_type_info* ti, __hlt_list_node* n, hlt_execution_context* ctx)
{
    GC_DTOR(n->next, __hlt_list_node, ctx);
    GC_DTOR_GENERIC(&n->data, n->type, ctx);
    n->next = 0;
}

void hlt_list_dtor(hlt_type_info* ti, hlt_list* l, hlt_execution_context* ctx)
{
    GC_DTOR(l->head, __hlt_list_node, ctx);
    GC_DTOR(l->tmgr, hlt_timer_mgr, ctx);
}

void hlt_iterator_list_cctor(hlt_type_info* ti, hlt_iterator_list* i, hlt_execution_context* ctx)
{
    GC_CCTOR(i->list, hlt_list, ctx);
    GC_CCTOR(i->node, __hlt_list_node, ctx);
}

void hlt_iterator_list_dtor(hlt_type_info* ti, hlt_iterator_list* i, hlt_execution_context* ctx)
{
    GC_DTOR(i->list, hlt_list, ctx);
    GC_DTOR(i->node, __hlt_list_node, ctx);
}

// Inserts n after node pos. If pos is null, inserts at front. n must be all
// zero-initialized and already ref'ed.
static void _link(hlt_list* l, __hlt_list_node* n, __hlt_list_node* pos, hlt_execution_context* ctx)
{
    if ( pos ) {
        if ( pos->next )
            pos->next->prev = n;
        else
            l->tail = n;

        GC_ASSIGN(n->next, pos->next, __hlt_list_node, ctx);
        GC_ASSIGN_REFED(pos->next, n, __hlt_list_node, ctx);
        n->prev = pos;
    }

    else {
        // Insert at head.
        GC_ASSIGN(n->next, l->head, __hlt_list_node, ctx);

        if ( l->head )
            l->head->prev = n;
        else
            l->tail = n;

        GC_ASSIGN_REFED(l->head, n, __hlt_list_node, ctx);
    }

    ++l->size;
}

// Unlinks the node from the list and invalidates it, including stopping its
// timer.
static void _unlink(hlt_list* l, __hlt_list_node* n, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( n->next )
        n->next->prev = n->prev;
    else
        l->tail = n->prev;

    // We don't ref/unref n->next here as we just move ownership. But we
    // ref/unref n once to avoid potentially deleting it while we move
    // pointers around.

    GC_CCTOR(n, __hlt_list_node, ctx);

    if ( n->prev ) {
        GC_ASSIGN_REFED(n->prev->next, n->next, __hlt_list_node, ctx);
    }
    else {
        GC_ASSIGN_REFED(l->head, n->next, __hlt_list_node, ctx);
    }

    n->next = 0;
    n->prev = 0;
    n->invalid = 1;
    --l->size;

    if ( n->timer && ctx )
        hlt_timer_cancel(n->timer, excpt, ctx);

    n->timer = 0;

    GC_DTOR(n, __hlt_list_node, ctx);
}

// Returns a ref'ed node. Val not yet ref'ed.
static __hlt_list_node* _make_node(hlt_list* l, void *val, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __hlt_list_node* n = GC_NEW_CUSTOM_SIZE_REF(__hlt_list_node, sizeof(__hlt_list_node) + l->type->size, ctx);
    n->type = l->type;

    // Other fields are null initialized.

    memcpy(&n->data, val, l->type->size);
    GC_CCTOR_GENERIC(&n->data, l->type, ctx);

    hlt_time t = (l->tmgr && l->timeout) ? hlt_timer_mgr_current(l->tmgr, excpt, ctx) + l->timeout : 0;

    if ( t ) {
        assert(l->tmgr);
        __hlt_list_timer_cookie cookie = { l, n };
        GC_CCTOR(cookie, hlt_iterator_list, ctx);
        n->timer = __hlt_timer_new_list(cookie, excpt, ctx);
        hlt_timer_mgr_schedule(l->tmgr, t, n->timer, excpt, ctx);
    }

    else
        n->timer = 0;

    return n;
}

// Returns true if the node has been marked invalid.
static int _invalid_node(__hlt_list_node* n)
{
    return n->invalid;
}

static inline void _access(hlt_list* l, __hlt_list_node* n, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! l->tmgr || ! hlt_enum_equal(l->strategy, Hilti_ExpireStrategy_Access, excpt, ctx) || l->timeout == 0 )
        return;

    if ( ! n->timer )
        return;

    hlt_time t = hlt_timer_mgr_current(l->tmgr, excpt, ctx) + l->timeout;
    hlt_timer_update(n->timer, t, excpt, ctx);
}

static inline void _hlt_list_init(hlt_list* l, const hlt_type_info* elemtype, hlt_timer_mgr* tmgr, hlt_exception** excpt, hlt_execution_context* ctx)
{
    GC_INIT(l->tmgr, tmgr, hlt_timer_mgr, ctx);
    l->head = l->tail = 0;
    l->size = 0;
    l->type = elemtype;
    l->timeout = 0.0;
    l->strategy = hlt_enum_unset(excpt, ctx);
}

hlt_list* hlt_list_new(const hlt_type_info* elemtype, hlt_timer_mgr* tmgr, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_list* l = GC_NEW(hlt_list, ctx);
    _hlt_list_init(l, elemtype, tmgr, excpt, ctx);
    return l;
}

static void _clone_init_in_thread(const hlt_type_info* ti, void* dstp, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_list* dst = *(hlt_list**)dstp;

    // If we arrive here, it can't be a custom timer mgr but only the
    // thread-wide one.
    dst->tmgr = ctx->tmgr;
    GC_DTOR(dst->tmgr, hlt_timer_mgr, ctx);

    for ( __hlt_list_node* n = dst->head; n; n = n->next ) {
        if ( ! n->timer )
            continue;

        hlt_timer_mgr_schedule(dst->tmgr, n->timer->time, n->timer, excpt, ctx);
        GC_DTOR(n->timer, hlt_timer, ctx); // Not memory-managed on our end.
    }
}

void* hlt_list_clone_alloc(const hlt_type_info* ti, void* srcp, __hlt_clone_state* cstate, hlt_exception** excpt, hlt_execution_context* ctx)
{
    return GC_NEW_REF(hlt_list, ctx);
}

void hlt_list_clone_init(void* dstp, const hlt_type_info* ti, void* srcp, __hlt_clone_state* cstate, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_list* src = *(hlt_list**)srcp;
    hlt_list* dst = *(hlt_list**)dstp;

    if ( src->tmgr && src->tmgr != ctx->tmgr ) {
        hlt_string msg = hlt_string_from_asciiz("list with non-standard timer mgr cannot be cloned", excpt, ctx);
        hlt_set_exception(excpt, &hlt_exception_cloning_not_supported, msg, ctx);
        return;
    }

    dst->head = dst->tail = 0;
    dst->size = 0;
    dst->type = src->type;
    dst->timeout = src->timeout;
    dst->strategy = src->strategy;
    dst->tmgr = 0; // Set by init_in_thread().

    for ( __hlt_list_node* ns = src->head; ns; ns = ns->next ) {
        __hlt_list_node* nd = GC_NEW_CUSTOM_SIZE_REF(__hlt_list_node, sizeof(__hlt_list_node) + dst->type->size, ctx);
        nd->type = ns->type;

        __hlt_clone(&nd->data, nd->type, &ns->data, cstate, excpt, ctx);

        if ( ns->timer ) {
            __hlt_list_timer_cookie cookie = { dst, nd };
            GC_CCTOR(cookie, hlt_iterator_list, ctx);
            hlt_timer* t = __hlt_timer_new_list(cookie, excpt, ctx);
            t->time = ns->timer->time;
            nd->timer = t;
        }

        else
            nd->timer = 0;

        _link(dst, nd, dst->tail, ctx);
    }

    if ( src->tmgr )
        __hlt_clone_init_in_thread(_clone_init_in_thread, ti, dstp, cstate, excpt, ctx);
}

void hlt_list_timeout(hlt_list* l, hlt_enum strategy, hlt_interval timeout, hlt_exception** excpt, hlt_execution_context* ctx)
{
    l->timeout = timeout;
    l->strategy = strategy;

    if ( ! l->tmgr )
        GC_ASSIGN(l->tmgr, ctx->tmgr, hlt_timer_mgr, ctx);
}

void hlt_list_push_front(hlt_list* l, const hlt_type_info* type, void* val, hlt_exception** excpt, hlt_execution_context* ctx)
{
    assert(__hlt_type_equal(l->type, type));

    __hlt_list_node* n = _make_node(l, val, excpt, ctx);
    if ( ! n ) {
        hlt_set_exception(excpt, &hlt_exception_out_of_memory, 0, ctx);
        return;
    }

    _link(l, n, 0, ctx);
}

void hlt_list_push_back(hlt_list* l, const hlt_type_info* type, void* val, hlt_exception** excpt, hlt_execution_context* ctx)
{
    // fprintf(stderr, "%s|%s\n", l->type->tag, type->tag);

    assert(__hlt_type_equal(l->type, type));

    __hlt_list_node* n = _make_node(l, val, excpt, ctx);
    if ( ! n ) {
        hlt_set_exception(excpt, &hlt_exception_out_of_memory, 0, ctx);
        return;
    }

    _link(l, n, l->tail, ctx);
}

void hlt_list_append(hlt_list* l1, hlt_list* l2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    assert(__hlt_type_equal(l1->type, l2->type));

    for ( __hlt_list_node* n = l2->head; n; n = n->next )
        hlt_list_push_back(l1, l2->type, &n->data, excpt, ctx);
}

void hlt_list_pop_front(hlt_list* l, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! l->head ) {
        hlt_set_exception(excpt, &hlt_exception_underflow, 0, ctx);
        return;
    }

    _unlink(l, l->head, excpt, ctx);
}

void hlt_list_pop_back(hlt_list* l, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! l->tail ) {
        hlt_set_exception(excpt, &hlt_exception_underflow, 0, ctx);
        return;
    }

    _unlink(l, l->tail, excpt, ctx);
}

void* hlt_list_front(hlt_list* l, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! l->head ) {
        hlt_set_exception(excpt, &hlt_exception_underflow, 0, ctx);
        return 0;
    }

    _access(l, l->head, excpt, ctx);

    return &l->head->data;
}

void* hlt_list_back(hlt_list* l, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! l->tail ) {
        hlt_set_exception(excpt, &hlt_exception_underflow, 0, ctx);
        return 0;
    }

    _access(l, l->tail, excpt, ctx);

    return &l->tail->data;
}

int64_t hlt_list_size(hlt_list* l, hlt_exception** excpt, hlt_execution_context* ctx)
{
    return l->size;
}

void hlt_list_erase(hlt_iterator_list i, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( (i.node && _invalid_node(i.node)) || ! i.list ) {
        hlt_set_exception(excpt, &hlt_exception_invalid_iterator, 0, ctx);
        return;
    }

    if ( ! i.node ) {
        hlt_set_exception(excpt, &hlt_exception_invalid_iterator, 0, ctx);
        return;
    }

    _unlink(i.list, i.node, excpt, ctx);
}

void hlt_list_expire(__hlt_list_timer_cookie cookie, hlt_exception** excpt, hlt_execution_context* ctx)
{
    _unlink(cookie.list, cookie.node, 0, 0); // don't pass context on
}

void hlt_list_insert(const hlt_type_info* type, void* val, hlt_iterator_list i, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( (i.node && _invalid_node(i.node)) || ! i.list ) {
        hlt_set_exception(excpt, &hlt_exception_invalid_iterator, 0, ctx);
        return;
    }

    assert(__hlt_type_equal(i.list->type, type));

    __hlt_list_node* n = _make_node(i.list, val, excpt, ctx);
    if ( ! n ) {
        hlt_set_exception(excpt, &hlt_exception_out_of_memory, 0, ctx);
        return;
    }

    if ( ! i.node )
        // Insert at end.
        _link(i.list, n, i.list->tail, ctx);
    else
        _link(i.list, n, i.node->prev, ctx);
}

hlt_iterator_list hlt_list_begin(hlt_list* l, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_iterator_list i;
    i.list = l;
    i.node = l->head;
    return i;
}

hlt_iterator_list hlt_list_end(hlt_list* l, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_iterator_list i;
    i.list = l;
    i.node = 0;
    return i;
}

hlt_iterator_list hlt_iterator_list_incr(hlt_iterator_list i, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( (i.node && _invalid_node(i.node)) || ! i.list ) {
        hlt_set_exception(excpt, &hlt_exception_invalid_iterator, 0, ctx);
        return i;
    }

    if ( ! i.node )
        // End of list.
        return i;

    hlt_iterator_list j;
    j.list = i.list;
    j.node = i.node->next;

    return j;
}

void* hlt_iterator_list_deref(const hlt_iterator_list i, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! i.list || ! i.node || _invalid_node(i.node) ) {
        hlt_set_exception(excpt, &hlt_exception_invalid_iterator, 0, ctx);
        return 0;
    }

    _access(i.list, i.node, excpt, ctx);

    return &i.node->data;
}

const hlt_type_info* hlt_iterator_deref_type(const hlt_iterator_list i, hlt_exception** excpt, hlt_execution_context* ctx)
{
    return i.list->type;
}

int8_t hlt_iterator_list_eq(const hlt_iterator_list i1, const hlt_iterator_list i2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( i1.node )
        _access(i1.list, i1.node, excpt, ctx);

    if ( i2.node )
        _access(i2.list, i2.node, excpt, ctx);

    return i1.list == i2.list && i1.node == i2.node;
}


hlt_string hlt_list_to_string(const hlt_type_info* type, const void* obj, int32_t options, __hlt_pointer_stack* seen, hlt_exception** excpt, hlt_execution_context* ctx)
{
    const hlt_list* l = *((const hlt_list**)obj);

    if ( ! l )
        return hlt_string_from_asciiz("(Null)", excpt, ctx);

    hlt_string prefix = hlt_string_from_asciiz("[", excpt, ctx);
    hlt_string postfix = hlt_string_from_asciiz("]", excpt, ctx);
    hlt_string separator = hlt_string_from_asciiz(", ", excpt, ctx);

    hlt_string s = prefix;
    hlt_string result = 0;

    for ( __hlt_list_node* n = l->head; n; n = n->next ) {

        hlt_string t = __hlt_object_to_string(l->type, &n->data, options, seen, excpt, ctx);

        if ( hlt_check_exception(excpt) )
            return 0;

        s = hlt_string_concat(s, t, excpt, ctx);

        if ( n->next )
            s = hlt_string_concat(s, separator, excpt, ctx);

        prefix = 0;
    }

    return hlt_string_concat(s, postfix, excpt, ctx);
}

const hlt_type_info* hlt_list_element_type(const hlt_type_info* type, hlt_exception** excpt, hlt_execution_context* ctx)
{
    return ((hlt_type_info**) &type->type_params)[0];
}

const hlt_type_info* hlt_list_element_type_from_list(hlt_list* l, hlt_exception** excpt, hlt_execution_context* ctx)
{
    return l->type;
}
