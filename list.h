#pragma once

#include <stddef.h>

#define container_of(ptr, type, member) \
    ((type *) ((void *) ptr - offsetof(type, member)))

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

#define list_prev_entry(pos, member) \
    list_entry((pos)->member.prev, typeof(*(pos)), member)

#define list_next_entry(pos, member) \
    list_entry((pos)->member.next, typeof(*(pos)), member)

#define list_entry_is_head(pos, head, member) (&pos->member == (head))

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_for_each_safe(pos, _next, head)                       \
    for (pos = (head)->next, _next = (pos)->next; (pos) != (head); \
         (pos) = _next, _next = (pos)->next)

#define list_for_each_entry(pos, head, member)                   \
    for (pos = list_first_entry(head, __typeof__(*pos), member); \
         &pos->member != (head); pos = list_next_entry(pos, member))

#define LIST_HEAD_INIT(name)             \
    {                                    \
        .prev = (&name), .next = (&name) \
    }

#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

struct list_head {
    struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->prev = list;
    list->next = list;
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static int list_is_last(const struct list_head *list,
                        const struct list_head *head)
{
    return list->next == head;
}

static inline void list_add(struct list_head *new, struct list_head *list)
{
    new->prev = list->prev;
    new->next = list;
    list->prev->next = new;
    list->prev = new;
}

static inline void list_del(struct list_head *list)
{
    list->next->prev = list->prev;
    list->prev->next = list->next;
}

static void list_del_init(struct list_head *entry)
{
    list_del(entry);
    INIT_LIST_HEAD(entry);
}

static inline void list_move(struct list_head *list, struct list_head *new_head)
{
    list_del(list);
    list_add(new_head, list);
}
