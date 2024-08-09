#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>

#if defined(__GNUC__)
#define __QUEUE_HAVE_TYPEOF 1
#endif

/* Merely a doubly-linked list */
struct queue_head {
    struct queue_head *prev, *next;
};

#define QUEUE_HEAD(head) struct queue_head head = {&(head), &(head)}

static inline void INIT_QUEUE_HEAD(struct queue_head *head)
{
    head->next = head;
    head->prev = head;
}

/* Push the node to the end of queue */
static inline void queue_push(struct queue_head *node, struct queue_head *head)
{
    struct queue_head *prev = head->prev;

    prev->next = node;
    node->next = head;
    node->prev = prev;
    head->prev = node;
}

static inline int queue_empty(const struct queue_head *head)
{
    return (head->next == head);
}

static inline void queue_del(struct queue_head *node)
{
    struct queue_head *next = node->next;
    struct queue_head *prev = node->prev;

    next->prev = prev;
    prev->next = next;

    node->prev = (struct queue_head *) (0x00100100);
    node->next = (struct queue_head *) (0x00200200);
}

static inline void queue_del_init(struct queue_head *node)
{
    queue_del(node);
    INIT_QUEUE_HEAD(node);
}

#ifndef container_of
#ifdef __QUEUE_HAVE_TYPEOF
#define container_of(ptr, type, member)                            \
    __extension__({                                                \
        const __typeof__(((type *) 0)->member) *__pmember = (ptr); \
        (type *) ((char *) __pmember - offsetof(type, member));    \
    })
#else
#define container_of(ptr, type, member) \
    ((type *) ((char *) (ptr) -offsetof(type, member)))
#endif
#endif

#define queue_entry(node, type, member) container_of(node, type, member)

#define queue_first_entry(head, type, member) \
    queue_entry((head)->next, type, member)

#define queue_last_entry(head, type, member) \
    queue_entry((head)->prev, type, member)

#define queue_for_each(node, head) \
    for (node = (head)->next; node != (head); node = node->next)

#ifdef __QUEUE_HAVE_TYPEOF
#define queue_for_each_entry(entry, head, member)                       \
    for (entry = queue_entry((head)->next, __typeof__(*entry), member); \
         &entry->member != (head);                                      \
         entry = queue_entry(entry->member.next, __typeof__(*entry), member))
#endif

#define queue_for_each_safe(node, safe, head)                    \
    for (node = (head)->next, safe = node->next; node != (head); \
         node = safe, safe = node->next)

#define queue_for_each_entry_safe(entry, safe, head, member)                \
    for (entry = queue_entry((head)->next, __typeof__(*entry), member),     \
        safe = queue_entry(entry->member.next, __typeof__(*entry), member); \
         &entry->member != (head); entry = safe,                            \
        safe = queue_entry(safe->member.next, __typeof__(*entry), member))

#endif
