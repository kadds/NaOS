#pragma once
#include "../trace.hpp"
#include "buddy.hpp"
#include "memory.hpp"

template <typename List> class list_node_cache
{
  public:
    using Node = typename List::list_node;

    struct KList
    {
        KList *next;
        Node *available;
        char data[0x1000 - sizeof(void *) * 2];
    };
    static_assert(sizeof(KList) == 0x1000);

  private:
    memory::IAllocator *allocator;

    KList *head;

  public:
    list_node_cache(memory::IAllocator *allocator)
        : allocator(allocator)
        , head(nullptr)
    {
    }
    KList *alloc_page()
    {
        KList *list = memory::New<KList>(allocator);
        list->next = nullptr;
        Node *start = align_of((Node *)(&list->data));
        list->available = start;
        while (1)
        {
            Node *next = align_of(start + 1);
            if ((char *)next - (char *)list > 0x1000)
            {
                start->next = nullptr;
                break;
            }
            start->next = next;
            start = start->next;
        }
        return list;
    }
    void free_page(KList *list) { memory::Delete<KList>(allocator, list); }
    Node *align_of(Node *node)
    {
        auto align = alignof(Node);
        return (Node *)(((u64)node + align - 1) & ~(align - 1));
    }
    Node *new_node()
    {
        KList *current = head;

        while (current != nullptr && current->available == nullptr)
        {
            current = current->next;
        }
        if (current == nullptr)
        {
            current = alloc_page();
            head = current;
            Node *node = current->available;
            current->available = current->available->next;
            node->next = nullptr;
            return node;
        }
        Node *node = current->available;
        current->available = current->available->next;
        node->next = nullptr;
        return node;
    }
    void delete_node(Node *node)
    {
        KList *current = head;
        while (current != nullptr)
        {
            if ((char *)current > (char *)node && (char *)current + memory::page_size < (char *)node)
            {
                node->next = current->available;
                current->available = node;
                return;
            }
            current = current->next;
        }
    }
};

namespace memory
{
template <typename Cache> class ListNodeCacheAllocator : public IAllocator
{
    Cache *cache;

  public:
    void *allocate(u64 size, u64 align) override { return cache->new_node(); }
    void deallocate(void *node) override { cache->delete_node((typename Cache::Node *)node); }
    ListNodeCacheAllocator(Cache *cache)
        : cache(cache)
    {
    }
};
} // namespace memory
