/*
 * implementation of inode_table.h using hash tables
 */

#define FUSE_USE_VERSION 30

#include <stdlib.h>
#include <string.h>

#include <fuse_lowlevel.h>

/* #include "../include/inode_table.h" */

#define FU_HT_MIN_SIZE 8192

// private hash table implementation,
struct fu_hash_t {
  struct fu_node_t **store;
  int size;
};

/*
 * inspiration for maintaining name and inode hashtables taken from
 * fuse.c in libfuse
 */
struct fu_node_t {
  fuse_ino_t inode;
  char *name;
  struct fu_node_t *parent;

  struct stat st;

  // used in the name hash table
  struct fu_node_t *next_name;

  // used in the indoe hash table
  struct fu_node_t *next_inode;
};

struct fu_table_t {
  struct fu_hash_t inode_table;
  struct fu_hash_t name_table;
};

struct fu_node_t * fu_node_parent(struct fu_node_t *node) {
  return node->parent;
}

fuse_ino_t fu_node_inode(struct fu_node_t *node) {
  return node->inode;
}

struct stat fu_node_stat(struct fu_node_t *node) {
  return node->st;
}

struct fu_node_t * fu_node_setstat(struct fu_node_t *node, struct stat st) {
  node->st = st;
  node->st.st_ino = node->inode;
  return node;
}

const char * fu_node_name(struct fu_node_t *node) {
  return node->name;
}

size_t inode_hash(fuse_ino_t inode, size_t max) {
  return ((uint32_t) inode * 2654435761U) % max;
}

size_t name_hash(const char *name, fuse_ino_t parent, size_t max) {
  uint64_t hash = parent;
  for (; *name; name++) {
    hash = hash * 31 + (unsigned char) *name;
  }

  return hash % max;
}
void fu_hash_alloc(struct fu_hash_t *ht) {
  ht->size = FU_HT_MIN_SIZE;
  ht->store = calloc(FU_HT_MIN_SIZE, sizeof(struct node *));
}

void fu_hash_free(struct fu_hash_t *ht) {
  free(ht->store);
}

struct fu_node_t *fu_hash_findname(
    struct fu_hash_t *ht, fuse_ino_t pinode, const char *name) {

  struct fu_node_t *node;
  size_t hash = name_hash(name, pinode, ht->size);
  for (node = ht->store[hash]; node; node = node->next_name) {
    if (node->parent->inode == pinode && strcmp(name, node->name) == 0) {
      break;
    }
  }

  return node;
}

struct fu_node_t *fu_hash_findinode(struct fu_hash_t *ht, fuse_ino_t inode) {
  struct fu_node_t *node;
  size_t hash = inode_hash(inode, ht->size);
  for (node = ht->store[hash]; node; node = node->next_inode) {
    if (node->inode == inode) {
      return node;
    }
  }

  return NULL;
}


struct fu_table_t * fu_table_alloc() {
  struct fu_table_t *table = calloc(1, sizeof(struct fu_table_t));
  fu_hash_alloc(&table->inode_table);
  fu_hash_alloc(&table->name_table);

  return table;
}

void fu_table_free(struct fu_table_t *table) {
  struct fu_node_t *node;
  int i;
  for (i = 0; i < table->inode_table.size; i++) {
    struct fu_node_t *next;
    for (
      node = table->inode_table.store[i];
      node != NULL;
      node = next
    ) {
      next = node->next_inode;
      free(node);
    }
  }
  fu_hash_free(&table->inode_table);
  fu_hash_free(&table->name_table);

  free(table);
}

struct fu_node_t * fu_table_get(struct fu_table_t *table, fuse_ino_t inode) {
  return fu_hash_findinode(&table->inode_table, inode);
}

struct fu_node_t * fu_table_lookup(
    struct fu_table_t *table, fuse_ino_t pinode, const char *name) {

  return fu_hash_findname(&table->name_table, pinode, name);
}

struct fu_node_t * fu_table_add(
    struct fu_table_t *table, fuse_ino_t pinode, const char *name, fuse_ino_t inode) {
  struct fu_node_t *pnode = fu_table_get(table, pinode);
  if (!pnode && pnode != 0) {
    return NULL;
  }

  struct fu_node_t *node = calloc(1, sizeof(struct fu_node_t));
  node->inode = inode;
  node->st.st_ino = inode;

  node->name = malloc(sizeof(char) * strlen(name));
  strcpy(node->name, name);


  node->parent = pnode;

  size_t ihash = inode_hash(inode, table->inode_table.size);
  size_t nhash = name_hash(name, pinode, table->name_table.size);

  node->next_inode = table->inode_table.store[ihash];
  node->next_name = table->name_table.store[nhash];

  table->inode_table.store[ihash] = table->name_table.store[nhash] = node;

  return node;
}

