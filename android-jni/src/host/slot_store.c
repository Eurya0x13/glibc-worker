/* Fixed-capacity C control for the same private storage contract. */
#include <limits.h>
#include <string.h>
#include "slot_store.h"

struct slot {
  uint32_t generation;
  int live, owner;
  size_t position;
};

struct owner_list {
  uint64_t *tokens;
  size_t count, capacity;
};

struct gw_slot_store {
  struct gw_slot_config config;
  struct slot *slots;
  unsigned char *objects;
  uint32_t *free_slots;
  struct owner_list *owners;
  size_t free_count, count;
};

static void *allocate(struct gw_slot_store *s, size_t n, size_t size,
                      size_t alignment) {
  if (n > SIZE_MAX / size) {
    return NULL;
  }
  void *p = s->config.allocate(s->config.context, n * size, alignment);
  if (p) {
    memset(p, 0, n * size);
  }
  return p;
}

int gw_slot_destroy(struct gw_slot_store *s) {
  if (!s) {
    return 1;
  }
  if (s->count) {
    return 0;
  }
  struct gw_slot_config c = s->config;
  if (s->owners) {
    for (size_t i = 0; i < c.owner_count; i++) {
      if (s->owners[i].tokens) {
        c.deallocate(c.context, s->owners[i].tokens);
      }
    }
    c.deallocate(c.context, s->owners);
  }
  if (s->slots) {
    c.deallocate(c.context, s->slots);
  }
  if (s->objects) {
    c.deallocate(c.context, s->objects);
  }
  if (s->free_slots) {
    c.deallocate(c.context, s->free_slots);
  }
  c.deallocate(c.context, s);
  return 1;
}

struct gw_slot_store *gw_slot_create(const struct gw_slot_config *c) {
  if (!c || !c->allocate || !c->deallocate || !c->object_size ||
      !c->object_alignment ||
      (c->object_alignment & (c->object_alignment - 1)) ||
      c->object_size % c->object_alignment || !c->generation_max ||
      !c->control_limit || c->control_limit > UINT32_MAX ||
      c->owner_count > INT_MAX) {
    return NULL;
  }
  struct gw_slot_store *s =
      c->allocate(c->context, sizeof *s, _Alignof(struct gw_slot_store));
  if (!s) {
    return NULL;
  }
  *s = (struct gw_slot_store){.config = *c};
  s->slots =
      allocate(s, c->control_limit, sizeof *s->slots, _Alignof(struct slot));
  s->objects =
      allocate(s, c->control_limit, c->object_size, c->object_alignment);
  s->free_slots =
      allocate(s, c->control_limit, sizeof(uint32_t), _Alignof(uint32_t));
  if (c->owner_count) {
    s->owners = allocate(s, c->owner_count, sizeof *s->owners,
                         _Alignof(struct owner_list));
  }
  if (!s->slots || !s->objects || !s->free_slots ||
      (c->owner_count && !s->owners)) {
    gw_slot_destroy(s);
    return NULL;
  }
  s->free_count = c->control_limit;
  for (size_t i = 0; i < c->control_limit; i++) {
    s->free_slots[i] = (uint32_t)(c->control_limit - i - 1);
  }
  return s;
}

static struct slot *lookup(struct gw_slot_store *s, uint64_t token) {
  uint32_t low = (uint32_t)token;
  if (!s || !low || low > s->config.control_limit) {
    return NULL;
  }
  struct slot *slot = &s->slots[low - 1];
  return slot->live && slot->generation == token >> 32 ? slot : NULL;
}

void *gw_slot_insert(struct gw_slot_store *s, int owner, uint64_t *token) {
  *token = 0;
  if (!s || !s->free_count || owner < -1 ||
      (owner >= 0 && (size_t)owner >= s->config.owner_count)) {
    return NULL;
  }
  struct owner_list *list = owner >= 0 ? &s->owners[owner] : NULL;
  if (list && list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2 : 8;
    if (capacity < list->capacity) {
      return NULL;
    }
    uint64_t *p = allocate(s, capacity, sizeof *p, _Alignof(uint64_t));
    if (!p) {
      return NULL;
    }
    if (list->tokens) {
      memcpy(p, list->tokens, list->count * sizeof *p);
      s->config.deallocate(s->config.context, list->tokens);
    }
    list->tokens = p;
    list->capacity = capacity;
  }
  size_t index = s->free_slots[--s->free_count];
  struct slot *slot = &s->slots[index];
  slot->generation++;
  slot->live = 1;
  slot->owner = owner;
  *token = ((uint64_t)slot->generation << 32) | (index + 1);
  if (list) {
    slot->position = list->count;
    list->tokens[list->count++] = *token;
  }
  s->count++;
  void *object = s->objects + index * s->config.object_size;
  memset(object, 0, s->config.object_size);
  return object;
}

void *gw_slot_get(struct gw_slot_store *s, uint64_t token) {
  return lookup(s, token)
             ? s->objects + ((uint32_t)token - 1) * s->config.object_size
             : NULL;
}

void *gw_slot_at(struct gw_slot_store *s, size_t index, uint64_t *token) {
  *token = 0;
  if (!s || index >= s->config.control_limit || !s->slots[index].live) {
    return NULL;
  }
  *token = ((uint64_t)s->slots[index].generation << 32) | (index + 1);
  return gw_slot_get(s, *token);
}

void *gw_slot_owned(struct gw_slot_store *s, int owner, size_t position,
                    uint64_t *token) {
  *token = 0;
  if (!s || owner < 0 || (size_t)owner >= s->config.owner_count ||
      position >= s->owners[owner].count) {
    return NULL;
  }
  *token = s->owners[owner].tokens[position];
  return gw_slot_get(s, *token);
}

size_t gw_slot_capacity(struct gw_slot_store *s) {
  return s ? s->config.control_limit : 0;
}

int gw_slot_erase(struct gw_slot_store *s, uint64_t token) {
  struct slot *slot = lookup(s, token);
  if (!slot) {
    return 0;
  }
  if (slot->owner >= 0) {
    struct owner_list *list = &s->owners[slot->owner];
    uint64_t moved = list->tokens[--list->count];
    if (slot->position < list->count) {
      list->tokens[slot->position] = moved;
      lookup(s, moved)->position = slot->position;
    }
  }
  slot->live = 0;
  if (slot->generation < s->config.generation_max) {
    s->free_slots[s->free_count++] = (uint32_t)token - 1;
  }
  s->count--;
  return 1;
}
