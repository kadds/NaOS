#pragma once
#include "buddy.hpp"
#include "mm.hpp"
#include "new.hpp"

namespace memory
{
template <typename ListType> class list_node_cache_allocator : public IAllocator
{
  public:
    using Node = typename ListType::list_node;

    struct KList
    {
        KList *prev;
        Node *available;
        u32 counter;
        char data[0x1000 - sizeof(void *) * 2 - sizeof(counter)];
        void add_counter() { counter++; }
        void remove_counter() { counter--; }
        u32 get_counter() { return counter; }
        void set_counter(u32 counter) { this->counter = counter; }
    } PackStruct;
    static_assert(sizeof(KList) == 0x1000);

  private:
    KList *last;

  public:
    list_node_cache_allocator()
        : last(nullptr)
    {
    }

    KList *alloc_page()
    {
        KList *list = memory::New<KList>(memory::KernelBuddyAllocatorV);
        // kassert(((u64)list & ~0xFFF) == (u64)list, "Allocator should allcate a page at ", (void *)list, ", allocator
        // ",
        //         (void *)memory::KernelBuddyAllocatorV);

        list->prev = nullptr;
        Node *start = align_of((Node *)(&list->data));
        list->available = start;
        while (1)
        {
            Node *next = align_of(start + 1);
            if ((u64)((char *)align_of(next + 1) - (char *)list) >= memory::page_size)
            {
                list->set_counter(0);
                start->next = nullptr;
                break;
            }

            start->next = next;
            start = start->next;
        }
        return list;
    }

    void free_page(KList *list) { memory::Delete<KList>(memory::KernelBuddyAllocatorV, list); }

    Node *align_of(Node *node)
    {
        auto align = alignof(Node);
        return (Node *)(((u64)node + align - 1) & ~(align - 1));
    }
    Node *new_node()
    {
        KList *current = last;
        if (current == nullptr || current->available == nullptr)
        {
            current = alloc_page();
            current->prev = last;
            last = current;
        }
        current->add_counter();
        Node *node = current->available;
        current->available = node->next;
        node->next = nullptr;
        return node;
    }
    void delete_node(Node *node)
    {
        KList *current = last, *next = nullptr;
        while (current != nullptr)
        {
            if ((char *)current < (char *)node && (char *)current + memory::page_size > (char *)node)
            {
                node->next = current->available;
                current->available = node;
                current->remove_counter();
                if (unlikely(current->get_counter() == 0))
                {
                    if (next != nullptr)
                    {
                        next->prev = current->prev;
                    }
                    else
                        last = nullptr;

                    free_page(current);
                }
                return;
            }
            next = current;
            current = current->prev;
        }
        // kassert(false, "Can't free list node ", (void *)node);
    }

    void *allocate(u64 size, u64 align) override { return new_node(); }

    void deallocate(void *node) override { delete_node((Node *)node); }
};

} // namespace memory