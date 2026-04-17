#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 256

// Simple linked list node
typedef struct Node {
    char *str;
    struct Node *next;
} Node;

static Node *load_file(const char *path) {
    FILE *f = fopen(path, "re");
    if (!f)
        return NULL;

    Node *head = NULL, **tail = &head;
    char line[MAX_LINE];

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        Node *node = malloc(sizeof(Node));
        if (!node)
            break;
        node->str = strdup(line);
        if (!node->str) {
            free(node);
            break;
        }
        node->next = NULL;
        *tail = node;
        tail = &node->next;
    }

    fclose(f);
    return head;
}

static void free_list(Node *head) {
    while (head) {
        Node *tmp = head;
        head = head->next;
        free(tmp->str);
        free(tmp);
    }
}

static int contains(Node *head, const char *s) {
    for (; head; head = head->next) {
        if (strcmp(head->str, s) == 0)
            return 1;
    }
    return 0;
}

static int save_file(const char *path, Node *head) {
    FILE *f = fopen(path, "we");
    if (!f)
        return -1;

    for (; head; head = head->next) {
        if (fprintf(f, "%s\n", head->str) < 0) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

int file_add(const char *path, const char *s) {
    Node *head = load_file(path);
    if (!head && errno != ENOENT)
        return -1;

    if (contains(head, s)) {
        free_list(head);
        return 0; // already present
    }

    Node *node = malloc(sizeof(Node));
    if (!node) {
        free_list(head);
        return -1;
    }
    node->str = strdup(s);
    if (!node->str) {
        free(node);
        free_list(head);
        return -1;
    }
    node->next = head;
    head = node;

    int ret = save_file(path, head);
    free_list(head);
    return ret;
}

int file_remove(const char *path, const char *s) {
    Node *head = load_file(path);
    if (!head) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    Node *cur = head, *prev = NULL;
    int removed = 0;

    while (cur) {
        if (strcmp(cur->str, s) == 0) {
            Node *tmp = cur;
            if (prev)
                prev->next = cur->next;
            else
                head = cur->next;
            cur = cur->next;
            free(tmp->str);
            free(tmp);
            removed = 1;
            continue;
        }
        prev = cur;
        cur = cur->next;
    }

    if (!removed) {
        free_list(head);
        return 0;
    }

    int ret = save_file(path, head);
    free_list(head);
    return ret;
}

int file_set(const char *path, const char **strings, size_t count) {
    FILE *f = fopen(path, "we");
    if (!f)
        return -1;

    for (size_t i = 0; i < count; i++) {
        if (fprintf(f, "%s\n", strings[i]) < 0) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

int file_contains(const char *path, const char *s) {
    Node *head = load_file(path);
    if (!head) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    int found = contains(head, s);
    free_list(head);
    return found;
}
