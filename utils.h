#pragma once

#include <stddef.h>
#include <stdint.h>

/* TIMER */
typedef struct {
    uint64_t begin;
    uint64_t freq;
} semu_timer_t;

void semu_timer_init(semu_timer_t *timer, uint64_t freq);
uint64_t semu_timer_get(semu_timer_t *timer);
void semu_timer_rebase(semu_timer_t *timer, uint64_t time);

/* Linux-like queue API */

#if defined(__GNUC__) || defined(__clang__) ||         \
    (defined(__STDC__) && defined(__STDC_VERSION__) && \
     (__STDC_VERSION__ >= 202000L)) /* C2x/C23 ?*/
#define __HAVE_TYPEOF 1
#else
#error You should use either GCC or Clang for compilation.
#endif

struct list_head {
    struct list_head *prev, *next;
};

#define list_HEAD(head) struct list_head head = {&(head), &(head)}

static inline void INIT_LIST_HEAD(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}

/* Push the node to the end of list */
static inline void list_push(struct list_head *node, struct list_head *head)
{
    struct list_head *prev = head->prev;

    prev->next = node;
    node->next = head;
    node->prev = prev;
    head->prev = node;
}

static inline int list_empty(const struct list_head *head)
{
    return (head->next == head);
}

static inline void list_del(struct list_head *node)
{
    struct list_head *next = node->next;
    struct list_head *prev = node->prev;

    next->prev = prev;
    prev->next = next;

    node->prev = (struct list_head *) (0x00100100);
    node->next = (struct list_head *) (0x00200200);
}

static inline void list_del_init(struct list_head *node)
{
    list_del(node);
    INIT_LIST_HEAD(node);
}

#ifndef container_of
#define container_of(ptr, type, member)                            \
    __extension__({                                                \
        const __typeof__(((type *) 0)->member) *__pmember = (ptr); \
        (type *) ((char *) __pmember - offsetof(type, member));    \
    })
#else
#define container_of(ptr, type, member) \
    ((type *) ((char *) (ptr) -offsetof(type, member)))
#endif

#define list_entry(node, type, member) container_of(node, type, member)

#define list_first_entry(head, type, member) \
    list_entry((head)->next, type, member)

#define list_last_entry(head, type, member) \
    list_entry((head)->prev, type, member)

#define list_for_each(node, head) \
    for (node = (head)->next; node != (head); node = node->next)

#define list_for_each_entry(entry, head, member)                       \
    for (entry = list_entry((head)->next, __typeof__(*entry), member); \
         &entry->member != (head);                                     \
         entry = list_entry(entry->member.next, __typeof__(*entry), member))

#define list_for_each_safe(node, safe, head)                     \
    for (node = (head)->next, safe = node->next; node != (head); \
         node = safe, safe = node->next)

#define list_for_each_entry_safe(entry, safe, head, member)                \
    for (entry = list_entry((head)->next, __typeof__(*entry), member),     \
        safe = list_entry(entry->member.next, __typeof__(*entry), member); \
         &entry->member != (head); entry = safe,                           \
        safe = list_entry(safe->member.next, __typeof__(*entry), member))
